// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "Audio/Protocols/DICE/Focusrite/SPro40D3NotificationHub.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <limits>
#include <thread>

namespace {
namespace Discovery = ASFW::Discovery;
using ASFW::Async::LocalRequestContext;
using ASFW::Async::ResponseCode;
using Hub = ASFW::Audio::DICE::Focusrite::SPro40D3::SPro40D3NotificationHub;

class Clock final : public ASFW::Audio::DICE::DiceWaitClock {
public:
    void SleepMs(uint32_t ms) noexcept override { now.fetch_add(ms); }
    void Backoff(uint32_t) noexcept override { now.fetch_add(1); }
    uint64_t NowMs() const noexcept override { return now.load(); }
    std::atomic<uint64_t> now{0};
};

class SPro40D3NotificationHubTests : public testing::Test {
protected:
    Discovery::DeviceRouteToken Bind(uint64_t guid = 0x130e0400000001ULL,
                                    uint8_t node = 3, uint32_t generation = 7) {
        Discovery::ConfigROM rom{};
        rom.bib.guid = guid;
        rom.gen = Discovery::Generation{generation};
        rom.nodeId = node;
        (void)registry.UpsertFromROM(rom, {});
        return *registry.CurrentRoute(guid);
    }

    LocalRequestContext Request(const Hub::Lease& lease) {
        return {.destOffset = lease.offset,
                .tCode = 1,
                .sourceID = static_cast<uint16_t>(0xFFC0U | lease.route.nodeId),
                .generation = lease.route.generation.value,
                .writePayload = frame,
                .dataLength = 16};
    }

    Discovery::DeviceRegistry registry;
    Clock clock;
    Hub hub{registry, clock, {0x1000, 0x1100}};
    std::array<uint8_t, 16> frame{0, 1, 2, 3, 4, 5, 6, 7,
                                  8, 9, 10, 11, 12, 13, 14, 15};
};

TEST_F(SPro40D3NotificationHubTests, AllocatesUniqueAlignedOffsetOnlyAddresses) {
    const auto route = Bind();
    const auto a = hub.Allocate(route);
    const auto b = hub.Allocate(route);
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);
    EXPECT_EQ(a->offset, 0x1000U);
    EXPECT_EQ(b->offset, 0x1010U);
    EXPECT_GT(b->id, a->id);
    EXPECT_EQ(a->route, route);
    EXPECT_STREQ(hub.Name(), "SPro40D3NotificationHub");
}

TEST_F(SPro40D3NotificationHubTests, ExhaustionNeverReusesReleasedAddress) {
    const auto route = Bind();
    Hub small{registry, clock, {0x2000, 0x2010}};
    const auto lease = small.Allocate(route);
    ASSERT_TRUE(lease);
    small.Release(*lease);
    const auto exhausted = small.Allocate(route);
    ASSERT_FALSE(exhausted);
    EXPECT_EQ(exhausted.error(), kIOReturnNoSpace);
}

TEST_F(SPro40D3NotificationHubTests, RejectsInvalidRegionsWithoutOverflow) {
    const auto route = Bind();
    constexpr uint64_t end = uint64_t{1} << 48U;
    for (const auto region : {Hub::Region{0x1001, 0x1100},
                              Hub::Region{0x1000, 0x1101},
                              Hub::Region{0x1000, 0x1000},
                              Hub::Region{0x1100, 0x1000},
                              Hub::Region{end, end + 16},
                              Hub::Region{0, std::numeric_limits<uint64_t>::max()},
                              Hub::Region{std::numeric_limits<uint64_t>::max() - 15, 0}}) {
        Hub invalid{registry, clock, region};
        const auto lease = invalid.Allocate(route);
        ASSERT_FALSE(lease);
        EXPECT_EQ(lease.error(), kIOReturnBadArgument);
    }
    Hub last{registry, clock, {end - 16, end}};
    const auto lease = last.Allocate(route);
    ASSERT_TRUE(lease);
    EXPECT_EQ(lease->offset, end - 16);
    EXPECT_EQ(last.Allocate(route).error(), kIOReturnNoSpace);
}

