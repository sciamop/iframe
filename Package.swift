// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "iFrame",
    platforms: [.macOS(.v14)],
    targets: [
        .target(name: "CGVirtualDisplayPrivate", path: "VirtualDisplay"),
        .executableTarget(
            name: "iframe-host",
            dependencies: ["CGVirtualDisplayPrivate"],
            path: ".",
            sources: ["Host", "Shared"],
            swiftSettings: [.unsafeFlags(["-Ounchecked"], .when(configuration: .release))]
        ),
    ]
)
