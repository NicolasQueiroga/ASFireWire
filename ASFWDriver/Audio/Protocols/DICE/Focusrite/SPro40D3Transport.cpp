// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
#include "SPro40D3Transport.hpp"

#include "../../../../Common/WireFormat.hpp"

#include <array>
#include <memory>
#include <utility>
#include <vector>

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
    // Native callbacks keep this storage alive even after the caller detaches.
    // One lock arbitrates completion vs abandonment and publishes the entire
    // result exactly once; duplicates never mutate already-published vectors.
    Pending() : lock(IOLockAlloc()) {}
    ~Pending() { if (lock.IsValid()) IOLockFree(lock.Raw()); }
    Pending(const Pending&) = delete;
    Pending& operator=(const Pending&) = delete;

    void Complete(Async::AsyncStatus status, std::span<const uint8_t> payload,
                  uint32_t responseLength) {
        Async::ScopedLock guard(lock);
        if (done || abandoned) return;
        result = Protocols::Ports::MapAsyncStatusToIOReturn(status);
        if (result == kIOReturnSuccess) {
            if (payload.size() < responseLength) result = kIOReturnUnderrun;
            else if (payload.size() > responseLength) result = kIOReturnOverrun;
            else response.assign(payload.begin(), payload.end());
        }
        done = true;
    }
    [[nodiscard]] bool Done() {
        Async::ScopedLock guard(lock);
        return done;
    }
    // A completion that already published wins over the caller's deadline.
    [[nodiscard]] bool Abandon() {
        Async::ScopedLock guard(lock);
        if (done) return false;
        abandoned = true;
        return true;
    }
    [[nodiscard]] std::expected<std::vector<uint8_t>, IOReturn> TakeResult() {
        Async::ScopedLock guard(lock);
        if (result != kIOReturnSuccess) return std::unexpected(result);
        return std::move(response);
    }

    Async::IOLockWrapper lock;
    bool done{false};
    bool abandoned{false};
    IOReturn result{kIOReturnError};
    std::vector<uint8_t> response;
    std::vector<uint8_t> request;
};

[[nodiscard]] Async::FWAddress ToAddress(uint64_t address) noexcept {
    return Async::FWAddress{Async::FWAddress::AddressParts{
        .addressHi = static_cast<uint16_t>(address >> 32),
        .addressLo = static_cast<uint32_t>(address)}};
}

} // namespace

SPro40D3Transport::SPro40D3Transport(Protocols::Ports::ProtocolRegisterIO& registerIO,
                                     DiceWaitClock& clock, uint32_t timeoutMs)
    : registerIO_(registerIO), clock_(clock), timeoutMs_(timeoutMs) {}

SPro40D3Transport::SPro40D3Transport(Async::IFireWireBusOps& busOps,
                                     Async::IFireWireBusInfo& busInfo,
                                     Discovery::DeviceRegistry& registry,
                                     Discovery::DeviceRouteToken route,
                                     DiceWaitClock& clock, uint32_t timeoutMs)
    : ownedRegisterIO_(std::make_unique<Protocols::Ports::ProtocolRegisterIO>(
          busOps, busInfo, registry, route))
    , registerIO_(*ownedRegisterIO_)
    , clock_(clock)
    , timeoutMs_(timeoutMs) {}

std::expected<std::vector<uint8_t>, IOReturn>
SPro40D3Transport::Run(Operation operation, uint64_t address,
                       uint32_t responseLength, std::vector<uint8_t> request) {
    if (admissionClosed_ || !registerIO_.IsRouteCurrent())
        return std::unexpected(kIOReturnOffline);

    const auto target = ToAddress(address);
    auto pending = std::make_shared<Pending>();
    auto completionGuard = std::make_shared<Protocols::Ports::ProtocolRegisterIO::CompletionRouteGuard>();
    if (!pending->lock.IsValid() || !completionGuard->IsValid()) {
        return std::unexpected(kIOReturnNoMemory);
    }
    pending->request = std::move(request);
    auto completion = [pending, responseLength](Async::AsyncStatus status,
                                                std::span<const uint8_t> payload) {
        pending->Complete(status, payload, responseLength);
    };

    const uint64_t startMs = clock_.NowMs();
    if (admissionClosed_) {
        completionGuard->Close();
        return std::unexpected(kIOReturnOffline);
    }
    Async::AsyncHandle handle;
    switch (operation) {
    case Operation::Read:
        handle = registerIO_.ReadBlock(target, responseLength, std::move(completion),
                                       std::nullopt, completionGuard);
        break;
    case Operation::Write:
        handle = registerIO_.WriteBlockWithResponse(target, pending->request,
                                                    std::move(completion),
                                                    std::nullopt, completionGuard);
        break;
    case Operation::CompareSwap: {
        // The canonical DCP-owner API is intentionally used here rather than
        // the generic BE64 CAS: the owner wire word is low BE32 then high BE32.
        const uint64_t expected = uint64_t{FW::ReadBE32(pending->request.data())} |
            (uint64_t{FW::ReadBE32(pending->request.data() + sizeof(uint32_t))} << 32);
        const uint64_t desired = uint64_t{FW::ReadBE32(pending->request.data() + 2 * sizeof(uint32_t))} |
            (uint64_t{FW::ReadBE32(pending->request.data() + 3 * sizeof(uint32_t))} << 32);
        handle = registerIO_.CompareSwap64DcpOwnerRaw(target, expected, desired,
                                                       std::move(completion),
                                                       std::nullopt, completionGuard);
        break;
    }
    }

    // A synchronous completion may legitimately accompany an empty handle.
    // Otherwise an empty handle means submission failed; do not burn a deadline.
    if (!handle && pending->Abandon()) {
        completionGuard->Close();
        return std::unexpected(kIOReturnError);
    }
    for (uint32_t attempt = 0; !pending->Done(); ++attempt) {
        IOReturn reason = kIOReturnSuccess;
        if (!registerIO_.IsRouteCurrent()) reason = kIOReturnOffline;
        else if (clock_.NowMs() - startMs >= timeoutMs_) reason = kIOReturnTimeout;
        if (reason != kIOReturnSuccess) {
            if (!pending->Abandon()) break; // completion won the publication race
            // Detach before Cancel, which may synchronously deliver completion.
            // Close drains registry checks, not hardware or external callbacks.
            // No lock is held across Cancel; shared buffers survive late replies.
            completionGuard->Close();
            (void)registerIO_.CancelTransaction(handle);
            return std::unexpected(reason);
        }
        clock_.Backoff(attempt);
    }
    completionGuard->Close();
    return pending->TakeResult();
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
    FW::WriteBE32(operands.data(), static_cast<uint32_t>(expected));
    FW::WriteBE32(operands.data() + sizeof(uint32_t), static_cast<uint32_t>(expected >> 32));
    FW::WriteBE32(operands.data() + 2 * sizeof(uint32_t), static_cast<uint32_t>(desired));
    FW::WriteBE32(operands.data() + 3 * sizeof(uint32_t), static_cast<uint32_t>(desired >> 32));
    auto result = Run(Operation::CompareSwap, address, 8, std::move(operands));
    if (!result) return std::unexpected(result.error());
    return uint64_t{FW::ReadBE32(result->data())} |
           (uint64_t{FW::ReadBE32(result->data() + sizeof(uint32_t))} << 32);
}

} // namespace ASFW::Audio::DICE::Focusrite::SPro40D3
