// SPDX-License-Identifier: Apache-2.0
// SPro40D3ControlModelTests.cpp - the pure D3 control model: validity, the
// partial-state completeness policy (strict default vs the D3 opt-in),
// wire decodings, truthful raw units, and null-not-fabricated serialization.
#include "Audio/Protocols/DICE/Focusrite/SPro40D3ControlModel.hpp"
#include "Audio/Protocols/DICE/Focusrite/SPro40D3ControlWrite.hpp"
#include "Audio/Protocols/DICE/Focusrite/SPro40D3Routing.hpp"
#include "Audio/Model/Topology/Validate.hpp"
#include "UserClient/WireFormats/ControlSnapshotDocument.hpp"
#include "UserClient/WireFormats/AVCDiscoveryDocument.hpp"
#include <cstring>
#include <gtest/gtest.h>
#include <algorithm>
#include <string>
#include <unordered_set>

namespace {
using namespace ASFW::Audio::DICE::Focusrite::SPro40D3;
namespace C = ASFW::Audio::Controls;
namespace AM = ASFW::AudioModel;

D3ControlStore Store() {
    D3ControlStore store;
    store.mute = 1;              // global mute engaged
    store.dim = 0;
    // Outputs 1 (ch 0) and 2 (ch 1): wire bytes 0x00 (level 127) and 0x7F
    // (level 0) - the wire order is INVERTED vs the level.
    store.volumes[0] = 0x007F00; // byte0 = 0x00 (ch 1), byte1 = 0x7F (ch 2)
    // Word 0: bit0 = hw volume ch 1 ON, bit3 = mute ch 2 ON.
    store.hwctl[0] = (1u << 0) | (1u << 3);
    store.assigns = (1u << 0) | (1u << 10); // ch 1 follows mute; ch 1 follows dim
    // Live wire forms (P07 hardware 2026-11-06): the pad and ADAT-as-SPDIF
    // registers carry their channel/flag bits INVERTED inside the sticky
    // all-ones complement (the pad reads 0xFFFFFFFF with nothing padded;
    // the flag register reads 0xFFFFFFFE with the flag set).
    store.monitorPad = ~2u;   // channels 1's bit padded: decode -> 2
    store.monitorVolume = 64;
    store.adatAsSpdif = ~1u;  // flag set: decode -> true
    return store;
}

constexpr AM::ParameterId VolumeId(uint32_t ch) {
    return AM::ParameterId{D3Control::OutputVolumeBase + ch};
}

TEST(SPro40D3ControlModel, TopologyValidatesButStrictStateRejectsMissingMixerCells) {
    const auto model = BuildSPro40D3Controls(Store(), {}, 7, 10);
    ASSERT_TRUE(AM::validate(model.topology));
    // The canonical validator is STRICT by default: the 288 mixer cells
    // have no state (no readback exists) and strict validation must keep
    // rejecting them for ordinary topologies.
    const auto strict = AM::validateState(model.topology, model.state);
    ASSERT_FALSE(strict);
    EXPECT_EQ(strict.error().kind, AM::StateErrorKind::MissingParameter);
    // The one narrow opt-in (plan §4): present entries stay strictly
    // validated; missing observations are permitted for THIS device.
    auto policy = AM::StateCompletenessPolicy{};
    policy.requireCompleteParameterState = false;
    // P06: an un-applied routing table publishes NO router state - the
    // same honest absence; every PRESENT entry stays strictly validated.
    policy.requireCompleteRouterState = false;
    EXPECT_TRUE(AM::validateState(model.topology, model.state, policy));
}

TEST(SPro40D3ControlModel, PermissivePolicyStillRejectsMalformedPresentState) {
    auto model = BuildSPro40D3Controls(Store(), {}, 7, 0);
    auto policy = AM::StateCompletenessPolicy{};
    policy.requireCompleteParameterState = false;
    // P06: an un-applied routing table publishes NO router state - the
    // same honest absence; every PRESENT entry stays strictly validated.
    policy.requireCompleteRouterState = false;
    // A PRESENT entry that violates its domain is never excused.
    model.state.parameters.at(VolumeId(0)).value = double(999);
    const auto result = AM::validateState(model.topology, model.state, policy);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, AM::StateErrorKind::InvalidParameterValue);
    // State naming something the topology does not know is never excused.
    auto alien = model;
    alien.state.parameters.erase(VolumeId(0));
    alien.state.parameters[AM::ParameterId{0xDEAD}] =
        AM::ParameterState{false, AM::StateOrigin::Observed};
    EXPECT_FALSE(AM::validateState(alien.topology, alien.state, policy));
}

