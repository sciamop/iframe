import AVFoundation
import GameController
import SwiftUI
import UIKit

/// Full-screen video surface plus all input handling.
///
/// Trackpad / mouse: pointer moves, clicks (right click = secondary button), and two-finger
/// scrolling map 1:1 to the Mac. Touch: tap = click, two-finger tap = right click,
/// drag = move pointer, long-press then drag = click-drag, two-finger drag = scroll,
/// three-finger tap = toolbar.
final class StreamUIView: UIView, UIPointerInteractionDelegate {
    override class var layerClass: AnyClass { AVSampleBufferDisplayLayer.self }
    private var displayLayer: AVSampleBufferDisplayLayer { layer as! AVSampleBufferDisplayLayer }

    let session: StreamSession
    var welcome: Welcome?
    var onThreeFingerTap: (() -> Void)?

    private let keyboardProxy = KeyboardProxy()
    private var pointerButton: UInt8?
    private var heldModifiers: [UInt16: KeyMods] = [:]
    private var capsLock = false
    private var heldKeys = Set<UInt16>()
    private var repeatTimer: Timer?
    private var keyboardShownForFocus = false
    private var focusRect: CGRect?        // normalized, while a Mac text field is focused
    private var keyboardTop: CGFloat?     // in view coordinates, while the on-screen keyboard is up

    init(session: StreamSession) {
        self.session = session
        super.init(frame: .zero)
        backgroundColor = .black
        isMultipleTouchEnabled = true
        displayLayer.videoGravity = .resizeAspect
        addInteraction(UIPointerInteraction(delegate: self))
        keyboardProxy.session = session
        addSubview(keyboardProxy)
        setUpGestures()
        session.attach(renderer: displayLayer.sampleBufferRenderer)
        session.onTextFocus = { [weak self] editable, rect in self?.textFocusChanged(editable, rect) }
        NotificationCenter.default.addObserver(self, selector: #selector(keyboardFrameChanged(_:)),
                                               name: UIResponder.keyboardWillChangeFrameNotification, object: nil)
        NotificationCenter.default.addObserver(self, selector: #selector(keyboardWillHide(_:)),
                                               name: UIResponder.keyboardWillHideNotification, object: nil)
    }

    // MARK: On-screen keyboard follows Mac text focus

    private func textFocusChanged(_ editable: Bool, _ rect: CGRect) {
        focusRect = editable ? rect : nil
        // With a hardware keyboard attached there's nothing to show.
        if editable, GCKeyboard.coalesced == nil, !keyboardProxy.isFirstResponder {
            keyboardShownForFocus = true
            keyboardProxy.becomeFirstResponder()
        } else if !editable, keyboardShownForFocus {
            keyboardShownForFocus = false
            becomeFirstResponder()
        }
        updateKeyboardAvoidance(animated: true)
    }

    @objc private func keyboardFrameChanged(_ note: Notification) {
        guard let frame = note.userInfo?[UIResponder.keyboardFrameEndUserInfoKey] as? CGRect else { return }
        // Convert from screen space without our own transform skewing the result.
        let top = superview?.convert(frame, from: nil).minY ?? frame.minY
        keyboardTop = top < (superview?.bounds.maxY ?? bounds.maxY) - 1 ? top : nil
        updateKeyboardAvoidance(animated: true)
    }

    @objc private func keyboardWillHide(_ note: Notification) {
        keyboardTop = nil
        updateKeyboardAvoidance(animated: true)
    }

