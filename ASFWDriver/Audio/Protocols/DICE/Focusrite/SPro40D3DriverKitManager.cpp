// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "SPro40D3DriverKitManager.hpp"
#include "../../../../Protocols/SBP2/AddressSpaceManager.hpp"
#ifdef ASFW_HOST_TEST
#include "../../../../DeviceProfiles/Audio/ResolvedDevicePolicy.hpp"
#include <atomic>
#include <limits>
#include <vector>
#endif
#include <utility>

namespace ASFW::Audio::DICE::Focusrite::SPro40D3 {
namespace {
class LockGuard final {
public:
    explicit LockGuard(IOLock* lock) noexcept : lock_(lock) { IOLockLock(lock_); }
    ~LockGuard() { IOLockUnlock(lock_); }
    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;
private:
    IOLock* lock_;
};
}

#ifdef ASFW_HOST_TEST
struct SPro40D3DriverKitManager::SimulatedOperation {
    DiagnosticAuthorization authorization;
    std::function<void(IOReturn)> completion;
    bool terminal{false};
};
#endif

struct SPro40D3DriverKitManager::Runtime {
    explicit Runtime(Discovery::DeviceRegistry& registry) noexcept : registry(&registry) {}
    ~Runtime() { if (lock) IOLockFree(lock); }
    IOLock* lock{IOLockAlloc()};
    // Protected by lock; Stop nulls the borrowed pointer before returning.
    Discovery::DeviceRegistry* registry;
    bool stopped{false};
    // Constant-size metadata fence, independent of permits/session lifetime.
    // All routes allocated through this epoch remain diagnostic-stale after
    // quiescence; strictly newer current routes can refresh metadata without I/O.
    uint64_t revokedRouteEpoch{0};
    DriverKitDiagnosticSnapshot cached{};
#ifdef ASFW_HOST_TEST
    std::optional<SimulatedPolicy> policy;
    std::optional<Discovery::DeviceRouteToken> retiredRoute;
    std::vector<std::shared_ptr<SimulatedOperation>> pending;

