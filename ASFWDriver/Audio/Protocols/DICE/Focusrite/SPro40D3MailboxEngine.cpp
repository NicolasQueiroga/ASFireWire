// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// SPro40D3MailboxEngine.cpp - see SPro40D3MailboxEngine.hpp for the design.

#include "SPro40D3MailboxEngine.hpp"

#include <utility>

namespace ASFW::Audio::DICE::Focusrite::SPro40D3 {

bool ReadDcpDescriptor(ISPro40D3Transport& transport,
                       ASFW::Common::MoveOnlyCallback<void(std::expected<DcpDescriptor, MailboxError>)> done) {
    using ReadResult = std::expected<std::vector<uint8_t>, IOReturn>;
    return transport.ReadBlock(
        kDcpBaseAddress, static_cast<uint32_t>(kDcpDescriptorSize),
        ASFW::Common::MoveOnlyCallback<void(ReadResult)>(
            [done = std::move(done)](ReadResult raw) mutable {
                if (!raw) {
                    done(std::unexpected(MailboxError::kBadDescriptor));
                    return;
                }
                auto parsed = ParseDcpDescriptor(std::span<const uint8_t>(*raw));
                if (!parsed) {
                    done(std::unexpected(MailboxError::kBadDescriptor));
                    return;
                }
                done(std::move(*parsed));
            }));
}

SPro40D3MailboxEngine::SPro40D3MailboxEngine(ISPro40D3Transport& transport,
                                             ASFW::Scheduling::ITimerScheduler& scheduler,
                                             DcpDescriptor descriptor, Options options)
    : transport_{transport}, scheduler_{scheduler}, descriptor_{descriptor}, options_{options} {
    gapNs_ = options_.commandGapMs * 1000000ull;
}

SPro40D3MailboxEngine::SPro40D3MailboxEngine(ISPro40D3Transport& transport,
                                             ASFW::Scheduling::ITimerScheduler& scheduler,
                                             DcpDescriptor descriptor)
    : SPro40D3MailboxEngine(transport, scheduler, descriptor, Options{}) {}

SPro40D3MailboxEngine::~SPro40D3MailboxEngine() {
    // Nothing may reference the engine at destruction (timer and transport
    // callbacks hold shared ownership until they fire or are cancelled), so
    // an in-flight deadline here is bookkeeping only - cancel it anyway.
    if (op_) CancelDeadline(*op_);
}

// ---------------------------------------------------------------------------
// Pump: single-threaded deferred processing (Stage-1 review P0 reentrancy)
// ---------------------------------------------------------------------------

bool SPro40D3MailboxEngine::Enter() noexcept {
    if (active_) return false; // re-entrant arrival: caller must queue
    active_ = true;
    return true;
}

void SPro40D3MailboxEngine::Exit() noexcept {
    Pump();
    active_ = false;
}

void SPro40D3MailboxEngine::LaunchTopLevel(Op pending) {
    // Called only from Send*/Begin* after they verified active_ == false.
    active_ = true;
    op_ = std::move(pending);
    Op& op = *op_;
    switch (op.kind) {
    case Kind::kRegister: {
        // Claim from unowned only, against the CONSTANT none sentinel, with
        // no preceding descriptor read: registration is transport-bounded
        // (no engine deadline), and the CAS result carries the current
        // owner atomically (HandleCasDone -> foreignOwner_). See the header
        // of BeginRegister for the hardware rationale.
        op.phase = Phase::kRegisterCas;
        const bool accepted = transport_.CompareSwap64(
            kDcpBaseAddress + 8, kDcpOwnerNone, op.registrationAddress,
            MakeCasDone(op.epoch));
        if (!accepted) {
            // Rejected at issue: the claim never left the host; the field
            // is untouched. Non-submission established.
            CompleteRegistration(op, std::unexpected(MailboxError::kTransport));
        }
        break;
    }
    case Kind::kUnregister: {
        op.phase = Phase::kUnregisterCas;
        const bool accepted = transport_.CompareSwap64(
            kDcpBaseAddress + 8, notificationAddress_, kDcpOwnerNone,
            MakeCasDone(op.epoch));
        if (!accepted) {
            // Rejected at issue: the release never left the host; our claim
            // (and the registration state) stands.
            CompleteRegistration(op, std::unexpected(MailboxError::kTransport));
        }
        break;
    }
    default:
        LaunchCurrentWrite();
        break;
    }
    Exit();
}

void SPro40D3MailboxEngine::Pump() noexcept {
    for (;;) {
        if (queuedBusReset_) {
            queuedBusReset_ = false;
            HandleBusReset();
            continue;
        }
        if (!queuedEvents_.empty()) {
            Event event = std::move(queuedEvents_.front());
            queuedEvents_.pop_front();
            HandleEvent(event);
            continue;
        }
        if (!queuedLaunches_.empty()) {
            Op cmd = std::move(queuedLaunches_.front());
            queuedLaunches_.pop_front();
            if (registration_ != RegistrationState::kRegistered) {
                // Accepted, then a bus reset unregistered us before the
                // queued launch could run: the reset is what killed it.
                FireCompletion(cmd, MailboxError::kBusReset);
            } else if (op_) {
                // Serialized slot occupied. The acceptance contract still
                // holds: complete exactly once, with kBusy.
                FireCompletion(cmd, MailboxError::kBusy);
            } else {
                op_ = std::move(cmd);
                LaunchCurrentWrite();
            }
            continue;
        }
        break;
    }
}

void SPro40D3MailboxEngine::Deliver(Event event) noexcept {
    if (!Enter()) {
        queuedEvents_.push_back(std::move(event));
        return;
    }
    HandleEvent(event);
    Exit();
}

// ---------------------------------------------------------------------------
// Ownership / registration lifecycle (asynchronous)
// ---------------------------------------------------------------------------

std::expected<void, MailboxError>
SPro40D3MailboxEngine::BeginRegister(uint64_t hostAddress, RegisterCallback completion) {
    // Registration is refused while the pump runs: it is a lifecycle
    // operation, not a queued command.
    if (active_) return std::unexpected(MailboxError::kBusy);
    if (op_) return std::unexpected(MailboxError::kBusy);
    if (registration_ == RegistrationState::kRegistered) {
        if (hostAddress == notificationAddress_) {
            // Idempotent: THIS engine's prior successful CAS is the
            // ownership corroboration; no device contact. (The read-based
            // device corroboration was removed with the register's
            // descriptor read - live hardware 2026-11-06: more than one
            // descriptor read between the inbound-window setup and the
            // claim silences the device's notifications.)
            return {};
        }
        // Registered to a different address: release first (the release
        // CAS is gated). Not a foreign-owner case; ForeignOwner() is
        // left untouched.
        return std::unexpected(MailboxError::kOwnershipConflict);
    }
    Op op;
    op.kind = Kind::kRegister;
    op.phase = Phase::kRegisterCas;
    op.registrationAddress = hostAddress;
    op.voidCb = std::move(completion);
    LaunchTopLevel(std::move(op));
    return {};
}

std::expected<void, MailboxError>
SPro40D3MailboxEngine::BeginUnregister(RegisterCallback completion) {
    if (active_) return std::unexpected(MailboxError::kBusy);
    if (op_) return std::unexpected(MailboxError::kBusy);
    if (registration_ != RegistrationState::kRegistered)
        return std::unexpected(MailboxError::kNotRegistered);
    // The release CAS is UNVERIFIED on hardware (the reference never
    // unregisters). Disabled unless tests opt in.
    if (!options_.allowOwnershipRelease)
        return std::unexpected(MailboxError::kReleaseDisabled);
    Op op;
    op.kind = Kind::kUnregister;
    op.phase = Phase::kUnregisterCas;
    op.voidCb = std::move(completion);
    LaunchTopLevel(std::move(op));
    return {};
}

void SPro40D3MailboxEngine::OnBusReset() {
    if (!Enter()) {
        queuedBusReset_ = true;
        return;
    }
    HandleBusReset();
    Exit();
}

void SPro40D3MailboxEngine::HandleBusReset() noexcept {
    // The host's registration state does not survive a reset, but the
    // device's owner register does: the D3's owner field is flash-backed
    // (live-proven 2026-11-06: it survives power cycles, generation 7->9,
    // and therefore bus resets; TCAT's per-generation re-claim is defensive
    // client behavior, not evidence of firmware clearing). The rebind's
    // fresh claim CAS meets its own retained address and adopts it.
    // Cached values are invalidated regardless: the hardware never
    // confirmed them (Stage 0 §7a).
    registration_ = RegistrationState::kUnregistered;
    notificationAddress_ = 0;
    foreignOwner_ = 0;
    counter_ = 0;
    state_.InvalidateAll();
    if (op_) {
        Op op = std::move(*op_);
        op_.reset();
        CancelDeadline(op);
        FireCompletion(op, MailboxError::kBusReset);
    }
}

// ---------------------------------------------------------------------------
// Registration event processing (called between pump steps only)
// ---------------------------------------------------------------------------

void SPro40D3MailboxEngine::HandleCasDone(Op& op, const Event& event) {
    if (!event.ok) {
        lastTransportError_ = event.status;
        CompleteRegistration(op, std::unexpected(MailboxError::kTransport));
        return;
    }
    const uint64_t previous = event.casPrevious;
    if (op.kind == Kind::kRegister) {
        if (previous == op.registrationAddress) {
            // Stale-self adoption (live hardware 2026-11-06): the field
            // already names this exact address - a claim left by an
            // earlier process of ours (the window address is fixed by the
            // DICE owner encoding, and this host's inbound range covers
            // it). The mailbox already points at our window, so the
            // registration is adopted with ZERO further device writes:
            // no release/claim cycle is performed at all, because the
            // live hardware goes silent for the next command if a
            // descriptor read crosses the owner-register transition (the
            // rebind's read must happen with the owner register stable,
            // and any release+reclaim cycle would strand its read on the
            // wrong side). A value-equal field is NOT proof of
            // self-ownership for a DIFFERENT address (finding A) - this
            // branch is exact-equality with the address we intend to
            // register, which is functionally ours by construction.
            foreignOwner_ = 0;
            notificationAddress_ = op.registrationAddress;
            registration_ = RegistrationState::kRegistered;
            CompleteRegistration(op, {});
            return;
        }
        if (previous != kDcpOwnerNone) {
            // Lost the race: the current owner is reported untouched. The
            // attempt itself proves the device does not name us (a CAS
            // against the none sentinel failed), so any prior registration
            // state is invalidated here - a stale kRegistered would let
            // subsequent Send*() pass their precondition against a mailbox
            // we know we no longer own. Re-claiming is an explicit caller
            // decision; ForeignOwner() preserves the observed value.
            foreignOwner_ = previous;
            registration_ = RegistrationState::kUnregistered;
            CompleteRegistration(op, std::unexpected(MailboxError::kOwnershipConflict));
            return;
        }
        foreignOwner_ = 0;
        notificationAddress_ = op.registrationAddress;
        registration_ = RegistrationState::kRegistered;
        CompleteRegistration(op, {});
        return;
    }
    // kUnregister: swap succeeded iff the field still named us.
    if (previous != notificationAddress_) {
        foreignOwner_ = previous;
        CompleteRegistration(op, std::unexpected(MailboxError::kOwnershipConflict));
        return;
    }
    registration_ = RegistrationState::kUnregistered;
    notificationAddress_ = 0;
    CompleteRegistration(op, {});
}

void SPro40D3MailboxEngine::CompleteRegistration(Op& op, std::expected<void, MailboxError> result) {
    if (op.voidCb) op.voidCb(std::move(result));
    op_.reset();
}

// ---------------------------------------------------------------------------
// Event plumbing helpers
// ---------------------------------------------------------------------------

ISPro40D3Transport::WriteDone SPro40D3MailboxEngine::MakeWriteDone(uint32_t epoch) {
    return ISPro40D3Transport::WriteDone{
        [self = shared_from_this(), epoch](std::expected<void, IOReturn> result) {
            Event event;
            event.type = Event::Type::kWriteDone;
            event.epoch = epoch;
            if (result) {
                event.ok = true;
            } else {
                event.status = result.error();
            }
            self->Deliver(std::move(event));
        }};
}

ISPro40D3Transport::ReadDone SPro40D3MailboxEngine::MakeReadDone(uint32_t epoch) {
    using ReadResult = std::expected<std::vector<uint8_t>, IOReturn>;
    return ISPro40D3Transport::ReadDone{
        [self = shared_from_this(), epoch](ReadResult result) {
            Event event;
            event.type = Event::Type::kReadDone;
            event.epoch = epoch;
            if (result) {
                event.ok = true;
                event.data = std::move(*result);
            } else {
                event.status = result.error();
            }
            self->Deliver(std::move(event));
        }};
}

ISPro40D3Transport::CasDone SPro40D3MailboxEngine::MakeCasDone(uint32_t epoch) {
    using CasResult = std::expected<uint64_t, IOReturn>;
    return ISPro40D3Transport::CasDone{
        [self = shared_from_this(), epoch](CasResult result) {
            Event event;
            event.type = Event::Type::kCasDone;
            event.epoch = epoch;
            if (result) {
                event.ok = true;
                event.casPrevious = *result;
            } else {
                event.status = result.error();
            }
            self->Deliver(std::move(event));
        }};
}

ASFW::Scheduling::TimerToken SPro40D3MailboxEngine::ScheduleDeadline(Op& op) {
    // Per-step generation: a cancelled (but already extracted) deadline from
    // a previous step of the SAME logical operation must not time out the
    // current step. Token-based Cancel cannot guarantee that (P00 R3: the
    // production scheduler extracts due callbacks under its lock and runs
    // them after unlock).
    const uint32_t generation = ++op.deadlineGeneration;
    const uint64_t delayNs = options_.responseTimeoutMs * 1000000ull;
    return scheduler_.ScheduleAfter(
        delayNs, [self = shared_from_this(), epoch = op.epoch, generation]() {
            Event event;
            event.type = Event::Type::kDeadline;
            event.epoch = epoch;
            event.generation = generation;
            self->Deliver(std::move(event));
        });
}

ASFW::Scheduling::TimerToken SPro40D3MailboxEngine::ScheduleGap(
    Op& op, uint64_t delayNs) {
    // Same generation discipline as ScheduleDeadline: the gap timer and the
    // response deadline share the token/generation pair, so a late extracted
    // callback from either side is filtered by the generation check and the
    // active one is re-armed on the next step.
    const uint32_t generation = ++op.deadlineGeneration;
    return scheduler_.ScheduleAfter(
        delayNs, [self = shared_from_this(), epoch = op.epoch, generation]() {
            Event event;
            event.type = Event::Type::kDeadline;
            event.epoch = epoch;
            event.generation = generation;
            self->Deliver(std::move(event));
        });
}

void SPro40D3MailboxEngine::CancelDeadline(Op& op) noexcept {
    // Bookkeeping, never correctness: a late extracted callback is filtered
    // by the generation check in HandleEvent.
    if (op.deadlineToken != ASFW::Scheduling::kInvalidTimerToken) {
        scheduler_.Cancel(op.deadlineToken);
        op.deadlineToken = ASFW::Scheduling::kInvalidTimerToken;
    }
}

// ---------------------------------------------------------------------------
// Command send path
// ---------------------------------------------------------------------------

std::expected<void, MailboxError>
SPro40D3MailboxEngine::SendSetVolume(uint8_t channel, uint8_t mix, int32_t volume,
                                     VoidCallback completion) {
    if (op_) return std::unexpected(MailboxError::kBusy);
    if (registration_ != RegistrationState::kRegistered)
        return std::unexpected(MailboxError::kNotRegistered);
    if (channel >= kChannelCount || mix >= kMixCount ||
        volume < 0 || volume > 0xFFFF)
        return std::unexpected(MailboxError::kInvalidArgument);
    Op cmd;
    cmd.kind = Kind::kSetVolume;
    cmd.expectedResponseLength = ExpectedResponseLength(Kind::kSetVolume);
    cmd.request.resize(kSetVolumeFrameSize);
    SerializeSetVolume(std::span<uint8_t, kSetVolumeFrameSize>(cmd.request), channel, mix, volume);
    cmd.volChannel = channel; cmd.volMix = mix; cmd.volValue = volume;
    cmd.voidCb = std::move(completion);
    if (active_) { queuedLaunches_.emplace_back(std::move(cmd)); return {}; }
    LaunchTopLevel(std::move(cmd));
    return {};
}

std::expected<void, MailboxError>
SPro40D3MailboxEngine::SendGetMasterMeter(uint32_t currentRateHz, MeterCallback completion) {
    if (op_) return std::unexpected(MailboxError::kBusy);
    if (registration_ != RegistrationState::kRegistered)
        return std::unexpected(MailboxError::kNotRegistered);
    if (currentRateHz == 0) return std::unexpected(MailboxError::kInvalidArgument);
    Op cmd;
    cmd.kind = Kind::kGetMasterMeter;
    cmd.expectedResponseLength = ExpectedResponseLength(Kind::kGetMasterMeter);
    cmd.request.resize(kGetMeterFrameSize);
    const bool highRate = currentRateHz > 48000;
    SerializeGetMeter(std::span<uint8_t, kGetMeterFrameSize>(cmd.request),
                      highRate ? kMasterMeterPosHighRate : kMasterMeterPosLowRate,
                      kMasterMeterCount);
    cmd.meterCb = std::move(completion);
    if (active_) { queuedLaunches_.emplace_back(std::move(cmd)); return {}; }
    LaunchTopLevel(std::move(cmd));
    return {};
}

std::expected<void, MailboxError>
SPro40D3MailboxEngine::SendGetMixerMeter(uint32_t currentRateHz, MeterCallback completion) {
    if (op_) return std::unexpected(MailboxError::kBusy);
    if (registration_ != RegistrationState::kRegistered)
        return std::unexpected(MailboxError::kNotRegistered);
    if (currentRateHz == 0) return std::unexpected(MailboxError::kInvalidArgument);
    Op cmd;
    cmd.kind = Kind::kGetMixerMeter;
    cmd.expectedResponseLength = ExpectedResponseLength(Kind::kGetMixerMeter);
    cmd.request.resize(kGetMeterFrameSize);
    const bool highRate = currentRateHz > 48000;
    SerializeGetMeter(std::span<uint8_t, kGetMeterFrameSize>(cmd.request),
                      highRate ? kMixerMeterPosHighRate : kMixerMeterPosLowRate,
                      kChannelCount);
    cmd.meterCb = std::move(completion);
    if (active_) { queuedLaunches_.emplace_back(std::move(cmd)); return {}; }
    LaunchTopLevel(std::move(cmd));
    return {};
}

std::expected<void, MailboxError>
SPro40D3MailboxEngine::SendGetControl32(uint32_t position, ControlCallback completion) {
    if (op_) return std::unexpected(MailboxError::kBusy);
    if (registration_ != RegistrationState::kRegistered)
        return std::unexpected(MailboxError::kNotRegistered);
    Op cmd;
    cmd.kind = Kind::kGetControl;
    cmd.expectedResponseLength = ExpectedResponseLength(Kind::kGetControl);
    cmd.request.resize(kGetControlFrameSize);
    SerializeGetControl(std::span<uint8_t, kGetControlFrameSize>(cmd.request), position, 4);
    cmd.controlPos = position;
    cmd.controlCb = std::move(completion);
    if (active_) { queuedLaunches_.emplace_back(std::move(cmd)); return {}; }
    LaunchTopLevel(std::move(cmd));
    return {};
}

std::expected<void, MailboxError>
SPro40D3MailboxEngine::SendSetControl32(uint32_t position, uint32_t value, uint32_t swNotice,
                                        VoidCallback completion) {
    if (op_) return std::unexpected(MailboxError::kBusy);
    if (registration_ != RegistrationState::kRegistered)
        return std::unexpected(MailboxError::kNotRegistered);
    if (swNotice != sw_notice::kOutputVolume && swNotice != sw_notice::kMuteDim &&
        swNotice != sw_notice::kMonitorPad && swNotice != sw_notice::kAdatAsSpdif)
        return std::unexpected(MailboxError::kInvalidArgument);
    Op cmd;
    cmd.kind = Kind::kSetControl;
    cmd.expectedResponseLength = ExpectedResponseLength(Kind::kSetControl);
    cmd.request.resize(28); // header + pos/len + 4-byte value
    const std::array<uint8_t, 4> valueBE{
        static_cast<uint8_t>(value >> 24), static_cast<uint8_t>(value >> 16),
        static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value)};
    SerializeSetControl(cmd.request, position, valueBE);
    cmd.controlPos = position;
    cmd.swNotice = swNotice;
    cmd.voidCb = std::move(completion);
    if (active_) { queuedLaunches_.emplace_back(std::move(cmd)); return {}; }
    LaunchTopLevel(std::move(cmd));
    return {};
}

