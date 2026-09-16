import Foundation
import CryptoKit

final class PhotoUploader: @unchecked Sendable {
    typealias StatusHandler = @Sendable (String) -> Void
    private let queue = DispatchQueue(label: "com.roundclock.avatar.upload")
    var statusHandler: StatusHandler?
    func upload(_ urls: [URL], startingSlot: Int) {
        guard !urls.isEmpty, startingSlot >= 0, startingSlot + urls.count <= 8 else { return }
        queue.async { [self] in
            for (index,url) in urls.enumerated() {
                do {
                    statusHandler?("正在识别人脸 \(index+1)/\(urls.count)…")
                    let data = try AvatarProcessor().package(from: url)
                    try Self.publish(data, slot: startingSlot+index)
                    statusHandler?("头像 \(startingSlot+index+1) 已上传，等待圆屏联网自动同步")
                } catch { statusHandler?(error.localizedDescription); return }
            }
        }
    }
    static func publish(_ data: Data, slot: Int) throws {
        let folder = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Application Support/RoundClock")
        let baseFile = folder.appendingPathComponent("api-base")
        let authorizationFile = folder.appendingPathComponent("authorization-header")
        guard let base = try? String(contentsOf: baseFile, encoding: .utf8).trimmingCharacters(in: .whitespacesAndNewlines),
              let endpoint = URL(string: base + "/v1/avatars"), endpoint.scheme == "https",
              FileManager.default.isReadableFile(atPath: authorizationFile.path)
        else { throw AvatarError.configuration }
        let temp = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString + ".rav")
        try data.write(to: temp, options: .atomic)
        try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: temp.path)
        defer { try? FileManager.default.removeItem(at: temp) }
        let process = Process(), out = Pipe(), err = Pipe()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/curl")
        process.arguments = ["--silent", "--show-error", "--fail", "--max-time", "30", "--proto", "=https",
            "--cacert",folder.appendingPathComponent("device-ca.pem").path,
            "--cert",folder.appendingPathComponent("device-client.pem").path,
            "--key",folder.appendingPathComponent("device-client.key").path,
            "--header","@"+authorizationFile.path,
            "-H","Content-Type: application/octet-stream","-H","X-Avatar-Slot: \(slot)",
            "--data-binary","@"+temp.path,endpoint.absoluteString]
        process.standardOutput = out; process.standardError = err
        try process.run()
        let response = out.fileHandleForReading.readDataToEndOfFile()
        process.waitUntilExit()
        let hash = SHA256.hash(data: data).map { String(format: "%02x",$0) }.joined()
        guard process.terminationStatus == 0,
              let object = try JSONSerialization.jsonObject(with: response) as? [String:Any],
              object["stored"] as? Bool == true, object["slot"] as? Int == slot,
              object["sha256"] as? String == hash else { throw AvatarError.upload }
    }
}
