import Foundation

enum PhotoPacketError: Error, Equatable {
    case invalidImageSize(Int)
}

struct PhotoPacketEncoder {
    static let width = 280
    static let height = 280
    static let imageBytes = width * height * 2
    static let packetBytes = 180
    static let headerBytes = 16
    static let payloadBytes = packetBytes - headerBytes

    func packets(rgb565: Data, session: UInt16, slot: UInt8 = 0xFF) throws -> [[UInt8]] {
        guard rgb565.count == Self.imageBytes else {
            throw PhotoPacketError.invalidImageSize(rgb565.count)
        }
        var result: [[UInt8]] = []
        result.reserveCapacity((rgb565.count + Self.payloadBytes - 1) / Self.payloadBytes)
        var offset = 0
        var sequence: UInt16 = 0
        while offset < rgb565.count {
            let length = min(Self.payloadBytes, rgb565.count - offset)
            var packet = [UInt8](repeating: 0, count: Self.packetBytes)
            packet[0] = 1
            if offset == 0 { packet[1] |= 0x01 }
            if offset + length == rgb565.count { packet[1] |= 0x02 }
            write(session, to: &packet, at: 2)
            write(sequence, to: &packet, at: 4)
            write(UInt32(rgb565.count), to: &packet, at: 6)
            write(UInt16(Self.width), to: &packet, at: 10)
            write(UInt16(Self.height), to: &packet, at: 12)
            packet[14] = slot
            packet[15] = UInt8(length)
            rgb565.copyBytes(to: &packet, from: offset..<(offset + length), at: Self.headerBytes)
            result.append(packet)
            offset += length
            sequence &+= 1
        }
        return result
    }

    private func write<T: FixedWidthInteger>(_ value: T, to bytes: inout [UInt8], at offset: Int) {
        var value = value.littleEndian
        withUnsafeBytes(of: &value) { source in
            bytes.replaceSubrange(offset..<(offset + source.count), with: source)
        }
    }
}

private extension Data {
    func copyBytes(to destination: inout [UInt8], from range: Range<Int>, at offset: Int) {
        withUnsafeBytes { source in
            guard let base = source.baseAddress else { return }
            destination.withUnsafeMutableBytes { target in
                target.baseAddress?.advanced(by: offset).copyMemory(
                    from: base.advanced(by: range.lowerBound), byteCount: range.count)
            }
        }
    }
}