std::expected<void, MailboxError>
SPro40D3MailboxEngine::SendSetRouting(const RouterOutSrc& outSrc, const RouterMixerSrc& mixerSrc,
                                      const RouterMeterSrc& meterSrc, VoidCallback completion) {
    if (op_) return std::unexpected(MailboxError::kBusy);
    if (registration_ != RegistrationState::kRegistered)
        return std::unexpected(MailboxError::kNotRegistered);
    // Source-domain validation (Stage-1 review round 3, finding B): every
    // entry is a symbolic source number; anything outside
    // [0, kMaxSymbolicRouteSource] has no device meaning and would overflow
    // the route entry's 12-bit source field, contaminating the destination
    // bits. Rejected BEFORE serialization: zero transport operations,
    // callback not invoked.
    for (uint32_t from : outSrc)
        if (!IsValidRouteSource(from)) return std::unexpected(MailboxError::kInvalidArgument);
    for (uint32_t from : mixerSrc)
        if (!IsValidRouteSource(from)) return std::unexpected(MailboxError::kInvalidArgument);
    for (uint32_t from : meterSrc)
        if (!IsValidRouteSource(from)) return std::unexpected(MailboxError::kInvalidArgument);
    Op cmd;
    cmd.kind = Kind::kSetRoutingLow;
    cmd.expectedResponseLength = ExpectedResponseLength(Kind::kSetRoutingLow);
    cmd.request.resize(kRoutingLowRateFrameSize);
    SerializeRoutingLowRate(std::span<uint8_t, kRoutingLowRateFrameSize>(cmd.request),
                             outSrc, mixerSrc, meterSrc);
    cmd.routerOut = outSrc; cmd.routerMixer = mixerSrc; cmd.routerMeter = meterSrc;
    cmd.voidCb = std::move(completion);
    if (active_) { queuedLaunches_.emplace_back(std::move(cmd)); return {}; }
    LaunchTopLevel(std::move(cmd));
    return {};
}

