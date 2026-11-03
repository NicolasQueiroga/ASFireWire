// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "Audio/Protocols/DICE/Focusrite/SPro40D3DriverKitManager.hpp"

#include <gtest/gtest.h>

#include <thread>

namespace {
using Manager = ASFW::Protocols::SBP2::AddressSpaceManager;
using DriverKitManager = ASFW::Audio::DICE::Focusrite::SPro40D3::SPro40D3DriverKitManager;
using Scope = ASFW::Audio::DICE::Focusrite::SPro40D3::DiagnosticScope;

class Clock final : public ASFW::Audio::DICE::DiceWaitClock {
public:
    void SleepMs(uint32_t milliseconds) noexcept override { nowMs += milliseconds; }
    void Backoff(uint32_t) noexcept override {}
    [[nodiscard]] uint64_t NowMs() const noexcept override { return nowMs; }

private:
    uint64_t nowMs{0};
};

TEST(SPro40D3DriverKitManagerTests, StartsDormantAndDeniesEveryActivationScope) {
    ASFW::Discovery::DeviceRegistry registry;
    Manager addresses{nullptr};
    Clock clock;
    DriverKitManager manager(registry, addresses, clock);

    const auto diagnostic = manager.CopyDiagnostics(0x130e0400000001ULL);
    EXPECT_FALSE(DriverKitManager::kProductionActivationEnabled);
    EXPECT_NE(diagnostic.notificationAddress, 0U);
    EXPECT_EQ(diagnostic.activationState, 0U);
    EXPECT_EQ(diagnostic.sessionState, 0U);
    EXPECT_EQ(diagnostic.foreignOwner, 0U);
    EXPECT_EQ(diagnostic.lastOperationStatus, static_cast<uint32_t>(kIOReturnNotPermitted));

    EXPECT_EQ(manager.RequestActivation(Scope::PassiveDescriptor), kIOReturnNotPermitted);
    EXPECT_EQ(manager.RequestActivation(Scope::NotificationOwnership), kIOReturnNotPermitted);
    EXPECT_EQ(manager.RequestActivation(Scope::ReadOnlyMailbox), kIOReturnNotPermitted);
    EXPECT_EQ(manager.RequestActivation(Scope::MixerRoutingWrite), kIOReturnNotPermitted);
}

TEST(SPro40D3DriverKitManagerTests, RetainedStoppedManagerNeverReadsDestroyedRegistry) {
    Clock clock;
    std::unique_ptr<DriverKitManager> retained;
    {
        ASFW::Discovery::DeviceRegistry registry;
        Manager addresses{nullptr};
        retained = std::make_unique<DriverKitManager>(registry, addresses, clock);
        retained->OnBusReset();
        retained->OnDeviceRemoved(1);
        retained->Stop();
        retained->Stop();
    }
    const auto snapshot = retained->CopyDiagnostics(1);
    EXPECT_EQ(snapshot.notificationAddress, 0U);
    EXPECT_EQ(snapshot.vendorId, 0U);
    EXPECT_EQ(snapshot.modelId, 0U);
    EXPECT_EQ(retained->RequestActivation(Scope::ReadOnlyMailbox), kIOReturnNotPermitted);
    retained->OnBusReset();
    retained->OnDeviceRemoved(1);
}

TEST(SPro40D3DriverKitManagerTests, ConcurrentCachedSnapshotsAndResetAreQuiescedByStop) {
    ASFW::Discovery::DeviceRegistry registry;
    Manager addresses{nullptr};
    Clock clock;
    DriverKitManager manager(registry, addresses, clock);
    std::thread snapshots([&] {
        for (unsigned i = 0; i < 1000; ++i) (void)manager.CopyDiagnostics(1);
    });
    std::thread resets([&] {
        for (unsigned i = 0; i < 100; ++i) manager.OnBusReset();
    });
    manager.Stop();
    snapshots.join();
    resets.join();
    EXPECT_EQ(manager.CopyDiagnostics(1).notificationAddress, 0U);
}

} // namespace
