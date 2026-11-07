// SPDX-License-Identifier: Apache-2.0
// SPro40D3ProtocolTests.cpp - the native provider end to end: Rebind chain
// (live descriptor read -> endpoint activation -> claim), the readable-group
// Prepare, publication through the generic snapshot document, the refresh
// action, honest refusals, and route-change invalidation. The simulated
// device is injected through the protocol's transport seam; notifications
// are delivered through the endpoint's local-request seam exactly as the
// P05 dispatch chain will (the peer substitutes for that not-yet-wired
// chain; the mailbox semantics themselves are the real engine's).
#include "Audio/Protocols/DICE/Focusrite/SPro40D3Protocol.hpp"
#include "Audio/Protocols/DICE/Focusrite/SPro40D3Routing.hpp"

#include "DICEDuplexTestSupport.hpp"
#include "FakeDiceWaitClock.hpp"
#include "FakeTimerScheduler.hpp"
#include "RecordingFireWireBus.hpp"
#include "SPro40D3SimulatedDevice.hpp"
#include "Hardware/IEEE1394.hpp"
#include "SPro40D3SimulatedScheduler.hpp"
#include "SPro40D3SimulatedScheduler.hpp"
#include "Audio/Protocols/DICE/Focusrite/SPro40D3NotificationEndpoint.hpp"
#include "Protocols/SBP2/AddressSpaceManager.hpp"
#include "UserClient/WireFormats/ControlSnapshotDocument.hpp"
#include <gtest/gtest.h>
#include <atomic>
#include <memory>
#include <optional>
#include <vector>

using namespace ASFW::Audio::DICE::Focusrite::SPro40D3;
using ASFW::Testing::DICE::RouteState;
using ASFW::Testing::DICE::SPro40D3SimulatedDevice;
using ASFW::Testing::DICE::SPro40D3SimulatedScheduler;
namespace AM = ASFW::AudioModel;
namespace Async = ASFW::Async;
using AReq = ASFW::Async::HW::AsyncRequestHeader;

/// Access to the protocol-owned endpoint for delivery and inspection (the
/// DICETcatProtocolTestPeer pattern). Global scope: the protocol befriends
/// this exact name.
struct SPro40D3ProtocolTestPeer {
    static SPro40D3NotificationEndpoint& Endpoint(SPro40D3Protocol& p) { return *p.endpoint_; }
    static SPro40D3MailboxEngine& Engine(SPro40D3Protocol& p) { return *p.engine_; }
    static ASFW::Discovery::DeviceRouteToken Route(SPro40D3Protocol& p) {
        return p.io_.Route();
    }
};

namespace {

class ProtocolRig final {
public:
    explicit ProtocolRig(SPro40D3SimulatedDevice::Options simOptions = {}) {
        sim_ = std::make_unique<SPro40D3SimulatedDevice>(
            [this](std::span<const uint8_t> frame) { Deliver(frame); }, simOptions);
        simPtr_ = sim_.get();
        clock_ = std::make_unique<SPro40D3SimulatedScheduler>();
        fakeTimer_ = std::make_unique<ASFW::Testing::FakeTimerScheduler>();
        waitClock_ = std::make_unique<ASFW::Testing::FakeDiceWaitClock>(*fakeTimer_);
        space_ = std::make_unique<ASFW::Protocols::SBP2::AddressSpaceManager>(nullptr);
        protocol_ = std::make_shared<SPro40D3Protocol>(
            bus_, bus_, route_.registry, route_.route, nullptr, *waitClock_, nullptr,
            clock_.get(), space_.get(), std::move(sim_));
    }

    [[nodiscard]] SPro40D3Protocol& protocol() noexcept { return *protocol_; }
    [[nodiscard]] SPro40D3SimulatedDevice& sim() noexcept { return *simPtr_; }
    [[nodiscard]] SPro40D3SimulatedScheduler& clock() noexcept { return *clock_; }

    /// Drives an asynchronous protocol operation to completion on simulated
    /// time. The quantum is 1 ms - far below the 200 ms command deadline -
    /// so 0-delay hops (notification delivery, paired-read completions)
    /// fire well before their deadlines, exactly like the engine and
    /// endpoint suites drive theirs. A 200 ms quantum would race each
    /// op's deadline inside one scheduler batch.
    [[nodiscard]] std::optional<IOReturn> Pump(std::function<void()> start) {
        std::optional<IOReturn> result;
        start();
        for (int step = 0; !result && step < 8192; ++step) {
            clock_->AdvanceNs(1'000'000);
            result = std::exchange(pending_, {});
        }
        // Drain straggler 0-delay hops (e.g. the device's unsolicited
        // connect-requested notification from the claim CAS): they must
        // land BETWEEN operations, where the engine counts them stale,
        // never inside a subsequent in-flight command. Four quanta flush
        // every chained hop this rig can produce.
        if (result) {
            for (int drain = 0; drain < 4; ++drain) {
                clock_->AdvanceNs(1'000'000);
            }
        }
        return result;
    }
    /// Callback adapter: captures the status for Pump().
    [[nodiscard]] std::function<void(IOReturn)> Capture() {
        return [this](IOReturn kr) { pending_ = kr; };
    }
    /// Callback adapter for SetParameter completions: captures the status
    /// AND the reported value (the observed truth, never the intent).
    [[nodiscard]] auto CaptureValue() {
        return [this](IOReturn kr, const ASFW::AudioModel::ParameterValue& v) {
            pending_ = kr;
            pendingValue_ = v;
        };
    }
    [[nodiscard]] const ASFW::AudioModel::ParameterValue& CapturedValue() const noexcept {
        return pendingValue_;
    }

private:
    void Deliver(std::span<const uint8_t> frame) {
        if (!protocol_) {
            return;
        }
        auto& endpoint = SPro40D3ProtocolTestPeer::Endpoint(*protocol_);
        const auto address = endpoint.ReservedAddress();
        if (address == 0) {
            return; // not bound (or revoked): late frames are no-ops
        }
        Async::LocalRequestContext ctx{};
        ctx.destOffset = address;
        ctx.tCode = AReq::kTcodeWriteBlock;
        const auto& current = SPro40D3ProtocolTestPeer::Route(*protocol_);
        ctx.sourceID = current.nodeId;
        ctx.generation = current.generation.value;
        // writePayload is a span: the backing storage must outlive the
        // HandleLocalRequest call (a temporary would dangle - the freed
        // heap then rides along as a garbage notification frame).
        std::vector<uint8_t> bytes(frame.begin(), frame.end());
        ctx.writePayload = bytes;
        ctx.dataLength = static_cast<uint32_t>(frame.size());
        (void)endpoint.HandleLocalRequest(ctx);
    }

    friend struct SPro40D3ProtocolTestPeer;