// ---------------------------------------------------------------------------
// Device ingress and the per-command state machine
// ---------------------------------------------------------------------------

void SPro40D3MailboxEngine::OnNotification(std::span<const uint8_t> frame) {
    Event event;
    event.type = Event::Type::kNotification;
    event.epoch = op_ ? op_->epoch : 0;
    event.data.assign(frame.begin(), frame.end());
    Deliver(std::move(event));
}

void SPro40D3MailboxEngine::HandleEvent(const Event& event) {
    // Identity re-check precedes every state touch (P00 R3): an extracted or
    // late callback for an operation that already reached its single
    // terminal state must be a no-op. Events carry copied results only -
    // no event references engine storage.
    if (!op_ || op_->epoch != event.epoch) {
        // Unattributable ingress is counted, never silently dropped - with
        // the Stage-1 ordering: malformed first (malformed++), well-formed
        // but unattributable second (stale++).
        if (event.type == Event::Type::kNotification) {
            if (ParseNotification(std::span<const uint8_t>(event.data))) {
                ++staleNotifications_;
            } else {
                ++malformedNotifications_;
            }
        }
        return;
    }
    switch (event.type) {
    case Event::Type::kNotification: HandleNotification(*op_, event); break;
    case Event::Type::kWriteDone:    HandleWriteDone(*op_, event); break;
    case Event::Type::kReadDone:     HandleReadDone(*op_, event); break;
    case Event::Type::kCasDone:      HandleCasDone(*op_, event); break;
    case Event::Type::kDeadline:
        if (op_->deadlineGeneration != event.generation) return; // superseded deadline
        HandleDeadline(*op_);
        break;
    }
}

