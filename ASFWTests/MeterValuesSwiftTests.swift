// SPDX-License-Identifier: Apache-2.0
// The P10a meter contract on the Swift side: the golden bytes from the
// driver's C++ pin test decode to the same batch, and every corruption
// the C++ parse rejects is rejected here too - the two parses must never
// drift.
import Foundation
import Testing
@testable import ASFW

struct MeterValuesDocumentTests {
    /// The driver-side golden fixture (MeterValuesWireTests,
    /// TheLayoutIsPinnedForTheSwiftConsumer): sequence 1, generation 7,
    /// timestamp 1000, samples [-1 valid, 0 invalid, 42 valid].
    static var golden: Data { Data([
        0x41, 0x53, 0x4D, 0x56,             // "ASMV" little-endian
        0x01, 0x00,                         // version 1
        0x1C, 0x00,                         // header 28 bytes
        0x01, 0x00, 0x00, 0x00,             // sequence 1
        0x07, 0x00, 0x00, 0x00,             // generation 7
        0xE8, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // timestamp 1000
        0x03, 0x00,                         // count 3
        0x00, 0x00,                         // reserved 0
        0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x00, 0x00, 0x00, // -1, valid
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 0, not measured
        0x2A, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, // 42, valid
    ]) }

    @Test func goldenEnvelopeDecodes() throws {
        let batch = try MeterValuesDocument.parse(Self.golden)
        #expect(batch.sequence == 1)
        #expect(batch.generation == 7)
        #expect(batch.timestampNs == 1000)
        #expect(batch.samples.count == 3)
        #expect(batch.samples[0].raw == -1 && batch.samples[0].valid)
        #expect(batch.samples[1].raw == 0 && !batch.samples[1].valid) // "not measured" is data
        #expect(batch.samples[2].raw == 42 && batch.samples[2].valid)
    }

    @Test func emptyBatchMeansNothingMeasured() throws {
        // Before any acquisition the driver serves the honest count-0
        // envelope: no samples, no fabricated values.
        var empty = Self.golden.prefix(28)
        empty[24] = 0
        empty[25] = 0
        let batch = try MeterValuesDocument.parse(Data(empty))
        #expect(batch.samples.isEmpty)
        #expect(batch.sequence == 1)
    }

    @Test func corruptionsAreRejected() {
        // Truncated, trailing, bad magic, bad version, dirty padding,
        // unknown flags: every rejection the C++ parse makes.
        let good = Self.golden
        #expect(throws: NSError.self) {
            _ = try MeterValuesDocument.parse(good.prefix(16))
        }
        #expect(throws: NSError.self) {
            _ = try MeterValuesDocument.parse(good + [0])
        }
        var badMagic = good; badMagic[0] = ~badMagic[0]
        #expect(throws: NSError.self) {
            _ = try MeterValuesDocument.parse(badMagic)
        }
        var badVersion = good; badVersion[4] = 9
        #expect(throws: NSError.self) {
            _ = try MeterValuesDocument.parse(badVersion)
        }
        var dirtyPad = good; dirtyPad[28 + 5] = 1
        #expect(throws: NSError.self) {
            _ = try MeterValuesDocument.parse(dirtyPad)
        }
        var dirtyFlags = good; dirtyFlags[28 + 4] = 0x02
        #expect(throws: NSError.self) {
            _ = try MeterValuesDocument.parse(dirtyFlags)
        }
    }
}
