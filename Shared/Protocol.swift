import Foundation

// Wire protocol shared by iframe-host (macOS) and the iFrame client (iPadOS).
//
// Every message is framed as: type (u8) | payload length (u32, big endian) | payload.
// Video is sent exactly as VideoToolbox produces it (length-prefixed AVCC/HVCC NAL units),
// so neither side has to rewrite bitstreams.

enum IFrame {
    static let serviceType = "_iframe._tcp"
    static let defaultPort: UInt16 = 7878
    static let protocolVersion = 1
    static let maxMessageSize = 32 << 20
}

enum MsgType: UInt8 {
    // host -> client
    case welcome = 0x01          // JSON Welcome
    case format = 0x02           // codec u8, count u8, [len u32, parameter set]...
    case frame = 0x03            // id u32, pts u64, flags u8, access unit
    case stats = 0x04            // JSON HostStats
    case pong = 0x05             // echo of ping payload
    case authFailed = 0x06
    case textFocus = 0x07        // editable u8, x f32, y f32, w f32, h f32 (normalized over the stream)
    case cursor = 0x08           // hotspot x u16, y u16, size w u16, h u16 (Mac points), PNG at 2x
    // client -> host
    case hello = 0x10            // JSON Hello
    case mouseMove = 0x11        // x f32, y f32 (normalized 0...1)
    case mouseButton = 0x12      // button u8, down u8, x f32, y f32
    case scroll = 0x13           // dx f32, dy f32 (Mac points)
    case key = 0x14              // mac keycode u16, KeyAction u8, KeyMods u32
    case text = 0x15             // utf8
    case requestKeyframe = 0x16
    case ping = 0x17             // client clock u64
    case ack = 0x18              // frame id u32, decode micros u32
    case display = 0x19          // JSON DisplayRequest (e.g. after rotation)
}

enum VideoCodec: UInt8, Codable {
    case h264 = 0
    case hevc = 1

    var name: String { self == .hevc ? "HEVC" : "H.264" }
}

struct Hello: Codable {
    var version: Int
    var pin: String
    var name: String
    var supportsHEVC: Bool
    var maxFPS: Int
    var display: DisplayRequest?
    /// The client draws the pointer itself from `cursor` messages; capture omits it.
    var localCursor: Bool?
}

/// The client's screen in pixels (current orientation). The host creates a virtual display
/// to match. uiScale is pixels per Mac point: 2 = Retina 1:1, lower = more desktop space.
/// uiScale <= 0 means stream the Mac's existing display instead.
struct DisplayRequest: Codable, Equatable {
    var width: Int
    var height: Int
    var uiScale: Double
}

struct Welcome: Codable, Equatable {
    var width: Int
    var height: Int
    var pointWidth: Double
    var pointHeight: Double
    var codec: VideoCodec
    var fps: Int
    var hostName: String
    var isVirtual: Bool
}

struct HostStats: Codable, Equatable {
    var fps: Double
    var mbps: Double
    var targetMbps: Double
    var encodeMs: Double
    var latencyMs: Double   // frame handed to network -> client ack (network + decode)
    var dropped: Int
}

struct KeyMods: OptionSet {
    let rawValue: UInt32
    static let shift = KeyMods(rawValue: 1 << 0)
    static let control = KeyMods(rawValue: 1 << 1)
    static let option = KeyMods(rawValue: 1 << 2)
    static let command = KeyMods(rawValue: 1 << 3)
    static let capsLock = KeyMods(rawValue: 1 << 4)
}

enum KeyAction: UInt8 {
    case up = 0
    case down = 1
    case repeatDown = 2
}

struct FrameMessage {
    var id: UInt32
    var pts: UInt64
    var isKeyframe: Bool
    var payload: Data
}

enum Wire {
    static func format(codec: VideoCodec, parameterSets: [Data]) -> Data {
        var w = ByteWriter()
        w.u8(codec.rawValue)
        w.u8(UInt8(parameterSets.count))
        for set in parameterSets {
            w.u32(UInt32(set.count))
            w.bytes(set)
        }
        return w.data
    }

    static func parseFormat(_ data: Data) -> (VideoCodec, [Data])? {
        var r = ByteReader(data)
        guard let raw = r.u8(), let codec = VideoCodec(rawValue: raw), let count = r.u8() else { return nil }
        var sets: [Data] = []
        for _ in 0..<count {
            guard let len = r.u32(), let set = r.bytes(Int(len)) else { return nil }
            sets.append(set)
        }
        return (codec, sets)
    }

    static func frame(id: UInt32, pts: UInt64, keyframe: Bool, payload: Data) -> Data {
        var w = ByteWriter(capacity: payload.count + 13)
        w.u32(id)
        w.u64(pts)
        w.u8(keyframe ? 1 : 0)
        w.bytes(payload)
        return w.data
    }

    static func parseFrame(_ data: Data) -> FrameMessage? {
        var r = ByteReader(data)
        guard let id = r.u32(), let pts = r.u64(), let flags = r.u8() else { return nil }
        return FrameMessage(id: id, pts: pts, isKeyframe: flags & 1 != 0, payload: r.rest())
    }

    static func ack(id: UInt32, decodeMicros: UInt32) -> Data {
        var w = ByteWriter()
        w.u32(id)
        w.u32(decodeMicros)
        return w.data
    }
}

struct ByteWriter {
    private(set) var data = Data()

    init(capacity: Int = 32) { data.reserveCapacity(capacity) }

    mutating func u8(_ v: UInt8) { data.append(v) }
    mutating func u16(_ v: UInt16) { append(v.bigEndian) }
    mutating func u32(_ v: UInt32) { append(v.bigEndian) }
    mutating func u64(_ v: UInt64) { append(v.bigEndian) }
    mutating func f32(_ v: Float) { u32(v.bitPattern) }
    mutating func bytes(_ d: Data) { data.append(d) }

    private mutating func append<T>(_ v: T) {
        withUnsafeBytes(of: v) { data.append(contentsOf: $0) }
    }
}

struct ByteReader {
    private let data: Data
    private var offset: Int

    init(_ data: Data) {
        self.data = data
        offset = data.startIndex
    }

    mutating func u8() -> UInt8? {
        guard offset < data.endIndex else { return nil }
        defer { offset += 1 }
        return data[offset]
    }

    mutating func u16() -> UInt16? { integer() }
    mutating func u32() -> UInt32? { integer() }
    mutating func u64() -> UInt64? { integer() }
    mutating func f32() -> Float? { u32().map(Float.init(bitPattern:)) }

    mutating func bytes(_ count: Int) -> Data? {
        guard count >= 0, data.endIndex - offset >= count else { return nil }
        defer { offset += count }
        return data[offset..<(offset + count)]
    }

    mutating func rest() -> Data {
        defer { offset = data.endIndex }
        return data[offset...]
    }

    private mutating func integer<T: FixedWidthInteger>() -> T? {
        let size = MemoryLayout<T>.size
        guard data.endIndex - offset >= size else { return nil }
        var value: T = 0
        for i in 0..<size { value = (value << 8) | T(data[offset + i]) }
        offset += size
        return value
    }
}

/// Monotonic clock in nanoseconds. Only meaningful on the machine that produced it.
func nowNanos() -> UInt64 { DispatchTime.now().uptimeNanoseconds }
