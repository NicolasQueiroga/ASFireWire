// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "Audio/Protocols/DICE/Focusrite/SPro40D3NotificationEndpoint.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <functional>
#include <latch>
#include <thread>

namespace {
namespace Discovery = ASFW::Discovery;
using ASFW::Async::ResponseCode;
using Manager = ASFW::Protocols::SBP2::AddressSpaceManager;
using Hub = ASFW::Audio::DICE::Focusrite::SPro40D3::SPro40D3NotificationHub;
using Endpoint = ASFW::Audio::DICE::Focusrite::SPro40D3::SPro40D3NotificationEndpoint;

class Clock final : public ASFW::Audio::DICE::DiceWaitClock {
public:
    void SleepMs(uint32_t ms) noexcept override { now += ms; }
    void Backoff(uint32_t) noexcept override {}
    uint64_t NowMs() const noexcept override {
        if (onNow) onNow();
        return now.load();
    }
    std::atomic<uint64_t> now{0};
    std::function<void()> onNow;
};

class SPro40D3NotificationEndpointTests : public testing::Test {
protected:
    Discovery::DeviceRouteToken Bind() {
        Discovery::ConfigROM rom{};
        rom.bib.guid = 0x130e0400000001ULL;
        rom.gen = Discovery::Generation{7};
        rom.nodeId = 3;
        (void)registry.UpsertFromROM(rom, {});
        return *registry.CurrentRoute(rom.bib.guid);
    }

    std::shared_ptr<Hub> MakeHub() {
        return std::make_shared<Hub>(registry, clock, Hub::Region{0x1000, 0x1100});
    }

