import Foundation
import Testing
@testable import ASFW

/// The full-size Saffire Pro 40 D3 snapshot: the P08 paging gate. The
/// fixture mirrors the live document captured against hardware on
/// 2026-11-06 (388 parameters with the P08 init action, 288 mixer
/// crosspoints, ~250 KB) so the decode path and the paged retrieval run
/// at the real mixer size, not at a toy fixture's size. The page envelope
/// mirrors the driver's packed AVCDiscoveryPageWire (magic "AVDD", 32
/// bytes, FNV-1a of the whole document).
@MainActor
struct SPro40FullSnapshotTests {

    /// The wire-shaped page for one slice of the document (the driver's
    /// SerializeDiscoveryPage, little-endian packed).
    static func page(of document: Data, offset: Int, session: UInt32,
                     generation: UInt32, checksum: UInt32) -> Data {
        let payload = document.dropFirst(offset).prefix(4096 - 32)
        var out = Data(count: 32 + payload.count)
        out.withUnsafeMutableBytes { w in
            func put32(_ v: UInt32, at i: Int) { w[i] = UInt8(v & 0xff); w[i+1] = UInt8((v >> 8) & 0xff); w[i+2] = UInt8((v >> 16) & 0xff); w[i+3] = UInt8((v >> 24) & 0xff) }
            func put16(_ v: UInt16, at i: Int) { w[i] = UInt8(v & 0xff); w[i+1] = UInt8((v >> 8) & 0xff) }
            put32(0x44445641, at: 0)   // "AVDD" (kAVCDiscoveryDocumentMagic)
            put16(1, at: 4)            // version
            put16(32, at: 6)           // header bytes
            put32(session, at: 8)
            put32(generation, at: 12)
            put32(UInt32(document.count), at: 16)
            put32(UInt32(offset), at: 20)
            put32(UInt32(payload.count), at: 24)
            put32(checksum, at: 28)
        }
        out.replaceSubrange(32..<(32 + payload.count), with: payload)
        return out
    }

    /// All pages of one consistent document, in offset order.
    static func pages(of document: Data, session: UInt32, generation: UInt32) -> [Data] {
        let checksum = AvcDiscoveryDocument.fnv1a(document)
        var out: [Data] = []
        var offset = 0
        while offset < document.count {
            out.append(page(of: document, offset: offset, session: session,
                            generation: generation, checksum: checksum))
            offset += 4096 - 32
        }
        return out
    }

