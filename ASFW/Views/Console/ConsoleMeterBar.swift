// SPDX-License-Identifier: Apache-2.0
// P12 C2: the strip meter bar. RAW values only - the P10a contract forbids
// fabricating dBFS/peak/clip from uncalibrated samples; the bar shows the
// magnitude fraction against the envelope's own scale and honours the
// per-sample valid flag ("not measured now" is DATA, never a zero).
import SwiftUI

enum ConsoleMeterMath {
    /// The display fraction for one raw sample. `nil` when not measured.
    /// A negative raw (if the device ever reports one) uses its magnitude.
    static func fraction(_ raw: Int32, valid: Bool) -> Double? {
        guard valid else { return nil }
        let magnitude = abs(Int64(raw))
        return min(1.0, Double(magnitude) / 2_147_483_647.0)
    }
}

/// One strip's meter: a vertical magnitude bar with the raw value in the
/// help tooltip, and a hollow "not measured" state for invalid samples.
struct ConsoleMeterBar: View {
    let sample: MeterValuesBatch.Sample?
    let tint: Color

    var body: some View {
        GeometryReader { geo in
            let fraction = sample.flatMap { ConsoleMeterMath.fraction($0.raw, valid: $0.valid) }
            ZStack(alignment: .bottom) {
                RoundedRectangle(cornerRadius: 2)
                    .fill(.white.opacity(fraction == nil ? 0.04 : 0.06))
                if let fraction {
                    RoundedRectangle(cornerRadius: 2)
                        .fill(fraction > 0.9 ? tint.opacity(0.35) : tint.opacity(0.8))
                        .frame(height: max(2, geo.size.height * fraction))
                } else if sample != nil {
                    // Measured-position sample that is invalid: not measured NOW.
                    RoundedRectangle(cornerRadius: 2)
                        .strokeBorder(.white.opacity(0.10), lineWidth: 1)
                }
            }
        }
        .frame(width: 5)
        .help(sample.map { $0.valid ? "raw \($0.raw)" : "not measured" } ?? "no meter")
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(Text("meter"))
        .accessibilityValue(Text(sample.map { $0.valid ? "raw \($0.raw)" : "not measured" } ?? "no meter"))
    }
}
