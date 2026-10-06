import AVFoundation
import Combine
import ImageIO
import Network
import VideoToolbox

/// The Mac's cursor shape. Hotspot and size are in Mac points; the image is 2x.
struct MacCursor {
    var image: CGImage
    var hotspot: CGPoint
    var size: CGSize
}

struct ClientStats: Equatable {
    var host: HostStats?
    var decodeFPS = 0
    var decodeMs = 0.0
    var rttMs = 0.0
}

/// Owns the connection to iframe-host: receives and decodes video straight into the display
/// layer's renderer on one high-priority queue, and sends input. Shared by the iPad and Mac apps.
final class StreamSession: ObservableObject {
    enum Phase: Equatable {
        case idle
        case connecting
        case streaming
        case failed(String)
    }

    @Published private(set) var phase: Phase = .idle
    @Published private(set) var stats = ClientStats()
    @Published private(set) var welcome: Welcome?
    private(set) var hostLabel = ""

    /// Called on the main thread when a Mac text field gains (true) or loses focus.
    /// The rect is normalized over the stream.
    var onTextFocus: ((Bool, CGRect) -> Void)?

    /// Called on the main thread with the Mac's cursor shape whenever it changes.
    var onCursor: ((MacCursor) -> Void)?

    // Everything below is confined to `queue`.
    private let queue = DispatchQueue(label: "iframe.client", qos: .userInteractive)
    private var channel: MessageChannel?
    private let decoder = VideoDecoder()
    private var renderer: AVSampleBufferVideoRenderer?
    private var displayFormat: CMVideoFormatDescription?
    private var timer: DispatchSourceTimer?
    private var lastKeyframeRequest: UInt64 = 0
    private var frameCount = 0
    private var decodeNanos: UInt64 = 0
    private var rttMs = 0.0
    private var hostStats: HostStats?

    init() {
        decoder.onFrame = { [weak self] pixelBuffer in self?.display(pixelBuffer) }
    }

    /// AVSampleBufferVideoRenderer is safe to feed from any thread, so decoded frames go
    /// straight to it from the network queue without a main-thread hop.
    func attach(renderer: AVSampleBufferVideoRenderer) {
        queue.async { self.renderer = renderer }
    }

    // MARK: Connection

    /// Mac points per client pixel choice (see DisplayRequest). 0 = use the Mac's own display.
    private var uiScale: Double = 2

    /// `pixels` is the client's screen (or window) size in pixels.
    func connect(to endpoint: NWEndpoint, pin: String, label: String, deviceName: String, maxFPS: Int,
                 pixels: CGSize, uiScale: Double) {
        self.uiScale = uiScale
        hostLabel = label
        phase = .connecting
        stats = ClientStats()
        welcome = nil
        let hello = Hello(
            version: IFrame.protocolVersion,
            pin: pin.trimmingCharacters(in: .whitespaces),
            name: deviceName,
            supportsHEVC: VTIsHardwareDecodeSupported(kCMVideoCodecType_HEVC),
            maxFPS: maxFPS,
            display: DisplayRequest(width: Int(pixels.width), height: Int(pixels.height), uiScale: uiScale),
            localCursor: true)

        queue.async { [self] in
            teardown()
            let channel = MessageChannel(connection: NWConnection(to: endpoint, using: MessageChannel.parameters()), queue: queue)
            self.channel = channel
            channel.onStateChange = { [weak self, weak channel] state in
                guard let self, let channel, channel === self.channel else { return }
                switch state {
                case .ready:
                    channel.send(.hello, json: hello)
                    self.startTimer()
                case .waiting(let error):
                    self.fail("Can't reach \(label): \(error.localizedDescription)")
                case .failed(let error):
                    self.fail("Connection failed: \(error.localizedDescription)")
                case .cancelled:
                    self.fail("Disconnected from \(label).")
                default:
                    break
                }
            }
            channel.onMessage = { [weak self, weak channel] type, data in
                guard let self, let channel, channel === self.channel else { return }
                self.handle(type, data)
            }
            channel.start()
        }
    }

