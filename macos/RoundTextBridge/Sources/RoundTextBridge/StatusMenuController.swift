import AppKit
import UniformTypeIdentifiers

@MainActor final class StatusMenuController: NSObject {
    private let statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
    private let statusLine = NSMenuItem(title: "联网头像助手 · 不使用蓝牙", action: nil, keyEquivalent: "")
    private let uploader: PhotoUploader

    init(uploader: PhotoUploader) {
        self.uploader = uploader
        super.init()
        if let button = statusItem.button {
            button.image = NSImage(
                systemSymbolName: "person.crop.circle.badge.plus",
                accessibilityDescription: "圆屏助手")
            button.toolTip = "圆屏助手"
        }
        let menu = NSMenu()
        let title = NSMenuItem(title: "圆屏本地助手", action: nil, keyEquivalent: "")
        title.isEnabled = false
        menu.addItem(title)
        statusLine.isEnabled = false
        menu.addItem(statusLine)
        menu.addItem(.separator())
        for slot in 0..<8 {
            let upload = NSMenuItem(title: "上传到头像 \(slot+1)…", action: #selector(selectPhotos(_:)), keyEquivalent: "")
            upload.tag = slot; upload.target = self; menu.addItem(upload)
        }
        menu.addItem(.separator())
        let quit = NSMenuItem(title: "退出头像助手",action: #selector(NSApplication.terminate(_:)),keyEquivalent: "q")
        quit.target = NSApplication.shared; menu.addItem(quit)
        statusItem.menu = menu
        uploader.statusHandler = { [weak self] status in
            DispatchQueue.main.async { self?.statusLine.title = status }
        }
    }

    @objc private func selectPhotos(_ sender: NSMenuItem) {
        let panel = NSOpenPanel()
        panel.title = "选择要传到圆屏的大头贴"
        panel.prompt = "上传"
        panel.allowedContentTypes = [.image]
        panel.allowsMultipleSelection = true
        panel.canChooseDirectories = false
        NSApplication.shared.activate(ignoringOtherApps: true)
        guard panel.runModal() == .OK else { return }
        let alert = NSAlert()
        guard panel.urls.count <= 8-sender.tag else {
            alert.messageText = "剩余位置不足，请最多选择 \(8-sender.tag) 张照片。"
            alert.runModal(); return
        }
        alert.messageText = "上传到头像 \(sender.tag+1)–\(sender.tag+panel.urls.count)？"
        alert.informativeText = "对应位置的旧头像会被替换，并在服务器保留一份备份。照片在本机裁剪、识别人脸，不发送给大模型；圆屏联网后自动同步。"
        alert.addButton(withTitle: "上传"); alert.addButton(withTitle: "取消")
        guard alert.runModal() == .alertFirstButtonReturn else { return }
        uploader.upload(panel.urls, startingSlot: sender.tag)
    }
}
