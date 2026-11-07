// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// SPro40D3AsyncEngineTests.cpp - P02-specific coverage: the asynchronous
// seam's ordering space, deadline admission, and the identity/ownership
// discipline for late and extracted callbacks.
//
// The simulated scheduler models the production extraction semantics
// (DriverKitSessionScheduler.cpp HandleTimerFired): due callbacks are
// extracted under the lock and executed after it is dropped, so Cancel()
// during a running batch cannot stop an already-extracted callback. The
// engine must therefore never rely on Cancel for correctness (P00 baseline
// §4, re-review R3) - every late callback is a harmless no-op by identity
// check. No wall-clock sleeps exist anywhere in this rig: time moves only
// through the scheduler's virtual clock.

#include "SPro40D3SimulatedDevice.hpp"
#include "SPro40D3SimulatedScheduler.hpp"

#include <gtest/gtest.h>

#include <functional>
#include <optional>

namespace {

using ASFW::Testing::DICE::SPro40D3SimulatedDevice;
using ASFW::Testing::DICE::SPro40D3SimulatedScheduler;
namespace D3 = ASFW::Audio::DICE::Focusrite::SPro40D3;
namespace Scheduling = ASFW::Scheduling;

constexpr uint64_t kHostAddress = 0x0000F00D00000000ull;

struct Capture {
    std::optional<std::expected<void, D3::MailboxError>> result;
    D3::SPro40D3MailboxEngine::VoidCallback cb() {
        return [this](std::expected<void, D3::MailboxError> r) { result = r; };
    }
};

class AsyncRig {
public:
    using SimOptions = SPro40D3SimulatedDevice::Options;

    explicit AsyncRig(SimOptions simOptions = {})
        : sim_{[this](std::span<const uint8_t> frame) {
              if (engine_) engine_->OnNotification(frame);
          },
              simOptions} {
        auto descriptor = D3::ParseDcpDescriptor(
            std::span<const uint8_t>(ASFW::Testing::DICE::kSPro40D3DescriptorBytes));
        engine_ = std::make_shared<D3::SPro40D3MailboxEngine>(sim_, clock_, *descriptor);
    }

    D3::SPro40D3MailboxEngine& engine() noexcept { return *engine_; }
    SPro40D3SimulatedDevice& sim() noexcept { return sim_; }
    SPro40D3SimulatedScheduler& clock() noexcept { return clock_; }

    [[nodiscard]] bool registerHost() {
        std::optional<std::expected<void, D3::MailboxError>> result;
        const auto accepted = engine_->BeginRegister(
            kHostAddress, [&](std::expected<void, D3::MailboxError> r) { result = r; });
        return accepted.has_value() && result.has_value() && result->has_value();
    }

