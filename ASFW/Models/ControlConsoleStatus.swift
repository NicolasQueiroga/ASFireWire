// SPDX-License-Identifier: Apache-2.0
// P12: the console's honest status line. Two real signals only:
// - the receiver-lock telemetry the INSTALLED dext actually exposes
//   (DiceReceiverLock) - decoded with the SAME rules as the proven
//   diagnostics text (firstLockMs/lostMask/phase), never invented bits;
// - the meter telemetry's own liveness (the last batch's age).
// There is no sample-rate surface through the 0.6.0 userclient - the
// rate reads (the DICE globals) belong to OUR dext's layer - so the
// status line shows no rate, honestly.
import Foundation

enum ControlConsoleStatus {
    /// The meters' liveness from the batch's age.
    enum MeterLiveness: Equatable {
        case never
        case live
        case stale(ageMs: UInt64)
        case stopped
    }

    static func meterLiveness(nowNs: UInt64, batch: MeterValuesBatch?, failed: Bool) -> MeterLiveness {
        if failed { return .stopped }
        guard let batch else { return .never }
        let age = nowNs > batch.timestampNs ? nowNs - batch.timestampNs : 0
        let oneSecond: UInt64 = 1_000_000_000
        return age <= 3 * oneSecond ? .live : .stale(ageMs: age / 1_000_000)
    }

    /// The receiver watch's verdict, mirroring the proven diagnostics
    /// decode: a channel observed locked is `firstLockMs != UInt32.max`;
    /// a lock loss is the lostMask bit. nil = no telemetry right now.
    static func receiverVerdict(_ lock: DiceReceiverLock?) -> String? {
        guard let lock, lock.expectedMask != 0 else { return nil }
        let count = lock.expectedMask.nonzeroBitCount
        var locked = 0
        var lost = 0
        for i in 0..<4 where lock.expectedMask & (1 << UInt32(i)) != 0 {
            if lock.firstLockMs.indices.contains(i), lock.firstLockMs[i] != UInt32.max {
                locked += 1
                if lock.lostMask & (1 << UInt32(i)) != 0 { lost += 1 }
            }
        }
        if locked == count, lost == 0 { return "stream locked" }
        if lock.phase == 1 { return "stream watching" }
        if lock.phase == 2 {
            return locked == 0 ? "stream unlocked" : "stream lock loss observed"
        }
        return lock.phaseLabel
    }
}
