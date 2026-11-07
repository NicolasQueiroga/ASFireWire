// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// SPro40D3AmbiguousWriteTests.cpp - The ambiguous-write policy regression,
// converted from validation/fw203-ambiguous-write.cpp (P01) and carried to
// the asynchronous seam (P02).
//
// Frozen policy (P00 baseline §5, P01): only where a write's fate is
// UNKNOWN may no prior state be trusted - invalidate the affected targets.
// Established non-submission (transport rejection at issue, refused
// deadline) and established non-application (explicit device rejection,
// the reset-ritual frame's own failure, timeout before the retry was
// submitted) leave prior state standing.
//
// Seam evolution since P01 (documented deliberately, not silently): the
// asynchronous transport now DISTINGUISHES rejection-at-issue (false
// return: the frame never left the host) from a completion error (the
// frame was submitted; its fate is unknown). The Stage-1 sync seam could
// not tell them apart and invalidated conservatively in both cases; the
// async engine invalidates only where ambiguity actually exists. The
// delivered-then-lost-ACK case gains a further precision: if the device
// demonstrably answered (a valid notification is held), the command
// proceeds through the notification path instead of dying - local write
// failure is not mailbox-command failure.
//
// The routing mirror keeps its own frozen rule: ANY terminal failure of
// the chained high-rate step invalidates the conflated mirror (a split
// table pair is unrepresentable), whatever the high write's fate.

#include "SPro40D3SimulatedDevice.hpp"
#include "SPro40D3SimulatedScheduler.hpp"

#include <gtest/gtest.h>

#include <functional>
#include <optional>

namespace {

using ASFW::Testing::DICE::SPro40D3SimulatedDevice;
using ASFW::Testing::DICE::SPro40D3SimulatedScheduler;
namespace D3 = ASFW::Audio::DICE::Focusrite::SPro40D3;

constexpr uint64_t kHostAddress = 0x0000F00D00000000ull;

/// Scripted-loss wrapper over the simulated device (P01 discipline kept:
/// the device APPLIES the frame, then its completion is reported lost).
class ScriptedLossTransport final : public D3::ISPro40D3Transport {
public:
    explicit ScriptedLossTransport(SPro40D3SimulatedDevice& inner) : inner_{inner} {}

    /// 1-based ordinal over COMMAND writes (ritual reset frames excluded):
    /// that write is delivered to the device, then its ACK is reported lost.
    uint32_t loseCommandCompletionAt{0};
    /// Count-based: response-area reads are served by the device, then the
    /// read completion is reported lost (unreadable confirmation).
    uint32_t failResponseReads{0};
    uint32_t commandOrdinal() const noexcept { return commandOrdinal_; }

    [[nodiscard]] bool WriteBlock(uint64_t address, std::span<const uint8_t> bytes,
                                  WriteDone done) override {
        const bool isCommand = !(bytes.size() == D3::kCounterResetFrameSize &&
                                 bytes[0] == 0x80 && bytes[1] == 0 && bytes[2] == 0 &&
                                 bytes[3] == 0);
        if (isCommand) ++commandOrdinal_;
        if (isCommand && loseCommandCompletionAt != 0 &&
            commandOrdinal_ == loseCommandCompletionAt) {
            return inner_.WriteBlock(
                address, bytes,
                WriteDone{[done = std::move(done)](std::expected<void, IOReturn>) mutable {
                    done(std::unexpected(kIOReturnTimeout));
                }});
        }
        return inner_.WriteBlock(address, bytes, std::move(done));
    }

    [[nodiscard]] bool ReadBlock(uint64_t address, uint32_t length, ReadDone done) override {
        if (address == SPro40D3SimulatedDevice::readAddress() && failResponseReads > 0) {
            --failResponseReads;
            return inner_.ReadBlock(
                address, length,
                ReadDone{[done = std::move(done)](
                             std::expected<std::vector<uint8_t>, IOReturn>) mutable {
                    done(std::unexpected(kIOReturnTimeout));
                }});
        }
        return inner_.ReadBlock(address, length, std::move(done));
    }