    ASFW::Testing::RecordingFireWireBus bus_;
    RouteState route_;
    std::unique_ptr<SPro40D3SimulatedDevice> sim_;
    SPro40D3SimulatedDevice* simPtr_{nullptr};
    std::unique_ptr<SPro40D3SimulatedScheduler> clock_;
    std::unique_ptr<ASFW::Testing::FakeTimerScheduler> fakeTimer_;
    std::unique_ptr<ASFW::Testing::FakeDiceWaitClock> waitClock_;
    std::unique_ptr<ASFW::Protocols::SBP2::AddressSpaceManager> space_;
    std::shared_ptr<SPro40D3Protocol> protocol_;
    std::optional<IOReturn> pending_{};
    ASFW::AudioModel::ParameterValue pendingValue_{false};
};

TEST(SPro40D3Protocol, TcatDelegationSurface) {
    // Streaming, clock and geometry stay TCAT's (the SPro24 pattern); only
    // the control surface is native. The structural surface pins that split.
    ProtocolRig rig;
    EXPECT_STREQ(rig.protocol().GetName(), "Focusrite Saffire Pro 40 (D3/TCD3070)");
    EXPECT_FALSE(rig.protocol().HasDsp());   // the Pro 40 carries no DSP
    EXPECT_TRUE(rig.protocol().HasMixer());  // 18-slot x 16-bus monitor mixer
    EXPECT_EQ(rig.protocol().AsFamilyDriver(), rig.protocol().AsFamilyDriver());
    // The plain TCAT carries no native provider; the D3 composition does.
    EXPECT_NE(rig.protocol().Controls(), nullptr);
}

TEST(SPro40D3Protocol, PrepareRefusesNotReadyWithoutRegistration) {
    ProtocolRig rig;
    // RebindNotifications has not run: the honest refusal, not a hang or a
    // fabricated empty snapshot.
    auto done = std::make_shared<std::optional<IOReturn>>();
    rig.protocol().Prepare([done](IOReturn kr) { *done = kr; });
    EXPECT_EQ(*done, kIOReturnNotReady);
    EXPECT_EQ(rig.protocol().CopySnapshot(), std::nullopt);
}

TEST(SPro40D3Protocol, RegistrationClaimsThenPreparePublishesObservedSnapshot) {
    ProtocolRig rig;
    auto bound = rig.Pump([&] { rig.protocol().RebindNotifications(rig.Capture()); });
    ASSERT_TRUE(bound.has_value());
    EXPECT_EQ(*bound, kIOReturnSuccess);

    auto prepared = rig.Pump([&] { rig.protocol().Prepare(rig.Capture()); });
    ASSERT_TRUE(prepared.has_value());
    EXPECT_EQ(*prepared, kIOReturnSuccess);

    const auto snapshot = rig.protocol().CopySnapshot();
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->family, "spro40d3");
    // The sim's control store reads back all-zero words: 127-max levels
    // (wire-inverted), nothing muted, everything genuinely OBSERVED.
    EXPECT_EQ(snapshot->state.parameters.size(), 55u);
    const auto firstLevel = snapshot->state.parameters.at(
        AM::ParameterId{D3Control::OutputVolumeBase});
    EXPECT_DOUBLE_EQ(std::get<double>(firstLevel.value), 127.0);
    // The pass criterion's document path: the published snapshot serializes
    // through the same generic schema the ControlsHandler serves.
    const auto doc = ASFW::UserClient::Wire::BuildControlSnapshotDocument(
        0xD1CE000000000002ULL, snapshot->family, snapshot->topology,
        snapshot->state, snapshot->sequence);
    EXPECT_NE(doc.find("spro40d3"), std::string::npos);
    // The mixer cells ride along as null values, never fabricated.
    EXPECT_NE(doc.find("\"value\":null"), std::string::npos);
}

TEST(SPro40D3Protocol, RebindDuringInFlightWriteFailsTheWriteLoudly) {
    // The recorded corner ("the registration state machine can stick after
    // bus regenerations"): RebindNotifications destroys the engine, and an
    // in-flight write's completion lived in that engine's op. The caller
    // would wait forever - a stuck write with a healthy device. The
    // completion must fire with an error status instead, and the rebind
    // itself must complete and register.
    ProtocolRig rig;
    auto bound = rig.Pump([&] { rig.protocol().RebindNotifications(rig.Capture()); });
    ASSERT_TRUE(bound.has_value());
    auto prepared = rig.Pump([&] { rig.protocol().Prepare(rig.Capture()); });
    ASSERT_TRUE(prepared.has_value());

    // A write starts and is NOT pumped: the merge-get sits mid-flight in
    // the engine when the rebind lands (the regeneration reset policy).
    std::optional<IOReturn> writeResult;
    rig.protocol().SetParameter(
        AM::ParameterId{D3Control::GlobalMute}, AM::ParameterValue{true},
        [&](IOReturn kr, const ASFW::AudioModel::ParameterValue&) {
            writeResult = kr;
        });
    EXPECT_FALSE(writeResult.has_value()); // accepted, in flight

    // The rebind lands mid-flight.
    auto rebind = rig.Pump([&] { rig.protocol().RebindNotifications(rig.Capture()); });
    ASSERT_TRUE(rebind.has_value());
    EXPECT_EQ(*rebind, kIOReturnSuccess); // the fresh engine registers

    // Give any late completion a chance to fire, then demand it fired:
    // the write must end LOUDLY (a reset-class failure), never hang.
    for (int step = 0; step < 16 && !writeResult; ++step) {
        rig.clock().AdvanceNs(1'000'000);
    }
    ASSERT_TRUE(writeResult.has_value())
        << "the rebind dropped the in-flight write's completion";
    EXPECT_NE(*writeResult, kIOReturnSuccess);

    // The stuck-state check: the NEW engine serves a fresh cycle cleanly
    // (prepare -> write -> success). This is the 'does not stick' proof.
    auto rePrepared = rig.Pump([&] { rig.protocol().Prepare(rig.Capture()); });
    ASSERT_TRUE(rePrepared.has_value());
    EXPECT_EQ(*rePrepared, kIOReturnSuccess);
    auto rewritten = rig.Pump([&] {
        rig.protocol().SetParameter(
            AM::ParameterId{D3Control::GlobalMute},
            AM::ParameterValue{false}, rig.CaptureValue());
    });
    ASSERT_TRUE(rewritten.has_value());
    EXPECT_EQ(*rewritten, kIOReturnSuccess); // the cycle completes: not stuck
}

TEST(SPro40D3Protocol, MeterRefreshAcquiresBothSourcesAndPublishes) {
    ProtocolRig rig;
    auto bound = rig.Pump([&] { rig.protocol().RebindNotifications(rig.Capture()); });
    ASSERT_TRUE(bound.has_value());
    auto prepared = rig.Pump([&] { rig.protocol().Prepare(rig.Capture()); });
    ASSERT_TRUE(prepared.has_value());

    // No batch before any acquisition: the honest empty state.
    EXPECT_FALSE(rig.protocol().CopyMeterTelemetry().has_value());

    rig.protocol().RequestMeterRefresh(48000);
    for (int step = 0; step < 16; ++step) {
        rig.clock().AdvanceNs(1'000'000);
    }
    const auto telemetry = rig.protocol().CopyMeterTelemetry();
    ASSERT_TRUE(telemetry.has_value());
    const auto& batch = *telemetry;
    EXPECT_EQ(batch.sequence, 1u);
    EXPECT_EQ(batch.generation, SPro40D3ProtocolTestPeer::Route(rig.protocol()).generation.value);
    EXPECT_GT(batch.timestampNs, 0u);
    // The sim's documented synthetic meters (encoding unknown on the real
    // device): value = 0x1000 + pos*0x10 + i. Low-rate positions: the mixer
    // group at 0x28 (18 values), the master pair at 0x3a.
    constexpr int32_t mixerLow =
        0x1000 + kMixerMeterPosLowRate * 0x10;
    constexpr int32_t masterLow =
        0x1000 + kMasterMeterPosLowRate * 0x10;
    ASSERT_EQ(batch.count, 20u);
    EXPECT_EQ(batch.raw[0], mixerLow);
    EXPECT_EQ(batch.raw[17], mixerLow + 17);
    EXPECT_EQ(batch.raw[18], masterLow);
    EXPECT_EQ(batch.raw[19], masterLow + 1);
    for (size_t i = 0; i < batch.count; ++i) {
        EXPECT_TRUE(batch.valid[i]);
    }
}