TEST(SPro40D3ControlModel, DecodesThePinnedWireEncodings) {
    const auto model = BuildSPro40D3Controls(Store(), {}, 7, 0);
    // Levels are wire-inverted: byte 0x00 -> 127 (loudest), 0x7F -> 0.
    EXPECT_DOUBLE_EQ(std::get<double>(model.state.parameters.at(VolumeId(0)).value), 127.0);
    EXPECT_DOUBLE_EQ(std::get<double>(model.state.parameters.at(VolumeId(1)).value), 0.0);
    EXPECT_DOUBLE_EQ(std::get<double>(
                         model.state.parameters.at(AM::ParameterId{D3Control::MonitorVolume}).value),
                     64.0);
    EXPECT_EQ(std::get<bool>(model.state.parameters.at(
                                    AM::ParameterId{D3Control::GlobalMute}).value),
              true);
    EXPECT_EQ(std::get<bool>(model.state.parameters.at(
                                    AM::ParameterId{D3Control::AdatAsSpdif}).value),
              true);
    // hwctl word 0: bit0 = ch 1 follows the hardware knob; bit3 = ch 2 mute.
    EXPECT_EQ(std::get<bool>(model.state.parameters.at(
                                    AM::ParameterId{D3Control::HwVolumeBase}).value),
              true);
    EXPECT_EQ(std::get<bool>(model.state.parameters.at(
                                    AM::ParameterId{D3Control::HwVolumeBase + 1}).value),
              false);
    EXPECT_EQ(std::get<bool>(model.state.parameters.at(
                                    AM::ParameterId{D3Control::OutputMuteBase}).value),
              false);
    EXPECT_EQ(std::get<bool>(model.state.parameters.at(
                                    AM::ParameterId{D3Control::OutputMuteBase + 1}).value),
              true);
    // Assignments: mute bit 0 (ch 1), dim bit 10 (ch 1).
    EXPECT_EQ(std::get<bool>(model.state.parameters.at(
                                    AM::ParameterId{D3Control::MuteTargetBase}).value),
              true);
    EXPECT_EQ(std::get<bool>(model.state.parameters.at(
                                    AM::ParameterId{D3Control::MuteTargetBase + 1}).value),
              false);
    EXPECT_EQ(std::get<bool>(model.state.parameters.at(
                                    AM::ParameterId{D3Control::DimTargetBase}).value),
              true);
    // Raw, uncalibrated, honest: no invented dB anywhere. Scalar-domain
    // state values are doubles by the canonical contract.
    EXPECT_DOUBLE_EQ(std::get<double>(model.state.parameters.at(
                                         AM::ParameterId{D3Control::MonitorPad}).value),
                     2.0);
}

TEST(SPro40D3ControlModel, UnknownMixerCellsPublishNullNotFabricatedValues) {
    const auto model = BuildSPro40D3Controls(Store(), {}, 7, 0);
    // All 288 mixer cells exist as parameters...
    const auto cells = std::ranges::count_if(
        model.topology.parameters, [](const AM::Parameter& p) {
            return p.id.value >= D3Control::MixerCellBase &&
                   p.id.value < D3Control::MixerCellBase + 288;
        });
    EXPECT_EQ(cells, 288);
    // ...and NONE carries state. Absence is the honest unknown.
    for (uint32_t id = D3Control::MixerCellBase; id < D3Control::MixerCellBase + 288; ++id) {
        EXPECT_FALSE(model.state.parameters.contains(AM::ParameterId{id})) << id;
    }
    // The serialized document emits null for them, never a fabricated
    // 0/false (plan §4): every absent-state parameter - 288 mixer cells
    // plus the refresh action - serializes as "value":null,"origin":null.
    const auto doc = ASFW::UserClient::Wire::BuildControlSnapshotDocument(
        0x1234, model.family, model.topology, model.state, 10);
    size_t nulls = 0;
    for (size_t pos = doc.find("\"origin\":null"); pos != std::string::npos;
         pos = doc.find("\"origin\":null", pos + 1)) {
        ++nulls;
    }
    // 288 mixer cells + the P04 refresh action + the P08 mixer-init action
    // + the P06 routing surfaces (42 source selectors + the apply action):
    // every topology-only parameter serializes null until its value is
    // actually known.
    EXPECT_EQ(nulls, 333u) << doc.substr(0, 400);
    // The refresh action parameter never carries state either.
    EXPECT_FALSE(model.state.parameters.contains(
        AM::ParameterId{D3Control::RefreshReadableControls}));
}

