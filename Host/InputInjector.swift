import AppKit
import CoreGraphics

/// Turns client input messages into real HID-level events on the Mac.
/// Requires the Accessibility permission for the process that runs iframe-host.
final class InputInjector {
    private var bounds = CGDisplayBounds(CGMainDisplayID())
    private let source = CGEventSource(stateID: .hidSystemState)
    private var buttonsDown = Set<UInt8>()
    private var keysDown = Set<UInt16>()
    private var flags: CGEventFlags = []
    private var position = CGPoint.zero
    private var lastClick: (button: UInt8, time: TimeInterval, point: CGPoint, count: Int)?
    private var scrollRemainder = CGPoint.zero

    private static let modifierKeys: Set<UInt16> = [0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F]
    private static let arrowKeys: Set<UInt16> = [0x7B, 0x7C, 0x7D, 0x7E]
    private static let functionKeys: Set<UInt16> = [
        0x7A, 0x78, 0x63, 0x76, 0x60, 0x61, 0x62, 0x64, 0x65, 0x6D, 0x67, 0x6F,  // F1-F12
        0x69, 0x6B, 0x71, 0x6A, 0x40, 0x4F, 0x50, 0x5A,                          // F13-F20
        0x72, 0x73, 0x74, 0x75, 0x77, 0x79,                                      // help/home/pgup/fwd-del/end/pgdn
    ]

    func setDisplay(_ id: CGDirectDisplayID) {
        bounds = CGDisplayBounds(id)
    }

    var displayPointSize: CGSize { bounds.size }

    private func point(_ x: Float, _ y: Float) -> CGPoint {
        let nx = CGFloat(min(max(x, 0), 1))
        let ny = CGFloat(min(max(y, 0), 1))
        return CGPoint(x: bounds.minX + nx * (bounds.width - 1), y: bounds.minY + ny * (bounds.height - 1))
    }

    func move(x: Float, y: Float) {
        position = point(x, y)
        let type: CGEventType
        let button: CGMouseButton
        if buttonsDown.contains(0) {
            (type, button) = (.leftMouseDragged, .left)
        } else if buttonsDown.contains(1) {
            (type, button) = (.rightMouseDragged, .right)
        } else if buttonsDown.contains(2) {
            (type, button) = (.otherMouseDragged, .center)
        } else {
            (type, button) = (.mouseMoved, .left)
        }
        post(CGEvent(mouseEventSource: source, mouseType: type, mouseCursorPosition: position, mouseButton: button))
    }

    func button(_ button: UInt8, down: Bool, x: Float, y: Float) {
        position = point(x, y)
        if down {
            buttonsDown.insert(button)
            let now = ProcessInfo.processInfo.systemUptime
            if let last = lastClick, last.button == button, now - last.time < NSEvent.doubleClickInterval,
               hypot(last.point.x - position.x, last.point.y - position.y) < 6 {
                lastClick = (button, now, position, last.count + 1)
            } else {
                lastClick = (button, now, position, 1)
            }
        } else {
            buttonsDown.remove(button)
        }
        postButton(button, down: down, clickCount: lastClick?.count ?? 1)
    }

    private func postButton(_ button: UInt8, down: Bool, clickCount: Int) {
        let type: CGEventType
        let cgButton: CGMouseButton
        switch button {
        case 0: (type, cgButton) = (down ? .leftMouseDown : .leftMouseUp, .left)
        case 1: (type, cgButton) = (down ? .rightMouseDown : .rightMouseUp, .right)
        default: (type, cgButton) = (down ? .otherMouseDown : .otherMouseUp, .center)
        }
        guard let event = CGEvent(mouseEventSource: source, mouseType: type, mouseCursorPosition: position, mouseButton: cgButton) else { return }
        event.setIntegerValueField(.mouseEventClickState, value: Int64(clickCount))
        if button >= 2 { event.setIntegerValueField(.mouseEventButtonNumber, value: Int64(button)) }
        post(event)
    }

    func scroll(dx: Float, dy: Float) {
        scrollRemainder.x += CGFloat(dx)
        scrollRemainder.y += CGFloat(dy)
        let ix = Int32(scrollRemainder.x.rounded(.towardZero))
        let iy = Int32(scrollRemainder.y.rounded(.towardZero))
        guard ix != 0 || iy != 0 else { return }
        scrollRemainder.x -= CGFloat(ix)
        scrollRemainder.y -= CGFloat(iy)
        post(CGEvent(scrollWheelEvent2Source: source, units: .pixel, wheelCount: 2, wheel1: iy, wheel2: ix, wheel3: 0))
    }

    func key(_ code: UInt16, action: KeyAction, mods: KeyMods) {
        flags = Self.flags(from: mods)
        let down = action != .up
        guard let event = CGEvent(keyboardEventSource: source, virtualKey: code, keyDown: down) else { return }
        var eventFlags = flags
        if Self.modifierKeys.contains(code) {
            event.type = .flagsChanged
        } else if Self.arrowKeys.contains(code) {
            eventFlags.formUnion([.maskNumericPad, .maskSecondaryFn])
        } else if Self.functionKeys.contains(code) {
            eventFlags.insert(.maskSecondaryFn)
        }
        if action == .repeatDown { event.setIntegerValueField(.keyboardEventAutorepeat, value: 1) }
        event.flags = eventFlags
        if down { keysDown.insert(code) } else { keysDown.remove(code) }
        event.post(tap: .cghidEventTap)
    }

    /// Types arbitrary Unicode text (used by the iPad's on-screen keyboard).
    func type(_ text: String) {
        let units = Array(text.utf16)
        var index = 0
        while index < units.count {
            let chunk = Array(units[index..<min(index + 16, units.count)])
            for down in [true, false] {
                guard let event = CGEvent(keyboardEventSource: source, virtualKey: 0, keyDown: down) else { continue }
                event.keyboardSetUnicodeString(stringLength: chunk.count, unicodeString: chunk)
                event.post(tap: .cghidEventTap)
            }
            index += 16
        }
    }

    /// Releases anything the client left held down (disconnects mid-drag, mid-shortcut, ...).
    func releaseAll() {
        for button in buttonsDown { postButton(button, down: false, clickCount: 1) }
        buttonsDown.removeAll()
        flags = []
        for code in keysDown {
            guard let event = CGEvent(keyboardEventSource: source, virtualKey: code, keyDown: false) else { continue }
            if Self.modifierKeys.contains(code) { event.type = .flagsChanged }
            event.flags = []
            event.post(tap: .cghidEventTap)
        }
        keysDown.removeAll()
    }

    private func post(_ event: CGEvent?) {
        guard let event else { return }
        event.flags = flags
        event.post(tap: .cghidEventTap)
    }

    private static func flags(from mods: KeyMods) -> CGEventFlags {
        var flags: CGEventFlags = []
        if mods.contains(.shift) { flags.insert(.maskShift) }
        if mods.contains(.control) { flags.insert(.maskControl) }
        if mods.contains(.option) { flags.insert(.maskAlternate) }
        if mods.contains(.command) { flags.insert(.maskCommand) }
        if mods.contains(.capsLock) { flags.insert(.maskAlphaShift) }
        return flags
    }
}
