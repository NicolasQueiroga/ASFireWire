// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// SPro40D3TransportPort.hpp - Adapts the SPro40D3 DCP mailbox engine to the
// shared register port (ProtocolRegisterIO over the real FireWire bus).
//
// P03 scope item: "genuine async status/rcode propagation". Every engine
// operation maps 1:1 onto one bus transaction: an operation the port cannot
// even attempt (stale route at issue) is a REJECTION (false; nothing was
// submitted, the completion never fires); an attempted operation completes
// exactly once with the port's genuine async status mapped to IOReturn.
// Block writes expect a zero-payload completion (the DCP transport FSM
// answers out-of-band via the notification write - Pro40d3-DCP.md).
//
// CAS WORD ORDER (DCP-specific, deliberately local): the SPro40D3 owner
// field stores its 64-bit value as QUADLET-0-LOW - the low 32 bits occupy
// the first four bytes (Stage 0 capture mailbox-descriptor-raw.txt:
// owner none = 00 00 00 00 FF FF 00 00 = 0xFFFF000000000000; spro40d3.rs
// deserialize_address). The generic port's BE64 CAS operand order
// (high quadlet first) is therefore WRONG for this device. The 16-byte
// Lock compare-swap operand is built HERE and the old value decoded HERE,
// over the generic raw-Lock passthrough (ProtocolRegisterIO::
// LockCompareSwapRaw); no vendor-named helper enters the shared port.

#pragma once

#include "SPro40D3MailboxEngine.hpp"
#include "../../../../Protocols/Ports/ProtocolRegisterIO.hpp"

namespace ASFW::Audio::DICE::Focusrite::SPro40D3 {

class SPro40D3TransportPort final : public ISPro40D3Transport {
public:
    explicit SPro40D3TransportPort(Protocols::Ports::ProtocolRegisterIO& io) noexcept
        : io_{io} {}

    [[nodiscard]] bool WriteBlock(uint64_t address, std::span<const uint8_t> bytes,
                                  WriteDone done) override;

    [[nodiscard]] bool ReadBlock(uint64_t address, uint32_t length, ReadDone done) override;

    [[nodiscard]] bool CompareSwap64(uint64_t address, uint64_t expected, uint64_t desired,
                                     CasDone done) override;

private:
    /// 48-bit DCP address -> bus address (the port supplies the node).
    [[nodiscard]] static Async::FWAddress FwAddressFor(uint64_t address) noexcept {
        Async::FWAddress out{};
        out.addressHi = static_cast<uint16_t>(address >> 32);
        out.addressLo = static_cast<uint32_t>(address);
        return out;
    }

    /// DCP 64-bit field encoding: quadlet 0 (low 32 bits) first, each
    /// quadlet big-endian (Stage 0 capture; spro40d3.rs deserialize_address).
    static void StoreDcpQuadlets(uint8_t* out, uint64_t value) noexcept {
        FW::WriteBE32(out, static_cast<uint32_t>(value));
        FW::WriteBE32(out + 4, static_cast<uint32_t>(value >> 32));
    }
    [[nodiscard]] static uint64_t LoadDcpQuadlets(const uint8_t* in) noexcept {
        return static_cast<uint64_t>(FW::ReadBE32(in)) |
               (static_cast<uint64_t>(FW::ReadBE32(in + 4)) << 32);
    }

    Protocols::Ports::ProtocolRegisterIO& io_;
};

} // namespace ASFW::Audio::DICE::Focusrite::SPro40D3
