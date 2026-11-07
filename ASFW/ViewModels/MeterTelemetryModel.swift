// SPDX-License-Identifier: Apache-2.0
// P12 C1: the meter telemetry poller. The driver coalesces refreshes
// (P10b: one in flight, >=100 ms apart, refuses while control work runs,
// count-0 honest) - the app's job is only WHEN to ask: at the 10 Hz cap
// while the meter surface is visible, never while hidden, restart on
// device change. The policy is a pure state machine (testable without a
// clock); the model is the thin shell that drives it.
import Foundation
import Observation

/// The meter polling policy: which event, what happens. No clock, no I/O.
enum MeterPollEvent: Equatable {
    case appeared(guid: UInt64)
    case disappeared
    case deviceChanged(guid: UInt64)
    case batchArrived
    case fetchFailed
    /// The driver refused this ask (the coalescer's Busy/NotReady/Window -
    /// DATA, not an error): the cadence backs off.
    case refused
}

enum MeterPollAction: Equatable {
    case none
    case startPolling(guid: UInt64)
    case stopPolling
    /// Stop and surface the failure (the transport is dead, not busy).
    case fail
}

struct MeterPollState: Equatable {
    var pollingGuid: UInt64?
    /// Consecutive fetch failures while polling. A coalescer refusal is
    /// never an error (the driver answers count-0), so only transport
    /// failures reach here.
    var consecutiveFailures = 0
    /// The current ask interval's backoff exponent (0 = the base cadence).
    /// Every refusal doubles the wait; a landed batch resets it.
    var backoffExponent = 0

    static let failureLimit = 3
    static let backoffLimit = 3

    /// The interval for the current exponent, in nanoseconds (the caller's
    /// base times 2^exponent, capped).
    static func backoffInterval(baseNs: UInt64, exponent: Int) -> UInt64 {
        let e = min(max(exponent, 0), backoffLimit)
        return baseNs << UInt64(e)
    }
}

enum MeterPollPolicy {
    static func next(_ state: MeterPollState, _ event: MeterPollEvent) -> (MeterPollState, MeterPollAction) {
        var state = state
        switch event {
        case .appeared(let guid):
            if state.pollingGuid == nil {
                state.pollingGuid = guid
                state.consecutiveFailures = 0
                return (state, .startPolling(guid: guid))
            }
            if state.pollingGuid == guid { return (state, .none) }
            state.pollingGuid = guid
            state.consecutiveFailures = 0
            return (state, .startPolling(guid: guid))
        case .disappeared:
            guard state.pollingGuid != nil else { return (state, .none) }
            state.pollingGuid = nil
            state.consecutiveFailures = 0
            return (state, .stopPolling)
        case .deviceChanged(let guid):
            if state.pollingGuid == guid { return (state, .none) }
            guard state.pollingGuid != nil else { return (state, .none) }
            state.pollingGuid = guid
            state.consecutiveFailures = 0
            return (state, .startPolling(guid: guid))
        case .batchArrived:
            state.consecutiveFailures = 0
            state.backoffExponent = 0
            return (state, .none)
        case .refused:
            guard state.pollingGuid != nil else { return (state, .none) }
            state.backoffExponent = min(state.backoffExponent + 1, MeterPollState.backoffLimit)
            return (state, .none)
        case .fetchFailed:
            guard state.pollingGuid != nil else { return (state, .none) }
            state.consecutiveFailures += 1
            if state.consecutiveFailures >= MeterPollState.failureLimit {
                state.pollingGuid = nil
                state.consecutiveFailures = 0
                return (state, .fail)
            }
            return (state, .none)
        }
    }
}

/// The thin shell: drives the policy on the main actor, asks the driver
/// at the 10 Hz cap, publishes what the device actually returned.
@MainActor
@Observable
final class MeterTelemetryModel {
    /// The latest batch the device returned (raw values, per the P10a
    /// envelope). nil until the first batch arrives; kept (stale) when
    /// polling stops so the UI can label its own age.
    private(set) var batch: MeterValuesBatch?
    private(set) var failed = false

    private var state = MeterPollState()
    private var task: Task<Void, Never>?
    private let fetch: (_ guid: UInt64) -> Result<Data, ControlsDriverError>
    /// The ask cadence: the P10b contract's cap (10 Hz). The effective
    /// wait backs off while the driver refuses.
    private let interval: TimeInterval

    init(interval: TimeInterval = 0.1,
         fetch: @escaping (_ guid: UInt64) -> Result<Data, ControlsDriverError>) {
        self.interval = interval
        self.fetch = fetch
    }

    func appeared(guid: UInt64) {
        let (next, action) = MeterPollPolicy.next(state, .appeared(guid: guid))
        state = next
        run(action)
    }

    func disappeared() {
        let (next, action) = MeterPollPolicy.next(state, .disappeared)
        state = next
        run(action)
    }

    func deviceChanged(guid: UInt64) {
        let (next, action) = MeterPollPolicy.next(state, .deviceChanged(guid: guid))
        state = next
        run(action)
    }

    private func run(_ action: MeterPollAction) {
        switch action {
        case .none:
            break
        case .fail:
            task?.cancel()
            task = nil
            failed = true
        case .stopPolling:
            task?.cancel()
            task = nil
        case .startPolling(let guid):
            task?.cancel()
            failed = false
            task = Task { [weak self] in
                guard let self else { return }
                while !Task.isCancelled {
                    let result = self.fetch(guid)
                    if Task.isCancelled { break }
                    switch result {
                    case .success(let data):
                        if let batch = try? MeterValuesDocument.parse(data) {
                            self.batch = batch
                        }
                        let landed = self.batch != nil && !(self.batch?.samples.isEmpty ?? true)
                        let event: MeterPollEvent = landed ? .batchArrived : .refused
                        let (nextState, _) = MeterPollPolicy.next(self.state, event)
                        self.state = nextState
                    case .failure:
                        let (next, nextAction) = MeterPollPolicy.next(self.state, .fetchFailed)
                        self.state = next
                        if nextAction == .fail {
                            self.failed = true
                            return
                        }
                    }
                    let waitNs = MeterPollState.backoffInterval(
                        baseNs: UInt64(self.interval * 1_000_000_000),
                        exponent: self.state.backoffExponent)
                    try? await Task.sleep(for: .nanoseconds(waitNs))
                }
            }
        }
    }
}