    /// Asks the host to reshape its virtual display, e.g. after the iPad rotates or the Mac window resizes.
    /// Pass `uiScale` to change density too.
    func requestDisplay(pixels: CGSize, uiScale newScale: Double? = nil) {
        if let newScale { uiScale = newScale }
        guard uiScale > 0, let data = try? JSONEncoder().encode(
            DisplayRequest(width: Int(pixels.width), height: Int(pixels.height), uiScale: uiScale)) else { return }
        send(.display, data)
    }

    func disconnect() {
        phase = .idle
        queue.async { self.teardown() }
    }

    private func fail(_ message: String) {
        teardown()
        DispatchQueue.main.async { self.phase = .failed(message) }
    }

    private func teardown() {
        timer?.cancel()
        timer = nil
        let old = channel
        channel = nil
        old?.cancel()
        decoder.invalidate()
        displayFormat = nil
        renderer?.flush(removingDisplayedImage: true, completionHandler: nil)
    }

    // MARK: Incoming

    private func handle(_ type: MsgType, _ data: Data) {
        switch type {
        case .welcome:
            guard let welcome = try? JSONDecoder().decode(Welcome.self, from: data) else { return }
            DispatchQueue.main.async {
                self.welcome = welcome
                self.phase = .streaming
            }
        case .authFailed:
            fail("Wrong PIN. Use the PIN printed by iframe-host.")
        case .format:
            guard let (codec, sets) = Wire.parseFormat(data) else { return }
            if !decoder.setFormat(codec: codec, parameterSets: sets) { requestKeyframeThrottled() }
        case .frame:
            guard let frame = Wire.parseFrame(data) else { return }
            let start = nowNanos()
            let ok = decoder.decode(frame.payload)
            let elapsed = nowNanos() - start
            channel?.send(.ack, Wire.ack(id: frame.id, decodeMicros: UInt32(min(elapsed / 1000, UInt64(UInt32.max)))))
            if ok {
                frameCount += 1
                decodeNanos += elapsed
            } else {
                requestKeyframeThrottled()
            }
        case .textFocus:
            var r = ByteReader(data)
            guard let editable = r.u8(), let x = r.f32(), let y = r.f32(), let w = r.f32(), let h = r.f32() else { return }
            let rect = CGRect(x: CGFloat(x), y: CGFloat(y), width: CGFloat(w), height: CGFloat(h))
            DispatchQueue.main.async { self.onTextFocus?(editable != 0, rect) }
        case .cursor:
            var r = ByteReader(data)
            guard let hx = r.u16(), let hy = r.u16(), let w = r.u16(), let h = r.u16(),
                  let source = CGImageSourceCreateWithData(r.rest() as CFData, nil),
                  let image = CGImageSourceCreateImageAtIndex(source, 0, nil) else { return }
            let cursor = MacCursor(image: image, hotspot: CGPoint(x: Int(hx), y: Int(hy)),
                                   size: CGSize(width: Int(w), height: Int(h)))
            DispatchQueue.main.async { self.onCursor?(cursor) }
        case .stats:
            hostStats = try? JSONDecoder().decode(HostStats.self, from: data)
        case .pong:
            var r = ByteReader(data)
            if let sent = r.u64() { rttMs = Double(nowNanos() - sent) / 1_000_000 }
        default:
            break
        }
    }