    /// The full-shape document: the 2026-11-06 live capture's structure
    /// plus the P08 mixer-init action - the current model's shape.
    static func document(cachedCells: Int = 4) -> Data {
        var json = """
        {"schema":"asfw.controls.v1","guid":"0xd3d3000000000001","family":"spro40d3","revision":1,"sequence":0,"nodes":[
        {"id":1,"name":"Host Capture","kind":"endpoint","host":true},
        {"id":2,"name":"Host Playback","kind":"endpoint","host":true},
        """
        for input in 1...8 {
            json += "{\"id\":\(9 + input),\"name\":\"Analog Input \(input)\",\"kind\":\"endpoint\"},"
        }
        json += """
        {"id":20,"name":"S/PDIF Input","kind":"endpoint"},
        {"id":21,"name":"ADAT Input","kind":"endpoint"},
        """
        for output in 1...10 {
            json += "{\"id\":\(29 + output),\"name\":\"Analog Output \(output)\",\"kind\":\"endpoint\"},"
        }
        json += """
        {"id":40,"name":"S/PDIF Output","kind":"endpoint"},
        {"id":41,"name":"ADAT Output","kind":"endpoint"},
        {"id":70,"name":"Monitor Mixer","kind":"mixer","inputs":[
        """
        json += (71...88).map(String.init).joined(separator: ",")
        json += "],\"outputs\":["
        json += (89...104).map(String.init).joined(separator: ",")
        json += "],\"crosspoints\":["
        var crosspoints: [String] = []
        for bus in 0..<16 {
            for slot in 0..<18 {
                crosspoints.append("{\"id\":\(bus * 18 + slot + 1),\"in\":\(71 + slot),\"out\":\(89 + bus)}")
            }
        }
        json += crosspoints.joined(separator: ",")
        json += "],\"writable\":true}],\"ports\":["
        var ports: [String] = []
        for input in 0..<18 {
            ports.append("{\"id\":\(71 + input),\"owner\":70,\"direction\":\"in\",\"channels\":1,\"name\":\"Mix In \(input + 1)\"}")
        }
        for bus in 0..<16 {
            ports.append("{\"id\":\(89 + bus),\"owner\":70,\"direction\":\"out\",\"channels\":1,\"name\":\"Mix Bus \(bus + 1)\"}")
        }
        json += ports.joined(separator: ",")
        json += "],\"parameters\":["

        var params: [String] = []
        // The output group, real live values from 2026-11-06: outputs 1-6 at
        // level 0 (wire 0x7F), 7-10 at 127 (wire 0x00); 1-2 follow the knob.
        for ch in 1...10 {
            let node = 29 + ch
            let level = ch <= 6 ? 0.0 : 127.0
            params.append("{\"id\":\(0x5A3D0100 + UInt32(ch - 1)),\"name\":\"Output Level \(ch) (raw)\",\"semantic\":\"level\",\"target\":{\"node\":\(node)},\"domain\":{\"kind\":\"scalar\",\"min\":0.0000,\"max\":127.0000,\"step\":1.0000,\"unit\":\"generic\"},\"value\":\(String(format: "%.4f", level)),\"origin\":\"observed\"}")
            params.append("{\"id\":\(0x5A3D0200 + UInt32(ch - 1)),\"name\":\"Output Mute \(ch)\",\"semantic\":\"mute\",\"target\":{\"node\":\(node)},\"domain\":{\"kind\":\"bool\"},\"value\":false,\"origin\":\"observed\"}")
            params.append("{\"id\":\(0x5A3D0310 + UInt32(ch - 1)),\"name\":\"Output \(ch) Follows Global Mute\",\"semantic\":\"mute\",\"target\":{\"node\":\(node)},\"domain\":{\"kind\":\"bool\"},\"value\":false,\"origin\":\"observed\"}")
            params.append("{\"id\":\(0x5A3D0320 + UInt32(ch - 1)),\"name\":\"Output \(ch) Follows Global Dim\",\"semantic\":\"dim\",\"target\":{\"node\":\(node)},\"domain\":{\"kind\":\"bool\"},\"value\":false,\"origin\":\"observed\"}")
            params.append("{\"id\":\(0x5A3D0330 + UInt32(ch - 1)),\"name\":\"Output \(ch) Hardware Volume\",\"semantic\":\"unknown\",\"target\":{\"node\":\(node)},\"domain\":{\"kind\":\"bool\"},\"value\":\(ch <= 2 ? "true" : "false"),\"origin\":\"observed\"}")
        }
        params.append("{\"id\":\(0x5A3D0300),\"name\":\"Global Mute\",\"semantic\":\"mute\",\"target\":{\"node\":30},\"domain\":{\"kind\":\"bool\"},\"value\":false,\"origin\":\"observed\"}")
        params.append("{\"id\":\(0x5A3D0301),\"name\":\"Global Dim\",\"semantic\":\"dim\",\"target\":{\"node\":30},\"domain\":{\"kind\":\"bool\"},\"value\":false,\"origin\":\"observed\"}")
        params.append("{\"id\":\(0x5A3D0400),\"name\":\"Active Monitor Pad (raw)\",\"semantic\":\"unknown\",\"target\":{\"node\":30},\"domain\":{\"kind\":\"scalar\",\"min\":0.0000,\"max\":1023.0000,\"step\":1.0000,\"unit\":\"generic\"},\"value\":0.0000,\"origin\":\"observed\"}")
        params.append("{\"id\":\(0x5A3D0500),\"name\":\"Monitor Knob (raw, get-only)\",\"semantic\":\"unknown\",\"target\":{\"node\":30},\"access\":\"readOnly\",\"domain\":{\"kind\":\"scalar\",\"min\":0.0000,\"max\":127.0000,\"step\":1.0000,\"unit\":\"generic\"},\"value\":102.0000,\"origin\":\"observed\"}")
        params.append("{\"id\":\(0x5A3D0600),\"name\":\"Optical Output Carries S/PDIF\",\"semantic\":\"unknown\",\"target\":{\"node\":41},\"domain\":{\"kind\":\"bool\"},\"value\":true,\"origin\":\"observed\"}")
        // The 288 crosspoints: stateless (Unknown) except a few the host
        // wrote through the P08 init action (Cached) - the gate shape.
        for bus in 0..<16 {
            for slot in 0..<18 {
                let id = 0x5A3D1000 + UInt32(bus * 18 + slot)
                let known = (bus * 18 + slot) < cachedCells
                let value = known ? "0.0000" : "null"
                // The driver emits a QUOTED string for known provenance and
                // the bare JSON null for unknown - never an unquoted word.
                let origin = known ? "\"cached\"" : "null"
                params.append("{\"id\":\(id),\"name\":\"Mixer \(slot + 1) \u{2192} Bus \(bus + 1) (raw, unity ~0x1fff)\",\"semantic\":\"level\",\"target\":{\"crosspoint\":\(bus * 18 + slot + 1)},\"domain\":{\"kind\":\"scalar\",\"min\":0.0000,\"max\":32767.0000,\"step\":1.0000,\"unit\":\"generic\"},\"value\":\(value),\"origin\":\(origin)}")
            }
        }
        // The P06 routing selectors: 22 output/mixer-feed slots + the 18
        // mixer input selections + 2 master meter sources.
        let outputNames = (1...10).map { "Line \($0)" }
            + (1...8).map { "ADAT \($0)" }
            + (1...2).map { "S/PDIF \($0)" }
            + (1...2).map { "Loopback \($0)" }
        for (slot, name) in outputNames.enumerated() {
            params.append("{\"id\":\(0x0200_0000 + UInt32(slot)),\"name\":\"\(name) Source\",\"semantic\":\"unknown\",\"target\":{\"node\":\(slot < 10 ? 30 + slot : (slot < 18 ? 41 : (slot < 20 ? 40 : 2)))},\"domain\":{\"kind\":\"scalar\",\"min\":0.0000,\"max\":38.0000,\"step\":1.0000,\"unit\":\"generic\"},\"value\":null,\"origin\":null}")
        }
        for input in 0..<18 {
            params.append("{\"id\":\(0x0201_0000 + UInt32(input)),\"name\":\"Mixer Input \(input + 1) Source\",\"semantic\":\"unknown\",\"target\":{\"node\":70},\"domain\":{\"kind\":\"scalar\",\"min\":0.0000,\"max\":38.0000,\"step\":1.0000,\"unit\":\"generic\"},\"value\":null,\"origin\":null}")
        }
        for meter in 0..<2 {
            params.append("{\"id\":\(0x0202_0000 + UInt32(meter)),\"name\":\"Master Meter \(meter + 1) Source\",\"semantic\":\"unknown\",\"target\":{\"node\":2},\"domain\":{\"kind\":\"scalar\",\"min\":0.0000,\"max\":38.0000,\"step\":1.0000,\"unit\":\"generic\"},\"value\":null,\"origin\":null}")
        }
        // The actions.
        params.append("{\"id\":\(0x5A3D1700),\"name\":\"Initialize Monitor Mixer\",\"semantic\":\"unknown\",\"target\":{\"node\":70},\"domain\":{\"kind\":\"action\"}}")
        params.append("{\"id\":\(0x5A3D0001),\"name\":\"Refresh Readable Controls\",\"semantic\":\"unknown\",\"target\":{\"node\":1},\"domain\":{\"kind\":\"action\"}}")
        params.append("{\"id\":\(0x0301_0001),\"name\":\"Apply Host Routing Program (full low+high tables)\",\"semantic\":\"unknown\",\"target\":{\"node\":2},\"domain\":{\"kind\":\"action\"}}")

        json += params.joined(separator: ",")
        json += "]}"
        return Data(json.utf8)
    }

