// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// SPro40D3ControlWrite.hpp - the pure write planner for the D3 control
// surface (P05). One function turns (parameter id, desired value, the
// FRESH current word from the device) into the merged wire word plus its
// notice number - never a full-word overwrite: pair-packed and shared-bit
// words merge so adjacent channels and reserved bits survive (plan §3).
//
// TRUTHFULNESS CONTRACT (plan P05; P00 baseline §2 row 43):
// - The notice numbers are the reference's own: set_output_vol /
//   set_vol_hwctl_mute -> 1, set_mute / set_dim -> 2,
//   set_active_monitor_pad / set_dim_mute_hwctl -> 3,
//   set_use_adat_as_spdif -> 4. The engine chains every set_control with
//   its notice and distinguishes the write, notice and readback outcomes.
// - Output levels merge one byte into the pair-packed volume word
//   (wire-inverted vs 127, exactly the model's decode inverted); mutes and
//   hardware assignments merge single bits in the hwctl words; target
//   assignments merge single bits in one shared word. The global flags,
//   monitor pad and optical mode use their demonstrated whole-word setter
//   forms (1/0 for flags, the raw pad selection).
// - The monitor pad write passes the RAW value through: the pad's wire
//   encoding is not bit-proven from the captures (§8), so the planner
//   never interprets it.
// - The get-only monitor knob, the refresh action and the mixer cells have
//   no write plan: they are rejected here (P08 owns the mixer write path).
// - The planner never decides publication: the protocol re-reads the word
//   after writing and publishes the OBSERVED result. A successful notice is
//   not output readback; a failed write may have applied.

#pragma once

#include "SPro40D3ControlModel.hpp"

#include <cmath>
#include <cstdint>
#include <optional>