    private func display(_ pixelBuffer: CVPixelBuffer) {
        guard let renderer else { return }
        if renderer.status == .failed { renderer.flush() }
        if displayFormat == nil || !CMVideoFormatDescriptionMatchesImageBuffer(displayFormat!, imageBuffer: pixelBuffer) {
            CMVideoFormatDescriptionCreateForImageBuffer(allocator: kCFAllocatorDefault, imageBuffer: pixelBuffer,
                                                         formatDescriptionOut: &displayFormat)
        }
        guard let displayFormat else { return }
        var timing = CMSampleTimingInfo(duration: .invalid,
                                        presentationTimeStamp: CMClockGetTime(CMClockGetHostTimeClock()),
                                        decodeTimeStamp: .invalid)
        var sample: CMSampleBuffer?
        CMSampleBufferCreateReadyWithImageBuffer(allocator: kCFAllocatorDefault, imageBuffer: pixelBuffer,
                                                 formatDescription: displayFormat, sampleTiming: &timing,
                                                 sampleBufferOut: &sample)
        guard let sample else { return }
        if let attachments = CMSampleBufferGetSampleAttachmentsArray(sample, createIfNecessary: true),
           CFArrayGetCount(attachments) > 0 {
            let dict = unsafeBitCast(CFArrayGetValueAtIndex(attachments, 0), to: CFMutableDictionary.self)
            CFDictionarySetValue(dict,
                                 Unmanaged.passUnretained(kCMSampleAttachmentKey_DisplayImmediately).toOpaque(),
                                 Unmanaged.passUnretained(kCFBooleanTrue).toOpaque())
        }
        renderer.enqueue(sample)
    }

    private func requestKeyframeThrottled() {
        let now = nowNanos()
        guard now - lastKeyframeRequest > 250_000_000 else { return }
        lastKeyframeRequest = now
        channel?.send(.requestKeyframe)
    }

    private func startTimer() {
        let timer = DispatchSource.makeTimerSource(queue: queue)
        timer.schedule(deadline: .now() + 0.5, repeating: 1)
        timer.setEventHandler { [weak self] in
            guard let self else { return }
            var w = ByteWriter()
            w.u64(nowNanos())
            self.channel?.send(.ping, w.data)
            let stats = ClientStats(
                host: self.hostStats,
                decodeFPS: self.frameCount,
                decodeMs: self.frameCount > 0 ? Double(self.decodeNanos) / Double(self.frameCount) / 1_000_000 : 0,
                rttMs: self.rttMs)
            self.frameCount = 0
            self.decodeNanos = 0
            DispatchQueue.main.async { self.stats = stats }
        }
        timer.resume()
        self.timer = timer
    }

    // MARK: Input (call from any thread; positions are normalized 0...1 over the video)

    private func send(_ type: MsgType, _ payload: Data = Data()) {
        queue.async { self.channel?.send(type, payload) }
    }

    func mouseMove(_ p: CGPoint) {
        var w = ByteWriter()
        w.f32(Float(p.x))
        w.f32(Float(p.y))
        send(.mouseMove, w.data)
    }

    func mouseButton(_ button: UInt8, down: Bool, at p: CGPoint) {
        var w = ByteWriter()
        w.u8(button)
        w.u8(down ? 1 : 0)
        w.f32(Float(p.x))
        w.f32(Float(p.y))
        send(.mouseButton, w.data)
    }

    func click(_ button: UInt8, at p: CGPoint) {
        mouseMove(p)
        mouseButton(button, down: true, at: p)
        mouseButton(button, down: false, at: p)
    }

    func scroll(dx: CGFloat, dy: CGFloat) {
        var w = ByteWriter()
        w.f32(Float(dx))
        w.f32(Float(dy))
        send(.scroll, w.data)
    }

    func key(_ code: UInt16, action: KeyAction, mods: KeyMods) {
        var w = ByteWriter()
        w.u16(code)
        w.u8(action.rawValue)
        w.u32(mods.rawValue)
        send(.key, w.data)
    }

    func requestKeyframe() {
        send(.requestKeyframe)
    }

    func tapKey(_ code: UInt16) {
        key(code, action: .down, mods: [])
        key(code, action: .up, mods: [])
    }

    func type(_ text: String) {
        send(.text, Data(text.utf8))
    }
}