TEST_F(SPro40D3NotificationHubTests, CopiesExactFrameWithIngressRatherThanConsumptionTime) {
    const auto lease = hub.Allocate(Bind());
    ASSERT_TRUE(lease);
    clock.now = 42;
    const auto original = frame;
    const auto result = hub.HandleLocalRequest(Request(*lease));
    EXPECT_TRUE(result.claimed);
    EXPECT_EQ(result.rcode, ResponseCode::Complete);
    frame.fill(0xFF);
    clock.now = 200;
    const auto notification = hub.Take(*lease);
    ASSERT_TRUE(notification);
    EXPECT_EQ(notification->frame, original);
    EXPECT_EQ(notification->receivedMs, 42U);
    EXPECT_FALSE(hub.Take(*lease));
}

TEST_F(SPro40D3NotificationHubTests, RejectsWrongGenerationNodeAndNonlocalBusAliases) {
    const auto lease = hub.Allocate(Bind());
    ASSERT_TRUE(lease);
    auto ctx = Request(*lease);
    ++ctx.generation;
    EXPECT_EQ(hub.HandleLocalRequest(ctx).rcode, ResponseCode::AddressError);
    for (const uint16_t source : {uint16_t{0xFFC4}, uint16_t{3}, uint16_t{0x0043},
                                  uint16_t{0xF003}}) {
        ctx = Request(*lease);
        ctx.sourceID = source;
        EXPECT_EQ(hub.HandleLocalRequest(ctx).rcode, ResponseCode::AddressError);
    }
    EXPECT_FALSE(hub.Take(*lease));
}

TEST_F(SPro40D3NotificationHubTests, RejectsWrongTcodeAndBothLengthMismatches) {
    const auto lease = hub.Allocate(Bind());
    ASSERT_TRUE(lease);
    for (const uint8_t tcode : {0, 4, 5, 9}) {
        auto ctx = Request(*lease);
        ctx.tCode = tcode;
        EXPECT_EQ(hub.HandleLocalRequest(ctx).rcode, ResponseCode::TypeError);
    }
    for (const uint32_t length : {0U, 15U, 17U}) {
        auto ctx = Request(*lease);
        ctx.dataLength = length;
        EXPECT_EQ(hub.HandleLocalRequest(ctx).rcode, ResponseCode::DataError);
    }
    auto ctx = Request(*lease);
    ctx.writePayload = std::span<const uint8_t>(frame).first(15);
    EXPECT_EQ(hub.HandleLocalRequest(ctx).rcode, ResponseCode::DataError);
    const std::array<uint8_t, 17> longFrame{};
    ctx.writePayload = longFrame;
    EXPECT_EQ(hub.HandleLocalRequest(ctx).rcode, ResponseCode::DataError);
    EXPECT_FALSE(hub.Take(*lease));
}

TEST_F(SPro40D3NotificationHubTests, UnallocatedInteriorAndOutsideOffsetsNeverEnqueue) {
    const auto lease = hub.Allocate(Bind());
    ASSERT_TRUE(lease);
    auto ctx = Request(*lease);
    ++ctx.destOffset;
    EXPECT_EQ(hub.HandleLocalRequest(ctx).rcode, ResponseCode::AddressError);
    ctx.destOffset = 0x10F0;
    EXPECT_EQ(hub.HandleLocalRequest(ctx).rcode, ResponseCode::AddressError);
    ctx.destOffset = 0x1100;
    EXPECT_FALSE(hub.HandleLocalRequest(ctx).claimed);
    ctx.destOffset = 0xFFF;
    EXPECT_FALSE(hub.HandleLocalRequest(ctx).claimed);
    EXPECT_FALSE(hub.Take(*lease));
}