namespace ASFW::Audio::DICE::Focusrite::SPro40D3 {

/// A planned single-word control write: the merged word value, its control
/// position, and the reference notice the engine pairs with the set.
struct D3ControlWrite {
    uint32_t position{};
    uint32_t word{};
    uint32_t notice{};
};

/// The control word backing a writable parameter, for the fresh read the
/// merge needs. Returns nothing when the parameter has no P05 write path.
[[nodiscard]] inline std::optional<uint32_t> D3WritableWordPosition(
    AudioModel::ParameterId id) {
    const uint32_t v = id.value;
    const auto channelOf = [v](uint32_t base) -> std::optional<uint32_t> {
        if (v < base || v >= base + kD3AnalogOutputs) return std::nullopt;
        return v - base;
    };
    if (const auto ch = channelOf(D3Control::OutputVolumeBase)) {
        return uint32_t{0x14} + (*ch / 2) * 4;
    }
    if (const auto ch = channelOf(D3Control::OutputMuteBase)) {
        return uint32_t{0x28} + (*ch / 2) * 4;
    }
    if (const auto ch = channelOf(D3Control::HwVolumeBase)) {
        return uint32_t{0x28} + (*ch / 2) * 4;
    }
    if (channelOf(D3Control::MuteTargetBase) || channelOf(D3Control::DimTargetBase)) {
        return uint32_t{0x3c};
    }
    switch (v) {
    case D3Control::GlobalMute: return uint32_t{0x0c};
    case D3Control::GlobalDim: return uint32_t{0x10};
    case D3Control::MonitorPad: return uint32_t{0x40};
    case D3Control::AdatAsSpdif: return uint32_t{0x5c};
    default: return std::nullopt;
    }
}

/// The merge itself. `currentWord` must be the FRESH read of the word the
/// planner returns above; the merge preserves every bit the parameter does
/// not own. Rejects non-boolean values for boolean domains, non-finite or
/// out-of-domain scalars, and every parameter without a P05 write plan
/// (the planner is defense in depth behind the canonical validator).
[[nodiscard]] inline std::optional<D3ControlWrite> BuildSPro40D3ParameterWrite(
    AudioModel::ParameterId id, const AudioModel::ParameterValue& value,
    uint32_t currentWord) {
    namespace AM = ASFW::AudioModel;
    const uint32_t v = id.value;
    const auto channelOf = [v](uint32_t base) -> std::optional<uint32_t> {
        if (v < base || v >= base + kD3AnalogOutputs) return std::nullopt;
        return v - base;
    };
    const auto flagOf = [&value]() -> std::optional<uint32_t> {
        if (!std::holds_alternative<bool>(value)) return std::nullopt;
        return std::get<bool>(value) ? 1u : 0u;
    };
    const auto scalarOf = [&value](double lo, double hi) -> std::optional<double> {
        if (!std::holds_alternative<double>(value)) return std::nullopt;
        const double d = std::get<double>(value);
        if (!std::isfinite(d) || d < lo || d > hi) return std::nullopt;
        return d;
    };
    // Output levels: one wire-inverted byte in the pair-packed volume word.
    if (const auto ch = channelOf(D3Control::OutputVolumeBase)) {
        const auto level = scalarOf(0, 127);
        if (!level) return std::nullopt;
        const uint32_t byte = static_cast<uint32_t>(127 - *level);
        const uint32_t shift = (*ch % 2) * 8;
        const uint32_t mask = 0xFFu << shift;
        return D3ControlWrite{uint32_t{0x14} + (*ch / 2) * 4,
                              (currentWord & ~mask) | (byte << shift), 1};
    }
    // Per-channel mutes and hardware-volume assignments: two bits per word.
    if (const auto ch = channelOf(D3Control::OutputMuteBase)) {
        const auto flag = flagOf();
        if (!flag) return std::nullopt;
        const uint32_t shift = 2 + (*ch % 2);
        const uint32_t bit = *flag << shift;
        return D3ControlWrite{uint32_t{0x28} + (*ch / 2) * 4,
                              (currentWord & ~(1u << shift)) | bit, 1};
    }
    if (const auto ch = channelOf(D3Control::HwVolumeBase)) {
        const auto flag = flagOf();
        if (!flag) return std::nullopt;
        const uint32_t shift = *ch % 2;
        const uint32_t bit = *flag << shift;
        return D3ControlWrite{uint32_t{0x28} + (*ch / 2) * 4,
                              (currentWord & ~(1u << shift)) | bit, 1};
    }
    // Mute/dim targets: single bits in one shared word.
    if (const auto ch = channelOf(D3Control::MuteTargetBase)) {
        const auto flag = flagOf();
        if (!flag) return std::nullopt;
        const uint32_t shift = *ch;
        const uint32_t bit = *flag << shift;
        return D3ControlWrite{uint32_t{0x3c},
                              (currentWord & ~(1u << shift)) | bit, 3};
    }
    if (const auto ch = channelOf(D3Control::DimTargetBase)) {
        const auto flag = flagOf();
        if (!flag) return std::nullopt;
        const uint32_t shift = 10 + *ch;
        const uint32_t bit = *flag << shift;
        return D3ControlWrite{uint32_t{0x3c},
                              (currentWord & ~(1u << shift)) | bit, 3};
    }
    // Global flags: whole-word demonstrated setter form.
    if (v == D3Control::GlobalMute || v == D3Control::GlobalDim) {
        const auto flag = flagOf();
        if (!flag) return std::nullopt;
        return D3ControlWrite{v == D3Control::GlobalMute ? uint32_t{0x0c} : uint32_t{0x10},
                              *flag, 2};
    }
    // Monitor pad: the RAW selection, encoding not interpreted (§8).
    if (v == D3Control::MonitorPad) {
        const auto pad = scalarOf(0, 15);
        if (!pad) return std::nullopt;
        if (*pad != std::floor(*pad)) return std::nullopt;
        return D3ControlWrite{uint32_t{0x40}, static_cast<uint32_t>(*pad), 3};
    }
    // Optical output mode: whole-word demonstrated setter form.
    if (v == D3Control::AdatAsSpdif) {
        const auto flag = flagOf();
        if (!flag) return std::nullopt;
        return D3ControlWrite{uint32_t{0x5c}, *flag, 4};
    }
    return std::nullopt; // get-only knob, refresh action, mixer cells (P08)
}

} // namespace ASFW::Audio::DICE::Focusrite::SPro40D3
