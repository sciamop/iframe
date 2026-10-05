import ApplicationServices
import Foundation
import IOKit.pwr_mgt
import Network
import ScreenCaptureKit

struct HostConfig {
    var port: UInt16 = IFrame.defaultPort
    var fps = 120
    var mbps: Double?
    var codec: VideoCodec?
    var displayIndex = 0
    var maxInflight = 3
    var pin = ""
    var virtualDisplay = true
}

/// Accepts clients, authenticates them by PIN and runs one active stream at a time.
final class HostServer {
    let config: HostConfig
    let injector = InputInjector()
    private let queue = DispatchQueue(label: "iframe.server", qos: .userInteractive)
    private var listener: NWListener?
    private var sessions: [ObjectIdentifier: ClientSession] = [:]
    private var active: ClientSession?
    private var failedAttempts = 0

    init(config: HostConfig) {
        self.config = config
    }

    func start() throws {
        guard let port = NWEndpoint.Port(rawValue: config.port) else { throw HostError("invalid port") }
        let listener = try NWListener(using: MessageChannel.parameters(), on: port)
        listener.service = NWListener.Service(name: Host.current().localizedName ?? "Mac", type: IFrame.serviceType)
        listener.stateUpdateHandler = { state in
            switch state {
            case .failed(let error):
                hostLog("listener failed: \(error)")
                exit(1)
            default:
                break
            }
        }
        listener.newConnectionHandler = { [weak self] connection in self?.accept(connection) }
        listener.start(queue: queue)
        self.listener = listener
    }

    func shutdown() {
        queue.sync {
            sessions.values.forEach { $0.close(reason: "host shutting down") }
            injector.releaseAll()
        }
    }

    private func accept(_ connection: NWConnection) {
        accept(transport: MessageChannel(connection: connection, queue: queue))
    }

    /// Browser clients arrive from the web server's queue.
    func accept(webSocket: WebSocketTransport) {
        queue.async { self.accept(transport: webSocket) }
    }

    private func accept(transport: MessageTransport) {
        let session = ClientSession(transport: transport, server: self, queue: queue)
        sessions[ObjectIdentifier(session)] = session
        session.start()
    }

    /// Server queue.
    func authenticate(_ session: ClientSession, hello: Hello) -> Bool {
        guard hello.version == IFrame.protocolVersion, constantTimeEquals(hello.pin, config.pin) else {
            failedAttempts += 1
            return false
        }
        failedAttempts = 0
        if let old = active, old !== session { old.close(reason: "replaced by \(hello.name)") }
        active = session
        return true
    }

    /// Slows down PIN guessing.
    var authFailureDelay: TimeInterval { min(Double(failedAttempts) * 0.5, 10) }

    /// Server queue.
    func removed(_ session: ClientSession) {
        sessions.removeValue(forKey: ObjectIdentifier(session))
        if active === session {
            active = nil
            injector.releaseAll()
        }
    }

    private func constantTimeEquals(_ a: String, _ b: String) -> Bool {
        let x = Array(a.utf8), y = Array(b.utf8)
        guard x.count == y.count else { return false }
        return zip(x, y).reduce(0) { $0 | ($1.0 ^ $1.1) } == 0
    }
}

final class ClientSession {
    private let channel: MessageTransport
    private unowned let server: HostServer
    private let queue: DispatchQueue
    private var streamer: Streamer?
    private var authenticated = false
    private var closed = false
    private var name = "client"
    private var sleepAssertion: IOPMAssertionID = 0
    private var focusTimer: DispatchSourceTimer?
    private var lastFocus = FocusWatcher.Focus.none
    private var streamDisplay: CGDirectDisplayID?
    private var cursorWatcher: CursorWatcher?
    private var inputCounts: [String: Int] = [:]
    private var lastInputLog = Date()

    init(transport: MessageTransport, server: HostServer, queue: DispatchQueue) {
        self.channel = transport
        self.server = server
        self.queue = queue
        name = transport.peerDescription
    }