    [[nodiscard]] bool AdmitsRoute(Discovery::DeviceRouteToken route) const noexcept {
        if (!registry || !route || !registry->IsCurrent(route)) return false;
        const auto record = registry->SnapshotByGuid(route.guid);
        if (!record || record->identity.observedGuid != route.guid) return false;
        const auto* resolved = DeviceProfiles::Audio::CurrentAudioPolicy(*record);
        using namespace DeviceProfiles::Audio;
        return resolved && resolved->route == route &&
               resolved->plan.exactVariantId ==
                   static_cast<uint32_t>(DeviceDefinitionId::FocusriteSPro40Tcd3070) &&
               resolved->plan.family == AudioFamilyProviderId::DICE &&
               resolved->plan.unitVersion == 1;
    }
    [[nodiscard]] bool Admits(const DiagnosticAuthorization& authorization) const noexcept {
        return !stopped && policy && authorization.scope == policy->scope &&
               authorization.route == policy->route && authorization.sessionToken != 0 &&
               authorization.sessionToken == cached.sessionToken &&
               authorization.runtimeGeneration == cached.runtimeGeneration &&
               AdmitsRoute(authorization.route);
    }
#endif
};

SPro40D3DriverKitManager::SPro40D3DriverKitManager(
    Discovery::DeviceRegistry& registry, Protocols::SBP2::AddressSpaceManager& addresses,
    DiceWaitClock& clock) noexcept : runtime_(std::make_shared<Runtime>(registry)) {
    if (!runtime_->lock) {
        runtime_->cached.lastError = runtime_->cached.lastOperationStatus =
            static_cast<uint32_t>(kIOReturnNoMemory);
        return;
    }
    // No queue, mailbox engine, transport or session is constructed. In
    // particular the historical experimental macro cannot make this path live.
    hub_ = std::make_shared<SPro40D3NotificationHub>(
        registry, clock, SPro40D3NotificationHub::Region{.begin = 0, .endExclusive = 0});
    auto endpoint = SPro40D3NotificationEndpoint::Create(addresses, hub_);
    if (!endpoint) {
        runtime_->cached.lastError = runtime_->cached.lastOperationStatus =
            static_cast<uint32_t>(endpoint.error());
        return;
    }
    endpoint_ = std::move(*endpoint);
    runtime_->cached.notificationAddress = endpoint_->address();
    runtime_->cached.notificationState = 1;
    runtime_->cached.reservationState = 1;
}

SPro40D3DriverKitManager::~SPro40D3DriverKitManager() { Stop(); }

IOReturn SPro40D3DriverKitManager::RequestActivation(DiagnosticScope) noexcept {
    if (runtime_->lock) {
        const LockGuard guard(runtime_->lock);
        runtime_->cached.lastError = runtime_->cached.lastOperationStatus =
            static_cast<uint32_t>(kIOReturnNotPermitted);
    }
    return kIOReturnNotPermitted;
}

DriverKitDiagnosticSnapshot
SPro40D3DriverKitManager::CopyDiagnostics(uint64_t guid) const noexcept {
    if (!runtime_->lock) {
        auto snapshot = runtime_->cached;
        snapshot.guid = guid;
        return snapshot;
    }
    const LockGuard guard(runtime_->lock);
    auto snapshot = runtime_->cached;
    snapshot.guid = guid;
    if (runtime_->registry) {
        if (const auto record = runtime_->registry->SnapshotByGuid(guid)) {
            snapshot.vendorId = record->vendorId;
            snapshot.modelId = record->modelId;
            const Discovery::DeviceRouteToken route{
                record->guid, record->deviceIncarnation, record->routeEpoch,
                record->gen, record->nodeId};
            // A newer epoch is sufficient even on the first-ever query for
            // this GUID. Metadata queries never modify or restore authorization.
            const bool current = route.routeEpoch > runtime_->revokedRouteEpoch &&
                                 runtime_->registry->IsCurrent(route);
            snapshot.identificationState = current ? 1U : 2U;
            snapshot.routeValid = current ? 1U : 0U;
            snapshot.generation = current ? record->gen.value : 0;
            snapshot.nodeId = current ? record->nodeId : Discovery::kInvalidNodeId;
            // Cached identity survives reset/removal, but a stale generation/node
            // must never masquerade as a currently usable route.
        }
    }
#ifdef ASFW_HOST_TEST
    if (!runtime_->policy || runtime_->policy->route.guid != guid) snapshot.sessionToken = 0;
#endif
    return snapshot;
}

void SPro40D3DriverKitManager::OnBusReset() noexcept { Quiesce(false); }
void SPro40D3DriverKitManager::OnDeviceRemoved(uint64_t guid) noexcept { Quiesce(false, guid); }
void SPro40D3DriverKitManager::Stop() noexcept { Quiesce(true); }

void SPro40D3DriverKitManager::Quiesce(bool stopping, uint64_t removedGuid) noexcept {
    if (!runtime_->lock) return;
    // These locals retain everything needed by the unlocked epilogue. No member
    // accesses occur after callbacks, which may reenter any lifecycle method.
    const auto runtime = runtime_;
    std::shared_ptr<SPro40D3NotificationEndpoint> endpoint;
    std::shared_ptr<SPro40D3NotificationHub> retiredHub;
#ifdef ASFW_HOST_TEST
    std::vector<std::shared_ptr<SimulatedOperation>> cancelled;
#else
    (void)removedGuid;
#endif
    {
        const LockGuard guard(runtime->lock);
        if (runtime->stopped) return;
#ifdef ASFW_HOST_TEST
        if (!stopping && removedGuid && runtime->policy &&
            runtime->policy->route.guid != removedGuid) return;
        if (runtime->policy) runtime->retiredRoute = runtime->policy->route;
        runtime->policy.reset();
        cancelled.swap(runtime->pending);
        for (const auto& operation : cancelled) operation->terminal = true;
        runtime->cached.cancelledOperations += cancelled.size();
#endif
        if (!stopping && runtime->registry) {
            // Capture before unlocked endpoint/callback work. Even GUIDs never
            // queried before this event have an objective freshness baseline.
            runtime->revokedRouteEpoch = runtime->registry->RouteEpochHighWatermark();
        }
        runtime->cached.pendingOperations = 0;
        runtime->cached.sessionToken = 0;
        ++runtime->cached.runtimeGeneration;
        runtime->cached.lastError = runtime->cached.lastOperationStatus =
            static_cast<uint32_t>(kIOReturnAborted);
        runtime->cached.notificationState = 0;
        if (runtime->cached.reservationState) runtime->cached.reservationState = 2;
        if (stopping) {
            runtime->stopped = true;
            runtime->registry = nullptr;
            runtime->cached.notificationAddress = 0;
            endpoint = std::move(endpoint_);
            retiredHub = std::move(hub_);
        } else {
            endpoint = endpoint_;
        }
    }
    // No manager lock across endpoint/hub work, destruction or callbacks. No
    // dispatch at all: safe even when lifecycle entry originates in a callback.
    if (endpoint) endpoint->Release();
#ifdef ASFW_HOST_TEST
    for (const auto& operation : cancelled) {
        std::function<void(IOReturn)> completion;
        completion.swap(operation->completion);
        if (completion) completion(kIOReturnAborted);
    }
#endif
}

#ifdef ASFW_HOST_TEST
std::expected<DiagnosticAuthorization, IOReturn>
SPro40D3DriverKitManager::AuthorizeSimulationForTesting(SimulatedPolicy policy) {
    if (!runtime_->lock) return std::unexpected(kIOReturnNoMemory);
    const LockGuard guard(runtime_->lock);
    if (runtime_->stopped || !runtime_->AdmitsRoute(policy.route) ||
        (runtime_->retiredRoute && *runtime_->retiredRoute == policy.route) ||
        static_cast<unsigned>(policy.scope) > static_cast<unsigned>(DiagnosticScope::MixerRoutingWrite))
        return std::unexpected(kIOReturnNotPermitted);
    if (runtime_->policy) return std::unexpected(kIOReturnBusy);
    // Globally unique, never wrapped: a permit from a destroyed manager cannot
    // become valid on another manager, even for the same route and epoch.
    static std::atomic<uint64_t> nextToken{1};
    uint64_t token = nextToken.load(std::memory_order_relaxed);
    do {
        if (token == std::numeric_limits<uint64_t>::max())
            return std::unexpected(kIOReturnNoResources);
    } while (!nextToken.compare_exchange_weak(token, token + 1, std::memory_order_relaxed));
    runtime_->policy = policy;
    runtime_->cached.sessionToken = token;
    return DiagnosticAuthorization{policy.scope, policy.route, token,
                                   runtime_->cached.runtimeGeneration};
}

std::expected<SPro40D3DriverKitManager::SimulatedCompletion, IOReturn>
SPro40D3DriverKitManager::BeginSimulationForTesting(
    DiagnosticAuthorization authorization, std::function<void(IOReturn)> completion) {
    if (!runtime_->lock) return std::unexpected(kIOReturnNoMemory);
    // Construct callback-owning state outside the lock; no user capture may be
    // destroyed under a runtime lock, even on a rejected request.
    auto operation = std::make_shared<SimulatedOperation>(
        SimulatedOperation{authorization, std::move(completion), false});
    {
        const LockGuard guard(runtime_->lock);
        if (!runtime_->Admits(authorization)) return std::unexpected(kIOReturnNotPermitted);
        runtime_->pending.push_back(operation);
        runtime_->cached.pendingOperations = runtime_->pending.size();
    }
    SimulatedCompletion ticket;
    ticket.runtime_ = runtime_;
    ticket.operation_ = std::move(operation);
    return ticket;
}

bool SPro40D3DriverKitManager::SimulatedCompletion::Complete(IOReturn status) const {
    if (!runtime_ || !operation_ || !runtime_->lock) return false;
    // Retain state across user callback. A copied ticket is safe to complete
    // concurrently, and survives manager destruction without borrowing it.
    const auto runtime = runtime_;
    const auto operation = operation_;
    std::function<void(IOReturn)> completion;
    {
        const LockGuard guard(runtime->lock);
        if (operation->terminal) return false;
        if (!runtime->Admits(operation->authorization)) status = kIOReturnAborted;
        operation->terminal = true;
        completion.swap(operation->completion);
        std::erase(runtime->pending, operation);
        runtime->cached.pendingOperations = runtime->pending.size();
        if (status == kIOReturnAborted) ++runtime->cached.cancelledOperations;
        if (status == kIOReturnTimeout) ++runtime->cached.timedOutOperations;
        runtime->cached.lastError = runtime->cached.lastOperationStatus = static_cast<uint32_t>(status);
    }
    if (completion) completion(status);
    return true;
}
#endif

} // namespace ASFW::Audio::DICE::Focusrite::SPro40D3
