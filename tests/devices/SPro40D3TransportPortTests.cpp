// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// SPro40D3TransportPortTests.cpp - P03: the native register-port adapter,
// over the scriptable RecordingFireWireBus exactly like the DICE family
// adapter tests. Pins: acceptance-vs-completion semantics (stale route at
// issue = rejection, zero I/O), genuine async status propagation, payload
// ownership (copied under the completion), and the DCP-specific CAS word
// order - the operand's first eight bytes must equal the descriptor's
// owner-none layout 00 00 00 00 FF FF 00 00 (quadlet-0-low), NOT the
// generic BE64 order.

#include "Audio/Protocols/DICE/Focusrite/SPro40D3TransportPort.hpp"
#include "../support/RecordingFireWireBus.hpp"

#include <gtest/gtest.h>

#include <optional>

namespace {

using ASFW::Testing::RecordingFireWireBus;
using ASFW::Testing::OpKind;
using ASFW::Testing::ExpectedRequest;
using ASFW::Testing::ResponseStep;
using ASFW::Testing::ByteView;
namespace D3 = ASFW::Audio::DICE::Focusrite::SPro40D3;
namespace Ports = ASFW::Protocols::Ports;
namespace FW = ASFW::FW;

/// Registry-backed route, the DICEDuplexTestSupport pattern.
struct RouteState {
    ASFW::Discovery::DeviceRegistry registry;
    ASFW::Discovery::DeviceRouteToken route{};

    RouteState() {
        ASFW::Discovery::ConfigROM rom{};
        rom.bib.guid = 0x5A03D31000000002ULL;
        rom.gen = ASFW::Discovery::Generation{1};
        rom.nodeId = 0x02;
        (void)registry.UpsertFromROM(rom, ASFW::Discovery::LinkPolicy{});
        route = *registry.CurrentRoute(rom.bib.guid);
    }
};

class PortRig {
public:
    PortRig() : io_{bus_, bus_, state_.registry, state_.route}, port_{io_} {}

    RecordingFireWireBus& bus() noexcept { return bus_; }
    D3::SPro40D3TransportPort& port() noexcept { return port_; }
    RouteState& state() noexcept { return state_; }

private:
    RecordingFireWireBus bus_;
    RouteState state_;
    Ports::ProtocolRegisterIO io_;
    D3::SPro40D3TransportPort port_;
};

// The DCP mailbox addresses from the Stage 0 capture.
constexpr uint64_t kWriteAddress = 0xFFFFE040'01C0ULL;
constexpr uint64_t kOwnerField = 0xFFFFE040'0008ULL;

TEST(SPro40D3TransportPort, WriteAcceptedMapsToBusWriteAndCompletesOnce) {
    PortRig rig;
    std::optional<std::expected<void, IOReturn>> done;
    const uint8_t request[16] = {0x80, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    const bool accepted = rig.port().WriteBlock(
        kWriteAddress, request,
        [&](std::expected<void, IOReturn> r) { done = r; });
    EXPECT_TRUE(accepted);
    ASSERT_TRUE(done.has_value());
    EXPECT_TRUE(done->has_value()); // genuine async status: success
    const auto& ops = rig.bus().Operations();
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0].kind, OpKind::Write);
    EXPECT_EQ(ops[0].addressHi, 0xFFFFu);
    EXPECT_EQ(ops[0].addressLo, 0xE04001C0u);
    ASSERT_EQ(ops[0].payload.size(), 16u);
    EXPECT_EQ(ops[0].payload[0], 0x80);
}

TEST(SPro40D3TransportPort, StaleRouteAtIssueIsRejectionWithZeroIO) {
    PortRig rig;
    rig.state().registry.InvalidateLiveMappingsForBusReset();

    bool completionFired = false;
    const uint8_t request[16]{};
    const bool accepted = rig.port().WriteBlock(
        kWriteAddress, request,
        [&](std::expected<void, IOReturn>) { completionFired = true; });
    EXPECT_FALSE(accepted);          // nothing was submitted
    EXPECT_FALSE(completionFired);  // the completion NEVER fires on rejection
    EXPECT_TRUE(rig.bus().Operations().empty()); // zero I/O

    bool casFired = false;
    EXPECT_FALSE(rig.port().CompareSwap64(
        kOwnerField, D3::kDcpOwnerNone, 0x1234,
        [&](std::expected<uint64_t, IOReturn>) { casFired = true; }));
    EXPECT_FALSE(casFired);
}

TEST(SPro40D3TransportPort, ReadCompletionCopiesPayloadIntoEngineOwnedStorage) {
    PortRig rig;
    static constexpr uint8_t kResponse[48] = {
        0x04, 0x06, 0x19, 0x73, 0x18, 0x10, 0x19, 0x66, // magic
        0, 0, 0, 0, 0xFF, 0xFF, 0, 0,                   // owner none
    };
    rig.bus().SetReadResponder([&](ASFW::Async::FWAddress, uint32_t length) {
        return std::vector<uint8_t>(kResponse, kResponse + length);
    });

    std::optional<std::expected<std::vector<uint8_t>, IOReturn>> done;
    const bool accepted = rig.port().ReadBlock(
        0xFFFFE040'0000ULL, 16,
        [&](std::expected<std::vector<uint8_t>, IOReturn> r) { done = std::move(r); });
    EXPECT_TRUE(accepted);
    ASSERT_TRUE(done.has_value());
    ASSERT_TRUE(done->has_value());
    const auto& bytes = **done;
    ASSERT_EQ(bytes.size(), 16u);
    EXPECT_EQ(bytes[0], 0x04); // copied bytes, engine-owned storage
    EXPECT_EQ(bytes[6], 0x19);
    const auto& ops = rig.bus().Operations();
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0].kind, OpKind::Read);
    EXPECT_EQ(ops[0].addressLo, 0xE0400000u);
}

