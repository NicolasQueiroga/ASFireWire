// SPDX-License-Identifier: Apache-2.0
// P12 C1: the meter polling policy (pure transitions) and the model's
// live loop (the cadence, stop-on-hidden, device change, failure stop).
import Foundation
import Testing
@testable import ASFW

@MainActor
struct MeterTelemetryModelTests {

    // MARK: - The pure policy

    @Test func appearedFromIdleStarts() {
        let (next, action) = MeterPollPolicy.next(MeterPollState(), .appeared(guid: 7))
        #expect(action == .startPolling(guid: 7))
        #expect(next.pollingGuid == 7)
    }

    @Test func appearedAgainIsANoOp() {
        let state = MeterPollState(pollingGuid: 7)
        let (next, action) = MeterPollPolicy.next(state, .appeared(guid: 7))
        #expect(action == .none)
        #expect(next.pollingGuid == 7)
    }

    @Test func appearedWithAnotherGuidRestarts() {
        let state = MeterPollState(pollingGuid: 7)
        let (next, action) = MeterPollPolicy.next(state, .appeared(guid: 9))
        #expect(action == .startPolling(guid: 9))
        #expect(next.pollingGuid == 9)
    }

    @Test func disappearedStopsOnlyWhilePolling() {
        let polling = MeterPollState(pollingGuid: 7)
        #expect(MeterPollPolicy.next(polling, .disappeared).1 == .stopPolling)
        #expect(MeterPollPolicy.next(MeterPollState(), .disappeared).1 == .none)
    }

    @Test func deviceChangeRestartsOnlyWhenDifferent() {
        let polling = MeterPollState(pollingGuid: 7)
        #expect(MeterPollPolicy.next(polling, .deviceChanged(guid: 7)).1 == .none)
        #expect(MeterPollPolicy.next(polling, .deviceChanged(guid: 9)).1 == .startPolling(guid: 9))
        // Idle (hidden) surface: nothing to restart.
        #expect(MeterPollPolicy.next(MeterPollState(), .deviceChanged(guid: 9)).1 == .none)
    }

    @Test func failuresAccumulateToTheStopThenReset() {
        var state = MeterPollState(pollingGuid: 7)
        for _ in 0..<(MeterPollState.failureLimit - 1) {
            let (next, action) = MeterPollPolicy.next(state, .fetchFailed)
            #expect(action == .none)
            state = next
        }
        #expect(state.consecutiveFailures == MeterPollState.failureLimit - 1)
        let (failed, failAction) = MeterPollPolicy.next(state, .fetchFailed)
        #expect(failAction == .fail)
        #expect(failed.pollingGuid == nil)
        // A batch resets the streak.
        var streak = MeterPollState(pollingGuid: 7)
        streak.consecutiveFailures = 2
        let (reset, _) = MeterPollPolicy.next(streak, .batchArrived)
        #expect(reset.consecutiveFailures == 0)
    }

    @Test func refusalBacksOffAndABatchResets() {
        var state = MeterPollState(pollingGuid: 7)
        let (s1, a1) = MeterPollPolicy.next(state, .refused)
        #expect(a1 == .none)
        #expect(s1.backoffExponent == 1)
        let (s2, _) = MeterPollPolicy.next(s1, .refused)
        #expect(s2.backoffExponent == 2)
        // Capped.
        var s3 = s2
        for _ in 0..<5 { let (n, _) = MeterPollPolicy.next(s3, .refused); s3 = n }
        #expect(s3.backoffExponent == MeterPollState.backoffLimit)
        // A landed batch resets the cadence.
        let (s4, _) = MeterPollPolicy.next(s3, .batchArrived)
        #expect(s4.backoffExponent == 0)
        // The interval math: base 100 ms -> 200/400/800 ms, capped.
        #expect(MeterPollState.backoffInterval(baseNs: 100_000_000, exponent: 1) == 200_000_000)
        #expect(MeterPollState.backoffInterval(baseNs: 100_000_000, exponent: 3) == 800_000_000)
        #expect(MeterPollState.backoffInterval(baseNs: 100_000_000, exponent: 9) == 800_000_000)
    }

    // MARK: - The model (the live loop, short real-time waits)

