// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// SPro40D3EndpointTests.cpp - P03: the real notification endpoint, exercised
// through the local-request seam (Async::LocalRequestDispatch::Route ->
// ILocalAddressHandler), full pipeline to the P02 mailbox engine.
//
// The rig wires: simulated device (transport) + virtual-clock scheduler
// (the Default-hop model) + AddressSpaceManager (host mode) + the endpoint
// (as a dispatch participant) + the async engine. Ingress is driven by
// ROUTED LocalRequestContexts - the exact production path minus only the
// OHCI packet source (P05 wires the chain into LocalRequestWiring).

#include "Audio/Protocols/DICE/Focusrite/SPro40D3NotificationEndpoint.hpp"
#include "SPro40D3SimulatedDevice.hpp"
#include "SPro40D3SimulatedScheduler.hpp"

#include "Hardware/IEEE1394.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <optional>

namespace {

using ASFW::Testing::DICE::SPro40D3SimulatedDevice;
using ASFW::Testing::DICE::SPro40D3SimulatedScheduler;
using ASFW::Testing::DICE::kSPro40D3DescriptorBytes;
namespace D3 = ASFW::Audio::DICE::Focusrite::SPro40D3;
namespace Async = ASFW::Async;
using AReq = ASFW::Async::HW::AsyncRequestHeader;

constexpr uint64_t kGuid = 0x5A03D31000000001ULL;

class EndpointRig {
public:
    using Engine = D3::SPro40D3MailboxEngine;
    using Endpoint = D3::SPro40D3NotificationEndpoint;

    explicit EndpointRig(D3::SPro40D3MailboxEngine::Options engineOptions = {},
                         uint32_t generation = 5, uint16_t node = 3,
                         ASFW::Protocols::SBP2::AddressSpaceManager* sharedSpace = nullptr)
        : sim_{[](std::span<const uint8_t>) {}} // ingress is driven via the dispatch
        , space_{nullptr}                       // host mode: no DMA
        , route_{} {
        spacePtr_ = sharedSpace ? sharedSpace : &space_;
        auto descriptor = D3::ParseDcpDescriptor(
            std::span<const uint8_t>(kSPro40D3DescriptorBytes));
        engine_ = std::make_shared<Engine>(sim_, clock_, *descriptor, engineOptions);
        // The local-request SEAM (ILocalAddressHandler + context + result)
        // exercised directly: the production dispatch is a thin first-claim
        // loop over self-filtering handlers (LocalRequestWiring.cpp), and
        // the chain wiring itself is P05 scope. With one D3 participant the
        // contract here is identical to Route().
        endpoint_ = new Endpoint(*spacePtr_, clock_);
        route_.guid = kGuid;
        route_.deviceIncarnation = 1;
        route_.routeEpoch = 1;
        route_.generation = ASFW::Discovery::Generation{generation};
        route_.nodeId = node;
    }

    Engine& engine() noexcept { return *engine_; }
    ASFW::Protocols::SBP2::AddressSpaceManager& space() noexcept { return *spacePtr_; }
    std::shared_ptr<Engine> enginePtr() noexcept { return engine_; }
    Endpoint& endpoint() noexcept { return *endpoint_; }
    SPro40D3SimulatedDevice& sim() noexcept { return sim_; }
    SPro40D3SimulatedScheduler& clock() noexcept { return clock_; }
    [[nodiscard]] uint64_t reservedAddress() const noexcept { return address_; }
    [[nodiscard]] uint16_t node() const noexcept { return route_.nodeId; }
    [[nodiscard]] uint32_t generation() const noexcept { return route_.generation.value; }

    [[nodiscard]] bool activate() {
        const auto activation = endpoint_->Activate(engine_, route_);
        address_ = activation.address;
        return address_ != 0;
    }
    [[nodiscard]] bool registerHost() {
        std::optional<std::expected<void, D3::MailboxError>> result;
        const auto accepted = engine_->BeginRegister(
            address_, [&](std::expected<void, D3::MailboxError> r) { result = r; });
        return accepted.has_value() && result.has_value() && result->has_value();
    }

