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

    /// Returns a virtual display matching `request`, reusing and reshaping the existing one when possible.
    static func obtain(for request: DisplayRequest, refreshRate: Double) -> VirtualScreen? {
        let screen = DispatchQueue.main.sync { () -> VirtualScreen? in
            let backing = backingPixels(for: request)
            let needed = max(backing.width, backing.height)
            if let current = shared, needed <= current.maxPixels {
                if current.request == request, current.refreshRate == refreshRate { return current }
                if current.apply(request, refreshRate: refreshRate) { return current }
            }
            shared = nil  // releasing the old display removes it
            shared = create(for: request, refreshRate: refreshRate)
            return shared
        }
        screen?.waitUntilReady()
        return screen
    }

    /// A new or reshaped display takes a moment to come online with its mode; capture and
    /// frame-rate decisions must wait for it. Called off the main thread.
    private func waitUntilReady() {
        let size = Self.backingPixels(for: request)
        for _ in 0..<60 {
            if let mode = CGDisplayCopyDisplayMode(displayID), mode.width == size.pointWidth, mode.refreshRate > 0 { break }
            selectMode(pointWidth: size.pointWidth, pixelWidth: size.width, refreshRate: refreshRate)
            Thread.sleep(forTimeInterval: 0.05)
        }
        let mode = CGDisplayCopyDisplayMode(displayID)
        hostLog("virtual display: \(mode?.width ?? 0)x\(mode?.height ?? 0) pt, \(mode?.pixelWidth ?? 0)x\(mode?.pixelHeight ?? 0) px @ \(Int(mode?.refreshRate ?? 0)) Hz (display \(displayID))")
    }

    private static func create(for request: DisplayRequest, refreshRate: Double) -> VirtualScreen? {
        let backing = backingPixels(for: request)
        // Square max size so the same display can rotate between landscape and portrait.
        let maxPixels = max(backing.width, backing.height)
        let descriptor = CGVirtualDisplayDescriptor()
        descriptor.setDispatchQueue(queue)
        descriptor.name = "iFrame Display"
        descriptor.maxPixelsWide = UInt32(maxPixels)
        descriptor.maxPixelsHigh = UInt32(maxPixels)
        // iPad Pro panels are 264 ppi; physical size drives macOS's default scaling choices.
        let mm = Double(maxPixels) * 25.4 / 264
        descriptor.sizeInMillimeters = CGSize(width: mm, height: mm)
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
        let scale = min(max(request.uiScale, 1), 2)
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
        selectMode(pointWidth: size.pointWidth, pixelWidth: size.width, refreshRate: refreshRate)
        return true
    }

    /// macOS usually picks the right mode itself; make sure it's the HiDPI one at full refresh.
    private func selectMode(pointWidth: Int, pixelWidth: Int, refreshRate: Double) {
        let options = [kCGDisplayShowDuplicateLowResolutionModes: true] as CFDictionary
        if let current = CGDisplayCopyDisplayMode(displayID),
           current.width == pointWidth, current.pixelWidth == pixelWidth, current.refreshRate == refreshRate { return }
        let modes = CGDisplayCopyAllDisplayModes(displayID, options) as? [CGDisplayMode] ?? []
        if let match = modes.first(where: { $0.width == pointWidth && $0.pixelWidth == pixelWidth && $0.refreshRate == refreshRate }) {
            CGDisplaySetDisplayMode(displayID, match, nil)
        }
    }
}
