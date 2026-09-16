import Foundation
import Vision
import CoreGraphics
import Testing
@testable import RoundTextBridge

@Test func avatarPackageLayoutAndCRC() {
    let regions: [UInt16] = [80,100,30,12,170,100,30,12,110,175,55,24]
    let result = AvatarProcessor.pack(rgb: Data(repeating: 0,count: 156800),regions: regions)
    #expect(result.count == 156864)
    #expect(String(data: result.prefix(4),encoding: .utf8) == "RAV2")
    #expect(Array(result[4..<8]) == [24,1,24,1])
    #expect(Array(result[8..<16]) == [80,0,100,0,30,0,12,0])
    #expect(result[32..<60].allSatisfy { $0 == 0 })
    // Fixed independent zlib.crc32 vector for 156800 zero bytes.
    #expect(Array(result[60..<64]) == [0x1d,0x95,0x5e,0xdd])
}
@Test func rejectsUnreadableAvatar() {
    #expect(throws: (any Error).self) {
        _ = try AvatarProcessor().package(from: URL(fileURLWithPath: "/nonexistent/avatar.png"))
    }
}

@Test func croppedPixelsAndLandmarksAgreeOnPublicFixture() throws {
    let input = try #require(Bundle.module.url(forResource: "astronaut",withExtension: "png",subdirectory: "Fixtures"))
    let data = try AvatarProcessor().package(from: input)
    var rgba = [UInt8](repeating: 255,count: 280*280*4)
    for i in 0..<(280*280) {
        let value = UInt16(data[64+i*2]) | UInt16(data[65+i*2])<<8
        rgba[i*4] = UInt8((value>>11)*255/31)
        rgba[i*4+1] = UInt8(((value>>5)&63)*255/63)
        rgba[i*4+2] = UInt8((value&31)*255/31)
    }
    let provider = try #require(CGDataProvider(data: Data(rgba) as CFData))
    let image = try #require(CGImage(width: 280,height: 280,bitsPerComponent: 8,bitsPerPixel: 32,
                                    bytesPerRow: 1120,space: CGColorSpaceCreateDeviceRGB(),
                                    bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.last.rawValue),
                                    provider: provider,decode: nil,shouldInterpolate: true,intent: .defaultIntent))
    let request = VNDetectFaceLandmarksRequest()
    try VNImageRequestHandler(cgImage: image).perform([request])
    let face = try #require(request.results?.first)
    let marks = try #require(face.landmarks)
    for (i,region) in [marks.leftEye,marks.rightEye,marks.outerLips].enumerated() {
        let points = try #require(region).normalizedPoints
        let x = points.map { (face.boundingBox.minX+$0.x*face.boundingBox.width)*280 }.reduce(0,+)/CGFloat(points.count)
        let y = points.map { (1-face.boundingBox.minY-$0.y*face.boundingBox.height)*280 }.reduce(0,+)/CGFloat(points.count)
        func u16(_ o: Int) -> CGFloat { CGFloat(UInt16(data[o]) | UInt16(data[o+1])<<8) }
        let offset=8+i*8
        #expect(abs(x-u16(offset)-u16(offset+4)/2)<9)
        #expect(abs(y-u16(offset+2)-u16(offset+6)/2)<9)
    }
}