    void advanceTo(uint64_t ms) { clock_.AdvanceToMs(ms); }
    void drainDeferred() { sim_.DrainDeferredCompletions(); }
    void deliverPending() { sim_.DeliverPendingNotifications(); }

private:
    SPro40D3SimulatedDevice sim_;
    SPro40D3SimulatedScheduler clock_;
    std::shared_ptr<D3::SPro40D3MailboxEngine> engine_;
};

// ---------------------------------------------------------------------------
// The scheduler model itself (P00 R3: the same-batch A-cancels-B order)
// ---------------------------------------------------------------------------

TEST(SPro40D3AsyncScheduler, SameBatchCancelCannotStopExtractedCallback) {
    // Production parity: HandleTimerFired extracts all due callbacks under
    // the lock and runs them after it is dropped. A's Cancel(B) during the
    // batch finds nothing to erase - B still runs. Pinned on the MODEL that
    // the engine tests drive (the production scheduler's own batch code is
    // host-unreachable by design; cited at DriverKitSessionScheduler.cpp).
    SPro40D3SimulatedScheduler scheduler;
    Scheduling::TimerToken tokenB{};
    int ranA = 0, ranB = 0;
    const auto tokenA = scheduler.ScheduleAfter(100, [&] {
        ++ranA;
        scheduler.Cancel(tokenB); // B is already extracted: a no-op
    });
    tokenB = scheduler.ScheduleAfter(100, [&] { ++ranB; });
    EXPECT_NE(tokenA, Scheduling::kInvalidTimerToken);
    EXPECT_NE(tokenB, Scheduling::kInvalidTimerToken);
    scheduler.AdvanceNs(100);
    EXPECT_EQ(ranA, 1);
    EXPECT_EQ(ranB, 1); // Cancel could not stop the extracted B
}

TEST(SPro40D3AsyncScheduler, ScheduleAfterAlwaysDefersEvenAtZero) {
    SPro40D3SimulatedScheduler scheduler;
    uint32_t ran = 0;
    EXPECT_NE(scheduler.ScheduleAfter(0, [&] { ++ran; }), Scheduling::kInvalidTimerToken);
    EXPECT_EQ(ran, 0u); // deferred, not inline (ITimerScheduler contract)
    scheduler.AdvanceNs(0); // still not due (deadline strictly at now)
    scheduler.AdvanceNs(1);
    EXPECT_EQ(ran, 1u);
    EXPECT_EQ(scheduler.NowNs(), 1u);
}

// ---------------------------------------------------------------------------
// Deadline admission (plan §4 item 7: scheduler-admission failure)
// ---------------------------------------------------------------------------

TEST(SPro40D3AsyncEngine, DeadlineAdmissionFailureRejectsWithZeroIO) {
    // The scheduler refuses the deadline: the engine must not perform any
    // I/O (there is no transport cancellation, so a submitted write must
    // never be unbounded). Prior state stands; completion fires exactly
    // once with kDeadlineScheduleFailed.
    AsyncRig rig;
    ASSERT_TRUE(rig.registerHost());
    Capture seed;
    ASSERT_TRUE(rig.engine()
                    .SendSetControl32(D3::control_offset::kMute, 5,
                                      D3::sw_notice::kMuteDim, seed.cb())
                    .has_value());
    ASSERT_TRUE(seed.result.has_value() && seed.result->has_value());

    rig.clock().ArmRejectNextSchedules(1);
    Capture failed;
    ASSERT_TRUE(rig.engine()
                    .SendSetControl32(D3::control_offset::kMute, 9,
                                      D3::sw_notice::kMuteDim, failed.cb())
                    .has_value());
    ASSERT_TRUE(failed.result.has_value());
    ASSERT_FALSE(failed.result->has_value());
    EXPECT_EQ(failed.result->error(), D3::MailboxError::kDeadlineScheduleFailed);
    EXPECT_EQ(rig.sim().RequestCount(), 2u); // seed set + notice only: zero I/O
    EXPECT_EQ(rig.engine().ControlState().mute.value, 5u);
    EXPECT_EQ(rig.engine().ControlState().mute.provenance, D3::ValueProvenance::Cached);
    EXPECT_FALSE(rig.engine().OperationInFlight());
}

TEST(SPro40D3AsyncEngine, ChainedStepDeadlineAdmissionFailureIsPartialCompletion) {
    // The primary set_control confirms; the NOTICE step's deadline cannot be
    // scheduled: the notice never leaves the host (zero I/O), so the
    // confirmed value stands and the completion is kPartialCompletion.
    AsyncRig::SimOptions opts;
    opts.deferNextWriteCompletions = 1; // park the SET's ACK mid-exchange
    AsyncRig rig{opts};
    ASSERT_TRUE(rig.registerHost());

    Capture partial;
    ASSERT_TRUE(rig.engine()
                    .SendSetControl32(D3::control_offset::kMute, 5,
                                      D3::sw_notice::kMuteDim, partial.cb())
                    .has_value());
    // The SET's own deadline scheduled fine (it is armed before its write);
    // now refuse exactly the next schedule - the NOTICE step's.
    rig.clock().ArmRejectNextSchedules(1);
    rig.drainDeferred(); // ACK -> held notification -> read -> chain -> refuse
    ASSERT_TRUE(partial.result.has_value());
    ASSERT_FALSE(partial.result->has_value());
    EXPECT_EQ(partial.result->error(), D3::MailboxError::kPartialCompletion);
    EXPECT_EQ(rig.sim().ControlStore().at(D3::control_offset::kMute), 5u);
    EXPECT_EQ(rig.engine().ControlState().mute.provenance, D3::ValueProvenance::Cached);
    EXPECT_FALSE(rig.engine().OperationInFlight());
    EXPECT_EQ(rig.clock().PendingCount(), 0u); // the SET's token was cancelled
}

// ---------------------------------------------------------------------------
// ACK / notification ordering (plan §4 item 4; P02 scope)
// ---------------------------------------------------------------------------

TEST(SPro40D3AsyncEngine, NotificationBeforeAckHeldThenCompletes) {
    // The firmware order: the notification is delivered inside the write,
    // the local ACK lands afterwards. With the ACK DEFERRED, the command is
    // in flight when Send returns (notification HELD, write pending); the
    // drained ACK consumes the held evidence and the command completes.
    AsyncRig::SimOptions opts;
    opts.deferNextWriteCompletions = 1;
    AsyncRig rig{opts};
    ASSERT_TRUE(rig.registerHost());

    Capture c;
    ASSERT_TRUE(rig.engine().SendSetVolume(0, 0, 42, c.cb()).has_value());
    // Not inline: the ACK is parked, the held notification cannot be used
    // yet (never confuse the two events).
    EXPECT_FALSE(c.result.has_value());
    EXPECT_TRUE(rig.engine().OperationInFlight());
    EXPECT_EQ(rig.sim().RequestCount(), 1u); // the device DID get the write

    rig.drainDeferred();
    ASSERT_TRUE(c.result.has_value());
    EXPECT_TRUE(c.result->has_value());
    EXPECT_EQ(rig.engine().ControlState().mixerVolumes[0][0].value, 42);
    EXPECT_FALSE(rig.engine().OperationInFlight());
    EXPECT_FALSE(rig.sim().HasDeferredCompletions());
}

TEST(SPro40D3AsyncEngine, AckBeforeNotificationOrderCompletes) {
    // The opposite order: the ACK completes inline, the notification is
    // parked (notifyDuringWrite=false); the command completes only when the
    // notification is delivered and the paired read validates.
    AsyncRig::SimOptions opts;
    opts.notifyDuringWrite = false;
    AsyncRig rig{opts};
    ASSERT_TRUE(rig.registerHost());

    Capture c;
    ASSERT_TRUE(rig.engine().SendSetVolume(0, 0, 42, c.cb()).has_value());
    EXPECT_FALSE(c.result.has_value()); // ACK done, notification still parked
    EXPECT_TRUE(rig.engine().OperationInFlight());
    rig.deliverPending();
    ASSERT_TRUE(c.result.has_value());
    EXPECT_TRUE(c.result->has_value());
    EXPECT_EQ(rig.engine().ControlState().mixerVolumes[0][0].value, 42);
}

// ---------------------------------------------------------------------------
// Late and extracted callbacks: identity checks at every transition
// ---------------------------------------------------------------------------

TEST(SPro40D3AsyncEngine, LateWriteCompletionAfterTimeoutIsNoOp) {
    // The deadline fires while the write completion is parked. The command
    // reaches its single terminal state (kTimeout, invalidated); the late
    // completion then arrives and must be a harmless no-op - no second
    // completion, no state touch, no buffer access.
    AsyncRig::SimOptions opts;
    opts.deferNextWriteCompletions = 1;
    AsyncRig rig{opts};
    ASSERT_TRUE(rig.registerHost());

    Capture c;
    ASSERT_TRUE(rig.engine().SendSetVolume(0, 0, 42, c.cb()).has_value());
    rig.advanceTo(200); // deadline: kTimeout while the ACK is still parked
    ASSERT_TRUE(c.result.has_value());
    ASSERT_FALSE(c.result->has_value());
    EXPECT_EQ(c.result->error(), D3::MailboxError::kTimeout);
    EXPECT_EQ(rig.engine().ControlState().mixerVolumes[0][0].provenance,
              D3::ValueProvenance::Unknown);
    const uint16_t staleBefore = rig.engine().StaleNotificationCount();

    rig.drainDeferred(); // the late, already-extracted ACK fires now
    EXPECT_FALSE(c.result->has_value() && c.result->has_value()); // unchanged
    EXPECT_EQ(rig.engine().ControlState().mixerVolumes[0][0].provenance,
              D3::ValueProvenance::Unknown); // no state touch
    EXPECT_EQ(rig.engine().StaleNotificationCount(), staleBefore);
    EXPECT_FALSE(rig.engine().OperationInFlight());
}

TEST(SPro40D3AsyncEngine, LatePairedReadAfterTimeoutIsNoOp) {
    // Same discipline on the response read: the deadline fires while the
    // read completion is parked; the late read result is a no-op.
    AsyncRig::SimOptions opts;
    opts.notifyDuringWrite = false;
    AsyncRig rig{opts};
    ASSERT_TRUE(rig.registerHost());
    rig.sim().ArmDeferReads(1); // armed AFTER registration (its read must run)

    Capture c;
    ASSERT_TRUE(rig.engine().SendSetVolume(0, 0, 42, c.cb()).has_value());
    rig.deliverPending(); // notification consumed -> paired read parked
    EXPECT_TRUE(rig.engine().OperationInFlight());
    rig.advanceTo(200); // deadline wins while the read is parked
    ASSERT_TRUE(c.result.has_value());
    EXPECT_EQ(c.result->error(), D3::MailboxError::kTimeout);
    rig.drainDeferred(); // the late read result fires: no-op
    EXPECT_FALSE(rig.engine().OperationInFlight());
    EXPECT_EQ(rig.engine().ControlState().mixerVolumes[0][0].provenance,
              D3::ValueProvenance::Unknown);
}

TEST(SPro40D3AsyncEngine, LateRegistrationCasAfterBusResetIsNoOp) {
    // The CAS completion is parked when a bus reset arrives; the
    // registration op reached its terminal state (kBusReset) and the late
    // CAS result must not resurrect registration state.
    AsyncRig::SimOptions opts;
    opts.deferNextCas = 1;
    AsyncRig rig{opts};
    Capture reg;
    ASSERT_TRUE(rig.engine()
                    .BeginRegister(kHostAddress, reg.cb())
                    .has_value()); // descriptor read inline, CAS parked
    rig.engine().OnBusReset();
    ASSERT_TRUE(reg.result.has_value());
    EXPECT_EQ(reg.result->error(), D3::MailboxError::kBusReset);
    EXPECT_EQ(rig.engine().GetRegistrationState(),
              D3::SPro40D3MailboxEngine::RegistrationState::kUnregistered);
    rig.drainDeferred(); // the late CAS "success" fires
    EXPECT_EQ(rig.engine().GetRegistrationState(),
              D3::SPro40D3MailboxEngine::RegistrationState::kUnregistered);
    EXPECT_EQ(rig.engine().NotificationAddress(), 0u);
}

TEST(SPro40D3AsyncEngine, RitualRetryNotFalselyTimedOutBySupersededDeadline) {
    // The ritual re-arms the deadline for the retry (new generation); the
    // superseded step's deadline is cancelled. After the retry completes,
    // no timer is left pending (no leak) and the command is NOT timed out.
    AsyncRig::SimOptions opts;
    opts.failNextResponses = 1; // first answer nonzero -> ritual, retry ok
    AsyncRig rig{opts};
    ASSERT_TRUE(rig.registerHost());

    Capture c;
    ASSERT_TRUE(rig.engine().SendSetVolume(0, 0, 42, c.cb()).has_value());
    ASSERT_TRUE(c.result.has_value());
    EXPECT_TRUE(c.result->has_value()); // the retry genuinely completed
    EXPECT_TRUE(rig.sim().SawResetFrame());
    EXPECT_EQ(rig.clock().PendingCount(), 0u); // no leaked deadline
    EXPECT_FALSE(rig.engine().OperationInFlight());
}

// ---------------------------------------------------------------------------
// Reentrancy from deferred drains (completion outside any Send* frame)
// ---------------------------------------------------------------------------

TEST(SPro40D3AsyncEngine, ReentrantLaunchFromDeferredCompletionDrains) {
    // A completion fired from DrainDeferredCompletions (not inside a Send*)
    // launches the next command: the pump admits it, runs it to completion,
    // and drains - bounded by construction, exactly once for every capture.
    AsyncRig::SimOptions opts;
    opts.deferNextWriteCompletions = 2; // both commands park their ACKs
    AsyncRig rig{opts};
    ASSERT_TRUE(rig.registerHost());

    struct ChainCapture {
        std::optional<std::expected<void, D3::MailboxError>> first{}, second{};
        D3::SPro40D3MailboxEngine* engine{};
        D3::SPro40D3MailboxEngine::VoidCallback cb() {
            return [this](std::expected<void, D3::MailboxError> r) {
                first = r;
                EXPECT_TRUE(engine
                                ->SendSetVolume(1, 0, 7,
                                                [this](std::expected<void, D3::MailboxError> s) {
                                                    second = s;
                                                })
                                .has_value());
            };
        }
    } cap;
    cap.engine = &rig.engine();

    ASSERT_TRUE(rig.engine().SendSetVolume(0, 0, 42, cap.cb()).has_value());
    rig.drainDeferred(); // first ACK fires -> completion launches second
    ASSERT_TRUE(cap.first.has_value());
    EXPECT_TRUE(cap.first->has_value());
    ASSERT_TRUE(cap.second.has_value()); // second completed in the same drain
    EXPECT_TRUE(cap.second->has_value());
    EXPECT_EQ(rig.engine().CommandCounter(), 2u);
    EXPECT_FALSE(rig.engine().OperationInFlight());
    EXPECT_EQ(rig.clock().PendingCount(), 0u); // queues drained to zero
    EXPECT_FALSE(rig.sim().HasDeferredCompletions());
}

// ---------------------------------------------------------------------------
// Submitted-write route errors (the "stale route" family on this seam)
// ---------------------------------------------------------------------------

TEST(SPro40D3AsyncEngine, SubmittedWriteCompletionErrorInvalidates) {
    // A submitted write whose completion reports an error (route lost at
    // completion time, ACK lost): the fate is unknown - invalidate.
    AsyncRig rig;
    ASSERT_TRUE(rig.registerHost());
    Capture seed;
    ASSERT_TRUE(rig.engine().SendSetVolume(3, 5, 11, seed.cb()).has_value());
    ASSERT_TRUE(seed.result.has_value() && seed.result->has_value());

    rig.sim().ArmFailWriteCompletions(1);
    rig.sim().ArmDropNotifications(1); // no held evidence either
    Capture failed;
    ASSERT_TRUE(rig.engine().SendSetVolume(3, 5, 22, failed.cb()).has_value());
    ASSERT_TRUE(failed.result.has_value());
    EXPECT_EQ(failed.result->error(), D3::MailboxError::kTransport);
    const auto& slot = rig.engine().ControlState().mixerVolumes[3][5];
    EXPECT_EQ(slot.value, 11);
    EXPECT_EQ(slot.provenance, D3::ValueProvenance::Unknown);
}

} // namespace
