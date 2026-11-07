// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "SPro40D3NotificationEndpoint.hpp"

#include "../../../../Hardware/IEEE1394.hpp"
#include "../../../../Logging/Logging.hpp"

#include <algorithm>

namespace ASFW::Audio::DICE::Focusrite::SPro40D3 {

namespace {
using AReq = ASFW::Async::HW::AsyncRequestHeader;
} // namespace

SPro40D3NotificationEndpoint::~SPro40D3NotificationEndpoint() {
    Deactivate();
    if (lock_) {
        IOLockFree(lock_);
        lock_ = nullptr;
    }
}

SPro40D3NotificationEndpoint::Activation SPro40D3NotificationEndpoint::Activate(
    const std::shared_ptr<Engine>& engine, const Discovery::DeviceRouteToken& route) {
    if (!lock_ || !engine || !route) return {};

    IOLockLock(lock_);
    if (state_.active || state_.quarantined) {
        IOLockUnlock(lock_);
        return {};
    }
    IOLockUnlock(lock_);

    // Collision-safe, on-demand reservation (auto window in CSR space,
    // addressHi 0xFFFF - kAutoAddressHi). No 16-byte-alignment assumption:
    // the manager's own window alignment is the only one that matters, and
    // software serves this range; nothing here requires 16-byte alignment.
    uint64_t handle = 0;
    Protocols::SBP2::AddressSpaceManager::AddressRangeMeta meta{};
    const kern_return_t kr = space_.AllocateAddressRangeAuto(
        this, 0xFFFFu, kSPro40D3EndpointRangeBytes, &handle, &meta);
    if (kr != kIOReturnSuccess || meta.address == 0) {
        // Zero allocations held on failure: the manager rolled back.
        return {};
    }

    IOLockLock(lock_);
    if (state_.active || state_.quarantined) {
        // Lost an Activate race; only one reservation may exist.
        IOLockUnlock(lock_);
        (void)space_.DeallocateAddressRange(this, handle);
        return {};
    }
    state_.active = true;
    state_.address = meta.address;
    state_.handle = handle;
    state_.route = route;
    state_.engine = engine;
    IOLockUnlock(lock_);

    ASFW_LOG(Audio,
             "[SPro40D3Mailbox] notification endpoint reserved addr=0x%012llx len=%u gen=%u node=0x%02x",
             static_cast<unsigned long long>(meta.address), kSPro40D3EndpointRangeBytes,
             route.generation.value, route.nodeId & 0x3F);

    // P05 ingress wiring: the local-request chain already routes remote
    // writes into manager-reserved ranges (Sbp2LocalHandler ->
    // ApplyRemoteWrite); without this callback such writes land in the
    // range and vanish - the device believes it delivered. Registration is
    // part of holding the range; Deactivate's dealloc erases the range and
    // this callback with it, and the revocation epoch makes any callback
    // already extracted a no-op.
    space_.SetRemoteWriteCallback(
        handle, [this](uint64_t, uint32_t offset, std::span<const uint8_t> payload) {
            DeliverRemoteWrite(offset, payload);
        });
    return Activation{meta.address, handle};
}

void SPro40D3NotificationEndpoint::Deactivate() {
    if (!lock_) return;
    uint64_t handle = 0;
    IOLockLock(lock_);
    if (!state_.active) {
        IOLockUnlock(lock_);
        return;
    }
    // Teardown order 1: revoke ingress under the lock. The revocation epoch
    // makes every already-scheduled delivery a no-op; the weak slot is
    // cleared so nothing new can be scheduled.
    ++*revokeEpoch_;
    state_.active = false;
    state_.address = 0;
    state_.engine.reset();
    handle = std::exchange(state_.handle, uint64_t{0});
    IOLockUnlock(lock_);

    // Teardown order 2: release the reservation. The manager checks that we
    // are still its owner for this handle.
    if (handle != 0) {
        (void)space_.DeallocateAddressRange(this, handle);
    }
}

void SPro40D3NotificationEndpoint::MarkQuarantined() {
    if (!lock_) return;
    IOLockLock(lock_);
    state_.quarantined = true;
    IOLockUnlock(lock_);
}

void SPro40D3NotificationEndpoint::ClearQuarantine() {
    if (!lock_) return;
    IOLockLock(lock_);
    state_.quarantined = false;
    IOLockUnlock(lock_);
}

bool SPro40D3NotificationEndpoint::Quarantined() const {
    if (!lock_) return false;
    IOLockLock(lock_);
    const bool quarantined = state_.quarantined;
    IOLockUnlock(lock_);
    return quarantined;
}

Async::LocalRequestResult SPro40D3NotificationEndpoint::HandleLocalRequest(
    const Async::LocalRequestContext& ctx) {
    if (!lock_) return Async::LocalRequestResult::NotMine();

    // Snapshot under the lock; the delivery path never runs while it is held.
    bool active = false;
    uint64_t base = 0;
    Discovery::DeviceRouteToken route{};
    std::weak_ptr<Engine> engine;
    IOLockLock(lock_);
    active = state_.active;
    base = state_.address;
    route = state_.route;
    engine = state_.engine;
    IOLockUnlock(lock_);

    if (!active || base == 0) return Async::LocalRequestResult::NotMine();

    // FULL-DESTINATION attribution first: claim only what overlaps the
    // reserved range, by absolute 48-bit destination.
    const uint64_t length = ctx.writePayload.size();
    const uint64_t end = ctx.destOffset + length;
    if (ctx.destOffset >= base + kSPro40D3EndpointRangeBytes || end <= base) {
        return Async::LocalRequestResult::NotMine();
    }
    // Overlapping but not fully inside: ours to answer, never to deliver
    // (a partially-overlapping block write is a shape violation).
    if (ctx.destOffset < base || end > base + kSPro40D3EndpointRangeBytes) {
        IOLockLock(lock_);
        ++state_.malformed;
        IOLockUnlock(lock_);
        return Async::LocalRequestResult::Write(Async::ResponseCode::DataError);
    }
    if (ctx.tCode != AReq::kTcodeWriteBlock) {
        IOLockLock(lock_);
        ++state_.malformed;
        IOLockUnlock(lock_);
        return Async::LocalRequestResult::Write(Async::ResponseCode::TypeError);
    }

    // SOURCE + GENERATION at ingress: a write from the wrong node or a
    // stale bus generation is answered (it is our address) but never
    // delivered - attribution happens before any engine state can change.
    if (!route || route.generation.value != ctx.generation ||
        (route.nodeId & 0x3F) != (ctx.sourceID & 0x3F)) {
        IOLockLock(lock_);
        ++state_.rejected;
        IOLockUnlock(lock_);
        return Async::LocalRequestResult::Write(Async::ResponseCode::Complete);
    }

    // Bounded COPY; the payload span is owned by the dispatch and does not
    // survive this callback. Short frames deliver too - the ENGINE's shape
    // rules decide their fate (counted, never trusted blindly).
    std::array<uint8_t, kSPro40D3EndpointRangeBytes> frame{};
    const auto frameLen = static_cast<uint32_t>(length);
    std::copy(ctx.writePayload.begin(), ctx.writePayload.end(), frame.begin());

    // Defer onto Default BEFORE touching engine state (P00 ingress row).
    // The scheduled work captures ONLY the weak engine, the frame copy and
    // the revocation epoch: no endpoint pointer crosses the queues, and a
    // delivery that fires after revocation is a no-op by epoch check.
    const uint64_t epoch = revokeEpoch_->load();
    const auto token = scheduler_.ScheduleAfter(
        0, [engine, frame, frameLen, epoch, revokeEpoch = revokeEpoch_]() {
            if (revokeEpoch->load() != epoch) return; // revoked in flight
            if (const auto held = engine.lock()) {
                held->OnNotification(std::span<const uint8_t>(frame.data(), frameLen));
            }
        });
    IOLockLock(lock_);
    if (token == Scheduling::kInvalidTimerToken) {
        // Refused Default hop: the frame is lost, counted; the dispatch
        // context must never block or grow queues.
        ++state_.dropped;
        IOLockUnlock(lock_);
        return Async::LocalRequestResult::Write(Async::ResponseCode::Complete);
    }
    ++state_.accepted;
    IOLockUnlock(lock_);
    return Async::LocalRequestResult::Write(Async::ResponseCode::Complete);
}

void SPro40D3NotificationEndpoint::DeliverRemoteWrite(uint32_t offset,
                                                      std::span<const uint8_t> payload) {
    if (!lock_ || payload.empty()) return;
    // Snapshot the activation identity; HandleLocalRequest re-validates
    // active/route/engine under its own lock, so a Deactivate between here
    // and delivery degrades to a rejected/no-op ingress, never a stale one.
    uint64_t address = 0;
    Discovery::DeviceRouteToken route{};
    IOLockLock(lock_);
    address = state_.address;
    route = state_.route;
    IOLockUnlock(lock_);
    if (address == 0) return; // torn down between the manager firing and here
    Async::LocalRequestContext ctx{};
    ctx.destOffset = address + offset;
    ctx.tCode = AReq::kTcodeWriteBlock;
    ctx.sourceID = route.nodeId; // only the registered device writes here
    ctx.generation = route.generation.value;
    ctx.writePayload = payload;
    ctx.dataLength = static_cast<uint32_t>(payload.size());
    (void)HandleLocalRequest(ctx);
}

uint64_t SPro40D3NotificationEndpoint::ReservedAddress() const {
    if (!lock_) return 0;
    IOLockLock(lock_);
    const uint64_t address = state_.address;
    IOLockUnlock(lock_);
    return address;
}

uint32_t SPro40D3NotificationEndpoint::AcceptedDeliveries() const {
    if (!lock_) return 0;
    IOLockLock(lock_);
    const uint32_t accepted = state_.accepted;
    IOLockUnlock(lock_);
    return accepted;
}

uint32_t SPro40D3NotificationEndpoint::RejectedIngress() const {
    if (!lock_) return 0;
    IOLockLock(lock_);
    const uint32_t rejected = state_.rejected;
    IOLockUnlock(lock_);
    return rejected;
}

uint32_t SPro40D3NotificationEndpoint::MalformedIngress() const {
    if (!lock_) return 0;
    IOLockLock(lock_);
    const uint32_t malformed = state_.malformed;
    IOLockUnlock(lock_);
    return malformed;
}

uint32_t SPro40D3NotificationEndpoint::DroppedByScheduleFailure() const {
    if (!lock_) return 0;
    IOLockLock(lock_);
    const uint32_t dropped = state_.dropped;
    IOLockUnlock(lock_);
    return dropped;
}

} // namespace ASFW::Audio::DICE::Focusrite::SPro40D3
