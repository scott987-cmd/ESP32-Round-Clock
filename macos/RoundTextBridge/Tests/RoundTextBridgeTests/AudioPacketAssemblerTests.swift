import Testing
@testable import RoundTextBridge

private func packet(
    session: UInt16 = 7,
    sequence: UInt16,
    flags: UInt8,
    total: Int,
    packetSamples: Int,
    initial: Int16 = 1_000,
    stepIndex: UInt8 = 0,
    payload: [UInt8]
) -> [UInt8] {
    var report = [UInt8](repeating: 0, count: 180)
    report[0] = 1
    report[1] = flags
    report[2] = UInt8(session & 0xFF)
    report[3] = UInt8(session >> 8)
    report[4] = UInt8(sequence & 0xFF)
    report[5] = UInt8(sequence >> 8)
    report[6] = UInt8(total & 0xFF)
    report[7] = UInt8((total >> 8) & 0xFF)
    report[8] = UInt8((total >> 16) & 0xFF)
    report[9] = UInt8((total >> 24) & 0xFF)
    report[10] = UInt8(packetSamples & 0xFF)
    report[11] = UInt8((packetSamples >> 8) & 0xFF)
    let initialBits = UInt16(bitPattern: initial)
    report[12] = UInt8(initialBits & 0xFF)
    report[13] = UInt8(initialBits >> 8)
    report[14] = stepIndex
    report[15] = UInt8(payload.count)
    report.replaceSubrange(16..<(16 + payload.count), with: payload)
    return report
}

@Test func decodesIMAADPCMAudio() throws {
    var assembler = AudioPacketAssembler()
    let result = try assembler.consume(packet(
        sequence: 0, flags: 3, total: 3, packetSamples: 3, payload: [0x10]
    ))
    #expect(result == DecodedAudio(session: 7, samples: [1_000, 1_000, 1_001]))
}

@Test func rejectsMissingAudioPacket() throws {
    var assembler = AudioPacketAssembler()
    _ = try assembler.consume(packet(
        sequence: 0, flags: 1, total: 4, packetSamples: 2, payload: [0x00]
    ))
    #expect(throws: AudioPacketError.self) {
        _ = try assembler.consume(packet(
            sequence: 2, flags: 2, total: 4, packetSamples: 2, payload: [0x00]
        ))
    }
}
