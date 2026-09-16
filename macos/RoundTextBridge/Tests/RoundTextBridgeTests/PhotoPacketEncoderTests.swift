import Foundation
import CoreGraphics
import ImageIO
import Testing
@testable import RoundTextBridge

@Test func photoPacketsRoundTrip() throws {
    let source = Data((0..<PhotoPacketEncoder.imageBytes).map { UInt8($0 & 0xFF) })
    let packets = try PhotoPacketEncoder().packets(rgb565: source, session: 0x1234)
    #expect(packets.count == 957)
    #expect(packets.first?[1] == 0x01)
    #expect(packets.last?[1] == 0x02)

    var rebuilt = Data()
    for (index, packet) in packets.enumerated() {
        #expect(packet.count == PhotoPacketEncoder.packetBytes)
        let sequence = UInt16(packet[4]) | (UInt16(packet[5]) << 8)
        #expect(sequence == UInt16(index))
        #expect(packet[2] == 0x34)
        #expect(packet[3] == 0x12)
        #expect(packet[14] == 0xFF)
        let length = Int(packet[15])
        rebuilt.append(contentsOf: packet[16..<(16 + length)])
    }
    #expect(rebuilt == source)
}

@Test func rejectsWrongPhotoSize() {
    #expect(throws: PhotoPacketError.self) {
        _ = try PhotoPacketEncoder().packets(rgb565: Data(count: 12), session: 1)
    }
}

@Test func convertsRGBAtoLittleEndianRGB565() {
    let result = RGB565Converter.convert(
        rgba: [255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255],
        width: 3, height: 1)
    #expect(Array(result) == [0x00, 0xF8, 0xE0, 0x07, 0x1F, 0x00])
}

@Test func rendersPortraitCardWithoutDetectedFace() throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("round-photo-test-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: false)
    defer { try? FileManager.default.removeItem(at: directory) }
    let url = directory.appendingPathComponent("portrait.png")
    let colorSpace = CGColorSpaceCreateDeviceRGB()
    let context = try #require(CGContext(
        data: nil, width: 120, height: 200, bitsPerComponent: 8, bytesPerRow: 0,
        space: colorSpace, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue))
    context.setFillColor(red: 0.8, green: 0.3, blue: 0.2, alpha: 1)
    context.fill(CGRect(x: 0, y: 0, width: 120, height: 200))
    let image = try #require(context.makeImage())
    let destination = try #require(CGImageDestinationCreateWithURL(
        url as CFURL, "public.png" as CFString, 1, nil))
    CGImageDestinationAddImage(destination, image, nil)
    #expect(CGImageDestinationFinalize(destination))

    let result = try PhotoProcessor().makeAvatar(from: url)
    #expect(result.count == PhotoPacketEncoder.imageBytes)
}
