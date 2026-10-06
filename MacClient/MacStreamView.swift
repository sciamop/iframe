import AppKit
import AVFoundation
import SwiftUI

/// The video surface plus all input handling.
///
/// Mouse, trackpad and keyboard map 1:1 to the remote Mac. NSEvent key codes are already macOS
/// virtual key codes, so keys pass straight through. While this view has focus, every key combo
/// (⌘Q, ⌘W, ⌘Space…) goes to the remote Mac except ⌃⌥⌘ ones, which drive this app's Stream menu.
/// The system still keeps ⌘Tab and other shortcuts it handles before apps see them.
///
/// The Mac's pointer shape arrives as a picture and becomes this window's NSCursor, so the pointer
/// is the real hardware cursor and moves with zero lag. Capture leaves it out of the video.
final class StreamNSView: NSView {
    let session: StreamSession
    private let displayLayer = AVSampleBufferDisplayLayer()

    var welcome: Welcome? {
        didSet {
            guard welcome != oldValue else { return }
            updateCursor()
            scheduleDisplayRequest()
        }
    }

    /// See MacDensity. Changing it reshapes the virtual display.
    var density: Double = 1 {
        didSet { if density != oldValue { scheduleDisplayRequest(force: true) } }
    }

    private var keyMonitor: Any?
    private var heldKeys = Set<UInt16>()
    private var heldModifiers = Set<UInt16>()
    private var heldButtons = Set<UInt8>()
    private var lastPoint = CGPoint(x: 0.5, y: 0.5)
    private var macCursor: MacCursor?
    private var nsCursor: NSCursor = .arrow

    init(session: StreamSession) {
        self.session = session
        super.init(frame: .zero)
        wantsLayer = true
        layerContentsRedrawPolicy = .never
        displayLayer.videoGravity = .resizeAspect
        displayLayer.backgroundColor = NSColor.black.cgColor
        session.attach(renderer: displayLayer.sampleBufferRenderer)
        session.onCursor = { [weak self] cursor in
            self?.macCursor = cursor
            self?.updateCursor()
        }
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is not supported") }

    override func makeBackingLayer() -> CALayer { displayLayer }

    override var acceptsFirstResponder: Bool { true }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        let center = NotificationCenter.default
        center.removeObserver(self)
        if let window {
            DispatchQueue.main.async { window.makeFirstResponder(self) }
            installKeyMonitor()
            center.addObserver(self, selector: #selector(windowResignedKey),
                               name: NSWindow.didResignKeyNotification, object: window)
        } else {
            releaseAll()
            removeKeyMonitor()
            pendingRequest?.cancel()
        }
    }

    override func resignFirstResponder() -> Bool {
        releaseAll()
        return super.resignFirstResponder()
    }

    @objc private func windowResignedKey() {
        releaseAll()
    }

    // MARK: Geometry

    private var videoRect: CGRect {
        guard let welcome, welcome.width > 0, welcome.height > 0 else { return bounds }
        return AVMakeRect(aspectRatio: CGSize(width: welcome.width, height: welcome.height), insideRect: bounds)
    }

    /// View point -> 0...1 over the video, top-left origin like the Mac's screen.
    private func normalized(_ event: NSEvent) -> CGPoint {
        let p = convert(event.locationInWindow, from: nil)
        let rect = videoRect
        guard rect.width > 0, rect.height > 0 else { return lastPoint }
        lastPoint = CGPoint(x: min(max((p.x - rect.minX) / rect.width, 0), 1),
                            y: min(max((rect.maxY - p.y) / rect.height, 0), 1))
        return lastPoint
    }

    /// Mac points per view point, so trackpad scrolling moves content 1:1 under your fingers.
    private var scrollScale: CGFloat {
        guard let welcome, videoRect.width > 0 else { return 1 }
        return CGFloat(welcome.pointWidth) / videoRect.width
    }

    override func setFrameSize(_ newSize: NSSize) {
        super.setFrameSize(newSize)
        updateCursor()
        scheduleDisplayRequest()
    }

    override func viewDidEndLiveResize() {
        super.viewDidEndLiveResize()
        scheduleDisplayRequest()
    }

    override func viewDidChangeBackingProperties() {
        super.viewDidChangeBackingProperties()
        scheduleDisplayRequest(force: true)
    }

    // MARK: Virtual display follows the window

    private var pendingRequest: DispatchWorkItem?
    private var requestedPixels: CGSize?
    private var requestedScale: Double?

