// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
#pragma once

#include "SPro40D3NotificationHub.hpp"
#include "SPro40D3Transport.hpp"
#include "../../../Controls/IControlProvider.hpp"

#include <memory>
#include <optional>

namespace ASFW::Audio::DICE::Focusrite::SPro40D3 {

/// Explicit, host-validation-only coordinator. Construction and provider APIs
/// never register or perform bus IO. Not wired into any production factory.
///
/// ALL session methods, callbacks and destruction belong to ONE serialized
/// control queue (not the bus Default/completion queue). Only the hub accepts
/// concurrent bus ingress. Dependencies outlive this object. Callbacks may
/// Stop/OnBusReset/DeviceRemoved, but must not throw or destroy this session;
/// retain the session until the outer API call returns. Reentrant Start/read/
/// Service calls are rejected/no-op, never nested engine entries.
class SPro40D3Session final : public Controls::IControlProvider {
public:
    enum class State : uint8_t { Disabled, Ready, NeedsRebind, Failed, Removed };
    using ControlCallback = SPro40D3MailboxEngine::ControlCallback;

    SPro40D3Session(Async::IFireWireBusOps& busOps,
                   Async::IFireWireBusInfo& busInfo,
                   Discovery::DeviceRegistry& registry, DiceWaitClock& clock,
                   SPro40D3NotificationHub& hub) noexcept;
    ~SPro40D3Session() override;
    SPro40D3Session(const SPro40D3Session&) = delete;
    SPro40D3Session& operator=(const SPro40D3Session&) = delete;

    /// Explicit opt-in. After reset caller must supply a newly discovered route.
    /// A failed attempt is latched: Stop or a real reset is required before an
    /// explicit retry. Duplicate Ready start revalidates the SAME engine's owner;
    /// never reconstructs it to reclaim a lost mailbox.
    [[nodiscard]] std::expected<void, MailboxError>
    StartForHostValidation(Discovery::DeviceRouteToken route);
    void OnBusReset();
    void Stop();
    void DeviceRemoved();
    void ServiceNotifications();
    [[nodiscard]] std::expected<void, MailboxError>
    ReadControl32ForValidation(uint32_t position, ControlCallback completion);

    [[nodiscard]] State GetState() const noexcept { return state_; }
    [[nodiscard]] bool IsReady() const noexcept;
    [[nodiscard]] std::optional<MailboxError> LastError() const noexcept { return lastError_; }
    [[nodiscard]] uint64_t ForeignOwner() const noexcept { return foreignOwner_; }
    [[nodiscard]] static std::optional<AudioModel::StateOrigin>
    ToStateOrigin(ValueProvenance provenance) noexcept;

    void Prepare(std::function<void(IOReturn)> done) override;
    [[nodiscard]] std::optional<Controls::ControlSnapshot> CopySnapshot() const override;
    void SetParameter(AudioModel::ParameterId, const AudioModel::ParameterValue&,
                      Completion done) override;
    void SetRoute(AudioModel::NodeId, AudioModel::PortId,
                  std::optional<AudioModel::PortId>, Completion done) override;

private:
    [[nodiscard]] bool Admits(Discovery::DeviceRouteToken route) const noexcept;
    [[nodiscard]] bool ValidateReady();
    void Quiesce(State state);
    void FinishEngineCall();

    Async::IFireWireBusOps& busOps_;
    Async::IFireWireBusInfo& busInfo_;
    Discovery::DeviceRegistry& registry_;
    DiceWaitClock& clock_;
    SPro40D3NotificationHub& hub_;
    State state_{State::Disabled};
    std::optional<MailboxError> lastError_;
    uint64_t foreignOwner_{0};
    std::optional<Discovery::DeviceRouteToken> route_;
    std::optional<Discovery::DeviceRouteToken> identityRoute_;
    std::optional<SPro40D3NotificationHub::Lease> lease_;
    // Declaration order: engine is destroyed before its borrowed transport.
    std::unique_ptr<SPro40D3Transport> transport_;
    std::unique_ptr<SPro40D3MailboxEngine> engine_;
    unsigned engineDepth_{0};
    bool resetPending_{false};
    bool resetDelivered_{false};
};

} // namespace ASFW::Audio::DICE::Focusrite::SPro40D3
