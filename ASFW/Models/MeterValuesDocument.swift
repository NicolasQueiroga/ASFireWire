// SPDX-License-Identifier: Apache-2.0
// The P10a meter envelope, the Swift consumer side. The byte layout is
// pinned by the driver-side golden test (MeterValuesWireTests); this
// parse mirrors the C++ ParseMeterValues rejection-for-rejection. The
// values are RAW (the device's own scale): no dBFS, no peak, no clip -
// those need calibrated conversions nothing may fabricate.
import Foundation

/// One parsed meter batch: the honesty metadata plus the raw samples.
/// `valid[i] == false` means "not measured now" - DATA, never a zero.
struct MeterValuesBatch: Equatable, Sendable {
    struct Sample: Equatable, Sendable {
        var raw: Int32
        var valid: Bool
    }

    var sequence: UInt32
    var generation: UInt32
    var timestampNs: UInt64
    var samples: [Sample]

    /// The envelope's hard cap (the wire contract's bound).
    static let maxSamples = 32
}

enum MeterValuesDocument {
    static let magic: UInt32 = 0x564D_5341 // "ASMV", little-endian
    static let version: UInt16 = 1
    static let headerBytes = 28
    static let sampleSize = 8

    /// Parse one envelope. Rejects: short/trailing bytes, bad magic,
    /// unknown version, header lies, count above the cap, dirty padding,
    /// unknown flag bits - every path the C++ parse rejects.
    static func parse(_ data: Data) throws -> MeterValuesBatch {
        guard data.count >= headerBytes else {
            throw NSError(domain: "MeterValues", code: Int(kIOReturnBadArgument),
                          userInfo: [NSLocalizedDescriptionKey: "short envelope"])
        }
        func u16(_ at: Int) -> UInt16 {
            UInt16(data[at]) | UInt16(data[at + 1]) << 8
        }
        func u32(_ at: Int) -> UInt32 {
            UInt32(u16(at)) | UInt32(u16(at + 2)) << 16
        }
        func u64(_ at: Int) -> UInt64 {
            UInt64(u32(at)) | UInt64(u32(at + 4)) << 32
        }
        guard u32(0) == magic, u16(4) == version, u16(6) == UInt16(headerBytes),
              u16(26) == 0 else {
            throw NSError(domain: "MeterValues", code: Int(kIOReturnBadArgument),
                          userInfo: [NSLocalizedDescriptionKey: "bad header"])
        }
        let count = Int(u16(24))
        guard count <= MeterValuesBatch.maxSamples,
              data.count == headerBytes + count * sampleSize else {
            throw NSError(domain: "MeterValues", code: Int(kIOReturnBadArgument),
                          userInfo: [NSLocalizedDescriptionKey: "count/length lie"])
        }
        var samples: [MeterValuesBatch.Sample] = []
        samples.reserveCapacity(count)
        for i in 0..<count {
            let at = headerBytes + i * sampleSize
            let flags = data[at + 4]
            guard flags & ~0x01 == 0, data[at + 5] == 0, data[at + 6] == 0,
                  data[at + 7] == 0 else {
                throw NSError(domain: "MeterValues", code: Int(kIOReturnBadArgument),
                              userInfo: [NSLocalizedDescriptionKey: "dirty sample"])
            }
            let raw = Int32(bitPattern: u32(at))
            samples.append(.init(raw: raw, valid: flags & 0x01 != 0))
        }
        return MeterValuesBatch(sequence: u32(8), generation: u32(12),
                                timestampNs: u64(16), samples: samples)
    }
}
