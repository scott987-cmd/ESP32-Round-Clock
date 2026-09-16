import AppKit
import Vision
import ImageIO

enum AvatarError: LocalizedError {
    case invalidImage, noFace, landmarks, configuration, upload
    var errorDescription: String? {
        switch self {
        case .invalidImage: return "无法读取图片，请选择 JPG、PNG 或 HEIC。"
        case .noFace: return "请使用只有一张清晰正脸的照片，避免侧脸和遮挡。"
        case .landmarks: return "眼睛或嘴巴没有识别清楚，请换一张正脸照片。"
        case .configuration: return "助手安全配置不完整，请重新执行设备配置。"
        case .upload: return "上传失败，请检查网络和圆屏证书后重试。"
        }
    }
}

struct AvatarProcessor {
    // Only the cropped avatar leaves this Mac, never the original photograph.
    func package(from url: URL) throws -> Data {
        guard let source = CGImageSourceCreateWithURL(url as CFURL, nil),
              let image = CGImageSourceCreateThumbnailAtIndex(source, 0, [
                kCGImageSourceCreateThumbnailFromImageAlways: true,
                kCGImageSourceCreateThumbnailWithTransform: true,
                kCGImageSourceThumbnailMaxPixelSize: 1600] as CFDictionary)
        else { throw AvatarError.invalidImage }
        let request = VNDetectFaceLandmarksRequest()
        try VNImageRequestHandler(cgImage: image).perform([request])
        guard let faces = request.results, faces.count == 1, let face = faces.first,
              abs(face.yaw?.doubleValue ?? 0) < 0.35, abs(face.roll?.doubleValue ?? 0) < 0.25
        else { throw AvatarError.noFace }
        let iw = CGFloat(image.width), ih = CGFloat(image.height), box = face.boundingBox
        let side = min(max(box.width * iw, box.height * ih) * 1.65, iw, ih)
        let crop = CGRect(x: min(max(box.midX * iw-side/2,0),iw-side),
                          y: min(max((1-box.midY) * ih-side*0.55,0),ih-side),
                          width: side, height: side).integral
        guard let head = image.cropping(to: crop), let marks = face.landmarks else { throw AvatarError.landmarks }
        func region(_ points: VNFaceLandmarkRegion2D?) throws -> [UInt16] {
            guard let points, points.pointCount >= 4 else { throw AvatarError.landmarks }
            let mapped: [CGPoint] = points.normalizedPoints.map { point -> CGPoint in
                let imageX: CGFloat = (box.minX + point.x * box.width) * iw
                let imageY: CGFloat = (1 - box.minY - point.y * box.height) * ih
                let outputX: CGFloat = 12 + (imageX - crop.minX) * 256 / crop.width
                let outputY: CGFloat = 12 + (imageY - crop.minY) * 256 / crop.height
                return CGPoint(x: outputX, y: outputY)
            }
            let xValues: [CGFloat] = mapped.map { $0.x }
            let yValues: [CGFloat] = mapped.map { $0.y }
            guard let rawMinX = xValues.min(), let rawMaxX = xValues.max(),
                  let rawMinY = yValues.min(), let rawMaxY = yValues.max()
            else { throw AvatarError.landmarks }
            let minX: CGFloat = floor(rawMinX) - 2
            let maxX: CGFloat = ceil(rawMaxX) + 2
            let minY: CGFloat = floor(rawMinY) - 2
            let maxY: CGFloat = ceil(rawMaxY) + 2
            guard minX >= 8, minY >= 8, maxX <= 272, maxY <= 272,
                  maxX-minX >= 4, maxX-minX <= 120, maxY-minY >= 2, maxY-minY <= 80
            else { throw AvatarError.landmarks }
            return [minX,minY,maxX-minX,maxY-minY].map { UInt16(Int($0)) }
        }
        let regions = try region(marks.leftEye) + region(marks.rightEye) + region(marks.outerLips)
        var rgba = [UInt8](repeating: 0, count: 280*280*4)
        let success = rgba.withUnsafeMutableBytes { buffer -> Bool in
            guard let ctx = CGContext(data: buffer.baseAddress, width: 280, height: 280,
                                      bitsPerComponent: 8, bytesPerRow: 1120,
                                      space: CGColorSpaceCreateDeviceRGB(),
                                      bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue | CGBitmapInfo.byteOrder32Big.rawValue)
            else { return false }
            ctx.setFillColor(NSColor(calibratedRed: 1, green: 0.91, blue: 0.94, alpha: 1).cgColor)
            ctx.fill(CGRect(x: 0,y: 0,width: 280,height: 280))
            ctx.addEllipse(in: CGRect(x: 12,y: 12,width: 256,height: 256)); ctx.clip()
            ctx.interpolationQuality = .high
            ctx.draw(head,in: CGRect(x: 12,y: 12,width: 256,height: 256))
            return true
        }
        guard success else { throw AvatarError.invalidImage }
        return Self.pack(rgb: RGB565Converter.convert(rgba: rgba,width: 280,height: 280), regions: regions)
    }

    static func pack(rgb: Data, regions: [UInt16]) -> Data {
        precondition(rgb.count == 156800 && regions.count == 12)
        var result = Data("RAV2".utf8)
        for value in [UInt16(280),280] + regions {
            result.append(UInt8(value & 255)); result.append(UInt8(value >> 8))
        }
        result.append(Data(repeating: 0,count: 28))
        var crc: UInt32 = 0xffffffff
        for byte in rgb {
            crc ^= UInt32(byte)
            for _ in 0..<8 { crc = (crc >> 1) ^ ((crc & 1) != 0 ? 0xedb88320 : 0) }
        }
        crc ^= 0xffffffff
        for shift in stride(from: 0,to: 32,by: 8) { result.append(UInt8((crc >> shift) & 255)) }
        result.append(rgb)
        return result
    }
}
