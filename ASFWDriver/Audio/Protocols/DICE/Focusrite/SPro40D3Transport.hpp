// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
#pragma once

#include "SPro40D3MailboxEngine.hpp"
#include "../Core/DiceWaitClock.hpp"
#include "../../../../Async/Interfaces/IFireWireBusInfo.hpp"
#include "../../../../Async/Interfaces/IFireWireBusOps.hpp"
#include "../../../../Discovery/DeviceRegistry.hpp"

namespace ASFW::Audio::DICE::Focusrite::SPro40D3 {

/// One immutable route, one async transaction per call, no retries.
/// Call and destroy on the owning serialized control queue, NEVER the bus
/// Default/completion queue: these calls block with a bounded wait. Replace the
/// adapter after rediscovery rather than updating its route. Borrowed bus/info,
/// registry and clock must outlive calls; registry must also outlive callbacks.
/// Late callbacks own their state and never access this adapter or caller spans.
class SPro40D3Transport final : public ISPro40D3Transport {
public:
    static constexpr uint32_t kDefaultTimeoutMs = 2000;
    static constexpr uint32_t kMaxBlockBytes = 2048;

    SPro40D3Transport(Async::IFireWireBusOps& busOps,
                     Async::IFireWireBusInfo& busInfo,
                     Discovery::DeviceRegistry& registry,
                     Discovery::DeviceRouteToken route,
                     DiceWaitClock& clock = DriverKitWaitClock::Shared(),
                     uint32_t timeoutMs = kDefaultTimeoutMs);

    [[nodiscard]] std::expected<void, IOReturn>
    WriteBlock(uint64_t address, std::span<const uint8_t> bytes) override;
    [[nodiscard]] std::expected<std::vector<uint8_t>, IOReturn>
    ReadBlock(uint64_t address, uint32_t length) override;
    [[nodiscard]] std::expected<uint64_t, IOReturn>
    CompareSwap64(uint64_t address, uint64_t expected, uint64_t desired) override;

private:
    enum class Operation { Read, Write, CompareSwap };
    [[nodiscard]] std::expected<std::vector<uint8_t>, IOReturn>
    Run(Operation operation, uint64_t address, uint32_t responseLength,
        std::vector<uint8_t> request);

    Async::IFireWireBusOps& busOps_;
    Async::IFireWireBusInfo& busInfo_;
    Discovery::DeviceRegistry& registry_;
    const Discovery::DeviceRouteToken route_;
    DiceWaitClock& clock_;
    const uint32_t timeoutMs_;
};

} // namespace ASFW::Audio::DICE::Focusrite::SPro40D3
