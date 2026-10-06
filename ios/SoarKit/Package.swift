// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "SoarKit",
    platforms: [.iOS(.v15), .macOS(.v12)],
    products: [
        .library(name: "SoarKit", targets: ["SoarKit"]),
    ],
    targets: [
        .target(name: "SoarKit"),
        .testTarget(name: "SoarKitTests", dependencies: ["SoarKit"]),
    ]
)
