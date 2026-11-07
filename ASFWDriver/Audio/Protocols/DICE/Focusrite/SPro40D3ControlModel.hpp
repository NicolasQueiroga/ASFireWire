// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// SPro40D3ControlModel.hpp - The pure Saffire Pro 40 D3 (TCD3070) control
// model (P04). One function turns the raw readable control-store words into
// the generic ControlSnapshot; no bus, no protocol, no state of its own.
// Pattern: DuetControlModel.hpp (FW-124 decomposition).
//
// TRUTHFULNESS CONTRACT (P00 baseline §2/§3/§8, plan §4):
// - State entries exist ONLY for words actually read back (Observed). The
//   monitor mixer coefficients are defined in the TOPOLOGY - the write path
//   is P08 - but carry NO state entries: the D3 protocol has no mixer
//   readback, so they start Unknown and absent - existing JSON then emits
//   null, never a fabricated 0/false.
// - Output levels publish the DE-INVERTED raw 0..127 scale with no dB
//   label: no calibration is established (§8 item 3). Unity for mixer
//   cells is ~0x1fff per the reference (R:11-21); the domain stays raw
//   0..0x7fff.
// - The active-monitor-pad word publishes as a RAW scalar: the reference
//   carries one set_active_monitor_pad value and the wire encoding of the
//   pad selection is not bit-proven from the captures (documented §8).
// - The monitor knob (pos 0x54) is get-only (§8): its parameter is
//   ParameterAccess::ReadOnly. Unsupported physical controls (input gain,
//   phantom, instrument, input pad, headphone pots) are NOT parameters.
// - This topology models the 48 kHz, capture-proven shape (20 stream
//   channels, 8 ADAT channels). High-rate formations stay TCAT's business
//   until the P06 bring-up capture closes §8 item 1.
// - Router selector nodes are NOT part of the read-only P04 snapshot:
//   the router is write-only with no readback (§8), there is nothing to
//   observe, and each destination's exact legal-source set is transcribed
//   from the reference tables (R:92-225) when the write path lands (P08) -
//   where a wrong legal-bundle set would be a real device hazard. The
//   topology grows then, as a normal revision rebind.
// - On the wire, analogue router destinations reverse by pairs (§2); that
//   is a write-path encoding concern (P08), not a topology concern - the
//   topology stays in logical channel order.

#pragma once

#include "../../../Controls/IControlProvider.hpp"
#include "SPro40D3MailboxTypes.hpp"
#include "SPro40D3Routing.hpp"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ASFW::Audio::DICE::Focusrite::SPro40D3 {

/// Native parameter IDs (below the 0x80000000 input-type projection
/// namespace, plan §4). 0x5A3D = this project's D3 control family.
namespace D3Control {
/// The single supported action at P04: re-read the whole readable group
/// (app-only recovery for a failed or stale attempt). ActionDomain
/// parameters never carry state (the canonical validator skips them).
inline constexpr uint32_t RefreshReadableControls = 0x5A3D0001;
inline constexpr uint32_t OutputVolumeBase = 0x5A3D0100; ///< + channel 0..9
inline constexpr uint32_t OutputMuteBase = 0x5A3D0200; ///< + channel 0..9
inline constexpr uint32_t GlobalMute = 0x5A3D0300;
inline constexpr uint32_t GlobalDim = 0x5A3D0301;
inline constexpr uint32_t MuteTargetBase = 0x5A3D0310; ///< + channel 0..9
inline constexpr uint32_t DimTargetBase = 0x5A3D0320; ///< + channel 0..9
inline constexpr uint32_t HwVolumeBase = 0x5A3D0330; ///< + channel 0..9
inline constexpr uint32_t MonitorPad = 0x5A3D0400; ///< raw word, encoding §8
inline constexpr uint32_t MonitorVolume = 0x5A3D0500; ///< get-only knob
inline constexpr uint32_t AdatAsSpdif = 0x5A3D0600;
inline constexpr uint32_t MixerCellBase = 0x5A3D1000; ///< + (bus*18 + slot)
/// The deliberate whole-mixer initialization (P08): the one action that
/// establishes a complete reviewed host recipe where no readback exists.
/// Ordinary cell writes refuse while the cells are Unknown; this action
/// is the only way they become host-known.
inline constexpr uint32_t InitializeMixer = 0x5A3D1700;
inline constexpr uint32_t kMixerCellCount = 16 * 18;
} // namespace D3Control

