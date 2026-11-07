// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// SPro40D3MailboxEngine.hpp - Natively asynchronous, transport-independent
// engine for the Saffire Pro 40 (TCD3070) DCP vendor mailbox.
//
// Design (P02; behavioral facts cited in SPro40D3MailboxTypes.hpp):
//
//   * Asynchronous everywhere. The transport seam is completion-based: every
//     ReadBlock/WriteBlock/CompareSwap64 returns ACCEPTANCE; an accepted
//     operation completes exactly once, inline within the call or deferred -
//     never both, never neither. Rejection (false return) establishes
//     non-submission: nothing was handed to the bus, no completion will ever
//     fire, and the engine leaves prior state standing. There is no blocking
//     wait, sleep, polling loop, dedicated executor or second engine; the
//     synchronous Stage-1 engine this replaces is gone (plan §4 item 2, §6
//     P02: one production engine).
//
//   * Entry-point callback semantics (plan §4 item 5; documented per entry
//     point as P02 requires):
//       - Send*/BeginRegister/BeginUnregister return ACCEPTANCE ONLY:
//         kBusy (an operation is already in flight), kNotRegistered (commands
//         only), kInvalidArgument (rejected input; zero I/O), or
//         kDeadlineScheduleFailed (the timer scheduler refused the deadline;
//         zero I/O). On any rejection the completion callback is NOT invoked.
//       - Once accepted, the completion fires EXACTLY once, inline or
//         deferred, with the final outcome of the LOGICAL operation
//         (including chained steps). Chained-step failures report
//         kPartialCompletion when the primary write was confirmed.
//       - OnNotification/OnBusReset return void and never fire user
//         callbacks re-entrantly from the caller's frame: work they
//         trigger is pumped after the current step.
//
//   * Single-threaded pump (Stage-1 review P0, preserved): every public
//     entry point and every transport/timer completion runs through one
//     re-entrancy guard. Events arriving while the guard is held (an inline
//     transport completion inside our own transport call, a notification
//     delivered inside ingress, an API call from inside a completion
//     callback) are QUEUED with their results copied, then drained
//     iteratively. Consequences, by construction: no recursive completion
//     chain; the request span handed to WriteBlock is engine-owned storage
//     that nothing moves or frees for the entire synchronous transport call
//     (state transitions happen only between pump steps); completion
//     callbacks fire only between pump steps and may call any engine API.
//     Callbacks must not destroy the engine and must not throw (the pump is
//     noexcept by design).
//
//   * Three independently modeled events per command (plan §4 item 4, §6
//     P02): local write completion (ACK), device notification, paired
//     response read. A VALID early notification that arrives before the
//     write completes is HELD and consumed once the write outcome is known.
//     Local write success is never confused with mailbox completion: the
//     command completes through notification + validated paired read. A
//     write completion error with a held valid notification still proceeds
//     through the notification path - the device demonstrably processed the
//     frame; only the local ACK was lost. A write completion error with no
//     held notification is terminal: ambiguous (P01 policy) - invalidate.
//
//   * Ambiguous-write policy (P01, frozen in the P00 baseline §5): a
//     submitted write whose fate is unknown - transport completion error
//     (initial write or counter-reset retry), paired-read failure, deadline
//     expiry, or a response that fails validation - invalidates the affected
//     targets to Unknown; old values are never republished as current.
//     NOT ambiguous, never invalidated: acceptance rejections and deadline
//     scheduling failures (zero I/O - non-submission established), transport
//     REJECTION at issue (non-submission established), the reset-ritual
//     frame's own completion error (the original attempt was explicitly
//     answered nonzero and the retry never left the host), an explicit
//     device rejection (kResponseStatus), and any read command. One
//     deliberate exception: a terminal failure of the chained HIGH-rate
//     routing step ALWAYS invalidates the conflated routing mirror - the
//     device then holds a split table pair the single mirror cannot
//     represent, regardless of whether the high write was submitted
//     (P00 baseline §6 step 4: never trust a half-applied routing).
//
//   * Deadlines (plan §4 item 7): one per logical step, on the injected
//     nonblocking ITimerScheduler (ScheduleAfter always defers; production
//     callbacks arrive on the Default queue). Deadline admission happens
//     BEFORE the transport call (there is no transport cancellation, so a
//     scheduled write must never be unbounded); a refused schedule rejects
//     the step with kDeadlineScheduleFailed and performs zero I/O. Cancellation
//     is bookkeeping, never correctness (P00 R3): the production scheduler
//     extracts all due callbacks under its lock and executes them after
//     unlock (DriverKitSessionScheduler.cpp HandleTimerFired), so a Cancel
//     cannot stop an already-extracted callback. Every timer and transport
//     callback therefore captures shared ownership of the engine plus the
//     operation epoch and re-checks identity before touching state; a late
//     or extracted callback is a harmless no-op by construction (buffers are
//     never touched after terminal - completions carry results, not
//     storage). Registration/unregistration are transport-bounded and carry
//     no engine deadline: every accepted transport operation completes
//     exactly once by seam contract.
//
//   * Exactly-once completion (Stage-1 review P1, preserved): see the
//     entry-point semantics above. kPartialCompletion marks a confirmed
//     primary with a failed chained step; kBusReset marks an operation
//     invalidated by a bus reset (all state invalidated regardless).
//
//   * Bounded response-area reads (Stage-1 review P0, preserved): the
//     response area is read ONLY paired with a shape-valid notification,
//     bounded by the descriptor's responseAreaSize, with the rlen/echo
//     validation of the Stage-1 engine unchanged.
//
//   * Notification attribution: response headers attribute by counter; the
//     notification frame itself carries no counter. Stale (recent completed)
//     counters keep the in-flight command waiting; unknown counters fail
//     with kCounterMismatch. The counter-reset ritual (reset frame, counter
//     zero, one retry) is the only automatic re-send.