    /// A minimal valid count-0 envelope (the driver's honest "nothing now").
    /// A landed batch (count 1, one valid sample) built against the
    /// parser's real layout - the healthy fetch's shape.
    private static func landedEnvelope() -> Data {
        var data = Self.emptyEnvelope()
        data.append(contentsOf: [0, 0, 0, 0, 0, 0, 0, 0])  // one sample's 8 bytes
        data[24] = 1  // count (u16 at 24)
        let at = MeterValuesDocument.headerBytes
        data.replaceSubrange(at..<at + 4, with: withUnsafeBytes(of: Int32(42).littleEndian) { Data($0) })
        data[at + 4] = 0x01  // flags: valid
        return data
    }

    private static func emptyEnvelope() -> Data {
        var data = Data(count: MeterValuesDocument.headerBytes)
        data.replaceSubrange(0..<4, with: withUnsafeBytes(of: MeterValuesDocument.magic.littleEndian) { Data($0) })
        data.replaceSubrange(4..<6, with: withUnsafeBytes(of: MeterValuesDocument.version.littleEndian) { Data($0) })
        data.replaceSubrange(6..<8, with: withUnsafeBytes(of: UInt16(MeterValuesDocument.headerBytes).littleEndian) { Data($0) })
        data[24] = 0
        data[25] = 0
        data[26] = 0
        data[27] = 0
        return data
    }

    @Test func pollerFetchesAtTheCadenceAndStopsOnHidden() async {
        let lock = NSLock()
        var asks: [UInt64] = []
        let model = MeterTelemetryModel(interval: 0.05) { guid in
            lock.lock()
            asks.append(guid)
            lock.unlock()
            // The healthy path: a LANDED batch (count > 0). An empty
            // envelope is the coalescer's refusal signal and backs off -
            // that behavior is the backoff test's territory.
            return .success(Self.landedEnvelope())
        }
        model.appeared(guid: 7)
        // Deadline-driven: wait for the observable (a second ask at the
        // cadence) with a generous margin. A fixed window races the full
        // suite's main-actor contention (the isolated run passes, the
        // loaded suite does not).
        let count = { () -> Int in
            lock.lock(); defer { lock.unlock() }
            return asks.count
        }
        let deadline = Date().addingTimeInterval(2.0)
        while count() < 2 && Date() < deadline {
            try? await Task.sleep(for: .seconds(0.02))
        }
        model.disappeared()
        let countAtStop = count()
        #expect(countAtStop >= 2)
        #expect(countAtStop <= 8)
        // Hidden: the asks stop.
        try? await Task.sleep(for: .seconds(0.2))
        lock.lock()
        let after = asks.count
        lock.unlock()
        #expect(after == countAtStop)
    }

    @Test func pollerStopsAfterTheFailureLimit() async {
        let lock = NSLock()
        var asks = 0
        let model = MeterTelemetryModel(interval: 0.02) { _ in
            lock.lock()
            asks += 1
            lock.unlock()
            return .failure(.notConnected)
        }
        model.appeared(guid: 7)
        // Deterministic: wait for the stop flag up to a deadline. Under
        // the full suite the main actor interleaves coarser, so a fixed
        // window races the loop's third iteration.
        let deadline = Date().addingTimeInterval(2.0)
        while !model.failed && Date() < deadline {
            try? await Task.sleep(for: .seconds(0.02))
        }
        lock.lock()
        let total = asks
        lock.unlock()
        // The limit stops the loop; the drift bound is generous but the
        // loop must NOT keep running (2 s at 20 ms would be ~100 asks).
        #expect(total <= MeterPollState.failureLimit + 2)
        #expect(model.failed)
    }

    @Test func pollerPublishesADecodedBatch() async {
        var envelope = Self.emptyEnvelope()
        var count: UInt16 = 1
        envelope.replaceSubrange(24..<26, with: withUnsafeBytes(of: count.littleEndian) { Data($0) })
        envelope.append(contentsOf: [0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00])
        let model = MeterTelemetryModel(interval: 0.05) { _ in .success(envelope) }
        model.appeared(guid: 7)
        try? await Task.sleep(for: .seconds(0.2))
        #expect(model.batch?.samples.count == 1)
        #expect(model.batch?.samples.first?.raw == 1)
        #expect(model.batch?.samples.first?.valid == true)
    }
}