TEST(SPro40D3Protocol, MeterRefreshCoalescesInsideTheCadenceWindow) {
    ProtocolRig rig;
    auto bound = rig.Pump([&] { rig.protocol().RebindNotifications(rig.Capture()); });
    ASSERT_TRUE(bound.has_value());
    auto prepared = rig.Pump([&] { rig.protocol().Prepare(rig.Capture()); });
    ASSERT_TRUE(prepared.has_value());

    rig.protocol().RequestMeterRefresh(48000);
    for (int step = 0; step < 16; ++step) {
        rig.clock().AdvanceNs(1'000'000);
    }
    ASSERT_TRUE(rig.protocol().CopyMeterTelemetry().has_value());

    // A second request inside the 100 ms window: coalesced away - no new
    // acquisition, the sequence does not advance (one batch, one truth).
    rig.protocol().RequestMeterRefresh(48000);
    for (int step = 0; step < 16; ++step) {
        rig.clock().AdvanceNs(1'000'000);
    }
    const auto secondTelemetry = rig.protocol().CopyMeterTelemetry();
    ASSERT_TRUE(secondTelemetry.has_value());
    EXPECT_EQ(secondTelemetry->sequence, 1u);
}

TEST(SPro40D3Protocol, MeterRefreshYieldsToForegroundControlWork) {
    ProtocolRig rig;
    auto bound = rig.Pump([&] { rig.protocol().RebindNotifications(rig.Capture()); });
    ASSERT_TRUE(bound.has_value());
    auto prepared = rig.Pump([&] { rig.protocol().Prepare(rig.Capture()); });
    ASSERT_TRUE(prepared.has_value());

    // A foreground write in flight: the refresh is refused, not queued -
    // control work wins, and no meter batch publishes meanwhile.
    std::optional<IOReturn> writeResult;
    rig.protocol().SetParameter(
        AM::ParameterId{D3Control::GlobalMute}, AM::ParameterValue{true},
        [&](IOReturn kr, const ASFW::AudioModel::ParameterValue&) {
            writeResult = kr;
        });
    rig.protocol().RequestMeterRefresh(48000);
    for (int step = 0; step < 32 && !writeResult; ++step) {
        rig.clock().AdvanceNs(1'000'000);
    }
    ASSERT_TRUE(writeResult.has_value());
    EXPECT_FALSE(rig.protocol().CopyMeterTelemetry().has_value());

    // With the foreground work done, the next request acquires.
    rig.protocol().RequestMeterRefresh(48000);
    for (int step = 0; step < 16; ++step) {
        rig.clock().AdvanceNs(1'000'000);
    }
    EXPECT_TRUE(rig.protocol().CopyMeterTelemetry().has_value());
}

TEST(SPro40D3Protocol, MeterRefreshUsesTheHighRateOffsetsAbove48k) {
    ProtocolRig rig;
    auto bound = rig.Pump([&] { rig.protocol().RebindNotifications(rig.Capture()); });
    ASSERT_TRUE(bound.has_value());
    auto prepared = rig.Pump([&] { rig.protocol().Prepare(rig.Capture()); });
    ASSERT_TRUE(prepared.has_value());

    rig.protocol().RequestMeterRefresh(96000);
    for (int step = 0; step < 16; ++step) {
        rig.clock().AdvanceNs(1'000'000);
    }
    const auto telemetry = rig.protocol().CopyMeterTelemetry();
    ASSERT_TRUE(telemetry.has_value());
    const auto& batch = *telemetry;
    // The high-rate positions (0x20 / 0x32) differ from the low-rate ones,
    // and the sim's synthetic values make the distinction directly visible.
    constexpr int32_t mixerHigh =
        0x1000 + kMixerMeterPosHighRate * 0x10;
    constexpr int32_t masterHigh =
        0x1000 + kMasterMeterPosHighRate * 0x10;
    EXPECT_EQ(batch.raw[0], mixerHigh);
    EXPECT_EQ(batch.raw[18], masterHigh);
}

TEST(SPro40D3Protocol, SecondPrepareJoinsWithoutNewIo) {
    ProtocolRig rig;
    auto bound = rig.Pump([&] { rig.protocol().RebindNotifications(rig.Capture()); });
    ASSERT_TRUE(bound);
    auto prepared = rig.Pump([&] { rig.protocol().Prepare(rig.Capture()); });
    ASSERT_TRUE(prepared);
    ASSERT_EQ(*prepared, kIOReturnSuccess);

    const auto before = rig.sim().RequestCount();
    auto joined = std::make_shared<std::optional<IOReturn>>();
    rig.protocol().Prepare([joined](IOReturn kr) { *joined = kr; });
    EXPECT_EQ(*joined, kIOReturnSuccess); // immediate: no pump needed
    EXPECT_EQ(rig.sim().RequestCount(), before);
}

TEST(SPro40D3Protocol, GetOnlySurfaceRefusesWrites) {
    // The P05/P08 boundary: the writable surface is live, but the get-only
    // monitor knob refuses writes, the readback-absent mixer cells refuse
    // ORDINARY writes until the init action establishes them (P08), and
    // the router refuses until P06.
    ProtocolRig rig;
    auto bound = rig.Pump([&] { rig.protocol().RebindNotifications(rig.Capture()); });
    ASSERT_TRUE(bound);
    auto prepared = rig.Pump([&] { rig.protocol().Prepare(rig.Capture()); });
    ASSERT_TRUE(prepared);

    auto status = std::make_optional(false);
    rig.protocol().SetParameter(AM::ParameterId{D3Control::MonitorVolume}, double(10),
                                [&](IOReturn kr, const AM::ParameterValue&) {
                                    *status = (kr == kIOReturnUnsupported);
                                });
    EXPECT_TRUE(*status);
    // The Unknown cell: the shared validator refuses a stateless write
    // (MissingParameter) - and no traffic reaches the device.
    const auto receiptsBefore = rig.sim().setVolumeReceipts;
    auto mixed = std::make_optional(false);
    rig.protocol().SetParameter(AM::ParameterId{D3Control::MixerCellBase}, double(0.5),
                               [&](IOReturn kr, const AM::ParameterValue&) {
                                   *mixed = (kr == kIOReturnBadArgument);
                               });
    EXPECT_TRUE(*mixed);
    EXPECT_EQ(rig.sim().setVolumeReceipts, receiptsBefore);
    auto routed = std::make_optional(false);
    rig.protocol().SetRoute(AM::NodeId{30}, AM::PortId{1}, AM::PortId{1},
                            [&](IOReturn kr, const AM::ParameterValue&) {
                                *routed = (kr == kIOReturnUnsupported);
                            });
    EXPECT_TRUE(*routed);
}

// ---------------------------------------------------------------------------
// The P05 write path
// ---------------------------------------------------------------------------

TEST(SPro40D3Protocol, WriteRefusesWhenNotReady) {
    ProtocolRig rig;
    auto bound = rig.Pump([&] { rig.protocol().RebindNotifications(rig.Capture()); });
    ASSERT_TRUE(bound);
    auto status = std::make_optional(false);
    rig.protocol().SetParameter(AM::ParameterId{D3Control::GlobalMute}, true,
                                [&](IOReturn kr, const AM::ParameterValue&) {
                                    *status = (kr == kIOReturnNotReady);
                                });
    EXPECT_TRUE(*status);
}

TEST(SPro40D3Protocol, WriteRejectsInvalidValues) {
    ProtocolRig rig;
    auto bound = rig.Pump([&] { rig.protocol().RebindNotifications(rig.Capture()); });
    ASSERT_TRUE(bound);
    auto prepared = rig.Pump([&] { rig.protocol().Prepare(rig.Capture()); });
    ASSERT_TRUE(prepared);

    auto nan = std::make_optional(false);
    rig.protocol().SetParameter(AM::ParameterId{D3Control::OutputVolumeBase}, double(NAN),
                                [&](IOReturn kr, const AM::ParameterValue&) {
                                    *nan = (kr == kIOReturnBadArgument);
                                });
    EXPECT_TRUE(*nan);
    auto over = std::make_optional(false);
    rig.protocol().SetParameter(AM::ParameterId{D3Control::OutputVolumeBase}, double(200),
                                [&](IOReturn kr, const AM::ParameterValue&) {
                                    *over = (kr == kIOReturnBadArgument);
                                });
    EXPECT_TRUE(*over);
    auto wrongType = std::make_optional(false);
    rig.protocol().SetParameter(AM::ParameterId{D3Control::GlobalMute}, int64_t(1),
                                [&](IOReturn kr, const AM::ParameterValue&) {
                                    *wrongType = (kr == kIOReturnBadArgument);
                                });
    EXPECT_TRUE(*wrongType);
}

