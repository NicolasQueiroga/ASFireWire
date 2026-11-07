// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "SPro40D3Protocol.hpp"

#include "SPro40D3ControlWrite.hpp"
#include "SPro40D3MailboxTypes.hpp"
#include "../Core/DiceNotificationRouter.hpp"
#include "../Core/DiceWaitClock.hpp"
#include "../../../../Protocols/SBP2/AddressSpaceManager.hpp"
#include "../../../Controls/ControlNotifications.hpp"
#include "../../../Model/Topology/Validate.hpp"
#include "../../../../Logging/Logging.hpp"

#include <bit>
#include <algorithm>
#include <chrono>
#include "../../../../UserClient/WireFormats/MeterValuesWire.hpp"

namespace ASFW::Audio::DICE::Focusrite::SPro40D3 {

namespace Wire = ::ASFW::UserClient::Wire;

namespace {

/// Mailbox results -> IOReturn for the IDeviceProtocol-facing hooks.
[[nodiscard]] IOReturn MapMailboxError(MailboxError error) noexcept {
    switch (error) {
        case MailboxError::kTransport:
            return kIOReturnError;
        case MailboxError::kBadDescriptor:
            return kIOReturnBadArgument;
        case MailboxError::kBusy:
            return kIOReturnBusy;
        case MailboxError::kNotRegistered:
            return kIOReturnNotReady;
        case MailboxError::kOwnershipConflict:
            return kIOReturnExclusiveAccess;
        case MailboxError::kTimeout:
            return kIOReturnTimeout;
        case MailboxError::kResponseStatus:
        case MailboxError::kCounterMismatch:
        case MailboxError::kShortResponse:
        case MailboxError::kMalformedResponse:
        case MailboxError::kResponseShapeMismatch:
            return kIOReturnDeviceError;
        case MailboxError::kBusReset:
            return kIOReturnAborted;
        case MailboxError::kInvalidArgument:
            return kIOReturnBadArgument;
        case MailboxError::kPartialCompletion:
            return kIOReturnError; // write-path-specific: the write chain
            // resolves partial outcomes by readback, never through this map
        case MailboxError::kReleaseDisabled:
            return kIOReturnUnsupported;
        case MailboxError::kDeadlineScheduleFailed:
            return kIOReturnError; // the scheduler refused: zero I/O happened
    }
    return kIOReturnError;
}

[[nodiscard]] IOReturn MapDescriptorError(DcpCodecError error) noexcept {
    return error == DcpCodecError::kTooShort ? kIOReturnUnderrun : kIOReturnBadArgument;
}

[[nodiscard]] Async::FWAddress DcpFwAddress(uint64_t address) noexcept {
    Async::FWAddress out{};
    out.addressHi = static_cast<uint16_t>(address >> 32);
    out.addressLo = static_cast<uint32_t>(address);
    return out;
}

} // namespace

SPro40D3Protocol::SPro40D3Protocol(Protocols::Ports::FireWireBusOps& busOps,
                                   Protocols::Ports::FireWireBusInfo& busInfo,
                                   Discovery::DeviceRegistry& routeRegistry,
                                   const Discovery::DeviceRouteToken& route,
                                   ::ASFW::IRM::IRMClient* irmClient,
                                   DiceWaitClock& waitClock,
                                   DiceNotificationRouter* diceNotifications,
                                   Scheduling::ITimerScheduler* preparationScheduler,
                                   Protocols::SBP2::AddressSpaceManager* space,
                                   std::unique_ptr<ISPro40D3Transport> transport,
                                   SPro40D3MailboxEngine::Options engineOptions)
    : tcat_{busOps, busInfo, routeRegistry, route, irmClient, waitClock, diceNotifications,
            TCAT::DICETcatRuntimePolicy{}, preparationScheduler}
    , space_{space}
    , scheduler_{preparationScheduler}
    , io_{busOps, busInfo, routeRegistry, route}
    , transport_{transport ? std::move(transport)
                           : std::unique_ptr<ISPro40D3Transport>(
                                 std::make_unique<SPro40D3TransportPort>(io_))} {
    engineOptions_ = engineOptions;
    if (space_ == nullptr || scheduler_ == nullptr) {
        // The control surface cannot come up; TCAT streaming still does.
        // Explicitly logged - never a silent always-denied placeholder.
        ASFW_LOG_ERROR(Audio,
                       "[SPro40D3Mailbox] protocol constructed without %s: controls "
                       "will refuse kIOReturnNotReady (streaming unaffected)",
                       space_ == nullptr ? "address space" : "scheduler");
    }
}

SPro40D3Protocol::~SPro40D3Protocol() {
    // Waiters answer with Aborted through their weak captures; the endpoint
    // dtor also deactivates, but the explicit order documents intent.
    ResetControlQueue();
    endpoint_.reset();
    engine_.reset();
}

// ---------------------------------------------------------------------------
// IDeviceProtocol
// ---------------------------------------------------------------------------

IOReturn SPro40D3Protocol::Initialize() {
    // Deliberately NO mailbox work here: the audio path must come up
    // independently of vendor controls (plan P04). RebindNotifications is
    // the explicit registration hook.
    return tcat_.Initialize();
}

IOReturn SPro40D3Protocol::Shutdown() {
    ++controlsEpoch_;
    controlsReady_ = false;
    controlSnapshot_.reset();
    StopControlPoll();
    ResetControlQueue();
    endpoint_.reset();
    engine_.reset();
    return tcat_.Shutdown();
}

void SPro40D3Protocol::UpdateRuntimeContext(const Discovery::DeviceRouteToken& route,
                                            std::shared_ptr<ASFW::AVC::IAvcUnit> avcUnit) {
    if (io_.Route() != route) {
        // A route change invalidates everything the control surface holds:
        // published state, in-flight work (epoch), the mailbox claim and
        // the reserved range. Late engine events are epoch-guarded no-ops.
        ++controlsEpoch_;
        controlsReady_ = false;
        controlSnapshot_.reset();
        StopControlPoll();
        ResetControlQueue();
        if (endpoint_) endpoint_->Deactivate();
        engine_.reset();
        ASFW_LOG(Audio,
                 "[SPro40D3Mailbox] route change: control surface invalidated "
                 "(gen=%u node=0x%02x)",
                 route.generation.value, route.nodeId & 0x3F);
    }
    io_.UpdateRoute(route);
    tcat_.UpdateRuntimeContext(route, std::move(avcUnit));
}

void SPro40D3Protocol::RebindNotifications(VoidCallback callback) {
    if (space_ == nullptr || scheduler_ == nullptr) {
        callback(kIOReturnNotReady);
        return;
    }
    auto self = std::static_pointer_cast<SPro40D3Protocol>(shared_from_this());
    // Abort any in-flight engine op BEFORE the reset: its completion must
    // fire loudly (kBusReset - the same class a real bus reset uses) and
    // its state must release NOW. Otherwise the op's async chain extends
    // this protocol's lifetime past the caller's dependencies (the live
    // reproduction: the endpoint teardown touching a freed manager) and
    // the write's caller waits forever. Rebind is an external lifecycle
    // call (never inside a pump step), so OnBusReset's pump-aware entry
    // is safe here - the same contract the reset below already requires.
    if (engine_ && engine_->OperationInFlight()) {
        engine_->OnBusReset();
    }
    if (endpoint_) endpoint_->Deactivate(); // fresh binding; idempotent
    engine_.reset();

    // (1) The LIVE descriptor, read from the device through the transport
    // seam: production carries no captured-byte assumption, and the native
    // port and the simulated transport serve the same 48-byte read. 48
    // bytes at the fixed DCP base.
    transport_->ReadBlock(kDcpBaseAddress, kDcpDescriptorSize,
                  [self, callback = std::move(callback)](
                      std::expected<std::vector<uint8_t>, IOReturn> result) mutable {
                      if (!result) {
                          callback(result.error());
                          return;
                      }
                      const auto descriptor = ParseDcpDescriptor(
                          std::span<const uint8_t>(result->data(), result->size()));
                      if (!descriptor) {
                          ASFW_LOG_ERROR(DICE, "[SPro40D3Mailbox] descriptor parse failed");
                          callback(MapDescriptorError(descriptor.error()));
                          return;
                      }
                      // (2) Engine + endpoint over the shared manager, then
                      // the claim-from-unowned-only CAS.
                      self->engine_ = std::make_shared<SPro40D3MailboxEngine>(
                          *self->transport_, *self->scheduler_, *descriptor,
                          self->engineOptions_);
                      if (!self->endpoint_) {
                          self->endpoint_ =
                              std::make_unique<SPro40D3NotificationEndpoint>(
                                  *self->space_, *self->scheduler_);
                      }
                      const auto activation =
                          self->endpoint_->Activate(self->engine_, self->io_.Route());
                      if (activation.address == 0) {
                          self->engine_.reset();
                          ASFW_LOG_ERROR(DICE,
                                         "[SPro40D3Mailbox] notification range "
                                         "unavailable; controls stay offline");
                          callback(kIOReturnNoResources);
                          return;
                      }
                      // The DICE owner encoding (hardware-verified
                      // 2026-11-06): the device's claim CAS accepts any
                      // 64-bit value, but it writes notifications ONLY for
                      // the reference owner form
                      // ((0xFFC0 | localNode) << 48) | window-address-48.
                      // Claiming the raw zero-extended window is silently
                      // accepted and suppresses every notification (live
                      // observation: claim CAS locked=YES, command write
                      // ACKed rcode=0, no notification arrived, 200 ms
                      // deadline expired). The raw window stays host-side
                      // routing only.
                      const uint64_t localNode =
                          static_cast<uint64_t>(
                              self->io_.LocalNode().value & 0x3F);
                      const uint64_t claimAddress =
                          ((0xFFC0ull | localNode) << 48) |
                          (activation.address & 0x0000'FFFF'FFFF'FFFFull);
                      const auto accepted = self->engine_->BeginRegister(
                          claimAddress,
                          [self, callback](std::expected<void, MailboxError> result) mutable {
                              if (!result) {
                                  // The claim failed (foreign owner or
                                  // transport): free the host range and
                                  // report; re-binding is the caller's
                                  // reset policy, never an implicit retry.
                                  self->endpoint_->Deactivate();
                                  callback(MapMailboxError(result.error()));
                                  return;
                              }
                              ASFW_LOG(DICE,
                                       "[SPro40D3Mailbox] registered at 0x%012llx "
                                       "(gen=%u node=0x%02x)",
                                       static_cast<unsigned long long>(
                                           self->engine_->NotificationAddress()),
                                       self->io_.Route().generation.value,
                                       self->io_.Route().nodeId & 0x3F);
                              callback(kIOReturnSuccess);
                          });
                      if (!accepted) {
                          // Freshly constructed: only kNotRegistered-class
                          // races reach here; handle honestly anyway.
                          self->endpoint_->Deactivate();
                          self->engine_.reset();
                          callback(kIOReturnBusy);
                      }
                  });
}

// ---------------------------------------------------------------------------
// IControlProvider: the read-only P04 surface
// ---------------------------------------------------------------------------

void SPro40D3Protocol::Prepare(std::function<void(IOReturn)> done) {
    if (controlsReady_) {
        done(kIOReturnSuccess); // join: already published
        return;
    }
    if (controlBusy_) {
        done(kIOReturnNotReady); // bounded, documented: no Prepare queueing
        return;
    }
    if (!engine_ || engine_->GetRegistrationState() != SPro40D3MailboxEngine::RegistrationState::kRegistered) {
        done(kIOReturnNotReady); // RebindNotifications has not run (or failed)
        return;
    }
    auto self = std::static_pointer_cast<SPro40D3Protocol>(shared_from_this());
    controlBusy_ = true;
    const auto epoch = controlsEpoch_;
    auto finish = [self, epoch, done](IOReturn status) mutable {
        if (epoch != self->controlsEpoch_) {
            // Route changed mid-read: ResetControlQueue already answered
            // the queue; publish nothing.
            done(kIOReturnAborted);
            return;
        }
        if (status == kIOReturnSuccess) {
            self->controlsReady_ = true;
            if (!self->CommitControls()) {
                self->controlsReady_ = false;
                status = kIOReturnError;
            }
        }
        self->ReleaseControl();
        if (self->controlsReady_) {
            // First publication (or recovery): the panel may have moved
            // anything while we were offline. The poll is the only way a
            // physical knob change ever reaches the app.
            self->ScheduleControlPoll();
        }
        done(status);
    };
    BeginControlGroupRead(std::move(finish));
}

std::optional<Audio::Controls::ControlSnapshot> SPro40D3Protocol::CopySnapshot() const {
    return controlsReady_ ? controlSnapshot_ : std::nullopt;
}

void SPro40D3Protocol::SetParameter(AudioModel::ParameterId id,
                                     const AudioModel::ParameterValue& value,
                                     Completion done) {
    if (id == AudioModel::ParameterId{D3Control::RefreshReadableControls}) {
        if (!std::holds_alternative<AudioModel::ActionTrigger>(value)) {
            done(kIOReturnBadArgument, AudioModel::ParameterValue{false});
            return;
        }
        if (controlBusy_) {
            // One group read at a time; a refresh arriving mid-read waits its
            // turn (bounded), exactly like the Duet's write queue.
            QueueControl([id, value, done](SPro40D3Protocol& self) {
                self.SetParameter(id, value, done);
            }, done);
            return;
        }
        if (!engine_ || engine_->GetRegistrationState() != SPro40D3MailboxEngine::RegistrationState::kRegistered) {
            done(kIOReturnNotReady, AudioModel::ParameterValue{AudioModel::ActionTrigger{}});
            return;
        }
        // The refresh action: re-read the whole group and (re)publish. This is
        // the app-only recovery for a failed or stale attempt - including from
        // controlsReady_ == false, which is the point.
        auto self = std::static_pointer_cast<SPro40D3Protocol>(shared_from_this());
        controlBusy_ = true;
        const auto epoch = controlsEpoch_;
        BeginControlGroupRead([self, epoch, done](IOReturn status) mutable {
            if (epoch != self->controlsEpoch_) {
                done(kIOReturnAborted, AudioModel::ParameterValue{AudioModel::ActionTrigger{}});
                return;
            }
            if (status == kIOReturnSuccess) {
                self->controlsReady_ = true;
                if (!self->CommitControls()) {
                    self->controlsReady_ = false;
                    status = kIOReturnError;
                }
            }
            self->ReleaseControl();
            done(status, AudioModel::ParameterValue{AudioModel::ActionTrigger{}});
        });
        return;
    }

    // -- The P06 routing surfaces -------------------------------------------
    // The deliberate full-table application (P00.4 safe profile): the ONLY
    // way an unknown routing table becomes host-known. Differential edits
    // ride the same full-table write against the known program.
    if (id == AudioModel::ParameterId{kApplyRoutingAction}) {
        if (!std::holds_alternative<AudioModel::ActionTrigger>(value)) {
            done(kIOReturnBadArgument, AudioModel::ParameterValue{false});
            return;
        }
        if (!controlsReady_ || !controlSnapshot_) {
            // P00 §6 step 2: the control sweep must be Observed before any
            // routing claim; nothing is written otherwise.
            done(kIOReturnNotReady, AudioModel::ParameterValue{false});
            return;
        }
        if (controlBusy_) {
            QueueControl([id, value, done](SPro40D3Protocol& self) {
                self.SetParameter(id, value, done);
            }, done);
            return;
        }
        if (!engine_ || engine_->GetRegistrationState() !=
                            SPro40D3MailboxEngine::RegistrationState::kRegistered) {
            done(kIOReturnNotReady, AudioModel::ParameterValue{AudioModel::ActionTrigger{}});
            return;
        }
        controlBusy_ = true;
        BeginRoutingProgramWrite(kSafeRouterOutSrc, kSafeRouterMixerSrc,
                                 kSafeRouterMeterSrc,
                                 AudioModel::ParameterValue{AudioModel::ActionTrigger{}},
                                 std::move(done));
        return;
    }
    if ((id.value >> 16) >= 0x0200 && (id.value >> 16) <= 0x0202) {
        // A source-select parameter. The write granularity is the WHOLE
        // low+high table pair, so the current program must be host-known
        // (applied): an unknown table is never guessed at (plan P06).
        const uint32_t table = (id.value >> 16) & 0xFF;
        const uint32_t index = id.value & 0xFFFFu;
        const uint32_t count = table == 0 ? kRouterOutSlots
                              : table == 1 ? static_cast<uint32_t>(kD3MixerSlots)
                                           : 2u;
        if (index >= count) {
            done(kIOReturnUnsupported, AudioModel::ParameterValue{false});
            return;
        }
        if (!std::holds_alternative<int64_t>(value)) {
            done(kIOReturnBadArgument, AudioModel::ParameterValue{false});
            return;
        }
        const int64_t from = std::get<int64_t>(value);
        const int64_t legalMax =
            table == 1 ? 38 : static_cast<int64_t>(kMaxSymbolicRouteSource);
        if (from < 0 || from > legalMax) {
            done(kIOReturnBadArgument, AudioModel::ParameterValue{false});
            return;
        }
        if (!controlsReady_ || !controlSnapshot_) {
            done(kIOReturnNotReady, AudioModel::ParameterValue{false});
            return;
        }
        if (!routingState_.FamilyApplied()) {
            ASFW_LOG(Audio,
                     "[SPro40D3Mailbox] routing edit refused at 0x%x: no applied "
                     "program (apply the host routing program first)",
                     id.value);
            done(kIOReturnNotReady, AudioModel::ParameterValue{false});
            return;
        }
        if (controlBusy_) {
            QueueControl([id, value, done](SPro40D3Protocol& self) {
                self.SetParameter(id, value, done);
            }, done);
            return;
        }
        if (!validateParameterWrite(controlSnapshot_->topology,
                                     controlSnapshot_->state, id, value)) {
            done(kIOReturnBadArgument, AudioModel::ParameterValue{false});
            return;
        }
        auto next = routingState_;
        if (table == 0) next.outSrc[index] = static_cast<uint32_t>(from);
        else if (table == 1) next.mixerSrc[index] = static_cast<uint32_t>(from);
        else next.meterSrc[index] = static_cast<uint32_t>(from);
        controlBusy_ = true;
        BeginRoutingProgramWrite(next.outSrc, next.mixerSrc, next.meterSrc,
                                 value, std::move(done));
        return;
    }

    // -- The P08 mixer surfaces --------------------------------------------
    // (a) The deliberate whole-mixer initialization: the ONLY way the
    //     unreadable coefficients become host-known. Ordinary cell writes
    //     refuse while Unknown (the shared validator's MissingParameter
    //     gate); this action establishes the reviewed all-silence recipe.
    if (id == AudioModel::ParameterId{D3Control::InitializeMixer}) {
        if (!std::holds_alternative<AudioModel::ActionTrigger>(value)) {
            done(kIOReturnBadArgument, AudioModel::ParameterValue{false});
            return;
        }
        if (!engine_ || engine_->GetRegistrationState() !=
                            SPro40D3MailboxEngine::RegistrationState::kRegistered) {
            done(kIOReturnNotReady, AudioModel::ParameterValue{AudioModel::ActionTrigger{}});
            return;
        }
        if (!controlsReady_ || !controlSnapshot_) {
            done(kIOReturnNotReady, AudioModel::ParameterValue{AudioModel::ActionTrigger{}});
            return;
        }
        if (controlBusy_) {
            QueueControl([id, value, done](SPro40D3Protocol& self) {
                self.SetParameter(id, value, done);
            }, done);
            return;
        }
        controlBusy_ = true;
        BeginMixerInit(std::move(done));
        return;
    }
    // (b) One coefficient. Requires the cell to be host-known (Cached):
    //     the validateParameterWrite below refuses stateless cells with
    //     MissingParameter - exactly the plan's gate. Only the requested
    //     coefficient is written, never a 288-cell sweep.
    if (id.value >= D3Control::MixerCellBase &&
        id.value < D3Control::MixerCellBase + D3Control::kMixerCellCount) {
        if (!std::holds_alternative<double>(value)) {
            done(kIOReturnBadArgument, AudioModel::ParameterValue{false});
            return;
        }
        if (!controlsReady_ || !controlSnapshot_) {
            done(kIOReturnNotReady, AudioModel::ParameterValue{false});
            return;
        }
        if (controlBusy_) {
            QueueControl([id, value, done](SPro40D3Protocol& self) {
                self.SetParameter(id, value, done);
            }, done);
            return;
        }
        if (!validateParameterWrite(controlSnapshot_->topology,
                                    controlSnapshot_->state, id, value)) {
            done(kIOReturnBadArgument, AudioModel::ParameterValue{false});
            return;
        }
        controlBusy_ = true;
        BeginMixerCellWrite(id, std::get<double>(value), std::move(done));
        return;
    }

    // -- The P05 write path -------------------------------------------------
    // Writable surface: output levels/mutes, hardware-volume assignments,
    // mute/dim targets, global mute/dim, monitor pad, optical mode. The
    // get-only knob refuses here; the mixer cells ride the P08 branch
    // above; routing rides the P06 branch.
    const auto position = D3WritableWordPosition(id);
    if (!position) {
        done(kIOReturnUnsupported, AudioModel::ParameterValue{false});
        return;
    }
    if (!controlsReady_ || !controlSnapshot_) {
        done(kIOReturnNotReady, AudioModel::ParameterValue{false});
        return;
    }
    if (controlBusy_) {
        QueueControl([id, value, done](SPro40D3Protocol& self) {
            self.SetParameter(id, value, done);
        }, done);
        return;
    }
    // Canonical authorization + effective-domain validation (access,
    // applicability, type, bounds) against the published model.
    if (!validateParameterWrite(controlSnapshot_->topology, controlSnapshot_->state,
                                id, value)) {
        done(kIOReturnBadArgument, AudioModel::ParameterValue{false});
        return;
    }
    auto self = std::static_pointer_cast<SPro40D3Protocol>(shared_from_this());
    controlBusy_ = true;
    BeginControlWrite(id, value, *position, std::move(done));
}

// ---------------------------------------------------------------------------
// The P08 mixer coefficient surfaces
// ---------------------------------------------------------------------------

namespace {
/// The initialization recipe (plan §4: a complete reviewed host recipe):
/// all-silence. The safest baseline - it can only quiet a bus, never
/// blast one - and the only state ordinary fader writes can start from.
constexpr int32_t kMixerInitRecipe = 0;
} // namespace

struct SPro40D3Protocol::MixerCellWriteState {
    std::shared_ptr<SPro40D3Protocol> self;
    uint64_t epoch{0};
    AudioModel::ParameterId id{};
    double value{0};
    Completion done;
};

void SPro40D3Protocol::BeginMixerCellWrite(AudioModel::ParameterId id, double value,
                                           Completion done) {
    auto state = std::make_shared<MixerCellWriteState>();
    state->self = std::static_pointer_cast<SPro40D3Protocol>(shared_from_this());
    state->epoch = controlsEpoch_;
    state->id = id;
    state->value = value;
    state->done = std::move(done);
    const uint32_t cell = id.value - D3Control::MixerCellBase;
    const uint32_t bus = cell / kD3MixerSlots;
    const uint32_t slot = cell % kD3MixerSlots;
    // The engine stores the cell (Cached) on its own completion; the
    // commit below publishes it. No readback exists (§8) - Cached IS the
    // honest provenance.
    const auto accepted = engine_->SendSetVolume(
        static_cast<uint8_t>(slot), static_cast<uint8_t>(bus),
        static_cast<int32_t>(value),
        [state](std::expected<void, MailboxError> result) {
            auto* self = state->self.get();
            if (state->epoch != self->controlsEpoch_) {
                self->ReleaseControl();
                state->done(kIOReturnAborted, AudioModel::ParameterValue{false});
                return;
            }
            if (!result) {
                // The engine left the cell Unknown on ambiguity; nothing
                // was published for it.
                self->ReleaseControl();
                state->done(MapMailboxError(result.error()),
                            AudioModel::ParameterValue{false});
                return;
            }
            (void)self->CommitControls();
            self->ReleaseControl();
            state->done(kIOReturnSuccess,
                        AudioModel::ParameterValue{state->value});
        });
    if (!accepted) {
        ReleaseControl();
        done(MapMailboxError(accepted.error()), AudioModel::ParameterValue{false});
    }
}

struct SPro40D3Protocol::MixerInitState
    : std::enable_shared_from_this<MixerInitState> {
    std::shared_ptr<SPro40D3Protocol> self;
    uint64_t epoch{0};
    uint32_t next{0}; // the next cell index (bus*18 + slot)
    Completion done;

    void Step() {
        // Required chained steps: one coefficient in flight at a time,
        // never coalesced, never batched (plan §4). Each engine callback
        // keeps this state alive via shared_from_this: the chain, not the
        // caller, owns the sequence.
        if (next >= D3Control::kMixerCellCount) {
            (void)self->CommitControls();
            self->ReleaseControl();
            done(kIOReturnSuccess, AudioModel::ParameterValue{AudioModel::ActionTrigger{}});
            return;
        }
        const uint32_t bus = next / kD3MixerSlots;
        const uint32_t slot = next % kD3MixerSlots;
        ++next;
        const auto accepted = self->engine_->SendSetVolume(
            static_cast<uint8_t>(slot), static_cast<uint8_t>(bus), kMixerInitRecipe,
            [keep = shared_from_this()](std::expected<void, MailboxError> result) {
                keep->OnWrite(result);
            });
        if (!accepted) {
            self->ReleaseControl();
            done(MapMailboxError(accepted.error()), AudioModel::ParameterValue{false});
        }
    }

    void OnWrite(std::expected<void, MailboxError> result) {
        if (epoch != self->controlsEpoch_) {
            self->ReleaseControl();
            done(kIOReturnAborted, AudioModel::ParameterValue{false});
            return;
        }
        if (!result) {
            // Failure-stop (P00 §6): cells written so far are Cached and
            // honestly visible in the snapshot; the unestablished rest
            // stays Unknown. The action NEVER reports success partially.
            self->ReleaseControl();
            done(MapMailboxError(result.error()), AudioModel::ParameterValue{false});
            return;
        }
        Step();
    }
};

void SPro40D3Protocol::BeginMixerInit(Completion done) {
    auto state = std::make_shared<MixerInitState>();
    state->self = std::static_pointer_cast<SPro40D3Protocol>(shared_from_this());
    state->epoch = controlsEpoch_;
    state->done = std::move(done);
    // The first SendSetVolume may be refused (busy) - handle honestly.
    state->Step();
}

// ---------------------------------------------------------------------------
// The P06 routing program write (one full low+high table pair per edit)
// ---------------------------------------------------------------------------

struct SPro40D3Protocol::RoutingWriteState {
    std::shared_ptr<SPro40D3Protocol> self;
    uint64_t epoch{0};
    RouterOutSrc out{};
    RouterMixerSrc mixer{};
    RouterMeterSrc meter{};
    AudioModel::ParameterValue report{false};
    Completion done;
};

void SPro40D3Protocol::BeginRoutingProgramWrite(
    RouterOutSrc outSrc, RouterMixerSrc mixerSrc, RouterMeterSrc meterSrc,
    AudioModel::ParameterValue report, Completion done) {
    auto state = std::make_shared<RoutingWriteState>();
    state->self = std::static_pointer_cast<SPro40D3Protocol>(shared_from_this());
    state->epoch = controlsEpoch_;
    state->out = outSrc;
    state->mixer = mixerSrc;
    state->meter = meterSrc;
    state->report = std::move(report);
    state->done = std::move(done);
    // One serialized mailbox op (the engine chains low -> high internally);
    // the busy flag and the epoch guard carry the P05 write discipline.
    const auto accepted =
        engine_->SendSetRouting(outSrc, mixerSrc, meterSrc,
                                [state](std::expected<void, MailboxError> result) {
            auto* self = state->self.get();
            if (state->epoch != self->controlsEpoch_) {
                self->ReleaseControl();
                state->done(kIOReturnAborted, AudioModel::ParameterValue{false});
                return;
            }
            if (result) {
                // Both tables confirmed. The program is host-known at BOTH
                // rate families; publication claims the family in force.
                self->routingState_.outSrc = state->out;
                self->routingState_.mixerSrc = state->mixer;
                self->routingState_.meterSrc = state->meter;
                self->routingState_.lowApplied = true;
                self->routingState_.highApplied = true;
                (void)self->CommitControls();
                self->ReleaseControl();
                state->done(kIOReturnSuccess, state->report);
                return;
            }
            const auto err = result.error();
            if (err == MailboxError::kPartialCompletion) {
                // The low table confirmed, the chained high table failed
                // (the engine's conflated routing mirror is invalidated).
                // The low table's program IS in force and known; the high
                // table is unknown. The operation as a whole NEVER
                // completes success (P00 §6 failure-stop states).
                self->routingState_.outSrc = state->out;
                self->routingState_.mixerSrc = state->mixer;
                self->routingState_.meterSrc = state->meter;
                self->routingState_.lowApplied = true;
                self->routingState_.highApplied = false;
                (void)self->CommitControls();
                self->ReleaseControl();
                state->done(kIOReturnError, state->report);
                return;
            }
            // Two failure classes are ESTABLISHED non-application and the
            // prior program stands: the device's explicit rejection after
            // the reset ritual (kResponseStatus), and a write rejected at
            // issue that never left the host (kTransportNotSubmitted). Any
            // other failure is ambiguous (the write may have landed): both
            // tables' knowledge is invalidated - unknown, never republished
            // as current (P00 §6 failure-stop states).
            if (err != MailboxError::kResponseStatus &&
                err != MailboxError::kTransportNotSubmitted) {
                self->routingState_.lowApplied = false;
                self->routingState_.highApplied = false;
            }
            (void)self->CommitControls();
            self->ReleaseControl();
            state->done(kIOReturnError, AudioModel::ParameterValue{false});
        });
    if (!accepted) {
        ReleaseControl();
        done(kIOReturnBusy, AudioModel::ParameterValue{false});
    }
}

void SPro40D3Protocol::SetRoute(AudioModel::NodeId router, AudioModel::PortId output,
                                std::optional<AudioModel::PortId> input,
                                Completion done) {
    // P06: the generic route edit maps onto the same single routing state -
    // one full low+high table write per edit, exactly like the source
    // parameters. Legality (the bundle set), the constraints, and the
    // unknown-baseline refusal all come from the shared route machinery
    // against the published model: an un-applied program publishes NO
    // router state, and prepareRouteWrite refuses it.
    if (router != AudioModel::NodeId{0x100} || !controlsReady_ ||
        !controlSnapshot_) {
        done(kIOReturnUnsupported, AudioModel::ParameterValue{false});
        return;
    }
    const auto prepared = AudioModel::prepareRouteWrite(
        controlSnapshot_->topology, controlSnapshot_->state, router, output,
        input);
    if (!prepared) {
        ASFW_LOG(Audio, "[SPro40D3Mailbox] route edit refused: %s",
                 prepared.error().message.c_str());
        done(kIOReturnBadArgument, AudioModel::ParameterValue{false});
        return;
    }
    if (controlBusy_) {
        QueueControl([router, output, input, done](SPro40D3Protocol& self) {
            self.SetRoute(router, output, input, done);
        }, done);
        return;
    }
    if (!engine_ || engine_->GetRegistrationState() !=
                        SPro40D3MailboxEngine::RegistrationState::kRegistered) {
        done(kIOReturnNotReady, AudioModel::ParameterValue{false});
        return;
    }
    // Map the ports back onto the wire pair through the router node's
    // ordered port lists (the same order the model built them in).
    const auto nodeIt = std::ranges::find(controlSnapshot_->topology.nodes, router,
                                          &AudioModel::Node::id);
    const auto& routerNode = std::get<AudioModel::RouterNode>(nodeIt->body);
    const auto outIt = std::ranges::find(routerNode.outputs, output);
    const auto inIt = input ? std::ranges::find(routerNode.inputs, *input)
                            : routerNode.inputs.end();
    if (outIt == routerNode.outputs.end() ||
        (input && inIt == routerNode.inputs.end())) {
        done(kIOReturnBadArgument, AudioModel::ParameterValue{false});
        return;
    }
    const uint32_t slot = static_cast<uint32_t>(outIt - routerNode.outputs.begin());
    const uint32_t from = input
        ? static_cast<uint32_t>(inIt - routerNode.inputs.begin()) + 1
        : kRouteNone;
    auto next = routingState_;
    if (slot < kRouterOutSlots) next.outSrc[slot] = from;
    else next.mixerSrc[slot - kRouterOutSlots] = from;
    controlBusy_ = true;
    BeginRoutingProgramWrite(next.outSrc, next.mixerSrc, next.meterSrc,
                             AudioModel::ParameterValue{int64_t(from)},
                             std::move(done));
}

// ---------------------------------------------------------------------------
// The readable-group read chain (one mailbox command at a time)
// ---------------------------------------------------------------------------

struct SPro40D3Protocol::ControlGroupState {
    std::shared_ptr<SPro40D3Protocol> self;
    uint64_t epoch{0};
    std::function<void(IOReturn)> done;
    size_t index{0};
};

void SPro40D3Protocol::BeginControlGroupRead(std::function<void(IOReturn)> done) {
    auto state = std::make_shared<ControlGroupState>();
    state->self = std::static_pointer_cast<SPro40D3Protocol>(shared_from_this());
    state->epoch = controlsEpoch_;
    state->done = std::move(done);
    ControlGroupAdvance(state);
}

void SPro40D3Protocol::ControlGroupAdvance(const std::shared_ptr<ControlGroupState>& state) {
    if (state->epoch != controlsEpoch_) {
        state->done(kIOReturnAborted);
        return;
    }
    if (state->index == kD3ReadableWordCount) {
        state->done(kIOReturnSuccess);
        return;
    }
    const uint32_t position = kD3ReadableOffsets[state->index];
    const auto accepted = engine_->SendGetControl32(
        position, [state](std::expected<uint32_t, MailboxError> result) {
            if (!result) {
                state->done(MapMailboxError(result.error()));
                return;
            }
            state->self->StoreReadableWord(state->index, *result);
            ++state->index;
            state->self->ControlGroupAdvance(state);
        });
    if (!accepted) {
        // The serialized engine was busy: a genuine admission failure -
        // our own one-at-a-time discipline makes this unreachable in
        // practice; refuse honestly instead of retrying.
        state->done(kIOReturnBusy);
    }
}

void SPro40D3Protocol::StoreReadableWord(size_t index, uint32_t word) {
    const uint32_t offset = kD3ReadableOffsets[index];
    if (offset >= control_offset::kOutputVolBase &&
        offset < control_offset::kOutputVolBase + 5 * 4) {
        store_.volumes[(offset - control_offset::kOutputVolBase) / 4] = word;
    } else if (offset >= control_offset::kVolHwctlBase &&
               offset < control_offset::kVolHwctlBase + 5 * 4) {
        store_.hwctl[(offset - control_offset::kVolHwctlBase) / 4] = word;
    } else if (offset == control_offset::kMute) {
        store_.mute = word;
    } else if (offset == control_offset::kDim) {
        store_.dim = word;
    } else if (offset == control_offset::kHwAssign) {
        store_.assigns = word;
    } else if (offset == control_offset::kMonitorPad) {
        store_.monitorPad = word;
    } else if (offset == control_offset::kMonitorVolume) {
        store_.monitorVolume = word;
    } else if (offset == control_offset::kAdatAsSpdif) {
        store_.adatAsSpdif = word;
    }
    store_.observed[index] = true; // fresh observation: the word is trusted
}

void SPro40D3Protocol::MarkWordUnknown(uint32_t position) {
    const auto it = std::find(kD3ReadableOffsets.begin(), kD3ReadableOffsets.end(),
                              position);
    if (it == kD3ReadableOffsets.end()) return;
    store_.observed[static_cast<size_t>(it - kD3ReadableOffsets.begin())] = false;
}

// ---------------------------------------------------------------------------
// The write chain (P05): fresh read -> merge-plan -> set+notice -> get-back.
// One mailbox command at a time, the same serialized engine discipline as
// the group read. The get-back ALWAYS runs: it makes success Observed and
// disambiguates a failed write (the device may have applied it anyway -
// P02 delivered-then-lost). The OBSERVED word is what gets published and
// returned, never the intended image (plan P05: a successful notice is not
// output readback, and Unknown is not success).
// ---------------------------------------------------------------------------

struct SPro40D3Protocol::ControlWriteState {
    std::shared_ptr<SPro40D3Protocol> self;
    uint64_t epoch{0};
    AudioModel::ParameterId id{};
    uint32_t position{0};
    AudioModel::ParameterValue value{false};
    Completion done;
};

void SPro40D3Protocol::BeginControlWrite(AudioModel::ParameterId id,
                                         const AudioModel::ParameterValue& value,
                                         uint32_t position, Completion done) {
    auto state = std::make_shared<ControlWriteState>();
    state->self = std::static_pointer_cast<SPro40D3Protocol>(shared_from_this());
    state->epoch = controlsEpoch_;
    state->id = id;
    state->position = position;
    state->value = value;
    state->done = std::move(done);

    // Step 1: the FRESH word. The merge must start from what the device
    // holds NOW - the front panel may have moved a pair-packed neighbour
    // (or this channel's knob) since the last observation.
    const auto accepted = engine_->SendGetControl32(
        position, [state](std::expected<uint32_t, MailboxError> word) {
            auto* self = state->self.get();
            if (state->epoch != self->controlsEpoch_) {
                self->ReleaseControl();
                state->done(kIOReturnAborted, AudioModel::ParameterValue{false});
                return;
            }
            if (!word) {
                // Nothing was written: the store stands as last observed.
                self->ReleaseControl();
                state->done(MapMailboxError(word.error()),
                            AudioModel::ParameterValue{false});
                return;
            }
            // Step 2: the pure merge-plan (adjacent bits survive).
            const auto plan = BuildSPro40D3ParameterWrite(state->id, state->value,
                                                          *word);
            if (!plan) {
                self->ReleaseControl();
                state->done(kIOReturnBadArgument, AudioModel::ParameterValue{false});
                return;
            }
            // Step 3: set_control + the chained software notice. The
            // engine distinguishes the write outcome from the notice
            // outcome; a confirmed write with a failed notice completes
            // kPartialCompletion (the write applied; the device's own
            // bookkeeping missed).
            const auto writeAccepted = self->engine_->SendSetControl32(
                plan->position, plan->word, plan->notice,
                [state, plan](std::expected<void, MailboxError> wresult) {
                    auto* self = state->self.get();
                    if (state->epoch != self->controlsEpoch_) {
                        self->ReleaseControl();
                        state->done(kIOReturnAborted, AudioModel::ParameterValue{false});
                        return;
                    }
                    const bool partial = !wresult &&
                                         wresult.error() == MailboxError::kPartialCompletion;
                    if (partial) {
                        ASFW_LOG(Audio,
                                 "[SPro40D3Mailbox] write confirmed but notice "
                                 "failed at 0x%x: readback decides",
                                 plan->position);
                    }
                    // A non-partial write failure is ambiguous (may have
                    // applied): the get-back resolves it. Status flows to
                    // the caller alongside the observed value either way.
                    const IOReturn writeStatus =
                        wresult ? kIOReturnSuccess
                                : (partial ? kIOReturnSuccess
                                           : MapMailboxError(wresult.error()));
                    // A failed set (or a failed chained notice) leaves its
                    // notification in the ingress pipeline; issuing the
                    // get-back from inside this completion would race that
                    // straggler into the new op (it shape-mismatches against
                    // the get and reads as a device fault). Hop once so the
                    // straggler lands with no op in flight (counted stale,
                    // harmless) BEFORE the get-back is issued. The busy
                    // flag stays held across the hop; the epoch check in
                    // the continuation is the abort path.
                    const auto weak = std::weak_ptr<SPro40D3Protocol>{state->self};
                    const auto hopAccepted = self->scheduler_->ScheduleAfter(
                        0, [weak, state, plan, writeStatus] {
                            const auto owner = weak.lock();
                            if (!owner) return;
                            auto* self = static_cast<SPro40D3Protocol*>(owner.get());
                            if (state->epoch != self->controlsEpoch_) {
                                self->ReleaseControl();
                                state->done(kIOReturnAborted,
                                            AudioModel::ParameterValue{false});
                                return;
                            }
                            const auto backAccepted = self->engine_->SendGetControl32(
                                plan->position,
                                [state, plan, writeStatus](
                                    std::expected<uint32_t, MailboxError> back) {
                                    auto* self = state->self.get();
                                    if (state->epoch != self->controlsEpoch_) {
                                        self->ReleaseControl();
                                        state->done(kIOReturnAborted,
                                                    AudioModel::ParameterValue{false});
                                        return;
                                    }
                                    if (!back) {
                                        // The write may have applied and cannot be
                                        // observed: the word - and only that word -
                                        // goes Unknown (absent state, the same
                                        // convention as the mixer cells).
                                        self->MarkWordUnknown(plan->position);
                                        (void)self->CommitControls();
                                        self->ReleaseControl();
                                        state->done(kIOReturnError,
                                                    AudioModel::ParameterValue{false});
                                        return;
                                    }
                                    // Step 4: publish the OBSERVED word.
                                    const auto it = std::find(
                                        kD3ReadableOffsets.begin(),
                                        kD3ReadableOffsets.end(), plan->position);
                                    if (it != kD3ReadableOffsets.end()) {
                                        self->StoreReadableWord(
                                            static_cast<size_t>(
                                                it - kD3ReadableOffsets.begin()),
                                            *back);
                                    }
                                    if (!self->CommitControls()) {
                                        self->ReleaseControl();
                                        state->done(kIOReturnError,
                                                    AudioModel::ParameterValue{false});
                                        return;
                                    }
                                    self->ReleaseControl();
                                    const auto found =
                                        self->controlSnapshot_->state.parameters.find(
                                            state->id);
                                    if (found ==
                                        self->controlSnapshot_->state.parameters.end()) {
                                        state->done(kIOReturnError,
                                                    AudioModel::ParameterValue{false});
                                        return;
                                    }
                                    state->done(writeStatus, found->second.value);
                                });
                            if (!backAccepted) {
                                // Verification is impossible: the word goes
                                // Unknown, exactly like a failed get-back.
                                self->MarkWordUnknown(plan->position);
                                (void)self->CommitControls();
                                self->ReleaseControl();
                                state->done(kIOReturnError,
                                            AudioModel::ParameterValue{false});
                            }
                        });
                    if (!hopAccepted) {
                        // No scheduler hop available: the pipeline cannot
                        // settle; the write is unverifiable. Unknown.
                        self->MarkWordUnknown(plan->position);
                        (void)self->CommitControls();
                        self->ReleaseControl();
                        state->done(kIOReturnError, AudioModel::ParameterValue{false});
                    }
                });
            if (!writeAccepted) {
                self->ReleaseControl();
                state->done(kIOReturnBusy, AudioModel::ParameterValue{false});
            }
        });
    if (!accepted) {
        ReleaseControl();
        state->done(kIOReturnBusy, AudioModel::ParameterValue{false});
    }
}

// ---------------------------------------------------------------------------
// Publication (the Duet CommitControls pattern, with the D3's opt-in
// completeness policy for its readback-absent mixer cells)
// ---------------------------------------------------------------------------

bool SPro40D3Protocol::CommitControls() {
    namespace AM = ASFW::AudioModel;
    namespace C = ASFW::Audio::Controls;
    // P08: host-written mixer cells publish Cached state; never-written
    // cells stay stateless (the view adapts the engine's provenance).
    D3MixerCells mixerCells;
    if (engine_) {
        const auto& vol = engine_->ControlState().mixerVolumes;
        for (size_t slot = 0; slot < vol.size(); ++slot) {
            for (size_t bus = 0; bus < vol[slot].size(); ++bus) {
                mixerCells.known[slot][bus] =
                    vol[slot][bus].provenance != ValueProvenance::Unknown;
                mixerCells.value[slot][bus] = vol[slot][bus].value;
            }
        }
    }
    auto next = BuildSPro40D3Controls(store_, routingState_, io_.Route().routeEpoch,
                                      controlSnapshot_ ? controlSnapshot_->sequence : 0,
                                      &mixerCells);
    if (!AM::validate(next.topology)) {
        return false;
    }
    // The one narrow opt-in (plan §4): every PRESENT entry is validated
    // strictly; the 288 mixer cells (and every future router state) are
    // permitted to be absent - the D3 has no readback for them, and absence
    // is the honest representation of unknown.
    auto policy = AM::StateCompletenessPolicy{};
    policy.requireCompleteParameterState = false;
    // P06: routing state is absent until a program is deliberately applied
    // (no readback exists - absence is the honest representation); every
    // PRESENT entry, parameter or bundle, stays strictly validated.
    policy.requireCompleteRouterState = false;
    if (!AM::validateState(next.topology, next.state, policy)) {
        return false;
    }
    if (!controlSnapshot_ || controlSnapshot_->topology.revision != next.topology.revision) {
        const bool rebound = controlSnapshot_.has_value();
        if (rebound) {
            next.sequence = C::ControlNotifications::Shared().NextSequence();
        }
        controlSnapshot_ = std::move(next);
        if (rebound) {
            C::ControlNotifications::Shared().Publish(
                {io_.Route().guid, controlSnapshot_->topology.revision,
                 controlSnapshot_->sequence, 0, 4, 0});
        }
        return true;
    }
    bool metadata = false;
    std::vector<C::ParameterChanged> events;
    for (const auto& [id, state] : next.state.parameters) {
        const auto old = controlSnapshot_->state.parameters.find(id);
        if (old == controlSnapshot_->state.parameters.end()) {
            metadata = true; // an observation appeared (e.g. post-refresh)
            continue;
        }
        if (old->second.domainOverride != state.domainOverride ||
            old->second.applicable != state.applicable) {
            metadata = true;
        }
        if (old->second.value == state.value) {
            continue;
        }
        C::ParameterChanged event{io_.Route().guid, next.topology.revision, 0, id.value, 0, 0};
        std::visit(
            [&event](const auto& changed) {
                using T = std::decay_t<decltype(changed)>;
                if constexpr (std::is_same_v<T, bool>) {
                    event.kind = 0;
                    event.bits = changed;
                } else if constexpr (std::is_same_v<T, double>) {
                    event.kind = 1;
                    event.bits = std::bit_cast<uint64_t>(changed);
                } else if constexpr (std::is_same_v<T, int64_t>) {
                    event.kind = 2;
                    event.bits = static_cast<uint64_t>(changed);
                }
            },
            state.value);
        events.push_back(event);
    }
    for (const auto& [id, state] : controlSnapshot_->state.parameters) {
        if (next.state.parameters.find(id) == next.state.parameters.end()) {
            // An observation went Unknown (an ambiguous write left its word
            // in doubt): the app must re-pull; the metadata event carries
            // that exactly like an appearing observation.
            metadata = true;
            break;
        }
    }
    if (metadata) {
        events = {{io_.Route().guid, next.topology.revision, 0, 0, 4, 0}};
    }
    for (auto& event : events) {
        event.sequence = next.sequence = C::ControlNotifications::Shared().NextSequence();
    }
    controlSnapshot_ = std::move(next); // exact values before fan-out
    for (const auto& event : events) {
        C::ControlNotifications::Shared().Publish(event);
    }
    return true;
}

// ---------------------------------------------------------------------------
// The bounded control queue (Duet mechanics, verbatim discipline)
// ---------------------------------------------------------------------------

void SPro40D3Protocol::QueueControl(std::function<void(SPro40D3Protocol&)> op,
                                     Completion refuse) {
    if (controlWaiters_.size() >= kMaxControlWaiters) {
        refuse(kIOReturnBusy, AudioModel::ParameterValue{false});
        return;
    }
    controlWaiters_.push_back([weak = weak_from_this(), op = std::move(op),
                               refuse = std::move(refuse)]() mutable {
        const auto owner = weak.lock();
        if (!owner) {
            refuse(kIOReturnAborted, AudioModel::ParameterValue{false});
            return;
        }
        op(*static_cast<SPro40D3Protocol*>(owner.get()));
    });
}

void SPro40D3Protocol::ReleaseControl() {
    controlBusy_ = false;
    while (!controlBusy_ && !controlWaiters_.empty()) {
        auto next = std::move(controlWaiters_.front());
        controlWaiters_.pop_front();
        next();
    }
}

void SPro40D3Protocol::ResetControlQueue() {
    controlBusy_ = false;
    auto waiters = std::move(controlWaiters_);
    controlWaiters_.clear();
    for (auto& waiter : waiters) {
        waiter(); // each answers itself: Aborted via weak, NotReady via op
    }
}

// ---------------------------------------------------------------------------
// The bounded output/knob observation refresh (P05)
// ---------------------------------------------------------------------------

void SPro40D3Protocol::StopControlPoll() {
    if (scheduler_ && controlPoll_ != Scheduling::kInvalidTimerToken) {
        scheduler_->Cancel(controlPoll_);
    }
    controlPoll_ = Scheduling::kInvalidTimerToken;
}

void SPro40D3Protocol::ScheduleControlPoll() {
    StopControlPoll();
    if (!controlsReady_ || scheduler_ == nullptr) return;
    const auto weak = weak_from_this();
    controlPoll_ = scheduler_->ScheduleAfter(500'000'000, [weak] {
        const auto owner = weak.lock();
        if (!owner) return;
        auto* self = static_cast<SPro40D3Protocol*>(owner.get());
        self->controlPoll_ = Scheduling::kInvalidTimerToken;
        self->PollControls();
    });
}

void SPro40D3Protocol::PollControls() {
    if (!controlsReady_) return;
    if (controlBusy_) {
        ScheduleControlPoll(); // a write or refresh owns the bus; retry later
        return;
    }
    auto self = std::static_pointer_cast<SPro40D3Protocol>(weak_from_this().lock());
    if (!self) return;
    controlBusy_ = true;
    const auto epoch = controlsEpoch_;
    BeginControlGroupRead([self, epoch](IOReturn status) {
        if (epoch != self->controlsEpoch_) return; // torn down or re-routed
        if (status == kIOReturnSuccess) {
            if (!self->CommitControls()) {
                // The poll observed something the validator rejects: stop
                // polling rather than loop on a hostile device; the refresh
                // action remains the explicit recovery.
                self->StopControlPoll();
                self->ReleaseControl();
                return;
            }
        }
        self->ReleaseControl();
        self->ScheduleControlPoll();
    });
}

// -- Meter telemetry (P10b) ------------------------------------------------

Audio::Controls::IControlProvider::MeterRefreshStatus
SPro40D3Protocol::RequestMeterRefresh(uint32_t currentRateHz) {
    using Status = Audio::Controls::IControlProvider::MeterRefreshStatus;
    if (currentRateHz == 0) {
        return Status::RefusedNotReady; // no rate context: never invent one
    }
    if (!engine_ || engine_->GetRegistrationState() !=
                       SPro40D3MailboxEngine::RegistrationState::kRegistered) {
        return Status::RefusedNotReady;
    }
    if (meterRefreshPending_ || controlBusy_) {
        // One pending batch; foreground control work wins. The caller's
        // next request (the app's cadence) refreshes.
        return Status::RefusedBusy;
    }
    const auto now = std::chrono::steady_clock::now();
    if (meterLastAcquisition_.time_since_epoch().count() != 0 &&
        now - meterLastAcquisition_ < std::chrono::milliseconds(100)) {
        return Status::RefusedCadenceWindow; // the 10 Hz cap
    }
    BeginMeterRefresh(currentRateHz);
    return Status::Launched;
}

void SPro40D3Protocol::BeginMeterRefresh(uint32_t currentRateHz) {
    auto self = std::static_pointer_cast<SPro40D3Protocol>(shared_from_this());
    meterRefreshPending_ = true;

    constexpr size_t kMixerMeters = 18;
    constexpr size_t kMasterMeters = 2;
    auto raw = std::make_shared<std::array<int32_t, kD3Meters>>();
    auto valid = std::make_shared<std::array<uint8_t, kD3Meters>>();

    const auto mixer = engine_->SendGetMixerMeter(
        currentRateHz,
        [self, raw, valid, currentRateHz, kMixerMeters, kMasterMeters](
            std::expected<std::span<const int32_t>, MailboxError> result) {
            if (!result) {
                // The acquisition failed: keep the last complete batch -
                // a partial never publishes, an old batch beats a lie.
                self->meterRefreshPending_ = false;
                return;
            }

            const size_t n = std::min(result->size(), kMixerMeters);
            for (size_t i = 0; i < n; ++i) {
                (*raw)[i] = (*result)[i];
                (*valid)[i] = ::ASFW::UserClient::Wire::kMeterValueValid;
            }
            const auto master = self->engine_->SendGetMasterMeter(
                currentRateHz,
                [self, raw, valid, kMasterMeters, kMixerMeters](
                    std::expected<std::span<const int32_t>, MailboxError> m) {
                    self->meterRefreshPending_ = false;
                    if (!m) {
                        return; // keep the last complete batch
                    }
                    const size_t mn = std::min(m->size(), kMasterMeters);
                    for (size_t i = 0; i < mn; ++i) {
                        (*raw)[kMixerMeters + i] = (*m)[i];
                        (*valid)[kMixerMeters + i] = ::ASFW::UserClient::Wire::kMeterValueValid;
                    }
                    Audio::Controls::MeterBatch batch;
                    batch.sequence = ++self->meterSequence_;
                    batch.generation = self->io_.Route().generation.value;
                    batch.timestampNs = static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count());
                    batch.count = self->kD3Meters;
                    for (size_t i = 0; i < self->kD3Meters; ++i) {
                        batch.raw[i] = (*raw)[i];
                        batch.valid[i] = (*valid)[i] != 0;
                    }
                    self->meterTelemetry_ = batch;
                    self->meterLastAcquisition_ = std::chrono::steady_clock::now();
                });
            if (!master) {
                self->meterRefreshPending_ = false;
            }
        });
    if (!mixer) {
        meterRefreshPending_ = false; // kBusy/kNotRegistered: refuse cleanly
    }
}

std::optional<Audio::Controls::MeterBatch>
SPro40D3Protocol::CopyMeterTelemetry() const {
    // The cache-copy: the immutable published value, no I/O, no waiting -
    // the external method's contract.
    return meterTelemetry_;
}


} // namespace ASFW::Audio::DICE::Focusrite::SPro40D3
