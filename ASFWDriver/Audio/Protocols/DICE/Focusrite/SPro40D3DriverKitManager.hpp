// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
#pragma once

#include "SPro40D3NotificationEndpoint.hpp"

#include "../../../../Discovery/DeviceRegistry.hpp"

#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/OSSharedPtr.h>

#include <atomic>
#include <cstdint>
#include <memory>

namespace ASFW::Protocols::SBP2 {
class AddressSpaceManager;
}

namespace ASFW::Audio::DICE::Focusrite::SPro40D3 {

#ifndef ASFW_SPRO40D3_EXPERIMENTAL_ACTIVATION
#define ASFW_SPRO40D3_EXPERIMENTAL_ACTIVATION 0
#endif

/// A copy-only production diagnostic. It is deliberately separate from a live
/// mailbox observation: producing it neither opens a DCP session nor submits a
/// FireWire operation. A future authorized diagnostic endpoint may use the
/// staged control queue and notification endpoint, but must not weaken the
/// scope ladder below.
struct DriverKitDiagnosticSnapshot {
    uint64_t guid{0};
    uint64_t notificationAddress{0};
    uint64_t foreignOwner{0};
    uint32_t vendorId{0};
    uint32_t modelId{0};
    uint32_t generation{0};
    uint32_t nodeId{Discovery::kInvalidNodeId};
    uint32_t sessionState{0};
    uint32_t activationState{0};
    uint32_t notificationState{0};
    uint32_t lastOperationStatus{static_cast<uint32_t>(kIOReturnNotPermitted)};
    uint64_t acceptedNotifications{0};
    uint64_t rejectedNotifications{0};
};

/// The scopes are intentionally non-hierarchical. An authorization to inspect
/// cached identity cannot register an owner, a registration permit cannot read
/// mailbox state, and a read permit cannot write mixer/routing state.
enum class DiagnosticScope : uint8_t {
    PassiveDescriptor,
    NotificationOwnership,
    ReadOnlyMailbox,
    MixerRoutingWrite,
};

/// Service-lifetime production composition for the TCD3070 mailbox. Constructing
/// it only creates a dedicated, non-audio control queue and reserves one local
/// software-served notification destination; it never sends a remote request,
/// performs an ownership CAS, binds a route, or constructs a mailbox engine.
///
/// The default policy is a hard deny. There is intentionally no UserClient
/// method or catalog/factory activation path in this stage. A later, separately
/// approved diagnostic surface must authenticate every individual scope before
/// posting onto the private control queue, and must retain this manager through completion.
class SPro40D3DriverKitManager final {
public:
    static constexpr bool kProductionActivationEnabled =
        ASFW_SPRO40D3_EXPERIMENTAL_ACTIVATION != 0;

    SPro40D3DriverKitManager(Discovery::DeviceRegistry& registry,
                             Protocols::SBP2::AddressSpaceManager& addresses,
                             DiceWaitClock& clock = DriverKitWaitClock::Shared()) noexcept;
    ~SPro40D3DriverKitManager();
    SPro40D3DriverKitManager(const SPro40D3DriverKitManager&) = delete;
    SPro40D3DriverKitManager& operator=(const SPro40D3DriverKitManager&) = delete;

    /// Always denies in Stage 2B. This is an executable guard against a
    /// compiled file becoming a live feature merely through device discovery.
    [[nodiscard]] IOReturn RequestActivation(DiagnosticScope scope) noexcept;

    /// Snapshot cached discovery and manager state only. Never performs I/O.
    [[nodiscard]] DriverKitDiagnosticSnapshot CopyDiagnostics(uint64_t guid) const noexcept;

    /// Reset/removal are callable from the service lifecycle. They revoke
    /// ingress on the control queue; no ownership-release CAS is attempted.
    void OnBusReset() noexcept;
    void OnDeviceRemoved(uint64_t guid) noexcept;
    void Stop() noexcept;

private:
    // Serializes teardown against cached-registry reads and lifecycle calls.
    // A retained manager can outlive service dependencies after Stop(); it
    // must not touch them once this gate is closed.
    mutable IOLock* lifecycleLock_{nullptr};
    Discovery::DeviceRegistry& registry_;
    std::shared_ptr<SPro40D3NotificationHub> hub_;
    std::shared_ptr<SPro40D3NotificationEndpoint> endpoint_;
    OSSharedPtr<IODispatchQueue> controlQueue_{};
    std::atomic<bool> stopped_{false};
    std::atomic<uint32_t> lastOperationStatus_{static_cast<uint32_t>(kIOReturnNotPermitted)};
};

} // namespace ASFW::Audio::DICE::Focusrite::SPro40D3
