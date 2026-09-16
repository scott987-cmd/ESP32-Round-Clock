import Foundation
import IOKit.hid

private let roundRemoteVendorID = 0x303A
private let roundRemoteProductID = 0x175C
private let roundRemoteSerialNumber = "175C-REMOTE"
private let audioReportID: UInt32 = 2
private let audioAcknowledgementReportID: CFIndex = 3
private let photoReportID: CFIndex = 4
private let photoAcknowledgementReportID: UInt32 = 5
private let maximumInputReportLength = 256

final class HIDDeviceHandle: @unchecked Sendable {
    fileprivate let device: IOHIDDevice

    fileprivate init(_ device: IOHIDDevice) {
        self.device = device
    }
}

final class HIDReceiver: @unchecked Sendable {
    typealias ReportHandler = @Sendable (HIDDeviceHandle, [UInt8]) -> Void
    typealias PhotoAcknowledgementHandler = @Sendable (UInt16, UInt8, UInt8) -> Void

    private let manager: IOHIDManager
    private let handler: ReportHandler
    private var reportBuffers: [ObjectIdentifier: UnsafeMutablePointer<UInt8>] = [:]
    private var deviceHandles: [ObjectIdentifier: HIDDeviceHandle] = [:]
    private let deviceLock = NSLock()
    var photoAcknowledgementHandler: PhotoAcknowledgementHandler?

    init(handler: @escaping ReportHandler) {
        self.handler = handler
        manager = IOHIDManagerCreate(kCFAllocatorDefault, IOOptionBits(kIOHIDOptionsTypeNone))
    }

    deinit {
        for buffer in reportBuffers.values { buffer.deallocate() }
        IOHIDManagerClose(manager, IOOptionBits(kIOHIDOptionsTypeNone))
    }

    func start() throws {
        let match: [String: Any] = [
            kIOHIDVendorIDKey as String: roundRemoteVendorID,
            kIOHIDProductIDKey as String: roundRemoteProductID,
            kIOHIDSerialNumberKey as String: roundRemoteSerialNumber,
            kIOHIDPrimaryUsagePageKey as String: 0xFF00,
            kIOHIDPrimaryUsageKey as String: 0x01,
        ]
        IOHIDManagerSetDeviceMatching(manager, match as CFDictionary)
        IOHIDManagerRegisterDeviceMatchingCallback(
            manager, deviceMatchedCallback, Unmanaged.passUnretained(self).toOpaque())
        IOHIDManagerRegisterDeviceRemovalCallback(
            manager, deviceRemovedCallback, Unmanaged.passUnretained(self).toOpaque())
        IOHIDManagerScheduleWithRunLoop(
            manager, CFRunLoopGetCurrent(), CFRunLoopMode.defaultMode.rawValue)
        let result = IOHIDManagerOpen(manager, IOOptionBits(kIOHIDOptionsTypeNone))
        guard result == kIOReturnSuccess else { throw HIDReceiverError.managerOpen(result) }
    }

    fileprivate func attach(_ device: IOHIDDevice) {
        let key = ObjectIdentifier(device)
        guard reportBuffers[key] == nil else { return }
        let result = IOHIDDeviceOpen(device, IOOptionBits(kIOHIDOptionsTypeNone))
        guard result == kIOReturnSuccess else {
            print("无法打开 Vibe Remote：\(result)")
            return
        }
        let buffer = UnsafeMutablePointer<UInt8>.allocate(capacity: maximumInputReportLength)
        deviceLock.lock()
        deviceHandles[key] = HIDDeviceHandle(device)
        deviceLock.unlock()
        reportBuffers[key] = buffer
        IOHIDDeviceRegisterInputReportCallback(
            device, buffer, maximumInputReportLength, inputReportCallback,
            Unmanaged.passUnretained(self).toOpaque())
        print("已连接 Vibe Remote，等待识别文字…")
    }

    fileprivate func detach(_ device: IOHIDDevice) {
        let key = ObjectIdentifier(device)
        deviceLock.lock()
        deviceHandles.removeValue(forKey: key)
        deviceLock.unlock()
        if let buffer = reportBuffers.removeValue(forKey: key) {
            buffer.deallocate()
        }
        IOHIDDeviceClose(device, IOOptionBits(kIOHIDOptionsTypeNone))
        print("Vibe Remote 已断开")
    }

    fileprivate func receive(device: IOHIDDevice, reportID: UInt32, bytes: [UInt8]) {
        if reportID == audioReportID {
            handler(HIDDeviceHandle(device), bytes)
        } else if reportID == photoAcknowledgementReportID && bytes.count >= 5 && bytes[0] == 1 {
            let session = UInt16(bytes[2]) | (UInt16(bytes[3]) << 8)
            photoAcknowledgementHandler?(session, bytes[1], bytes[4])
        }
    }

    func connectedDevice() -> HIDDeviceHandle? {
        deviceLock.lock()
        defer { deviceLock.unlock() }
        return deviceHandles.values.first
    }

    func sendAcknowledgement(to handle: HIDDeviceHandle, session: UInt16, status: UInt8) {
        var report = [UInt8](repeating: 0, count: 4)
        report[0] = 1
        report[1] = status
        report[2] = UInt8(session & 0xFF)
        report[3] = UInt8(session >> 8)
        let reportCount = report.count
        let result = report.withUnsafeMutableBytes { buffer -> IOReturn in
            guard let base = buffer.bindMemory(to: UInt8.self).baseAddress else {
                return kIOReturnBadArgument
            }
            return IOHIDDeviceSetReport(
                handle.device, kIOHIDReportTypeOutput, audioAcknowledgementReportID,
                base, reportCount)
        }
        if result != kIOReturnSuccess {
            print("无法向圆屏返回识别状态：\(result)")
        }
    }

    func sendPhotoPacket(to handle: HIDDeviceHandle, packet: [UInt8]) throws {
        guard packet.count == PhotoPacketEncoder.packetBytes else {
            throw HIDReceiverError.invalidPhotoPacket(packet.count)
        }
        var packet = packet
        let packetCount = packet.count
        let result = packet.withUnsafeMutableBytes { buffer -> IOReturn in
            guard let base = buffer.bindMemory(to: UInt8.self).baseAddress else {
                return kIOReturnBadArgument
            }
            return IOHIDDeviceSetReport(
                handle.device, kIOHIDReportTypeOutput, photoReportID, base, packetCount)
        }
        guard result == kIOReturnSuccess else { throw HIDReceiverError.reportWrite(result) }
    }
}

enum HIDReceiverError: Error {
    case managerOpen(IOReturn)
    case invalidPhotoPacket(Int)
    case reportWrite(IOReturn)
}

private let deviceMatchedCallback: IOHIDDeviceCallback = { context, _, _, device in
    guard let context else { return }
    Unmanaged<HIDReceiver>.fromOpaque(context).takeUnretainedValue().attach(device)
}

private let deviceRemovedCallback: IOHIDDeviceCallback = { context, _, _, device in
    guard let context else { return }
    Unmanaged<HIDReceiver>.fromOpaque(context).takeUnretainedValue().detach(device)
}

private let inputReportCallback: IOHIDReportCallback = {
    context, result, sender, _, reportID, report, reportLength in
    guard result == kIOReturnSuccess, let context, let sender else { return }
    let bytes = Array(UnsafeBufferPointer(start: report, count: reportLength))
    let device = Unmanaged<IOHIDDevice>.fromOpaque(sender).takeUnretainedValue()
    Unmanaged<HIDReceiver>.fromOpaque(context).takeUnretainedValue()
        .receive(device: device, reportID: reportID, bytes: bytes)
}
