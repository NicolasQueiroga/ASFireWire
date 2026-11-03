// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "Audio/Protocols/DICE/Focusrite/SPro40D3Transport.hpp"
#include "../mocks/MockFireWireBus.hpp"

#include <array>
#include <barrier>
#include <functional>
#include <latch>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace {
using namespace ASFW;
namespace D3 = Audio::DICE::Focusrite::SPro40D3;
using ::testing::_;
using ::testing::Return;

class Clock final : public Audio::DICE::DiceWaitClock {
public:
    void SleepMs(uint32_t ms) noexcept override { now += ms; }
    void Backoff(uint32_t) noexcept override {
        ++backoffs;
        ++now;
        if (onBackoff) onBackoff();
    }
    uint64_t NowMs() const noexcept override {
        if (onNow) onNow();
        return now;
    }
    uint64_t now{};
    unsigned backoffs{};
    std::function<void()> onBackoff;
    std::function<void()> onNow;
};

enum class Op { Read, Write, Cas };

class SPro40D3TransportTests : public ::testing::TestWithParam<Op> {
protected:
    void SetUp() override {
        Discovery::ConfigROM rom{};
        rom.bib.guid = 0xD1CE000000000002ULL;
        rom.gen = FW::Generation{19};
        rom.nodeId = 7;
        (void)registry->UpsertFromROM(rom, Discovery::LinkPolicy{});
        route = *registry->CurrentRoute(rom.bib.guid);
        ON_CALL(bus, GetSpeed(FW::NodeId{7})).WillByDefault(Return(FW::FwSpeed::S400));
        registerIO = std::make_unique<Protocols::Ports::ProtocolRegisterIO>(bus, bus, *registry, route);
        transport = MakeTransport();
    }
    std::unique_ptr<D3::SPro40D3Transport> MakeTransport(uint32_t timeout = 5) {
        return std::make_unique<D3::SPro40D3Transport>(*registerIO, clock, timeout);
    }
    static constexpr uint64_t address = 0xFFFFE0400008ULL;
    static constexpr std::array<uint8_t, 4> bytes{0x12, 0x34, 0x56, 0x78};
    static constexpr Async::AsyncHandle handle{123};

    IOReturn Invoke(Op op) {
        switch (op) {
        case Op::Read: {
            const auto result = transport->ReadBlock(address, 4);
            return result ? kIOReturnSuccess : result.error();
        }
        case Op::Write: {
            const auto result = transport->WriteBlock(address, bytes);
            return result ? kIOReturnSuccess : result.error();
        }
        case Op::Cas: {
            const auto result = transport->CompareSwap64(address, 1, 2);
            return result ? kIOReturnSuccess : result.error();
        }
        }
        return kIOReturnError;
    }
    uint32_t ReplyLength(Op op) const { return op == Op::Read ? 4 : op == Op::Cas ? 8 : 0; }

    // All transactions pass through an IFireWireBusOps mock. Real registry
    // tokens are used, including generation/epoch/incarnation checks.
    void ExpectSubmit(Op op, std::function<void(Async::InterfaceCompletionCallback)> action,
                      Async::AsyncHandle returned = handle) {
        switch (op) {
        case Op::Read:
            EXPECT_CALL(bus, ReadBlock(route.generation, FW::NodeId{7}, _, 4, FW::FwSpeed::S400, _))
                .WillOnce([action, returned](auto, auto, auto target, auto, auto, auto cb) {
                    CheckAddress(target);
                    action(std::move(cb));
                    return returned;
                });
            break;
        case Op::Write:
            EXPECT_CALL(bus, WriteBlock(route.generation, FW::NodeId{7}, _, _, FW::FwSpeed::S400, _))
                .WillOnce([action, returned](auto, auto, auto target, auto data, auto, auto cb) {
                    CheckAddress(target);
                    EXPECT_EQ(std::vector<uint8_t>(data.begin(), data.end()),
                              std::vector<uint8_t>(bytes.begin(), bytes.end()));
                    action(std::move(cb));
                    return returned;
                });
            break;
        case Op::Cas:
            EXPECT_CALL(bus, Lock(route.generation, FW::NodeId{7}, _, FW::LockOp::kCompareSwap,
                                  _, 8, FW::FwSpeed::S400, _))
                .WillOnce([action, returned](auto, auto, auto target, auto, auto data, auto, auto, auto cb) {
                    CheckAddress(target);
                    EXPECT_EQ(data.size(), 16U);
                    action(std::move(cb));
                    return returned;
                });
            break;
        }
    }
    static void CheckAddress(Async::FWAddress target) {
        EXPECT_EQ(target.addressHi, 0xFFFF);
        EXPECT_EQ(target.addressLo, 0xE0400008U);
        EXPECT_EQ(target.nodeID, 0); // node is a separate bus argument, not packed into address
    }
    testing::NiceMock<Async::Mocks::MockFireWireBus> bus;
    std::unique_ptr<Discovery::DeviceRegistry> registry = std::make_unique<Discovery::DeviceRegistry>();
    Discovery::DeviceRouteToken route;
    Clock clock;
    std::unique_ptr<Protocols::Ports::ProtocolRegisterIO> registerIO;
    std::unique_ptr<D3::SPro40D3Transport> transport;
};

