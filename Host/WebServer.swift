import CryptoKit
import Foundation
import Network
import Security

/// HTTPS + secure WebSocket server for the browser client, on a single port.
///
/// Browsers only expose hardware video decoding (WebCodecs) to secure pages, so this always
/// speaks TLS. With no certificate configured it creates a self-signed one on first run
/// (browsers ask you to accept it once per machine).
///
/// Routes: `GET /ws` upgrades to the iFrame protocol over WebSocket; anything else serves a
/// static file from the web client directory.
final class WebServer {
    private let port: UInt16
    private let webRoot: URL
    private let identity: SecIdentity
    private let queue = DispatchQueue(label: "iframe.web", qos: .userInteractive)
    private let onWebSocket: (WebSocketTransport) -> Void
    private var listener: NWListener?

    init(port: UInt16, webRoot: URL, identity: SecIdentity, onWebSocket: @escaping (WebSocketTransport) -> Void) {
        self.port = port
        self.webRoot = webRoot.standardizedFileURL
        self.identity = identity
        self.onWebSocket = onWebSocket
    }

    func start() throws {
        guard let nwPort = NWEndpoint.Port(rawValue: port), let secIdentity = sec_identity_create(identity) else {
            throw HostError("invalid web server configuration")
        }
        let tls = NWProtocolTLS.Options()
        sec_protocol_options_set_local_identity(tls.securityProtocolOptions, secIdentity)
        sec_protocol_options_set_min_tls_protocol_version(tls.securityProtocolOptions, .TLSv12)
        let tcp = NWProtocolTCP.Options()
        tcp.noDelay = true
        tcp.enableKeepalive = true
        tcp.keepaliveIdle = 5
        let params = NWParameters(tls: tls, tcp: tcp)
        params.serviceClass = .interactiveVideo
        let listener = try NWListener(using: params, on: nwPort)
        listener.stateUpdateHandler = { state in
            if case .failed(let error) = state { hostLog("web server failed: \(error)") }
        }
        listener.newConnectionHandler = { [weak self] connection in self?.handle(connection) }
        listener.start(queue: queue)
        self.listener = listener
    }

    // MARK: HTTP

    private func handle(_ connection: NWConnection) {
        connection.start(queue: queue)
        readRequest(connection, buffer: Data())
    }

    private func readRequest(_ connection: NWConnection, buffer: Data) {
        connection.receive(minimumIncompleteLength: 1, maximumLength: 16384) { [weak self] data, _, isComplete, error in
            guard let self else { return }
            var buffer = buffer
            if let data { buffer.append(data) }
            if let end = buffer.range(of: Data("\r\n\r\n".utf8)) {
                let head = String(decoding: buffer[..<end.lowerBound], as: UTF8.self)
                let rest = Data(buffer[end.upperBound...])
                self.route(connection, head: head, leftover: rest)
            } else if buffer.count > 16384 || isComplete || error != nil {
                connection.cancel()
            } else {
                self.readRequest(connection, buffer: buffer)
            }
        }
    }

