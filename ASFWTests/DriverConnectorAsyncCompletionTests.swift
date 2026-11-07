import Testing
@testable import ASFW
import Darwin.Mach
import IOKit

/// The async completion parser reads IOKit's OSMessageNotification.h layout:
/// Mach header, OSNotificationHeader64 { content size, type 150, 64-byte
/// OSAsyncReference64 }, then IOAsyncCompletionContent { IOReturn result,
/// then the scalars }. The count follows from the content size. Hardware
/// evidence for the layout (upstream PR #233): size 120, size=0x14,
/// type=0x96, sequence at byte 100, reason at byte 108.
struct DriverConnectorAsyncCompletionTests {

    private func makeCompletionMessage(
        type: UInt32 = UInt32(kIOAsyncCompletionNotificationType),
        contentSize: UInt32 = 20,
        result: Int32 = kIOReturnSuccess,
        scalars: [UInt64] = [7, 0x4348_414E]  // sequence, 'CHAN'-ish reason
    ) -> [UInt8] {
        var bytes = [UInt8](repeating: 0, count: 512)
        let headerSize = MemoryLayout<mach_msg_header_t>.size
        let contentOffset = headerSize + 2 * MemoryLayout<UInt32>.size
            + MemoryLayout<OSAsyncReference64>.size
        let scalarsOffset = contentOffset + MemoryLayout<IOReturn>.size
        // mach_msg_header_t: size covers header + content.
        bytes.withUnsafeMutableBytes { raw in
            let base = raw.baseAddress!
            let messageBytes = scalarsOffset + scalars.count * 8
            base.storeBytes(of: UInt32(messageBytes), toByteOffset: 4, as: UInt32.self)
            // OSNotificationHeader64
            base.storeBytes(of: contentSize, toByteOffset: headerSize, as: UInt32.self)
            base.storeBytes(of: type, toByteOffset: headerSize + 4, as: UInt32.self)
            // IOAsyncCompletionContent: result, then packed scalars.
            base.storeBytes(of: result, toByteOffset: contentOffset, as: Int32.self)
            for (index, scalar) in scalars.enumerated() {
                base.storeBytes(of: scalar, toByteOffset: scalarsOffset + index * 8,
                                as: UInt64.self)
            }
        }
        return bytes
    }

    @Test func parsesTheHardwareObservedLayout() {
        // Exactly the frame the device delivers for a control change: the
        // sequence lands at byte 100, the reason at byte 108.
        let bytes = makeCompletionMessage()
        let parsed = bytes.withUnsafeBytes { raw in
            ASFWDriverConnector.asyncCompletionScalars(base: raw.baseAddress!,
                                                        capacity: raw.count)
        }
        #expect(parsed != nil)
        #expect(parsed!.count == 2)
        #expect(parsed![0] == 7)
        #expect(parsed![1] == 0x4348_414E)
    }

    @Test func derivesCountFromContentSizeNotASentField() {
        // Four scalars: content size = 4 (result) + 4 * 8.
        let bytes = makeCompletionMessage(
            contentSize: 4 + 4 * 8,
            scalars: [1, 2, 3, 4])
        let parsed = bytes.withUnsafeBytes { raw in
            ASFWDriverConnector.asyncCompletionScalars(base: raw.baseAddress!,
                                                        capacity: raw.count)
        }
        #expect(parsed != nil)
        #expect(parsed! == [1, 2, 3, 4])
    }

    @Test func refusesWrongNotificationType() {
        let bytes = makeCompletionMessage(type: 0x99)
        let parsed = bytes.withUnsafeBytes { raw in
            ASFWDriverConnector.asyncCompletionScalars(base: raw.baseAddress!,
                                                        capacity: raw.count)
        }
        #expect(parsed == nil)
    }

    @Test func refusesTruncatedAndUnsizedContent() {
        // Content smaller than the IOReturn, or fewer than two scalars:
        // never guessed at.
        let tooSmall = makeCompletionMessage(contentSize: 4)
        let parsed = tooSmall.withUnsafeBytes { raw in
            ASFWDriverConnector.asyncCompletionScalars(base: raw.baseAddress!,
                                                        capacity: raw.count)
        }
        #expect(parsed == nil)

        let truncated = makeCompletionMessage()
        let parsedTruncated = truncated.withUnsafeBytes { raw in
            ASFWDriverConnector.asyncCompletionScalars(base: raw.baseAddress!,
                                                        capacity: 100)
        }
        #expect(parsedTruncated == nil)
    }
}