TEST(SPro40D3ControlModel, KnownMixerCellsPublishCachedStateUnknownsStayStateless) {
    // P08: the host-written cells (the init action / later writes) are the
    // ONLY mixer state there is - no readback exists. Known cells publish
    // Cached provenance; the rest stay stateless (Unknown).
    D3MixerCells cells;
    cells.known[4][9] = true;   // slot 4, bus 9
    cells.value[4][9] = 0x1fff; // unity
    cells.known[17][15] = true;
    cells.value[17][15] = 1234;
    const auto model = BuildSPro40D3Controls(Store(), {}, 7, 0, &cells);

    const auto unity = model.state.parameters.find(
        AM::ParameterId{D3Control::MixerCellBase + 9 * 18 + 4});
    ASSERT_NE(unity, model.state.parameters.end());
    EXPECT_DOUBLE_EQ(std::get<double>(unity->second.value), double(0x1fff));
    EXPECT_EQ(unity->second.origin, AM::StateOrigin::Cached);
    const auto written = model.state.parameters.find(
        AM::ParameterId{D3Control::MixerCellBase + 15 * 18 + 17});
    ASSERT_NE(written, model.state.parameters.end());
    EXPECT_DOUBLE_EQ(std::get<double>(written->second.value), 1234.0);

    // Every other cell stays stateless - exactly 2 cell states exist.
    size_t cellStates = 0;
    for (const auto& [id, st] : model.state.parameters) {
        if (id.value >= D3Control::MixerCellBase &&
            id.value < D3Control::MixerCellBase + D3Control::kMixerCellCount) {
            ++cellStates;
        }
    }
    EXPECT_EQ(cellStates, 2u);

    // The permissive D3 policy still validates: present entries strict,
    // absent cells permitted.
    auto policy = AM::StateCompletenessPolicy{};
    policy.requireCompleteParameterState = false;
    policy.requireCompleteRouterState = false;
    EXPECT_TRUE(AM::validateState(model.topology, model.state, policy));
}

TEST(SPro40D3ControlModel, ReadableSurfaceOnlyAndNativeIdSpace) {
    const auto model = BuildSPro40D3Controls(Store(), {}, 7, 0);
    // State entries: exactly the readable group (10 channels x 5 params
    // + global mute/dim + pad + knob + optical = 55).
    EXPECT_EQ(model.state.parameters.size(), 55u);
    // The monitor knob is get-only hardware: read-only parameter.
    const auto& knob = *std::ranges::find_if(model.topology.parameters, [](const AM::Parameter& p) {
        return p.id.value == D3Control::MonitorVolume;
    });
    EXPECT_EQ(knob.access, AM::ParameterAccess::ReadOnly);
    // Physical-only controls are not parameters at all (P00 §8).
    for (const auto& p : model.topology.parameters) {
        EXPECT_EQ(p.name.find("Phantom"), std::string::npos);
        EXPECT_EQ(p.name.find("Gain"), std::string::npos);
    }
    // Native ids live below the 0x80000000 input-type projection namespace.
    for (const auto& p : model.topology.parameters) {
        EXPECT_LT(p.id.value, 0x80000000u);
    }
    EXPECT_EQ(model.family, "spro40d3");
    EXPECT_EQ(model.topology.revision, 7u);
}

// ---------------------------------------------------------------------------
// The P05 write planner: pure merges that preserve every bit the parameter
// does not own, and honest refusals for everything without a write plan.
// ---------------------------------------------------------------------------

