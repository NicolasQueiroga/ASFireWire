// SPDX-License-Identifier: Apache-2.0
// The P10a meter contract tests: the versioned, size-bounded envelope.
// Round-trip fidelity, the bounded-call rule (the count cap), and every
// rejection path (magic, version, header size, dirty padding, unknown
// flags, truncation, length mismatch). The golden-bytes test pins the
// layout for the Swift consumer (P10b).
#include "UserClient/WireFormats/MeterValuesWire.hpp"
#include <gtest/gtest.h>
#include <array>
#include <vector>

namespace W = ASFW::UserClient::Wire;

namespace {
std::vector<W::MeterSample> Three() {
    return std::vector<W::MeterSample>{
        W::MeterSample{-100, W::kMeterValueValid},
        W::MeterSample{0, 0},                       // not measured now
        W::MeterSample{0x1FFF'FFFF, W::kMeterValueValid},
    };
}
} // namespace

TEST(MeterValuesWire, RoundTripsMetadataAndSamples) {
    const auto bytes = W::SerializeMeterValues(7, 9, 1234567890ULL, Three());
    ASSERT_EQ(bytes.size(), W::MeterValuesWireSize(3));
    const auto parsed = W::ParseMeterValues(bytes);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->sequence, 7u);
    EXPECT_EQ(parsed->generation, 9u);
    EXPECT_EQ(parsed->timestampNs, 1234567890ULL);
    ASSERT_EQ(parsed->samples.size(), 3u);
    EXPECT_EQ(parsed->samples[0].raw, -100);
    EXPECT_EQ(parsed->samples[0].flags, W::kMeterValueValid);
    EXPECT_EQ(parsed->samples[1].raw, 0);
    EXPECT_EQ(parsed->samples[1].flags, 0); // invalid is data, not a zero reading
    EXPECT_EQ(parsed->samples[2].raw, 0x1FFF'FFFF);
}

TEST(MeterValuesWire, TheBoundedCallRuleCapsTheCount) {
    std::vector<W::MeterSample> tooMany(W::kMaxMeterValues + 1);
    EXPECT_TRUE(W::SerializeMeterValues(1, 1, 0, tooMany).empty());
    // At the cap exactly: still one bounded call.
    std::vector<W::MeterSample> atCap(W::kMaxMeterValues);
    const auto bytes = W::SerializeMeterValues(1, 1, 0, atCap);
    ASSERT_FALSE(bytes.empty());
    EXPECT_EQ(bytes.size(), W::MeterValuesWireSize(W::kMaxMeterValues));
    EXPECT_LE(bytes.size(), 512u); // one external-method call, no paging
}

TEST(MeterValuesWire, RejectsEveryCorruption) {
    const auto good = W::SerializeMeterValues(1, 1, 1, Three());
    ASSERT_FALSE(good.empty());

    // Truncated below the header.
    EXPECT_FALSE(W::ParseMeterValues(std::span<const uint8_t>(good).first(16)).has_value());
    // Truncated inside the payload.
    EXPECT_FALSE(W::ParseMeterValues(std::span<const uint8_t>(good).first(good.size() - 1)).has_value());
    // Trailing junk (length must match exactly).
    auto padded = good;
    padded.push_back(0);
    EXPECT_FALSE(W::ParseMeterValues(padded).has_value());

    // Bad magic.
    auto badMagic = good;
    badMagic[0] = ~badMagic[0];
    EXPECT_FALSE(W::ParseMeterValues(badMagic).has_value());

    // Unknown version.
    auto badVersion = good;
    badVersion[4] = 9;
    EXPECT_FALSE(W::ParseMeterValues(badVersion).has_value());

    // Header-size lie.
    auto badHeader = good;
    badHeader[6] = 99;
    EXPECT_FALSE(W::ParseMeterValues(badHeader).has_value());

    // Count lie (above the cap): the count field sits at header offset 24.
    auto badCount = good;
    badCount[24] = 0xFF;
    badCount[25] = 0xFF;
    EXPECT_FALSE(W::ParseMeterValues(badCount).has_value());

    // Dirty reserved bits in the header (offset 26).
    auto dirtyReserved = good;
    dirtyReserved[26] = 1;
    EXPECT_FALSE(W::ParseMeterValues(dirtyReserved).has_value());

    // Unknown flag bits on a sample.
    auto dirtyFlags = good;
    dirtyFlags[sizeof(W::MeterValuesWire) + 4] = 0x02;
    EXPECT_FALSE(W::ParseMeterValues(dirtyFlags).has_value());

    // Dirty per-sample padding.
    auto dirtyPad = good;
    dirtyPad[sizeof(W::MeterValuesWire) + 5] = 1;
    EXPECT_FALSE(W::ParseMeterValues(dirtyPad).has_value());
}

TEST(MeterValuesWire, TheLayoutIsPinnedForTheSwiftConsumer) {
    // The golden shape (little-endian): the first batch of the contract's
    // documentation - sequence 1, generation 7, timestamp 1000, three
    // samples [-1 valid, 0 invalid, 42 valid]. Any byte here changing is
    // a wire-format break the Swift side must know about.
    const auto bytes = W::SerializeMeterValues(1, 7, 1000, std::vector<W::MeterSample>{
        W::MeterSample{-1, W::kMeterValueValid},
        W::MeterSample{0, 0},
        W::MeterSample{42, W::kMeterValueValid},
    });
    ASSERT_EQ(bytes.size(), 28 + 3 * 8);
    const unsigned char expected[] = {
        0x41, 0x53, 0x4D, 0x56,             // "ASMV" little-endian
        0x01, 0x00,                         // version 1
        0x1C, 0x00,                         // header 28 bytes
        0x01, 0x00, 0x00, 0x00,             // sequence 1
        0x07, 0x00, 0x00, 0x00,             // generation 7
        0xE8, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // timestamp 1000
        0x03, 0x00,                         // count 3
        0x00, 0x00,                         // reserved 0
        0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x00, 0x00, 0x00, // -1, valid
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 0, not measured
        0x2A, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, // 42, valid
    };
    EXPECT_EQ(0, std::memcmp(bytes.data(), expected, sizeof(expected)));
}
