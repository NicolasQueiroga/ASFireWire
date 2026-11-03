// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "Audio/Protocols/DICE/Focusrite/SPro40D3DriverKitManager.hpp"
#include "DeviceProfiles/Audio/AudioDeviceIds.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <latch>
#include <thread>

namespace {
namespace D3 = ASFW::Audio::DICE::Focusrite::SPro40D3;
namespace Discovery = ASFW::Discovery;
namespace Profiles = ASFW::DeviceProfiles::Audio;
using AddressManager = ASFW::Protocols::SBP2::AddressSpaceManager;
using DriverKitManager = D3::SPro40D3DriverKitManager;
using Scope = D3::DiagnosticScope;

class Clock final : public ASFW::Audio::DICE::DiceWaitClock {
public:
    void SleepMs(uint32_t milliseconds) noexcept override { nowMs += milliseconds; }
    void Backoff(uint32_t) noexcept override {}
    [[nodiscard]] uint64_t NowMs() const noexcept override { return nowMs; }
private:
    uint64_t nowMs{0};
};

class SPro40D3DriverKitManagerTests : public ::testing::Test {
protected:
    void SetUp() override {
        rom.bib.guid = 0x00130E0404C01234ULL;
        rom.gen = ASFW::FW::Generation{19};
        rom.nodeId = 7;
        rom.rootDirMinimal = {
            {.key = Discovery::CfgKey::VendorId, .value = Profiles::kFocusriteVendorId},
            {.key = Discovery::CfgKey::ModelId, .value = Profiles::kSPro40Tcd3070ModelId}};
        Discovery::UnitDirectory unit{};
        unit.offsetQuadlets = 5;
        unit.unitSpecId = Profiles::kFocusriteVendorId;
        unit.unitSwVersion = 1;
        unit.modelId = Profiles::kSPro40Tcd3070ModelId;
        rom.unitDirectories.push_back(unit);
        Discover();
        manager = std::make_unique<DriverKitManager>(registry, addresses, clock);
    }
    void Discover() {
        (void)registry.UpsertFromROM(rom, Discovery::LinkPolicy{});
        route = registry.CurrentRoute(rom.bib.guid).value_or(Discovery::DeviceRouteToken{});
    }
    D3::DiagnosticAuthorization Authorize(Scope scope = Scope::ReadOnlyMailbox) {
        auto result = manager->AuthorizeSimulationForTesting({route, scope});
        EXPECT_TRUE(result.has_value());
        return result.value_or(D3::DiagnosticAuthorization{});
    }
    auto Snapshot() { return manager->CopyDiagnostics(rom.bib.guid); }
    Discovery::DeviceRegistry registry;
    AddressManager addresses{nullptr};
    Clock clock;
    Discovery::ConfigROM rom{};
    Discovery::DeviceRouteToken route{};
    std::unique_ptr<DriverKitManager> manager;
};

TEST_F(SPro40D3DriverKitManagerTests, StartsDormantAndDeniesEveryActivationScopeEvenWithMacroFlip) {
    // This target deliberately compiles the historical experimental macro as 1.
    static_assert(ASFW_SPRO40D3_EXPERIMENTAL_ACTIVATION == 1);
    static_assert(!DriverKitManager::kProductionActivationEnabled);
    const auto diagnostic = Snapshot();
    EXPECT_NE(diagnostic.notificationAddress, 0U);
    EXPECT_EQ(diagnostic.activationState, 0U);
    EXPECT_EQ(diagnostic.sessionState, 0U);
    EXPECT_EQ(diagnostic.sessionToken, 0U);
    EXPECT_EQ(diagnostic.foreignOwner, 0U);
    EXPECT_EQ(diagnostic.ownerProvenance, 0U);
    EXPECT_EQ(diagnostic.identificationState, 1U);
    EXPECT_EQ(diagnostic.reservationState, 1U);
    EXPECT_EQ(diagnostic.hardwareActivationDisabled, 1U);
    EXPECT_EQ(diagnostic.lastOperationStatus, static_cast<uint32_t>(kIOReturnNotPermitted));
    for (const auto scope : {Scope::PassiveDescriptor, Scope::NotificationOwnership,
                            Scope::ReadOnlyMailbox, Scope::MixerRoutingWrite})
        EXPECT_EQ(manager->RequestActivation(scope), kIOReturnNotPermitted);
}

TEST_F(SPro40D3DriverKitManagerTests, MissingGuidHasNoIdentityRouteOrSessionProvenance) {
    (void)Authorize();
    const auto missing = manager->CopyDiagnostics(rom.bib.guid + 100);
    EXPECT_EQ(missing.identificationState, 0U);
    EXPECT_EQ(missing.routeValid, 0U);
    EXPECT_EQ(missing.vendorId, 0U);
    EXPECT_EQ(missing.modelId, 0U);
    EXPECT_EQ(missing.generation, 0U);
    EXPECT_EQ(missing.nodeId, Discovery::kInvalidNodeId);
    EXPECT_EQ(missing.sessionToken, 0U);
    EXPECT_EQ(missing.sessionState, 0U);
    EXPECT_EQ(missing.ownerProvenance, 0U);
    EXPECT_EQ(missing.hardwareActivationDisabled, 1U);
}

TEST_F(SPro40D3DriverKitManagerTests, OtherIdentityIsCachedDiscoveryNotVerifiedTcdIdentification) {
    rom.bib.guid = 0x00A0B00404C01234ULL;
    rom.nodeId = 8;
    rom.rootDirMinimal[0].value = 0x000A0B;
    rom.rootDirMinimal[1].value = 0x00ABCD;
    rom.unitDirectories[0].unitSpecId = 0x000A0B;
    rom.unitDirectories[0].modelId = 0x00ABCD;
    Discover();
    const auto cached = Snapshot();
    EXPECT_EQ(cached.identificationState, 1U); // Discovery provenance, not TCD verification.
    EXPECT_EQ(cached.routeValid, 1U);
    EXPECT_EQ(cached.vendorId, 0x000A0BU);
    EXPECT_EQ(cached.modelId, 0x00ABCDU);
    EXPECT_EQ(cached.nodeId, 8U);
    EXPECT_EQ(cached.sessionToken, 0U);
    EXPECT_EQ(cached.sessionState, 0U);
    EXPECT_EQ(cached.activationState, 0U);
    EXPECT_EQ(cached.ownerProvenance, 0U);
    EXPECT_EQ(cached.hardwareActivationDisabled, 1U);
    EXPECT_FALSE(manager->AuthorizeSimulationForTesting({route, Scope::ReadOnlyMailbox}));
    EXPECT_EQ(manager->RequestActivation(Scope::ReadOnlyMailbox), kIOReturnNotPermitted);
}

TEST_F(SPro40D3DriverKitManagerTests, NodeZeroIsAValidRouteNotAnUnavailableSentinel) {
    rom.nodeId = 0;
    Discover();
    EXPECT_EQ(Snapshot().routeValid, 1U);
    EXPECT_EQ(Snapshot().nodeId, 0U);
    manager->OnBusReset();
    EXPECT_EQ(Snapshot().routeValid, 0U);
    EXPECT_EQ(Snapshot().nodeId, Discovery::kInvalidNodeId);
}

TEST_F(SPro40D3DriverKitManagerTests, RegistryInvalidationRetainsIdentityButNeverStaleNodeOrGeneration) {
    registry.InvalidateLiveMappingsForBusReset();
    const auto stale = Snapshot();
    EXPECT_EQ(stale.identificationState, 2U);
    EXPECT_EQ(stale.vendorId, Profiles::kFocusriteVendorId);
    EXPECT_EQ(stale.modelId, Profiles::kSPro40Tcd3070ModelId);
    EXPECT_EQ(stale.nodeId, Discovery::kInvalidNodeId);
    EXPECT_EQ(stale.routeValid, 0U);
    EXPECT_EQ(stale.generation, 0U);
    EXPECT_EQ(stale.hardwareActivationDisabled, 1U);
}

TEST_F(SPro40D3DriverKitManagerTests, LifecycleRevokesDiagnosticRouteBeforeRegistryInvalidation) {
    for (const bool removal : {false, true}) {
        DriverKitManager candidate(registry, addresses, clock);
        if (removal) candidate.OnDeviceRemoved(route.guid);
        else candidate.OnBusReset();
        ASSERT_TRUE(registry.IsCurrent(route)); // Simulate delayed registry teardown.
        const auto stale = candidate.CopyDiagnostics(route.guid);
        EXPECT_EQ(stale.identificationState, 2U);
        EXPECT_EQ(stale.vendorId, Profiles::kFocusriteVendorId);
        EXPECT_EQ(stale.modelId, Profiles::kSPro40Tcd3070ModelId);
        EXPECT_EQ(stale.nodeId, Discovery::kInvalidNodeId);
        EXPECT_EQ(stale.routeValid, 0U);
        EXPECT_EQ(stale.generation, 0U);
        EXPECT_EQ(stale.hardwareActivationDisabled, 1U);
    }
}

TEST_F(SPro40D3DriverKitManagerTests, FirstEverQueryAfterResetOrRemovalSeesFreshMetadataWithoutAuthorization) {
    for (const bool removal : {false, true}) {
        DriverKitManager candidate(registry, addresses, clock);
        const uint64_t fence = registry.RouteEpochHighWatermark();
        const auto oldRoute = route;
        if (removal) candidate.OnDeviceRemoved(route.guid);
        else candidate.OnBusReset();
        EXPECT_EQ(registry.RouteEpochHighWatermark(), fence); // Fence capture is metadata-only.
        ASSERT_TRUE(registry.IsCurrent(oldRoute));
        if (removal) registry.RetireDevice(route.guid);
        else registry.InvalidateLiveMappingsForBusReset();
        ++rom.gen.value;
        Discover();
        ASSERT_GT(route.routeEpoch, fence);
        EXPECT_EQ(registry.RouteEpochHighWatermark(), route.routeEpoch);
        // No prior CopyDiagnostics or host authorization on candidate: the first
        // query must recognize fresh rediscovery, not quarantine it once again.
        const auto first = candidate.CopyDiagnostics(route.guid);
        EXPECT_EQ(first.identificationState, 1U);
        EXPECT_EQ(first.routeValid, 1U);
        EXPECT_EQ(first.nodeId, rom.nodeId);
        EXPECT_EQ(first.generation, rom.gen.value);
        EXPECT_EQ(first.vendorId, Profiles::kFocusriteVendorId);
        EXPECT_EQ(first.modelId, Profiles::kSPro40Tcd3070ModelId);
        EXPECT_EQ(first.sessionToken, 0U);
        EXPECT_EQ(first.sessionState, 0U);
        EXPECT_EQ(first.activationState, 0U);
        EXPECT_EQ(first.hardwareActivationDisabled, 1U);
        EXPECT_FALSE(candidate.BeginSimulationForTesting({}, {}));
        for (const auto scope : {Scope::PassiveDescriptor, Scope::NotificationOwnership,
                                Scope::ReadOnlyMailbox, Scope::MixerRoutingWrite})
            EXPECT_EQ(candidate.RequestActivation(scope), kIOReturnNotPermitted);
    }
}

TEST_F(SPro40D3DriverKitManagerTests, FreshResetRediscoveryRefreshesOnlyMetadataWithoutAuthorization) {
    ASSERT_EQ(Snapshot().identificationState, 1U);
    const auto permit = Authorize();
    unsigned callbacks = 0;
    auto ticket = manager->BeginSimulationForTesting(permit, [&](IOReturn status) {
        EXPECT_EQ(status, kIOReturnAborted);
        ++callbacks;
    });
    ASSERT_TRUE(ticket);
    manager->OnBusReset();
    ASSERT_TRUE(registry.IsCurrent(route));
    EXPECT_EQ(Snapshot().identificationState, 2U);
    EXPECT_EQ(Snapshot().identificationState, 2U); // Same token never revives itself.
    EXPECT_EQ(Snapshot().generation, 0U);
    registry.InvalidateLiveMappingsForBusReset();
    ++rom.gen.value;
    Discover();
    const auto fresh = Snapshot();
    EXPECT_EQ(fresh.identificationState, 1U);
    EXPECT_EQ(fresh.routeValid, 1U);
    EXPECT_EQ(fresh.generation, rom.gen.value);
    EXPECT_EQ(fresh.nodeId, rom.nodeId);
    EXPECT_EQ(fresh.sessionToken, 0U);
    EXPECT_EQ(fresh.sessionState, 0U);
    EXPECT_EQ(fresh.activationState, 0U);
    EXPECT_EQ(fresh.hardwareActivationDisabled, 1U);
    EXPECT_EQ(callbacks, 1U);
    EXPECT_FALSE(ticket->Complete());
    EXPECT_FALSE(manager->BeginSimulationForTesting(permit, {}));
    for (const auto scope : {Scope::PassiveDescriptor, Scope::NotificationOwnership,
                            Scope::ReadOnlyMailbox, Scope::MixerRoutingWrite})
        EXPECT_EQ(manager->RequestActivation(scope), kIOReturnNotPermitted);
}

TEST_F(SPro40D3DriverKitManagerTests, SameGuidReincarnationRefreshesOnlyMetadataAfterRemoval) {
    ASSERT_EQ(Snapshot().identificationState, 1U);
    const auto permit = Authorize();
    const auto oldRoute = route;
    manager->OnDeviceRemoved(route.guid);
    ASSERT_TRUE(registry.IsCurrent(oldRoute));
    EXPECT_EQ(Snapshot().identificationState, 2U);
    EXPECT_EQ(Snapshot().nodeId, Discovery::kInvalidNodeId);
    EXPECT_EQ(Snapshot().routeValid, 0U);
    registry.RetireDevice(route.guid);
    Discover(); // Same GUID, generation and node; incarnation/route epoch change.
    ASSERT_GT(route.deviceIncarnation, oldRoute.deviceIncarnation);
    ASSERT_GT(route.routeEpoch, oldRoute.routeEpoch);
    const auto fresh = Snapshot();
    EXPECT_EQ(fresh.identificationState, 1U);
    EXPECT_EQ(fresh.routeValid, 1U);
    EXPECT_EQ(fresh.generation, rom.gen.value);
    EXPECT_EQ(fresh.nodeId, rom.nodeId);
    EXPECT_EQ(fresh.sessionToken, 0U);
    EXPECT_EQ(fresh.sessionState, 0U);
    EXPECT_EQ(fresh.activationState, 0U);
    EXPECT_EQ(fresh.hardwareActivationDisabled, 1U);
    EXPECT_FALSE(manager->BeginSimulationForTesting(permit, {}));
    for (const auto scope : {Scope::PassiveDescriptor, Scope::NotificationOwnership,
                            Scope::ReadOnlyMailbox, Scope::MixerRoutingWrite})
        EXPECT_EQ(manager->RequestActivation(scope), kIOReturnNotPermitted);
}

TEST_F(SPro40D3DriverKitManagerTests, ScopesAreIndependentAndEveryPermitComponentIsRequired) {
    for (const auto allowed : {Scope::PassiveDescriptor, Scope::NotificationOwnership,
                              Scope::ReadOnlyMailbox, Scope::MixerRoutingWrite}) {
        DriverKitManager candidate(registry, addresses, clock);
        auto authorized = candidate.AuthorizeSimulationForTesting({route, allowed});
        ASSERT_TRUE(authorized);
        unsigned callbacks = 0;
        auto callback = [&](IOReturn status) { EXPECT_EQ(status, kIOReturnSuccess); ++callbacks; };
        for (const auto requested : {Scope::PassiveDescriptor, Scope::NotificationOwnership,
                                    Scope::ReadOnlyMailbox, Scope::MixerRoutingWrite}) {
            auto permit = *authorized;
            permit.scope = requested;
            auto begun = candidate.BeginSimulationForTesting(permit, callback);
            EXPECT_EQ(begun.has_value(), requested == allowed);
            if (begun) EXPECT_TRUE(begun->Complete());
        }
        auto reject = [&](auto mutate) {
            auto permit = *authorized;
            mutate(permit);
            EXPECT_FALSE(candidate.BeginSimulationForTesting(permit, callback));
        };
        reject([](auto& permit) { ++permit.route.guid; });
        reject([](auto& permit) { ++permit.route.deviceIncarnation; });
        reject([](auto& permit) { ++permit.route.routeEpoch; });
        reject([](auto& permit) { ++permit.route.generation.value; });
        reject([](auto& permit) { ++permit.route.nodeId; });
        reject([](auto& permit) { ++permit.sessionToken; });
        reject([](auto& permit) { ++permit.runtimeGeneration; });
        EXPECT_EQ(callbacks, 1U);
        EXPECT_EQ(candidate.RequestActivation(allowed), kIOReturnNotPermitted);
        EXPECT_EQ(candidate.CopyDiagnostics(route.guid).sessionState, 0U);
    }
}

TEST_F(SPro40D3DriverKitManagerTests, PermitsCannotCrossManagersOrActivateAnUnknownIdentity) {
    const auto permit = Authorize();
    DriverKitManager other(registry, addresses, clock);
    const auto otherPermit = other.AuthorizeSimulationForTesting({route, Scope::ReadOnlyMailbox});
    ASSERT_TRUE(otherPermit);
    EXPECT_NE(permit.sessionToken, otherPermit->sessionToken);
    EXPECT_FALSE(other.BeginSimulationForTesting(permit, {}));
    auto invented = route;
    ++invented.guid;
    EXPECT_FALSE(other.AuthorizeSimulationForTesting({invented, Scope::PassiveDescriptor}));
    // A live route is insufficient: the exact catalog identity is also required.
    rom.rootDirMinimal[1].value = Profiles::kSPro40ModelId;
    rom.unitDirectories[0].modelId = Profiles::kSPro40ModelId;
    Discover();
    EXPECT_FALSE(manager->BeginSimulationForTesting(permit, {}));
    DriverKitManager wrongModel(registry, addresses, clock);
    EXPECT_FALSE(wrongModel.AuthorizeSimulationForTesting({route, Scope::PassiveDescriptor}));
}

TEST_F(SPro40D3DriverKitManagerTests, RegistryInvalidationRejectsAdmissionAndLateSuccess) {
    const auto permit = Authorize();
    unsigned callbacks = 0;
    auto ticket = manager->BeginSimulationForTesting(permit, [&](IOReturn status) {
        EXPECT_EQ(status, kIOReturnAborted);
        ++callbacks;
    });
    ASSERT_TRUE(ticket);
    registry.InvalidateLiveMappingsForBusReset();
    EXPECT_FALSE(manager->BeginSimulationForTesting(permit, {}));
    EXPECT_TRUE(ticket->Complete(kIOReturnSuccess));
    EXPECT_FALSE(ticket->Complete());
    EXPECT_EQ(callbacks, 1U);
    EXPECT_EQ(Snapshot().pendingOperations, 0U);
    EXPECT_EQ(Snapshot().cancelledOperations, 1U);
}

TEST_F(SPro40D3DriverKitManagerTests, ResetCancelsBeforeReentrantCallbackAndRequiresFreshAuthorization) {
    const auto permit = Authorize();
    unsigned callbacks = 0;
    auto ticket = manager->BeginSimulationForTesting(permit, [&](IOReturn status) {
        EXPECT_EQ(status, kIOReturnAborted);
        ++callbacks;
        EXPECT_EQ(Snapshot().pendingOperations, 0U);
        EXPECT_EQ(Snapshot().sessionToken, 0U);
        EXPECT_FALSE(manager->BeginSimulationForTesting(permit, {}));
        manager->OnBusReset();
        manager->OnDeviceRemoved(route.guid);
        EXPECT_EQ(manager->RequestActivation(Scope::MixerRoutingWrite), kIOReturnNotPermitted);
    });
    ASSERT_TRUE(ticket);
    EXPECT_EQ(Snapshot().pendingOperations, 1U);
    manager->OnBusReset();
    EXPECT_FALSE(ticket->Complete());
    EXPECT_EQ(callbacks, 1U);
    EXPECT_EQ(Snapshot().cancelledOperations, 1U);
    EXPECT_FALSE(manager->AuthorizeSimulationForTesting({route, Scope::ReadOnlyMailbox}));
    registry.InvalidateLiveMappingsForBusReset();
    ++rom.gen.value;
    Discover();
    EXPECT_EQ(Snapshot().identificationState, 1U); // Fresh metadata is not authorization.
    EXPECT_EQ(Snapshot().generation, rom.gen.value);
    EXPECT_EQ(Snapshot().sessionToken, 0U);
    EXPECT_FALSE(manager->BeginSimulationForTesting(permit, {}));
    const auto fresh = Authorize();
    EXPECT_EQ(Snapshot().identificationState, 1U);
    EXPECT_EQ(Snapshot().generation, rom.gen.value);
    EXPECT_EQ(Snapshot().nodeId, rom.nodeId);
    EXPECT_NE(fresh.sessionToken, permit.sessionToken);
    EXPECT_NE(fresh.runtimeGeneration, permit.runtimeGeneration);
    EXPECT_FALSE(manager->BeginSimulationForTesting(permit, {}));
    auto begun = manager->BeginSimulationForTesting(fresh, {});
    ASSERT_TRUE(begun);
    EXPECT_TRUE(begun->Complete());
}

TEST_F(SPro40D3DriverKitManagerTests, RemovalIsIdentityScopedAndCannotDoubleCancel) {
    const auto permit = Authorize();
    unsigned callbacks = 0;
    auto ticket = manager->BeginSimulationForTesting(permit, [&](IOReturn status) {
        EXPECT_EQ(status, kIOReturnAborted);
        ++callbacks;
        manager->Stop();
        manager->Stop();
    });
    ASSERT_TRUE(ticket);
    manager->OnDeviceRemoved(route.guid + 1);
    EXPECT_EQ(Snapshot().pendingOperations, 1U);
    manager->OnDeviceRemoved(route.guid);
    EXPECT_FALSE(ticket->Complete());
    EXPECT_EQ(callbacks, 1U);
    EXPECT_EQ(Snapshot().cancelledOperations, 1U);
    EXPECT_EQ(Snapshot().notificationAddress, 0U);
}

TEST_F(SPro40D3DriverKitManagerTests, StopWinsBeforeOutstandingCompletionWithoutTimingAssumptions) {
    const auto permit = Authorize();
    std::atomic<unsigned> callbacks{0};
    auto ticket = manager->BeginSimulationForTesting(permit, [&](IOReturn status) {
        EXPECT_EQ(status, kIOReturnAborted);
        callbacks.fetch_add(1);
    });
    ASSERT_TRUE(ticket);
    std::latch workerReady{1}, mayComplete{1};
    std::thread worker([&, ticket = *ticket] {
        workerReady.count_down();
        mayComplete.wait();
        EXPECT_FALSE(ticket.Complete());
    });
    workerReady.wait();
    manager->Stop();
    EXPECT_EQ(callbacks.load(), 1U);
    EXPECT_FALSE(manager->BeginSimulationForTesting(permit, {}));
    EXPECT_EQ(Snapshot().pendingOperations, 0U);
    mayComplete.count_down();
    worker.join();
    EXPECT_EQ(Snapshot().cancelledOperations, 1U);
}

TEST_F(SPro40D3DriverKitManagerTests, ClaimedCallbackCanOverlapStopAndReenterWithoutSelfWait) {
    const auto permit = Authorize();
    std::latch callbackEntered{1}, mayReturn{1};
    std::atomic<unsigned> callbacks{0};
    auto ticket = manager->BeginSimulationForTesting(permit, [&](IOReturn status) {
        EXPECT_EQ(status, kIOReturnSuccess);
        callbacks.fetch_add(1);
        callbackEntered.count_down();
        mayReturn.wait();
        manager->Stop();
        manager->OnBusReset();
        EXPECT_EQ(Snapshot().pendingOperations, 0U);
    });
    ASSERT_TRUE(ticket);
    std::thread worker([ticket = *ticket] { EXPECT_TRUE(ticket.Complete()); });
    callbackEntered.wait();
    // No dependency on callback completion: it has already claimed the terminal
    // event and its invoking frame retains the manager until the worker joins.
    manager->Stop();
    EXPECT_EQ(Snapshot().pendingOperations, 0U);
    EXPECT_EQ(Snapshot().cancelledOperations, 0U);
    EXPECT_FALSE(ticket->Complete());
    mayReturn.count_down();
    worker.join();
    EXPECT_EQ(callbacks.load(), 1U);
}

TEST_F(SPro40D3DriverKitManagerTests, ClaimedCallbackOutlivesAllStoppedManagersBorrowedDependencies) {
    auto borrowedRegistry = std::make_unique<Discovery::DeviceRegistry>();
    auto borrowedAddresses = std::make_unique<AddressManager>(nullptr);
    auto borrowedClock = std::make_unique<Clock>();
    (void)borrowedRegistry->UpsertFromROM(rom, Discovery::LinkPolicy{});
    const auto borrowedRoute = borrowedRegistry->CurrentRoute(rom.bib.guid);
    ASSERT_TRUE(borrowedRoute);
    auto retained = std::make_unique<DriverKitManager>(
        *borrowedRegistry, *borrowedAddresses, *borrowedClock);
    const auto permit = retained->AuthorizeSimulationForTesting({*borrowedRoute, Scope::ReadOnlyMailbox});
    ASSERT_TRUE(permit);
    std::latch callbackEntered{1}, dependenciesDestroyed{1};
    auto observer = std::make_shared<std::atomic<unsigned>>(0);
    const std::weak_ptr<std::atomic<unsigned>> weakObserver = observer;
    const uint64_t guid = rom.bib.guid;
    auto ticket = retained->BeginSimulationForTesting(*permit, [&, observer, guid](IOReturn status) {
        EXPECT_EQ(status, kIOReturnSuccess);
        callbackEntered.count_down();
        dependenciesDestroyed.wait();
        // Only detached runtime is consulted. The callback itself retains its
        // observer, and the outer frame retains the invoking manager until join.
        retained->Stop();
        retained->OnBusReset();
        retained->OnDeviceRemoved(guid);
        const auto stopped = retained->CopyDiagnostics(guid);
        EXPECT_EQ(stopped.identificationState, 0U);
        EXPECT_EQ(stopped.vendorId, 0U);
        EXPECT_EQ(stopped.pendingOperations, 0U);
        EXPECT_EQ(retained->RequestActivation(Scope::ReadOnlyMailbox), kIOReturnNotPermitted);
        observer->fetch_add(1);
    });
    ASSERT_TRUE(ticket);
    std::thread worker([ticket = *ticket] { EXPECT_TRUE(ticket.Complete()); });
    callbackEntered.wait();
    retained->Stop();
    borrowedRegistry.reset();
    borrowedAddresses.reset();
    borrowedClock.reset();
    observer.reset();
    EXPECT_FALSE(weakObserver.expired());
    dependenciesDestroyed.count_down();
    worker.join();
    EXPECT_TRUE(weakObserver.expired());
    EXPECT_FALSE(ticket->Complete());
    retained.reset();
    EXPECT_FALSE(ticket->Complete());
}

TEST_F(SPro40D3DriverKitManagerTests, ConcurrentResetAndStopClaimExactlyOneCancellation) {
    const auto permit = Authorize();
    std::atomic<unsigned> callbacks{0};
    auto ticket = manager->BeginSimulationForTesting(permit, [&](IOReturn status) {
        EXPECT_EQ(status, kIOReturnAborted);
        callbacks.fetch_add(1);
        (void)Snapshot();
        manager->Stop();
    });
    ASSERT_TRUE(ticket);
    std::barrier start{3};
    std::thread reset([&] { start.arrive_and_wait(); manager->OnBusReset(); });
    std::thread stop([&] { start.arrive_and_wait(); manager->Stop(); });
    start.arrive_and_wait();
    reset.join();
    stop.join();
    EXPECT_EQ(callbacks.load(), 1U);
    EXPECT_FALSE(ticket->Complete());
    EXPECT_EQ(Snapshot().pendingOperations, 0U);
    EXPECT_EQ(Snapshot().cancelledOperations, 1U);
    EXPECT_EQ(Snapshot().notificationAddress, 0U);
}

TEST_F(SPro40D3DriverKitManagerTests, VirtualTimeoutIsTerminalAndNotReclassifiedByStop) {
    const auto permit = Authorize();
    unsigned callbacks = 0;
    auto ticket = manager->BeginSimulationForTesting(permit, [&](IOReturn status) {
        EXPECT_EQ(status, kIOReturnTimeout);
        ++callbacks;
        manager->Stop();
    });
    ASSERT_TRUE(ticket);
    EXPECT_TRUE(ticket->Complete(kIOReturnTimeout));
    EXPECT_FALSE(ticket->Complete());
    manager->Stop();
    EXPECT_EQ(callbacks, 1U);
    EXPECT_EQ(Snapshot().timedOutOperations, 1U);
    EXPECT_EQ(Snapshot().cancelledOperations, 0U);
}

TEST(SPro40D3DriverKitManagerLifetimeTests, StoppedManagerAndLateTicketOutliveEveryBorrowedDependency) {
    std::unique_ptr<DriverKitManager> retained;
    DriverKitManager::SimulatedCompletion late;
    std::weak_ptr<int> observer;
    {
        Discovery::DeviceRegistry registry;
        AddressManager addresses{nullptr};
        Clock clock;
        retained = std::make_unique<DriverKitManager>(registry, addresses, clock);
        EXPECT_FALSE(late.Complete());
        Discovery::ConfigROM rom{};
        rom.bib.guid = 0x00130E0404C01234ULL;
        rom.gen = ASFW::FW::Generation{19};
        rom.nodeId = 7;
        rom.rootDirMinimal = {
            {.key = Discovery::CfgKey::VendorId, .value = Profiles::kFocusriteVendorId},
            {.key = Discovery::CfgKey::ModelId, .value = Profiles::kSPro40Tcd3070ModelId}};
        Discovery::UnitDirectory unit{};
        unit.offsetQuadlets = 5;
        unit.unitSpecId = Profiles::kFocusriteVendorId;
        unit.unitSwVersion = 1;
        unit.modelId = Profiles::kSPro40Tcd3070ModelId;
        rom.unitDirectories.push_back(unit);
        (void)registry.UpsertFromROM(rom, Discovery::LinkPolicy{});
        auto route = registry.CurrentRoute(rom.bib.guid);
        ASSERT_TRUE(route);
        auto permit = retained->AuthorizeSimulationForTesting({*route, Scope::ReadOnlyMailbox});
        ASSERT_TRUE(permit);
        auto owner = std::make_shared<int>(0);
        observer = owner;
        auto begun = retained->BeginSimulationForTesting(*permit, [owner](IOReturn status) {
            EXPECT_EQ(status, kIOReturnAborted);
            ++*owner;
        });
        ASSERT_TRUE(begun);
        late = *begun;
        retained->Stop();
        EXPECT_EQ(*owner, 1);
        owner.reset();
        EXPECT_TRUE(observer.expired());
    }
    const auto snapshot = retained->CopyDiagnostics(1);
    EXPECT_EQ(snapshot.notificationAddress, 0U);
    EXPECT_EQ(snapshot.vendorId, 0U);
    EXPECT_EQ(snapshot.modelId, 0U);
    EXPECT_EQ(snapshot.reservationState, 2U);
    EXPECT_EQ(retained->RequestActivation(Scope::ReadOnlyMailbox), kIOReturnNotPermitted);
    retained->OnBusReset();
    retained->OnDeviceRemoved(1);
    retained.reset();
    EXPECT_FALSE(late.Complete());
}

TEST_F(SPro40D3DriverKitManagerTests, LateTicketDoesNotRetainCallbackCapturesOrManager) {
    const auto permit = Authorize();
    auto observer = std::make_shared<int>(0);
    std::weak_ptr<int> weakObserver = observer;
    auto ticket = manager->BeginSimulationForTesting(permit, [observer](IOReturn status) {
        EXPECT_EQ(status, kIOReturnAborted);
        ++*observer;
    });
    ASSERT_TRUE(ticket);
    manager->Stop();
    EXPECT_EQ(*observer, 1);
    observer.reset();
    EXPECT_TRUE(weakObserver.expired());
    manager.reset();
    registry.Clear();
    EXPECT_FALSE(ticket->Complete());
}

TEST_F(SPro40D3DriverKitManagerTests, RejectedCallbackCaptureDestructionMayReenterDiagnostics) {
    struct Observer {
        DriverKitManager* manager;
        uint64_t guid;
        ~Observer() { (void)manager->CopyDiagnostics(guid); }
    };
    // Rejection releases the last capture after unlocking, not under an unfair
    // lock. The host lock shim aborts on recursive locking, as DriverKit does.
    auto observer = std::make_shared<Observer>(Observer{manager.get(), route.guid});
    EXPECT_FALSE(manager->BeginSimulationForTesting({}, [observer = std::move(observer)](IOReturn) {}));
}

} // namespace
