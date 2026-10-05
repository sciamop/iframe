import ApplicationServices
import CoreGraphics

/// Polls the Accessibility API for the focused UI element so the client can raise its
/// on-screen keyboard when a text field is focused, and keep that field visible above it.
enum FocusWatcher {
    struct Focus: Equatable {
        var editable: Bool
        var frame: CGRect  // global display points
        static let none = Focus(editable: false, frame: .zero)
    }

    private static let textRoles: Set<String> = ["AXTextField", "AXTextArea", "AXComboBox", "AXSearchField"]
    private static let system: AXUIElement = {
        let element = AXUIElementCreateSystemWide()
        AXUIElementSetMessagingTimeout(element, 0.1)  // never stall on a hung app
        return element
    }()

    static func current() -> Focus {
        guard let element: AXUIElement = attribute(system, kAXFocusedUIElementAttribute),
              let role: String = attribute(element, kAXRoleAttribute) else { return .none }
        var editable = textRoles.contains(role)
        if editable {
            // Static, read-only text areas (e.g. labels in some apps) aren't worth a keyboard.
            var settable: DarwinBoolean = false
            if AXUIElementIsAttributeSettable(element, kAXValueAttribute as CFString, &settable) == .success {
                editable = settable.boolValue || role == "AXTextArea"
            }
        }
        guard editable else { return .none }
        var frame = CGRect.zero
        if let position: AXValue = attribute(element, kAXPositionAttribute),
           let size: AXValue = attribute(element, kAXSizeAttribute) {
            AXValueGetValue(position, .cgPoint, &frame.origin)
            AXValueGetValue(size, .cgSize, &frame.size)
        }
        return Focus(editable: true, frame: frame)
    }

    private static func attribute<T>(_ element: AXUIElement, _ name: String) -> T? {
        var value: CFTypeRef?
        guard AXUIElementCopyAttributeValue(element, name as CFString, &value) == .success, let value else { return nil }
        return value as? T
    }
}