TEST_F(SPro40D3NotificationHubTests, BoundedQueueRejectsOverflowAndWrapsInFifoOrder) {
    const auto lease = hub.Allocate(Bind());
    ASSERT_TRUE(lease);
    for (size_t i = 0; i < Hub::kQueueCapacity; ++i) {
        clock.now = i;
        frame[0] = static_cast<uint8_t>(i);
        EXPECT_EQ(hub.HandleLocalRequest(Request(*lease)).rcode, ResponseCode::Complete);
    }
    EXPECT_EQ(hub.HandleLocalRequest(Request(*lease)).rcode, ResponseCode::ConflictError);
    for (size_t i = 0; i < Hub::kQueueCapacity / 2; ++i) {
        const auto notification = hub.Take(*lease);
        ASSERT_TRUE(notification);
        EXPECT_EQ(notification->frame[0], i);
        EXPECT_EQ(notification->receivedMs, i);
    }
    for (size_t i = Hub::kQueueCapacity; i < Hub::kQueueCapacity * 3 / 2; ++i) {
        clock.now = i;
        frame[0] = static_cast<uint8_t>(i);
        EXPECT_EQ(hub.HandleLocalRequest(Request(*lease)).rcode, ResponseCode::Complete);
    }
    for (size_t i = Hub::kQueueCapacity / 2; i < Hub::kQueueCapacity * 3 / 2; ++i) {
        const auto notification = hub.Take(*lease);
        ASSERT_TRUE(notification);
        EXPECT_EQ(notification->frame[0], i);
        EXPECT_EQ(notification->receivedMs, i);
    }
    EXPECT_FALSE(hub.Take(*lease));
}

TEST_F(SPro40D3NotificationHubTests, ReleaseClearsQueueAndLateWritesCannotReachReplacement) {
    const auto route = Bind();
    const auto old = hub.Allocate(route);
    ASSERT_TRUE(old);
    EXPECT_EQ(hub.HandleLocalRequest(Request(*old)).rcode, ResponseCode::Complete);
    hub.Release(*old);
    hub.Release(*old); // Idempotent.
    EXPECT_FALSE(hub.Take(*old));
    const auto replacement = hub.Allocate(route);
    ASSERT_TRUE(replacement);
    EXPECT_NE(replacement->offset, old->offset);
    EXPECT_EQ(hub.HandleLocalRequest(Request(*old)).rcode, ResponseCode::AddressError);
    EXPECT_FALSE(hub.Take(*replacement));
    EXPECT_EQ(hub.HandleLocalRequest(Request(*replacement)).rcode, ResponseCode::Complete);
    EXPECT_TRUE(hub.Take(*replacement));
}

TEST_F(SPro40D3NotificationHubTests, ForgedCapabilityCannotConsumeOrReleaseAnotherLease) {
    const auto route = Bind();
    const auto a = hub.Allocate(route);
    const auto b = hub.Allocate(route);
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);
    EXPECT_EQ(hub.HandleLocalRequest(Request(*b)).rcode, ResponseCode::Complete);
    auto forged = *a;
    forged.offset = b->offset;
    hub.Release(forged);
    EXPECT_FALSE(hub.Take(forged));
    forged = *b;
    ++forged.route.routeEpoch;
    hub.Release(forged);
    EXPECT_FALSE(hub.Take(forged));
    forged = *b;
    forged.id = 0;
    hub.Release(forged);
    EXPECT_FALSE(hub.Take(forged));
    EXPECT_TRUE(hub.Take(*b));
}

TEST_F(SPro40D3NotificationHubTests, ResetDropsQueuedFramesAndRejectsOldRouteAfterRebind) {
    const auto route = Bind();
    const auto lease = hub.Allocate(route);
    ASSERT_TRUE(lease);
    EXPECT_EQ(hub.HandleLocalRequest(Request(*lease)).rcode, ResponseCode::Complete);
    registry.InvalidateLiveMappingsForBusReset();
    EXPECT_FALSE(hub.Take(*lease));
    EXPECT_EQ(hub.Allocate(route).error(), kIOReturnNotReady);
    EXPECT_EQ(hub.HandleLocalRequest(Request(*lease)).rcode, ResponseCode::AddressError);
    const auto rebound = Bind(route.guid, 3, 8);
    EXPECT_NE(rebound.routeEpoch, route.routeEpoch);
    EXPECT_EQ(hub.Allocate(route).error(), kIOReturnNotReady);
    const auto next = hub.Allocate(rebound);
    ASSERT_TRUE(next);
    auto spoofed = Request(*lease);
    spoofed.generation = rebound.generation.value;
    EXPECT_EQ(hub.HandleLocalRequest(spoofed).rcode, ResponseCode::AddressError);
    EXPECT_FALSE(hub.Take(*next));
}

