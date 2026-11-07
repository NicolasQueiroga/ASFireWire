// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// SPro40D3Protocol.hpp - Saffire Pro 40 D3 (TCD3070) native protocol (P04).
//
// Composition: a DICETcatProtocol WRAPPER (the SPro24Dsp delegation pattern:
// every applicable IDeviceProtocol method forwards to TCAT - streaming,
// clock, rates, labels, diagnostics and notification rebind stay TCAT's;
// vendor controls must not alter audio behavior) plus the native D3 control
// provider (the ApogeeDuetProtocol admission/publication mechanics):
//
//   IDeviceProtocol
//     `-> tcat_ (DICETcatProtocol: streaming, clock, geometry)
//     `-> Controls() == this (IControlProvider, read-only at P04)
//           `-> SPro40D3MailboxEngine over SPro40D3TransportPort
//                 (ProtocolRegisterIO on the same bus ports)
//           `-> SPro40D3NotificationEndpoint on the shared
//                 AddressSpaceManager + controller scheduler
//
// OWNERSHIP/QUEUE CONTRACT (plan §4 item 1, P00 baseline rows):
// - Provider state has one serialization domain: the Default/controller
//   queue. Mutable entries (Prepare/SetParameter) run through the bounded
//   one-at-a-time control queue (kMaxControlWaiters; a full queue refuses
//   kIOReturnBusy); every async step retains the protocol (shared_from_this
//   discipline as in the Duet) and re-checks controlsEpoch_ - a route
//   change aborts in-flight work instead of publishing stale state.
// - CopySnapshot never does I/O: it hands out the published immutable
//   snapshot or nothing.
// - Registration lifecycle: Initialize does NOT touch the mailbox (the
//   audio path must come up independently of vendor controls - a controls
//   failure never costs the audio device). RebindNotifications is the
//   explicit hook that reads the DCP descriptor from the device, parses
//   it, activates the notification endpoint and claims the mailbox
//   (claim-from-unowned-only CAS); UpdateRuntimeContext on a changed route
//   revokes ingress, tears the claim down and invalidates published state.
// - READ-ONLY at P04: SetParameter answers kIOReturnUnsupported except
//   the single RefreshReadableControls action (an app-only recovery
//   re-read of the output group); SetRoute answers kIOReturnUnsupported
//   until the write path (P08). No defaults are ever written to populate
//   a screen.

#pragma once

#include "SPro40D3ControlModel.hpp"
#include "SPro40D3MailboxEngine.hpp"
#include "SPro40D3NotificationEndpoint.hpp"
#include "SPro40D3TransportPort.hpp"
#include "../TCAT/DICETcatProtocol.hpp"
#include "../../IDeviceProtocol.hpp"
#include "../../../Controls/IControlProvider.hpp"
#include <deque>
#include <functional>
#include <memory>
#include <optional>

namespace ASFW::Protocols::SBP2 {
class AddressSpaceManager;
}

/// Test inspection/delivery peer (global scope; befriended by the class
/// below). Defined in SPro40D3ProtocolTests.cpp; mirrors the
/// DICETcatProtocolTestPeer pattern.
struct SPro40D3ProtocolTestPeer;

