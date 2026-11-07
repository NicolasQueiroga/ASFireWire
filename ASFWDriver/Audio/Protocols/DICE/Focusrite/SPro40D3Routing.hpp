// SPDX-License-Identifier: Apache-2.0
// SPro40D3Routing.hpp - the pure D3 routing model: the frozen from-index
// encoding, the per-table source domains, the P00.4 safe initial profile,
// and the deterministic (slot, source) <-> RouteBundleId allocation.
//
// Evidence (P00 baseline §5/§6, all frozen):
//   - Wire encoding: from-index 0..54 (None, Analog-1..8, S/PDIF-1..2,
//     ADAT-1..8, Stream-1..20, Mixer-1..16; R's 55-label list).
//   - Per-table domains: router-out and meter tables accept 0..54; mixer
//     INPUTS accept 0..38 only - no Mixer-N sources, R-enforced, physically
//     required to prevent bus feedback.
//   - Both rate tables receive the SAME arrays (low then high, P:624-687);
//     Stream-15..20 references are inert at high rate (documented superset,
//     never silently clamped - P06/P11 qualify rate-dependent legality).
//   - The safe profile (P00.4): direct trusted playback, no mixer
//     dependence, no loopback feedback.
#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "SPro40D3MailboxTypes.hpp"

namespace ASFW::Audio::DICE::Focusrite::SPro40D3 {

// -- The from-index encoding (P00 baseline §5) -----------------------------

inline constexpr uint32_t kRouteNone     = 0;   ///< silence
inline constexpr uint32_t kRouteAnalogFirst = 1;    ///< Analog-1..8   -> 1..8
inline constexpr uint32_t kRouteSpdifFirst  = 9;    ///< S/PDIF-1/2    -> 9..10
inline constexpr uint32_t kRouteAdatFirst   = 11;   ///< ADAT-1..8      -> 11..18
inline constexpr uint32_t kRouteStreamFirst = 19;   ///< Stream-1..20   -> 19..38
inline constexpr uint32_t kRouteMixerFirst  = 39;   ///< Mixer-1..16    -> 39..54
// kMaxSymbolicRouteSource (54) already lives in SPro40D3MailboxTypes.hpp.

/// Router output slots (22): line 1..10, S/PDIF 1/2, ADAT 1..8, loopback 1/2.
inline constexpr uint32_t kRouterOutSlots = 22;
inline constexpr uint32_t kSlotLineFirst = 0;   ///< 10 line outputs
inline constexpr uint32_t kSlotSpdifFirst = 10; ///< 2 S/PDIF outputs
inline constexpr uint32_t kSlotAdatFirst  = 12; ///< 8 ADAT outputs
inline constexpr uint32_t kSlotLoopbackFirst = 20; ///< 2 loopback slots

// -- ParameterId allocation for routing surfaces (P00 baseline §5) ----------

inline constexpr uint32_t kRouterSourceParamBase   = 0x0200'0000; ///< + slot (0..21)
inline constexpr uint32_t kMixerSourceParamBase   = 0x0201'0000; ///< + input (0..17)
inline constexpr uint32_t kMeterSourceParamBase   = 0x0202'0000; ///< + meter (0..1)
inline constexpr uint32_t kApplyRoutingAction     = 0x0301'0001; ///< ActionTrigger

/// Deterministic RouteBundleId allocation: one singleton bundle per legal
/// (slot, from-index) pair. P06's SetRoute rides the shared bundle
/// machinery; the id decodes losslessly back into the pair.
inline constexpr uint32_t kRouteBundleIdBase = 0x0200'0000;

[[nodiscard]] constexpr uint32_t RouteBundleIdFor(uint32_t slot,
                                                  uint32_t from) noexcept {
    return kRouteBundleIdBase + slot * (kMaxSymbolicRouteSource + 1) + from;
}

[[nodiscard]] constexpr uint32_t RouteSlotFromBundleId(uint32_t id) noexcept {
    return (id - kRouteBundleIdBase) / (kMaxSymbolicRouteSource + 1);
}

[[nodiscard]] constexpr uint32_t RouteSourceFromBundleId(uint32_t id) noexcept {
    return (id - kRouteBundleIdBase) % (kMaxSymbolicRouteSource + 1);
}

// -- Per-table source domains (P00 baseline §5, frozen) ----------------------

/// Router OUTPUT slots: the full symbolic superset 0..54. Both rate tables
/// receive the same array; Stream-15..20 are inert at high rate.
[[nodiscard]] constexpr bool IsValidOutSlotSource(uint32_t from) noexcept {
    return from <= kMaxSymbolicRouteSource;
}

/// Mixer INPUTS: 0..38 only - never a Mixer-N source (bus feedback).
[[nodiscard]] constexpr bool IsValidMixerInputSource(uint32_t from) noexcept {
    return from <= 38;
}

/// Master meter sources: the full superset 0..54.
[[nodiscard]] constexpr bool IsValidMeterSource(uint32_t from) noexcept {
    return from <= kMaxSymbolicRouteSource;
}

// -- The P00.4 safe initial host-owned profile (P00 baseline §6) ------------
//
// Deliberate profile: direct trusted playback, no mixer dependence, no
// loopback feedback. Unlike ALSA's invented defaults (all 10 line outs fed
// from Mixer-1/2), nothing here depends on unknown mixer cells.

/// line 1..10 <- Stream-1..10, S/PDIF <- Stream-11/12,
/// ADAT 1..8 <- Stream-13..20, loopback 1/2 <- None.
inline constexpr RouterOutSrc kSafeRouterOutSrc{
    19, 20, 21, 22, 23, 24, 25, 26, 27, 28,
    29, 30,
    31, 32, 33, 34, 35, 36, 37, 38,
    0, 0};
/// All mixer inputs silent.
inline constexpr RouterMixerSrc kSafeRouterMixerSrc{};
/// Master meters <- Mon 1/2 (the monitor section).
inline constexpr RouterMeterSrc kSafeRouterMeterSrc{19, 20};

static_assert(kSafeRouterOutSrc.size() == kRouterOutSlots);
static_assert(kSafeRouterOutSrc[kSlotLoopbackFirst] == kRouteNone);
static_assert(kSafeRouterOutSrc[kSlotLoopbackFirst + 1] == kRouteNone);

/// The per-slot/per-input source-domain check for a (slot, from) pair,
/// routing-table aware: the out/meter tables share the superset; mixer
/// inputs exclude Mixer-N. Pure, reused by planner, protocol and tests.
[[nodiscard]] constexpr bool IsValidTableSource(bool mixerInputTable,
                                                uint32_t from) noexcept {
    return mixerInputTable ? IsValidMixerInputSource(from)
                           : IsValidOutSlotSource(from);
}

// -- Rate families and per-family knowledge (P00 baseline §6) ---------------
//
// Both tables receive the same arrays in one write, but their fates are
// tracked separately: the low table can confirm while the high table fails
// (and vice versa on re-application). The family IN FORCE decides which
// table's knowledge the published state claims - per-rate knowledge lives
// in this device-native state, exactly as the baseline froze it.

enum class D3RateFamily : uint8_t {
    Low,  ///< 44.1/48 kHz formation (stream 1..20, ADAT 1..8; §8 item 1 shape)
    High, ///< 88.2/96 kHz formation (R-claimed 14 streams, ADAT 1..4; §8)
};

struct D3RoutingState {
    bool lowApplied{false};  ///< the low table's contents are host-known
    bool highApplied{false}; ///< the high table's contents are host-known
    D3RateFamily current{D3RateFamily::Low}; ///< the family in force now
    RouterOutSrc outSrc{};
    RouterMixerSrc mixerSrc{};
    RouterMeterSrc meterSrc{};

    /// Whether the CURRENT family's table is host-known - the publication
    /// gate for every routing surface. The other family's arrays stay
    /// recorded for re-application but publish nothing.
    [[nodiscard]] bool FamilyApplied() const noexcept {
        return current == D3RateFamily::Low ? lowApplied : highApplied;
    }
};

/// The 55-label source name list (R's label table, P00 baseline §5).
[[nodiscard]] inline std::string RouteSourceLabel(uint32_t from) {
    if (from == kRouteNone) return "None";
    if (from < kRouteSpdifFirst)
        return "Analog " + std::to_string(from - kRouteAnalogFirst + 1);
    if (from < kRouteAdatFirst)
        return "S/PDIF " + std::to_string(from - kRouteSpdifFirst + 1);
    if (from < kRouteStreamFirst)
        return "ADAT " + std::to_string(from - kRouteAdatFirst + 1);
    if (from < kRouteMixerFirst)
        return "Stream " + std::to_string(from - kRouteStreamFirst + 1);
    return "Mixer " + std::to_string(from - kRouteMixerFirst + 1);
}

} // namespace ASFW::Audio::DICE::Focusrite::SPro40D3
