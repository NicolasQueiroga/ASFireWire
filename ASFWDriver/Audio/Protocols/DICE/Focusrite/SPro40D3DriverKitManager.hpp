// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
#pragma once

#include "SPro40D3NotificationEndpoint.hpp"
#include "../../../../Discovery/DeviceRegistry.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#ifdef ASFW_HOST_TEST
#include <functional>
#endif

namespace ASFW::Protocols::SBP2 {
class AddressSpaceManager;
}

namespace ASFW::Audio::DICE::Focusrite::SPro40D3 {

/// Copy-only cached state. No field requires a remote read. Unobserved descriptor,
/// session and owner state remain unavailable (zero), not synthesized from ROM.
/// Identity provenance is cached discovery/ROM only, never verified TCD descriptor
/// identification. identificationState 2 preserves that identity but sets generation
/// to zero and node to kInvalidNodeId because route freshness is revoked/unproven.
/// A strictly advanced, current registry route restores metadata state 1 and
/// routeValid only: never authorization, a session or I/O.
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
    uint64_t sessionToken{0};
    uint64_t runtimeGeneration{1};
    uint64_t pendingOperations{0};
    uint64_t cancelledOperations{0};
    uint64_t timedOutOperations{0};
    uint32_t identificationState{0}; // 0 unavailable, 1 cached discovery, 2 stale cached discovery
    uint32_t reservationState{0};    // 0 absent, 1 reserved, 2 retired/tombstoned
    uint32_t ownerProvenance{0};     // 0 unavailable; never inferred from reservation
    uint32_t lastError{static_cast<uint32_t>(kIOReturnNotPermitted)};
    uint32_t hardwareActivationDisabled{1};
    uint32_t routeValid{0}; // 1 only for a current, non-quarantined full registry route
};

/// Deliberately non-hierarchical: no scope grants any other scope.
enum class DiagnosticScope : uint8_t {
    PassiveDescriptor,
    NotificationOwnership,
    ReadOnlyMailbox,
    MixerRoutingWrite,
};

/// A permit is meaningful only for one identity/incarnation, generation/node/
/// route epoch, runtime epoch, session token, and exact scope. It is not itself
/// an authentication mechanism and never enables the production runtime.
struct DiagnosticAuthorization {
    DiagnosticScope scope{DiagnosticScope::PassiveDescriptor};
    Discovery::DeviceRouteToken route{};
    uint64_t sessionToken{0};
    uint64_t runtimeGeneration{0};
};

/// Dormant service-lifetime composition. Reserves a local notification address,
/// but NEVER binds a route, constructs a session/transport/engine, or issues bus
/// I/O. Debug and Release stay disabled even if the old experimental macro is
/// defined. There is no discovery activation or unrestricted register endpoint.
///
/// There is intentionally no dispatch queue until a real, independently reviewed
/// activation/cancellation protocol exists: synchronous self-dispatch cannot be
/// made safe by holding a lifecycle lock. Endpoint release and callbacks happen
/// outside the manager lock. The only nested lock operation is a non-callback
/// DeviceRegistry metadata snapshot/route check, serialized with Stop so borrowed
/// dependencies cannot be accessed after Stop returns.
///
/// Lifetime: retain this manager across every invoking API frame. Stop closes
/// admission and claims cancellation synchronously, but does not wait on an
/// already-claimed host callback (which could itself call Stop). Such a callback
/// must retain every observer and externally captured dependency until its outer
/// frame returns; Stop is not a drain fence for user callbacks. Manager-owned
/// borrowed dependencies may be destroyed after Stop: the completion epilogue
/// never accesses them after invoking the callback, and reentrant APIs cannot
/// access the registry once Stop has detached it.
/// Callbacks may reenter lifecycle methods, but must not throw or destroy the
/// invoking manager. A caller needing to destroy callback-owned dependencies must
/// first join/drain its callback frames outside any reentrant lifecycle call.
/// Late host completion tickets own only detached runtime state; after Stop they never
/// access the manager, registry, address allocator, clock or bus.
class SPro40D3DriverKitManager final {
    struct Runtime;
#ifdef ASFW_HOST_TEST
    struct SimulatedOperation;
#endif
public:
    static constexpr bool kProductionActivationEnabled = false;

    SPro40D3DriverKitManager(Discovery::DeviceRegistry& registry,
                             Protocols::SBP2::AddressSpaceManager& addresses,
                             DiceWaitClock& clock = DriverKitWaitClock::Shared()) noexcept;
    ~SPro40D3DriverKitManager();
    SPro40D3DriverKitManager(const SPro40D3DriverKitManager&) = delete;
    SPro40D3DriverKitManager& operator=(const SPro40D3DriverKitManager&) = delete;

    [[nodiscard]] IOReturn RequestActivation(DiagnosticScope scope) noexcept;
    [[nodiscard]] DriverKitDiagnosticSnapshot CopyDiagnostics(uint64_t guid) const noexcept;
    void OnBusReset() noexcept;
    void OnDeviceRemoved(uint64_t guid) noexcept;
    void Stop() noexcept;

#ifdef ASFW_HOST_TEST
    /// A closed simulator seam, not a public job queue: only one exact scope and
    /// current catalog-confirmed TCD3070 route can be admitted. No bus dependency
    /// is provided to this seam and a permit cannot activate hardware.
    struct SimulatedPolicy {
        Discovery::DeviceRouteToken route{};
        DiagnosticScope scope{DiagnosticScope::PassiveDescriptor};
    };
    class SimulatedCompletion final {
    public:
        SimulatedCompletion() = default;
        /// First terminal event wins. Timeout is explicit virtual-time expiry;
        /// no wall-clock sleeps or production timer are installed.
        [[nodiscard]] bool Complete(IOReturn status = kIOReturnSuccess) const;
    private:
        friend class SPro40D3DriverKitManager;
        std::shared_ptr<Runtime> runtime_;
        std::shared_ptr<SimulatedOperation> operation_;
    };
    [[nodiscard]] std::expected<DiagnosticAuthorization, IOReturn>
    AuthorizeSimulationForTesting(SimulatedPolicy policy);
    [[nodiscard]] std::expected<SimulatedCompletion, IOReturn>
    BeginSimulationForTesting(DiagnosticAuthorization authorization,
                              std::function<void(IOReturn)> completion);
#endif

private:
    void Quiesce(bool stopping, uint64_t removedGuid = 0) noexcept;
    std::shared_ptr<Runtime> runtime_;
    std::shared_ptr<SPro40D3NotificationHub> hub_;
    std::shared_ptr<SPro40D3NotificationEndpoint> endpoint_;
};

} // namespace ASFW::Audio::DICE::Focusrite::SPro40D3
