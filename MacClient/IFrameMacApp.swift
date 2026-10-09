import AppKit
import Network
import SwiftUI

@main
struct IFrameMacApp: App {
    @NSApplicationDelegateAdaptor private var appDelegate: AppDelegate
    @StateObject private var session = StreamSession()
    @StateObject private var window = MainWindow()
    @AppStorage("showStats") private var showStats = false
    @AppStorage("macDensity") private var density = 1.0

    var body: some Scene {
        Window("iFrame", id: "main") {
            Group {
                if session.phase == .streaming {
                    MacStreamScreen()
                } else {
                    MacConnectView()
                }
            }
            .environmentObject(session)
            .environmentObject(window)
            .background(WindowReader(window: window))
            .onChange(of: session.phase) { old, phase in
                window.phaseChanged(from: old, to: phase, session: session)
            }
            .onChange(of: session.welcome) { _, welcome in
                if let welcome { window.welcomeChanged(welcome) }
            }
        }
        .windowResizability(.contentMinSize)
        .defaultSize(width: MainWindow.connectSize.width, height: MainWindow.connectSize.height)
        .commands {
            CommandGroup(replacing: .newItem) {}
            // ⌃⌥⌘ shortcuts stay with this app; every other key combo goes to the remote Mac.
            CommandMenu("Stream") {
                Button("Enter or Exit Full Screen") { window.window?.toggleFullScreen(nil) }
                    .keyboardShortcut("f", modifiers: [.control, .option, .command])
                Toggle("Show Stats", isOn: $showStats)
                    .keyboardShortcut("s", modifiers: [.control, .option, .command])
                Button("Refresh Picture") { session.requestKeyframe() }
                    .keyboardShortcut("r", modifiers: [.control, .option, .command])
                    .disabled(session.phase != .streaming)
                Divider()
                Picker("Mac Display", selection: $density) {
                    ForEach(MacDensity.virtualChoices) { choice in
                        Text(choice.label).tag(choice.rawValue)
                    }
                }
                .disabled(session.phase == .streaming && session.welcome?.isVirtual != true)
                Divider()
                Button("Disconnect") { session.disconnect() }
                    .keyboardShortcut("d", modifiers: [.control, .option, .command])
                    .disabled(session.phase == .idle)
            }
        }
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}

/// How dense the Mac's virtual display is, relative to this Mac's own screen.
/// 1 = the same size things are here (sharpest); lower = more desktop space; 0 = mirror the Mac's display.
enum MacDensity: Double, CaseIterable, Identifiable {
    case match = 1.0
    case more = 0.8
    case most = 0.667
    case mirror = 0

    var id: Double { rawValue }
    static let virtualChoices: [MacDensity] = [.match, .more, .most]

    var label: String {
        switch self {
        case .match: return "Match this Mac · sharpest"
        case .more: return "More space"
        case .most: return "Most space"
        case .mirror: return "Use the Mac's own display"
        }
    }

    /// The host's uiScale (pixels per Mac point) for a screen with this backing scale.
    func uiScale(backing: CGFloat) -> Double {
        rawValue <= 0 ? 0 : max(1, Double(backing) * rawValue)
    }
}

/// The app's single window: sized small for the connect form and large for the stream.
final class MainWindow: ObservableObject {
    static let connectSize = CGSize(width: 520, height: 720)

    weak var window: NSWindow?
    /// Content size (points) the stream will use, decided when connecting.
    private var streamSize: CGSize?
    private var connectFrame: NSRect?
    /// Go full screen when the stream starts, and leave it again when it ends.
    private var enterFullScreen = false
    private var enteredFullScreen = false

    var backingScale: CGFloat { window?.backingScaleFactor ?? NSScreen.main?.backingScaleFactor ?? 2 }
    var maxFPS: Int { (window?.screen ?? NSScreen.main)?.maximumFramesPerSecond ?? 60 }

    /// Picks the window size for streaming and returns it in pixels for the host's virtual display.
    /// With `fullScreen`, the stream takes the whole screen, so the Mac's desktop matches it.
    func prepareStream(fullScreen: Bool) -> CGSize {
        let screen = window?.screen ?? NSScreen.main
        let size: CGSize
        if let window, window.styleMask.contains(.fullScreen) {
            size = window.contentLayoutRect.size
            enterFullScreen = false
        } else if fullScreen, let screen {
            // Full screen windows sit below the notch on Macs that have one.
            size = CGSize(width: screen.frame.width, height: screen.frame.height - screen.safeAreaInsets.top)
            enterFullScreen = true
        } else {
            let visible = screen?.visibleFrame.size ?? CGSize(width: 1440, height: 900)
            let titleBar = chrome.height
            let saved = CGSize(width: UserDefaults.standard.double(forKey: "streamWidth"),
                               height: UserDefaults.standard.double(forKey: "streamHeight"))
            let wanted = saved.width >= 640 && saved.height >= 480
                ? saved
                : CGSize(width: visible.width * 0.85, height: visible.height * 0.85 - titleBar)
            size = CGSize(width: min(wanted.width, visible.width).rounded(),
                          height: min(wanted.height, visible.height - titleBar).rounded())
            enterFullScreen = false
        }
        streamSize = size
        return CGSize(width: size.width * backingScale, height: size.height * backingScale)
    }