    std::array<uint8_t, Hub::kFrameBytes> frame{0, 1, 2, 3, 4, 5, 6, 7,
                                                 8, 9, 10, 11, 12, 13, 14, 15};
    Discovery::DeviceRegistry registry;
    Clock clock;
    Manager manager{nullptr};
};

TEST_F(SPro40D3NotificationEndpointTests, AllocatesCollisionFreeManagerAddresses) {
    const auto first = Endpoint::Create(manager, MakeHub());
    const auto second = Endpoint::Create(manager, MakeHub());
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ((*first)->address() % Hub::kFrameBytes, 0U);
    EXPECT_NE((*first)->address(), (*second)->address());
    EXPECT_EQ((*second)->address() - (*first)->address(), Hub::kFrameBytes);
}

TEST_F(SPro40D3NotificationEndpointTests, AlignsAfterEightByteAllocationWithInterleavedConsumers) {
    int owner = 0;
    uint64_t handle = 0;
    Manager::AddressRangeMeta meta{};
    ASSERT_EQ(kIOReturnSuccess,
              manager.AllocateAddressRangeAuto(&owner, 0xFFFF, 8, &handle, &meta));
    EXPECT_EQ(meta.addressLo, 0x0010'0000u);
    const auto first = Endpoint::Create(manager, MakeHub());
    ASSERT_TRUE(first);
    EXPECT_EQ((*first)->address(), 0xFFFF'0010'0010ULL);
    ASSERT_TRUE((*first)->Bind(Bind()));
    ASSERT_EQ(kIOReturnSuccess,
              manager.AllocateAddressRangeAuto(&owner, 0xFFFF, 8, &handle, &meta));
    EXPECT_EQ(meta.addressLo, 0x0010'0008u); // Legacy consumer reuses padding.
    ASSERT_EQ(kIOReturnSuccess,
              manager.AllocateAddressRangeAuto(&owner, 0xFFFF, 8, &handle, &meta));
    EXPECT_EQ(meta.addressLo, 0x0010'0020u);
    const auto second = Endpoint::Create(manager, MakeHub());
    ASSERT_TRUE(second);
    EXPECT_EQ((*second)->address(), 0xFFFF'0010'0030ULL);
    ASSERT_TRUE((*second)->Bind(Bind()));
    (*first)->Release();
    manager.ReleaseOwner(&owner);
    const auto third = Endpoint::Create(manager, MakeHub());
    ASSERT_TRUE(third);
    EXPECT_EQ((*third)->address(), 0xFFFF'0010'0000ULL);
    const auto fourth = Endpoint::Create(manager, MakeHub());
    ASSERT_TRUE(fourth);
    EXPECT_EQ((*fourth)->address(), 0xFFFF'0010'0020ULL);
    // Neither the released first endpoint nor the live second was reused.
}

TEST_F(SPro40D3NotificationEndpointTests, FragmentationAndExhaustionFailWithoutReservingRanges) {
    int owner = 0;
    uint64_t prefix = 0, divider = 0, tail = 0;
    // Leave two 8-byte holes near the end of the real allocation window.
    ASSERT_EQ(kIOReturnSuccess,
              manager.AllocateAddressRange(&owner, 0xFFFF, 0x0010'0000,
                                           0x0FF0'0000U - 40, &prefix));
    ASSERT_EQ(kIOReturnSuccess,
              manager.AllocateAddressRange(&owner, 0xFFFF, 0x0FFF'FFE0, 8, &divider));
    ASSERT_EQ(kIOReturnSuccess,
              manager.AllocateAddressRange(&owner, 0xFFFF, 0x0FFF'FFF0, 16, &tail));
    const auto fragmented = Endpoint::Create(manager, MakeHub());
    ASSERT_FALSE(fragmented);
    EXPECT_EQ(fragmented.error(), kIOReturnNoSpace);

    uint64_t hole1 = 0, hole2 = 0;
    Manager::AddressRangeMeta meta{};
    ASSERT_EQ(kIOReturnSuccess,
              manager.AllocateAddressRangeAuto(&owner, 0xFFFF, 8, &hole1, &meta));
    EXPECT_EQ(meta.addressLo, 0x0FFF'FFD8u);
    ASSERT_EQ(kIOReturnSuccess,
              manager.AllocateAddressRangeAuto(&owner, 0xFFFF, 8, &hole2, &meta));
    EXPECT_EQ(meta.addressLo, 0x0FFF'FFE8u);
    const auto exhausted = Endpoint::Create(manager, MakeHub());
    ASSERT_FALSE(exhausted);
    EXPECT_EQ(exhausted.error(), kIOReturnNoSpace);

    // Free the two holes and divider; first-fit must skip the unaligned 8
    // bytes and use the final aligned 16-byte gap without crossing the tail.
    ASSERT_EQ(kIOReturnSuccess, manager.DeallocateAddressRange(&owner, hole1));
    ASSERT_EQ(kIOReturnSuccess, manager.DeallocateAddressRange(&owner, hole2));
    ASSERT_EQ(kIOReturnSuccess, manager.DeallocateAddressRange(&owner, divider));
    const auto recovered = Endpoint::Create(manager, MakeHub());
    ASSERT_TRUE(recovered);
    EXPECT_EQ((*recovered)->address(), 0xFFFF'0FFF'FFE0ULL);
    ASSERT_TRUE((*recovered)->Bind(Bind()));
    (*recovered)->Release();
    const auto retired = Endpoint::Create(manager, MakeHub());
    ASSERT_FALSE(retired);
    EXPECT_EQ(retired.error(), kIOReturnNoSpace);
}

TEST_F(SPro40D3NotificationEndpointTests, DeliversFullProvenanceAndValidatesFrame) {
    const auto hub = MakeHub();
    const auto endpoint = Endpoint::Create(manager, hub);
    ASSERT_TRUE(endpoint);
    const auto lease = (*endpoint)->Bind(Bind());
    ASSERT_TRUE(lease);
    EXPECT_EQ(lease->offset, (*endpoint)->address());
    const auto source = static_cast<uint16_t>(0xFFC0U | lease->route.nodeId);

    clock.now = 42;
    const auto original = frame;
    EXPECT_EQ(manager.ApplyRemoteWrite((*endpoint)->address(), frame, source,
                                       lease->route.generation.value, 1),
              ResponseCode::Complete);
    frame.fill(0xEE); // No borrowed payload survives manager callback return.
    clock.now = 99;
    const auto notification = hub->Take(*lease);
    ASSERT_TRUE(notification);
    EXPECT_EQ(notification->frame, original);
    EXPECT_EQ(notification->receivedMs, 42U);
    frame = original;

    EXPECT_EQ(manager.ApplyRemoteWrite((*endpoint)->address(), frame, source + 1,
                                       lease->route.generation.value, 1),
              ResponseCode::AddressError);
    EXPECT_EQ(manager.ApplyRemoteWrite((*endpoint)->address(), frame, source,
                                       lease->route.generation.value + 1, 1),
              ResponseCode::AddressError);
    EXPECT_EQ(manager.ApplyRemoteWrite((*endpoint)->address(), frame, source,
                                       lease->route.generation.value, 0),
              ResponseCode::TypeError);
    // A codec-readable 12-byte prefix is not silently padded to the strict
    // ingress policy's 16 bytes. Reservation size alone is not wire evidence.
    for (const size_t length : {4U, 11U, 12U, 13U, 14U, 15U}) {
        EXPECT_EQ(manager.ApplyRemoteWrite((*endpoint)->address(),
                                           std::span<const uint8_t>(frame).first(length), source,
                                           lease->route.generation.value, 1),
                  ResponseCode::DataError) << length;
    }
    EXPECT_EQ(manager.ApplyRemoteWrite((*endpoint)->address() + 1,
                                       std::span<const uint8_t>(frame).first(15), source,
                                       lease->route.generation.value, 1),
              ResponseCode::AddressError);
    EXPECT_FALSE(hub->Take(*lease));
}

TEST_F(SPro40D3NotificationEndpointTests, ReleaseRetiresAddressAndClearsDelivery) {
    const auto hub = MakeHub();
    const auto endpoint = Endpoint::Create(manager, hub);
    ASSERT_TRUE(endpoint);
    const auto lease = (*endpoint)->Bind(Bind());
    ASSERT_TRUE(lease);
    const auto address = (*endpoint)->address();
    (*endpoint)->Release();
    (*endpoint)->Release(); // Retirement must be idempotent and clear lease state.
    EXPECT_FALSE((*endpoint)->Bind(Bind())); // A retired address cannot be rebound.
    EXPECT_EQ(manager.ApplyRemoteWrite(address, frame,
                                       static_cast<uint16_t>(0xFFC0U | lease->route.nodeId),
                                       lease->route.generation.value, 1),
              ResponseCode::AddressError);
    EXPECT_FALSE(hub->Take(*lease));

    const auto successor = Endpoint::Create(manager, MakeHub());
    ASSERT_TRUE(successor);
    EXPECT_NE((*successor)->address(), address);
}

TEST_F(SPro40D3NotificationEndpointTests, ClockCanReenterRetirementWithoutEndpointLock) {
    const auto hub = MakeHub();
    const auto endpoint = Endpoint::Create(manager, hub);
    ASSERT_TRUE(endpoint);
    const auto lease = (*endpoint)->Bind(Bind());
    ASSERT_TRUE(lease);
    clock.onNow = [&] { (*endpoint)->Release(); };
    EXPECT_EQ(manager.ApplyRemoteWrite((*endpoint)->address(), frame, 0xFFC3, 7, 1),
              ResponseCode::AddressError);
    clock.onNow = {};
    EXPECT_FALSE(hub->Take(*lease));
    EXPECT_FALSE((*endpoint)->Bind(Bind()));
}

TEST_F(SPro40D3NotificationEndpointTests, PausedIngressPinsLifetimeAndRejectsConcurrentRetirement) {
    auto hub = MakeHub();
    auto endpoint = Endpoint::Create(manager, hub);
    ASSERT_TRUE(endpoint);
    const auto lease = (*endpoint)->Bind(Bind());
    ASSERT_TRUE(lease);
    const auto address = (*endpoint)->address();
    const std::weak_ptr<Endpoint> weakEndpoint = *endpoint;
    const std::weak_ptr<Hub> weakHub = hub;
    std::latch entered{1};
    std::latch resume{1};
    clock.onNow = [&] { entered.count_down(); resume.wait(); };
    ResponseCode result = ResponseCode::Complete;
    std::thread ingress([&] {
        result = manager.ApplyRemoteWrite(address, frame, 0xFFC3, 7, 1);
    });
    entered.wait(); // Callback owns endpoint, but no endpoint/hub lock is held.
    (*endpoint)->Release();
    EXPECT_FALSE(hub->Take(*lease));
    endpoint->reset();
    hub.reset();
    EXPECT_FALSE(weakEndpoint.expired());
    EXPECT_FALSE(weakHub.expired());
    resume.count_down();
    ingress.join();
    clock.onNow = {};
    EXPECT_EQ(result, ResponseCode::AddressError);
    EXPECT_TRUE(weakEndpoint.expired());
    EXPECT_TRUE(weakHub.expired());
    EXPECT_EQ(manager.ApplyRemoteWrite(address, frame, 0xFFC3, 7, 1),
              ResponseCode::AddressError);
}

TEST_F(SPro40D3NotificationEndpointTests, DestroyedEndpointCannotReceiveManagerCallback) {
    const auto hub = MakeHub();
    uint64_t address = 0;
    void* retiredOwner = nullptr;
    {
        const auto endpoint = Endpoint::Create(manager, hub);
        ASSERT_TRUE(endpoint);
        retiredOwner = endpoint->get();
        const auto lease = (*endpoint)->Bind(Bind());
        ASSERT_TRUE(lease);
        address = (*endpoint)->address();
    }
    EXPECT_EQ(manager.ApplyRemoteWrite(address, frame, 0xFFC3, 7, 1),
              ResponseCode::AddressError);
    // Simulate unrelated owner cleanup after malloc reuses the endpoint's
    // address. The controller-owned tombstone must still exclude this range.
    manager.ReleaseOwner(retiredOwner);
    const auto replacement = Endpoint::Create(manager, MakeHub());
    ASSERT_TRUE(replacement);
    EXPECT_NE((*replacement)->address(), address);
}

} // namespace