TEST_P(SPro40D3TransportTests, SynchronousCompletionBeforeEmptyHandleIsValid) {
    const std::vector<uint8_t> reply(ReplyLength(GetParam()), 0xAB);
    ExpectSubmit(GetParam(), [&](auto cb) { cb(Async::AsyncStatus::kSuccess, reply); }, {});
    EXPECT_EQ(Invoke(GetParam()), kIOReturnSuccess);
    EXPECT_EQ(clock.backoffs, 0U);
}

TEST_P(SPro40D3TransportTests, DeferredCompletionSucceedsBeforeDeadline) {
    Async::InterfaceCompletionCallback saved;
    ExpectSubmit(GetParam(), [&](auto cb) { saved = std::move(cb); });
    clock.onBackoff = [&] {
        const std::vector<uint8_t> reply(ReplyLength(GetParam()), 0x5A);
        auto cb = std::exchange(saved, {});
        cb(Async::AsyncStatus::kSuccess, reply);
    };
    EXPECT_EQ(Invoke(GetParam()), kIOReturnSuccess);
    EXPECT_EQ(clock.backoffs, 1U);
}

TEST_P(SPro40D3TransportTests, InvalidHandleWithoutCompletionFailsImmediately) {
    ExpectSubmit(GetParam(), [](auto) {}, {});
    EXPECT_EQ(Invoke(GetParam()), kIOReturnError);
    EXPECT_EQ(clock.backoffs, 0U);
    EXPECT_EQ(clock.now, 0U);
}

TEST_P(SPro40D3TransportTests, AsyncErrorsAreMappedWithoutRetry) {
    const std::array cases{
        std::pair{Async::AsyncStatus::kTimeout, kIOReturnTimeout},
        std::pair{Async::AsyncStatus::kShortRead, kIOReturnUnderrun},
        std::pair{Async::AsyncStatus::kBusyRetryExhausted, kIOReturnBusy},
        std::pair{Async::AsyncStatus::kAborted, kIOReturnAborted},
        std::pair{Async::AsyncStatus::kHardwareError, kIOReturnError},
        std::pair{Async::AsyncStatus::kLockCompareFail, kIOReturnExclusiveAccess},
        std::pair{Async::AsyncStatus::kStaleGeneration, kIOReturnOffline}};
    for (const auto& [status, error] : cases) {
        ExpectSubmit(GetParam(), [status](auto cb) { cb(status, {}); });
        EXPECT_EQ(Invoke(GetParam()), error);
    }
}

TEST_P(SPro40D3TransportTests, OversizedResponseIsNotSilentlyTruncated) {
    const std::vector<uint8_t> reply(ReplyLength(GetParam()) + 4, 0);
    ExpectSubmit(GetParam(), [&](auto cb) { cb(Async::AsyncStatus::kSuccess, reply); });
    EXPECT_EQ(Invoke(GetParam()), kIOReturnOverrun);
}

TEST_P(SPro40D3TransportTests, StaleRouteIsRejectedWithoutSubmitting) {
    registry->InvalidateLiveMappingsForBusReset();
    EXPECT_CALL(bus, ReadBlock(_, _, _, _, _, _)).Times(0);
    EXPECT_CALL(bus, WriteBlock(_, _, _, _, _, _)).Times(0);
    EXPECT_CALL(bus, Lock(_, _, _, _, _, _, _, _)).Times(0);
    EXPECT_EQ(Invoke(GetParam()), kIOReturnOffline);
}