TEST(SPro40D3Protocol, SuccessfulWritePublishesObservedValueAndBumpsSequence) {
    ProtocolRig rig;
    auto bound = rig.Pump([&] { rig.protocol().RebindNotifications(rig.Capture()); });
    ASSERT_TRUE(bound);
    auto prepared = rig.Pump([&] { rig.protocol().Prepare(rig.Capture()); });
    ASSERT_TRUE(prepared);
    const auto before = rig.protocol().CopySnapshot();
    ASSERT_TRUE(before.has_value());

    // Level 50 on output 1: wire byte 77 in volumes word 0 (pos 0x14).
    auto w = rig.Pump([&] {
        rig.protocol().SetParameter(AM::ParameterId{D3Control::OutputVolumeBase}, double(50),
                                    rig.CaptureValue());
    });
    ASSERT_TRUE(w);
    EXPECT_EQ(*w, kIOReturnSuccess);
    // The completion reports the OBSERVED value (== the intent here, but
    // always the readback, never a copy of what we asked for).
    ASSERT_TRUE(std::holds_alternative<double>(rig.CapturedValue()));
    EXPECT_EQ(std::get<double>(rig.CapturedValue()), 50);

    const auto after = rig.protocol().CopySnapshot();
    ASSERT_TRUE(after.has_value());
    const auto ch0 = after->state.parameters.find(AM::ParameterId{D3Control::OutputVolumeBase});
    ASSERT_NE(ch0, after->state.parameters.end());
    ASSERT_TRUE(std::holds_alternative<double>(ch0->second.value));
    EXPECT_EQ(std::get<double>(ch0->second.value), 50);
    // The pair-packed neighbour survives untouched (level 127 = wire 0).
    const auto ch1 = after->state.parameters.find(AM::ParameterId{D3Control::OutputVolumeBase + 1});
    ASSERT_NE(ch1, after->state.parameters.end());
    ASSERT_TRUE(std::holds_alternative<double>(ch1->second.value));
    EXPECT_EQ(std::get<double>(ch1->second.value), 127);
    // Value changes publish through the notification sequence.
    EXPECT_GT(after->sequence, before->sequence);
    // A value write never rewrites topology identity.
    EXPECT_EQ(after->topology.revision, before->topology.revision);
}

TEST(SPro40D3Protocol, AmbiguousWriteStillPublishesTheAppliedReadback) {
    // The device APPLIES the set_control, then its write ACK fails
    // (delivered-then-lost, P01/P02 semantics). The write chain still
    // get-backs: the caller sees the failure status AND the observed
    // truth - and the snapshot carries the applied value, never the
    // last-known-before-write.
    ProtocolRig rig;
    auto bound = rig.Pump([&] { rig.protocol().RebindNotifications(rig.Capture()); });
    ASSERT_TRUE(bound);
    auto prepared = rig.Pump([&] { rig.protocol().Prepare(rig.Capture()); });
    ASSERT_TRUE(prepared);

    // Command-write ordinals are ABSOLUTE (Prepare consumed 16). The
    // fresh read is base+1, the set base+2, the notice base+3. Fail the
    // set's ACK AFTER the device applies it.
    const auto base = rig.sim().RequestCount();
    rig.sim().SetFailWriteCompletionAtRequestIndex(base + 2);
    auto w = rig.Pump([&] {
        rig.protocol().SetParameter(AM::ParameterId{D3Control::GlobalDim}, true,
                                    rig.CaptureValue());
    });
    ASSERT_TRUE(w);
    EXPECT_NE(*w, kIOReturnSuccess); // the ACK failure is reported
    // ...and the readback proves the write applied.
    const auto snap = rig.protocol().CopySnapshot();
    ASSERT_TRUE(snap.has_value());
    const auto dim = snap->state.parameters.find(AM::ParameterId{D3Control::GlobalDim});
    ASSERT_NE(dim, snap->state.parameters.end());
    ASSERT_TRUE(std::holds_alternative<bool>(dim->second.value));
    EXPECT_TRUE(std::get<bool>(dim->second.value));
}

TEST(SPro40D3Protocol, ReadbackMismatchPublishesObservedNotIntended) {
    // A foreign writer (the front panel) lands a different value in the
    // word between our set_control and our get-back: the published and
    // reported value must be what the DEVICE now holds, not the intended
    // image. Same-word writes stay serialized through the engine; the
    // sim store models the panel.
    ProtocolRig rig;
    auto bound = rig.Pump([&] { rig.protocol().RebindNotifications(rig.Capture()); });
    ASSERT_TRUE(bound);
    auto prepared = rig.Pump([&] { rig.protocol().Prepare(rig.Capture()); });
    ASSERT_TRUE(prepared);

    // Step the write manually: fresh read (base+1), set (base+2),
    // notice (base+3); the get-back is base+4.
    const auto base = rig.sim().RequestCount();
    rig.protocol().SetParameter(AM::ParameterId{D3Control::OutputVolumeBase}, double(50),
                                rig.CaptureValue());
    for (int step = 0; rig.sim().RequestCount() < base + 3 && step < 4096; ++step) {
        rig.clock().AdvanceNs(1'000'000);
    }
    ASSERT_EQ(rig.sim().RequestCount(), base + 3);
    // The panel turns the knob: a different level for channel 0 (wire
    // byte 0x55 -> level 127 - 0x55 = 42).
    rig.sim().SimulateForeignStoreWrite(0x14, 0x00000055u);
    // Let the get-back (write 4) run to completion.
    for (int step = 0; step < 4096; ++step) {
        rig.clock().AdvanceNs(1'000'000);
    }
    const auto snap = rig.protocol().CopySnapshot();
    ASSERT_TRUE(snap.has_value());
    const auto ch0 = snap->state.parameters.find(AM::ParameterId{D3Control::OutputVolumeBase});
    ASSERT_NE(ch0, snap->state.parameters.end());
    ASSERT_TRUE(std::holds_alternative<double>(ch0->second.value));
    EXPECT_EQ(std::get<double>(ch0->second.value), 42); // observed, not 50
}

TEST(SPro40D3Protocol, NoticeFailureStillPublishesTheConfirmedWrite) {
    // The set confirms; the chained software notice's ACK fails
    // (kPartialCompletion): the write applied, only the device-side
    // bookkeeping missed. The readback decides the published state and
    // the completion - a successful notice is not output readback, and
    // a failed notice is not a failed write.
    ProtocolRig rig;
    auto bound = rig.Pump([&] { rig.protocol().RebindNotifications(rig.Capture()); });
    ASSERT_TRUE(bound);
    auto prepared = rig.Pump([&] { rig.protocol().Prepare(rig.Capture()); });
    ASSERT_TRUE(prepared);

    // Command ordinals: fresh read base+1, set base+2, notice base+3.
    const auto base = rig.sim().RequestCount();
    rig.sim().SetFailWriteCompletionAtRequestIndex(base + 3);
    auto w = rig.Pump([&] {
        rig.protocol().SetParameter(AM::ParameterId{D3Control::GlobalMute}, true,
                                    rig.CaptureValue());
    });
    ASSERT_TRUE(w);
    EXPECT_EQ(*w, kIOReturnSuccess); // readback confirmed the write
    const auto snap = rig.protocol().CopySnapshot();
    ASSERT_TRUE(snap.has_value());
    const auto mute = snap->state.parameters.find(AM::ParameterId{D3Control::GlobalMute});
    ASSERT_NE(mute, snap->state.parameters.end());
    ASSERT_TRUE(std::holds_alternative<bool>(mute->second.value));
    EXPECT_TRUE(std::get<bool>(mute->second.value)); // observed, applied
}