    func phaseChanged(from old: StreamSession.Phase, to phase: StreamSession.Phase, session: StreamSession) {
        guard let window else { return }
        let fullScreen = window.styleMask.contains(.fullScreen)
        if phase == .streaming, old != .streaming {
            window.contentMinSize = CGSize(width: 640, height: 480)
            window.title = session.welcome?.hostName ?? session.hostLabel
            guard !fullScreen else { return }
            connectFrame = window.frame
            if enterFullScreen {
                enteredFullScreen = true
                window.toggleFullScreen(nil)
            } else if let size = streamSize {
                setContentSize(size, centered: true)
            }
        } else if old == .streaming, phase != .streaming {
            window.title = "iFrame"
            window.contentMinSize = CGSize(width: 420, height: 520)
            if fullScreen, enteredFullScreen {
                // Leaving full screen restores the connect window's frame.
                enteredFullScreen = false
                window.toggleFullScreen(nil)
                return
            }
            enteredFullScreen = false
            guard !fullScreen else { return }
            let content = window.contentLayoutRect.size
            UserDefaults.standard.set(content.width, forKey: "streamWidth")
            UserDefaults.standard.set(content.height, forKey: "streamHeight")
            if let connectFrame {
                window.setFrame(connectFrame, display: true, animate: true)
            } else {
                setContentSize(Self.connectSize, centered: true)
            }
        }
    }

    /// Mirroring the Mac's own display: shape the window like it.
    func welcomeChanged(_ welcome: Welcome) {
        guard let window else { return }
        window.title = welcome.hostName
        guard !welcome.isVirtual, welcome.width > 0, welcome.height > 0 else {
            return
        }
        let aspect = CGSize(width: welcome.width, height: welcome.height)
        guard !window.styleMask.contains(.fullScreen) else { return }
        let bounds = window.contentLayoutRect.size
        let scale = min(bounds.width / aspect.width, bounds.height / aspect.height)
        setContentSize(CGSize(width: (aspect.width * scale).rounded(), height: (aspect.height * scale).rounded()),
                       centered: false)
    }

    /// Window frame minus the area SwiftUI lays out in. SwiftUI windows use a full-size content
    /// view, so this is the title bar (plus toolbar), not what frameRect(forContentRect:) reports.
    private var chrome: CGSize {
        guard let window else { return CGSize(width: 0, height: 28) }
        return CGSize(width: window.frame.width - window.contentLayoutRect.width,
                      height: window.frame.height - window.contentLayoutRect.height)
    }

    /// Sizes the window so SwiftUI's layout area (below the title bar) is exactly `size`.
    private func setContentSize(_ size: CGSize, centered: Bool) {
        guard let window else { return }
        var frame = NSRect(origin: .zero, size: CGSize(width: size.width + chrome.width, height: size.height + chrome.height))
        let visible = (window.screen ?? NSScreen.main)?.visibleFrame ?? .zero
        if centered {
            frame.origin = CGPoint(x: visible.midX - frame.width / 2, y: visible.midY - frame.height / 2)
        } else {
            // Keep the top-left corner where it is.
            frame.origin = CGPoint(x: window.frame.minX, y: window.frame.maxY - frame.height)
        }
        frame.origin.x = max(visible.minX, min(frame.origin.x, visible.maxX - frame.width))
        frame.origin.y = max(visible.minY, min(frame.origin.y, visible.maxY - frame.height))
        window.setFrame(frame, display: true, animate: true)
    }
}

/// Hands the hosting NSWindow to MainWindow.
private struct WindowReader: NSViewRepresentable {
    let window: MainWindow

    func makeNSView(context: Context) -> NSView {
        let view = NSView()
        DispatchQueue.main.async { [weak view] in
            window.window = view?.window
            view?.window?.tabbingMode = .disallowed
        }
        return view
    }

    func updateNSView(_ view: NSView, context: Context) {
        if window.window == nil, let w = view.window { window.window = w }
    }
}

extension StreamSession {
    func connect(to endpoint: NWEndpoint, pin: String, label: String, window: MainWindow, density: MacDensity) {
        let pixels = window.prepareStream(fullScreen: UserDefaults.standard.object(forKey: "fullScreenStream") as? Bool ?? true)
        connect(to: endpoint, pin: pin, label: label,
                deviceName: Host.current().localizedName ?? "Mac",
                maxFPS: window.maxFPS, pixels: pixels,
                uiScale: density.uiScale(backing: window.backingScale))
    }
}
