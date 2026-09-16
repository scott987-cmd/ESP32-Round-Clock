import AppKit
import Foundation
import ImageIO
import Vision

enum PhotoProcessorError: Error {
    case unreadableImage
    case renderFailed
}

struct PhotoProcessor {
    func makeAvatar(from url: URL) throws -> Data {
        guard let source = CGImageSourceCreateWithURL(url as CFURL, nil),
              let image = CGImageSourceCreateThumbnailAtIndex(source, 0, [
                  kCGImageSourceCreateThumbnailFromImageAlways: true,
                  kCGImageSourceCreateThumbnailWithTransform: true,
                  kCGImageSourceThumbnailMaxPixelSize: 2048,
              ] as CFDictionary)
        else { throw PhotoProcessorError.unreadableImage }

        let crop = faceCrop(in: image)
        guard let face = image.cropping(to: crop) else {
            throw PhotoProcessorError.unreadableImage
        }
        let width = PhotoPacketEncoder.width
        let height = PhotoPacketEncoder.height
        var rgba = [UInt8](repeating: 0, count: width * height * 4)
        guard let context = CGContext(
            data: &rgba, width: width, height: height, bitsPerComponent: 8,
            bytesPerRow: width * 4, space: CGColorSpaceCreateDeviceRGB(),
            bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue |
                CGBitmapInfo.byteOrder32Big.rawValue)
        else { throw PhotoProcessorError.renderFailed }

        let colors = [
            NSColor(calibratedRed: 0.10, green: 0.04, blue: 0.23, alpha: 1).cgColor,
            NSColor(calibratedRed: 0.07, green: 0.55, blue: 0.63, alpha: 1).cgColor,
        ] as CFArray
        let gradient = CGGradient(
            colorsSpace: CGColorSpaceCreateDeviceRGB(), colors: colors,
            locations: [0, 1])!
        context.drawLinearGradient(
            gradient, start: CGPoint(x: 0, y: height), end: CGPoint(x: width, y: 0),
            options: [])

        context.setFillColor(NSColor(calibratedRed: 0.23, green: 0.16, blue: 0.49, alpha: 1).cgColor)
        context.fillEllipse(in: CGRect(x: 42, y: -18, width: 196, height: 112))
        context.setStrokeColor(NSColor(calibratedRed: 0.57, green: 0.95, blue: 1, alpha: 0.9).cgColor)
        context.setLineWidth(8)
        context.strokeEllipse(in: CGRect(x: 42, y: -18, width: 196, height: 112))

        let portrait = CGRect(x: 24, y: 38, width: 232, height: 232)
        context.saveGState()
        context.addEllipse(in: portrait)
        context.clip()
        context.translateBy(x: 0, y: CGFloat(height))
        context.scaleBy(x: 1, y: -1)
        let flippedPortrait = CGRect(
            x: portrait.minX, y: CGFloat(height) - portrait.maxY,
            width: portrait.width, height: portrait.height)
        context.interpolationQuality = .high
        context.draw(face, in: flippedPortrait)
        context.restoreGState()
        context.setStrokeColor(NSColor(calibratedRed: 0.78, green: 0.97, blue: 1, alpha: 1).cgColor)
        context.setLineWidth(7)
        context.strokeEllipse(in: portrait.insetBy(dx: 3.5, dy: 3.5))

        context.setFillColor(NSColor(calibratedWhite: 1, alpha: 0.85).cgColor)
        context.fillEllipse(in: CGRect(x: 30, y: 235, width: 8, height: 8))
        context.fillEllipse(in: CGRect(x: 250, y: 214, width: 5, height: 5))
        context.fillEllipse(in: CGRect(x: 17, y: 188, width: 4, height: 4))
        return RGB565Converter.convert(rgba: rgba, width: width, height: height)
    }

    private func faceCrop(in image: CGImage) -> CGRect {
        let request = VNDetectFaceRectanglesRequest()
        try? VNImageRequestHandler(cgImage: image).perform([request])
        guard let face = request.results?.max(by: {
            $0.boundingBox.width * $0.boundingBox.height <
                $1.boundingBox.width * $1.boundingBox.height
        }) else {
            let side = CGFloat(min(image.width, image.height))
            return CGRect(
                x: (CGFloat(image.width) - side) / 2,
                y: (CGFloat(image.height) - side) / 2,
                width: side, height: side).integral
        }

        let imageWidth = CGFloat(image.width)
        let imageHeight = CGFloat(image.height)
        let box = face.boundingBox
        let centerX = (box.midX * imageWidth)
        let centerY = (1 - box.midY) * imageHeight + box.height * imageHeight * 0.08
        var side = max(box.width * imageWidth, box.height * imageHeight) * 1.75
        side = min(side, imageWidth, imageHeight)
        let x = min(max(centerX - side / 2, 0), imageWidth - side)
        let y = min(max(centerY - side / 2, 0), imageHeight - side)
        return CGRect(x: x, y: y, width: side, height: side).integral
    }
}

enum RGB565Converter {
    static func convert(rgba: [UInt8], width: Int, height: Int) -> Data {
        precondition(rgba.count == width * height * 4)
        var output = Data(count: width * height * 2)
        output.withUnsafeMutableBytes { target in
            let bytes = target.bindMemory(to: UInt8.self)
            for pixel in 0..<(width * height) {
                let source = pixel * 4
                let value = (UInt16(rgba[source] & 0xF8) << 8) |
                    (UInt16(rgba[source + 1] & 0xFC) << 3) |
                    UInt16(rgba[source + 2] >> 3)
                bytes[pixel * 2] = UInt8(value & 0xFF)
                bytes[pixel * 2 + 1] = UInt8(value >> 8)
            }
        }
        return output
    }
}