TEST(SPro40D3Protocol, FailedGetBackMarksTheWordUnknownAndRefreshRecovers) {
    // The write applied or not, and the get-back notification is lost:
    // the word cannot be observed. The affected parameter publishes as
    // ABSENT (Unknown - the same convention as the mixer cells), the
    // write reports failure with no value, and the refresh action
    // restores the observed state on the SAME session.
    SPro40D3SimulatedDevice::Options faulted{};
    // Notifications: Prepare read 1-16; fresh read 17, set 18, notice 19,
    // get-back 20. Drop #20: the get-back cannot complete.
    faulted.dropNotificationAtAbsoluteIndex = 20;
    ProtocolRig rig{faulted};
    auto bound = rig.Pump([&] { rig.protocol().RebindNotifications(rig.Capture()); });
    ASSERT_TRUE(bound);
    auto prepared = rig.Pump([&] { rig.protocol().Prepare(rig.Capture()); });
    ASSERT_TRUE(prepared);

    auto w = rig.Pump([&] {
        rig.protocol().SetParameter(AM::ParameterId{D3Control::MonitorPad}, double(6),
                                    rig.CaptureValue());
    });
    ASSERT_TRUE(w);
    EXPECT_NE(*w, kIOReturnSuccess);

    const auto snap = rig.protocol().CopySnapshot();
    ASSERT_TRUE(snap.has_value());
    EXPECT_EQ(snap->state.parameters.find(AM::ParameterId{D3Control::MonitorPad}),
              snap->state.parameters.end()); // Unknown = absent
    // Neighbouring parameters on OTHER words stay observed.
    EXPECT_NE(snap->state.parameters.find(AM::ParameterId{D3Control::GlobalMute}),
              snap->state.parameters.end());

    // The refresh action re-reads the whole group and restores it.
    auto refreshed = rig.Pump([&] {
        rig.protocol().SetParameter(AM::ParameterId{D3Control::RefreshReadableControls},
                                    AM::ParameterValue{AM::ActionTrigger{}},
                                    rig.CaptureValue());
    });
    ASSERT_TRUE(refreshed);
    EXPECT_EQ(*refreshed, kIOReturnSuccess);
    const auto back = rig.protocol().CopySnapshot();
    ASSERT_TRUE(back.has_value());
    EXPECT_NE(back->state.parameters.find(AM::ParameterId{D3Control::MonitorPad}),
              back->state.parameters.end()); // observed again
}

TEST(SPro40D3Protocol, ForeignKnobChangeReachesAppThroughThePoll) {
    // The panel moves the (get-only) monitor knob behind our back. The
    // 500 ms observation poll is the only path by which the app learns.
    ProtocolRig rig;
    auto bound = rig.Pump([&] { rig.protocol().RebindNotifications(rig.Capture()); });
    ASSERT_TRUE(bound);
    auto prepared = rig.Pump([&] { rig.protocol().Prepare(rig.Capture()); });
    ASSERT_TRUE(prepared);

    rig.sim().SimulateForeignStoreWrite(0x54, 77u);
    // Step past the 500 ms poll cadence; the poll chain (16 reads) runs
    // on 0-delay hops, so 600 quanta are ample.
    for (int step = 0; step < 600; ++step) {
        rig.clock().AdvanceNs(1'000'000);
    }
    const auto snap = rig.protocol().CopySnapshot();
    ASSERT_TRUE(snap.has_value());
    const auto knob = snap->state.parameters.find(AM::ParameterId{D3Control::MonitorVolume});
    ASSERT_NE(knob, snap->state.parameters.end());
    ASSERT_TRUE(std::holds_alternative<double>(knob->second.value));
    EXPECT_EQ(std::get<double>(knob->second.value), 77);
}

// ---------------------------------------------------------------------------
// The P06 routing surfaces
// ---------------------------------------------------------------------------

namespace {
/// Common setup: bound, prepared, and (optionally) the program applied.
struct RoutingRig {
    ProtocolRig rig;
    bool ready{false};
    bool applied{false};
    explicit RoutingRig(bool doApply) {
        ready = rig.Pump([&] { rig.protocol().RebindNotifications(rig.Capture()); }).has_value();
        if (!ready) return;
        ready = rig.Pump([&] { rig.protocol().Prepare(rig.Capture()); }).has_value();
        if (ready && doApply) {
            applied = rig.Pump([&] {
                rig.protocol().SetParameter(
                    AM::ParameterId{kApplyRoutingAction},
                    AM::ParameterValue{AM::ActionTrigger{}}, rig.CaptureValue());
            }).has_value();
        }
    }
};
} // namespace

TEST(SPro40D3Protocol, RoutingApplyRefusedUntilControlsAreObserved) {
    // P00 §6 step 2: the control sweep must be Observed before any routing
    // claim. After a bare rebind (no Prepare) the apply refuses.
    ProtocolRig rig;
    auto bound = rig.Pump([&] { rig.protocol().RebindNotifications(rig.Capture()); });
    ASSERT_TRUE(bound);
    auto applied = rig.Pump([&] {
        rig.protocol().SetParameter(AM::ParameterId{kApplyRoutingAction},
                                    AM::ParameterValue{AM::ActionTrigger{}},
                                    rig.CaptureValue());
    });
    ASSERT_TRUE(applied);
    EXPECT_EQ(*applied, kIOReturnNotReady);
    EXPECT_EQ(rig.sim().RequestCount(), 0u); // zero I/O
}

TEST(SPro40D3Protocol, RoutingEditRefusedWithoutAppliedProgram) {
    // A differential edit before ANY application: the table is unknown and
    // never guessed at (plan P06). Nothing reaches the bus.
    ProtocolRig rig;
    auto bound = rig.Pump([&] { rig.protocol().RebindNotifications(rig.Capture()); });
    ASSERT_TRUE(bound);
    auto prepared = rig.Pump([&] { rig.protocol().Prepare(rig.Capture()); });
    ASSERT_TRUE(prepared);
    const auto before = rig.sim().RequestCount();
    auto refused = rig.Pump([&] {
        rig.protocol().SetParameter(AM::ParameterId{kRouterSourceParamBase},
                                    AM::ParameterValue{int64_t(5)},
                                    rig.CaptureValue());
    });
    ASSERT_TRUE(refused);
    EXPECT_EQ(*refused, kIOReturnNotReady);
    EXPECT_EQ(rig.sim().RequestCount(), before);
}

TEST(SPro40D3Protocol, ApplyEstablishesTheSafeProgramAndPublishesIt) {
    RoutingRig rr{true};
    ASSERT_TRUE(rr.ready);
    ASSERT_TRUE(rr.applied);
    const auto snap = rr.rig.protocol().CopySnapshot();
    ASSERT_TRUE(snap.has_value());
    for (uint32_t slot = 0; slot < kRouterOutSlots; ++slot) {
        const auto found = snap->state.parameters.find(
            AM::ParameterId{kRouterSourceParamBase + slot});
        ASSERT_NE(found, snap->state.parameters.end()) << "slot " << slot;
        ASSERT_TRUE(std::holds_alternative<int64_t>(found->second.value));
        EXPECT_EQ(std::get<int64_t>(found->second.value),
                  int64_t(kSafeRouterOutSrc[slot])) << "slot " << slot;
        EXPECT_EQ(found->second.origin, AM::StateOrigin::Cached);
    }
    const auto mixer0 = snap->state.parameters.find(
        AM::ParameterId{kMixerSourceParamBase});
    ASSERT_NE(mixer0, snap->state.parameters.end());
    EXPECT_EQ(std::get<int64_t>(mixer0->second.value), 0);
    const auto routers = snap->state.routers.find(AM::NodeId{0x100});
    ASSERT_NE(routers, snap->state.routers.end());
    EXPECT_EQ(routers->second.activeBundles.size(), 20u);
    EXPECT_FALSE(snap->state.parameters.contains(AM::ParameterId{kApplyRoutingAction}));
}