/// The raw readable control store, exactly as get_control reads return it
/// (positions from spro40d3.rs; word encodings P00 §2 row 43). All 4-byte
/// big-endian u32s on the wire.
struct D3ControlStore {
    uint32_t mute{0}; ///< pos 0x0c: global mute, nonzero = engaged
    uint32_t dim{0}; ///< pos 0x10: global dim, nonzero = engaged
    std::array<uint32_t, 5> volumes{}; ///< pos 0x14+i*4: byte0 = ch 2i,
    ///< byte1 = ch 2i+1, each WIRE-INVERTED vs 127 (level = 127 - byte)
    std::array<uint32_t, 5> hwctl{}; ///< pos 0x28+i*4: bit0 = hw-knob
    ///< assignment ch 2i, bit1 = ch 2i+1, bit2 = volume mute ch 2i,
    ///< bit3 = volume mute ch 2i+1
    uint32_t assigns{0}; ///< pos 0x3c: bits 0-9 = output follows global
    ///< mute, bits 10-19 = output follows global dim
    uint32_t monitorPad{0}; ///< pos 0x40: active monitor pad (raw)
    uint32_t monitorVolume{0}; ///< pos 0x54: hardware monitor knob (raw)
    uint32_t adatAsSpdif{0}; ///< pos 0x5c: optical output carries S/PDIF

    /// Confidence per readable word, indexed like kD3ReadableOffsets:
    /// true = observed (or test-seeded); false = unknown - an ambiguous
    /// write left the word in doubt. Unknown words publish their
    /// parameters as topology-only (absent state = Unknown, the same
    /// convention as the readback-absent mixer cells; the D3 completeness
    /// opt-in already permits absence). Defaults to all-observed: a
    /// hand-built store in tests is trusted; only the protocol write
    /// path clears bits it cannot confirm.
    std::array<bool, 16> observed{true, true, true, true, true, true, true, true,
                                  true, true, true, true, true, true, true, true};
};

/// The readable group: every control_offset position, ascending. The
/// Prepare path reads exactly these, in this order (16 words).
inline constexpr size_t kD3ReadableWordCount = 16;
inline constexpr std::array<uint32_t, kD3ReadableWordCount> kD3ReadableOffsets{
    0x0c, 0x10, 0x14, 0x18, 0x1c, 0x20, 0x24, 0x28,
    0x2c, 0x30, 0x34, 0x38, 0x3c, 0x40, 0x54, 0x5c,
};

inline constexpr size_t kD3AnalogOutputs = 10;
inline constexpr size_t kD3AnalogInputs = 8;
inline constexpr size_t kD3MixerSlots = 18; ///< 8 analog + 2 S/PDIF + 8 ADAT
inline constexpr size_t kD3MixerBuses = 16; ///< mono buses (8 stereo pairs)

/// Host-known mixer cell view for BuildSPro40D3Controls (P08). A cell is
/// known only when the host wrote it (no readback exists, so Observed is
/// impossible): known cells publish Cached state; unknown cells publish
/// topology only - absent state = Unknown, the honest representation.
struct D3MixerCells {
    std::array<std::array<bool, kD3MixerBuses>, kD3MixerSlots> known{};
    std::array<std::array<int32_t, kD3MixerBuses>, kD3MixerSlots> value{};
};

inline constexpr size_t kD3StreamChannels = 20; ///< 48 kHz shape (Â§8 item 1)
inline constexpr size_t kD3AdatChannels = 8; ///< 48 kHz shape (Â§8 item 1)