TEST(SPro40D3TransportPort, ShortReadSurfacesTheShortPayloadHonestly) {
    PortRig rig;
    static constexpr std::array<ExpectedRequest, 1> kRequests{{
        {OpKind::Read, 0xFFFFu, 0xE0400030u, 16u, FW::FwSpeed::S400, 0u, {nullptr, 0u}},
    }};
    static constexpr uint8_t kShort[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    static constexpr std::array<ResponseStep, 1> kResponses{{
        {OpKind::Read, 0xFFFFu, 0xE0400030u, 16u, 0u, FW::FwSpeed::S400,
         ASFW::Async::AsyncStatus::kShortRead, {kShort, sizeof(kShort)}},
    }};
    rig.bus().SetScript(kRequests, kResponses);

    std::optional<std::expected<std::vector<uint8_t>, IOReturn>> done;
    EXPECT_TRUE(rig.port().ReadBlock(
        0xFFFFE040'0030ULL, 16,
        [&](std::expected<std::vector<uint8_t>, IOReturn> r) { done = std::move(r); }));
    ASSERT_TRUE(done.has_value());
    ASSERT_TRUE(done->has_value());
    // The short payload is passed through as-is: the ENGINE's short-response
    // rules decide its fate; the adapter neither truncates silently nor
    // invents bytes.
    const auto& shortBytes = **done;
    ASSERT_EQ(shortBytes.size(), 4u);
    EXPECT_EQ(shortBytes[0], 0xAA);
    EXPECT_EQ(shortBytes[3], 0xDD);
    EXPECT_TRUE(rig.bus().ScriptConsumed());
}

TEST(SPro40D3TransportPort, CasOperandAndResponseUseDcpQuadlet0LowOrder) {
    PortRig rig;
    // The operand WE expect on the wire for CAS(none -> ours):
    // expected 0xFFFF000000000000 stored quadlet-0-LOW first =
    // [00 00 00 00][FF FF 00 00] - byte-for-byte the descriptor's owner-none
    // layout (Stage 0 capture). The generic BE64 CAS order would be
    // [FF FF 00 00][00 00 00 00] - WRONG for this device and pinned out.
    static constexpr uint8_t kOperand[16] = {
        0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, // expected: none
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0, 0x0D, // desired: ours
    };
    static constexpr std::array<ExpectedRequest, 1> kRequests{{
        {OpKind::Lock, 0xFFFFu, 0xE0400008u, 16u, FW::FwSpeed::S400, 8u,
         {kOperand, sizeof(kOperand)}},
    }};
    // The device answers the CAS with the OLD field bytes, quadlet-0-low:
    // [low 32][high 32]. These raw bytes decode (per the D3 field order) to
    // 0x9ABCDEF0'12345678 - and NOT to the BE64 reading 0x12345678'9ABCDEF0.
    static constexpr uint8_t kOldValue[8] = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0};
    static constexpr std::array<ResponseStep, 1> kResponses{{
        {OpKind::Lock, 0xFFFFu, 0xE0400008u, 16u, 8u, FW::FwSpeed::S400,
         ASFW::Async::AsyncStatus::kSuccess, {kOldValue, sizeof(kOldValue)}},
    }};
    rig.bus().SetScript(kRequests, kResponses);

    std::optional<std::expected<uint64_t, IOReturn>> done;
    const bool accepted = rig.port().CompareSwap64(
        kOwnerField, D3::kDcpOwnerNone, 0x0000F00D00000000ULL,
        [&](std::expected<uint64_t, IOReturn> r) { done = r; });
    EXPECT_TRUE(accepted);
    ASSERT_TRUE(done.has_value());
    ASSERT_TRUE(done->has_value());
    EXPECT_EQ(*done, 0x9ABCDEF0'12345678ULL); // DCP quadlet-0-low decode
    EXPECT_TRUE(rig.bus().ScriptConsumed());

    // And the operand actually recorded on the wire:
    const auto& ops = rig.bus().Operations();
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0].kind, OpKind::Lock);
    EXPECT_EQ(ops[0].responseLength, 8u);
    ASSERT_EQ(ops[0].payload.size(), 16u);
    for (size_t i = 0; i < 16; ++i) EXPECT_EQ(ops[0].payload[i], kOperand[i]) << "byte " << i;
}

TEST(SPro40D3TransportPort, ErrorStatusPropagatesGenuinely) {
    PortRig rig;
    rig.bus().FailNext(OpKind::Write, 0xE04001C0u, ASFW::Async::AsyncStatus::kTimeout);
    std::optional<std::expected<void, IOReturn>> done;
    EXPECT_TRUE(rig.port().WriteBlock(
        kWriteAddress, std::array<uint8_t, 16>{},
        [&](std::expected<void, IOReturn> r) { done = r; }));
    ASSERT_TRUE(done.has_value());
    ASSERT_FALSE(done->has_value());
    EXPECT_EQ(done->error(), kIOReturnTimeout); // status, not a blanket error
}

} // namespace
