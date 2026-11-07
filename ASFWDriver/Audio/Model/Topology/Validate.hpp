// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
// Ported from midi:ADKVirtualAudioLab/Core/AudioModel (AUAA semantic core, FW-202).
#pragma once

#include <cstdint>

#include "State.hpp"
#include "Topology.hpp"

#include <expected>
#include <string>
#include <vector>

namespace ASFW::AudioModel {

enum class TopologyErrorKind : uint8_t {
    DuplicateId,
    NonexistentNode,
    NonexistentPort,
    NonexistentCrosspoint,
    InvalidChannelCount,
    MissingSignalIdentity,
    InvalidPortDirection,
    ForeignPortReference,
    DuplicateRouteOrCrosspoint,
    IncompatibleChannelCount,
    MultipleDriversOnInput,
    InvalidDomain,
    InvalidConstraint,
};

struct TopologyError {
    TopologyErrorKind kind;
    std::string message;
};

enum class StateErrorKind : uint8_t {
    TopologyRevisionMismatch,
    MissingParameter,
    NonexistentParameter,
    InvalidParameterValue,
    InvalidParameterState,
    ReadOnlyParameter,
    ParameterNotApplicable,
    MissingRouter,
    NonexistentRouter,
    NonexistentRouteBundle,
    DuplicateActiveRouteBundle,
    RoutingConstraintViolated,
    MissingMeter,
    NonexistentMeter,
    InvalidMeterValue,
};

struct StateError {
    StateErrorKind kind;
    std::string message;
};

// Structural Topology Validation
std::vector<TopologyError> validateAll(const Topology& topology);
std::expected<void, TopologyError> validate(const Topology& topology);

// Dynamic State Legality Validation
[[nodiscard]] std::expected<void, StateError> validateParameterValue(
    const Parameter& parameter, const ParameterValue& value);
[[nodiscard]] std::expected<void, StateError> validateParameterState(
    const Parameter& parameter, const ParameterState& state);
/// Authorization and effective-domain validation before invoking any binding.
[[nodiscard]] std::expected<void, StateError> validateParameterWrite(
    const Topology& topology, const DeviceState& state, ParameterId id,
    const ParameterValue& value);

std::expected<void, StateError> validateParameterValue(
    const Topology& topology,
    ParameterId id,
    const ParameterValue& value);

/// Resolve one output edit to its indivisible route bundle and validate the result.
[[nodiscard]] std::expected<RouterState, StateError> prepareRouteWrite(
    const Topology&, const DeviceState&, NodeId router, PortId output, std::optional<PortId> input);

std::expected<void, StateError> validateRouterState(
    const Topology& topology,
    NodeId routerNodeId,
    const RouterState& routerState);

std::expected<void, StateError> validateMeterValue(
    const Topology& topology,
    MeterId id,
    double value);

/// Completeness requirements for validateState. The default is the strict
/// original behavior: every non-Action parameter, every router and every
/// meter must have state. A device whose protocol cannot read everything
/// back (a write-only mixer/router surface with no readback) opts into
/// validating every PRESENT entry strictly while permitting missing
/// observations - absence is then the honest representation of unknown
/// value, never an error, and never permission for a present entry to be
/// malformed. State that names something the topology does not is still
/// rejected under every policy (plan §4: one narrow explicit policy, not a
/// weakened validator).
struct StateCompletenessPolicy {
    bool requireCompleteParameterState{true};
    bool requireCompleteRouterState{true};
    bool requireCompleteMeterState{true};

    [[nodiscard]] constexpr bool strict() const noexcept {
        return requireCompleteParameterState && requireCompleteRouterState &&
               requireCompleteMeterState;
    }
};

// Complete device state snapshot validation
std::expected<void, StateError> validateState(
    const Topology& topology,
    const DeviceState& state,
    StateCompletenessPolicy policy = {});

} // namespace ASFW::AudioModel