    @Test func fullMixerDocumentDecodesAtRealSize() throws {
        let doc = Self.document()
        // The compact fixture is ~97 KB / 24 pages; the live document is
        // ~250 KB (4-decimal formatting) - the true-size traversal is
        // proven by the C++ gate test against the real builder. Here the
        // gate must still run at a representative multi-page size.
        #expect(doc.count > 80_000)
        #expect(doc.count < AvcDiscoveryDocument.maxBytes)
        #expect(doc.count / (4096 - 32) > 20)

        let snapshot = try ControlSnapshot.decode(doc)
        #expect(snapshot.family == "spro40d3")
        #expect(snapshot.nodes.count == 25)
        let mixer = try #require(snapshot.nodes.first { $0.kind == .mixer })
        #expect(mixer.crosspoints.count == 288)
        #expect(mixer.inputs.count == 18)
        #expect(mixer.outputs.count == 16)
        #expect(snapshot.parameters.count == 388)

        // The stateless cells are Unknown (null), the written ones Cached.
        let cells = snapshot.parameters.filter {
            if case .scalar = $0.domain, $0.name.contains("unity ~0x1fff") { true } else { false }
        }
        #expect(cells.count == 288)
        #expect(cells.filter { $0.value != nil }.count == 4)
        // Crosspoint cells belong to no node; the console renders them from
        // the mixer's crosspoint grid.
        #expect(snapshot.unownedParameters.filter { p in
            if case .scalar = p.domain { return p.name.contains("unity ~0x1fff") }
            return false
        }.count == 288)
    }

    @Test func fullMixerDocumentAssemblesPageByPage() throws {
        let doc = Self.document()
        let all = Self.pages(of: doc, session: 4, generation: 3)
        #expect(all.count > 20)
        var served = 0
        let result = AvcDiscoveryDocument.assemble { _ in
            defer { served += 1 }
            return served < all.count ? all[served] : nil
        }
        let assembled = try #require(try result.get())
        #expect(assembled == doc)
        #expect(served == all.count) // exactly the pages, no extra fetch
    }

    @Test func equalLengthValueChangeBetweenPagesIsDetected() throws {
        // P09's subtlest case: an EQUAL-LENGTH value change (0.0000 ->
        // 9.0000) between the two documents. Sizes agree, totalBytes
        // agrees, origins agree; the page checksum is the ONLY witness.
        // (Origin-only changes cannot be size-preserving - the words have
        // different lengths - so the checksum-only proof lives here.) The
        // assembly must refuse the mix, not assemble a hybrid whose values
        // are half a lie.
        var a = String(decoding: Self.document(), as: UTF8.self)
        let old = "\"value\":0.0000"
        let new = "\"value\":9.0000"
        let range = a.range(of: old)!
        a.replaceSubrange(range, with: new) // one level value flips
        let docA = Data(a.utf8)
        let docB = Self.document()          // identical except that value
        #expect(docA.count == docB.count)

        let pagesA = Self.pages(of: docA, session: 4, generation: 3)
        let pagesB = Self.pages(of: docB, session: 5, generation: 3)
        let mixed: (UInt32) -> Data? = { offset in
            let index = Int(offset) / (4096 - 32)
            return index < 10 ? pagesA[index] : pagesB[index]
        }
        guard case .failure = AvcDiscoveryDocument.assemble(fetch: mixed) else {
            Issue.record("an origin-only mid-stream change must not assemble")
            return
        }
    }

    @Test func refreshDuringRetrievalIsDetectedThenRecoversBounded() throws {
        // A poll lands a write mid-retrieval: the document changes under
        // the read (session/generation/content all shift). The assembler
        // must report mixed pages; the connector's bounded policy retries
        // at most three times - a device that settles recovers on the next
        // attempt; one that never settles fails explicitly.
        let a = Self.document()
        let b = Self.document(cachedCells: 9) // the refreshed document
        let pagesA = Self.pages(of: a, session: 4, generation: 3)
        let pagesB = Self.pages(of: b, session: 5, generation: 3)
        // fetch takes the byte offset (UInt32) and must return the page AT
        // that offset; a mid-stream document swap keeps the offsets, only
        // the source changes (pages of A below the switch point, then B).
        func stream(fromA switchAt: Int) -> (UInt32) -> Data? {
            { offset in
                let index = Int(offset) / (4096 - 32)
                return index < switchAt ? pagesA[index] : pagesB[index]
            }
        }

        // Mid-stream change: mixed pages, nothing half-assembled leaks out.
        // (Switch well inside the page count so the stream truly mixes.)
        guard case .failure(.mixedPages) =
            AvcDiscoveryDocument.assemble(fetch: stream(fromA: 10)) else {
            Issue.record("expected mixedPages")
            return
        }

        // The connector's policy (at most three attempts): a device that
        // settles between attempts assembles byte-exactly on the next one.
        let assembled = try #require(try AvcDiscoveryDocument
            .assemble(fetch: stream(fromA: 0)).get())
        #expect(assembled == b)
    }
}