#pragma once

#include "SPro40D3MailboxTypes.hpp"
#include "../../../../Scheduling/ITimerScheduler.hpp"
#include "../../../../Common/MoveOnlyCallback.hpp"

#include <DriverKit/IOReturn.h>

#include <array>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace ASFW::Audio::DICE::Focusrite::SPro40D3 {

// ============================================================================
// Transport seam (asynchronous)
// ============================================================================

/// Completion-based register-block transport. Production adapters forward to
/// the async bus ports (route-current checked at issue and completion); host
/// tests bind a deterministic simulated device.
///
/// Acceptance contract: a FALSE return rejects the operation - nothing was
/// submitted, the completion will never fire, and the caller's buffers are
/// released. Once accepted, the completion fires EXACTLY once, either inline
/// (within the call) or deferred. The engine guarantees `bytes` remains
/// valid for the entire synchronous call; transports copy what they need
/// before returning and must not touch the span afterwards (inline
/// completions included: the engine only processes them after the call
/// returns). A rejected read/CAS establishes non-submission of that
/// operation only.
class ISPro40D3Transport {
public:
    using WriteDone = ASFW::Common::MoveOnlyCallback<void(std::expected<void, IOReturn>)>;
    using ReadDone  = ASFW::Common::MoveOnlyCallback<void(std::expected<std::vector<uint8_t>, IOReturn>)>;
    using CasDone   = ASFW::Common::MoveOnlyCallback<void(std::expected<uint64_t, IOReturn>)>;

    virtual ~ISPro40D3Transport() = default;

    [[nodiscard]] virtual bool WriteBlock(uint64_t address, std::span<const uint8_t> bytes,
                                          WriteDone done) = 0;
    [[nodiscard]] virtual bool ReadBlock(uint64_t address, uint32_t length, ReadDone done) = 0;
    /// Returns the previous value at `address` in the completion; the swap
    /// happened iff that value equals `expected`.
    [[nodiscard]] virtual bool CompareSwap64(uint64_t address, uint64_t expected,
                                             uint64_t desired, CasDone done) = 0;
};

// ---------------------------------------------------------------------------
// MailboxError codes
// ---------------------------------------------------------------------------

enum class MailboxError : uint8_t {
    kTransport,           ///< transport completion failed (see LastTransportError)
    kTransportNotSubmitted, ///< the write never left the host (rejected at
                          ///  issue): non-submission is ESTABLISHED, the
                          ///  prior state stands (P06: distinct from the
                          ///  ambiguous kTransport class)
    kBadDescriptor,       ///< descriptor read/parse/revalidation failed
    kBusy,                ///< an operation is already in flight (serialized engine)
    kNotRegistered,       ///< no notification address registered
    kOwnershipConflict,   ///< CAS saw a foreign owner (see ForeignOwner)
    kTimeout,             ///< deadline expired (late notifications are never trusted)
    kResponseStatus,      ///< device status stayed nonzero after the reset ritual
    kCounterMismatch,     ///< response echoed a counter matching no recent command
    kShortResponse,       ///< transport returned fewer bytes than requested
    kMalformedResponse,   ///< response header inconsistent (opcode/length echo)
    kResponseShapeMismatch, ///< well-formed notification whose length contradicts
                           ///< the command's expected response (no read performed)
    kBusReset,            ///< in-flight operation invalidated by a bus reset
    kInvalidArgument,     ///< rejected: input out of range (never sent)
    kPartialCompletion,   ///< primary write confirmed, chained step failed
    kReleaseDisabled,     ///< ownership-release CAS disabled (unverified on hw)
    kDeadlineScheduleFailed, ///< the timer scheduler refused the deadline; zero I/O
};

