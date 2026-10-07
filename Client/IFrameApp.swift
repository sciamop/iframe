import Network
import SwiftUI
import UIKit

@main
struct IFrameApp: App {
    @StateObject private var session = StreamSession()
    @Environment(\.scenePhase) private var scenePhase

    var body: some Scene {
        WindowGroup {
            Group {
                if session.phase == .streaming {
                    StreamScreen()
                } else {
                    ConnectView()
                }
            }
            .environmentObject(session)
            .onChange(of: scenePhase) { _, phase in
                // iOS suspends sockets in the background; drop cleanly instead of freezing.
                if phase == .background, session.phase != .idle { session.disconnect() }
            }
        }
    }
}

extension StreamSession {
    /// The iPad's screen in pixels, in the current orientation.
    static func screenPixels() -> CGSize {
        let screen = UIScreen.main
        return CGSize(width: screen.bounds.width * screen.scale, height: screen.bounds.height * screen.scale)
    }

    func connect(to endpoint: NWEndpoint, pin: String, label: String, uiScale: Double) {
        connect(to: endpoint, pin: pin, label: label, deviceName: UIDevice.current.name,
                maxFPS: UIScreen.main.maximumFramesPerSecond, pixels: Self.screenPixels(), uiScale: uiScale)
    }
}