TEST_P(SPro40D3TransportTests, RouteInvalidatedBeforeCompletionRejectsEvenSuccessfulReply) {
    Async::InterfaceCompletionCallback saved;
    ExpectSubmit(GetParam(), [&](auto cb) { saved = std::move(cb); });
    clock.onBackoff = [&] {
        registry->InvalidateLiveMappingsForBusReset();
        const std::vector<uint8_t> reply(ReplyLength(GetParam()), 0);
        auto cb = std::exchange(saved, {});
        cb(Async::AsyncStatus::kSuccess, reply);
    };
    EXPECT_EQ(Invoke(GetParam()), kIOReturnOffline);
    EXPECT_EQ(clock.backoffs, 1U);
}

TEST_P(SPro40D3TransportTests, RouteInvalidationWithoutCompletionCancelsPromptly) {
    Async::InterfaceCompletionCallback saved;
    // Only the selected operation may be submitted, exactly once.
    EXPECT_CALL(bus, ReadBlock(_, _, _, _, _, _)).Times(0);
    EXPECT_CALL(bus, WriteBlock(_, _, _, _, _, _)).Times(0);
    EXPECT_CALL(bus, Lock(_, _, _, _, _, _, _, _)).Times(0);
    ExpectSubmit(GetParam(), [&](auto cb) { saved = std::move(cb); });
    clock.onBackoff = [&] { registry->InvalidateLiveMappingsForBusReset(); };
    EXPECT_CALL(bus, Cancel(_)).WillOnce([](auto cancelled) {
        EXPECT_EQ(cancelled.value, handle.value);
        return false; // The bus may still deliver the held callback later.
    });
    EXPECT_EQ(Invoke(GetParam()), kIOReturnOffline);
    EXPECT_EQ(clock.backoffs, 1U);
    EXPECT_EQ(clock.now, 1U);
    ASSERT_TRUE(saved);
    transport.reset();
    const std::vector<uint8_t> reply(ReplyLength(GetParam()), 0xEF);
    saved(Async::AsyncStatus::kSuccess, reply);
}

TEST_P(SPro40D3TransportTests, TimeoutDetachesCallbackBeforeAdapterAndRegistryDestruction) {
    Async::InterfaceCompletionCallback saved;
    ExpectSubmit(GetParam(), [&](auto cb) { saved = std::move(cb); });
    EXPECT_CALL(bus, Cancel(_)).WillOnce([](auto cancelled) {
        EXPECT_EQ(cancelled.value, handle.value);
        return false; // cancellation need not suppress the late callback
    });
    EXPECT_EQ(Invoke(GetParam()), kIOReturnTimeout);
    EXPECT_EQ(clock.now, 5U);
    transport.reset();
    registerIO.reset();
    registry.reset(); // late native callback must not even attempt IsCurrent
    const std::vector<uint8_t> reply(ReplyLength(GetParam()), 0xEF);
    saved(Async::AsyncStatus::kSuccess, reply);
    saved = {};
}

TEST_P(SPro40D3TransportTests, CancelCanCompleteSynchronouslyWithoutChangingTimeoutOutcome) {
    Async::InterfaceCompletionCallback saved;
    ExpectSubmit(GetParam(), [&](auto cb) { saved = std::move(cb); });
    EXPECT_CALL(bus, Cancel(_)).WillOnce([&](auto) {
        saved(Async::AsyncStatus::kAborted, {});
        return true;
    });
    EXPECT_EQ(Invoke(GetParam()), kIOReturnTimeout);
}

TEST_P(SPro40D3TransportTests, ConcurrentDuplicateCompletionsCannotOverwritePublishedResult) {
    const std::vector<uint8_t> reply(ReplyLength(GetParam()), 0x5A);
    ExpectSubmit(GetParam(), [&](auto cb) {
        cb(Async::AsyncStatus::kSuccess, reply);
        std::barrier start{3};
        std::thread first([&] {
            start.arrive_and_wait();
            cb(Async::AsyncStatus::kHardwareError, {});
        });
        std::thread second([&] {
            start.arrive_and_wait();
            cb(Async::AsyncStatus::kAborted, {});
        });
        start.arrive_and_wait();
        first.join();
        second.join();
    });
    EXPECT_EQ(Invoke(GetParam()), kIOReturnSuccess);
}