    /// Transport callbacks may arrive on other queues; all session state lives on `queue`.
    func start() {
        channel.onClosed = { [weak self] in self?.queue.async { self?.teardown() } }
        channel.onMessage = { [weak self] type, data in self?.queue.async { self?.handle(type, data) } }
        channel.start()
    }

    /// Server queue.
    func close(reason: String) {
        guard !closed else { return }
        hostLog("closing \(name): \(reason)")
        channel.cancel()
        teardown()
    }

    private func teardown() {
        guard !closed else { return }
        closed = true
        streamGeneration += 1
        streamer?.stop()
        streamer = nil
        focusTimer?.cancel()
        focusTimer = nil
        cursorWatcher?.stop()
        cursorWatcher = nil
        if sleepAssertion != 0 {
            IOPMAssertionRelease(sleepAssertion)
            sleepAssertion = 0
        }
        if authenticated { hostLog("\(name) disconnected") }
        server.removed(self)
    }

    private func handle(_ type: MsgType, _ data: Data) {
        guard authenticated else {
            if type == .hello, let hello = try? JSONDecoder().decode(Hello.self, from: data) {
                handleHello(hello)
            } else {
                close(reason: "unexpected message before hello")
            }
            return
        }
        var r = ByteReader(data)
        let input = server.injector
        countInput(type, data)
        switch type {
        case .mouseMove:
            if let x = r.f32(), let y = r.f32() { input.move(x: x, y: y) }
        case .mouseButton:
            if let b = r.u8(), let down = r.u8(), let x = r.f32(), let y = r.f32() {
                input.button(b, down: down != 0, x: x, y: y)
            }
        case .scroll:
            if let dx = r.f32(), let dy = r.f32() { input.scroll(dx: dx, dy: dy) }
        case .key:
            if let code = r.u16(), let raw = r.u8(), let action = KeyAction(rawValue: raw), let mods = r.u32() {
                input.key(code, action: action, mods: KeyMods(rawValue: mods))
            }
        case .text:
            if let text = String(data: data, encoding: .utf8) { input.type(text) }
        case .requestKeyframe:
            streamer?.requestKeyframe()
        case .ping:
            channel.sendMessage(.pong, data)
        case .ack:
            if let id = r.u32() { streamer?.ack(id) }
        case .display:
            if let request = try? JSONDecoder().decode(DisplayRequest.self, from: data) {
                hostLog("\(name) requested display \(request.width)x\(request.height) @ scale \(request.uiScale)")
                restartStream(display: request)
            }
        default:
            break
        }
    }

    private func handleHello(_ hello: Hello) {
        let peer = name
        name = "\(hello.name) (\(peer))"
        guard server.authenticate(self, hello: hello) else {
            hostLog("rejected \(name): wrong PIN or protocol version")
            queue.asyncAfter(deadline: .now() + server.authFailureDelay) { [weak self] in
                guard let self else { return }
                self.channel.sendMessage(.authFailed, Data()) { _ in self.queue.async { self.close(reason: "auth failed") } }
            }
            return
        }
        authenticated = true
        hostLog("\(name) connected")

        // Wake the display if it's asleep, and keep it awake while someone is watching.
        var activity: IOPMAssertionID = 0
        IOPMAssertionDeclareUserActivity("iFrame remote session" as CFString, kIOPMUserActiveLocal, &activity)
        IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleDisplaySleep as CFString,
                                    IOPMAssertionLevel(kIOPMAssertionLevelOn),
                                    "iFrame remote session" as CFString, &sleepAssertion)