namespace ASFW::Audio::DICE::Focusrite::SPro40D3 {

class SPro40D3Protocol final : public Audio::IDeviceProtocol,
                               public Audio::Controls::IControlProvider {
public:
    /// `space` is the SHARED AddressSpaceManager (collision-safe
    /// reservation for the notification range). Null is a construction
    /// failure of the control surface only: TCAT streaming still works,
    /// Prepare refuses with kIOReturnNotReady, and the fact is logged -
    /// never a silent placeholder that always denies activation.
    ///
    /// `transport` is a construction seam for the D3 host tests: the
    /// simulated device implements ISPro40D3Transport directly, while the
    /// native SPro40D3TransportPort needs the live FireWire stack behind
    /// ProtocolRegisterIO. Production passes nothing and gets the native
    /// port; tests hand over the simulated transport so protocol behavior
    /// is exercised against real mailbox semantics, not a parallel stub.
    SPro40D3Protocol(Protocols::Ports::FireWireBusOps& busOps,
                     Protocols::Ports::FireWireBusInfo& busInfo,
                     Discovery::DeviceRegistry& routeRegistry,
                     const Discovery::DeviceRouteToken& route,
                     ::ASFW::IRM::IRMClient* irmClient,
                     DiceWaitClock& waitClock,
                     DiceNotificationRouter* diceNotifications,
                     Scheduling::ITimerScheduler* preparationScheduler,
                     Protocols::SBP2::AddressSpaceManager* space,
                     std::unique_ptr<class ISPro40D3Transport> transport = nullptr,
                     SPro40D3MailboxEngine::Options engineOptions = {});
    ~SPro40D3Protocol() override;

    SPro40D3Protocol(const SPro40D3Protocol&) = delete;
    SPro40D3Protocol& operator=(const SPro40D3Protocol&) = delete;

    // -- IDeviceProtocol (TCAT delegation; the SPro24Dsp surface) ---------

    IOReturn Initialize() override;
    IOReturn Shutdown() override;
    [[nodiscard]] const char* GetName() const override {
        return "Focusrite Saffire Pro 40 (D3/TCD3070)";
    }
    [[nodiscard]] bool HasDsp() const override { return false; }
    [[nodiscard]] bool HasMixer() const override { return true; } // 18x16 monitor mixer
    [[nodiscard]] FamilyDriver* AsFamilyDriver() noexcept override {
        return tcat_.AsFamilyDriver();
    }
    [[nodiscard]] bool GetRuntimeAudioStreamCaps(AudioStreamRuntimeCaps& outCaps) const override {
        return tcat_.GetRuntimeAudioStreamCaps(outCaps);
    }
    [[nodiscard]] std::shared_ptr<const std::vector<Runtime::RateFormation>> RateFormations()
        const override {
        return tcat_.RateFormations();
    }
    void ReadRateObservation(std::function<void(IOReturn, RateHardwareObservation)> cb) override {
        tcat_.ReadRateObservation(std::move(cb));
    }
    void EnsureRuntimeStreamGeometry(std::function<void(IOReturn)> cb) override {
        tcat_.EnsureRuntimeStreamGeometry(std::move(cb));
    }
    void CancelPendingPreparation() override { tcat_.CancelPendingPreparation(); }
    [[nodiscard]] bool GetChannelLabels(std::vector<std::string>& inNames,
                                         std::vector<std::string>& outNames) const override {
        return tcat_.GetChannelLabels(inNames, outNames);
    }
    [[nodiscard]] bool CopyDiceReceiverLock(Diagnostics::DiceLockSnapshot& out) const override {
        return tcat_.CopyDiceReceiverLock(out);
    }
    [[nodiscard]] IOReturn RequestDiceReceiverDump(bool fifoEvents) override {
        return tcat_.RequestDiceReceiverDump(fifoEvents);
    }
    [[nodiscard]] bool CopyDiceReceiverDump(Diagnostics::DiceReceiverDumpSnapshot& out) const override {
        return tcat_.CopyDiceReceiverDump(out);
    }
    [[nodiscard]] IOReturn StopDuplex() override { return tcat_.StopDuplex(); }
    void UpdateRuntimeContext(const Discovery::DeviceRouteToken& route,
                              std::shared_ptr<ASFW::AVC::IAvcUnit> avcUnit) override;
    void RebindNotifications(VoidCallback callback) override;

    // -- IControlProvider (read-only at P04; writes P05) ----------------

    [[nodiscard]] Audio::Controls::IControlProvider* Controls() noexcept override {
        return this;
    }
    void Prepare(std::function<void(IOReturn)> done) override;
    [[nodiscard]] std::optional<Audio::Controls::ControlSnapshot> CopySnapshot() const override;
    void SetParameter(AudioModel::ParameterId id, const AudioModel::ParameterValue& value,
                      Completion done) override;
    void SetRoute(AudioModel::NodeId node, AudioModel::PortId output,
                  std::optional<AudioModel::PortId> input, Completion done) override;

    // -- Meter telemetry (P10b): bounded, honest, cache-copy --------------
    // The D3's batch: 18 mixer-source meters then 2 master meters, in the
    // device's own raw scale, riding the interface's generic MeterBatch.
    /// Ask for an acquisition. Coalesced: at most one in flight, at most
    /// one per 100 ms window (the 10 Hz cap), and NEVER while a foreground
    /// control op runs (control work wins; the next request refreshes).
    /// This is the app's visibility-driven hook - nothing polls on its own.
    [[nodiscard]] Audio::Controls::IControlProvider::MeterRefreshStatus RequestMeterRefresh(uint32_t currentRateHz) override;

    /// The last complete batch (cache-copy). Never does I/O, never waits.
    /// nullopt before the first complete acquisition lands.
    [[nodiscard]] std::optional<Audio::Controls::MeterBatch> CopyMeterTelemetry() const override;

private:
    friend struct ::SPro40D3ProtocolTestPeer;

    /// Sequential state for the one-at-a-time readable-group read chain.
    struct ControlGroupState;
    /// Sequential state for the one-at-a-time write chain (P05): fresh
    /// read, merge-plan, set+notice, get-back.
    struct ControlWriteState;

    void BeginControlGroupRead(std::function<void(IOReturn)> done);
    void ControlGroupAdvance(const std::shared_ptr<ControlGroupState>& state);
    struct RoutingWriteState;
    struct MixerCellWriteState;
    struct MixerInitState;
    /// One full low+high table-pair write (P06): the shared tail of the
    /// apply action, the source-select parameters and SetRoute.
    void BeginRoutingProgramWrite(RouterOutSrc outSrc, RouterMixerSrc mixerSrc,
                                  RouterMeterSrc meterSrc,
                                  AudioModel::ParameterValue report, Completion done);
    void BeginMixerCellWrite(AudioModel::ParameterId id, double value, Completion done);
    void BeginMixerInit(Completion done);
    void BeginControlWrite(AudioModel::ParameterId id, const AudioModel::ParameterValue& value,
                           uint32_t position, Completion done);
    void StoreReadableWord(size_t index, uint32_t word);
    void MarkWordUnknown(uint32_t position);
    [[nodiscard]] bool CommitControls();
    void QueueControl(std::function<void(SPro40D3Protocol&)> op, Completion refuse);
    void ReleaseControl();
    void ResetControlQueue();
    /// Bounded output/knob observation refresh (P05): the front panel
    /// changes levels behind the host's back. 500 ms, non-overlapping
    /// (one-at-a-time with writes via the shared busy flag), re-armed
    /// after every poll. Never at audio rate; never blocks a write.
    void ScheduleControlPoll();
    void StopControlPoll();
    void PollControls();

    TCAT::DICETcatProtocol tcat_;
    Protocols::SBP2::AddressSpaceManager* space_{nullptr};
    Scheduling::ITimerScheduler* scheduler_{nullptr};
    Protocols::Ports::ProtocolRegisterIO io_;
    std::unique_ptr<ISPro40D3Transport> transport_;
    std::shared_ptr<SPro40D3MailboxEngine> engine_;
    SPro40D3MailboxEngine::Options engineOptions_;
    std::unique_ptr<SPro40D3NotificationEndpoint> endpoint_;
    D3ControlStore store_{};
    D3RoutingState routingState_{};

    std::optional<Audio::Controls::ControlSnapshot> controlSnapshot_;
    bool controlsReady_{false};
    bool controlBusy_{false};
    uint64_t controlsEpoch_{0};
    static constexpr size_t kMaxControlWaiters = 16;
    std::deque<std::function<void()>> controlWaiters_;
    Scheduling::TimerToken controlPoll_{Scheduling::kInvalidTimerToken};

    // -- Meter telemetry state (P10b) ---------------------------------------
    /// The publication discipline mirrors controlSnapshot_: the batch is
    /// published from the pump (the acquisition chain's completion) and
    /// copied out as an immutable value; the copy never does I/O.
    static constexpr size_t kD3Meters = 20; ///< 18 mixer sources + 2 masters.
    std::optional<Audio::Controls::MeterBatch> meterTelemetry_{};
    uint32_t meterSequence_{0};
    bool meterRefreshPending_{false};
    std::chrono::steady_clock::time_point meterLastAcquisition_{};
    void BeginMeterRefresh(uint32_t currentRateHz);
};

} // namespace ASFW::Audio::DICE::Focusrite::SPro40D3