TEST(SPro40D3Protocol, DifferentialRoutingEditUpdatesOneSlotOnly) {
    RoutingRig rr{true};
    ASSERT_TRUE(rr.applied);
    const auto base = rr.rig.sim().RequestCount();
    auto edited = rr.rig.Pump([&] {
        rr.rig.protocol().SetParameter(AM::ParameterId{kRouterSourceParamBase},
                                       AM::ParameterValue{int64_t(5)},
                                       rr.rig.CaptureValue());
    });
    ASSERT_TRUE(edited);
    EXPECT_EQ(*edited, kIOReturnSuccess);
    EXPECT_EQ(rr.rig.sim().RequestCount(), base + 2);
    const auto snap = rr.rig.protocol().CopySnapshot();
    ASSERT_TRUE(snap.has_value());
    const auto line1 = snap->state.parameters.find(
        AM::ParameterId{kRouterSourceParamBase});
    ASSERT_NE(line1, snap->state.parameters.end());
    EXPECT_EQ(std::get<int64_t>(line1->second.value), 5);
    const auto line2 = snap->state.parameters.find(
        AM::ParameterId{kRouterSourceParamBase + 1});
    ASSERT_NE(line2, snap->state.parameters.end());
    EXPECT_EQ(std::get<int64_t>(line2->second.value),
              int64_t(kSafeRouterOutSrc[1]));
}

TEST(SPro40D3Protocol, RoutingSourceDomainsAreEnforcedBeforeAnyIo) {
    RoutingRig rr{true};
    ASSERT_TRUE(rr.applied);
    const auto base = rr.rig.sim().RequestCount();
    auto feedback = rr.rig.Pump([&] {
        rr.rig.protocol().SetParameter(AM::ParameterId{kMixerSourceParamBase},
                                       AM::ParameterValue{int64_t(39)},
                                       rr.rig.CaptureValue());
    });
    ASSERT_TRUE(feedback);
    EXPECT_EQ(*feedback, kIOReturnBadArgument);
    auto over = rr.rig.Pump([&] {
        rr.rig.protocol().SetParameter(AM::ParameterId{kRouterSourceParamBase},
                                       AM::ParameterValue{int64_t(55)},
                                       rr.rig.CaptureValue());
    });
    ASSERT_TRUE(over);
    EXPECT_EQ(*over, kIOReturnBadArgument);
    EXPECT_EQ(rr.rig.sim().RequestCount(), base);
}

TEST(SPro40D3Protocol, PartialHighTableFailureKeepsLowKnowledgeOnly) {
    RoutingRig rr{true};
    ASSERT_TRUE(rr.applied);
    const auto base = rr.rig.sim().RequestCount();
    rr.rig.sim().SetFailWriteCompletionAtRequestIndex(base + 2);
    auto edited = rr.rig.Pump([&] {
        rr.rig.protocol().SetParameter(AM::ParameterId{kRouterSourceParamBase},
                                       AM::ParameterValue{int64_t(5)},
                                       rr.rig.CaptureValue());
    });
    ASSERT_TRUE(edited);
    EXPECT_NE(*edited, kIOReturnSuccess);
    // The current family is Low: the program still publishes - including
    // the edit the low table confirmed.
    const auto snap = rr.rig.protocol().CopySnapshot();
    ASSERT_TRUE(snap.has_value());
    const auto line1 = snap->state.parameters.find(
        AM::ParameterId{kRouterSourceParamBase});
    ASSERT_NE(line1, snap->state.parameters.end());
    EXPECT_EQ(std::get<int64_t>(line1->second.value), 5);
    auto reapplied = rr.rig.Pump([&] {
        rr.rig.protocol().SetParameter(
            AM::ParameterId{kApplyRoutingAction},
            AM::ParameterValue{AM::ActionTrigger{}}, rr.rig.CaptureValue());
    });
    ASSERT_TRUE(reapplied);
    EXPECT_EQ(*reapplied, kIOReturnSuccess);
}

TEST(SPro40D3Protocol, AmbiguousLowTableFailureInvalidatesTheProgram) {
    RoutingRig rr{true};
    ASSERT_TRUE(rr.applied);
    const auto base = rr.rig.sim().RequestCount();
    rr.rig.sim().SetFailWriteCompletionAtRequestIndex(base + 1);
    auto edited = rr.rig.Pump([&] {
        rr.rig.protocol().SetParameter(AM::ParameterId{kRouterSourceParamBase},
                                       AM::ParameterValue{int64_t(5)},
                                       rr.rig.CaptureValue());
    });
    ASSERT_TRUE(edited);
    EXPECT_NE(*edited, kIOReturnSuccess);
    const auto snap = rr.rig.protocol().CopySnapshot();
    ASSERT_TRUE(snap.has_value());
    EXPECT_EQ(snap->state.parameters.find(AM::ParameterId{kRouterSourceParamBase}),
              snap->state.parameters.end());
    EXPECT_EQ(snap->state.routers.find(AM::NodeId{0x100}),
              snap->state.routers.end());
    auto reapplied = rr.rig.Pump([&] {
        rr.rig.protocol().SetParameter(
            AM::ParameterId{kApplyRoutingAction},
            AM::ParameterValue{AM::ActionTrigger{}}, rr.rig.CaptureValue());
    });
    ASSERT_TRUE(reapplied);
    EXPECT_EQ(*reapplied, kIOReturnSuccess);
    const auto back = rr.rig.protocol().CopySnapshot();
    ASSERT_TRUE(back.has_value());
    EXPECT_NE(back->state.parameters.find(AM::ParameterId{kRouterSourceParamBase}),
              back->state.parameters.end());
}

TEST(SPro40D3Protocol, ExplicitRoutingRejectionKeepsTheKnownProgram) {
    RoutingRig rr{true};
    ASSERT_TRUE(rr.applied);
    auto first = rr.rig.Pump([&] {
        rr.rig.protocol().SetParameter(AM::ParameterId{kRouterSourceParamBase},
                                       AM::ParameterValue{int64_t(5)},
                                       rr.rig.CaptureValue());
    });
    ASSERT_TRUE(first);
    EXPECT_EQ(*first, kIOReturnSuccess);
    const auto base = rr.rig.sim().RequestCount();
    rr.rig.sim().ArmRejectWriteAtRequestIndex(base + 1);
    auto rejected = rr.rig.Pump([&] {
        rr.rig.protocol().SetParameter(AM::ParameterId{kRouterSourceParamBase},
                                       AM::ParameterValue{int64_t(7)},
                                       rr.rig.CaptureValue());
    });
    ASSERT_TRUE(rejected);
    EXPECT_NE(*rejected, kIOReturnSuccess);
    const auto snap = rr.rig.protocol().CopySnapshot();
    ASSERT_TRUE(snap.has_value());
    const auto line1 = snap->state.parameters.find(
        AM::ParameterId{kRouterSourceParamBase});
    ASSERT_NE(line1, snap->state.parameters.end());
    EXPECT_EQ(std::get<int64_t>(line1->second.value), 5);
}