    /// Route one local request through the dispatch, exactly as production
    /// ingress would. Payload is copied into the context the same way the
    /// packet path builds it; the endpoint copies again before deferring.
    [[nodiscard]] Async::LocalRequestResult routeWrite(std::span<const uint8_t> payload,
                                                       uint64_t dest,
                                                       std::optional<uint32_t> gen = {},
                                                       std::optional<uint16_t> source = {},
                                                       uint8_t tCode = AReq::kTcodeWriteBlock) {
        std::vector<uint8_t> bytes(payload.begin(), payload.end());
        Async::LocalRequestContext ctx{};
        ctx.destOffset = dest;
        ctx.tCode = tCode;
        ctx.sourceID = source.value_or(route_.nodeId);
        ctx.generation = gen.value_or(route_.generation.value);
        ctx.writePayload = bytes;
        ctx.dataLength = static_cast<uint32_t>(bytes.size());
        return endpoint_->HandleLocalRequest(ctx);
    }

    /// A well-formed 12-byte notification frame (engine-parseable,
    /// live-hardware shape: request opcode echo at [0..4]).
    static std::array<uint8_t, 12> notifFrame(
        uint16_t rlen, uint32_t rstatus,
        const D3::Opcode& opcode = D3::kOpcodeGetControl) {
        std::array<uint8_t, 12> frame{};
        frame[0] = opcode[0]; frame[1] = opcode[1];
        frame[2] = opcode[2]; frame[3] = opcode[3];
        frame[6] = static_cast<uint8_t>(rlen >> 8);
        frame[7] = static_cast<uint8_t>(rlen);
        frame[8] = static_cast<uint8_t>(rstatus >> 24);
        frame[9] = static_cast<uint8_t>(rstatus >> 16);
        frame[10] = static_cast<uint8_t>(rstatus >> 8);
        frame[11] = static_cast<uint8_t>(rstatus);
        return frame;
    }

private:
    SPro40D3SimulatedDevice sim_;
    ASFW::Protocols::SBP2::AddressSpaceManager space_;
    ASFW::Protocols::SBP2::AddressSpaceManager* spacePtr_{nullptr};
    SPro40D3SimulatedScheduler clock_;
    std::shared_ptr<Engine> engine_;
    D3::SPro40D3NotificationEndpoint* endpoint_{nullptr};
    ASFW::Discovery::DeviceRouteToken route_{};
    uint64_t address_{0};
};

// ---------------------------------------------------------------------------
// Reservation and activation
// ---------------------------------------------------------------------------

TEST(SPro40D3Endpoint, ActivateReservesCollisionSafeRangeAndRegistersEngine) {
    EndpointRig rig;
    ASSERT_TRUE(rig.activate());
    const uint64_t address = rig.reservedAddress();
    // Auto window in CSR space: 0xFFFF'0010'0000..0xFFFF'0FFF'FFFF.
    ASSERT_GE(address, 0xFFFF0010'0000ULL);
    ASSERT_LE(address, 0xFFFF0FFF'FFFFULL - 16);
    ASSERT_EQ(address & 0x7ULL, 0u); // the manager's own window alignment
    ASSERT_TRUE(rig.registerHost());
    EXPECT_EQ(rig.engine().NotificationAddress(), address);
    EXPECT_EQ(rig.sim().Owner(), address);
    EXPECT_EQ(rig.endpoint().AcceptedDeliveries(), 0u);
    EXPECT_EQ(rig.endpoint().RejectedIngress(), 0u);
}

TEST(SPro40D3Endpoint, CompetingEndpointsDoNotCollide) {
    // ONE driver shares ONE AddressSpaceManager: competing endpoints must
    // draw disjoint ranges from it.
    ASFW::Protocols::SBP2::AddressSpaceManager sharedSpace{nullptr};
    EndpointRig a{D3::SPro40D3MailboxEngine::Options{}, 5, 3, &sharedSpace};
    EndpointRig b{D3::SPro40D3MailboxEngine::Options{}, 6, 4, &sharedSpace};
    EndpointRig c{D3::SPro40D3MailboxEngine::Options{}, 7, 9, &sharedSpace};
    ASSERT_TRUE(a.activate());
    ASSERT_TRUE(b.activate());
    ASSERT_TRUE(c.activate());
    const auto aa = a.reservedAddress();
    const auto ab = b.reservedAddress();
    const auto ac = c.reservedAddress();
    EXPECT_NE(aa, ab);
    EXPECT_NE(aa, ac);
    EXPECT_NE(ab, ac);
    // 16-byte ranges must not overlap.
    EXPECT_TRUE(aa + 16 <= ab || ab + 16 <= aa);
    EXPECT_TRUE(aa + 16 <= ac || ac + 16 <= aa);
    EXPECT_TRUE(ab + 16 <= ac || ac + 16 <= ab);
}

TEST(SPro40D3Endpoint, FailedActivationHoldsZeroAllocations) {
    // Every reachable refusal (null engine, null route, double activate)
    // must leave the manager holding nothing for the endpoint.
    EndpointRig rig;
    ASFW::Discovery::DeviceRouteToken badRoute{}; // explicit-bool false
    EXPECT_EQ(rig.endpoint().Activate({}, badRoute).address, 0u);
    EXPECT_EQ(rig.endpoint().Activate(nullptr, {}).address, 0u);
    ASSERT_TRUE(rig.activate());
    EXPECT_EQ(rig.endpoint().Activate(rig.enginePtr(), {}).address, 0u); // double
    // The FIRST activation still stands.
    EXPECT_NE(rig.endpoint().ReservedAddress(), 0u);
}

// ---------------------------------------------------------------------------
// Ingress attribution: full destination, source, generation (the seam)
// ---------------------------------------------------------------------------

TEST(SPro40D3Endpoint, ValidFrameDeliversViaDefaultHopAndEngineCountsStale) {
    // A well-formed frame with no command in flight is unattributable by
    // the ENGINE (stale) - proving the endpoint delivered it, and proving
    // the DEFAULT HOP: nothing happens until the clock advances.
    EndpointRig rig;
    ASSERT_TRUE(rig.activate());
    const auto frame = EndpointRig::notifFrame(0, 0);
    const auto result = rig.routeWrite(frame, rig.reservedAddress());
    EXPECT_TRUE(result.claimed);
    EXPECT_EQ(result.rcode, Async::ResponseCode::Complete);
    EXPECT_EQ(rig.endpoint().AcceptedDeliveries(), 1u);
    EXPECT_EQ(rig.engine().StaleNotificationCount(), 0u); // deferred, not yet run
    rig.clock().AdvanceNs(0); // the Default hop fires
    EXPECT_EQ(rig.engine().StaleNotificationCount(), 1u);
}

TEST(SPro40D3Endpoint, RemoteWriteThroughTheManagerReachesTheEngine) {
    // The P05 ingress wiring, hardware-shaped: production ingress is
    // OHCI -> packet router -> LocalRequestDispatch -> Sbp2LocalHandler ->
    // AddressSpaceManager::ApplyRemoteWrite -> THIS endpoint's registered
    // callback. Before P05 the range swallowed remote writes silently -
    // the device believed it delivered. The manager path now lands in the
    // same validated HandleLocalRequest delivery the dispatch seam uses.
    EndpointRig rig;
    ASSERT_TRUE(rig.activate());
    const auto frame = EndpointRig::notifFrame(0, 0);
    const auto rcode = rig.space().ApplyRemoteWrite(rig.reservedAddress(), frame);
    EXPECT_EQ(rcode, Async::ResponseCode::Complete);
    EXPECT_EQ(rig.endpoint().AcceptedDeliveries(), 1u); // attributed, deferred
    EXPECT_EQ(rig.engine().StaleNotificationCount(), 0u);
    rig.clock().AdvanceNs(0); // the Default hop fires
    EXPECT_EQ(rig.engine().StaleNotificationCount(), 1u); // delivered: no op in flight
}

TEST(SPro40D3Endpoint, WrongSourceAndWrongGenerationRejectedAtIngress) {
    EndpointRig rig;
    ASSERT_TRUE(rig.activate());
    const auto frame = EndpointRig::notifFrame(0, 0);

    // Wrong source node: answered, never delivered.
    auto wrongSource = rig.routeWrite(frame, rig.reservedAddress(), {}, 0x3F);
    EXPECT_TRUE(wrongSource.claimed);
    EXPECT_EQ(wrongSource.rcode, Async::ResponseCode::Complete);
    rig.clock().AdvanceNs(0);
    EXPECT_EQ(rig.endpoint().RejectedIngress(), 1u);
    EXPECT_EQ(rig.endpoint().AcceptedDeliveries(), 0u);
    EXPECT_EQ(rig.engine().StaleNotificationCount(), 0u);

    // Stale generation: same policy.
    auto staleGen = rig.routeWrite(frame, rig.reservedAddress(), rig.generation() + 1);
    EXPECT_TRUE(staleGen.claimed);
    EXPECT_EQ(staleGen.rcode, Async::ResponseCode::Complete);
    rig.clock().AdvanceNs(0);
    EXPECT_EQ(rig.endpoint().RejectedIngress(), 2u);
    EXPECT_EQ(rig.endpoint().AcceptedDeliveries(), 0u);
}

TEST(SPro40D3Endpoint, TwoDevicesEachAcceptOnlyTheirOwnFrames) {
    EndpointRig rigA{D3::SPro40D3MailboxEngine::Options{}, 5, 3};
    EndpointRig rigB{D3::SPro40D3MailboxEngine::Options{}, 7, 9};
    ASSERT_TRUE(rigA.activate());
    ASSERT_TRUE(rigB.activate());

    // A's device writes to A's range: delivered to A.
    const auto frame = EndpointRig::notifFrame(0, 0);
    EXPECT_TRUE(rigA.routeWrite(frame, rigA.reservedAddress()).claimed);
    // A's frame to B's RANGE but with A's source/generation: B rejects
    // (attribution is source+generation, not just destination).
    EXPECT_TRUE(rigB.routeWrite(frame, rigB.reservedAddress(), 5, 3).claimed);
    rigA.clock().AdvanceNs(0);
    rigB.clock().AdvanceNs(0);
    EXPECT_EQ(rigA.engine().StaleNotificationCount(), 1u);
    EXPECT_EQ(rigB.endpoint().RejectedIngress(), 1u);
    EXPECT_EQ(rigB.engine().StaleNotificationCount(), 0u);

    // B's own frame delivers to B.
    EXPECT_TRUE(rigB.routeWrite(frame, rigB.reservedAddress()).claimed);
    rigB.clock().AdvanceNs(0);
    EXPECT_EQ(rigB.engine().StaleNotificationCount(), 1u);
}

TEST(SPro40D3Endpoint, OffsetsShortAndOversizedFrames) {
    EndpointRig rig;
    ASSERT_TRUE(rig.activate());
    const uint64_t base = rig.reservedAddress();

    // Outside the range entirely (no overlap): not ours.
    EXPECT_FALSE(rig.routeWrite(EndpointRig::notifFrame(0, 0), base - 16).claimed);
    EXPECT_FALSE(rig.routeWrite(EndpointRig::notifFrame(0, 0), base + 16).claimed);
    EXPECT_EQ(rig.endpoint().MalformedIngress(), 0u);

    // STARTING before the range but overlapping it: ours to answer with a
    // shape error, never to deliver.
    std::array<uint8_t, 12> overlap{};
    auto partial = rig.routeWrite(overlap, base - 4);
    EXPECT_TRUE(partial.claimed);
    EXPECT_EQ(partial.rcode, Async::ResponseCode::DataError);
    EXPECT_EQ(rig.endpoint().MalformedIngress(), 1u);

    // Overrunning the range: same policy.
    std::array<uint8_t, 12> big{};
    auto overrun = rig.routeWrite(big, base + 8);
    EXPECT_TRUE(overrun.claimed);
    EXPECT_EQ(overrun.rcode, Async::ResponseCode::DataError);
    EXPECT_EQ(rig.endpoint().MalformedIngress(), 2u);

    // Quadlet write inside the range: wrong transaction type.
    std::array<uint8_t, 4> quad{};
    auto asQuad = rig.routeWrite(quad, base, {}, {}, AReq::kTcodeWriteQuad);
    EXPECT_TRUE(asQuad.claimed);
    EXPECT_EQ(asQuad.rcode, Async::ResponseCode::TypeError);
    EXPECT_EQ(rig.endpoint().MalformedIngress(), 3u);

    // Zero-length block write: no bytes in range, nobody's to answer.
    EXPECT_FALSE(rig.routeWrite({}, base).claimed);

    // Short frame at a nonzero offset, fully inside: delivered - the ENGINE's
    // shape rules decide (counted malformed by the engine, not trusted).
    std::array<uint8_t, 4> shortFrame{};
    auto delivered = rig.routeWrite(shortFrame, base + 4);
    EXPECT_TRUE(delivered.claimed);
    EXPECT_EQ(delivered.rcode, Async::ResponseCode::Complete);
    rig.clock().AdvanceNs(0);
    EXPECT_EQ(rig.engine().MalformedNotificationCount(), 1u);

    // A full 16-byte frame inside the range: delivered. Its header parses
    // (>= 12 bytes), so with no command in flight the ENGINE counts it as
    // well-formed-but-unattributable (stale) - the endpoint delivered; the
    // engine's attribution rules did the rest.
    std::array<uint8_t, 16> sixteen{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    EXPECT_TRUE(rig.routeWrite(sixteen, base).claimed);
    rig.clock().AdvanceNs(0);
    EXPECT_EQ(rig.engine().StaleNotificationCount(), 1u);
    EXPECT_EQ(rig.engine().MalformedNotificationCount(), 1u); // still just the 4-byte frame
}

TEST(SPro40D3Endpoint, ScheduleRefusalDropsFrameAndCounts) {
    // The Default hop is refused: the frame is lost (counted), the dispatch
    // still gets its answer, and the engine is never touched.
    EndpointRig rig;
    ASSERT_TRUE(rig.activate());
    rig.clock().ArmRejectNextSchedules(1);
    const auto frame = EndpointRig::notifFrame(0, 0);
    const auto result = rig.routeWrite(frame, rig.reservedAddress());
    EXPECT_TRUE(result.claimed);
    EXPECT_EQ(result.rcode, Async::ResponseCode::Complete);
    rig.clock().AdvanceNs(0);
    EXPECT_EQ(rig.endpoint().DroppedByScheduleFailure(), 1u);
    EXPECT_EQ(rig.endpoint().AcceptedDeliveries(), 0u);
    EXPECT_EQ(rig.engine().StaleNotificationCount(), 0u);
}

// ---------------------------------------------------------------------------
// The false self-owner rule
// ---------------------------------------------------------------------------

TEST(SPro40D3Endpoint, EqualStaleAddressIsAdoptedDifferentStaysConflict) {
    // The newly reserved range address happens to equal the value the
    // device's owner field already holds (a stale registration from a
    // previous host incarnation). Live-hardware semantics (2026-11-06):
    // that exact value IS ours by construction (the fixed window, the
    // host's range covers it) and the registration is ADOPTED with zero
    // device writes - a release/reclaim cycle would strand the rebind's
    // descriptor read across the owner transition. The endpoint never
    // short-circuits "the device already names me"; the claim CAS still
    // classifies, and only a DIFFERENT value stays a conflict.
    EndpointRig rig;
    ASSERT_TRUE(rig.activate());
    rig.sim().ForceOwner(rig.reservedAddress()); // stale equal value
    std::optional<std::expected<void, D3::MailboxError>> result;
    const auto accepted = rig.engine().BeginRegister(
        rig.reservedAddress(),
        [&](std::expected<void, D3::MailboxError> r) { result = r; });
    ASSERT_TRUE(accepted.has_value());
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->has_value()); // adopted
    EXPECT_EQ(rig.engine().GetRegistrationState(),
              D3::SPro40D3MailboxEngine::RegistrationState::kRegistered);
    // A stale value at any OTHER address is still foreign: conflict.
    // (Bus reset first: while REGISTERED the registration is state-based
    // idempotent and no CAS runs at all.)
    rig.engine().OnBusReset();
    rig.sim().ForceOwner(rig.reservedAddress() ^ 0x1);
    std::optional<std::expected<void, D3::MailboxError>> result2;
    const auto accepted2 = rig.engine().BeginRegister(
        rig.reservedAddress(),
        [&](std::expected<void, D3::MailboxError> r) { result2 = r; });
    ASSERT_TRUE(accepted2.has_value());
    ASSERT_TRUE(result2.has_value());
    EXPECT_EQ(result2->error(), D3::MailboxError::kOwnershipConflict);
}

// ---------------------------------------------------------------------------
// Full pipeline: routed ingress completes a real mailbox exchange
// ---------------------------------------------------------------------------

TEST(SPro40D3Endpoint, RoutedNotificationCompletesPendingCommand) {
    // The flagship: a get_control exchange whose ONLY notification ingress
    // is a routed local request. The write lands inline; the notification
    // arrives through the endpoint's Default hop; the engine's paired read
    // and dispatch complete the command.
    EndpointRig rig;
    ASSERT_TRUE(rig.activate());
    ASSERT_TRUE(rig.registerHost());

    std::optional<std::expected<uint32_t, D3::MailboxError>> value;
    const auto accepted = rig.engine().SendGetControl32(
        D3::control_offset::kDim,
        [&](std::expected<uint32_t, D3::MailboxError> r) { value = r; });
    ASSERT_TRUE(accepted.has_value());
    EXPECT_FALSE(value.has_value()); // in flight: no notification has arrived

    // The device's answer: rlen 4, rstatus 0 (the sim built the response
    // inline; only its notification never reached us - until now).
    const auto frame = EndpointRig::notifFrame(4, 0);
    const auto result = rig.routeWrite(frame, rig.reservedAddress());
    EXPECT_TRUE(result.claimed);
    EXPECT_EQ(result.rcode, Async::ResponseCode::Complete);
    EXPECT_FALSE(value.has_value()); // still deferred onto Default
    rig.clock().AdvanceNs(0);

    ASSERT_TRUE(value.has_value());
    ASSERT_TRUE(value->has_value());
    EXPECT_EQ(rig.engine().ControlState().dim.provenance, D3::ValueProvenance::Observed);
    EXPECT_EQ(rig.sim().UnpairedResponseReads(), 0u);
    EXPECT_FALSE(rig.engine().OperationInFlight());
}

// ---------------------------------------------------------------------------
// Release, revocation, quarantine (the lifecycle)
// ---------------------------------------------------------------------------

TEST(SPro40D3Endpoint, DeactivateRevokesIngressAndLateWritesAreNoOps) {
    // Teardown order 1: revoke ingress. A write arriving afterwards is not
    // the endpoint's anymore, and a scheduled delivery that upgrades an
    // empty weak slot is a harmless no-op.
    EndpointRig rig;
    ASSERT_TRUE(rig.activate());
    ASSERT_TRUE(rig.registerHost());
    const uint64_t base = rig.reservedAddress();

    // A frame scheduled but NOT yet fired when teardown happens.
    const auto frame = EndpointRig::notifFrame(0, 0);
    EXPECT_TRUE(rig.routeWrite(frame, base).claimed);
    rig.endpoint().Deactivate();

    // The scheduled delivery fires into a revoked endpoint: no-op.
    rig.clock().AdvanceNs(0);
    EXPECT_EQ(rig.engine().StaleNotificationCount(), 0u);

    // Late writes after revocation: not ours, nothing delivered.
    EXPECT_FALSE(rig.routeWrite(frame, base).claimed);
    rig.clock().AdvanceNs(0);
    EXPECT_EQ(rig.engine().StaleNotificationCount(), 0u);
    EXPECT_EQ(rig.endpoint().ReservedAddress(), 0u);

    // The engine is untouched and still finishes its own deadline policy.
    std::optional<std::expected<void, D3::MailboxError>> value;
    ASSERT_TRUE(rig.engine()
                    .SendSetVolume(0, 0, 7,
                                   [&](std::expected<void, D3::MailboxError> r) { value = r; })
                    .has_value());
    rig.clock().AdvanceToMs(200);
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(value->error(), D3::MailboxError::kTimeout);
}

TEST(SPro40D3Endpoint, ReleaseCallbackDuringTeardownIsSafe) {
    // The device-side release runs while ingress is already revoked: a
    // notification racing the release finds the endpoint inactive and is
    // never delivered into a tearing-down engine.
    EndpointRig rig{D3::SPro40D3MailboxEngine::Options{.allowOwnershipRelease = true}};
    ASSERT_TRUE(rig.activate());
    ASSERT_TRUE(rig.registerHost());

    std::optional<std::expected<void, D3::MailboxError>> release;
    const auto accepted = rig.engine().BeginUnregister(
        [&](std::expected<void, D3::MailboxError> r) { release = r; });
    ASSERT_TRUE(accepted.has_value());
    // Teardown order: revoke ingress BEFORE the release CAS completes.
    rig.endpoint().Deactivate();
    ASSERT_TRUE(release.has_value());
    EXPECT_TRUE(release->has_value());
    EXPECT_EQ(rig.sim().Owner(), D3::kDcpOwnerNone);

    // The racing notification: revoked, not delivered (the cached old
    // address is no longer the endpoint's after teardown).
    const auto frame = EndpointRig::notifFrame(0, 0);
    EXPECT_FALSE(rig.routeWrite(frame, rig.reservedAddress()).claimed);
    rig.clock().AdvanceNs(0);
    EXPECT_EQ(rig.engine().StaleNotificationCount(), 0u);
}

TEST(SPro40D3Endpoint, FailedReleaseQuarantinesWithFiniteRecovery) {
    // The device-side release cannot be corroborated (a foreign owner took
    // the mailbox): the endpoint is marked quarantined - Activate refuses
    // until the operator/provider clears it EXPLICITLY (finite recovery:
    // no automatic retries anywhere).
    EndpointRig rig{D3::SPro40D3MailboxEngine::Options{.allowOwnershipRelease = true}};
    ASSERT_TRUE(rig.activate());
    ASSERT_TRUE(rig.registerHost());

    rig.sim().ForceOwner(0x0000BEEF00000000ULL); // foreign claimant wins
    std::optional<std::expected<void, D3::MailboxError>> release;
    const auto accepted = rig.engine().BeginUnregister(
        [&](std::expected<void, D3::MailboxError> r) { release = r; });
    ASSERT_TRUE(accepted.has_value());
    ASSERT_TRUE(release.has_value());
    EXPECT_EQ(release->error(), D3::MailboxError::kOwnershipConflict);

    // Host-side teardown proceeds even when the device-side release
    // failed (the reservation is not the mailbox); quarantine then retains
    // the failure and refuses activation until cleared EXPLICITLY.
    rig.endpoint().Deactivate();
    rig.endpoint().MarkQuarantined();
    EXPECT_TRUE(rig.endpoint().Quarantined());
    EXPECT_EQ(rig.endpoint().Activate(rig.enginePtr(), {}).address, 0u);
    rig.endpoint().ClearQuarantine();
    EXPECT_FALSE(rig.endpoint().Quarantined());
    // Host-side recovery is possible immediately. The DEVICE-side claim is
    // still the foreign owner's (claim-from-unowned-only): re-claiming the
    // mailbox is the provider's re-probe policy (P04+), never the
    // endpoint's.
    EXPECT_TRUE(rig.activate());
}

} // namespace