TEST(SPro40D3ControlModel, PlannerMergesOnlyTheTargetVolumeByte) {
    // Word 0x14: byte0 = ch 1, byte1 = ch 2 (wire-inverted). Writing ch 1
    // leaves ch 2 untouched, and vice versa.
    const uint32_t current = 0x3C5A1177u; // arbitrary neighbour bits set
    const auto plan = BuildSPro40D3ParameterWrite(
        AM::ParameterId{D3Control::OutputVolumeBase}, double(50), current);
    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->position, 0x14u);
    EXPECT_EQ(plan->notice, 1u);
    EXPECT_EQ(plan->word, (current & ~0xFFu) | ((127 - 50) & 0xFFu));
    const auto planOdd = BuildSPro40D3ParameterWrite(
        AM::ParameterId{D3Control::OutputVolumeBase + 1}, double(50), current);
    ASSERT_TRUE(planOdd.has_value());
    EXPECT_EQ(planOdd->position, 0x14u);
    EXPECT_EQ(planOdd->word,
              (current & ~(0xFFu << 8)) | (((127 - 50) & 0xFFu) << 8));
    // Higher pair words address their own positions.
    const auto planCh7 = BuildSPro40D3ParameterWrite(
        AM::ParameterId{D3Control::OutputVolumeBase + 7}, double(0), 0u);
    ASSERT_TRUE(planCh7.has_value());
    EXPECT_EQ(planCh7->position, 0x14u + 3 * 4);
    EXPECT_EQ(planCh7->word, 0x7Fu << 8); // byte1 of word 3
}

// ---------------------------------------------------------------------------
// The P06 routing model: per-family knowledge, the safe profile, and the
// deterministic bundle allocation
// ---------------------------------------------------------------------------

TEST(SPro40D3ControlModel, RouterTopologyCarriesTheFrozenShape) {
    // 54 sources (None is not a port), 40 destination slots, and one
    // singleton legal bundle per legal (slot, source) pair: out slots take
    // the 0..54 superset, mixer inputs exclude Mixer-N (0..38).
    const auto model = BuildSPro40D3Controls(Store(), {}, 7, 0);
    ASSERT_TRUE(AM::validate(model.topology));
    const auto nodeIt = std::ranges::find(model.topology.nodes, AM::NodeId{0x100},
                                          &AM::Node::id);
    ASSERT_NE(nodeIt, model.topology.nodes.end());
    const auto& router = std::get<AM::RouterNode>(nodeIt->body);
    EXPECT_EQ(router.inputs.size(), 54u);
    EXPECT_EQ(router.outputs.size(), 40u);
    EXPECT_TRUE(router.writable);
    ASSERT_TRUE(router.constraints.maxSourcesPerOutput.has_value());
    EXPECT_EQ(*router.constraints.maxSourcesPerOutput, 1u);
    EXPECT_EQ(router.legalBundles.size(), 22u * 54u + 18u * 38u);
    std::unordered_set<uint64_t> pairs;
    for (const auto& bundle : router.legalBundles) {
        ASSERT_EQ(bundle.routes.size(), 1u);
        const auto& r = bundle.routes[0];
        const uint64_t key = (uint64_t(r.input.value) << 32) | r.output.value;
        EXPECT_TRUE(pairs.insert(key).second);
        const uint32_t slot = RouteSlotFromBundleId(bundle.id.value);
        const uint32_t from = RouteSourceFromBundleId(bundle.id.value);
        EXPECT_EQ(bundle.id.value, RouteBundleIdFor(slot, from));
        if (slot < kRouterOutSlots)
            EXPECT_TRUE(IsValidOutSlotSource(from)) << "slot " << slot;
        else
            EXPECT_TRUE(IsValidMixerInputSource(from)) << "slot " << slot;
    }
    // Absent state: nothing routing-related is published before an
    // application (the P04 discipline preserved).
    EXPECT_EQ(model.state.routers.find(AM::NodeId{0x100}), model.state.routers.end());
}

TEST(SPro40D3ControlModel, SafeProfileIsTheDirectPlaybackProgram) {
    // Line 1..10 <- Stream 1..10, S/PDIF <- Stream 11/12, ADAT 1..8 <-
    // Stream 13..20, loopback silent; mixer inputs all None; meters Mon 1/2.
    for (uint32_t slot = 0; slot < kRouterOutSlots; ++slot) {
        const uint32_t expect = slot < kSlotLoopbackFirst ? 19 + slot : 0;
        EXPECT_EQ(kSafeRouterOutSrc[slot], expect) << "slot " << slot;
    }
    for (uint32_t in = 0; in < kD3MixerSlots; ++in)
        EXPECT_EQ(kSafeRouterMixerSrc[in], 0u);
    EXPECT_EQ(kSafeRouterMeterSrc[0], 19u);
    EXPECT_EQ(kSafeRouterMeterSrc[1], 20u);
}

