// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
#pragma once

#include "SPro40D3MailboxEngine.hpp"
#include "../Core/DiceWaitClock.hpp"
#include "../../../../Async/Interfaces/IFireWireBusInfo.hpp"
#include "../../../../Async/Interfaces/IFireWireBusOps.hpp"
#include "../../../../Discovery/DeviceRegistry.hpp"
#include "../../../../Protocols/Ports/ProtocolRegisterIO.hpp"

#include <memory>

namespace ASFW::Audio::DICE::Focusrite::SPro40D3 {

/// One immutable route, one async transaction per call, no retries.
/// Call and destroy on the owning serialized control queue, NEVER the bus
/// Default/completion queue: these calls block with a bounded wait. Replace the
/// adapter after rediscovery rather than updating its route. Borrowed bus/info,
/// registry and clock must outlive active calls, not detached native callbacks.
/// Each call closes its registry fence before returning or requesting cancellation.
/// Cancellation detaches callbacks, not necessarily the hardware transaction.
/// Late callbacks own their buffers and never access the registry, this adapter,
/// or caller spans. Concurrent/duplicate completions publish at most one result.
class SPro40D3Transport final : public ISPro40D3Transport {
public:
    static constexpr uint32_t kDefaultTimeoutMs = 2000;
    static constexpr uint32_t kMaxBlockBytes = 2048;

    /// Canonical production entry point. All I/O, route validation, speed
    /// selection, and cancellation go through ProtocolRegisterIO.
    SPro40D3Transport(Protocols::Ports::ProtocolRegisterIO& registerIO,
                     DiceWaitClock& clock = DriverKitWaitClock::Shared(),
                     uint32_t timeoutMs = kDefaultTimeoutMs);

    /// Compatibility bridge for the Stage 2A session. It owns the same
    /// ProtocolRegisterIO used by the canonical constructor; no operation in
    /// this adapter calls IFireWireBusOps directly.
    SPro40D3Transport(Async::IFireWireBusOps& busOps,
                     Async::IFireWireBusInfo& busInfo,
                     Discovery::DeviceRegistry& registry,
                     Discovery::DeviceRouteToken route,
                     DiceWaitClock& clock = DriverKitWaitClock::Shared(),
                     uint32_t timeoutMs = kDefaultTimeoutMs);

    /// Permanently close admission on the owning serialized control queue.
    /// An already submitted call drains normally, retaining its completion fence
    /// and buffers; subsequent calls fail locally even if the route stays current.
    /// Rediscovery/restart must construct a new adapter.
    void Invalidate() noexcept { admissionClosed_ = true; }

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

    std::unique_ptr<Protocols::Ports::ProtocolRegisterIO> ownedRegisterIO_;
    Protocols::Ports::ProtocolRegisterIO& registerIO_;
    DiceWaitClock& clock_;
    const uint32_t timeoutMs_;
    bool admissionClosed_{false};
};

} // namespace ASFW::Audio::DICE::Focusrite::SPro40D3