TEST_P(SPro40D3TransportTests, ConcurrentInitialCompletionsPublishOneWholeResult) {
    const std::vector<uint8_t> reply(ReplyLength(GetParam()), 0xA5);
    ExpectSubmit(GetParam(), [&](auto cb) {
        std::barrier start{3};
        std::thread first([&] {
            start.arrive_and_wait();
            cb(Async::AsyncStatus::kSuccess, reply);
        });
        std::thread second([&] {
            start.arrive_and_wait();
            cb(Async::AsyncStatus::kSuccess, reply);
        });
        start.arrive_and_wait();
        first.join();
        second.join();
    });
    EXPECT_EQ(Invoke(GetParam()), kIOReturnSuccess);
}

TEST_P(SPro40D3TransportTests, CompletionBetweenDeadlineObservationAndAbandonmentWins) {
    Async::InterfaceCompletionCallback saved;
    ExpectSubmit(GetParam(), [&](auto cb) { saved = std::move(cb); });
    bool completed = false;
    const std::vector<uint8_t> reply(ReplyLength(GetParam()), 0x5A);
    clock.onBackoff = [&] {
        if (clock.now == 5) {
            // Run has already polled Done when it next asks for the deadline.
            clock.onNow = [&] {
                if (std::exchange(completed, true)) return;
                saved(Async::AsyncStatus::kSuccess, reply);
            };
        }
    };
    EXPECT_CALL(bus, Cancel(_)).Times(0);
    EXPECT_EQ(Invoke(GetParam()), kIOReturnSuccess);
    EXPECT_TRUE(completed);
    EXPECT_EQ(clock.now, 5U);
}

TEST_P(SPro40D3TransportTests, SuccessAlsoDetachesDuplicateAfterRegistryDestruction) {
    Async::InterfaceCompletionCallback saved;
    const std::vector<uint8_t> reply(ReplyLength(GetParam()), 0x5A);
    ExpectSubmit(GetParam(), [&](auto cb) {
        saved = cb;
        cb(Async::AsyncStatus::kSuccess, reply);
    });
    EXPECT_EQ(Invoke(GetParam()), kIOReturnSuccess);
    transport.reset();
    registerIO.reset();
    registry.reset();
    saved(Async::AsyncStatus::kSuccess, reply);
}

TEST_P(SPro40D3TransportTests, EmptyHandleDetachesEvenIfNativeCallbackWasRetained) {
    Async::InterfaceCompletionCallback saved;
    ExpectSubmit(GetParam(), [&](auto cb) { saved = std::move(cb); }, {});
    EXPECT_EQ(Invoke(GetParam()), kIOReturnError);
    transport.reset();
    registerIO.reset();
    registry.reset();
    const std::vector<uint8_t> reply(ReplyLength(GetParam()), 0x5A);
    saved(Async::AsyncStatus::kSuccess, reply);
}

INSTANTIATE_TEST_SUITE_P(BusOperations, SPro40D3TransportTests,
                        ::testing::Values(Op::Read, Op::Write, Op::Cas));

TEST_F(SPro40D3TransportTests, RouteFenceDoesNotHoldLockAcrossExternalCompletion) {
    auto fence = std::make_shared<Protocols::Ports::ProtocolRegisterIO::CompletionRouteGuard>();
    Async::InterfaceCompletionCallback saved;
    ExpectSubmit(Op::Read, [&](auto cb) { saved = std::move(cb); });
    std::latch entered{1};
    std::latch release{1};
    const auto submitted = registerIO->ReadBlock(
        Async::FWAddress{Async::FWAddress::AddressParts{.addressHi = 0xFFFF, .addressLo = 0xE0400008}},
        4, [&](auto status, auto payload) {
            EXPECT_EQ(status, Async::AsyncStatus::kSuccess);
            EXPECT_EQ(payload.size(), 4U);
            fence->Close(); // reentry must not deadlock
            entered.count_down();
            release.wait();
        }, std::nullopt, fence);
    EXPECT_EQ(submitted.value, handle.value);
    std::thread completion([&] { saved(Async::AsyncStatus::kSuccess, bytes); });
    entered.wait();
    // Close must return even though external completion is still blocked.
    fence->Close();
    transport.reset();
    registerIO.reset();
    registry.reset();
    release.count_down();
    completion.join();
    saved(Async::AsyncStatus::kSuccess, bytes); // detached duplicate after destruction
}