void SPro40D3MailboxEngine::HandleNotification(Op& op, const Event& event) {
    auto notif = ParseNotification(std::span<const uint8_t>(event.data));
    if (!notif) {
        // Malformed ingress: never read the response area for it. The
        // genuine notification may still arrive; the deadline bounds the
        // wait.
        ++malformedNotifications_;
        return;
    }
    lastDeviceStatus_ = notif->responseStatus;
    // Hold the VALID frame (<= 12 bytes, bounded copy) until it can be
    // paired: while the write is pending its outcome decides how the
    // evidence is used; once awaited it is consumed immediately.
    const size_t held = std::min(event.data.size(), op.heldNotification.size());
    std::copy_n(event.data.begin(), held, op.heldNotification.begin());
    op.hasHeldNotification = true;
    if (op.phase == Phase::kWritePending) return; // held; the write completion consumes it
    if (op.phase == Phase::kAwaitNotification || op.phase == Phase::kRitualAnswerWait) {
        ConsumeNotification(op);
        return;
    }
    // A notification while the paired read is pending, or during a
    // registration exchange: the device cannot legitimately answer the
    // current mailbox command yet. Count it stale; the pending exchange
    // decides.
    op.hasHeldNotification = false;
    ++staleNotifications_;
}

// Live hardware (P07 2026-11-06): the device nondeterministically drops the
// ritual frame itself (an answered mismatch retry is the observable); three
// attempts keep the expected recovery under ~2 s with the 400 ms gap while
// still bounding a genuinely refusing device.
constexpr uint8_t kMaxRitualAttempts = 3;