    private func route(_ connection: NWConnection, head: String, leftover: Data) {
        let lines = head.components(separatedBy: "\r\n")
        let parts = lines.first?.split(separator: " ") ?? []
        guard parts.count >= 2 else { return respond(connection, status: "400 Bad Request", body: Data()) }
        var headers: [String: String] = [:]
        for line in lines.dropFirst() {
            guard let colon = line.firstIndex(of: ":") else { continue }
            headers[line[..<colon].lowercased()] = line[line.index(after: colon)...].trimmingCharacters(in: .whitespaces)
        }
        let method = String(parts[0])
        let path = String(parts[1].split(separator: "?").first ?? "/")

        if path == "/ws", headers["upgrade"]?.lowercased() == "websocket", let key = headers["sec-websocket-key"] {
            let accept = Data(Insecure.SHA1.hash(data: Data((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").utf8))).base64EncodedString()
            let response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: \(accept)\r\n\r\n"
            connection.send(content: Data(response.utf8), completion: .contentProcessed { [weak self] error in
                guard let self, error == nil else { return connection.cancel() }
                self.onWebSocket(WebSocketTransport(connection: connection, queue: self.queue, leftover: leftover))
            })
            return
        }

        guard method == "GET" || method == "HEAD" else { return respond(connection, status: "405 Method Not Allowed", body: Data()) }
        let relative = path == "/" ? "index.html" : String(path.drop(while: { $0 == "/" }))
        let file = webRoot.appendingPathComponent(relative.removingPercentEncoding ?? relative).standardizedFileURL
        guard file.path.hasPrefix(webRoot.path + "/"), let body = try? Data(contentsOf: file) else {
            return respond(connection, status: "404 Not Found", body: Data("not found".utf8), type: "text/plain")
        }
        respond(connection, status: "200 OK", body: method == "HEAD" ? Data() : body, type: Self.contentType(file.pathExtension),
                length: body.count)
    }

    private func respond(_ connection: NWConnection, status: String, body: Data, type: String = "text/plain", length: Int? = nil) {
        let head = "HTTP/1.1 \(status)\r\nContent-Type: \(type)\r\nContent-Length: \(length ?? body.count)\r\n"
            + "Cache-Control: no-cache\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n"
        connection.send(content: Data(head.utf8) + body, isComplete: true, completion: .contentProcessed { _ in
            connection.cancel()
        })
    }

    private static func contentType(_ ext: String) -> String {
        switch ext.lowercased() {
        case "html": return "text/html; charset=utf-8"
        case "js": return "text/javascript; charset=utf-8"
        case "css": return "text/css; charset=utf-8"
        case "png": return "image/png"
        case "svg": return "image/svg+xml"
        case "json", "webmanifest": return "application/json"
        case "ico": return "image/x-icon"
        default: return "application/octet-stream"
        }
    }

    // MARK: Certificate

    /// Loads (or creates) the self-signed TLS identity. It's regenerated whenever the set of
    /// names/addresses changes, so the certificate always matches how you reach the Mac.
    static func loadIdentity(directory: URL, names: [String], addresses: [String]) throws -> SecIdentity {
        let fm = FileManager.default
        try fm.createDirectory(at: directory, withIntermediateDirectories: true)
        let p12 = directory.appendingPathComponent("identity.p12")
        let sansFile = directory.appendingPathComponent("names.txt")
        let sans = (names.map { "DNS:\($0)" } + addresses.map { "IP:\($0)" }).joined(separator: ",")

        if (try? String(contentsOf: sansFile, encoding: .utf8)) != sans || !fm.fileExists(atPath: p12.path) {
            hostLog("web: creating self-signed certificate for \(sans)")
            let key = directory.appendingPathComponent("key.pem")
            let cert = directory.appendingPathComponent("cert.pem")
            try run("/usr/bin/openssl", ["req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "825",
                                         "-keyout", key.path, "-out", cert.path, "-subj", "/CN=iFrame Host",
                                         "-addext", "subjectAltName=\(sans)",
                                         "-addext", "extendedKeyUsage=serverAuth"])
            try run("/usr/bin/openssl", ["pkcs12", "-export", "-inkey", key.path, "-in", cert.path,
                                         "-out", p12.path, "-passout", "pass:iframe"])
            try? fm.removeItem(at: key)
            try sans.write(to: sansFile, atomically: true, encoding: .utf8)
            try? fm.setAttributes([.posixPermissions: 0o600], ofItemAtPath: p12.path)
        }

        let data = try Data(contentsOf: p12)
        var options: [String: Any] = [kSecImportExportPassphrase as String: "iframe"]
        if #available(macOS 15.0, *) { options[kSecImportToMemoryOnly as String] = true }
        var items: CFArray?
        let status = SecPKCS12Import(data as CFData, options as CFDictionary, &items)
        guard status == errSecSuccess,
              let item = (items as? [[String: Any]])?.first,
              let identity = item[kSecImportItemIdentity as String] else {
            throw HostError("could not load TLS identity (\(status))")
        }
        return identity as! SecIdentity
    }

    private static func run(_ tool: String, _ args: [String]) throws {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: tool)
        process.arguments = args
        process.standardOutput = FileHandle.nullDevice
        process.standardError = FileHandle.nullDevice
        try process.run()
        process.waitUntilExit()
        guard process.terminationStatus == 0 else { throw HostError("\(tool) failed (\(process.terminationStatus))") }
    }
}

/// The iFrame protocol over a WebSocket (RFC 6455). Each binary frame carries one message:
/// type (u8) followed by its payload; the WebSocket framing replaces the length prefix.
final class WebSocketTransport: MessageTransport {
    var onMessage: ((MsgType, Data) -> Void)?
    var onClosed: (() -> Void)?
    private let connection: NWConnection
    private let queue: DispatchQueue
    private var buffer: Data
    private var fragments = Data()
    private var closed = false

    var peerDescription: String {
        if case let .hostPort(host, _) = connection.endpoint { return "\(host) (browser)" }
        return "browser"
    }

    init(connection: NWConnection, queue: DispatchQueue, leftover: Data) {
        self.connection = connection
        self.queue = queue
        self.buffer = leftover
    }

    func start() {
        connection.stateUpdateHandler = { [weak self] state in
            switch state {
            case .failed, .cancelled: self?.finish()
            default: break
            }
        }
        parseFrames()
        receive()
    }

    func cancel() {
        queue.async {
            guard !self.closed else { return }
            self.sendFrame(opcode: 0x8, payload: Data([0x03, 0xE8]))  // 1000 normal closure
            self.connection.cancel()
        }
    }

    func sendMessage(_ type: MsgType, _ payload: Data, completion: ((Bool) -> Void)?) {
        var message = Data(capacity: payload.count + 1)
        message.append(type.rawValue)
        message.append(payload)
        sendFrame(opcode: 0x2, payload: message, completion: completion)
    }

    private func finish() {
        guard !closed else { return }
        closed = true
        onClosed?()
    }

    private func sendFrame(opcode: UInt8, payload: Data, completion: ((Bool) -> Void)? = nil) {
        var frame = Data(capacity: payload.count + 10)
        frame.append(0x80 | opcode)
        if payload.count < 126 {
            frame.append(UInt8(payload.count))
        } else if payload.count <= 0xFFFF {
            frame.append(126)
            withUnsafeBytes(of: UInt16(payload.count).bigEndian) { frame.append(contentsOf: $0) }
        } else {
            frame.append(127)
            withUnsafeBytes(of: UInt64(payload.count).bigEndian) { frame.append(contentsOf: $0) }
        }
        frame.append(payload)
        connection.send(content: frame, completion: .contentProcessed { error in completion?(error == nil) })
    }

    private func receive() {
        connection.receive(minimumIncompleteLength: 1, maximumLength: 1 << 20) { [weak self] data, _, isComplete, error in
            guard let self else { return }
            if let data, !data.isEmpty {
                self.buffer.append(data)
                self.parseFrames()
            }
            if isComplete || error != nil {
                self.connection.cancel()
            } else if !self.closed {
                self.receive()
            }
        }
    }

    private func parseFrames() {
        while true {
            let bytes = buffer
            guard bytes.count >= 2 else { return }
            let b0 = bytes[bytes.startIndex], b1 = bytes[bytes.startIndex + 1]
            let fin = b0 & 0x80 != 0
            let opcode = b0 & 0x0F
            let masked = b1 & 0x80 != 0
            var length = Int(b1 & 0x7F)
            var offset = 2
            if length == 126 {
                guard bytes.count >= 4 else { return }
                length = Int(bytes[bytes.startIndex + 2]) << 8 | Int(bytes[bytes.startIndex + 3])
                offset = 4
            } else if length == 127 {
                guard bytes.count >= 10 else { return }
                length = (0..<8).reduce(0) { $0 << 8 | Int(bytes[bytes.startIndex + 2 + $1]) }
                offset = 10
            }
            guard length <= IFrame.maxMessageSize, masked else { return connection.cancel() }  // clients must mask
            guard bytes.count >= offset + 4 + length else { return }
            let mask = Array(bytes[(bytes.startIndex + offset)..<(bytes.startIndex + offset + 4)])
            let start = bytes.startIndex + offset + 4
            var payload = Data(bytes[start..<(start + length)])
            payload.withUnsafeMutableBytes { raw in
                for i in 0..<raw.count { raw[i] ^= mask[i & 3] }
            }
            buffer = Data(bytes[(start + length)...])

            switch opcode {
            case 0x0, 0x2:  // continuation, binary
                fragments.append(payload)
                if fin {
                    let message = fragments
                    fragments = Data()
                    if let raw = message.first, let type = MsgType(rawValue: raw) {
                        onMessage?(type, Data(message.dropFirst()))
                    }
                }
            case 0x8:  // close
                sendFrame(opcode: 0x8, payload: payload.prefix(2))
                connection.cancel()
                return
            case 0x9:  // ping
                sendFrame(opcode: 0xA, payload: payload)
            default:   // text, pong: ignored
                break
            }
        }
    }
}