/// Reads and parses the 48-byte descriptor (static zone; always safe).
/// Asynchronous: acceptance per ISPro40D3Transport; the completion carries
/// the parsed descriptor or kBadDescriptor.
[[nodiscard]] bool ReadDcpDescriptor(
    ISPro40D3Transport& transport,
    ASFW::Common::MoveOnlyCallback<void(std::expected<DcpDescriptor, MailboxError>)> done);

// ============================================================================
// Engine
// ============================================================================

class SPro40D3MailboxEngine final : public std::enable_shared_from_this<SPro40D3MailboxEngine> {
public:
    struct Options {
        /// Deadline for one logical command step (notification + response
        /// exchange); the reference passes timeout_ms per transaction.
        uint64_t responseTimeoutMs{200};
        /// Minimum spacing between mailbox command writes, measured from
        /// the previous command's write submission. Live hardware
        /// (2026-11-06): the device silently DROPS commands written inside
        /// the ~200 ms busy window that follows each answered command (no
        /// index advance, no notification) and the response area
        /// self-erases ~200 ms after the write - the paired read must land
        /// inside that window, and the next command outside it. The
        /// reference driver paces naturally through its synchronous
        /// write->notification->read cycle; an event-driven engine chaining
        /// ops in milliseconds does not. 0 disables pacing (simulators,
        /// tests).
        uint64_t commandGapMs{0};
        /// Enable the ownership-release CAS in BeginUnregister. OFF by
        /// default: the release CAS is UNVERIFIED on hardware.
        bool allowOwnershipRelease{false};
    };

    using VoidCallback    = std::function<void(std::expected<void, MailboxError>)>;
    using ControlCallback = std::function<void(std::expected<uint32_t, MailboxError>)>;
    /// Meter values point into engine storage; valid until the next command
    /// starts. Copy them if they must outlive the command.
    using MeterCallback   = std::function<void(std::expected<std::span<const int32_t>, MailboxError>)>;
    using RegisterCallback = VoidCallback;

    SPro40D3MailboxEngine(ISPro40D3Transport& transport, ASFW::Scheduling::ITimerScheduler& scheduler,
                          DcpDescriptor descriptor, Options options);
    SPro40D3MailboxEngine(ISPro40D3Transport& transport, ASFW::Scheduling::ITimerScheduler& scheduler,
                          DcpDescriptor descriptor);
    ~SPro40D3MailboxEngine();

    SPro40D3MailboxEngine(const SPro40D3MailboxEngine&) = delete;
    SPro40D3MailboxEngine& operator=(const SPro40D3MailboxEngine&) = delete;

    // ------------------------------------------------------------------
    // Ownership / notification registration (asynchronous)
    // ------------------------------------------------------------------

    enum class RegistrationState : uint8_t { kUnregistered, kRegistered };

    /// Begins registration: re-reads and revalidates the descriptor, then
    /// classifies ownership and claims only from the constant unowned
    /// sentinel (Stage-1 review round 3, finding A - unchanged policy).
    /// Acceptance: kBusy if an operation is in flight or the pump is
    /// running. Once accepted, `completion` fires exactly once with {} or
    /// kBadDescriptor / kOwnershipConflict / kTransport. Registration is
    /// transport-bounded: no engine deadline (see the design block).
    [[nodiscard]] std::expected<void, MailboxError>
    BeginRegister(uint64_t hostAddress, RegisterCallback completion);

    /// CAS our address back to kDcpOwnerNone. Disabled unless
    /// Options::allowOwnershipRelease (kReleaseDisabled at acceptance).
    [[nodiscard]] std::expected<void, MailboxError>
    BeginUnregister(RegisterCallback completion);