TEST_F(SPro40D3TransportTests, ConcurrentDuplicateReadsPreserveFirstPublishedPayload) {
    ExpectSubmit(Op::Read, [&](auto cb) {
        cb(Async::AsyncStatus::kSuccess, bytes);
        std::barrier start{3};
        std::thread first([&] {
            const std::array<uint8_t, 4> duplicate{1, 1, 1, 1};
            start.arrive_and_wait();
            cb(Async::AsyncStatus::kSuccess, duplicate);
        });
        std::thread second([&] {
            const std::array<uint8_t, 4> duplicate{2, 2, 2, 2};
            start.arrive_and_wait();
            cb(Async::AsyncStatus::kSuccess, duplicate);
        });
        start.arrive_and_wait();
        first.join();
        second.join();
    });
    const auto result = transport->ReadBlock(address, 4);
    ASSERT_TRUE(result);
    EXPECT_EQ(*result, (std::vector<uint8_t>(bytes.begin(), bytes.end())));
}

TEST_F(SPro40D3TransportTests, ReadCopiesTransientPayloadAndRejectsShortReply) {
    ExpectSubmit(Op::Read, [](auto cb) {
        std::array<uint8_t, 4> reply{1, 2, 3, 4};
        cb(Async::AsyncStatus::kSuccess, reply);
        reply.fill(0);
    });
    const auto result = transport->ReadBlock(address, 4);
    ASSERT_TRUE(result);
    EXPECT_EQ(*result, (std::vector<uint8_t>{1, 2, 3, 4}));
    ExpectSubmit(Op::Read, [](auto cb) { cb(Async::AsyncStatus::kSuccess, {}); });
    EXPECT_EQ(Invoke(Op::Read), kIOReturnUnderrun);
}

TEST_F(SPro40D3TransportTests, CasUsesLowQuadletFirstAndReturnsForeignPreviousWithoutTakeover) {
    const std::vector<uint8_t> operands{
        0x55, 0x66, 0x77, 0x88, 0x11, 0x22, 0x33, 0x44,
        0xDD, 0xEE, 0xFF, 0x00, 0x99, 0xAA, 0xBB, 0xCC};
    const std::array<uint8_t, 8> previous{0x01, 0x23, 0x45, 0x67, 0xFC, 0xDE, 0xBA, 0x98};
    EXPECT_CALL(bus, Lock(route.generation, FW::NodeId{7}, _, FW::LockOp::kCompareSwap,
                          _, 8, FW::FwSpeed::S400, _))
        .WillOnce([&](auto, auto, auto target, auto, auto data, auto, auto, auto cb) {
            CheckAddress(target);
            EXPECT_EQ(std::vector<uint8_t>(data.begin(), data.end()), operands);
            cb(Async::AsyncStatus::kSuccess, previous);
            return Async::AsyncHandle{};
        });
    const auto result = transport->CompareSwap64(address, 0x1122334455667788ULL,
                                                0x99AABBCCDDEEFF00ULL);
    ASSERT_TRUE(result);
    EXPECT_EQ(*result, 0xFCDEBA9801234567ULL);
}

TEST_F(SPro40D3TransportTests, CasRejectsQuadletAlignedButNotEightByteAlignedAddress) {
    EXPECT_CALL(bus, Lock(_, _, _, _, _, _, _, _)).Times(0);
    const auto result = transport->CompareSwap64(address + 4, 1, 2);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), kIOReturnBadArgument);
}

TEST_F(SPro40D3TransportTests, ShortCasReplyIsRejected) {
    ExpectSubmit(Op::Cas, [](auto cb) { cb(Async::AsyncStatus::kSuccess, bytes); });
    EXPECT_EQ(Invoke(Op::Cas), kIOReturnUnderrun);
}

TEST_F(SPro40D3TransportTests, RequestStorageIsCopiedAndRetainedThroughTimeoutAndDestruction) {
    Async::InterfaceCompletionCallback saved;
    std::span<const uint8_t> submitted;
    std::vector<uint8_t> caller{1, 2, 3, 4};
    EXPECT_CALL(bus, WriteBlock(_, _, _, _, _, _))
        .WillOnce([&](auto, auto, auto, auto data, auto, auto cb) {
            EXPECT_NE(data.data(), caller.data());
            submitted = data;
            saved = std::move(cb);
            return handle;
        });
    EXPECT_CALL(bus, Cancel(_)).WillOnce(Return(false));
    const auto result = transport->WriteBlock(address, caller);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), kIOReturnTimeout);
    caller.clear();
    caller.shrink_to_fit();
    transport.reset();
    EXPECT_EQ(std::vector<uint8_t>(submitted.begin(), submitted.end()),
              (std::vector<uint8_t>{1, 2, 3, 4}));
    saved(Async::AsyncStatus::kSuccess, {});
}

