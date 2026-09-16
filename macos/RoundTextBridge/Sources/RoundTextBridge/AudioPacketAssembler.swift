import Foundation

struct DecodedAudio: Sendable, Equatable {
    let session: UInt16
    let samples: [Int16]
}

enum AudioPacketError: Error, Equatable {
    case invalidLength(Int)
    case unsupportedVersion(UInt8)
    case invalidPayloadLength(UInt8)
    case invalidStepIndex(UInt8)
    case invalidTotalSamples(Int)
    case missingStart
    case unexpectedSequence(expected: UInt16, received: UInt16)
    case inconsistentSession
    case inconsistentTotal
    case inconsistentSampleCount
    case incompleteAudio(expected: Int, received: Int)
}

struct AudioPacketAssembler {
    private static let indexTable = [
        -1, -1, -1, -1, 2, 4, 6, 8,
        -1, -1, -1, -1, 2, 4, 6, 8,
    ]
    private static let stepTable = [
        7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
        34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
        157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544,
        598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878,
        2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
        6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818,
        18500, 20350, 22385, 24623, 27086, 29794, 32767,
    ]

    private var session: UInt16?
    private var expectedSequence: UInt16?
    private var expectedTotal = 0
    private var samples: [Int16] = []
    private var predictor = 0
    private var stepIndex = 0

    mutating func consume(_ rawReport: [UInt8]) throws -> DecodedAudio? {
        let report: [UInt8]
        if rawReport.count == 181 && rawReport.first == 2 {
            report = Array(rawReport.dropFirst())
        } else if rawReport.count == 180 {
            report = rawReport
        } else {
            throw AudioPacketError.invalidLength(rawReport.count)
        }
        guard report[0] == 1 else { throw AudioPacketError.unsupportedVersion(report[0]) }

        let flags = report[1]
        let packetSession = UInt16(report[2]) | (UInt16(report[3]) << 8)
        let sequence = UInt16(report[4]) | (UInt16(report[5]) << 8)
        let total = Int(report[6]) | (Int(report[7]) << 8) |
            (Int(report[8]) << 16) | (Int(report[9]) << 24)
        let packetSamples = Int(report[10]) | (Int(report[11]) << 8)
        let payloadLength = Int(report[15])
        guard payloadLength <= 164 else {
            throw AudioPacketError.invalidPayloadLength(report[15])
        }
        guard total > 0 && total <= 16_000 * 20 else {
            reset()
            throw AudioPacketError.invalidTotalSamples(total)
        }

        let isStart = (flags & 0x01) != 0
        if isStart {
            let initial = UInt16(report[12]) | (UInt16(report[13]) << 8)
            let initialIndex = report[14]
            guard initialIndex <= 88 else { throw AudioPacketError.invalidStepIndex(initialIndex) }
            session = packetSession
            expectedSequence = 0
            expectedTotal = total
            samples.removeAll(keepingCapacity: true)
            samples.reserveCapacity(total)
            predictor = Int(Int16(bitPattern: initial))
            stepIndex = Int(initialIndex)
            samples.append(Int16(predictor))
        }

        guard let currentSession = session, let wantedSequence = expectedSequence else {
            throw AudioPacketError.missingStart
        }
        guard packetSession == currentSession else {
            reset()
            throw AudioPacketError.inconsistentSession
        }
        guard sequence == wantedSequence else {
            reset()
            throw AudioPacketError.unexpectedSequence(expected: wantedSequence, received: sequence)
        }
        guard total == expectedTotal else {
            reset()
            throw AudioPacketError.inconsistentTotal
        }

        let encodedSamples = packetSamples - (isStart ? 1 : 0)
        guard encodedSamples >= 0 && payloadLength == (encodedSamples + 1) / 2 else {
            reset()
            throw AudioPacketError.inconsistentSampleCount
        }
        for index in 0..<encodedSamples {
            let packed = report[16 + index / 2]
            let code = Int((index & 1) == 0 ? packed & 0x0F : packed >> 4)
            samples.append(decode(code))
        }
        expectedSequence = sequence &+ 1

        guard (flags & 0x02) != 0 else { return nil }
        guard samples.count == expectedTotal else {
            let received = samples.count
            reset()
            throw AudioPacketError.incompleteAudio(expected: total, received: received)
        }
        let result = DecodedAudio(session: currentSession, samples: samples)
        reset()
        return result
    }

    private mutating func decode(_ code: Int) -> Int16 {
        let step = Self.stepTable[stepIndex]
        var delta = step >> 3
        if (code & 4) != 0 { delta += step }
        if (code & 2) != 0 { delta += step >> 1 }
        if (code & 1) != 0 { delta += step >> 2 }
        predictor += (code & 8) != 0 ? -delta : delta
        predictor = min(Int(Int16.max), max(Int(Int16.min), predictor))
        stepIndex += Self.indexTable[code]
        stepIndex = min(88, max(0, stepIndex))
        return Int16(predictor)
    }

    private mutating func reset() {
        session = nil
        expectedSequence = nil
        expectedTotal = 0
        samples.removeAll(keepingCapacity: true)
        predictor = 0
        stepIndex = 0
    }
}