    /// Slides the stream up just enough to keep the focused field above the keyboard.
    private func updateKeyboardAvoidance(animated: Bool) {
        var offset: CGFloat = 0
        if let keyboardTop, let focusRect {
            let video = videoRect
            let fieldBottom = video.minY + min(focusRect.maxY, 1) * video.height
            let fieldTop = video.minY + max(focusRect.minY, 0) * video.height
            let needed = fieldBottom + 24 - keyboardTop
            // Never push the top of the field off the top of the screen.
            offset = max(0, min(needed, fieldTop - 24, bounds.height - keyboardTop))
        }
        let target = CGAffineTransform(translationX: 0, y: -offset)
        guard transform != target else { return }
        let apply = { self.transform = target }
        if animated {
            UIView.animate(withDuration: 0.25, delay: 0, options: [.beginFromCurrentState, .curveEaseOut], animations: apply)
        } else {
            apply()
        }
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is not supported") }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        if window != nil {
            DispatchQueue.main.async { self.becomeFirstResponder() }
        } else {
            releaseKeys()
        }
    }

    // MARK: Geometry

    private var pendingDisplayRequest: DispatchWorkItem?

    /// When the iPad rotates, reshape the Mac's virtual display to match instead of letterboxing.
    override func layoutSubviews() {
        super.layoutSubviews()
        guard let welcome, welcome.isVirtual, bounds.width > 0, bounds.height > 0 else { return }
        let viewIsLandscape = bounds.width > bounds.height
        let streamIsLandscape = welcome.width > welcome.height
        pendingDisplayRequest?.cancel()
        guard viewIsLandscape != streamIsLandscape else { return }
        let work = DispatchWorkItem { [weak self] in
            self?.session.requestDisplay(pixels: StreamSession.screenPixels())
        }
        pendingDisplayRequest = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.4, execute: work)
    }

    private var videoRect: CGRect {
        guard let welcome, welcome.width > 0, welcome.height > 0 else { return bounds }
        return AVMakeRect(aspectRatio: CGSize(width: welcome.width, height: welcome.height), insideRect: bounds)
    }

    private func normalized(_ point: CGPoint) -> CGPoint {
        let rect = videoRect
        guard rect.width > 0, rect.height > 0 else { return .zero }
        return CGPoint(x: min(max((point.x - rect.minX) / rect.width, 0), 1),
                       y: min(max((point.y - rect.minY) / rect.height, 0), 1))
    }

    /// Mac points per view point, so scrolling moves content 1:1 under your fingers.
    private var scrollScale: CGFloat {
        guard let welcome, videoRect.width > 0 else { return 1 }
        return CGFloat(welcome.pointWidth) / videoRect.width
    }

    // MARK: Gestures

    private func setUpGestures() {
        let touchTypes = [UITouch.TouchType.direct, .pencil].map { NSNumber(value: $0.rawValue) }

        let hover = UIHoverGestureRecognizer(target: self, action: #selector(handleHover(_:)))
        addGestureRecognizer(hover)

        let wheel = UIPanGestureRecognizer(target: self, action: #selector(handleScroll(_:)))
        wheel.allowedScrollTypesMask = .all
        wheel.allowedTouchTypes = []
        addGestureRecognizer(wheel)

        let tap = UITapGestureRecognizer(target: self, action: #selector(handleTap(_:)))
        tap.allowedTouchTypes = touchTypes
        addGestureRecognizer(tap)

        let twoFingerTap = UITapGestureRecognizer(target: self, action: #selector(handleTwoFingerTap(_:)))
        twoFingerTap.numberOfTouchesRequired = 2
        twoFingerTap.allowedTouchTypes = touchTypes
        addGestureRecognizer(twoFingerTap)

        let threeFingerTap = UITapGestureRecognizer(target: self, action: #selector(handleThreeFingerTap(_:)))
        threeFingerTap.numberOfTouchesRequired = 3
        threeFingerTap.allowedTouchTypes = touchTypes
        addGestureRecognizer(threeFingerTap)

        let press = UILongPressGestureRecognizer(target: self, action: #selector(handleLongPress(_:)))
        press.minimumPressDuration = 0.3
        press.allowableMovement = 12
        press.allowedTouchTypes = touchTypes
        addGestureRecognizer(press)

        let drag = UIPanGestureRecognizer(target: self, action: #selector(handleDrag(_:)))
        drag.maximumNumberOfTouches = 1
        drag.allowedTouchTypes = touchTypes
        drag.require(toFail: press)
        addGestureRecognizer(drag)

        let twoFingerPan = UIPanGestureRecognizer(target: self, action: #selector(handleScroll(_:)))
        twoFingerPan.minimumNumberOfTouches = 2
        twoFingerPan.maximumNumberOfTouches = 2
        twoFingerPan.allowedTouchTypes = touchTypes
        addGestureRecognizer(twoFingerPan)
    }

    @objc private func handleHover(_ g: UIHoverGestureRecognizer) {
        guard g.state == .began || g.state == .changed else { return }
        session.mouseMove(normalized(g.location(in: self)))
    }

    @objc private func handleScroll(_ g: UIPanGestureRecognizer) {
        let t = g.translation(in: self)
        g.setTranslation(.zero, in: self)
        guard t != .zero else { return }
        session.scroll(dx: t.x * scrollScale, dy: t.y * scrollScale)
    }

    @objc private func handleTap(_ g: UITapGestureRecognizer) {
        session.click(0, at: normalized(g.location(in: self)))
    }

    @objc private func handleTwoFingerTap(_ g: UITapGestureRecognizer) {
        session.click(1, at: normalized(g.location(in: self)))
    }

    @objc private func handleThreeFingerTap(_ g: UITapGestureRecognizer) {
        onThreeFingerTap?()
    }

    @objc private func handleLongPress(_ g: UILongPressGestureRecognizer) {
        let p = normalized(g.location(in: self))
        switch g.state {
        case .began:
            session.mouseMove(p)
            session.mouseButton(0, down: true, at: p)
        case .changed:
            session.mouseMove(p)
        case .ended, .cancelled, .failed:
            session.mouseButton(0, down: false, at: p)
        default:
            break
        }
    }

    @objc private func handleDrag(_ g: UIPanGestureRecognizer) {
        guard g.state == .began || g.state == .changed else { return }
        session.mouseMove(normalized(g.location(in: self)))
    }

    // MARK: Trackpad / mouse buttons (needs UIApplicationSupportsIndirectInputEvents)

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        for touch in touches where touch.type == .indirectPointer {
            let button: UInt8 = event?.buttonMask.contains(.secondary) == true ? 1 : 0
            pointerButton = button
            session.mouseButton(button, down: true, at: normalized(touch.location(in: self)))
        }
        super.touchesBegan(touches, with: event)
    }

    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        for touch in touches where touch.type == .indirectPointer {
            session.mouseMove(normalized(touch.location(in: self)))
        }
        super.touchesMoved(touches, with: event)
    }

    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        endPointer(touches)
        super.touchesEnded(touches, with: event)
    }

    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
        endPointer(touches)
        super.touchesCancelled(touches, with: event)
    }

    private func endPointer(_ touches: Set<UITouch>) {
        for touch in touches where touch.type == .indirectPointer {
            session.mouseButton(pointerButton ?? 0, down: false, at: normalized(touch.location(in: self)))
            pointerButton = nil
        }
    }

    /// Hide the iPad pointer; the Mac's own cursor is in the video.
    func pointerInteraction(_ interaction: UIPointerInteraction, styleFor region: UIPointerRegion) -> UIPointerStyle? {
        .hidden()
    }

    // MARK: Hardware keyboard

    override var canBecomeFirstResponder: Bool { true }

    private var currentMods: KeyMods {
        var mods = heldModifiers.values.reduce(into: KeyMods()) { $0.formUnion($1) }
        if capsLock { mods.insert(.capsLock) }
        return mods
    }

    override func pressesBegan(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        var unhandled = Set<UIPress>()
        for press in presses {
            guard let key = press.key, let code = KeyMap.macKeyCode(for: key.keyCode) else {
                unhandled.insert(press)
                continue
            }
            capsLock = key.modifierFlags.contains(.alphaShift)
            if let mod = KeyMap.modifiers[code] {
                heldModifiers[code] = mod
                session.key(code, action: .down, mods: currentMods)
            } else if code == KeyMap.capsLock {
                session.key(code, action: .down, mods: currentMods)
            } else {
                heldKeys.insert(code)
                session.key(code, action: .down, mods: currentMods)
                startRepeat(code)
            }
        }
        if !unhandled.isEmpty { super.pressesBegan(unhandled, with: event) }
    }

    override func pressesEnded(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        endPresses(presses, event: event, cancelled: false)
    }

    override func pressesCancelled(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        endPresses(presses, event: event, cancelled: true)
    }

    private func endPresses(_ presses: Set<UIPress>, event: UIPressesEvent?, cancelled: Bool) {
        var unhandled = Set<UIPress>()
        for press in presses {
            guard let key = press.key, let code = KeyMap.macKeyCode(for: key.keyCode) else {
                unhandled.insert(press)
                continue
            }
            heldModifiers.removeValue(forKey: code)
            heldKeys.remove(code)
            stopRepeat()
            session.key(code, action: .up, mods: currentMods)
        }
        guard !unhandled.isEmpty else { return }
        if cancelled {
            super.pressesCancelled(unhandled, with: event)
        } else {
            super.pressesEnded(unhandled, with: event)
        }
    }

    /// iPadOS delivers a single press for a held key, so generate autorepeat like a Mac keyboard does.
    private func startRepeat(_ code: UInt16) {
        stopRepeat()
        repeatTimer = Timer.scheduledTimer(withTimeInterval: 0.4, repeats: false) { [weak self] _ in
            guard let self, self.heldKeys.contains(code) else { return }
            self.repeatTimer = Timer.scheduledTimer(withTimeInterval: 1.0 / 30, repeats: true) { [weak self] timer in
                guard let self, self.heldKeys.contains(code) else { return timer.invalidate() }
                self.session.key(code, action: .repeatDown, mods: self.currentMods)
            }
        }
    }

    private func stopRepeat() {
        repeatTimer?.invalidate()
        repeatTimer = nil
    }

    func releaseKeys() {
        stopRepeat()
        for code in heldKeys.union(heldModifiers.keys) { session.key(code, action: .up, mods: []) }
        heldKeys.removeAll()
        heldModifiers.removeAll()
    }

    func toggleSoftwareKeyboard() {
        keyboardShownForFocus = false
        if keyboardProxy.isFirstResponder {
            becomeFirstResponder()
        } else {
            keyboardProxy.becomeFirstResponder()
        }
    }
}

/// Invisible text input used for the on-screen keyboard.
final class KeyboardProxy: UIView, UIKeyInput {
    weak var session: StreamSession?

    var autocorrectionType: UITextAutocorrectionType = .no
    var autocapitalizationType: UITextAutocapitalizationType = .none
    var spellCheckingType: UITextSpellCheckingType = .no
    var smartQuotesType: UITextSmartQuotesType = .no
    var smartDashesType: UITextSmartDashesType = .no
    var smartInsertDeleteType: UITextSmartInsertDeleteType = .no
    var keyboardType: UIKeyboardType = .asciiCapable

    override init(frame: CGRect) {
        super.init(frame: frame)
        inputAssistantItem.leadingBarButtonGroups = []
        inputAssistantItem.trailingBarButtonGroups = []
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is not supported") }

    override var canBecomeFirstResponder: Bool { true }
    var hasText: Bool { true }

    func insertText(_ text: String) {
        if text == "\n" {
            session?.tapKey(0x24)
        } else {
            session?.type(text)
        }
    }

    func deleteBackward() {
        session?.tapKey(0x33)
    }
}

final class StreamViewHandle: ObservableObject {
    weak var view: StreamUIView?
}

struct StreamViewRepresentable: UIViewRepresentable {
    let session: StreamSession
    let welcome: Welcome?
    let handle: StreamViewHandle
    let onThreeFingerTap: () -> Void

    func makeUIView(context: Context) -> StreamUIView {
        let view = StreamUIView(session: session)
        handle.view = view
        return view
    }

    func updateUIView(_ view: StreamUIView, context: Context) {
        if view.welcome != welcome {
            view.welcome = welcome
            view.setNeedsLayout()
        }
        view.onThreeFingerTap = onThreeFingerTap
    }
}

struct StreamScreen: View {
    @EnvironmentObject private var session: StreamSession
    @StateObject private var handle = StreamViewHandle()
    @State private var showToolbar = true
    @State private var showStats = false

    var body: some View {
        ZStack(alignment: .top) {
            StreamViewRepresentable(session: session, welcome: session.welcome, handle: handle) {
                withAnimation(.easeOut(duration: 0.15)) { showToolbar.toggle() }
            }
            .ignoresSafeArea()

            VStack(spacing: 8) {
                if showToolbar {
                    toolbar.transition(.move(edge: .top).combined(with: .opacity))
                } else {
                    grabber.transition(.opacity)
                }
                if showStats {
                    StatsView(stats: session.stats, welcome: session.welcome)
                        .allowsHitTesting(false)
                }
            }
            .padding(.top, 8)
        }
        .background(Color.black)
        .statusBarHidden()
        .persistentSystemOverlays(.hidden)
        .defersSystemGestures(on: .all)
        .task {
            // Stay open long enough to notice on first connect, then tuck away into the handle.
            try? await Task.sleep(for: .seconds(12))
            withAnimation { showToolbar = false }
        }
    }

    /// Always-visible grabber at the top edge; tap to open the toolbar.
    private var grabber: some View {
        Button {
            withAnimation(.easeOut(duration: 0.15)) { showToolbar = true }
        } label: {
            Capsule()
                .fill(.white.opacity(0.55))
                .frame(width: 44, height: 5)
                .shadow(color: .black.opacity(0.5), radius: 2)
                .frame(width: 88, height: 28)
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
    }

    /// Round translucent buttons, like Pinry Saver's fullscreen viewer.
    private var toolbar: some View {
        HStack(spacing: 12) {
            ToolbarCircle(symbol: "keyboard") { handle.view?.toggleSoftwareKeyboard() }
            ToolbarCircle(symbol: "gauge.with.dots.needle.50percent", active: showStats) { showStats.toggle() }
            ToolbarCircle(symbol: "xmark") { session.disconnect() }
            ToolbarCircle(symbol: "chevron.up") {
                withAnimation(.easeOut(duration: 0.15)) { showToolbar = false }
            }
        }
    }
}

private struct ToolbarCircle: View {
    let symbol: String
    var active = false
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Image(systemName: symbol)
                .font(.system(size: 20, weight: .medium))
                .foregroundColor(active ? .iframeInk : .white)
                .frame(width: 48, height: 48)
                .background(active ? Color.iframeTeal : Color.black.opacity(0.45))
                .clipShape(Circle())
        }
        .buttonStyle(.plain)
    }
}

struct StatsView: View {
    let stats: ClientStats
    let welcome: Welcome?

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            if let w = welcome {
                Text("\(w.hostName) · \(w.width)×\(w.height) \(w.codec.name) @ \(w.fps)")
            }
            if let h = stats.host {
                Text(String(format: "%.0f fps · %.1f Mbps (target %.0f)", h.fps, h.mbps, h.targetMbps))
                Text(String(format: "encode %.1f ms · decode %.1f ms", h.encodeMs, stats.decodeMs))
                Text(String(format: "net RTT %.1f ms · frame ack %.1f ms", stats.rttMs, h.latencyMs))
                if h.dropped > 0 { Text("skipped \(h.dropped) frames (congestion)") }
            } else {
                Text("waiting for stats…")
            }
        }
        .font(.system(size: 11, design: .monospaced))
        .foregroundStyle(.white)
        .padding(8)
        .background(.black.opacity(0.6), in: RoundedRectangle(cornerRadius: 8))
    }
}
