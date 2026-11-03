// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
#include "SPro40D3Transport.hpp"

#include "../../../../Common/WireFormat.hpp"
#include "../../../../Protocols/Ports/ProtocolRegisterIO.hpp"

#include <atomic>
#include <memory>
#include <utility>

namespace ASFW::Audio::DICE::Focusrite::SPro40D3 {
namespace {

constexpr uint64_t kAddressLimit = uint64_t{1} << 48;

bool ValidRange(uint64_t address, size_t length) noexcept {
    return address < kAddressLimit && (address % 4) == 0 &&
           length != 0 && (length % 4) == 0 &&
           length <= SPro40D3Transport::kMaxBlockBytes &&
           length <= kAddressLimit - address;
}

struct Pending {
    // The bus invokes completion exactly once. Only that callback writes the
    // result; the waiting thread reads it only after an acquire of done.
    std::atomic<bool> done{false};
    std::atomic<bool> abandoned{false};
    IOReturn status{kIOReturnError};
    std::vector<uint8_t> response;
    std::vector<uint8_t> request;
};

// DCP is low quadlet first, each quadlet big-endian, NOT a BE64 value.
// Behavioral reference: snd-firewire-ctl-services, protocols/dice/src/
// focusrite/spro40d3.rs:259-276 (16-byte compare/replacement, 8-byte lock).
void EncodeOwner(uint8_t* bytes, uint64_t value) noexcept {
    FW::WriteBE32(bytes, static_cast<uint32_t>(value));
    FW::WriteBE32(bytes + 4, static_cast<uint32_t>(value >> 32));
}

} // namespace

SPro40D3Transport::SPro40D3Transport(Async::IFireWireBusOps& busOps,
                                     Async::IFireWireBusInfo& busInfo,
                                     Discovery::DeviceRegistry& registry,
                                     Discovery::DeviceRouteToken route,
                                     DiceWaitClock& clock, uint32_t timeoutMs)
    : busOps_(busOps), busInfo_(busInfo), registry_(registry), route_(route),
      clock_(clock), timeoutMs_(timeoutMs) {}

std::expected<std::vector<uint8_t>, IOReturn>
SPro40D3Transport::Run(Operation operation, uint64_t address,
                       uint32_t responseLength, std::vector<uint8_t> request) {
    if (!route_ || !registry_.IsCurrent(route_)) {
        return std::unexpected(kIOReturnOffline);
    }
    const FW::NodeId node{static_cast<uint8_t>(route_.nodeId)};
    const Async::FWAddress target{Async::FWAddress::AddressParts{
        .addressHi = static_cast<uint16_t>(address >> 32),
        .addressLo = static_cast<uint32_t>(address)}};
    auto pending = std::make_shared<Pending>();
    pending->request = std::move(request);
    auto completion = [pending, registry = &registry_, route = route_, responseLength](
                          Async::AsyncStatus status, std::span<const uint8_t> payload) {
        if (pending->abandoned.load(std::memory_order_acquire)) return;
        IOReturn result = registry->IsCurrent(route)
            ? Protocols::Ports::MapAsyncStatusToIOReturn(status) : kIOReturnOffline;
        if (result == kIOReturnSuccess) {
            if (payload.size() < responseLength) {
                result = kIOReturnUnderrun;
            } else if (payload.size() > responseLength) {
                result = kIOReturnOverrun;
            } else {
                pending->response.assign(payload.begin(), payload.end());
            }
        }
        pending->status = result;
        pending->done.store(true, std::memory_order_release);
    };
    const auto speed = busInfo_.GetSpeed(node);
    // Recheck immediately before submission, after consulting bus policy.
    if (!registry_.IsCurrent(route_)) return std::unexpected(kIOReturnOffline);
    const uint64_t startMs = clock_.NowMs();
    Async::AsyncHandle handle;
    switch (operation) {
    case Operation::Read:
        handle = busOps_.ReadBlock(route_.generation, node, target, responseLength,
                                   speed, std::move(completion));
        break;
    case Operation::Write:
        handle = busOps_.WriteBlock(route_.generation, node, target, pending->request,
                                    speed, std::move(completion));
        break;
    case Operation::CompareSwap:
        handle = busOps_.Lock(route_.generation, node, target, FW::LockOp::kCompareSwap,
                              pending->request, responseLength, speed, std::move(completion));
        break;
    }
    // A synchronous completion may legitimately accompany an empty handle.
    // Otherwise an empty handle means submission failed; do not burn a deadline.
    if (!handle && !pending->done.load(std::memory_order_acquire)) {
        pending->abandoned.store(true, std::memory_order_release);
        return std::unexpected(kIOReturnError);
    }
    for (uint32_t attempt = 0; !pending->done.load(std::memory_order_acquire); ++attempt) {
        if (!registry_.IsCurrent(route_)) {
            pending->abandoned.store(true, std::memory_order_release);
            (void)busOps_.Cancel(handle);
            return std::unexpected(kIOReturnOffline);
        }
        if (clock_.NowMs() - startMs >= timeoutMs_) {
            pending->abandoned.store(true, std::memory_order_release);
            (void)busOps_.Cancel(handle);
            return std::unexpected(kIOReturnTimeout);
        }
        clock_.Backoff(attempt);
    }
    if (pending->status != kIOReturnSuccess) return std::unexpected(pending->status);
    return std::move(pending->response);
}

std::expected<void, IOReturn>
SPro40D3Transport::WriteBlock(uint64_t address, std::span<const uint8_t> bytes) {
    if (!ValidRange(address, bytes.size())) return std::unexpected(kIOReturnBadArgument);
    auto result = Run(Operation::Write, address, 0, {bytes.begin(), bytes.end()});
    if (!result) return std::unexpected(result.error());
    return {};
}

std::expected<std::vector<uint8_t>, IOReturn>
SPro40D3Transport::ReadBlock(uint64_t address, uint32_t length) {
    if (!ValidRange(address, length)) return std::unexpected(kIOReturnBadArgument);
    return Run(Operation::Read, address, length, {});
}

std::expected<uint64_t, IOReturn>
SPro40D3Transport::CompareSwap64(uint64_t address, uint64_t expected, uint64_t desired) {
    if (!ValidRange(address, 8) || (address % 8) != 0) {
        return std::unexpected(kIOReturnBadArgument);
    }
    std::vector<uint8_t> operands(16);
    EncodeOwner(operands.data(), expected);
    EncodeOwner(operands.data() + 8, desired);
    auto result = Run(Operation::CompareSwap, address, 8, std::move(operands));
    if (!result) return std::unexpected(result.error());
    return uint64_t{FW::ReadBE32(result->data())} |
           (uint64_t{FW::ReadBE32(result->data() + 4)} << 32);
}

} // namespace ASFW::Audio::DICE::Focusrite::SPro40D3