/// Build the full D3 control snapshot from the observed words. `revision`
/// is the route epoch (route identity of the observation), `sequence` the
/// caller's publication sequence. Pure: no I/O, no globals, no time.
[[nodiscard]] inline Controls::ControlSnapshot BuildSPro40D3Controls(
    const D3ControlStore& store, const D3RoutingState& routing,
    uint64_t revision, uint64_t sequence,
    const D3MixerCells* mixerCells = nullptr) {
    namespace AM = ASFW::AudioModel;
    namespace C = ASFW::Audio::Controls;
    C::ControlSnapshot result;
    result.family = "spro40d3";
    result.topology.revision = result.state.topologyRevision = revision;
    result.sequence = sequence;
    auto& t = result.topology;
    auto& s = result.state;

    uint32_t nextPort = 1;
    auto node = [&t](uint32_t id, std::string name, AM::NodeBody body) {
        t.nodes.push_back({AM::NodeId{id}, std::move(name), std::move(body)});
    };
    auto port = [&t, &nextPort](uint32_t owner, AM::PortDirection direction,
                                 uint32_t channels, AM::SignalIdentity signal = {},
                                 std::string name = "") {
        const AM::PortId id{nextPort++};
        t.ports.push_back({.id = id,
                           .owner = AM::NodeId{owner},
                           .direction = direction,
                           .channels = channels,
                           .name = std::move(name),
                           .signal = signal});
        return id;
    };
    auto param = [&t, &s](uint32_t id, AM::ParameterTarget target,
                           AM::ParameterSemantic semantic, AM::ParameterDomain domain,
                           std::string name, AM::ParameterValue value,
                           AM::ParameterAccess access = AM::ParameterAccess::ReadWrite) {
        t.parameters.push_back({.id = AM::ParameterId{id},
                                 .target = target,
                                 .semantic = semantic,
                                 .domain = std::move(domain),
                                 .name = std::move(name),
                                 .access = access});
        s.parameters.emplace(AM::ParameterId{id},
                             AM::ParameterState{std::move(value), AM::StateOrigin::Observed});
    };
    /// A crosspoint parameter with optional host-known state: written
    /// cells publish Cached state; never-written cells stay stateless
    /// (no readback exists - absent state = Unknown).
    auto cellParam = [&t, &s](uint32_t id, AM::CrosspointId target,
                               AM::ParameterSemantic semantic,
                               AM::ParameterDomain domain, std::string name,
                               const int32_t* knownValue) {
        t.parameters.push_back({.id = AM::ParameterId{id},
                                 .target = AM::ParameterTarget{target},
                                 .semantic = semantic,
                                 .domain = std::move(domain),
                                 .name = std::move(name)});
        if (knownValue != nullptr) {
            s.parameters.emplace(AM::ParameterId{id},
                AM::ParameterState{AM::ParameterValue{double(*knownValue)},
                                   AM::StateOrigin::Cached});
        }
    };
    /// Topology-only parameter: defined for the write path (P08), published
    /// with NO state entry - mixer readback does not exist (§8).
    auto latentParam = [&t](uint32_t id, AM::ParameterTarget target,
                             AM::ParameterSemantic semantic, AM::ParameterDomain domain,
                             std::string name) {
        t.parameters.push_back({.id = AM::ParameterId{id},
                                 .target = target,
                                 .semantic = semantic,
                                 .domain = std::move(domain),
                                 .name = std::move(name)});
    };
    /// A readable-surface parameter backed by one control word: emitted
    /// with observed state when that word is trusted, topology-only when
    /// it is not (absent state = Unknown; the completeness opt-in permits
    /// exactly this absence).
    auto wordParam = [&t, &store, &param, &latentParam](
                         size_t word, uint32_t id, AM::ParameterTarget target,
                         AM::ParameterSemantic semantic, AM::ParameterDomain domain,
                         std::string name, AM::ParameterValue value,
                         AM::ParameterAccess access = AM::ParameterAccess::ReadWrite) {
        if (store.observed[word]) {
            param(id, target, semantic, std::move(domain), std::move(name),
                  std::move(value), access);
        } else {
            latentParam(id, target, semantic, std::move(domain), std::move(name));
        }
    };

    auto link = [&t](AM::PortId a, AM::PortId b) { t.fixedLinks.push_back({a, b}); };

    // ------------------------------------------------------------------
    // Endpoints
    // ------------------------------------------------------------------
    constexpr uint32_t kHostCapture = 1, kHostPlayback = 2;
    node(kHostCapture, "Host Capture", AM::EndpointNode{AM::EndpointKind::Host});
    node(kHostPlayback, "Host Playback", AM::EndpointNode{AM::EndpointKind::Host});

    // Physical sources (menu roots). Analog inputs 1-8 are combo jacks
    // (Mic/Instrument/Line); gain and phantom are physical-only (§8), so
    // the ports exist as mixer sources and nothing more.
    constexpr uint32_t kAnalogInBase = 10; // 10..17
    std::array<AM::PortId, kD3AnalogInputs> analogIn{};
    for (uint32_t ch = 0; ch < kD3AnalogInputs; ++ch) {
        const uint32_t id = kAnalogInBase + ch;
        node(id, "Analog Input " + std::to_string(ch + 1),
             AM::EndpointNode{AM::EndpointKind::Physical});
        analogIn[ch] = port(id, AM::PortDirection::Output, 1,
                            {AM::SignalKind::AnalogMicXlr, ch + 1});
    }
    constexpr uint32_t kSpdifIn = 20, kAdatIn = 21;
    node(kSpdifIn, "S/PDIF Input", AM::EndpointNode{AM::EndpointKind::Physical});
    std::array<AM::PortId, 2> spdifIn{};
    for (uint32_t ch = 0; ch < 2; ++ch) {
        spdifIn[ch] = port(kSpdifIn, AM::PortDirection::Output, 1,
                           {AM::SignalKind::SpdifCoaxial, ch + 1});
    }
    node(kAdatIn, "ADAT Input", AM::EndpointNode{AM::EndpointKind::Physical});
    std::array<AM::PortId, kD3AdatChannels> adatIn{};
    for (uint32_t ch = 0; ch < kD3AdatChannels; ++ch) {
        adatIn[ch] = port(kAdatIn, AM::PortDirection::Output, 1,
                         {AM::SignalKind::Adat, ch + 1});
    }

    // Physical output endpoints. Ten mono line outputs; HP1 shares lines
    // 7/8 and HP2 shares 9/10 (headphone pots are physical, §8).
    constexpr uint32_t kAnalogOutBase = 30; // 30..39
    std::array<AM::PortId, kD3AnalogOutputs> analogOut{};
    for (uint32_t ch = 0; ch < kD3AnalogOutputs; ++ch) {
        const uint32_t id = kAnalogOutBase + ch;
        node(id, "Analog Output " + std::to_string(ch + 1),
             AM::EndpointNode{AM::EndpointKind::Physical});
        analogOut[ch] = port(id, AM::PortDirection::Input, 1,
                             {AM::SignalKind::AnalogLine, ch + 1});
    }
    constexpr uint32_t kSpdifOut = 40, kAdatOut = 41;
    node(kSpdifOut, "S/PDIF Output", AM::EndpointNode{AM::EndpointKind::Physical});
    const auto spdifOut = port(kSpdifOut, AM::PortDirection::Input, 2,
                               {AM::SignalKind::SpdifCoaxial, 1});
    node(kAdatOut, "ADAT Output", AM::EndpointNode{AM::EndpointKind::Physical});
    const auto adatOut = port(kAdatOut, AM::PortDirection::Input, kD3AdatChannels,
                              {AM::SignalKind::Adat, 1});

    // Host stream endpoint ports: 20 capture, 20 playback (48 kHz shape).
    // The playback ports are the router's Stream-1..20 sources; the capture
    // ports are its fixed hardware-capture destinations (serializer-owned).
    std::array<AM::PortId, kD3StreamChannels> playbackPort{};
    for (uint32_t ch = 0; ch < kD3StreamChannels; ++ch) {
        (void)port(kHostCapture, AM::PortDirection::Input, 1,
                   {AM::SignalKind::HostStream, ch + 1});
        playbackPort[ch] = port(kHostPlayback, AM::PortDirection::Output, 1,
                                {AM::SignalKind::HostStream, ch + 1});
    }

    // ------------------------------------------------------------------
    // Monitor mixer: 18 selectable input slots x 16 mono buses (288 cells).
    // Slot sources are the physical/digital inputs; mix buses feed the
    // (write-only, P08) router. Fixed links wire each source once to its
    // slot - the per-slot SELECTOR is router work, not mixer work.
    // ------------------------------------------------------------------
    constexpr uint32_t kMixerNode = 70;
    std::array<AM::PortId, kD3MixerSlots> mixerIn{};
    AM::MixerNode mixer;
    for (uint32_t slot = 0; slot < kD3MixerSlots; ++slot) {
        mixerIn[slot] = port(kMixerNode, AM::PortDirection::Input, 1, {},
                             "Slot " + std::to_string(slot + 1));
        mixer.inputs.push_back(mixerIn[slot]);
    }
    // The 18 slots: 8 analog, 2 S/PDIF, 8 ADAT - in reference order, one
    // mono source channel per slot.
    for (uint32_t ch = 0; ch < kD3AnalogInputs; ++ch) link(analogIn[ch], mixerIn[ch]);
    for (uint32_t ch = 0; ch < 2; ++ch) {
        link(spdifIn[ch], mixerIn[kD3AnalogInputs + ch]);
    }
    for (uint32_t ch = 0; ch < kD3AdatChannels; ++ch) {
        link(adatIn[ch], mixerIn[kD3AnalogInputs + 2 + ch]);
    }
    std::array<AM::PortId, kD3MixerBuses> mixerBus{};
    for (uint32_t bus = 0; bus < kD3MixerBuses; ++bus) {
        mixerBus[bus] = port(kMixerNode, AM::PortDirection::Output, 1, {},
                             "Mix Bus " + std::to_string(bus + 1));
        mixer.outputs.push_back(mixerBus[bus]);
        for (uint32_t slot = 0; slot < kD3MixerSlots; ++slot) {
            const AM::CrosspointId id{
                static_cast<uint32_t>(bus * kD3MixerSlots + slot + 1)};
            mixer.crosspoints.push_back({id, mixerIn[slot], mixerBus[bus]});
            // Raw coefficient, no invented dB law; reference unity ~0x1fff
            // (R:11-21) stays in the name, not the domain. A cell the host
            // has written publishes Cached state; a never-written cell stays
            // stateless (no readback exists) and ordinary writes refuse on
            // it until the Initialize Monitor Mixer action runs.
            const int32_t* known =
                mixerCells != nullptr && mixerCells->known[slot][bus]
                    ? &mixerCells->value[slot][bus]
                    : nullptr;
            cellParam(D3Control::MixerCellBase + bus * kD3MixerSlots + slot, id,
                      AM::ParameterSemantic::Level,
                      AM::ScalarDomain{0, 32767, 1, AM::ScalarUnit::Generic},
                      "Mixer " + std::to_string(slot + 1) + " \xE2\x86\x92 Bus " +
                          std::to_string(bus + 1) + " (raw, unity ~0x1fff)",
                      known);
        }
    }
    node(kMixerNode, "Monitor Mixer", std::move(mixer));
    // ------------------------------------------------------------------
    // The D3 router (P00 baseline §5/§6; P06). One wire table per rate
    // family, written as a unit (low then high, the same arrays), with NO
    // readback anywhere: state is host-known (Cached) only after a
    // deliberate full application, and the family in force decides which
    // table's knowledge the snapshot claims. Unknown = absent, always.
    //
    // Surfaces: 22 output slots (line 1..10, S/PDIF 1/2, ADAT 1..8,
    // loopback 1/2), 18 mixer input selectors, 2 master meter taps.
    // ------------------------------------------------------------------
    constexpr uint32_t kRouterNode = 0x100;
    // The loopback endpoints: router slots whose routed signal lands on
    // host playback channels 19/20 (serializer-fixed to STREAM+18/19).
    constexpr uint32_t kLoopbackNode = 42;
    node(kLoopbackNode, "Loopback", AM::EndpointNode{AM::EndpointKind::Host});
    for (uint32_t ch = 0; ch < 2; ++ch) {
        const auto loop = port(kLoopbackNode, AM::PortDirection::Input, 1,
                               {AM::SignalKind::HostStream, ch + 1},
                               "Loopback " + std::to_string(ch + 1));
        link(playbackPort[kD3StreamChannels - 2 + ch], loop);
    }
    // Router ports are OWNED by the router node (validator contract, the
    // same discipline as the mixer's slot ports): 54 source-selection
    // inputs in from-index order (None is not a port - detaching a slot
    // is validateRouteEdit's nullopt), and 40 destination slots in
    // wire-slot order (0..21 outputs, 22..39 mixer inputs). The names
    // carry the physical identity the selection refers to.
    std::array<AM::PortId, kMaxSymbolicRouteSource> routeInput{};
    for (uint32_t from = 1; from <= kMaxSymbolicRouteSource; ++from) {
        routeInput[from - 1] = port(kRouterNode, AM::PortDirection::Input, 1, {},
                                    RouteSourceLabel(from));
    }
    std::array<AM::PortId, kRouterOutSlots + kD3MixerSlots> routeOutput{};
    const auto slotPortName = [](uint32_t slot) {
        return slot < kSlotSpdifFirst
                   ? "Line " + std::to_string(slot + 1)
                   : slot < kSlotAdatFirst
                         ? "S/PDIF " + std::to_string(slot - kSlotSpdifFirst + 1)
                         : slot < kSlotLoopbackFirst
                               ? "ADAT " + std::to_string(slot - kSlotAdatFirst + 1)
                               : "Loopback " +
                                     std::to_string(slot - kSlotLoopbackFirst + 1);
    };
    for (uint32_t slot = 0; slot < kRouterOutSlots; ++slot) {
        routeOutput[slot] = port(kRouterNode, AM::PortDirection::Output, 1, {},
                                 slotPortName(slot));
    }
    for (uint32_t in = 0; in < kD3MixerSlots; ++in) {
        routeOutput[kRouterOutSlots + in] =
            port(kRouterNode, AM::PortDirection::Output, 1, {},
                 "Mixer Input " + std::to_string(in + 1));
    }

    AM::RouterNode router;
    router.inputs.assign(routeInput.begin(), routeInput.end());
    router.outputs.assign(routeOutput.begin(), routeOutput.end());
    router.writable = true;
    router.constraints.maxSourcesPerOutput = 1;
    // One singleton legal bundle per (destination slot, legal source).
    // Out slots take the 0..54 superset; mixer inputs take 0..38 only -
    // no Mixer-N sources, physically required to prevent bus feedback.
    // from == 0 (None) has no port and no bundle: detaching a slot is
    // validateRouteEdit's nullopt input, and a None-valued slot publishes
    // no active bundle.
    for (uint32_t slot = 0; slot < routeOutput.size(); ++slot) {
        const bool mixerInput = slot >= kRouterOutSlots;
        const uint32_t legalMax = mixerInput ? 38u : kMaxSymbolicRouteSource;
        for (uint32_t from = 1; from <= legalMax; ++from) {
            router.legalBundles.push_back(
                {AM::RouteBundleId{RouteBundleIdFor(slot, from)},
                 {{routeInput[from - 1], routeOutput[slot]}}});
        }
    }
    node(kRouterNode, "D3 Router", std::move(router));

    // Source-select parameters: enum of legal from-indexes per table,
    // published with Cached origin ONLY when the family in force is
    // host-known (absent state otherwise - the write path refuses edits
    // against an unknown table; the snapshot never claims one).
    auto routeSourceParam = [&t, &s, &routing](uint32_t id, AM::PortId slotPort,
                                              AM::ParameterDomain domain,
                                              std::string name, uint32_t from) {
        t.parameters.push_back({.id = AM::ParameterId{id},
                                .target = slotPort,
                                .semantic = AM::ParameterSemantic::Unknown,
                                .domain = std::move(domain),
                                .name = std::move(name),
                                .access = AM::ParameterAccess::ReadWrite});
        if (routing.FamilyApplied()) {
            s.parameters.emplace(
                AM::ParameterId{id},
                AM::ParameterState{int64_t(from), AM::StateOrigin::Cached});
        }
    };
    const auto outSourceDomain = [] {
        AM::EnumDomain d;
        for (uint32_t from = 0; from <= kMaxSymbolicRouteSource; ++from)
            d.values.push_back({int64_t(from), RouteSourceLabel(from)});
        return d;
    };
    const auto mixerSourceDomain = [] {
        AM::EnumDomain d;
        for (uint32_t from = 0; from <= 38; ++from)
            d.values.push_back({int64_t(from), RouteSourceLabel(from)});
        return d;
    };
    for (uint32_t slot = 0; slot < kRouterOutSlots; ++slot) {
        routeSourceParam(kRouterSourceParamBase + slot, routeOutput[slot],
                         outSourceDomain(), slotPortName(slot) + " Source",
                         routing.outSrc[slot]);
    }
    for (uint32_t in = 0; in < kD3MixerSlots; ++in) {
        routeSourceParam(kMixerSourceParamBase + in,
                         routeOutput[kRouterOutSlots + in], mixerSourceDomain(),
                         "Mixer Input " + std::to_string(in + 1) + " Source",
                         routing.mixerSrc[in]);
    }
    for (uint32_t m = 0; m < 2; ++m) {
        routeSourceParam(kMeterSourceParamBase + m, routeOutput[0],
                         outSourceDomain(),
                         "Master Meter " + std::to_string(m + 1) + " Source",
                         routing.meterSrc[m]);
    }
    // The deliberate full-application action (P00.4 safe profile): the ONLY
    // way an unknown table becomes known. Topology-only, like the P04
    // refresh action.
    latentParam(kApplyRoutingAction, AM::NodeId{kRouterNode},
                AM::ParameterSemantic::Unknown, AM::ActionDomain{},
                "Apply Host Routing Program (full low+high tables)");
    // The router state publishes the in-force program as active bundles.
    if (routing.FamilyApplied()) {
        AM::RouterState published;
        for (uint32_t slot = 0; slot < kRouterOutSlots; ++slot)
            if (routing.outSrc[slot] != kRouteNone)
                published.activeBundles.push_back(
                    AM::RouteBundleId{RouteBundleIdFor(slot, routing.outSrc[slot])});
        for (uint32_t in = 0; in < kD3MixerSlots; ++in)
            if (routing.mixerSrc[in] != kRouteNone)
                published.activeBundles.push_back(AM::RouteBundleId{
                    RouteBundleIdFor(kRouterOutSlots + in, routing.mixerSrc[in])});
        s.routers.emplace(AM::NodeId{kRouterNode}, std::move(published));
    }

    // ------------------------------------------------------------------
    // The readable group (16 words, positions 0x0c..0x5c) -> Observed
    // state. Globals and the monitor pad/knob act on the monitor pair
    // (outputs 1/2, manual pp.21-23); each parameter's name carries its
    // own meaning. The optical mode targets the ADAT output.
    // ------------------------------------------------------------------
    const auto outputNode = [&](uint32_t ch) {
        return static_cast<AM::NodeId>(kAnalogOutBase + ch);
    };
    const auto rawLevel = [&](uint32_t ch) {
        const uint32_t word = store.volumes[ch / 2];
        const uint32_t wireByte = (ch % 2 == 0) ? (word & 0xFFu) : ((word >> 8) & 0xFFu);
        return static_cast<double>(127 - wireByte); // wire-inverted (§2)
    };
    const auto hwVolume = [&](uint32_t ch) {
        return (store.hwctl[ch / 2] & (1u << (ch % 2))) != 0;
    };
    const auto channelMute = [&](uint32_t ch) {
        return (store.hwctl[ch / 2] & (1u << (2 + (ch % 2)))) != 0;
    };
    const auto followsMute = [&](uint32_t ch) {
        return (store.assigns & (1u << ch)) != 0;
    };
    const auto followsDim = [&](uint32_t ch) {
        return (store.assigns & (1u << (10 + ch))) != 0;
    };

    for (uint32_t ch = 0; ch < kD3AnalogOutputs; ++ch) {
        const auto target = outputNode(ch);
        const auto chName = std::to_string(ch + 1);
        wordParam(2 + ch / 2, D3Control::OutputVolumeBase + ch, target,
                  AM::ParameterSemantic::Level,
                  AM::ScalarDomain{0, 127, 1, AM::ScalarUnit::Generic},
                  "Output Level " + chName + " (raw)", rawLevel(ch));
        wordParam(7 + ch / 2, D3Control::OutputMuteBase + ch, target,
                  AM::ParameterSemantic::Mute,
                  AM::BooleanDomain{}, "Output Mute " + chName, channelMute(ch));
        wordParam(7 + ch / 2, D3Control::HwVolumeBase + ch, target,
                  AM::ParameterSemantic::Unknown,
                  AM::BooleanDomain{}, "Output " + chName + " Hardware Volume",
                  hwVolume(ch));
        wordParam(12, D3Control::MuteTargetBase + ch, target,
                  AM::ParameterSemantic::Unknown,
                  AM::BooleanDomain{}, "Output " + chName + " Follows Global Mute",
                  followsMute(ch));
        wordParam(12, D3Control::DimTargetBase + ch, target,
                  AM::ParameterSemantic::Unknown,
                  AM::BooleanDomain{}, "Output " + chName + " Follows Global Dim",
                  followsDim(ch));
    }
    wordParam(0, D3Control::GlobalMute, outputNode(0), AM::ParameterSemantic::Mute,
              AM::BooleanDomain{}, "Global Mute", store.mute != 0);
    wordParam(1, D3Control::GlobalDim, outputNode(0), AM::ParameterSemantic::Dim,
              AM::BooleanDomain{}, "Global Dim", store.dim != 0);
    // Scalar-domain values are doubles by the canonical state contract.
    // Live hardware (P07 2026-11-06): the pad register reads 0xFFFFFFFF
    // with no channel padded - the channel bits are ACTIVE-LOW in the
    // upper sticky bits' complement (the same ~0xFFFFFFFF form the
    // ADAT-as-SPDIF register shows). Decode the ten channel bits
    // inverted; the domain covers the full ten-bit field.
    wordParam(13, D3Control::MonitorPad, outputNode(0), AM::ParameterSemantic::Unknown,
               AM::ScalarDomain{0, 1023, 1, AM::ScalarUnit::Generic},
               "Active Monitor Pad (raw)", double(~store.monitorPad & 0x3FFu));
    wordParam(14, D3Control::MonitorVolume, outputNode(0), AM::ParameterSemantic::Unknown,
              AM::ScalarDomain{0, 127, 1, AM::ScalarUnit::Generic},
              "Monitor Knob (raw, get-only)", double(store.monitorVolume),
              AM::ParameterAccess::ReadOnly);
    // Live hardware (P07 2026-11-06): the register reads 0xFFFFFFFE with
    // the flag SET - the flag bit rides in the complement of the sticky
    // all-ones form. A plain nonzero test would read any sticky-1s pattern
    // as true; decode the flag bit inverted.
    wordParam(15, D3Control::AdatAsSpdif, static_cast<AM::NodeId>(kAdatOut),
              AM::ParameterSemantic::Unknown, AM::BooleanDomain{},
              "Optical Output Carries S/PDIF", (~store.adatAsSpdif & 1u) != 0);

    // The refresh action (no state entry; ActionDomain is excluded from
    // completeness by the canonical validator itself).
    // The whole-mixer initialization action (P08): the ONLY path that
    // establishes the host recipe over the unreadable coefficients. The
    // recipe is the reviewed all-silence baseline; partial completion is
    // reported honestly (written cells Cached, the rest stay Unknown).
    latentParam(D3Control::InitializeMixer,
                static_cast<AM::NodeId>(kMixerNode), AM::ParameterSemantic::Unknown,
                AM::ActionDomain{}, "Initialize Monitor Mixer");
    latentParam(D3Control::RefreshReadableControls,
                static_cast<AM::NodeId>(kHostCapture), AM::ParameterSemantic::Unknown,
                AM::ActionDomain{}, "Refresh Readable Controls");

    return result;
}

} // namespace ASFW::Audio::DICE::Focusrite::SPro40D3
