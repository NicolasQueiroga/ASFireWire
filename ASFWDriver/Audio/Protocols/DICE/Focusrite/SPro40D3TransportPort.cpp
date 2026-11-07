// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "SPro40D3TransportPort.hpp"

namespace ASFW::Audio::DICE::Focusrite::SPro40D3 {

bool SPro40D3TransportPort::WriteBlock(uint64_t address, std::span<const uint8_t> bytes,
                                       WriteDone done) {
    // Stale route at issue: nothing left the host. Non-submission is
    // established at the seam; the engine keeps prior state standing.
    if (!io_.IsRouteCurrent()) return false;
    // The port's completion slots are std::function (copyable); the engine's
    // completion is move-only. A shared holder bridges them and retains the
    // completion until it fires exactly once.
    auto holder = std::make_shared<WriteDone>(std::move(done));
    io_.WriteBlock(FwAddressFor(address), bytes,
                   [holder](Async::AsyncStatus status) {
                       // Zero expected response payload for block writes: the
                       // completion carries status only.
                       (*holder)(status == Async::AsyncStatus::kSuccess
                                     ? std::expected<void, IOReturn>{}
                                     : std::unexpected(
                                           Protocols::Ports::MapAsyncStatusToIOReturn(status)));
                   });
    return true;
}

bool SPro40D3TransportPort::ReadBlock(uint64_t address, uint32_t length, ReadDone done) {
    if (!io_.IsRouteCurrent()) return false;
    auto holder = std::make_shared<ReadDone>(std::move(done));
    io_.ReadBlock(FwAddressFor(address), length,
                  [holder, length](Async::AsyncStatus status,
                                   std::span<const uint8_t> payload) {
                      if (status != Async::AsyncStatus::kSuccess &&
                          status != Async::AsyncStatus::kShortRead) {
                          (*holder)(std::unexpected(Protocols::Ports::MapAsyncStatusToIOReturn(
                              status)));
                          return;
                      }
                      // Copy under the completion: the payload span is owned
                      // by the bus layer and does not survive this callback.
                      // A short payload is passed through honestly (the
                      // engine's short-response rules decide its fate); a
                      // successful payload shorter than the request is also
                      // surfaced as-is for the same reason.
                      std::vector<uint8_t> out(payload.begin(), payload.end());
                      (void)length;
                      (*holder)(std::expected<std::vector<uint8_t>, IOReturn>{std::move(out)});
                  });
    return true;
}

bool SPro40D3TransportPort::CompareSwap64(uint64_t address, uint64_t expected, uint64_t desired,
                                          CasDone done) {
    if (!io_.IsRouteCurrent()) return false;
    // Engine-owned operand bytes, DCP quadlet-0-low order (see header).
    std::array<uint8_t, 16> operand{};
    StoreDcpQuadlets(operand.data(), expected);
    StoreDcpQuadlets(operand.data() + 8, desired);
    auto holder = std::make_shared<CasDone>(std::move(done));
    io_.LockCompareSwapRaw(
        FwAddressFor(address), std::span<const uint8_t>(operand.data(), operand.size()),
        sizeof(uint64_t),
        [holder](Async::AsyncStatus status,
                 std::span<const uint8_t> payload) {
            if (status != Async::AsyncStatus::kSuccess) {
                (*holder)(std::unexpected(Protocols::Ports::MapAsyncStatusToIOReturn(status)));
                return;
            }
            if (payload.size() < sizeof(uint64_t)) {
                (*holder)(std::unexpected(kIOReturnUnderrun));
                return;
            }
            (*holder)(std::expected<uint64_t, IOReturn>{LoadDcpQuadlets(payload.data())});
        });
    return true;
}

} // namespace ASFW::Audio::DICE::Focusrite::SPro40D3