void SPro40D3MailboxEngine::ConsumeNotification(Op& op) {
    auto notif = ParseNotification(
        std::span<const uint8_t>(op.heldNotification.data(), op.heldNotification.size()));
    op.hasHeldNotification = false;
    if (!notif) {
        ++malformedNotifications_; // cannot happen (validated at hold); defensive
        return;
    }
    // Opcode discriminator (live hardware 2026-11-06): the device echoes
    // the request opcode in every notification, including the ritual's own
    // 0x80000000 ack. A held notification whose opcode is not THIS
    // command's belongs to a previous exchange (e.g. the ritual's ack
    // arriving while the retry is gap-held): discard it and keep waiting
    // for the real one.
    if (op.phase == Phase::kRitualAnswerWait) {
        // The ritual's own ack echoes the 16-byte reset frame's form.
        static constexpr uint8_t kRitualEcho[4] = {0x80, 0x00, 0x00, 0x00};
        for (size_t i = 0; i < 4; ++i) {
            if (op.heldNotification[i] != kRitualEcho[i]) {
                ++staleNotifications_;
                return;
            }
        }
    } else {
        const auto expect = OpcodeFor(op.kind);
        for (size_t i = 0; i < 4; ++i) {
            if (op.heldNotification[i] != expect[i]) {
                ++staleNotifications_;
                return;
            }
        }
    }
    lastDeviceStatus_ = notif->responseStatus;
    if (op.phase == Phase::kRitualAnswerWait) {
        // The ritual's own ack: consume it from the response area exactly
        // like any answer (rlen+16), then the retry launches when the
        // read completes. Skipping this read leaves the ritual's answer
        // unconsumed and the mailbox drops the retry (live 2026-11-06).
        op.pendingRitualHandshake = true;
        op.expectedResponseLength = notif->responsePayloadLength;
        BeginPairedRead(op);
        return;
    }
    if (notif->responseStatus != 0) {
        // Live hardware (2026-11-06): the answer must be CONSUMED from the
        // response area before anything else is sent to the mailbox - the
        // reference reads rlen+16 after every answer, mismatch included.
        // Without this handshake the device's mailbox stays stuck: the
        // ritual and the retry that follow are silently dropped every
        // time, while raw sequences that read after every command never
        // observe it. The classification (ritual vs established refusal)
        // runs when the handshake read completes.
        op.pendingMismatchHandshake = true;
        op.savedExpectedResponseLength = op.expectedResponseLength;
        op.expectedResponseLength = notif->responsePayloadLength;
        BeginPairedRead(op);
        return;
    }
    // ---- Bounded response-area read (Stage-1 review P0; Stage 0 §7a) -------
    // Every check below happens BEFORE the transport read.
    const uint32_t rlen = notif->responsePayloadLength;
    const uint32_t readLen = static_cast<uint32_t>(kMessageHeaderSize) + rlen;
    if (readLen > descriptor_.responseAreaSize) {
        // Oversized: adversarial or desynchronized device. Never read; the
        // deadline bounds the in-flight command.
        ++malformedNotifications_;
        return;
    }
    if (rlen != op.expectedResponseLength) {
        // Well-formed but contradicts the command's expected response
        // shape: our size model is wrong for this command. Fail it without
        // reading. Ambiguous for a write: the device may have applied it.
        CompleteFailure(MailboxError::kResponseShapeMismatch, /*invalidate=*/true);
        return;
    }
    BeginPairedRead(op);
}

void SPro40D3MailboxEngine::BeginPairedRead(Op& op) {
    op.phase = Phase::kReadPending;
    const uint32_t readLen =
        static_cast<uint32_t>(kMessageHeaderSize) + op.expectedResponseLength;
    // The one paired read of the response area.
    const bool accepted =
        transport_.ReadBlock(descriptor_.readAddress, readLen, MakeReadDone(op.epoch));
    if (!accepted) {
        // The confirmation is unreadable even though the read never left
        // the host: for a write, whether it applied is unknown (P01
        // paired-read site).
        CompleteFailure(MailboxError::kTransport, /*invalidate=*/true);
    }
}

void SPro40D3MailboxEngine::RunRitual(Op& op) {
    // Counter-reset ritual (spro40d3.rs send_command): reset frame, then
    // the SAME command retried once. The reset frame is a stack buffer:
    // transports copy before returning (seam contract) and completions
    // carry results, never storage.
    //
    // The ritual is ITSELF a mailbox write to the device: the ~200 ms
    // busy window applies to it (live hardware 2026-11-06: a ritual
    // written ~150 ms after the failed command is silently DROPPED, and
    // a retry written right behind a dropped ritual never sees an
    // answer). Hold it behind the same inter-command gap, and let its
    // write anchor the retry's gap.
    if (op.phase != Phase::kGapWait && gapNs_ > 0 && haveCommandWriteAnchor_) {
        const uint64_t now = scheduler_.NowNs();
        const uint64_t earliest = lastCommandWriteNs_ + gapNs_;
        if (now < earliest) {
            op.pendingWrite = WriteStep::kResetFrame;
            op.phase = Phase::kGapWait;
            op.deadlineToken = ScheduleGap(op, earliest - now);
            return;
        }
    }
    ++op.ritualAttempts;
    SubmitResetFrame(op);
}

void SPro40D3MailboxEngine::SubmitResetFrame(Op& op) {
    std::array<uint8_t, kCounterResetFrameSize> reset{};
    reset[0] = 0x80;
    op.pendingWrite = WriteStep::kResetFrame;
    op.phase = Phase::kWritePending;
    lastCommandWriteNs_ = scheduler_.NowNs();
    haveCommandWriteAnchor_ = true;
    const bool accepted = transport_.WriteBlock(descriptor_.writeAddress, reset,
                                                MakeWriteDone(op.epoch));
    if (!accepted) {
        // Rejected at issue: the retry never left the host. NOT ambiguous -
        // the original attempt was explicitly answered nonzero
        // (non-application established). Prior state stands.
        CompleteFailure(MailboxError::kTransportNotSubmitted, /*invalidate=*/false);
    }
    // The write completion submits the retry (counter 1) and re-arms the
    // deadline; until then the ORIGINAL deadline covers the window, and a
    // timeout there leaves prior state standing (retry never submitted).
}

void SPro40D3MailboxEngine::HandleWriteDone(Op& op, const Event& event) {
    if (op.phase != Phase::kWritePending) return; // superseded step; defensive
    if (op.pendingWrite == WriteStep::kResetFrame) {
        if (!event.ok) {
            lastTransportError_ = event.status;
            // NOT ambiguous: the original attempt was explicitly answered
            // with a nonzero status (non-application established) and the
            // retry never left the host. Prior state stands.
            CompleteFailure(MailboxError::kTransportNotSubmitted, /*invalidate=*/false);
            return;
        }
        // Reset frame accepted by the device. Reference semantics
        // (spro40d3.rs send_command): the ritual frame itself consumes
        // counter 0, so the retry carries counter 1 and the next command
        // uses 2. Live-verified 2026-11-06: after the ritual the device's
        // index restarts at 0 and the counter-1 retry is ACCEPTED
        // (rstatus 0, rlen 4, real value). LaunchCurrentWrite applies the
        // command gap (live hardware drops a retry written inside the
        // post-ritual busy window).
        op.usedResetRitual = true;
        op.phase = Phase::kRitualAnswerWait;
        if (op.hasHeldNotification) ConsumeNotification(op);
        return;
    }
    // kCommand write completion (initial or ritual retry).
    if (event.ok) {
        op.phase = Phase::kAwaitNotification;
        if (op.hasHeldNotification) ConsumeNotification(op);
        return;
    }
    lastTransportError_ = event.status;
    if (op.hasHeldNotification) {
        // The device demonstrably processed the frame - it answered; only
        // the local ACK was lost. Continue through the notification path:
        // local write failure is not mailbox-command failure (plan §4
        // item 4; P02 scope).
        ConsumeNotification(op);
        return;
    }
    // Ambiguous (P01): a submitted write whose completion failed may have
    // landed (the delivered-then-lost-completion case; see
    // SPro40D3AmbiguousWriteTests). Invalidate before completing.
    CompleteFailure(MailboxError::kTransport, /*invalidate=*/true);
}

