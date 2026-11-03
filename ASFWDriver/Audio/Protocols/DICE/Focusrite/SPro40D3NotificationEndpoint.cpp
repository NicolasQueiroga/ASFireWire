// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "SPro40D3NotificationEndpoint.hpp"

#include <utility>

namespace ASFW::Audio::DICE::Focusrite::SPro40D3 {

SPro40D3NotificationEndpoint::SPro40D3NotificationEndpoint(
    std::shared_ptr<SPro40D3NotificationHub> hub) noexcept
    : hub_(std::move(hub)), lock_(IOLockAlloc()) {}

std::expected<std::shared_ptr<SPro40D3NotificationEndpoint>, IOReturn>
SPro40D3NotificationEndpoint::Create(Protocols::SBP2::AddressSpaceManager& manager,
                                      std::shared_ptr<SPro40D3NotificationHub> hub) {
    if (!hub) {
        return std::unexpected(kIOReturnBadArgument);
    }

    auto endpoint = std::shared_ptr<SPro40D3NotificationEndpoint>(
        new SPro40D3NotificationEndpoint(std::move(hub)));
    if (!endpoint->lock_) return std::unexpected(kIOReturnNoMemory);
    Protocols::SBP2::AddressSpaceManager::AddressRangeMeta meta{};
    // Use the controller-lifetime allocator itself as the reservation owner.
    // An endpoint address can be recycled by malloc after endpoint teardown;
    // retaining that pointer as the owner would let an unrelated future
    // UserClient's ReleaseOwner(this) accidentally reclaim this tombstone.
    const kern_return_t result = manager.AllocateAddressRangeAuto(&manager,
                                                                   0xFFFFu,
                                                                   kAddressBytes,
                                                                   &endpoint->handle_,
                                                                   &meta,
                                                                   kAddressBytes);
    if (result != kIOReturnSuccess) {
        return std::unexpected(result);
    }
    endpoint->address_ = meta.address;
    std::weak_ptr<SPro40D3NotificationEndpoint> weakEndpoint = endpoint;
    manager.SetRemoteWriteContextCallback(
        endpoint->handle_, [weakEndpoint](const auto& context) -> Async::ResponseCode {
            const auto locked = weakEndpoint.lock();
            return locked ? locked->HandleWrite(context) : Async::ResponseCode::AddressError;
        });
    return endpoint;
}

SPro40D3NotificationEndpoint::~SPro40D3NotificationEndpoint() {
    Release();
    // Intentionally retain the manager range, including its address reservation.
    // The callback retains only a weak reference, so it cannot enter this
    // endpoint after destruction; keeping it installed makes retired ranges
    // answer AddressError instead of silently accepting late device writes.
    if (lock_) {
        IOLockFree(lock_);
        lock_ = nullptr;
    }
}

std::expected<SPro40D3NotificationHub::Lease, IOReturn>
SPro40D3NotificationEndpoint::Bind(Discovery::DeviceRouteToken route) {
    if (!lock_) {
        return std::unexpected(kIOReturnNoMemory);
    }
    IOLockLock(lock_);
    if (lease_) {
        IOLockUnlock(lock_);
        return std::unexpected(kIOReturnBusy);
    }
    auto lease = hub_->AllocateExternal(route, address_);
    if (lease) {
        lease_ = *lease;
    }
    IOLockUnlock(lock_);
    return lease;
}

void SPro40D3NotificationEndpoint::Release() noexcept {
    if (!lock_) {
        return;
    }
    std::optional<SPro40D3NotificationHub::Lease> lease;
    IOLockLock(lock_);
    lease = std::exchange(lease_, std::nullopt);
    if (lease) hub_->Release(*lease);
    IOLockUnlock(lock_);
    // Keep the weak callback installed: no active lease means HandleWrite()
    // rejects the write, while the reserved address remains non-reusable.
}

Async::ResponseCode SPro40D3NotificationEndpoint::HandleWrite(
    const Protocols::SBP2::AddressSpaceManager::RemoteWriteContext& context) noexcept {
    if (!lock_ || context.destination != address_) {
        return Async::ResponseCode::AddressError;
    }
    IOLockLock(lock_);
    const bool bound = lease_.has_value();
    IOLockUnlock(lock_);
    if (!bound) {
        return Async::ResponseCode::AddressError;
    }
    // Do not nest endpoint -> hub ingress: ingress invokes the injected clock,
    // which may reenter Release(). The manager callback's shared_ptr pins this
    // endpoint (and hub_) for the entire call. Release linearizes at the hub:
    // it either clears an already-enqueued copy or rejects this pending write.
    // A released address cannot be rebound, so no successor lease can receive it.
    const Async::LocalRequestContext request{
        .destOffset = context.destination,
        .tCode = context.tCode,
        .sourceID = context.sourceID,
        .generation = context.generation,
        .writePayload = context.payload,
        .dataLength = static_cast<uint32_t>(context.payload.size()),
    };
    return hub_->HandleLocalRequest(request).rcode;
}

} // namespace ASFW::Audio::DICE::Focusrite::SPro40D3