    /// The host's virtual display is reshaped in place, so resizing the window (or going full screen)
    /// gives the Mac a desktop exactly this size instead of letterboxing a fixed one.
    private func scheduleDisplayRequest(force: Bool = false) {
        guard let welcome, welcome.isVirtual else { return }
        if requestedPixels == nil {
            // The stream we connected with already matches; only react to changes from here on.
            requestedPixels = targetPixels
            requestedScale = targetScale
            if !force { return }
        }
        pendingRequest?.cancel()
        let work = DispatchWorkItem { [weak self] in self?.sendDisplayRequest() }
        pendingRequest = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.4, execute: work)
    }

    private var backing: CGFloat { window?.backingScaleFactor ?? 2 }

    private var targetPixels: CGSize {
        // The host needs at least 640x480 for a virtual display.
        CGSize(width: max(640, (bounds.width * backing).rounded()), height: max(480, (bounds.height * backing).rounded()))
    }

    private var targetScale: Double {
        MacDensity(rawValue: density)?.uiScale(backing: backing) ?? Double(backing)
    }

    private func sendDisplayRequest() {
        guard !inLiveResize, window != nil, bounds.width > 0, welcome?.isVirtual == true else { return }
        let pixels = targetPixels
        let scale = targetScale
        if let current = requestedPixels, abs(current.width - pixels.width) <= 2,
           abs(current.height - pixels.height) <= 2, requestedScale == scale { return }
        requestedPixels = pixels
        requestedScale = scale
        session.requestDisplay(pixels: pixels, uiScale: scale)
    }

    // MARK: Cursor

    /// Sizes the Mac's pointer like it is on the Mac: Mac points scaled to the video on screen.
    private func updateCursor() {
        if let macCursor, let welcome, welcome.pointWidth > 0 {
            let scale = videoRect.width / CGFloat(welcome.pointWidth)   // view points per Mac point
            let size = NSSize(width: max(1, macCursor.size.width * scale), height: max(1, macCursor.size.height * scale))
            nsCursor = NSCursor(image: NSImage(cgImage: macCursor.image, size: size),
                                hotSpot: NSPoint(x: macCursor.hotspot.x * scale, y: macCursor.hotspot.y * scale))
        } else {
            nsCursor = .arrow
        }
        window?.invalidateCursorRects(for: self)
        // Cursor rects only apply on the next mouse move; show a new shape right away.
        if let window, window.isKeyWindow, videoRect.contains(convert(window.mouseLocationOutsideOfEventStream, from: nil)) {
            nsCursor.set()
        }
    }

    override func resetCursorRects() {
        addCursorRect(videoRect, cursor: nsCursor)
    }

    // MARK: Mouse

    private var trackingArea: NSTrackingArea?

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        if let trackingArea { removeTrackingArea(trackingArea) }
        let area = NSTrackingArea(rect: .zero, options: [.mouseMoved, .activeInKeyWindow, .inVisibleRect],
                                  owner: self, userInfo: nil)
        addTrackingArea(area)
        trackingArea = area
    }

    override func mouseMoved(with event: NSEvent) { session.mouseMove(normalized(event)) }
    override func mouseDragged(with event: NSEvent) { session.mouseMove(normalized(event)) }
    override func rightMouseDragged(with event: NSEvent) { session.mouseMove(normalized(event)) }
    override func otherMouseDragged(with event: NSEvent) { session.mouseMove(normalized(event)) }

    override func mouseDown(with event: NSEvent) { button(0, down: true, event) }
    override func mouseUp(with event: NSEvent) { button(0, down: false, event) }
    override func rightMouseDown(with event: NSEvent) { button(1, down: true, event) }
    override func rightMouseUp(with event: NSEvent) { button(1, down: false, event) }
    override func otherMouseDown(with event: NSEvent) { button(UInt8(clamping: event.buttonNumber), down: true, event) }
    override func otherMouseUp(with event: NSEvent) { button(UInt8(clamping: event.buttonNumber), down: false, event) }

    private func button(_ button: UInt8, down: Bool, _ event: NSEvent) {
        if down {
            window?.makeFirstResponder(self)
            heldButtons.insert(button)
        } else {
            heldButtons.remove(button)
        }
        let p = normalized(event)
        session.mouseMove(p)
        session.mouseButton(button, down: down, at: p)
    }

    override func scrollWheel(with event: NSEvent) {
        var dx = event.scrollingDeltaX
        var dy = event.scrollingDeltaY
        if event.hasPreciseScrollingDeltas {
            dx *= scrollScale
            dy *= scrollScale
        } else {
            // Wheel mice report lines.
            dx *= 16
            dy *= 16
        }
        guard dx != 0 || dy != 0 else { return }
        session.scroll(dx: dx, dy: dy)
    }

    // MARK: Keyboard

    /// Device-dependent modifier bits (NX_DEVICE*KEYMASK), to tell a left from a right modifier.
    private static let modifierBits: [UInt16: UInt] = [
        0x3B: 0x0001, 0x3E: 0x2000,   // left, right control
        0x38: 0x0002, 0x3C: 0x0004,   // left, right shift
        0x37: 0x0008, 0x36: 0x0010,   // left, right command
        0x3A: 0x0020, 0x3D: 0x0040,   // left, right option
    ]
    private static let capsLock: UInt16 = 0x39
    private static let appShortcut: NSEvent.ModifierFlags = [.control, .option, .command]

    /// A local monitor sees every key event before menus do, including key-ups with ⌘ held
    /// (which never reach keyUp(with:)), so shortcuts can go to the remote Mac.
    private func installKeyMonitor() {
        guard keyMonitor == nil else { return }
        keyMonitor = NSEvent.addLocalMonitorForEvents(matching: [.keyDown, .keyUp, .flagsChanged]) { [weak self] event in
            guard let self, let window = self.window, event.window === window, window.isKeyWindow,
                  window.firstResponder === self else { return event }
            if event.type == .keyDown,
               event.modifierFlags.intersection(.deviceIndependentFlagsMask).isSuperset(of: Self.appShortcut) {
                return event   // the Stream menu
            }
            self.forward(event)
            return nil
        }
    }

    private func removeKeyMonitor() {
        if let keyMonitor { NSEvent.removeMonitor(keyMonitor) }
        keyMonitor = nil
    }

    private static func mods(_ flags: NSEvent.ModifierFlags) -> KeyMods {
        var mods = KeyMods()
        if flags.contains(.shift) { mods.insert(.shift) }
        if flags.contains(.control) { mods.insert(.control) }
        if flags.contains(.option) { mods.insert(.option) }
        if flags.contains(.command) { mods.insert(.command) }
        if flags.contains(.capsLock) { mods.insert(.capsLock) }
        return mods
    }

    private func forward(_ event: NSEvent) {
        let code = event.keyCode
        let mods = Self.mods(event.modifierFlags)
        switch event.type {
        case .keyDown:
            heldKeys.insert(code)
            session.key(code, action: event.isARepeat ? .repeatDown : .down, mods: mods)
        case .keyUp:
            heldKeys.remove(code)
            session.key(code, action: .up, mods: mods)
        case .flagsChanged:
            if code == Self.capsLock {
                // Each press toggles caps lock; there's no separate release event.
                session.key(code, action: .down, mods: mods)
                session.key(code, action: .up, mods: mods)
            } else if let bit = Self.modifierBits[code] {
                let down = event.modifierFlags.rawValue & bit != 0
                if down { heldModifiers.insert(code) } else { heldModifiers.remove(code) }
                session.key(code, action: down ? .down : .up, mods: mods)
            }
        default:
            break
        }
    }

    /// Lets go of everything on the remote Mac when focus leaves, so nothing stays stuck down.
    func releaseAll() {
        for code in heldKeys.union(heldModifiers) { session.key(code, action: .up, mods: []) }
        heldKeys.removeAll()
        heldModifiers.removeAll()
        for button in heldButtons { session.mouseButton(button, down: false, at: lastPoint) }
        heldButtons.removeAll()
    }
}

private struct StreamViewRepresentable: NSViewRepresentable {
    let session: StreamSession
    let welcome: Welcome?
    let density: Double

    func makeNSView(context: Context) -> StreamNSView {
        StreamNSView(session: session)
    }

    func updateNSView(_ view: StreamNSView, context: Context) {
        view.welcome = welcome
        view.density = density
    }

    static func dismantleNSView(_ view: StreamNSView, coordinator: ()) {
        view.releaseAll()
    }
}

struct MacStreamScreen: View {
    @EnvironmentObject private var session: StreamSession
    @AppStorage("showStats") private var showStats = false
    @AppStorage("macDensity") private var density = MacDensity.match.rawValue

    var body: some View {
        ZStack(alignment: .topLeading) {
            StreamViewRepresentable(session: session, welcome: session.welcome,
                                    density: density > 0 ? density : MacDensity.match.rawValue)
            if showStats {
                StatsView(stats: session.stats, welcome: session.welcome)
                    .padding(10)
                    .allowsHitTesting(false)
            }
        }
        .background(Color.black)
        .frame(minWidth: 640, minHeight: 480)
    }
}