void SPro40D3MailboxEngine::HandleCommandReadDone(Op& op, const Event& event) {
    if (op.phase != Phase::kReadPending) return; // superseded step; defensive
    if (!event.ok) {
        lastTransportError_ = event.status;
        // The notification says the device answered, but the confirmation
        // is unreadable: for a write, whether it applied is unknown.
        CompleteFailure(MailboxError::kTransport, /*invalidate=*/true);
        return;
    }
    const uint32_t readLen =
        static_cast<uint32_t>(kMessageHeaderSize) + op.expectedResponseLength;
    if (event.data.size() < readLen) {
        CompleteFailure(MailboxError::kShortResponse, /*invalidate=*/true);
        return;
    }
    if (op.pendingRitualHandshake) {
        op.pendingRitualHandshake = false;
        // Reset frame consumed counter 0 (reference send_command): the
        // retry carries counter 1. LaunchCurrentWrite applies the command
        // gap (the busy window after the ritual's answered exchange). The
        // retry validates against the ORIGINAL command's expected
        // response shape - the handshake reads' rlen-0 clobber is undone.
        counter_ = 1;
        op.expectedResponseLength = op.savedExpectedResponseLength;
        op.pendingWrite = WriteStep::kCommand;
        LaunchCurrentWrite();
        return;
    }
    if (op.pendingMismatchHandshake) {
        // The nonzero-status answer's consumption is complete; the
        // notification already carried the status, the area content is
        // corroborative only. Classify now (was: ConsumeNotification's
        // pre-read path).
        op.pendingMismatchHandshake = false;
        if (op.usedResetRitual) {
            // The retry after a ritual answered nonzero. Two readings
            // exist: the device genuinely refuses the command (reference
            // semantics: established non-application), OR - live hardware
            // 2026-11-06 - the RITUAL itself was silently dropped at the
            // device's ingress (the identical 16-byte frame answers or
            // drops nondeterministically), so the retry ran against a
            // stale counter and was answered as a plain mismatch. The
            // second reading is distinguishable only by trying the ritual
            // again: bound the attempts and keep the established-failure
            // semantics once the budget is spent.
            if (op.ritualAttempts < kMaxRitualAttempts) {
                op.usedResetRitual = false;
                RunRitual(op);
                return;
            }
            CompleteFailure(MailboxError::kResponseStatus, /*invalidate=*/false);
            return;
        }
        RunRitual(op);
        return;
    }
    const std::span<const uint8_t> responseSpan{event.data};
    auto header = ParseResponseHeader(responseSpan);
    if (!header) {
        CompleteFailure(MailboxError::kShortResponse, /*invalidate=*/true);
        return;
    }
    if (header->counter != op.counter) {
        if (CounterIsRecent(header->counter)) {
            // Response belongs to a recent COMPLETED command: this
            // notification was stale. The in-flight command keeps waiting
            // (the phase returns to await; a further notification may
            // re-trigger the paired read). The deadline bounds the wait.
            op.phase = Phase::kAwaitNotification;
            ++staleNotifications_;
            return;
        }
        CompleteFailure(MailboxError::kCounterMismatch, /*invalidate=*/true);
        return;
    }
    if (!(header->opcode == OpcodeFor(op.kind))) {
        CompleteFailure(MailboxError::kMalformedResponse, /*invalidate=*/true);
        return;
    }
    if (header->payloadLength != op.expectedResponseLength) {
        CompleteFailure(MailboxError::kMalformedResponse, /*invalidate=*/true);
        return;
    }
    Op cmd = std::move(op);
    op_.reset();
    Dispatch(cmd, responseSpan);
}

void SPro40D3MailboxEngine::HandleReadDone(Op& op, const Event& event) {
    HandleCommandReadDone(op, event);
}

void SPro40D3MailboxEngine::LaunchCurrentWrite() {
    Op& op = *op_;
    // Inter-command gap (live hardware 2026-11-06): a command written inside
    // the ~200 ms busy window after the previous answered command is
    // silently DROPPED. Hold the op until the gap elapses; the scheduled
    // event re-enters here with phase == kGapWait and passes through.
    if (op.phase != Phase::kGapWait && gapNs_ > 0 && haveCommandWriteAnchor_) {
        const uint64_t now = scheduler_.NowNs();
        const uint64_t earliest = lastCommandWriteNs_ + gapNs_;
        if (now < earliest) {
            op.phase = Phase::kGapWait;
            op.deadlineToken = ScheduleGap(op, earliest - now);
            return;
        }
    }
    // Serialize the counter here, once: serializers wrote only the opcode
    // and payload (spro40d3.rs send_command fills [4..8] at send time). The
    // ritual retry arrives with counter_ = 1 (the ritual consumed 0), so
    // the retry carries 1 and the next command uses 2.
    op.counter = counter_++;
    lastCommandWriteNs_ = scheduler_.NowNs();
    haveCommandWriteAnchor_ = true;
    RememberCounter(op.counter);
    // The request length field counts bytes after the 8-byte DICE request
    // header (kDiceRequestHeaderBytes) - hardware-verified against the
    // reference arithmetic (see the constant's note).
    WriteMessageHeader(op.request, OpcodeFor(op.kind), op.counter,
                      static_cast<uint16_t>(op.request.size() -
                                           kDiceRequestHeaderBytes));
    if (op.request.size() > descriptor_.maxRequestSize) {
        CompleteFailure(MailboxError::kInvalidArgument, /*invalidate=*/false); // defensive; static frames
        return;
    }
    // A superseded step's deadline is cancelled first (bookkeeping); the
    // generation check filters any callback the scheduler already extracted.
    CancelDeadline(op);
    op.deadlineToken = ScheduleDeadline(op);
    if (op.deadlineToken == ASFW::Scheduling::kInvalidTimerToken) {
        // Deadline admission failed BEFORE any I/O. There is no transport
        // cancellation, so a submitted write must never be unbounded: zero
        // I/O, non-submission established, prior state stands.
        CompleteFailure(MailboxError::kDeadlineScheduleFailed, /*invalidate=*/false);
        return;
    }
    op.phase = Phase::kWritePending;
    // The request buffer is engine-owned storage. Nothing can move or free
    // it while WriteBlock executes (the pump defers all processing), and
    // the seam contract forbids transports to touch the span after return.
    const bool accepted = transport_.WriteBlock(descriptor_.writeAddress,
                                                std::span<const uint8_t>(op.request),
                                                MakeWriteDone(op.epoch));
    if (!accepted) {
        // Rejection at issue: the frame never left the host; non-submission
        // established. NOT ambiguous - prior state stands. (The async seam
        // distinguishes this from a completion error; the Stage-1 sync seam
        // could not and invalidated conservatively.)
        CancelDeadline(op);
        CompleteFailure(MailboxError::kTransportNotSubmitted, /*invalidate=*/false);
    }
    // An inline completion inside WriteBlock was queued by Deliver; the
    // pump drains it next.
}