TEST(SPro40D3Protocol, SetRouteMapsThroughTheSharedValidation) {
    RoutingRig rr{true};
    ASSERT_TRUE(rr.applied);
    const auto snap0 = rr.rig.protocol().CopySnapshot();
    ASSERT_TRUE(snap0.has_value());
    const auto nodeIt = std::ranges::find(snap0->topology.nodes,
                                          AM::NodeId{0x100}, &AM::Node::id);
    ASSERT_NE(nodeIt, snap0->topology.nodes.end());
    const auto& routerNode = std::get<AM::RouterNode>(nodeIt->body);
    ASSERT_EQ(routerNode.outputs.size(), 40u);
    ASSERT_EQ(routerNode.inputs.size(), 54u);
    auto routed = rr.rig.Pump([&] {
        rr.rig.protocol().SetRoute(AM::NodeId{0x100}, routerNode.outputs[0],
                                   routerNode.inputs[4], rr.rig.CaptureValue());
    });
    ASSERT_TRUE(routed);
    EXPECT_EQ(*routed, kIOReturnSuccess);
    auto snap = rr.rig.protocol().CopySnapshot();
    ASSERT_TRUE(snap.has_value());
    const auto line1 = snap->state.parameters.find(
        AM::ParameterId{kRouterSourceParamBase});
    ASSERT_NE(line1, snap->state.parameters.end());
    EXPECT_EQ(std::get<int64_t>(line1->second.value), 5);
    auto feedback = rr.rig.Pump([&] {
        rr.rig.protocol().SetRoute(AM::NodeId{0x100},
                                   routerNode.outputs[kRouterOutSlots],
                                   routerNode.inputs[53], rr.rig.CaptureValue());
    });
    ASSERT_TRUE(feedback);
    EXPECT_EQ(*feedback, kIOReturnBadArgument);
    auto detached = rr.rig.Pump([&] {
        rr.rig.protocol().SetRoute(AM::NodeId{0x100}, routerNode.outputs[0],
                                   std::nullopt, rr.rig.CaptureValue());
    });
    ASSERT_TRUE(detached);
    EXPECT_EQ(*detached, kIOReturnSuccess);
    snap = rr.rig.protocol().CopySnapshot();
    ASSERT_TRUE(snap.has_value());
    const auto none = snap->state.parameters.find(
        AM::ParameterId{kRouterSourceParamBase});
    ASSERT_NE(none, snap->state.parameters.end());
    EXPECT_EQ(std::get<int64_t>(none->second.value), 0);
    const auto routers = snap->state.routers.find(AM::NodeId{0x100});
    ASSERT_NE(routers, snap->state.routers.end());
    EXPECT_EQ(routers->second.activeBundles.size(), 19u);
}

TEST(SPro40D3Protocol, TimedOutGroupReadLeavesControlsAbsentAndStreamingAlive) {
    // ONE dropped notification is enough to fail the group read honestly
    // (and the countdown knob exhausts itself with it, which is what makes
    // the SAME session recoverable without re-constructing anything).
    SPro40D3SimulatedDevice::Options faulted{};
    faulted.dropNextNotifications = 1;
    ProtocolRig rig{faulted};
    auto bound = rig.Pump([&] { rig.protocol().RebindNotifications(rig.Capture()); });
    ASSERT_TRUE(bound);

    auto prepared = rig.Pump([&] { rig.protocol().Prepare(rig.Capture()); });
    ASSERT_TRUE(prepared);
    EXPECT_EQ(*prepared, kIOReturnTimeout);
    EXPECT_EQ(rig.protocol().CopySnapshot(), std::nullopt);

    // Audio is a TCAT concern and never depended on the controls: the
    // family driver exists regardless of the failed vendor read.
    EXPECT_NE(rig.protocol().AsFamilyDriver(), nullptr);

    // The refresh action is the app-only recovery, on the SAME protocol:
    // the fault is spent, so the re-read succeeds and publishes.
    auto refreshed = rig.Pump([&] {
        rig.protocol().SetParameter(
            AM::ParameterId{D3Control::RefreshReadableControls}, AM::ActionTrigger{},
            [capture = rig.Capture()](IOReturn kr, const AM::ParameterValue&) { capture(kr); });
    });
    ASSERT_TRUE(refreshed);
    EXPECT_EQ(*refreshed, kIOReturnSuccess);
    const auto snapshot = rig.protocol().CopySnapshot();
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->state.parameters.size(), 55u);
}

TEST(SPro40D3Protocol, ForeignOwnerBlocksRegistrationWithExclusiveAccess) {
    ProtocolRig rig;
    rig.sim().ForceOwner(0x0BADF00D0000ULL);
    auto bound = rig.Pump([&] { rig.protocol().RebindNotifications(rig.Capture()); });
    ASSERT_TRUE(bound);
    EXPECT_EQ(*bound, kIOReturnExclusiveAccess);
    // The host range was freed; nothing is published.
    EXPECT_EQ(SPro40D3ProtocolTestPeer::Endpoint(rig.protocol()).ReservedAddress(), 0u);
    EXPECT_EQ(rig.protocol().CopySnapshot(), std::nullopt);
}

TEST(SPro40D3Protocol, RouteChangeInvalidatesStateAndRebindRestoresIt) {
    ProtocolRig rig;
    auto bound = rig.Pump([&] { rig.protocol().RebindNotifications(rig.Capture()); });
    ASSERT_TRUE(bound);
    auto prepared = rig.Pump([&] { rig.protocol().Prepare(rig.Capture()); });
    ASSERT_TRUE(prepared);
    ASSERT_TRUE(rig.protocol().CopySnapshot().has_value());

    // A route change must invalidate everything: published snapshot, the
    // claim, the reserved range (no stale-owner hazards). Same GUID, new
    // generation and node id - a genuine re-enumeration, not an equal token.
    RouteState next;
    ASFW::Discovery::ConfigROM rom{};
    rom.bib.guid = next.route.guid;
    rom.gen = ASFW::Discovery::Generation{2};
    rom.nodeId = 0x03;
    (void)next.registry.UpsertFromROM(rom, ASFW::Discovery::LinkPolicy{});
    const auto changed = *next.registry.CurrentRoute(rom.bib.guid);
    ASSERT_TRUE(changed != next.route);
    rig.protocol().UpdateRuntimeContext(changed, nullptr);
    EXPECT_EQ(rig.protocol().CopySnapshot(), std::nullopt);
    EXPECT_EQ(SPro40D3ProtocolTestPeer::Endpoint(rig.protocol()).ReservedAddress(), 0u);

    // Re-binding on the same powered device with the retained claim: the
    // device still holds OUR previous claim naming this exact window (the
    // window encoding is route-independent). The claim CAS fails against
    // the none sentinel, casPrevious equals the intended address, and the
    // registration is ADOPTED with zero further device writes (live
    // hardware 2026-11-06: a release/reclaim cycle would strand the
    // rebind's descriptor read across the owner transition and silence
    // the mailbox). Never stolen from a DIFFERENT value - that stays a
    // self-conflict.
    auto rebind = rig.Pump([&] { rig.protocol().RebindNotifications(rig.Capture()); });
    ASSERT_TRUE(rebind);
    EXPECT_EQ(*rebind, kIOReturnSuccess);

    // The device clears its owner storage (power cycle / reset on real
    // hardware): the rebind still restores the whole chain on the new
    // route through the fresh-claim path.
    rig.sim().ForceOwner(kDcpOwnerNone);
    auto reclaimed = rig.Pump([&] { rig.protocol().RebindNotifications(rig.Capture()); });
    ASSERT_TRUE(reclaimed);
    EXPECT_EQ(*reclaimed, kIOReturnSuccess);
    auto again = rig.Pump([&] { rig.protocol().Prepare(rig.Capture()); });
    ASSERT_TRUE(again);
    EXPECT_EQ(*again, kIOReturnSuccess);
    EXPECT_TRUE(rig.protocol().CopySnapshot().has_value());
}

} // namespace


// ---------------------------------------------------------------------------
// The P08 mixer coefficient surfaces
// ---------------------------------------------------------------------------

