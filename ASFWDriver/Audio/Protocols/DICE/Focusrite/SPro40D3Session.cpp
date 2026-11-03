// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
#include "SPro40D3Session.hpp"

#include "../../../../DeviceProfiles/Audio/ResolvedDevicePolicy.hpp"

#include <utility>

namespace ASFW::Audio::DICE::Focusrite::SPro40D3 {

SPro40D3Session::SPro40D3Session(Async::IFireWireBusOps& busOps,
                               Async::IFireWireBusInfo& busInfo,
                               Discovery::DeviceRegistry& registry, DiceWaitClock& clock,
                               SPro40D3NotificationHub& hub) noexcept
    : busOps_(busOps), busInfo_(busInfo), registry_(registry), clock_(clock), hub_(hub) {}

SPro40D3Session::~SPro40D3Session() {
    // Destruction inside a callback is prohibited by the lifetime contract.
    DeviceRemoved();
}

bool SPro40D3Session::Admits(Discovery::DeviceRouteToken route) const noexcept {
    if (!registry_.IsCurrent(route)) return false;
    const auto record = registry_.SnapshotByGuid(route.guid);
    if (!record || record->identity.observedGuid != route.guid) return false;
    const auto* policy = DeviceProfiles::Audio::CurrentAudioPolicy(*record);
    if (!policy || policy->route != route) return false;
    using namespace DeviceProfiles::Audio;
    return policy->plan.exactVariantId ==
               static_cast<uint32_t>(DeviceDefinitionId::FocusriteSPro40Tcd3070) &&
           policy->plan.family == AudioFamilyProviderId::DICE &&
           policy->plan.unitVersion == 1;
}

bool SPro40D3Session::IsReady() const noexcept {
    return state_ == State::Ready && route_ && Admits(*route_);
}

bool SPro40D3Session::ValidateReady() {
    if (state_ != State::Ready) return false;
    if (route_ && Admits(*route_)) return true;
    lastError_ = MailboxError::kBusReset;
    Quiesce(State::NeedsRebind);
    return false;
}

void SPro40D3Session::Quiesce(State state) {
    // Close admission and revoke ingress BEFORE callbacks. No ownership-release
    // transaction is verified, so never call UnregisterNotificationAddress().
    if (state_ != State::Removed) state_ = state;
    if (lease_) {
        hub_.Release(*lease_);
        lease_.reset();
    }
    if (engine_ && !resetDelivered_) resetPending_ = true;
    if (engineDepth_ == 0) FinishEngineCall();
}

void SPro40D3Session::FinishEngineCall() {
    // Never delete/reset an engine on its own callback stack. OnBusReset is
    // also guarded: cancellation callbacks can reenter lifecycle APIs safely.
    if (engineDepth_ != 0 || !resetPending_) return;
    resetPending_ = false;
    resetDelivered_ = true;
    ++engineDepth_;
    engine_->OnBusReset();
    --engineDepth_;
}

std::expected<void, MailboxError>
SPro40D3Session::StartForHostValidation(Discovery::DeviceRouteToken route) {
    if (state_ == State::Removed) return std::unexpected(MailboxError::kNotRegistered);
    if (engineDepth_ != 0) return std::unexpected(MailboxError::kBusy);
    if (state_ == State::Failed)
        return std::unexpected(lastError_.value_or(MailboxError::kNotRegistered));
    if (state_ == State::Ready && (!route_ || *route_ != route)) {
        lastError_ = MailboxError::kBusReset;
        Quiesce(State::NeedsRebind);
        // Cancellation callbacks can stop or permanently remove this session.
        // An outer start must not override their lifecycle decision.
        if (state_ != State::NeedsRebind)
            return std::unexpected(MailboxError::kNotRegistered);
    }
    if ((state_ == State::NeedsRebind && route_ && *route_ == route) ||
        !Admits(route) ||
        (identityRoute_ && (identityRoute_->guid != route.guid ||
                           identityRoute_->deviceIncarnation != route.deviceIncarnation))) {
        lastError_ = MailboxError::kNotRegistered;
        Quiesce(State::NeedsRebind);
        return std::unexpected(*lastError_);
    }

    if (state_ != State::Ready) {
        // Old engine has been quiesced and its callback stack has unwound.
        engine_.reset();
        transport_.reset();
        route_ = route;
        if (!identityRoute_) identityRoute_ = route;
        foreignOwner_ = 0;
        transport_ = std::make_unique<SPro40D3Transport>(busOps_, busInfo_, registry_, route, clock_);
        const auto descriptor = ReadDcpDescriptor(*transport_);
        if (!descriptor || descriptor->version != 2 || descriptor->maxRequestSize != 1024 ||
            descriptor->responseAreaSize != 400 ||
            descriptor->writeAddress != kDcpBaseAddress + 0x1C0 ||
            descriptor->readAddress != kDcpBaseAddress + 0x30) {
            lastError_ = MailboxError::kBadDescriptor;
            Quiesce(State::Failed);
            return std::unexpected(*lastError_);
        }
        // Receiver must exist before ownership can route notifications to us.
        const auto lease = hub_.Allocate(route);
        if (!lease) {
            lastError_ = MailboxError::kNotRegistered;
            Quiesce(State::Failed);
            return std::unexpected(*lastError_);
        }
        lease_ = *lease;
        engine_ = std::make_unique<SPro40D3MailboxEngine>(*transport_, *descriptor);
        resetDelivered_ = false;
    }

    ++engineDepth_;
    const auto registered = engine_->RegisterNotificationAddress(lease_->offset);
    --engineDepth_;
    if (!registered) {
        // kBusy is only acceptance failure, not ownership loss.
        if (registered.error() == MailboxError::kBusy) return registered;
        foreignOwner_ = engine_->ForeignOwner();
        lastError_ = registered.error();
        Quiesce(State::Failed);
        return registered;
    }
    if (!Admits(route)) {
        lastError_ = MailboxError::kBusReset;
        Quiesce(State::NeedsRebind);
        return std::unexpected(*lastError_);
    }
    state_ = State::Ready;
    lastError_.reset();
    return {};
}

void SPro40D3Session::OnBusReset() {
    lastError_ = MailboxError::kBusReset;
    Quiesce(State::NeedsRebind);
}

void SPro40D3Session::Stop() { Quiesce(State::Disabled); }
void SPro40D3Session::DeviceRemoved() { Quiesce(State::Removed); }

std::expected<void, MailboxError>
SPro40D3Session::ReadControl32ForValidation(uint32_t position, ControlCallback completion) {
    if (!ValidateReady()) return std::unexpected(MailboxError::kNotRegistered);
    if (engineDepth_ != 0) return std::unexpected(MailboxError::kBusy);
    ++engineDepth_;
    auto accepted = engine_->SendGetControl32(position, clock_.NowMs(),
        [this, completion = std::move(completion)](auto result) {
            if (!result) lastError_ = result.error();
            if (completion) completion(std::move(result));
        });
    --engineDepth_;
    FinishEngineCall();
    return accepted;
}

void SPro40D3Session::ServiceNotifications() {
    if (engineDepth_ != 0 || !ValidateReady()) return;
    // Time NOW wins over even a previously queued, timely notification. This
    // bounds a stalled control queue without trusting a stale response window.
    ++engineDepth_;
    engine_->Poll(clock_.NowMs());
    --engineDepth_;
    FinishEngineCall();
    for (size_t n = 0; n < SPro40D3NotificationHub::kQueueCapacity; ++n) {
        if (!ValidateReady()) return;
        const auto notification = hub_.Take(*lease_);
        if (!notification) return;
        ++engineDepth_;
        engine_->Poll(clock_.NowMs());
        --engineDepth_;
        FinishEngineCall();
        if (!ValidateReady()) return;
        ++engineDepth_;
        engine_->OnNotification(notification->frame, notification->receivedMs);
        --engineDepth_;
        FinishEngineCall();
    }
}

std::optional<AudioModel::StateOrigin>
SPro40D3Session::ToStateOrigin(ValueProvenance provenance) noexcept {
    switch (provenance) {
    case ValueProvenance::Observed: return AudioModel::StateOrigin::Observed;
    case ValueProvenance::Cached: return AudioModel::StateOrigin::Cached;
    case ValueProvenance::Unknown: return std::nullopt;
    }
    return std::nullopt;
}

void SPro40D3Session::Prepare(std::function<void(IOReturn)> done) {
    if (done) done(IsReady() ? kIOReturnUnsupported : kIOReturnNotReady);
}
std::optional<Controls::ControlSnapshot> SPro40D3Session::CopySnapshot() const {
    return std::nullopt;
}
void SPro40D3Session::SetParameter(AudioModel::ParameterId,
                                  const AudioModel::ParameterValue&, Completion done) {
    if (done) done(kIOReturnUnsupported, false);
}
void SPro40D3Session::SetRoute(AudioModel::NodeId, AudioModel::PortId,
                              std::optional<AudioModel::PortId>, Completion done) {
    if (done) done(kIOReturnUnsupported, false);
}

} // namespace ASFW::Audio::DICE::Focusrite::SPro40D3