TEST_F(SPro40D3TransportTests, CasOperandsRemainAliveAfterTimeout) {
    Async::InterfaceCompletionCallback saved;
    std::span<const uint8_t> submitted;
    EXPECT_CALL(bus, Lock(_, _, _, _, _, 8, _, _))
        .WillOnce([&](auto, auto, auto, auto, auto data, auto, auto, auto cb) {
            submitted = data;
            saved = std::move(cb);
            return handle;
        });
    EXPECT_CALL(bus, Cancel(_)).WillOnce(Return(false));
    EXPECT_EQ(Invoke(Op::Cas), kIOReturnTimeout);
    transport.reset();
    EXPECT_EQ(std::vector<uint8_t>(submitted.begin(), submitted.end()),
              (std::vector<uint8_t>{0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0}));
    saved(Async::AsyncStatus::kAborted, {});
}

TEST_F(SPro40D3TransportTests, ValidatesQuadletsLengthsAnd48BitAddressBoundsBeforeBusAccess) {
    EXPECT_CALL(bus, ReadBlock(_, _, _, _, _, _)).Times(0);
    EXPECT_CALL(bus, WriteBlock(_, _, _, _, _, _)).Times(0);
    EXPECT_CALL(bus, Lock(_, _, _, _, _, _, _, _)).Times(0);
    for (auto invalid : {address + 1, uint64_t{1} << 48, UINT64_MAX}) {
        EXPECT_EQ(transport->ReadBlock(invalid, 4).error(), kIOReturnBadArgument);
        EXPECT_EQ(transport->WriteBlock(invalid, bytes).error(), kIOReturnBadArgument);
        EXPECT_EQ(transport->CompareSwap64(invalid, 1, 2).error(), kIOReturnBadArgument);
    }
    for (uint32_t length : {0U, 1U, 3U, 2049U, 2052U, UINT32_MAX}) {
        EXPECT_EQ(transport->ReadBlock(address, length).error(), kIOReturnBadArgument);
        std::vector<uint8_t> request(length == UINT32_MAX ? 2052 : length);
        EXPECT_EQ(transport->WriteBlock(address, request).error(), kIOReturnBadArgument);
    }
    EXPECT_EQ(transport->ReadBlock((uint64_t{1} << 48) - 4, 8).error(), kIOReturnBadArgument);
    EXPECT_EQ(transport->CompareSwap64((uint64_t{1} << 48) - 4, 1, 2).error(), kIOReturnBadArgument);
}

TEST_F(SPro40D3TransportTests, MaximumBlockAndLastQuadletAreAccepted) {
    EXPECT_CALL(bus, ReadBlock(_, _, _, D3::SPro40D3Transport::kMaxBlockBytes, _, _))
        .WillOnce([](auto, auto, auto, auto length, auto, auto cb) {
            const std::vector<uint8_t> reply(length, 0x5A);
            cb(Async::AsyncStatus::kSuccess, reply);
            return Async::AsyncHandle{};
        });
    const auto result = transport->ReadBlock(address, D3::SPro40D3Transport::kMaxBlockBytes);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->size(), 2048U);
    EXPECT_CALL(bus, ReadBlock(_, _, _, 4, _, _))
        .WillOnce([](auto, auto, auto target, auto, auto, auto cb) {
            EXPECT_EQ(target.addressHi, 0xFFFF);
            EXPECT_EQ(target.addressLo, 0xFFFFFFFCU);
            cb(Async::AsyncStatus::kSuccess, bytes);
            return Async::AsyncHandle{};
        });
    EXPECT_TRUE(transport->ReadBlock((uint64_t{1} << 48) - 4, 4));
}

TEST_F(SPro40D3TransportTests, DefaultTimeoutIs2000Milliseconds) {
    transport = std::make_unique<D3::SPro40D3Transport>(*registerIO, clock);
    ExpectSubmit(Op::Read, [](auto) {});
    EXPECT_CALL(bus, Cancel(_)).WillOnce(Return(true));
    EXPECT_EQ(Invoke(Op::Read), kIOReturnTimeout);
    EXPECT_EQ(clock.now, 2000U);
}

} // namespace