void SPro40D3MailboxEngine::LaunchChainedStep(Op next) {
    op_ = std::move(next);
    LaunchCurrentWrite();
}

void SPro40D3MailboxEngine::HandleDeadline(Op& op) {
    // The deadline covers one logical step (submission, ACK, notification,
    // paired read). One precision (P02): a timeout while the reset frame is
    // in flight means the ritual retry was never submitted, and the
    // original attempt was explicitly answered nonzero - non-application
    // established, prior state stands. Every other phase has a submitted
    // write of unknown fate - ambiguous (P01) - or a pending read.
    if (op.phase == Phase::kGapWait) {
        // The inter-command gap elapsed: launch the held write now -
        // the retry (a command frame) or the held ritual frame.
        if (op.pendingWrite == WriteStep::kResetFrame) {
            SubmitResetFrame(op);
        } else {
            op.phase = Phase::kWritePending;
            LaunchCurrentWrite();
        }
        return;
    }
    const bool retryNotSubmitted = (op.pendingWrite == WriteStep::kResetFrame);
    CompleteFailure(MailboxError::kTimeout, /*invalidate=*/!retryNotSubmitted);
}

void SPro40D3MailboxEngine::Dispatch(Op& cmd, std::span<const uint8_t> response) {
    // The step's deadline has served its purpose (a chained step re-arms its
    // own in LaunchCurrentWrite). Not cancelling here would leak the token:
    // the extracted-but-never-fired callback is a harmless no-op by
    // generation check, but it pins the engine alive and PendingCount shows
    // the leak.
    CancelDeadline(cmd);
    switch (cmd.kind) {
    case Kind::kSetVolume:
        state_.mixerVolumes[cmd.volChannel][cmd.volMix] = {cmd.volValue, ValueProvenance::Cached};
        FireSuccessVoid(cmd);
        break;
    case Kind::kGetMasterMeter:
        DeserializeMeters(response,
                           std::span<int32_t>(meterBuffer_).first(kMasterMeterCount));
        if (cmd.meterCb)
            cmd.meterCb(std::span<const int32_t>(meterBuffer_).first(kMasterMeterCount));
        break;
    case Kind::kGetMixerMeter:
        DeserializeMeters(response, meterBuffer_);
        if (cmd.meterCb) cmd.meterCb(std::span<const int32_t>(meterBuffer_));
        break;
    case Kind::kGetControl: {
        const uint32_t value = ::ASFW::FW::ReadBE32(response.data() + kMessageHeaderSize);
        ApplyObserved(cmd.controlPos, value);
        if (cmd.controlCb) cmd.controlCb(value);
        break;
    }
    case Kind::kSetControl: {
        // The write itself was confirmed by its response. Chain the software
        // notice (the reference pairs every set with write_sw_notice); the
        // ORIGINAL callback travels with the chain and fires when the notice
        // completes. If the notice step fails, the completion is
        // kPartialCompletion: the value stays Cached (confirmed), the notice
        // did not land.
        ApplyCached(cmd.controlPos, ::ASFW::FW::ReadBE32(cmd.request.data() + 24));
        Op notice;
        notice.kind = Kind::kSwNotice;
        notice.isChainedStep = true;
        notice.epoch = cmd.epoch;
        notice.expectedResponseLength = ExpectedResponseLength(Kind::kSwNotice);
        notice.request.resize(kSwNoticeFrameSize);
        SerializeWriteSwNotice(std::span<uint8_t, kSwNoticeFrameSize>(notice.request),
                                cmd.swNotice);
        notice.voidCb = std::move(cmd.voidCb);
        LaunchChainedStep(std::move(notice));
        break;
    }
    case Kind::kSwNotice:
        FireSuccessVoid(cmd); // the logical set_control command is complete
        break;
    case Kind::kSetRoutingLow: {
        // Low-rate table confirmed; chain the high-rate table. The original
        // callback travels with it. If the high-rate step fails for ANY
        // reason, completion is kPartialCompletion and the routing mirror
        // falls back to Unknown (mixed state: low applied, high not - the
        // conflated mirror cannot represent a split pair; CompleteFailure
        // enforces this for every high-step failure class).
        Op high;
        high.kind = Kind::kSetRoutingHigh;
        high.isChainedStep = true;
        high.epoch = cmd.epoch;
        high.expectedResponseLength = ExpectedResponseLength(Kind::kSetRoutingHigh);
        high.request.resize(kRoutingHighRateFrameSize);
        SerializeRoutingHighRate(std::span<uint8_t, kRoutingHighRateFrameSize>(high.request),
                                  cmd.routerOut, cmd.routerMixer, cmd.routerMeter);
        high.routerOut = cmd.routerOut; high.routerMixer = cmd.routerMixer;
        high.routerMeter = cmd.routerMeter;
        high.voidCb = std::move(cmd.voidCb);
        LaunchChainedStep(std::move(high));
        break;
    }
    case Kind::kSetRoutingHigh:
        for (size_t i = 0; i < kOutputCount; ++i)
            state_.routerOutSrc[i] = {cmd.routerOut[i], ValueProvenance::Cached};
        for (size_t i = 0; i < kChannelCount; ++i)
            state_.routerMixerSrc[i] = {cmd.routerMixer[i], ValueProvenance::Cached};
        for (size_t i = 0; i < kMasterMeterCount; ++i)
            state_.routerMeterSrc[i] = {cmd.routerMeter[i], ValueProvenance::Cached};
        FireSuccessVoid(cmd);
        break;
    case Kind::kRegister:
    case Kind::kUnregister:
        break; // unreachable: registrations complete in HandleCasDone
    }
}

// ---------------------------------------------------------------------------
// Completion (exactly-once point) and failure semantics
// ---------------------------------------------------------------------------