    [[nodiscard]] bool CompareSwap64(uint64_t address, uint64_t expected, uint64_t desired,
                                    CasDone done) override {
        return inner_.CompareSwap64(address, expected, desired, std::move(done));
    }

private:
    SPro40D3SimulatedDevice& inner_;
    uint32_t commandOrdinal_{0};
};

struct Capture {
    std::optional<std::expected<void, D3::MailboxError>> result;
    std::function<void(std::expected<void, D3::MailboxError>)> cb() {
        return [this](std::expected<void, D3::MailboxError> r) { result = r; };
    }
};

class AmbigRig {
public:
    using SimOptions = SPro40D3SimulatedDevice::Options;

    explicit AmbigRig(SimOptions opts = {})
        : sim_{[this](std::span<const uint8_t> frame) {
              if (engine_) engine_->OnNotification(frame);
          },
              opts} {
        auto descriptor = D3::ParseDcpDescriptor(
            std::span<const uint8_t>(ASFW::Testing::DICE::kSPro40D3DescriptorBytes));
        engine_ = std::make_shared<D3::SPro40D3MailboxEngine>(loss_, clock_, *descriptor);
    }

    D3::SPro40D3MailboxEngine& engine() noexcept { return *engine_; }
    SPro40D3SimulatedDevice& sim() noexcept { return sim_; }
    SPro40D3SimulatedScheduler& clock() noexcept { return clock_; }
    ScriptedLossTransport& loss() noexcept { return loss_; }

    /// Registers synchronously (inline completions make the registration
    /// resolve within the call). False on any rejection or error.
    [[nodiscard]] bool registerHost() {
        std::optional<std::expected<void, D3::MailboxError>> result;
        const auto accepted =
            engine_->BeginRegister(kHostAddress,
                                   [&](std::expected<void, D3::MailboxError> r) {
                                       result = r;
                                   });
        return accepted.has_value() && result.has_value() && result->has_value();
    }

    /// Drives the deadline clock to an absolute millisecond; due deadline
    /// callbacks fire (extracted under the "lock", run after unlock).
    void advanceTo(uint64_t ms) { clock_.AdvanceToMs(ms); }
    /// For notifyDuringWrite=false rigs: deliver queued notifications.
    void deliverPending() { sim_.DeliverPendingNotifications(); }

