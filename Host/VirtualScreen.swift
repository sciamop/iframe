import CGVirtualDisplayPrivate
import CoreGraphics
import Foundation

/// A virtual monitor shaped exactly like the client's screen, so the stream is pixel-for-pixel
/// with no letterboxing or scaling, at the client's refresh rate (120 Hz on ProMotion iPads).
///
/// It lives for the life of iframe-host and is reshaped in place (rotation, density changes),
/// so windows don't get shuffled between displays on every reconnect. macOS removes it when
/// the process exits.
final class VirtualScreen {
    private static var shared: VirtualScreen?
    private static let queue = DispatchQueue(label: "iframe.virtual-display")

    let display: CGVirtualDisplay
    private let maxPixels: Int
    private(set) var request: DisplayRequest
    private(set) var refreshRate: Double

    var displayID: CGDirectDisplayID { display.displayID }

    /// Pixel size the stream is captured at (always the client's native resolution).
    var captureSize: (width: Int, height: Int) { (request.width & ~1, request.height & ~1) }

    private init(display: CGVirtualDisplay, maxPixels: Int, request: DisplayRequest, refreshRate: Double) {
        self.display = display
        self.maxPixels = maxPixels
        self.request = request
        self.refreshRate = refreshRate
    }

    /// Largest backing size (pixels per side) a display may use. The display is created at this
    /// size up front so any client shape can be applied in place: tearing a virtual display down
    /// and recreating it races with macOS reusing the same display ID.
    static let maxBackingPixels = 8192

    /// Mac point size the display will have for `request` (what ScreenCaptureKit reports).
    static func pointSize(for request: DisplayRequest) -> (width: Int, height: Int) {
        let size = backingPixels(for: request)
        return (size.pointWidth, size.pointHeight)
    }

    /// Returns a virtual display matching `request`, reusing and reshaping the existing one when possible.
    /// Returns nil if the display didn't come online in the requested shape.
    static func obtain(for request: DisplayRequest, refreshRate: Double) -> VirtualScreen? {
        let screen = DispatchQueue.main.sync { () -> VirtualScreen? in
            let backing = backingPixels(for: request)
            let needed = max(backing.width, backing.height)
            if let current = shared, needed <= current.maxPixels {
                if current.request == request, current.refreshRate == refreshRate { return current }
                if current.apply(request, refreshRate: refreshRate) { return current }
            }
            if let old = shared {
                let oldID = old.displayID
                shared = nil  // releasing the old display removes it
                waitUntilGone(oldID)
            }
            shared = create(for: request, refreshRate: refreshRate)
            return shared
        }
        // Check readiness on the main thread too: CoreGraphics only refreshes its view of a
        // reshaped display there; background-thread queries keep returning the old mode.
        guard let screen, DispatchQueue.main.sync(execute: { screen.waitUntilReady() }) else { return nil }
        return screen
    }

    private static func waitUntilGone(_ id: CGDirectDisplayID) {
        for _ in 0..<60 where CGDisplayIsOnline(id) != 0 {
            Thread.sleep(forTimeInterval: 0.05)
        }
    }

    /// A new or reshaped display takes a moment to come online with its mode; capture and
    /// frame-rate decisions must wait for it. Main thread only.
    private func waitUntilReady() -> Bool {
        let size = Self.backingPixels(for: request)
        var ready = false
        for _ in 0..<100 {
            if let mode = CGDisplayCopyDisplayMode(displayID), mode.width == size.pointWidth,
               mode.height == size.pointHeight, mode.pixelWidth == size.width, mode.refreshRate > 0 {
                ready = true
                break
            }
            Thread.sleep(forTimeInterval: 0.05)
        }
        let mode = CGDisplayCopyDisplayMode(displayID)
        hostLog("virtual display\(ready ? "" : " NOT READY"): \(mode?.width ?? 0)x\(mode?.height ?? 0) pt, \(mode?.pixelWidth ?? 0)x\(mode?.pixelHeight ?? 0) px @ \(Int(mode?.refreshRate ?? 0)) Hz (display \(displayID))")
        return ready
    }

    private static func create(for request: DisplayRequest, refreshRate: Double) -> VirtualScreen? {
        // Square, generous max size: any orientation or window shape can then be applied in place.
        let maxPixels = maxBackingPixels
        let descriptor = CGVirtualDisplayDescriptor()
        descriptor.setDispatchQueue(queue)
        descriptor.name = "iFrame Display"
        descriptor.maxPixelsWide = UInt32(maxPixels)
        descriptor.maxPixelsHigh = UInt32(maxPixels)
        // A small physical size (very high ppi) makes macOS default to the HiDPI variant of each
        // mode. We must never select modes ourselves: after any CGDisplaySetDisplayMode or
        // CGConfigureDisplayWithDisplayMode, macOS ignores later applySettings reshapes.
        descriptor.sizeInMillimeters = CGSize(width: 263, height: 263)
        descriptor.vendorID = 0x676C   // "gl"
        descriptor.productID = 0x6964  // "id"
        descriptor.serialNum = 1
        guard let display = CGVirtualDisplay(descriptor: descriptor) else {
            hostLog("virtual display: creation failed")
            return nil
        }
        let screen = VirtualScreen(display: display, maxPixels: maxPixels, request: request, refreshRate: refreshRate)
        guard screen.apply(request, refreshRate: refreshRate) else {
            hostLog("virtual display: could not apply mode")
            return nil
        }
        return screen
    }

    /// Mac points and backing pixels for a request. HiDPI modes always render at 2x points.
    private static func backingPixels(for request: DisplayRequest) -> (width: Int, height: Int, pointWidth: Int, pointHeight: Int) {
        var scale = min(max(request.uiScale, 1), 2)
        // HiDPI renders at 2x points; keep that within the display's maximum.
        let longest = Double(max(request.width, request.height))
        scale = max(scale, longest * 2 / Double(maxBackingPixels))
        let pw = Int((Double(request.width) / scale).rounded()) & ~1
        let ph = Int((Double(request.height) / scale).rounded()) & ~1
        return (pw * 2, ph * 2, pw, ph)
    }

    private func apply(_ request: DisplayRequest, refreshRate: Double) -> Bool {
        let size = Self.backingPixels(for: request)
        let settings = CGVirtualDisplaySettings()
        settings.hiDPI = 1
        var modes = [CGVirtualDisplayMode(width: UInt32(size.pointWidth), height: UInt32(size.pointHeight), refreshRate: refreshRate)]
        if refreshRate != 60 {
            modes.append(CGVirtualDisplayMode(width: UInt32(size.pointWidth), height: UInt32(size.pointHeight), refreshRate: 60))
        }
        settings.modes = modes
        guard display.apply(settings) else { return false }
        self.request = request
        self.refreshRate = refreshRate
        return true
    }
}
