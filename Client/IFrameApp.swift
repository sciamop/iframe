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
                switch phase {
                case .background: enterBackground()
                case .active: enterForeground()
                default: break
                }
            }
        }
    }

    private static var backgroundTask = UIBackgroundTaskIdentifier.invalid

    /// Keeps the stream alive for a quick app switch. iOS gives a background task about 30 s;
    /// when that runs out, drop the connection cleanly and reconnect on return.
    private func enterBackground() {
        session.inBackground = true
        guard session.phase == .streaming || session.phase == .connecting,
              Self.backgroundTask == .invalid else { return }
        Self.backgroundTask = UIApplication.shared.beginBackgroundTask(withName: "iFrame stream") { [session] in
            session.suspend()
            Self.endBackgroundTask()
        }
    }

    private func enterForeground() {
        Self.endBackgroundTask()
        session.inBackground = false
        session.resume(pixels: StreamSession.screenPixels())
    }

    private static func endBackgroundTask() {
        guard backgroundTask != .invalid else { return }
        UIApplication.shared.endBackgroundTask(backgroundTask)
        backgroundTask = .invalid
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