TEST(SPro40D3ControlModel, AppliedProgramPublishesCachedPerFamilyInForce) {
    D3RoutingState applied;
    applied.lowApplied = true;
    applied.outSrc = kSafeRouterOutSrc;
    applied.mixerSrc = kSafeRouterMixerSrc;
    applied.meterSrc = kSafeRouterMeterSrc;
    applied.current = D3RateFamily::Low;
    const auto low = BuildSPro40D3Controls(Store(), applied, 7, 0);
    ASSERT_TRUE(AM::validate(low.topology));
    const auto line1 = low.state.parameters.find(
        AM::ParameterId{kRouterSourceParamBase});
    ASSERT_NE(line1, low.state.parameters.end());
    EXPECT_EQ(std::get<int64_t>(line1->second.value), 19);
    EXPECT_EQ(line1->second.origin, AM::StateOrigin::Cached);
    EXPECT_NE(low.state.routers.find(AM::NodeId{0x100}), low.state.routers.end());

    // The SAME knowledge does NOT claim the high family: the low table's
    // program is not in force at 88.2/96 kHz. Nothing routing publishes.
    applied.current = D3RateFamily::High;
    const auto high = BuildSPro40D3Controls(Store(), applied, 7, 0);
    ASSERT_TRUE(AM::validate(high.topology));
    EXPECT_EQ(high.state.parameters.find(AM::ParameterId{kRouterSourceParamBase}),
              high.state.parameters.end());
    EXPECT_EQ(high.state.routers.find(AM::NodeId{0x100}),
              high.state.routers.end());

    // Both tables applied: the high family publishes too.
    applied.highApplied = true;
    const auto both = BuildSPro40D3Controls(Store(), applied, 7, 0);
    ASSERT_TRUE(AM::validate(both.topology));
    EXPECT_NE(both.state.parameters.find(AM::ParameterId{kRouterSourceParamBase}),
              both.state.parameters.end());
    EXPECT_NE(both.state.routers.find(AM::NodeId{0x100}),
              both.state.routers.end());
}

TEST(SPro40D3ControlModel, RouteSourceLabelsCoverTheFrozenEncoding) {
    EXPECT_EQ(RouteSourceLabel(0), "None");
    EXPECT_EQ(RouteSourceLabel(1), "Analog 1");
    EXPECT_EQ(RouteSourceLabel(8), "Analog 8");
    EXPECT_EQ(RouteSourceLabel(9), "S/PDIF 1");
    EXPECT_EQ(RouteSourceLabel(10), "S/PDIF 2");
    EXPECT_EQ(RouteSourceLabel(11), "ADAT 1");
    EXPECT_EQ(RouteSourceLabel(18), "ADAT 8");
    EXPECT_EQ(RouteSourceLabel(19), "Stream 1");
    EXPECT_EQ(RouteSourceLabel(38), "Stream 20");
    EXPECT_EQ(RouteSourceLabel(39), "Mixer 1");
    EXPECT_EQ(RouteSourceLabel(54), "Mixer 16");
}

