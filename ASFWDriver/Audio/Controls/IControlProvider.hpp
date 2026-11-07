// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../Model/Topology/State.hpp"
#include "../Model/Topology/Topology.hpp"
#include <DriverKit/IOReturn.h>
#include <array>
#include <functional>
#include <optional>
#include <string>

namespace ASFW::Audio::Controls {
struct ControlSnapshot {
    AudioModel::Topology topology;
    AudioModel::DeviceState state;
    std::string family;
    uint64_t sequence{};
};
/// One acquired meter batch (P10b): raw device-scale values with the
/// honesty metadata. kMaxSamples matches the wire contract's bound; the
/// values are NEVER labeled with derived units (dBFS/peak/clip need
/// validated calibration, which no layer may fabricate).
struct MeterBatch {
    uint32_t sequence{};          ///< Monotonic per acquired batch.
    uint32_t generation{};        ///< Route generation at acquisition.
    uint64_t timestampNs{};      ///< Host boot-relative acquisition time.
    static constexpr size_t kMaxSamples = 32;
    size_t count{};               ///< Samples actually measured.
    std::array<int32_t, kMaxSamples> raw{};
    std::array<bool, kMaxSamples> valid{};
};

/// Controller-queue interface. The protocol owns its provider; every async
/// caller retains the protocol until completion. CopySnapshot never does I/O.
class IControlProvider {
public:
    using Completion = std::function<void(IOReturn, AudioModel::ParameterValue)>;
    virtual ~IControlProvider() = default;
    virtual void Prepare(std::function<void(IOReturn)> done) { done(kIOReturnSuccess); }
    [[nodiscard]] virtual std::optional<ControlSnapshot> CopySnapshot() const = 0;
    virtual void SetParameter(AudioModel::ParameterId, const AudioModel::ParameterValue&, Completion) = 0;
    virtual void SetRoute(AudioModel::NodeId, AudioModel::PortId, std::optional<AudioModel::PortId>, Completion done) {
        done(kIOReturnUnsupported, false);
    }

    // -- Meter telemetry (P10b): bounded, honest, cache-copy. --------------
    // The app's visibility-driven hook: the app owns the cadence and the
    // stop-on-hidden policy; the provider coalesces (one in flight, the
    // rate cap) and gives foreground control work priority. The copy NEVER
    // does I/O. Providers without telemetry honestly report nothing.
    /// Why a refresh request was not launched (or that it was): the
    /// app's back-off policy needs the reason, not a silent drop.
    enum class MeterRefreshStatus : uint8_t {
        Launched,             ///< The acquisition chain started.
        RefusedNotReady,      ///< No registered engine / no rate context.
        RefusedBusy,          ///< Foreground control work owns the engine.
        RefusedCadenceWindow, ///< Inside the 10 Hz coalescing window.
    };
    [[nodiscard]] virtual MeterRefreshStatus RequestMeterRefresh(uint32_t /*currentRateHz*/) {
        return MeterRefreshStatus::RefusedNotReady;
    }
    [[nodiscard]] virtual std::optional<MeterBatch> CopyMeterTelemetry() const {
        return std::nullopt;
    }
};
} // namespace ASFW::Audio::Controls
