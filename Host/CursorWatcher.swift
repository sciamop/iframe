import AppKit

/// Tracks the Mac's current cursor shape so clients can draw the pointer locally: it then
/// moves with zero latency and never gets lost in the video. (Capture runs without the
/// cursor for those clients.)
final class CursorWatcher {
    struct Shape {
        var png: Data
        var pointWidth: Int
        var pointHeight: Int
        var hotspotX: Int
        var hotspotY: Int
    }

    var onChange: ((Shape) -> Void)?
    private var timer: DispatchSourceTimer?
    private var lastPixels: Data?
    private var lastHotspot = CGPoint(x: -1, y: -1)

    func start() {
        let timer = DispatchSource.makeTimerSource(queue: .main)
        timer.schedule(deadline: .now(), repeating: .milliseconds(33))
        timer.setEventHandler { [weak self] in self?.poll() }
        timer.resume()
        self.timer = timer
    }

    func stop() {
        timer?.cancel()
        timer = nil
    }

    /// Forces the next poll to report the shape again (e.g. after the stream restarts).
    func resend() {
        DispatchQueue.main.async { self.lastPixels = nil }
    }

    private func poll() {
        guard let cursor = NSCursor.currentSystem else { return }
        let image = cursor.image
        let size = image.size
        guard size.width > 0, size.height > 0, size.width <= 256, size.height <= 256 else { return }

        // Render at 2x so it stays crisp on high-density client screens.
        guard let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: Int(size.width * 2), pixelsHigh: Int(size.height * 2),
                                         bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
                                         colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0) else { return }
        rep.size = size
        NSGraphicsContext.saveGraphicsState()
        NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
        image.draw(in: NSRect(origin: .zero, size: size))
        NSGraphicsContext.restoreGraphicsState()
        guard let raw = rep.bitmapData else { return }
        let pixels = Data(bytes: raw, count: rep.bytesPerRow * rep.pixelsHigh)
        let hotspot = cursor.hotSpot
        guard pixels != lastPixels || hotspot != lastHotspot else { return }
        lastPixels = pixels
        lastHotspot = hotspot

        guard let png = rep.representation(using: .png, properties: [:]) else { return }
        onChange?(Shape(png: png, pointWidth: Int(size.width.rounded()), pointHeight: Int(size.height.rounded()),
                        hotspotX: Int(hotspot.x.rounded()), hotspotY: Int(hotspot.y.rounded())))
    }
}