void SPro40D3MailboxEngine::CompleteFailure(MailboxError error, bool invalidateAffectedTargets) {
    Op cmd = std::move(*op_);
    op_.reset();
    CancelDeadline(cmd);
    // Ambiguous-write policy (P01, frozen in the P00 baseline §5; see the
    // hpp design block): a submitted write whose fate is unknown invalidates
    // the affected targets; established non-submission or established
    // non-application never does. The one exception is the high-rate routing
    // step: its terminal failure ALWAYS invalidates the conflated mirror -
    // the device then holds a split table pair the mirror cannot represent,
    // whatever the high write's fate (P00 baseline §6 step 4).
    if (invalidateAffectedTargets || cmd.kind == Kind::kSetRoutingHigh) {
        InvalidateWriteTargets(cmd);
    }
    // A chained step failing after the primary write was confirmed is an
    // explicit partial completion (bus reset still reports kBusReset: it
    // invalidated everything, including possibly the applied primary).
    const MailboxError reported =
        (cmd.isChainedStep && error != MailboxError::kBusReset)
            ? MailboxError::kPartialCompletion
            : error;
    FireCompletion(cmd, reported);
}

void SPro40D3MailboxEngine::FireCompletion(Op& cmd, MailboxError error) noexcept {
    if (cmd.voidCb) cmd.voidCb(std::unexpected(error));
    else if (cmd.controlCb) cmd.controlCb(std::unexpected(error));
    else if (cmd.meterCb) cmd.meterCb(std::unexpected(error));
}

void SPro40D3MailboxEngine::FireSuccessVoid(Op& cmd) noexcept {
    if (cmd.voidCb) cmd.voidCb(std::expected<void, MailboxError>{});
}

void SPro40D3MailboxEngine::InvalidateWriteTargets(const Op& cmd) noexcept {
    switch (cmd.kind) {
    case Kind::kSetVolume:
        state_.mixerVolumes[cmd.volChannel][cmd.volMix].provenance = ValueProvenance::Unknown;
        break;
    case Kind::kSetControl:
        InvalidateControl(cmd.controlPos);
        break;
    case Kind::kSetRoutingLow:
    case Kind::kSetRoutingHigh:
        for (auto& slot : state_.routerOutSrc) slot.provenance = ValueProvenance::Unknown;
        for (auto& slot : state_.routerMixerSrc) slot.provenance = ValueProvenance::Unknown;
        for (auto& slot : state_.routerMeterSrc) slot.provenance = ValueProvenance::Unknown;
        break;
    default:
        break; // reads and sw-notice carry no mirrored device state
    }
}

// ---------------------------------------------------------------------------
// Counter attribution
// ---------------------------------------------------------------------------

void SPro40D3MailboxEngine::RememberCounter(uint16_t counter) noexcept {
    recentCounters_[recentCounterNext_] = counter;
    recentCounterNext_ = (recentCounterNext_ + 1) % kRecentCounterWindow;
    if (recentCounterCount_ < kRecentCounterWindow) ++recentCounterCount_;
}

bool SPro40D3MailboxEngine::CounterIsRecent(uint16_t counter) const noexcept {
    for (size_t i = 0; i < recentCounterCount_; ++i)
        if (recentCounters_[i] == counter) return true;
    return false;
}

Opcode SPro40D3MailboxEngine::OpcodeFor(Kind kind) noexcept {
    switch (kind) {
    case Kind::kSetVolume:      return kOpcodeSetVolume;
    case Kind::kGetMasterMeter:
    case Kind::kGetMixerMeter:  return kOpcodeGetMeter;
    case Kind::kGetControl:     return kOpcodeGetControl;
    case Kind::kSetControl:     return kOpcodeSetControl;
    case Kind::kSwNotice:       return kOpcodeWriteSwNotice;
    case Kind::kSetRoutingLow:
    case Kind::kSetRoutingHigh: return kOpcodeSetRouting;
    case Kind::kRegister:
    case Kind::kUnregister:     return kOpcodeGetControl; // unused
    }
    return kOpcodeGetControl; // unreachable
}

uint32_t SPro40D3MailboxEngine::ExpectedResponseLength(Kind kind) noexcept {
    switch (kind) {
    case Kind::kGetControl:     return 4;
    case Kind::kGetMasterMeter: return kMasterMeterCount * 4;
    case Kind::kGetMixerMeter:  return kChannelCount * 4;
    default:                    return 0; // writes: header echo only (assumption)
    }
}

// ---------------------------------------------------------------------------
// Provenance store
// ---------------------------------------------------------------------------

void SPro40D3MailboxEngine::DecodeAndStoreControl(uint32_t position, uint32_t value,
                                                  ValueProvenance prov) noexcept {
    namespace off = control_offset;
    auto set = [](auto& slot, uint32_t v, ValueProvenance p) {
        slot = {v, p};
    };
    if (position == off::kMute) set(state_.mute, value, prov);
    else if (position == off::kDim) set(state_.dim, value, prov);
    else if (position == off::kMonitorPad) set(state_.monitorPad, value, prov);
    else if (position == off::kAdatAsSpdif) set(state_.adatAsSpdif, value, prov);
    else if (position == off::kMonitorVolume) set(state_.monitorVolume, value, prov);
    else if (position >= off::kOutputVolBase &&
             position < off::kOutputVolBase + kOutputPairCount * 4) {
        int32_t even{}, odd{};
        UnpackOutputVolumePair(value, even, odd);
        const size_t pair = (position - off::kOutputVolBase) / 4;
        state_.outputVolumes[pair * 2]     = {even, prov};
        state_.outputVolumes[pair * 2 + 1]  = {odd, prov};
    }
    // kVolHwctlBase / kHwAssign are not mirrored in the Stage 1 store.
}

void SPro40D3MailboxEngine::InvalidateControl(uint32_t position) noexcept {
    namespace off = control_offset;
    auto invalidate = [](auto& slot) { slot.provenance = ValueProvenance::Unknown; };
    if (position == off::kMute) invalidate(state_.mute);
    else if (position == off::kDim) invalidate(state_.dim);
    else if (position == off::kMonitorPad) invalidate(state_.monitorPad);
    else if (position == off::kAdatAsSpdif) invalidate(state_.adatAsSpdif);
    else if (position == off::kMonitorVolume) invalidate(state_.monitorVolume);
    else if (position >= off::kOutputVolBase &&
             position < off::kOutputVolBase + kOutputPairCount * 4) {
        const size_t pair = (position - off::kOutputVolBase) / 4;
        invalidate(state_.outputVolumes[pair * 2]);
        invalidate(state_.outputVolumes[pair * 2 + 1]);
    }
}

void SPro40D3MailboxEngine::ApplyObserved(uint32_t position, uint32_t value) noexcept {
    DecodeAndStoreControl(position, value, ValueProvenance::Observed);
}

void SPro40D3MailboxEngine::ApplyCached(uint32_t position, uint32_t value) noexcept {
    DecodeAndStoreControl(position, value, ValueProvenance::Cached);
}

} // namespace ASFW::Audio::DICE::Focusrite::SPro40D3
