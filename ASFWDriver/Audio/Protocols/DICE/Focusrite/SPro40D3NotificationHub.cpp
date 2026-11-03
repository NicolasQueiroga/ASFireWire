// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "SPro40D3NotificationHub.hpp"

#include <algorithm>
#include <limits>

namespace ASFW::Audio::DICE::Focusrite::SPro40D3 {
namespace {
constexpr uint64_t kAddressSpaceEnd = uint64_t{1} << 48U;

class LockGuard final {
public:
    explicit LockGuard(IOLock* lock) noexcept : lock_(lock) { IOLockLock(lock_); }
    ~LockGuard() { IOLockUnlock(lock_); }
    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;
private:
    IOLock* lock_;
};
} // namespace

SPro40D3NotificationHub::SPro40D3NotificationHub(
    Discovery::DeviceRegistry& registry, DiceWaitClock& clock, Region region) noexcept
    : registry_(registry), clock_(clock), region_(region),
      validRegion_(region.begin < region.endExclusive &&
                   region.endExclusive <= kAddressSpaceEnd &&
                   region.begin % kFrameBytes == 0 &&
                   region.endExclusive % kFrameBytes == 0),
      lock_(IOLockAlloc()), nextOffset_(region.begin) {}

SPro40D3NotificationHub::~SPro40D3NotificationHub() {
    // Caller has quiesced dispatch/control queues; no callbacks to cancel.
    if (lock_) IOLockFree(lock_);
}

bool SPro40D3NotificationHub::Matches(const Entry& entry, const Lease& lease) noexcept {
    return lease.id != 0 && entry.lease.id == lease.id &&
           entry.lease.offset == lease.offset && entry.lease.route == lease.route;
}

std::expected<SPro40D3NotificationHub::Lease, IOReturn>
SPro40D3NotificationHub::Allocate(Discovery::DeviceRouteToken route) {
    if (!validRegion_) return std::unexpected(kIOReturnBadArgument);
    if (!lock_) return std::unexpected(kIOReturnNoMemory);
    const LockGuard guard(lock_);
    // Operational routes use physical node IDs; do not alias unexpected bus bits.
    if (route.nodeId > 0x3FU || !registry_.IsCurrent(route)) {
        return std::unexpected(kIOReturnNotReady);
    }
    if (nextOffset_ >= region_.endExclusive ||
        region_.endExclusive - nextOffset_ < kFrameBytes ||
        nextId_ == std::numeric_limits<uint64_t>::max()) {
        return std::unexpected(kIOReturnNoSpace);
    }
    const Lease lease{nextId_, nextOffset_, route};
    auto [it, inserted] = entries_.try_emplace(lease.offset);
    if (!inserted) return std::unexpected(kIOReturnNoSpace);
    it->second.lease = lease;
    ++nextId_;
    nextOffset_ += kFrameBytes; // Region validation makes this non-overflowing.
    return lease;
}

void SPro40D3NotificationHub::Release(const Lease& lease) noexcept {
    if (!lock_) return;
    const LockGuard guard(lock_);
    const auto it = entries_.find(lease.offset);
    if (it != entries_.end() && Matches(it->second, lease)) {
        entries_.erase(it); // Atomically retires ingress and all pending copies.
    }
}

std::optional<SPro40D3NotificationHub::Notification>
SPro40D3NotificationHub::Take(const Lease& lease) noexcept {
    if (!lock_) return std::nullopt;
    const LockGuard guard(lock_);
    const auto it = entries_.find(lease.offset);
    if (it == entries_.end() || !Matches(it->second, lease)) return std::nullopt;
    auto& entry = it->second;
    if (!registry_.IsCurrent(entry.lease.route)) {
        entry.head = 0;
        entry.count = 0;
        return std::nullopt;
    }
    if (entry.count == 0) return std::nullopt;
    const auto notification = entry.queue[entry.head];
    entry.head = (entry.head + 1) % kQueueCapacity;
    --entry.count;
    return notification;
}

Async::LocalRequestResult SPro40D3NotificationHub::HandleLocalRequest(
    const Async::LocalRequestContext& ctx) {
    using Async::LocalRequestResult;
    using Async::ResponseCode;
    if (!validRegion_ || ctx.destOffset < region_.begin ||
        ctx.destOffset >= region_.endExclusive) {
        return LocalRequestResult::NotMine();
    }
    // Capture ingress time, not control-queue consumption time. Never call the
    // injected clock (or any engine/session callback) while holding our lock.
    const uint64_t receivedMs = clock_.NowMs();
    if (!lock_) return LocalRequestResult::Write(ResponseCode::AddressError);
    const LockGuard guard(lock_);
    const auto it = entries_.find(ctx.destOffset);
    if (it == entries_.end()) {
        return LocalRequestResult::Write(ResponseCode::AddressError);
    }
    auto& entry = it->second;
    const auto& route = entry.lease.route;
    // Full local-bus source ID is required, not just matching low six bits.
    if (ctx.sourceID != static_cast<uint16_t>(0xFFC0U | route.nodeId) ||
        ctx.generation != route.generation.value || !registry_.IsCurrent(route)) {
        return LocalRequestResult::Write(ResponseCode::AddressError);
    }
    if (ctx.tCode != 0x1) return LocalRequestResult::Write(ResponseCode::TypeError);
    // Behavioral source: snd-firewire-ctl-services protocols/dice/src/focusrite/
    // spro40d3.rs:245-259 reserves a 16-byte notification address. Preserve the
    // entire raw frame for the engine; no protocol decode or bus reads here.
    if (ctx.dataLength != kFrameBytes || ctx.writePayload.size() != kFrameBytes) {
        return LocalRequestResult::Write(ResponseCode::DataError);
    }
    if (entry.count == kQueueCapacity) {
        return LocalRequestResult::Write(ResponseCode::ConflictError);
    }
    auto& notification = entry.queue[(entry.head + entry.count) % kQueueCapacity];
    std::ranges::copy(ctx.writePayload, notification.frame.begin());
    notification.receivedMs = receivedMs;
    ++entry.count;
    return LocalRequestResult::Write(ResponseCode::Complete);
}

} // namespace ASFW::Audio::DICE::Focusrite::SPro40D3