TEST(SPro40D3ControlModel, PlannerMergesSingleBitsInSharedWords) {
    // hwctl word bits: bit0/1 = hw volume, bit2/3 = mute. Writing one
    // channel's mute preserves the other channels' bits.
    const uint32_t hwctl = 0x0000'000F; // all four bits of word 0 set
    const auto muteCh2Off = BuildSPro40D3ParameterWrite(
        AM::ParameterId{D3Control::OutputMuteBase + 1}, false, hwctl);
    ASSERT_TRUE(muteCh2Off.has_value());
    EXPECT_EQ(muteCh2Off->position, 0x28u);
    EXPECT_EQ(muteCh2Off->notice, 1u);
    EXPECT_EQ(muteCh2Off->word, 0x0000'0007u); // 0xF minus bit3 // bit3 (mute ch 2) cleared
    const auto hwCh1Off = BuildSPro40D3ParameterWrite(
        AM::ParameterId{D3Control::HwVolumeBase + 0}, false, hwctl);
    ASSERT_TRUE(hwCh1Off.has_value());
    EXPECT_EQ(hwCh1Off->word, 0x0000'000Eu); // bit0 cleared, mutes intact
    // The assignments word: mute targets bits 0-9, dim targets bits 10-19;
    // upper reserved bits survive every merge.
    const uint32_t assigns = 0xFFF0'0000u | 0x555u;
    const auto dimCh5On = BuildSPro40D3ParameterWrite(
        AM::ParameterId{D3Control::DimTargetBase + 4}, true, assigns);
    ASSERT_TRUE(dimCh5On.has_value());
    EXPECT_EQ(dimCh5On->position, 0x3cu);
    EXPECT_EQ(dimCh5On->notice, 3u);
    EXPECT_EQ(dimCh5On->word, assigns | (1u << (10 + 4)));
    const auto muteCh10Off = BuildSPro40D3ParameterWrite(
        AM::ParameterId{D3Control::MuteTargetBase + 9}, false, assigns | (1u << 9));
    ASSERT_TRUE(muteCh10Off.has_value());
    EXPECT_EQ(muteCh10Off->word, assigns & ~(1u << 9));
}

TEST(SPro40D3ControlModel, PlannerWholeWordFormsAndPadPassThrough) {
    // Global flags and optical mode use the demonstrated whole-word forms.
    const auto mute = BuildSPro40D3ParameterWrite(
        AM::ParameterId{D3Control::GlobalMute}, true, 0xDEADu);
    ASSERT_TRUE(mute.has_value());
    EXPECT_EQ(mute->position, 0x0cu);
    EXPECT_EQ(mute->word, 1u);
    EXPECT_EQ(mute->notice, 2u);
    const auto optical = BuildSPro40D3ParameterWrite(
        AM::ParameterId{D3Control::AdatAsSpdif}, false, 1u);
    ASSERT_TRUE(optical.has_value());
    EXPECT_EQ(optical->position, 0x5cu);
    EXPECT_EQ(optical->word, 0u);
    EXPECT_EQ(optical->notice, 4u);
    // The pad passes the RAW selection through (encoding not proven, S8).
    const auto pad = BuildSPro40D3ParameterWrite(
        AM::ParameterId{D3Control::MonitorPad}, double(9), 0u);
    ASSERT_TRUE(pad.has_value());
    EXPECT_EQ(pad->position, 0x40u);
    EXPECT_EQ(pad->word, 9u);
    EXPECT_EQ(pad->notice, 3u);
}

TEST(SPro40D3ControlModel, PlannerRefusesEverythingWithoutAWritePlan) {
    // The get-only knob, the refresh action, the mixer cells and unknown
    // ids have no plan.
    EXPECT_FALSE(BuildSPro40D3ParameterWrite(
        AM::ParameterId{D3Control::MonitorVolume}, double(10), 0u).has_value());
    EXPECT_FALSE(BuildSPro40D3ParameterWrite(
        AM::ParameterId{D3Control::RefreshReadableControls}, true, 0u).has_value());
    EXPECT_FALSE(BuildSPro40D3ParameterWrite(
        AM::ParameterId{D3Control::MixerCellBase}, double(0.5), 0u).has_value());
    EXPECT_FALSE(BuildSPro40D3ParameterWrite(
        AM::ParameterId{0x1234'5678}, true, 0u).has_value());
    EXPECT_FALSE(BuildSPro40D3ParameterWrite(
        AM::ParameterId{D3Control::OutputVolumeBase + 10}, double(1), 0u).has_value());
    // Wrong types and out-of-domain values refuse (defense in depth behind
    // the canonical validator).
    EXPECT_FALSE(BuildSPro40D3ParameterWrite(
        AM::ParameterId{D3Control::OutputVolumeBase}, int64_t(50), 0u).has_value());
    EXPECT_FALSE(BuildSPro40D3ParameterWrite(
        AM::ParameterId{D3Control::GlobalMute}, double(1), 0u).has_value());
    EXPECT_FALSE(BuildSPro40D3ParameterWrite(
        AM::ParameterId{D3Control::OutputVolumeBase}, double(NAN), 0u).has_value());
    EXPECT_FALSE(BuildSPro40D3ParameterWrite(
        AM::ParameterId{D3Control::OutputVolumeBase}, double(128), 0u).has_value());
    EXPECT_FALSE(BuildSPro40D3ParameterWrite(
        AM::ParameterId{D3Control::OutputVolumeBase}, double(-1), 0u).has_value());
    EXPECT_FALSE(BuildSPro40D3ParameterWrite(
        AM::ParameterId{D3Control::MonitorPad}, double(16), 0u).has_value());
    EXPECT_FALSE(BuildSPro40D3ParameterWrite(
        AM::ParameterId{D3Control::MonitorPad}, double(2.5), 0u).has_value());
}

TEST(SPro40D3ControlModel, UnknownWordsPublishTopologyOnly) {
    // The confidence mask: an unobserved word publishes its parameters as
    // topology-only (absent state), with the same convention the mixer
    // cells already use. Store() seeds observed words; clearing one drops
    // exactly its parameters.
    auto store = Store();
    store.observed[2] = false; // volumes word 0 -> outputs 1 and 2
    const auto model = BuildSPro40D3Controls(store, {}, 7, 0);
    EXPECT_EQ(model.state.parameters.find(AM::ParameterId{D3Control::OutputVolumeBase}),
              model.state.parameters.end());
    EXPECT_EQ(model.state.parameters.find(AM::ParameterId{D3Control::OutputVolumeBase + 1}),
              model.state.parameters.end());
    // The neighbours on other words stay observed.
    EXPECT_NE(model.state.parameters.find(AM::ParameterId{D3Control::OutputVolumeBase + 2}),
              model.state.parameters.end());
    EXPECT_NE(model.state.parameters.find(AM::ParameterId{D3Control::GlobalMute}),
              model.state.parameters.end());
    // The topology still lists the parameters (writable once re-observed).
    const auto top = std::ranges::find(model.topology.parameters,
                                       AM::ParameterId{D3Control::OutputVolumeBase},
                                       &AM::Parameter::id);
    ASSERT_NE(top, model.topology.parameters.end());
}

TEST(SPro40D3ControlModel, MixerTopologyMatchesTheCapabilityBoundary) {
    const auto model = BuildSPro40D3Controls(Store(), {}, 7, 0);
    const auto mixer = std::ranges::find_if(model.topology.nodes, [](const AM::Node& n) {
        return std::holds_alternative<AM::MixerNode>(n.body);
    });
    ASSERT_NE(mixer, model.topology.nodes.end());
    const auto& body = std::get<AM::MixerNode>(mixer->body);
    EXPECT_EQ(body.inputs.size(), 18u);
    EXPECT_EQ(body.outputs.size(), 16u);
    EXPECT_EQ(body.crosspoints.size(), 288u);
}


TEST(SPro40D3ControlModel, FullSnapshotDocumentPagesReassembleWithinTheWireLimit) {
    // The P08 gate's own ask: the full control snapshot - all 288 mixer
    // cells initialized (the init recipe's unity), the honest maximum
    // document - must traverse the paged wire envelope (the same one the
    // app consumes) with intact page-by-page reassembly.
    D3MixerCells cells;
    for (auto& slot : cells.known) std::fill(slot.begin(), slot.end(), true);
    for (auto& row : cells.value) std::fill(row.begin(), row.end(), 0x1fff);
    const auto model = BuildSPro40D3Controls(Store(), {}, 7, 0, &cells);
    const auto doc = ASFW::UserClient::Wire::BuildControlSnapshotDocument(
        0xD3D3'0000'0000'0001ull, model.family, model.topology, model.state, 10);
    ASSERT_GT(doc.size(), 4096u) << "the full D3 snapshot must need several pages";

    std::string reassembled;
    const auto checksum = ASFW::UserClient::Wire::Fnv1a32(doc);
    for (uint32_t offset = 0;;) {
        const auto page = ASFW::UserClient::Wire::SerializeDiscoveryPage(
            doc, 7, 9, offset, 4096);
        ASSERT_LE(page.size(), 4096U);
        ASFW::UserClient::Wire::AVCDiscoveryPageWire header{};
        std::memcpy(&header, page.data(), sizeof(header));
        EXPECT_EQ(header.totalBytes, doc.size());
        EXPECT_EQ(header.checksum, checksum);
        EXPECT_EQ(header.offset, offset);
        if (header.length == 0) break;
        reassembled.append(reinterpret_cast<const char*>(page.data() + sizeof(header)),
                           header.length);
        offset += header.length;
    }
    EXPECT_EQ(reassembled, doc);
}

} // namespace