    /// Seed: a confirmed set_control (mute, value 5) - command writes #1
    /// (the set) and #2 (the chained software notice).
    void seedMute() {
        Capture seed;
        ASSERT_TRUE(engine()
                        .SendSetControl32(D3::control_offset::kMute, 5,
                                         D3::sw_notice::kMuteDim, seed.cb())
                        .has_value());
        ASSERT_TRUE(seed.result.has_value() && seed.result->has_value());
        ASSERT_EQ(engine().ControlState().mute.provenance, D3::ValueProvenance::Cached);
        ASSERT_EQ(sim().RequestCount(), 2u); // set + notice
    }

private:
    SPro40D3SimulatedDevice sim_;
    ScriptedLossTransport loss_{sim_};
    SPro40D3SimulatedScheduler clock_;
    std::shared_ptr<D3::SPro40D3MailboxEngine> engine_;
};

// ---------------------------------------------------------------------------
// Submitted-write ambiguity (P01 core, async seam)
// ---------------------------------------------------------------------------

TEST(SPro40D3AmbiguousWrite, DeliveredThenLostWithoutEvidenceInvalidates) {
    // The write is DELIVERED (the device applies it), its ACK completion is
    // reported lost, and no notification arrives: the write's fate is
    // unknown. The previously confirmed value must not remain trusted.
    AmbigRig rig;
    ASSERT_TRUE(rig.registerHost());
    rig.seedMute();
    ASSERT_EQ(rig.sim().RequestCount(), 2u);

    // Seed writes: 1 (set), 2 (notice). The test write is #3.
    rig.loss().loseCommandCompletionAt = 3;
    rig.sim().ArmDropNotifications(1); // the device's answer never arrives
    Capture lost;
    ASSERT_TRUE(rig.engine()
                    .SendSetControl32(D3::control_offset::kMute, 9,
                                      D3::sw_notice::kMuteDim, lost.cb())
                    .has_value());
    ASSERT_TRUE(lost.result.has_value());
    ASSERT_FALSE(lost.result->has_value());
    ASSERT_EQ(lost.result->error(), D3::MailboxError::kTransport);
    // Ambiguous: the last value is kept but demoted to Unknown.
    EXPECT_EQ(rig.engine().ControlState().mute.value, 5);
    EXPECT_EQ(rig.engine().ControlState().mute.provenance, D3::ValueProvenance::Unknown);
}

TEST(SPro40D3AmbiguousWrite, DeliveredThenLostWithHeldNotificationStillCompletes) {
    // P02 seam semantics: the same lost ACK, but the device demonstrably
    // answered - its valid notification is held. Local write failure is
    // not mailbox-command failure: the command proceeds through the
    // notification path and completes successfully.
    AmbigRig rig;
    ASSERT_TRUE(rig.registerHost());
    rig.seedMute();

    rig.loss().loseCommandCompletionAt = 3; // the test's set write
    Capture lost;
    ASSERT_TRUE(rig.engine()
                    .SendSetControl32(D3::control_offset::kMute, 9,
                                      D3::sw_notice::kMuteDim, lost.cb())
                    .has_value());
    ASSERT_TRUE(lost.result.has_value());
    ASSERT_TRUE(lost.result->has_value());
    EXPECT_EQ(rig.engine().ControlState().mute.value, 9);
    EXPECT_EQ(rig.engine().ControlState().mute.provenance, D3::ValueProvenance::Cached);
}

TEST(SPro40D3AmbiguousWrite, TransportRejectionAtIssueEstablishesNonSubmission) {
    // The async seam distinguishes rejection-at-issue (false return: the
    // frame never left the host) from a completion error. The Stage-1 sync
    // seam could not tell them apart and invalidated conservatively; here
    // non-submission is established, so the previously confirmed value
    // stands - and the device sees ZERO I/O for the rejected write.
    AmbigRig rig;
    ASSERT_TRUE(rig.registerHost());
    rig.seedMute();
    const uint32_t requestsBefore = rig.sim().RequestCount();

    rig.sim().ArmRejectWriteAtRequestIndex(3); // the test's set write
    Capture rejected;
    ASSERT_TRUE(rig.engine()
                    .SendSetControl32(D3::control_offset::kMute, 9,
                                      D3::sw_notice::kMuteDim, rejected.cb())
                    .has_value());
    ASSERT_TRUE(rejected.result.has_value());
    ASSERT_FALSE(rejected.result->has_value());
    EXPECT_EQ(rejected.result->error(), D3::MailboxError::kTransportNotSubmitted);
    EXPECT_EQ(rig.sim().RequestCount(), requestsBefore); // zero I/O
    EXPECT_EQ(rig.engine().ControlState().mute.value, 5);
    EXPECT_EQ(rig.engine().ControlState().mute.provenance, D3::ValueProvenance::Cached);
}

TEST(SPro40D3AmbiguousWrite, RitualRetryLostCompletionInvalidates) {
    // The device answers the first attempt with a nonzero status (explicit
    // rejection), the ritual reset frame is delivered, and the RETRIED
    // command is submitted - then its ACK is lost and its notification
    // never arrives. The retry's fate is unknown: invalidate.
    AmbigRig rig;
    ASSERT_TRUE(rig.registerHost());
    rig.seedMute();
    rig.sim().ArmFailResponses(1); // first response nonzero -> ritual

    // Command write ordinals: seed set=1, seed notice=2, test set=3,
    // retry=4. The retry's ACK is lost. Absolute notifications: seed set=1,
    // seed notice=2, test set's nonzero answer=3 (arrives: triggers the
    // ritual), the RITUAL's OWN ack=4 (live-verified 2026-11-06: the ritual
    // is a full exchange - arrives and is consumed), the retry's answer=5
    // never comes.
    rig.loss().loseCommandCompletionAt = 4;
    rig.sim().SetDropNotificationAtAbsoluteIndex(5);
    Capture retried;
    ASSERT_TRUE(rig.engine()
                    .SendSetControl32(D3::control_offset::kMute, 9,
                                      D3::sw_notice::kMuteDim, retried.cb())
                    .has_value());
    ASSERT_TRUE(retried.result.has_value());
    ASSERT_FALSE(retried.result->has_value());
    EXPECT_EQ(retried.result->error(), D3::MailboxError::kTransport);
    EXPECT_TRUE(rig.sim().SawResetFrame());
    EXPECT_EQ(rig.engine().ControlState().mute.value, 5);
    EXPECT_EQ(rig.engine().ControlState().mute.provenance, D3::ValueProvenance::Unknown);
}

TEST(SPro40D3AmbiguousWrite, FailedPairedReadInvalidatesWriteNotPriorReads) {
    // The notification says the device answered, but the paired read of the
    // confirmation is reported lost: for a write, whether it applied is
    // unknown - invalidate the write's target, never the unrelated
    // previously OBSERVED value.
    AmbigRig rig;
    ASSERT_TRUE(rig.registerHost());
    rig.seedMute(); // mute: Cached via set_control (write #1)

    // Seed an independent OBSERVED value first.
    struct Observed {
        std::optional<std::expected<uint32_t, D3::MailboxError>> result;
    } observed;
    ASSERT_TRUE(rig.engine()
                    .SendGetControl32(D3::control_offset::kDim,
                                      [&observed](std::expected<uint32_t, D3::MailboxError> r) {
                                          observed.result = r;
                                      })
                    .has_value());
    ASSERT_TRUE(observed.result.has_value() && observed.result->has_value());
    ASSERT_EQ(rig.engine().ControlState().dim.provenance, D3::ValueProvenance::Observed);

    rig.loss().failResponseReads = 1;
    Capture lost;
    ASSERT_TRUE(rig.engine()
                    .SendSetControl32(D3::control_offset::kMute, 9,
                                      D3::sw_notice::kMuteDim, lost.cb())
                    .has_value());
    ASSERT_TRUE(lost.result.has_value());
    ASSERT_FALSE(lost.result->has_value());
    EXPECT_EQ(lost.result->error(), D3::MailboxError::kTransport);
    EXPECT_EQ(rig.engine().ControlState().mute.provenance, D3::ValueProvenance::Unknown);
    EXPECT_EQ(rig.engine().ControlState().dim.provenance, D3::ValueProvenance::Observed);
}

// ---------------------------------------------------------------------------
// The conflated routing mirror (P00 baseline §6 step 4)
// ---------------------------------------------------------------------------

TEST(SPro40D3AmbiguousWrite, RoutingHighTableTransportFailureInvalidatesCachedMirror) {
    // A terminal failure of the chained high-rate step ALWAYS invalidates
    // the conflated routing mirror: the device then holds a split table
    // pair (low applied, high not) the single mirror cannot represent -
    // here scripted as a delivered-then-lost high write with no evidence.
    AmbigRig rig;
    ASSERT_TRUE(rig.registerHost());

    D3::RouterOutSrc out{};
    D3::RouterMixerSrc mix{};
    D3::RouterMeterSrc meter{};
    // Seed: a fully confirmed routing (writes 1 and 2).
    Capture seed;
    ASSERT_TRUE(rig.engine().SendSetRouting(out, mix, meter, seed.cb()).has_value());
    ASSERT_TRUE(seed.result.has_value() && seed.result->has_value());
    ASSERT_EQ(rig.engine().ControlState().routerOutSrc[0].provenance,
              D3::ValueProvenance::Cached);

    // Test routing: low confirms (write 3, notification 3), the high write
    // (write 4) is delivered but its ACK is lost and its notification
    // (absolute ordinal 4) never arrives.
    rig.loss().loseCommandCompletionAt = 4;
    rig.sim().SetDropNotificationAtAbsoluteIndex(4);
    Capture failed;
    ASSERT_TRUE(rig.engine().SendSetRouting(out, mix, meter, failed.cb()).has_value());
    ASSERT_TRUE(failed.result.has_value());
    ASSERT_FALSE(failed.result->has_value());
    EXPECT_EQ(failed.result->error(), D3::MailboxError::kPartialCompletion);
    for (const auto& slot : rig.engine().ControlState().routerOutSrc)
        EXPECT_EQ(slot.provenance, D3::ValueProvenance::Unknown);
}

// ---------------------------------------------------------------------------
// Chained-step deadlines (P00 baseline §6)
// ---------------------------------------------------------------------------

TEST(SPro40D3AmbiguousWrite, ChainedStepTimeoutIsPartialCompletion) {
    // The primary set_control confirms (value Cached); the chained
    // software notice's notification is lost in the mail and the notice
    // times out. The completion is kPartialCompletion; the confirmed value
    // STANDS (a notice carries no mirrored state).
    AmbigRig rig;
    ASSERT_TRUE(rig.registerHost());
    // Command write ordinals: set=1, notice=2. Notifications: set=1,
    // notice=2. Drop the NOTICE's notification by absolute ordinal.
    rig.sim().SetDropNotificationAtAbsoluteIndex(2);

    Capture partial;
    ASSERT_TRUE(rig.engine()
                    .SendSetControl32(D3::control_offset::kMute, 7,
                                      D3::sw_notice::kMuteDim, partial.cb())
                    .has_value());
    rig.advanceTo(200); // the notice deadline fires
    ASSERT_TRUE(partial.result.has_value());
    ASSERT_FALSE(partial.result->has_value());
    EXPECT_EQ(partial.result->error(), D3::MailboxError::kPartialCompletion);
    EXPECT_EQ(rig.engine().ControlState().mute.value, 7);
    EXPECT_EQ(rig.engine().ControlState().mute.provenance, D3::ValueProvenance::Cached);
}

TEST(SPro40D3AmbiguousWrite, ExplicitRejectionKeepsPreviouslyConfirmedState) {
    // The device reports a nonzero status for the original command AND for
    // every ritual retry: a confirmed, explicit rejection. Non-application
    // is established once the bounded ritual budget (three attempts, live
    // hardware 2026-11-06) is spent, so the previously confirmed value
    // stands - an explicit rejection and a transport timeout are NOT
    // interchangeable.
    AmbigRig rig;
    ASSERT_TRUE(rig.registerHost());
    rig.seedMute();
    rig.sim().ArmFailResponses(4);

    Capture rejected;
    ASSERT_TRUE(rig.engine()
                    .SendSetControl32(D3::control_offset::kMute, 9,
                                      D3::sw_notice::kMuteDim, rejected.cb())
                    .has_value());
    ASSERT_TRUE(rejected.result.has_value());
    ASSERT_FALSE(rejected.result->has_value());
    EXPECT_EQ(rejected.result->error(), D3::MailboxError::kResponseStatus);
    EXPECT_TRUE(rig.sim().SawResetFrame());
    EXPECT_EQ(rig.engine().ControlState().mute.value, 5);
    EXPECT_EQ(rig.engine().ControlState().mute.provenance, D3::ValueProvenance::Cached);
}

// ---------------------------------------------------------------------------
// Acceptance-class rejections: zero I/O, no callback
// ---------------------------------------------------------------------------

TEST(SPro40D3AmbiguousWrite, DefinitelyNotSubmittedRejectionKeepsPriorState) {
    // Invalid input is rejected at admission: no serialization, no
    // transport call, no callback. Prior state stands untouched.
    AmbigRig rig;
    ASSERT_TRUE(rig.registerHost());
    rig.seedMute();

    Capture never;
    const auto accepted = rig.engine().SendSetControl32(
        D3::control_offset::kMute, 9, /*swNotice=*/0xDEAD, never.cb());
    ASSERT_FALSE(accepted.has_value());
    EXPECT_EQ(accepted.error(), D3::MailboxError::kInvalidArgument);
    EXPECT_FALSE(never.result.has_value()); // callback never invoked
    EXPECT_EQ(rig.sim().RequestCount(), 2u); // zero I/O beyond the seed
    EXPECT_EQ(rig.engine().ControlState().mute.value, 5);
    EXPECT_EQ(rig.engine().ControlState().mute.provenance, D3::ValueProvenance::Cached);
}

} // namespace
