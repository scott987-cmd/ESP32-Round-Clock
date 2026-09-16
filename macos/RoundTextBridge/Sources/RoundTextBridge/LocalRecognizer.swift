import Foundation

enum LocalRecognizerError: Error {
    case runtimeMissing(String)
    case processFailed(Int32, String)
    case emptyResult
}

struct LocalRecognizer: Sendable {
    private let executable: String
    private let model: String

    init(environment: [String: String] = ProcessInfo.processInfo.environment) {
        let root = FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Application Support/RoundClock/Runtime").path
        executable = environment["ROUND_SENSEVOICE_CLI"] ?? "\(root)/bin/llama-funasr-sensevoice"
        model = environment["ROUND_SENSEVOICE_MODEL"] ?? "\(root)/models/sensevoice-small-q8.gguf"
    }

    func recognize(_ audio: DecodedAudio) throws -> String {
        guard FileManager.default.isExecutableFile(atPath: executable) else {
            throw LocalRecognizerError.runtimeMissing(executable)
        }
        guard FileManager.default.isReadableFile(atPath: model) else {
            throw LocalRecognizerError.runtimeMissing(model)
        }

        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("round-voice-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: false)
        defer { try? FileManager.default.removeItem(at: directory) }
        let waveURL = directory.appendingPathComponent("input.wav")
        try waveData(samples: audio.samples).write(to: waveURL, options: .atomic)

        let process = Process()
        process.executableURL = URL(fileURLWithPath: executable)
        process.arguments = ["-m", model, "-a", waveURL.path]
        let standardOutput = Pipe()
        let standardError = Pipe()
        process.standardOutput = standardOutput
        process.standardError = standardError
        try process.run()
        process.waitUntilExit()

        let output = standardOutput.fileHandleForReading.readDataToEndOfFile()
        let errorOutput = standardError.fileHandleForReading.readDataToEndOfFile()
        guard process.terminationStatus == 0 else {
            let detail = String(decoding: errorOutput.suffix(600), as: UTF8.self)
            throw LocalRecognizerError.processFailed(process.terminationStatus, detail)
        }
        let text = String(decoding: output, as: UTF8.self)
            .trimmingCharacters(in: .whitespacesAndNewlines)
        guard !text.isEmpty else { throw LocalRecognizerError.emptyResult }
        return text
    }

    private func waveData(samples: [Int16]) -> Data {
        let payloadBytes = UInt32(samples.count * MemoryLayout<Int16>.size)
        var data = Data(capacity: 44 + Int(payloadBytes))
        data.append(contentsOf: Array("RIFF".utf8))
        append(UInt32(36) + payloadBytes, to: &data)
        data.append(contentsOf: Array("WAVEfmt ".utf8))
        append(UInt32(16), to: &data)
        append(UInt16(1), to: &data)
        append(UInt16(1), to: &data)
        append(UInt32(16_000), to: &data)
        append(UInt32(32_000), to: &data)
        append(UInt16(2), to: &data)
        append(UInt16(16), to: &data)
        data.append(contentsOf: Array("data".utf8))
        append(payloadBytes, to: &data)
        for sample in samples { append(UInt16(bitPattern: sample), to: &data) }
        return data
    }

    private func append<T: FixedWidthInteger>(_ value: T, to data: inout Data) {
        var littleEndian = value.littleEndian
        withUnsafeBytes(of: &littleEndian) { data.append(contentsOf: $0) }
    }
}
