import AppKit
import Foundation

if CommandLine.arguments.count == 4, CommandLine.arguments[1] == "--prepare" {
    do {
        let data = try AvatarProcessor().package(from: URL(fileURLWithPath: CommandLine.arguments[2]))
        try data.write(to: URL(fileURLWithPath: CommandLine.arguments[3]), options: .atomic)
    } catch { fputs(error.localizedDescription + "\n", stderr); exit(1) }
} else if CommandLine.arguments.count == 4, CommandLine.arguments[1] == "--upload",
          let slot = Int(CommandLine.arguments[3]), (1...8).contains(slot) {
    do {
        let data = try AvatarProcessor().package(from: URL(fileURLWithPath: CommandLine.arguments[2]))
        try PhotoUploader.publish(data,slot: slot-1)
        print("头像已上传，设备联网后同步。")
    } catch { fputs(error.localizedDescription + "\n", stderr); exit(1) }
} else {
    let application = NSApplication.shared
    application.setActivationPolicy(.accessory)
    let uploader = PhotoUploader()
    let menuController = StatusMenuController(uploader: uploader)
    withExtendedLifetime(menuController) { application.run() }
}