    /// Bus reset: registration lost, counter restarts at 0, the in-flight
    /// operation (command or registration) completes with kBusReset, and
    /// every control value falls back to Unknown. Late transport/timer
    /// events for the invalidated operation are harmless no-ops.
    void OnBusReset();

    [[nodiscard]] RegistrationState GetRegistrationState() const noexcept { return registration_; }
    [[nodiscard]] uint64_t NotificationAddress() const noexcept { return notificationAddress_; }
    /// Observed foreign owner after kOwnershipConflict; 0 when none.
    [[nodiscard]] uint64_t ForeignOwner() const noexcept { return foreignOwner_; }

    // ------------------------------------------------------------------
    // Commands (serialized; one in flight)
    // ------------------------------------------------------------------

    /// Mixer crosspoint volume (channel < 18, mix bus < 16, 0 <= volume <=
    /// 0xFFFF). On success the value is Cached (host-written).
    [[nodiscard]] std::expected<void, MailboxError>
    SendSetVolume(uint8_t channel, uint8_t mix, int32_t volume, VoidCallback completion);

    /// Master (output) meters; currentRateHz > 0 selects the meter position.
    [[nodiscard]] std::expected<void, MailboxError>
    SendGetMasterMeter(uint32_t currentRateHz, MeterCallback completion);

    /// Mixer input meters (18 channels); currentRateHz > 0.
    [[nodiscard]] std::expected<void, MailboxError>
    SendGetMixerMeter(uint32_t currentRateHz, MeterCallback completion);

    /// Reads one u32 from the control store. On success the value is
    /// Observed. Read commands never invalidate anything.
    [[nodiscard]] std::expected<void, MailboxError>
    SendGetControl32(uint32_t position, ControlCallback completion);

    /// Writes one u32, then chains the software notice (the reference pairs
    /// every set with write_sw_notice). On confirmed write + failed notice
    /// the completion is kPartialCompletion and the value stays Cached. On
    /// full success Cached.
    [[nodiscard]] std::expected<void, MailboxError>
    SendSetControl32(uint32_t position, uint32_t value, uint32_t swNotice,
                     VoidCallback completion);

    /// Writes BOTH routing tables (low, then high). High-step terminal
    /// failure of any kind reports kPartialCompletion and invalidates the
    /// conflated routing mirror (split table pairs are unrepresentable).
    /// On full success Cached.
    [[nodiscard]] std::expected<void, MailboxError>
    SendSetRouting(const RouterOutSrc& outSrc, const RouterMixerSrc& mixerSrc,
                   const RouterMeterSrc& meterSrc, VoidCallback completion);

    // ------------------------------------------------------------------
    // Device ingress
    // ------------------------------------------------------------------

    /// Delivers a notification block-write. Shape-invalid or oversized
    /// frames are counted and dropped without reading; a valid frame for a
    /// write-pending command is held until the write outcome is known; a
    /// valid frame while awaiting consumes it (rstatus/ritual/bounded paired
    /// read). With no operation in flight it is stale (counted, dropped).
    void OnNotification(std::span<const uint8_t> frame);

    // ------------------------------------------------------------------
    // State
    // ------------------------------------------------------------------

    [[nodiscard]] const SPro40D3ControlState& ControlState() const noexcept { return state_; }
    [[nodiscard]] uint16_t CommandCounter() const noexcept { return counter_; }
    void OverrideCommandCounter(uint16_t value) noexcept { counter_ = value; }
    [[nodiscard]] uint32_t StaleNotificationCount() const noexcept { return staleNotifications_; }
    [[nodiscard]] uint32_t MalformedNotificationCount() const noexcept {
        return malformedNotifications_;
    }
    [[nodiscard]] uint32_t LastDeviceStatus() const noexcept { return lastDeviceStatus_; }
    [[nodiscard]] IOReturn LastTransportError() const noexcept { return lastTransportError_; }
    [[nodiscard]] bool OperationInFlight() const noexcept { return op_.has_value(); }
    /// Command counter of the in-flight operation (0 when idle).
    [[nodiscard]] uint32_t InFlightEpoch() const noexcept { return op_ ? op_->epoch : 0; }

private:
    enum class Kind : uint8_t {
        kSetVolume, kGetMasterMeter, kGetMixerMeter,
        kGetControl, kSetControl, kSwNotice, kSetRoutingLow, kSetRoutingHigh,
        kRegister, kUnregister,
    };
    enum class Phase : uint8_t {
        kWritePending,   ///< command frame (or ritual retry) submitted, ACK pending
        kGapWait,        ///< command frame held for the inter-command gap
        kAwaitNotification,
        kRitualAnswerWait, ///< write completed; notification consumed or pending
        kReadPending,    ///< paired response read submitted
        kRegisterRead,   ///< registration: descriptor read submitted
        kRegisterCas,     ///< registration: ownership CAS submitted
        kUnregisterCas,  ///< release CAS submitted
    };
    /// Which submitted write a WriteBlock completion belongs to.
    enum class WriteStep : uint8_t { kCommand, kResetFrame };