        self.hello = hello
        restartStream(display: hello.display)
        startFocusWatcher()
        if hello.localCursor == true {
            let watcher = CursorWatcher()
            watcher.onChange = { [weak self] shape in
                var w = ByteWriter(capacity: shape.png.count + 8)
                w.u16(UInt16(clamping: shape.hotspotX))
                w.u16(UInt16(clamping: shape.hotspotY))
                w.u16(UInt16(clamping: shape.pointWidth))
                w.u16(UInt16(clamping: shape.pointHeight))
                w.bytes(shape.png)
                self?.channel.sendMessage(.cursor, w.data)
            }
            watcher.start()
            cursorWatcher = watcher
        }
    }

    /// Periodic summary of input received, so input problems show up in the log.
    private func countInput(_ type: MsgType, _ data: Data) {
        let label: String
        switch type {
        case .mouseMove: label = "moves"
        case .mouseButton: label = "clicks"
        case .scroll: label = "scrolls"
        case .key: label = "keys"
        case .text: label = "text"
        default: return
        }
        inputCounts[label, default: 0] += 1
        if type == .key, data.count >= 3 {
            inputCounts["last key 0x" + String(Int(data[data.startIndex]) << 8 | Int(data[data.startIndex + 1]), radix: 16)] = 0
        }
        guard Date().timeIntervalSince(lastInputLog) > 5 else { return }
        let summary = inputCounts.sorted { $0.key < $1.key }
            .map { $0.value > 0 ? "\($0.key) \($0.value)" : $0.key }.joined(separator: ", ")
        hostLog("input from \(name): \(summary)\(AXIsProcessTrusted() ? "" : " — NOT INJECTED: Accessibility permission missing")")
        inputCounts = [:]
        lastInputLog = Date()
    }

    private var hello: Hello?
    private var streamGeneration = 0

    /// Server queue. (Re)starts the stream for a display request, e.g. after the iPad rotates.
    private func restartStream(display request: DisplayRequest?) {
        streamGeneration += 1
        let generation = streamGeneration
        streamer?.stop()
        streamer = nil
        guard let hello else { return }
        Task { await self.startStreaming(hello, request: request, generation: generation) }
    }

    private func startStreaming(_ hello: Hello, request: DisplayRequest?, generation: Int) async {
        do {
            let fpsLimit = max(24, min(server.config.fps, hello.maxFPS))
            var virtual: VirtualScreen?
            if server.config.virtualDisplay, let request, request.uiScale > 0, request.width >= 640, request.height >= 480 {
                virtual = VirtualScreen.obtain(for: request, refreshRate: fpsLimit >= 100 ? 120 : 60)
                if virtual == nil { hostLog("falling back to the existing display") }
            }

            let expected = virtual.map { VirtualScreen.pointSize(for: $0.request) }
            let display = try await findDisplay(id: virtual?.displayID, expectedPoints: expected)
            let mode = CGDisplayCopyDisplayMode(display.displayID)
            let refresh = mode?.refreshRate ?? 60
            let fps = min(fpsLimit, refresh > 0 ? Int(refresh.rounded()) : 60)
            let codec = server.config.codec ?? (hello.supportsHEVC ? .hevc : .h264)
            let capture = virtual?.captureSize
            let streamWidth: Int = capture?.width ?? mode?.pixelWidth ?? 1920
            let streamHeight: Int = capture?.height ?? mode?.pixelHeight ?? 1080
            let pixelCount = Double(streamWidth * streamHeight)
            // ~0.1 bits/pixel at 60 fps for HEVC (more for H.264), scaled sublinearly with frame rate:
            // at higher rates each frame differs less from the last.
            let bitsPerPixel = (codec == .hevc ? 0.1 : 0.15) * (60.0 / Double(fps)).squareRoot()
            let autoMbps = pixelCount * Double(fps) * bitsPerPixel / 1_000_000
            let startMbps = server.config.mbps ?? min(max(autoMbps, 8), 120)
            let config = Streamer.Config(
                fps: fps,
                bitrate: Int(startMbps * 1_000_000),
                minBitrate: 2_000_000,
                maxBitrate: Int(startMbps * 1.5 * 1_000_000),
                maxInflight: server.config.maxInflight)

            let streamer = try Streamer(display: display, codec: codec, config: config, captureSize: capture,
                                        showsCursor: hello.localCursor != true)
            streamer.onFormat = { [weak self] codec, sets in
                self?.channel.sendMessage(.format, Wire.format(codec: codec, parameterSets: sets))
            }
            streamer.onFrame = { [weak self] id, pts, keyframe, data in
                self?.channel.sendMessage(.frame, Wire.frame(id: id, pts: pts, keyframe: keyframe, payload: data))
            }
            streamer.onStats = { [weak self] stats in self?.channel.sendMessage(.stats, json: stats) }
            streamer.onStop = { [weak self] error in
                self?.queue.async { self?.close(reason: "capture stopped: \(error?.localizedDescription ?? "unknown")") }
            }

            let proceed: Bool = await withCheckedContinuation { continuation in
                queue.async {
                    guard !self.closed, generation == self.streamGeneration else { return continuation.resume(returning: false) }
                    self.streamer = streamer
                    self.streamDisplay = display.displayID
                    self.lastFocus = .none
                    self.server.injector.setDisplay(display.displayID)
                    let points = CGDisplayBounds(display.displayID).size
                    self.channel.sendMessage(.welcome, json: Welcome(
                        width: streamer.width, height: streamer.height,
                        pointWidth: points.width, pointHeight: points.height,
                        codec: streamer.codec, fps: fps,
                        hostName: Host.current().localizedName ?? "Mac",
                        isVirtual: virtual != nil))
                    self.cursorWatcher?.resend()  // the client rebuilds its cursor for the new scale
                    continuation.resume(returning: true)
                }
            }
            guard proceed else { return streamer.stop() }

            try await streamer.start()
            hostLog("streaming \(streamer.width)x\(streamer.height) @ \(fps) fps, \(streamer.encoderDescription), start \(String(format: "%.0f", startMbps)) Mbps")
        } catch {
            hostLog("could not start stream: \(error.localizedDescription)")
            if !CGPreflightScreenCaptureAccess() {
                hostLog("Screen Recording permission is missing for this process. See README → Permissions.")
            }
            queue.async { self.close(reason: "stream setup failed") }
        }
    }

    /// Tells the client when a text field gains or loses focus, so it can show its keyboard.
    private func startFocusWatcher() {
        let timer = DispatchSource.makeTimerSource(queue: DispatchQueue.global(qos: .utility))
        timer.schedule(deadline: .now() + 0.5, repeating: .milliseconds(200))
        timer.setEventHandler { [weak self] in
            let focus = FocusWatcher.current()
            self?.queue.async { self?.focusChanged(focus) }
        }
        timer.resume()
        focusTimer = timer
    }

    private func focusChanged(_ focus: FocusWatcher.Focus) {
        guard !closed, focus != lastFocus, let displayID = streamDisplay else { return }
        lastFocus = focus
        let bounds = CGDisplayBounds(displayID)
        var w = ByteWriter()
        w.u8(focus.editable ? 1 : 0)
        w.f32(Float((focus.frame.minX - bounds.minX) / bounds.width))
        w.f32(Float((focus.frame.minY - bounds.minY) / bounds.height))
        w.f32(Float(focus.frame.width / bounds.width))
        w.f32(Float(focus.frame.height / bounds.height))
        channel.sendMessage(.textFocus, w.data)
    }

    /// A freshly created virtual display can take a moment to show up in ScreenCaptureKit.
    /// ScreenCaptureKit can briefly report a stale display (old size) after a reshape, so for
    /// virtual displays wait until it reports the expected point size too.
    private func findDisplay(id: CGDirectDisplayID?, expectedPoints: (width: Int, height: Int)? = nil) async throws -> SCDisplay {
        for attempt in 0..<20 {
            let content = try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: true)
            if let id {
                if let match = content.displays.first(where: { $0.displayID == id }),
                   expectedPoints.map({ abs(match.width - $0.width) <= 1 && abs(match.height - $0.height) <= 1 }) ?? true {
                    return match
                }
            } else {
                let main = CGMainDisplayID()
                let displays = content.displays.sorted {
                    ($0.displayID == main ? 0 : 1, $0.displayID) < ($1.displayID == main ? 0 : 1, $1.displayID)
                }
                if !displays.isEmpty { return displays[min(server.config.displayIndex, displays.count - 1)] }
            }
            if attempt < 19 { try await Task.sleep(for: .milliseconds(100)) }
        }
        throw HostError(id == nil ? "no displays available to capture" : "virtual display \(id!) never appeared")
    }
}