TEST_F(SPro40D3NotificationHubTests, SameGenerationReplugCannotValidateOldLease) {
    const auto oldRoute = Bind();
    const auto lease = hub.Allocate(oldRoute);
    ASSERT_TRUE(lease);
    EXPECT_EQ(hub.HandleLocalRequest(Request(*lease)).rcode, ResponseCode::Complete);
    registry.RetireDevice(oldRoute.guid);
    const auto replug = Bind(oldRoute.guid);
    EXPECT_NE(replug.deviceIncarnation, oldRoute.deviceIncarnation);
    EXPECT_FALSE(hub.Take(*lease));
    EXPECT_EQ(hub.HandleLocalRequest(Request(*lease)).rcode, ResponseCode::AddressError);
    EXPECT_EQ(hub.Allocate(oldRoute).error(), kIOReturnNotReady);
    const auto current = hub.Allocate(replug);
    ASSERT_TRUE(current);
    EXPECT_NE(current->offset, lease->offset);
    EXPECT_EQ(hub.HandleLocalRequest(Request(*current)).rcode, ResponseCode::Complete);
    EXPECT_TRUE(hub.Take(*current));
}

TEST_F(SPro40D3NotificationHubTests, TwoDevicesInSameGenerationRemainAttributed) {
    const auto a = hub.Allocate(Bind(0x130e0400000001ULL, 3));
    const auto b = hub.Allocate(Bind(0x130e0400000002ULL, 4));
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);
    auto wrongDevice = Request(*a);
    wrongDevice.sourceID = Request(*b).sourceID;
    EXPECT_EQ(hub.HandleLocalRequest(wrongDevice).rcode, ResponseCode::AddressError);
    frame[0] = 11;
    EXPECT_EQ(hub.HandleLocalRequest(Request(*a)).rcode, ResponseCode::Complete);
    frame[0] = 22;
    EXPECT_EQ(hub.HandleLocalRequest(Request(*b)).rcode, ResponseCode::Complete);
    const auto first = hub.Take(*a);
    const auto second = hub.Take(*b);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(first->frame[0], 11);
    EXPECT_EQ(second->frame[0], 22);
}

TEST_F(SPro40D3NotificationHubTests, ConcurrentIngressAndReleaseLeaveNoQueuedFrames) {
    const auto lease = hub.Allocate(Bind());
    ASSERT_TRUE(lease);
    const auto ctx = Request(*lease);
    std::atomic<bool> started{false};
    std::atomic<bool> released{false};
    std::atomic<bool> invalidResult{false};
    std::thread ingress([&] {
        started = true;
        for (size_t i = 0; i < 1000; ++i) {
            const auto code = hub.HandleLocalRequest(ctx).rcode;
            if (code != ResponseCode::Complete && code != ResponseCode::ConflictError &&
                code != ResponseCode::AddressError) invalidResult = true;
        }
        while (!released.load()) std::this_thread::yield();
        for (size_t i = 0; i < 100; ++i) {
            if (hub.HandleLocalRequest(ctx).rcode != ResponseCode::AddressError) {
                invalidResult = true;
            }
        }
    });
    while (!started.load()) std::this_thread::yield();
    hub.Release(*lease);
    released = true;
    ingress.join();
    EXPECT_FALSE(invalidResult.load());
    EXPECT_FALSE(hub.Take(*lease));
}

} // namespace