    struct Op {
        uint32_t epoch{};
        Kind kind{};
        Phase phase{};
        WriteStep pendingWrite{WriteStep::kCommand};
        std::vector<uint8_t> request; ///< current wire frame; engine-owned
        std::array<uint8_t, 12> heldNotification{};
        bool hasHeldNotification{false};
        uint16_t counter{};
        ASFW::Scheduling::TimerToken deadlineToken{ASFW::Scheduling::kInvalidTimerToken};
        uint32_t deadlineGeneration{0};
        bool usedResetRitual{};
    /// Nonzero-status answers must still consume the response area (the
    /// reference reads rlen+16 after EVERY answer, mismatch included) or
    /// the device's mailbox stays stuck and drops the following frames
    /// (live hardware 2026-11-06: without this read the ritual and the
    /// retry are silently dropped every time; the raw probes that read
    /// after every command never see it).
    bool pendingMismatchHandshake{};
    /// The ritual's OWN answer must also be consumed (notification + 16-byte
    /// area read) before the retry is sent: spro40d3.rs runs the reset
    /// frame through the same send_message handshake as any command. Live
    /// hardware 2026-11-06: with the retry launched straight after the
    /// reset-frame ACK, the ritual's answer sits unconsumed in the mailbox
    /// and the retry is dropped every time.
    bool pendingRitualHandshake{};
    /// The mismatch/ritual handshake reads carry rlen 0; the retry that
    /// follows must still validate against the ORIGINAL command's expected
    /// response length (the live retry's accepted answer is rlen 4 - a
    /// clobbered expectation would shape-mismatch it, live 2026-11-06).
    uint32_t savedExpectedResponseLength{};
    uint8_t ritualAttempts{}; ///< live hardware: the ritual frame itself is
                             ///< subject to the device's ingress flakiness
                             ///< (P07 2026-11-06: identical 16-byte frames
                             ///< answer or drop nondeterministically); a
                             ///< mismatched retry answer means the ritual
                             ///< never landed, not that the device refused.
        bool isChainedStep{}; ///< terminal failure reports kPartialCompletion
        uint32_t expectedResponseLength{};
        VoidCallback voidCb{};
        ControlCallback controlCb{};
        MeterCallback meterCb{};
        // Typed decode context
        uint32_t controlPos{};
        uint32_t swNotice{};
        uint8_t volChannel{}, volMix{};
        int32_t volValue{};
        RouterOutSrc routerOut{};
        RouterMixerSrc routerMixer{};
        RouterMeterSrc routerMeter{};
        // Registration context
        uint64_t registrationAddress{};
    };

    // -- pump plumbing -----------------------------------------------------
    [[nodiscard]] bool Enter() noexcept;
    void Exit() noexcept;          ///< Pump() then release the guard
    void LaunchTopLevel(Op op);    ///< Enter + run + drain (pump known idle)
    void Pump() noexcept;          ///< drain queued work iteratively

