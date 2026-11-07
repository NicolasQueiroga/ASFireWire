// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
// The P10a meter delivery contract: a versioned, size-bounded, neutral
// envelope for one batch of meter samples. The D3 carries 20 values per
// acquisition (18 mixer sources + 2 master); the envelope caps at 32
// samples so the whole batch always fits ONE bounded external-method
// call (28-byte header + 8 bytes per sample = 284 bytes worst case -
// no paging, no unbounded buffers).
//
// Honesty rules carried by the format itself (P10b implements them;
// the contract fixes the vocabulary):
//   - Values are RAW i32 (the device's own scale). No dBFS, no peak, no
//     clip labels - those require validated calibration that no code may
//     fabricate from raw numbers.
//   - Each sample carries a validity flag. An invalid/stale sample is
//     DATA ("not measured now"), never a silent zero.
//   - The batch carries the bus generation and a host timestamp: a
//     consumer crossing a generation boundary drops old batches rather
//     than mixing epochs.
//   - The sequence is monotonic per producer; consumers may coalesce.
#pragma once

#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

namespace ASFW::UserClient::Wire {

inline constexpr uint32_t kMeterValuesMagic = 0x564D5341; // "ASMV", little-endian
inline constexpr uint16_t kMeterValuesVersion = 1;

/// The hard sample cap: one batch, one bounded call, forever.
inline constexpr size_t kMaxMeterValues = 32;

/// Per-sample flags (the rest of the byte is reserved, must be 0).
inline constexpr uint8_t kMeterValueValid = 0x01; ///< The value is a real, current measurement.

struct __attribute__((packed)) MeterValueWire {
    int32_t raw{0};        ///< The device's own scale; never a derived unit.
    uint8_t flags{0};
    uint8_t reserved[3]{0};
};
static_assert(sizeof(MeterValueWire) == 8, "MeterValueWire must be 8 bytes");

struct __attribute__((packed)) MeterValuesWire {
    uint32_t magic{kMeterValuesMagic};
    uint16_t version{kMeterValuesVersion};
    uint16_t headerBytes{sizeof(MeterValuesWire)};
    uint32_t sequence{0};      ///< Monotonic per producer; consumers may coalesce.
    uint32_t generation{0};   ///< Bus generation at acquisition (epoch staleness).
    uint64_t timestampNs{0};   ///< Host boot-relative acquisition time.
    uint16_t count{0};         ///< Samples that follow; <= kMaxMeterValues.
    uint16_t reserved{0};
};
static_assert(sizeof(MeterValuesWire) == 28, "MeterValuesWire must be 28 bytes");

/// One batch as the producer measured it.
struct MeterSample {
    int32_t raw{0};
    uint8_t flags{0};
};

[[nodiscard]] inline size_t MeterValuesWireSize(size_t count) noexcept {
    return sizeof(MeterValuesWire) + count * sizeof(MeterValueWire);
}

/// Serialize one batch. Returns empty on count > kMaxMeterValues (the
/// bounded-call rule is the contract; there is no partial encoding).
[[nodiscard]] inline std::vector<uint8_t> SerializeMeterValues(
    uint32_t sequence, uint32_t generation, uint64_t timestampNs,
    std::span<const MeterSample> samples) {
    if (samples.size() > kMaxMeterValues) {
        return {};
    }
    std::vector<uint8_t> out(MeterValuesWireSize(samples.size()));
    MeterValuesWire header{};
    header.sequence = sequence;
    header.generation = generation;
    header.timestampNs = timestampNs;
    header.count = static_cast<uint16_t>(samples.size());
    std::memcpy(out.data(), &header, sizeof(header));
    auto* dst = reinterpret_cast<MeterValueWire*>(out.data() + sizeof(header));
    for (const auto& s : samples) {
        dst->raw = s.raw;
        dst->flags = s.flags & kMeterValueValid;
        ++dst;
    }
    return out;
}

/// The parsed batch (metadata + samples).
struct ParsedMeterValues {
    uint32_t sequence{0};
    uint32_t generation{0};
    uint64_t timestampNs{0};
    std::vector<MeterSample> samples;
};

/// Parse and validate one envelope. Rejects: short buffer, bad magic,
/// unknown version, header-size mismatch, count above the cap, or the
/// buffer not exactly matching header + count * 8. Never trusts a
/// length it did not check against the actual buffer size.
[[nodiscard]] inline std::optional<ParsedMeterValues> ParseMeterValues(
    std::span<const uint8_t> bytes) {
    if (bytes.size() < sizeof(MeterValuesWire)) {
        return std::nullopt;
    }
    MeterValuesWire header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (header.magic != kMeterValuesMagic ||
        header.version != kMeterValuesVersion ||
        header.headerBytes != sizeof(MeterValuesWire) ||
        header.reserved != 0) {
        return std::nullopt;
    }
    if (header.count > kMaxMeterValues) {
        return std::nullopt;
    }
    if (bytes.size() != MeterValuesWireSize(header.count)) {
        return std::nullopt;
    }
    ParsedMeterValues out;
    out.sequence = header.sequence;
    out.generation = header.generation;
    out.timestampNs = header.timestampNs;
    out.samples.reserve(header.count);
    const auto* src = reinterpret_cast<const MeterValueWire*>(
        bytes.data() + sizeof(header));
    for (uint16_t i = 0; i < header.count; ++i, ++src) {
        if ((src->flags & ~kMeterValueValid) != 0 ||
            src->reserved[0] || src->reserved[1] || src->reserved[2]) {
            return std::nullopt; // unknown flag bits or dirty padding: reject
        }
        out.samples.push_back(MeterSample{src->raw, src->flags});
    }
    return out;
}

} // namespace ASFW::UserClient::Wire
