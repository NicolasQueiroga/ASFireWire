// SPDX-License-Identifier: Apache-2.0
// P12: the status line's pure derivations - the meter liveness from the
// batch's age, and the receiver verdict mirroring the proven diagnostics
// decode (firstLockMs/lostMask/phase).
import Foundation
import Testing
@testable import ASFW

struct ControlConsoleStatusTests {

    private static func batch(ts: UInt64) -> MeterValuesBatch {
        MeterValuesBatch(sequence: 1, generation: 1, timestampNs: ts, samples: [])
    }

    @Test func livenessNeverWithoutABatch() {
        #expect(ControlConsoleStatus.meterLiveness(nowNs: 1_000, batch: nil, failed: false) == .never)
    }

    @Test func livenessStoppedWhenFailed() {
        #expect(ControlConsoleStatus.meterLiveness(nowNs: 1, batch: Self.batch(ts: 1), failed: true) == .stopped)
    }

    @Test func livenessLiveWithinThreeSeconds() {
        let now: UInt64 = 10_000_000_000
        #expect(ControlConsoleStatus.meterLiveness(nowNs: now, batch: Self.batch(ts: now), failed: false) == .live)
        #expect(ControlConsoleStatus.meterLiveness(nowNs: now, batch: Self.batch(ts: now - 3_000_000_000), failed: false) == .live)
    }

    @Test func livenessStalePastThreeSeconds() {
        let now: UInt64 = 10_000_000_000
        let result = ControlConsoleStatus.meterLiveness(nowNs: now, batch: Self.batch(ts: now - 5_000_000_000), failed: false)
        #expect(result == .stale(ageMs: 5_000))
    }

    private static func lock(phase: UInt32 = 2, expected: UInt32 = 0b0011,
                             first: [UInt32] = [0, 0, UInt32.max, UInt32.max],
                             lost: UInt32 = 0) -> DiceReceiverLock {
        DiceReceiverLock(guid: 7, generation: 1, phase: phase, elapsedMs: 500, samples: 10,
                         readErrors: 0, lastError: 0, status: 0, extStatus: 0,
                         slipHistory: 0, expectedMask: expected, observedMask: expected,
                         lostMask: lost, channels: [1, 2, 0, 0], firstLockMs: first)
    }

    @Test func verdictNilWithoutTelemetryOrStreams() {
        #expect(ControlConsoleStatus.receiverVerdict(nil) == nil)
        #expect(ControlConsoleStatus.receiverVerdict(Self.lock(expected: 0)) == nil)
    }

    @Test func verdictLockedWhenEveryExpectedChannelLocked() {
        #expect(ControlConsoleStatus.receiverVerdict(Self.lock()) == "stream locked")
    }

    @Test func verdictWatchingWhilePhaseOne() {
        // Watching and nothing locked yet: the watch is still running.
        // (A locked-first observation while phase 1 runs is a LOCK -
        // the evidence beats the phase - and reads "stream locked".)
        #expect(ControlConsoleStatus.receiverVerdict(
            Self.lock(phase: 1, first: [UInt32.max, UInt32.max, UInt32.max, UInt32.max]))
            == "stream watching")
    }

    @Test func verdictUnlockedWhenNothingEverLocked() {
        #expect(ControlConsoleStatus.receiverVerdict(
            Self.lock(first: [UInt32.max, UInt32.max, UInt32.max, UInt32.max])) == "stream unlocked")
    }

    @Test func verdictLockLossWhenLockedThenLost() {
        #expect(ControlConsoleStatus.receiverVerdict(Self.lock(lost: 0b0001)) == "stream lock loss observed")
    }
}