    // -- event plumbing -----------------------------------------------------
    // Every asynchronous arrival (transport completion, notification,
    // deadline) becomes an Event carrying its COPIED result; events are
    // processed only between pump steps. No event ever references engine
    // storage beyond the epoch/counter identity it carries.
    struct Event {
        enum class Type : uint8_t {
            kNotification, kWriteDone, kReadDone, kCasDone, kDeadline,
        };
        Type type{};
        uint32_t epoch{};
        uint32_t generation{0}; ///< deadline generation (superseded-step filter)
        IOReturn status{kIOReturnSuccess};
        bool ok{false};              ///< write outcome
        std::vector<uint8_t> data{}; ///< notification frame or read payload
        uint64_t casPrevious{0};
    };
    void Deliver(Event event) noexcept; ///< Enter-or-queue, then process
    void HandleBusReset() noexcept;
    void HandleEvent(const Event& event);
    void HandleNotification(Op& op, const Event& event);
    void HandleWriteDone(Op& op, const Event& event);
    void HandleReadDone(Op& op, const Event& event);
    void HandleCasDone(Op& op, const Event& event);
    void HandleCommandReadDone(Op& op, const Event& event);
    void HandleDeadline(Op& op);
    void ConsumeNotification(Op& op); ///< held/arrived frame -> read or ritual
    void BeginPairedRead(Op& op);
    void RunRitual(Op& op);
    /// Submits the ritual frame (gap already satisfied); anchors the
    /// inter-command gap for the retry.
    void SubmitResetFrame(Op& op);
    void Dispatch(Op& op, std::span<const uint8_t> response);
    /// Schedules the deadline, then submits the op's current write. The op
    /// must already live in op_. A refused schedule or a transport
    /// rejection completes the step with zero I/O.
    void LaunchCurrentWrite();
    /// Replaces op_ with the next chained step (notice / high-rate table),
    /// preserving the epoch and callbacks, and launches it.
    void LaunchChainedStep(Op next);
    [[nodiscard]] ISPro40D3Transport::WriteDone MakeWriteDone(uint32_t epoch);
    [[nodiscard]] ISPro40D3Transport::ReadDone MakeReadDone(uint32_t epoch);
    [[nodiscard]] ISPro40D3Transport::CasDone MakeCasDone(uint32_t epoch);
    [[nodiscard]] ASFW::Scheduling::TimerToken ScheduleDeadline(Op& op);
    /// Hold an op until the inter-command gap elapses (Options::commandGapMs).
    /// Shares the deadline token/generation discipline: exactly one of the
    /// gap/response timers is active per op step.
    [[nodiscard]] ASFW::Scheduling::TimerToken ScheduleGap(Op& op, uint64_t delayNs);
    void CompleteFailure(MailboxError error, bool invalidateAffectedTargets);
    void CompleteRegistration(Op& op, std::expected<void, MailboxError> result);
    void FireCompletion(Op& op, MailboxError error) noexcept;
    void FireSuccessVoid(Op& op) noexcept;
    void CancelDeadline(Op& op) noexcept;
    void InvalidateWriteTargets(const Op& op) noexcept;
    void RememberCounter(uint16_t counter) noexcept;
    [[nodiscard]] bool CounterIsRecent(uint16_t counter) const noexcept;
    [[nodiscard]] static Opcode OpcodeFor(Kind kind) noexcept;
    [[nodiscard]] static uint32_t ExpectedResponseLength(Kind kind) noexcept;

    // -- provenance store ----------------------------------------------------
    void DecodeAndStoreControl(uint32_t position, uint32_t value, ValueProvenance prov) noexcept;
    void ApplyObserved(uint32_t position, uint32_t value) noexcept;
    void ApplyCached(uint32_t position, uint32_t value) noexcept;
    void InvalidateControl(uint32_t position) noexcept;

    ISPro40D3Transport& transport_;
    ASFW::Scheduling::ITimerScheduler& scheduler_;
    uint64_t lastCommandWriteNs_{0}; ///< gap anchor: last command write submission
    bool haveCommandWriteAnchor_{false}; ///< anchor valid (clocks may start at 0)
    uint64_t gapNs_{0};              ///< cached options_.commandGapMs in ns (0 = off)
    DcpDescriptor descriptor_;
    Options options_;

    RegistrationState registration_{RegistrationState::kUnregistered};
    uint64_t notificationAddress_{0};
    uint64_t foreignOwner_{0};

    uint16_t counter_{0};
    uint32_t nextEpoch_{1};
    std::optional<Op> op_{};
    SPro40D3ControlState state_{};
    std::array<int32_t, kChannelCount> meterBuffer_{};
    uint32_t staleNotifications_{0};
    uint32_t malformedNotifications_{0};
    uint32_t lastDeviceStatus_{0};
    IOReturn lastTransportError_{kIOReturnSuccess};

    // Pump state: work queued while an entry point is executing.
    bool active_{false};
    std::deque<Event> queuedEvents_{};
    std::deque<Op> queuedLaunches_{};
    bool queuedBusReset_{false};

    // Recent counters for stale-notification attribution (ring of 4).
    static constexpr size_t kRecentCounterWindow = 4;
    std::array<uint16_t, kRecentCounterWindow> recentCounters_{};
    size_t recentCounterCount_{0};
    size_t recentCounterNext_{0};
};

} // namespace ASFW::Audio::DICE::Focusrite::SPro40D3