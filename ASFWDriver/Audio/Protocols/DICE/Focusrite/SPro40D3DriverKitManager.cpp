// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "SPro40D3DriverKitManager.hpp"

#include "../../../../Protocols/SBP2/AddressSpaceManager.hpp"

#include <utility>

namespace ASFW::Audio::DICE::Focusrite::SPro40D3 {

SPro40D3DriverKitManager::SPro40D3DriverKitManager(
    Discovery::DeviceRegistry& registry, Protocols::SBP2::AddressSpaceManager& addresses,
    DiceWaitClock& clock) noexcept
    : lifecycleLock_(IOLockAlloc()), registry_(registry) {
    if (!lifecycleLock_) {
        lastOperationStatus_.store(static_cast<uint32_t>(kIOReturnNoMemory));
        return;
    }
    // A distinct serial queue prevents the synchronous mailbox adapter from
    // blocking PacketRouter/AR completions on the driver's Default queue.
    IODispatchQueue* rawQueue = nullptr;
    if (IODispatchQueue::Create("com.asfw.dice.spro40d3.control", 0, 0, &rawQueue) ==
            kIOReturnSuccess &&
        rawQueue != nullptr) {
        controlQueue_ = OSSharedPtr(rawQueue, OSNoRetain);
    } else {
        lastOperationStatus_.store(static_cast<uint32_t>(kIOReturnNoResources),
                                   std::memory_order_release);
        return;
    }

    // The invalid fixed region deliberately disables the old host-only bump
    // allocator. Stage 2B uses only an AddressSpaceManager-reserved external
    // address, so no imaginary DriverKit aperture enters production code.
    hub_ = std::make_shared<SPro40D3NotificationHub>(
        registry_, clock, SPro40D3NotificationHub::Region{.begin = 0, .endExclusive = 0});
    auto endpoint = SPro40D3NotificationEndpoint::Create(addresses, hub_);
    if (!endpoint) {
        lastOperationStatus_.store(static_cast<uint32_t>(endpoint.error()), std::memory_order_release);
        return;
    }
    endpoint_ = std::move(*endpoint);
}

SPro40D3DriverKitManager::~SPro40D3DriverKitManager() {
    Stop();
    if (lifecycleLock_) IOLockFree(lifecycleLock_);
}

IOReturn SPro40D3DriverKitManager::RequestActivation(DiagnosticScope) noexcept {
    // Do not turn source inclusion into activation. Future scope-specific
    // authorization must replace this deliberate terminal denial.
    lastOperationStatus_.store(static_cast<uint32_t>(kIOReturnNotPermitted),
                               std::memory_order_release);
    return kIOReturnNotPermitted;
}

DriverKitDiagnosticSnapshot
SPro40D3DriverKitManager::CopyDiagnostics(uint64_t guid) const noexcept {
    DriverKitDiagnosticSnapshot snapshot{};
    snapshot.guid = guid;
    if (!lifecycleLock_) return snapshot;
    IOLockLock(lifecycleLock_);
    snapshot.activationState = kProductionActivationEnabled ? 1U : 0U;
    snapshot.notificationAddress = endpoint_ ? endpoint_->address() : 0;
    snapshot.notificationState = endpoint_ ? 1U : 0U; // Reserved only; never device-registered.
    snapshot.lastOperationStatus = lastOperationStatus_.load(std::memory_order_acquire);

    if (!stopped_.load(std::memory_order_acquire)) {
        if (const auto record = registry_.SnapshotByGuid(guid)) {
            snapshot.vendorId = record->vendorId;
            snapshot.modelId = record->modelId;
            snapshot.generation = record->gen.value;
            snapshot.nodeId = record->nodeId;
        }
    }
    // No descriptor, firmware, owner, or engine state is invented. Those stay
    // zero/unavailable until a separately authorized live experiment records
    // them on the dedicated queue.
    IOLockUnlock(lifecycleLock_);
    return snapshot;
}

void SPro40D3DriverKitManager::OnBusReset() noexcept {
    if (!lifecycleLock_) return;
    IOLockLock(lifecycleLock_);
    if (stopped_.load(std::memory_order_acquire) || !controlQueue_) {
        IOLockUnlock(lifecycleLock_);
        return;
    }
    // Close ingress before reset/removal callers proceed. This never blocks a
    // FireWire completion queue: reset fanout originates on the service queue,
    // and the control queue does not issue any Stage 2B I/O.
#ifdef ASFW_HOST_TEST
    controlQueue_->DispatchSync([endpoint = endpoint_] {
        if (endpoint) endpoint->Release();
    });
#else
    controlQueue_->DispatchSync(^{
        if (endpoint_) endpoint_->Release();
    });
#endif
    IOLockUnlock(lifecycleLock_);
}

void SPro40D3DriverKitManager::OnDeviceRemoved(uint64_t) noexcept {
    // There is no active Stage 2B session. Still revoke ingress before any
    // future session teardown can run, and never issue an unverified release CAS.
    OnBusReset();
}

void SPro40D3DriverKitManager::Stop() noexcept {
    if (!lifecycleLock_) return;
    IOLockLock(lifecycleLock_);
    bool expected = false;
    if (!stopped_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        IOLockUnlock(lifecycleLock_);
        return;
    }
    if (controlQueue_) {
#ifdef ASFW_HOST_TEST
        controlQueue_->DispatchSync([endpoint = endpoint_] {
            if (endpoint) endpoint->Release();
        });
#else
        controlQueue_->DispatchSync(^{
            if (endpoint_) endpoint_->Release();
        });
#endif
    }
    endpoint_.reset();
    hub_.reset();
    controlQueue_.reset();
    IOLockUnlock(lifecycleLock_);
}

} // namespace ASFW::Audio::DICE::Focusrite::SPro40D3
