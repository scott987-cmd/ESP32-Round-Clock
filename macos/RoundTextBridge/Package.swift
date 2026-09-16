// swift-tools-version: 6.0

import PackageDescription

let package = Package(
    name: "RoundTextBridge",
    platforms: [.macOS(.v13)],
    products: [.executable(name: "RoundTextBridge", targets: ["RoundTextBridge"])],
    targets: [
        .executableTarget(
            name: "RoundTextBridge",
            swiftSettings: [.unsafeFlags(["-strict-concurrency=minimal"])],
            linkerSettings: [
                .linkedFramework("AppKit"),
                .linkedFramework("ImageIO"),
                .linkedFramework("IOKit"),
                .linkedFramework("Vision"),
            ]
        ),
        .testTarget(name: "RoundTextBridgeTests", dependencies: ["RoundTextBridge"], resources: [.copy("Fixtures")]),
    ]
)
