// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// SPro40D3NotificationEndpoint.hpp - The host-side notification endpoint for
// one SPro40D3 mailbox engine (P03).
//
// RESERVATION: one 16-byte, collision-safe, software-served range is
// reserved ON DEMAND from the shared AddressSpaceManager (auto window in
// CSR space; no fixed vendor address). No always-allocated dormant manager:
// nothing exists until Activate, and a failed Activate holds zero
// allocations.
//
// INGRESS ATTRIBUTION: served at the LOCAL-REQUEST level. The manager's
// generic write callback omits source and generation, so the endpoint is an
// ILocalAddressHandler: every write is validated against the FULL
// destination, the source node and the bus generation at ingress, not
// after attribution information is lost (P03). An equal stale address is a
// CONFLICT, never self-proof - the device-side claim still goes through
// the engine's descriptor re-read + classification CAS.
//
// DELIVERY CONTRACT (P00 baseline ingress row): the handler runs on the
// dispatch context. The bounded frame is COPIED and deferred onto Default
// via ITimerScheduler::ScheduleAfter(0) BEFORE any engine state changes;
// at delivery the WEAK engine reference is upgraded (MOTU lifetime-slot
// pattern, MotuNotificationMailbox.hpp). No protocol pointer crosses the
// queues; a late or post-revocation write can therefore never touch a
// dangling engine. A refused schedule drops the frame (counted) - the
// dispatch context must never block.
//
// TEARDOWN ORDER (frozen here): (1) revoke ingress - the weak slot is
// cleared under the endpoint lock, so no scheduled delivery survives it;
// (2) deallocate the reserved range (owner-checked); (3) device-side
// release LAST - the engine's ownership CAS is corroborated: it succeeds
// only while the device still names us. When that release cannot be
// established (foreign owner took the mailbox), the caller marks the
// endpoint QUARANTINED: ingress stays revoked, the failure is retained,
// and re-activation requires an explicit ClearQuarantine decision - the
// endpoint never retries on its own (finite recovery policy).

#pragma once

#include "SPro40D3MailboxEngine.hpp"

#include "../../../../Protocols/SBP2/AddressSpaceManager.hpp"
#include "../../../../Async/Rx/LocalRequestDispatch.hpp"
#include "../../../../Scheduling/ITimerScheduler.hpp"
#include "../../../../Discovery/DeviceRouteToken.hpp"

#ifdef ASFW_HOST_TEST
#include "../../../../Testing/HostDriverKitStubs.hpp"
#endif
#include <DriverKit/IOLib.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>

namespace ASFW::Audio::DICE::Focusrite::SPro40D3 {

/// The largest frame the endpoint will ever deliver (the D3 notification
/// frame is 12 bytes; the reserved range is 16).
inline constexpr uint32_t kSPro40D3EndpointRangeBytes = 16;

class SPro40D3NotificationEndpoint final : public Async::ILocalAddressHandler {
public:
    using Engine = SPro40D3MailboxEngine;

    /// A successful Activate: the reserved 48-bit address (the value the
    /// caller then registers with the device via the engine's claim CAS)
    /// and the manager handle. address == 0 means failure.
    struct Activation {
        uint64_t address{0};
        uint64_t handle{0};
    };

    SPro40D3NotificationEndpoint(Protocols::SBP2::AddressSpaceManager& space,
                                 Scheduling::ITimerScheduler& scheduler) noexcept
        : space_{space}
        , scheduler_{scheduler}
        , lock_{IOLockAlloc()} {}

    ~SPro40D3NotificationEndpoint() override;

    SPro40D3NotificationEndpoint(const SPro40D3NotificationEndpoint&) = delete;
    SPro40D3NotificationEndpoint& operator=(const SPro40D3NotificationEndpoint&) = delete;

    /// Reserve the range and arm delivery for `engine` under `route`.
    /// Fails (address 0, zero manager allocations held) when the manager
    /// cannot serve the window or the endpoint is quarantined/active.
    /// The caller registers the returned address with the device itself.
    [[nodiscard]] Activation Activate(const std::shared_ptr<Engine>& engine,
                                      const Discovery::DeviceRouteToken& route);

    /// Revoke ingress and release the reservation (teardown order 1+2).
    /// Never touches the device; safe from any context, idempotent.
    void Deactivate();

    /// Record a failed device-side release (teardown order 3 could not be
    /// corroborated). Quarantine is retained state: Activate refuses until
    /// the operator/provider clears it explicitly.
    void MarkQuarantined();
    /// The explicit, finite recovery step - a deliberate operator/provider
    /// decision, never automatic.
    void ClearQuarantine();
    [[nodiscard]] bool Quarantined() const;

    /// The manager's remote-write ingress (P05 wiring): the AddressSpaceManager
    /// serves writes into its reserved ranges from the local-request chain
    /// (Sbp2LocalHandler -> ApplyRemoteWrite); this is the same validated
    /// delivery path HandleLocalRequest already implements, so hardware
    /// ingress and the test seam share one code path.
    void DeliverRemoteWrite(uint32_t offset, std::span<const uint8_t> payload);

    // -- ILocalAddressHandler (the local-request seam) -------------------

    [[nodiscard]] const char* Name() const noexcept override { return "SPro40D3Mailbox"; }
    [[nodiscard]] Async::LocalRequestResult HandleLocalRequest(
        const Async::LocalRequestContext& ctx) override;

    // -- Observers (telemetry/tests) ---------------------------------------

    [[nodiscard]] uint64_t ReservedAddress() const; ///< 0 when inactive
    /// Frames attributed and scheduled for delivery.
    [[nodiscard]] uint32_t AcceptedDeliveries() const;
    /// Answered but not delivered: wrong source node or stale generation.
    [[nodiscard]] uint32_t RejectedIngress() const;
    /// Answered with an error: wrong transaction type or partial overlap.
    [[nodiscard]] uint32_t MalformedIngress() const;
    /// Valid frames lost because the Default hop was refused.
    [[nodiscard]] uint32_t DroppedByScheduleFailure() const;

private:
    struct State {
        bool active{false};
        bool quarantined{false};
        uint64_t address{0};
        uint64_t handle{0};
        Discovery::DeviceRouteToken route{};
        std::weak_ptr<Engine> engine{};
        uint32_t accepted{0};
        uint32_t rejected{0};
        uint32_t malformed{0};
        uint32_t dropped{0};
    };

    Protocols::SBP2::AddressSpaceManager& space_;
    Scheduling::ITimerScheduler& scheduler_;
    IOLock* lock_;
    State state_{}; // all under lock_; no callback runs while it is held
    /// Revocation epoch: bumped on Deactivate. A scheduled delivery samples
    /// it when queued and re-checks it before delivering, so NO delivery
    /// survives revocation - even one already sitting on the Default queue.
    /// Shared so the closure keeps it alive without touching `this`.
    std::shared_ptr<std::atomic<uint64_t>> revokeEpoch_{
        std::make_shared<std::atomic<uint64_t>>(0)};
};

} // namespace ASFW::Audio::DICE::Focusrite::SPro40D3