TEST(SPro40D3Protocol, InitializeMixerEstablishesEveryCellThenWritesSucceed) {
    ProtocolRig rig;
    auto bound = rig.Pump([&] { rig.protocol().RebindNotifications(rig.Capture()); });
    ASSERT_TRUE(bound);
    auto prepared = rig.Pump([&] { rig.protocol().Prepare(rig.Capture()); });
    ASSERT_TRUE(prepared);

    // The action is the only path in; it must take exactly one set_volume
    // per cell (288, never a coalesced sweep) and mark every cell Cached.
    auto initialized = std::make_optional(false);
    rig.Pump([&] {
        rig.protocol().SetParameter(AM::ParameterId{D3Control::InitializeMixer},
                                   AM::ParameterValue{AM::ActionTrigger{}},
                                   [&](IOReturn kr, const AM::ParameterValue&) {
                                       *initialized = (kr == kIOReturnSuccess);
                                   });
    });
    ASSERT_TRUE(*initialized);
    EXPECT_EQ(rig.sim().setVolumeReceipts, 288u);

    const auto snap = rig.protocol().CopySnapshot();
    ASSERT_TRUE(snap.has_value());
    for (uint32_t bus = 0; bus < 16; ++bus) {
        for (uint32_t slot = 0; slot < 18; ++slot) {
            const auto it = snap->state.parameters.find(AM::ParameterId{
                D3Control::MixerCellBase + bus * 18 + slot});
            ASSERT_NE(it, snap->state.parameters.end());
            EXPECT_EQ(std::get<double>(it->second.value), 0.0);
        }
    }

    // The recipe reached the device, one cell at a time.
    EXPECT_EQ(rig.sim().MixerVolume(0, 0), 0);
    EXPECT_EQ(rig.sim().MixerVolume(17, 15), 0);

    // An ordinary write now succeeds, touches exactly one coefficient, and
    // publishes Cached state.
    const auto receipts = rig.sim().setVolumeReceipts;
    auto wrote = std::make_optional(false);
    rig.Pump([&] {
        rig.protocol().SetParameter(
            AM::ParameterId{D3Control::MixerCellBase + 3 * 18 + 5}, double(1234),
            [&](IOReturn kr, const AM::ParameterValue& reported) {
                *wrote = (kr == kIOReturnSuccess) &&
                         std::get<double>(reported) == 1234.0;
            });
    });
    EXPECT_TRUE(*wrote);
    EXPECT_EQ(rig.sim().setVolumeReceipts, receipts + 1);
    EXPECT_EQ(rig.sim().MixerVolume(5, 3), 1234);
    const auto after = rig.protocol().CopySnapshot();
    ASSERT_TRUE(after.has_value());
    const auto cell = after->state.parameters.find(
        AM::ParameterId{D3Control::MixerCellBase + 3 * 18 + 5});
    ASSERT_NE(cell, after->state.parameters.end());
    EXPECT_EQ(std::get<double>(cell->second.value), 1234.0);
}

TEST(SPro40D3Protocol, PartialMixerInitStopsHonestlyAndCanRerun) {
    ProtocolRig rig;
    auto bound = rig.Pump([&] { rig.protocol().RebindNotifications(rig.Capture()); });
    ASSERT_TRUE(bound);
    auto prepared = rig.Pump([&] { rig.protocol().Prepare(rig.Capture()); });
    ASSERT_TRUE(prepared);
    rig.sim().setVolumeFailAfter = 3; // cells 1-3 accepted, the 4th rejected

    auto failed = std::make_optional(false);
    rig.Pump([&] {
        rig.protocol().SetParameter(AM::ParameterId{D3Control::InitializeMixer},
                                   AM::ParameterValue{AM::ActionTrigger{}},
                                   [&](IOReturn kr, const AM::ParameterValue&) {
                                       *failed = (kr == kIOReturnDeviceError);
                                   });
    });
    ASSERT_TRUE(*failed);
    // 3 accepted, then the rejected write runs the engine's documented
    // bounded recovery: 2 ritual-retry cycles (2 initial attempts + 2
    // ritual retries) before the honest error. Nothing after the budget.
    EXPECT_EQ(rig.sim().setVolumeReceipts, 3u + 2u + 2u);

    const auto snap = rig.protocol().CopySnapshot();
    ASSERT_TRUE(snap.has_value());
    size_t known = 0;
    for (const auto& [id, st] : snap->state.parameters) {
        if (id.value >= D3Control::MixerCellBase &&
            id.value < D3Control::MixerCellBase + D3Control::kMixerCellCount) {
            ++known;
        }
    }
    EXPECT_EQ(known, 3u); // exactly the established prefix; the rest Unknown

    // The recovery: a re-run with the fault cleared completes the recipe.
    rig.sim().setVolumeFailAfter = SIZE_MAX;
    auto rerun = std::make_optional(false);
    rig.Pump([&] {
        rig.protocol().SetParameter(AM::ParameterId{D3Control::InitializeMixer},
                                   AM::ParameterValue{AM::ActionTrigger{}},
                                   [&](IOReturn kr, const AM::ParameterValue&) {
                                       *rerun = (kr == kIOReturnSuccess);
                                   });
    });
    EXPECT_TRUE(*rerun);
    const auto complete = rig.protocol().CopySnapshot();
    ASSERT_TRUE(complete.has_value());
    size_t allKnown = 0;
    for (const auto& [id, st] : complete->state.parameters) {
        if (id.value >= D3Control::MixerCellBase &&
            id.value < D3Control::MixerCellBase + D3Control::kMixerCellCount) {
            ++allKnown;
        }
    }
    EXPECT_EQ(allKnown, 288u);
}

TEST(SPro40D3Protocol, MixerCellWritesValidateBoundsAndType) {
    ProtocolRig rig;
    auto bound = rig.Pump([&] { rig.protocol().RebindNotifications(rig.Capture()); });
    ASSERT_TRUE(bound);
    auto prepared = rig.Pump([&] { rig.protocol().Prepare(rig.Capture()); });
    ASSERT_TRUE(prepared);
    rig.Pump([&] {
        rig.protocol().SetParameter(AM::ParameterId{D3Control::InitializeMixer},
                                   AM::ParameterValue{AM::ActionTrigger{}},
                                   [](IOReturn, const AM::ParameterValue&) {});
    });
    const auto id = AM::ParameterId{D3Control::MixerCellBase + 7 * 18 + 2};

    auto top = std::make_optional(false);
    rig.Pump([&] {
        rig.protocol().SetParameter(id, double(32767),
                                    [&](IOReturn kr, const AM::ParameterValue&) {
                                        *top = (kr == kIOReturnSuccess);
                                    });
    });
    EXPECT_TRUE(*top);
    auto over = std::make_optional(false);
    rig.protocol().SetParameter(id, double(32768),
                               [&](IOReturn kr, const AM::ParameterValue&) {
                                   *over = (kr == kIOReturnBadArgument);
                               });
    EXPECT_TRUE(*over);
    auto negative = std::make_optional(false);
    rig.protocol().SetParameter(id, double(-1),
                               [&](IOReturn kr, const AM::ParameterValue&) {
                                   *negative = (kr == kIOReturnBadArgument);
                               });
    EXPECT_TRUE(*negative);
    auto wrongType = std::make_optional(false);
    rig.protocol().SetParameter(id, int64_t(5),
                               [&](IOReturn kr, const AM::ParameterValue&) {
                                   *wrongType = (kr == kIOReturnBadArgument);
                               });
    EXPECT_TRUE(*wrongType);
    // The init action itself rejects a non-trigger value.
    auto badAction = std::make_optional(false);
    rig.protocol().SetParameter(AM::ParameterId{D3Control::InitializeMixer},
                                double(0),
                                [&](IOReturn kr, const AM::ParameterValue&) {
                                    *badAction = (kr == kIOReturnBadArgument);
                                });
    EXPECT_TRUE(*badAction);
}
