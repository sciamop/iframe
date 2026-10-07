import Foundation

/// Rewrites an H.264 SPS so decoders know frames are never reordered.
///
/// VideoToolbox leaves the VUI `bitstream_restriction_flag` at 0, so a spec-following decoder
/// (Windows/Chromium hardware decoders, for one) assumes it may need to buffer a full DPB of
/// frames before output. Holding 4+ frames deadlocks against the host's `maxInflight` ack
/// window. We encode with `AllowFrameReordering` off, so declaring `max_num_reorder_frames = 0`
/// is accurate and lets those decoders output each frame immediately.
enum H264SPS {
    /// Returns the SPS NAL unit (header byte included, no start code) with the bitstream
    /// restriction set, or nil if it couldn't be parsed.
    static func withoutReordering(_ nal: Data) -> Data? {
        let bytes = [UInt8](nal)
        guard bytes.count > 4, bytes[0] & 0x1F == 7 else { return nil }
        var r = BitReader(removeEmulationPrevention(Array(bytes[1...])))
        var w = BitWriter()

        // Copies one field through unchanged, returning its value.
        func bits(_ n: Int) -> UInt32? {
            guard let v = r.bits(n) else { return nil }
            w.bits(v, n)
            return v
        }
        func ue() -> UInt32? {
            guard let v = r.ue() else { return nil }
            w.ue(v)
            return v
        }
        func se() -> Bool {
            guard let v = r.ue() else { return false }
            w.ue(v)   // se(v) shares ue(v)'s bit pattern, so copy it as-is
            return true
        }
        func hrd() -> Bool {
            guard let count = ue(), count < 32, bits(4) != nil, bits(4) != nil else { return false }
            for _ in 0...count {
                guard ue() != nil, ue() != nil, bits(1) != nil else { return false }
            }
            return bits(20) != nil   // four 5-bit delay/offset lengths
        }

        guard let profile = bits(8), bits(8) != nil, bits(8) != nil, ue() != nil else { return nil }
        if [100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135].contains(profile) {
            guard let chroma = ue() else { return nil }
            if chroma == 3 { guard bits(1) != nil else { return nil } }
            guard ue() != nil, ue() != nil, bits(1) != nil, let scaling = bits(1) else { return nil }
            if scaling == 1 {
                for i in 0..<(chroma == 3 ? 12 : 8) {
                    guard let present = bits(1) else { return nil }
                    guard present == 1 else { continue }
                    var last: Int32 = 8, next: Int32 = 8
                    for _ in 0..<(i < 6 ? 16 : 64) where next != 0 {
                        guard let delta = r.se() else { return nil }
                        w.se(delta)
                        next = (last + delta + 256) % 256
                        if next != 0 { last = next }
                    }
                }
            }
        }
        guard ue() != nil, let pocType = ue() else { return nil }
        if pocType == 0 {
            guard ue() != nil else { return nil }
        } else if pocType == 1 {
            guard bits(1) != nil, se(), se(), let cycle = ue(), cycle < 256 else { return nil }
            for _ in 0..<cycle { guard se() else { return nil } }
        }
        guard let maxRefFrames = ue(), bits(1) != nil, ue() != nil, ue() != nil,
              let frameMbsOnly = bits(1) else { return nil }
        if frameMbsOnly == 0 { guard bits(1) != nil else { return nil } }
        guard bits(1) != nil, let cropping = bits(1) else { return nil }
        if cropping == 1 { for _ in 0..<4 { guard ue() != nil else { return nil } } }

        // Defaults the spec infers when there's no bitstream restriction.
        var mvOverBoundaries: UInt32 = 1, maxBytesPerPicDenom: UInt32 = 2, maxBitsPerMbDenom: UInt32 = 1
        var log2MaxMvH: UInt32 = 15, log2MaxMvV: UInt32 = 15

        guard let vui = r.bits(1) else { return nil }
        w.bits(1, 1)
        if vui == 1 {
            guard let aspect = bits(1) else { return nil }
            if aspect == 1 {
                guard let idc = bits(8) else { return nil }
                if idc == 255 { guard bits(32) != nil else { return nil } }
            }
            guard let overscan = bits(1) else { return nil }
            if overscan == 1 { guard bits(1) != nil else { return nil } }
            guard let signal = bits(1) else { return nil }
            if signal == 1 {
                guard bits(4) != nil, let colour = bits(1) else { return nil }
                if colour == 1 { guard bits(24) != nil else { return nil } }
            }
            guard let chromaLoc = bits(1) else { return nil }
            if chromaLoc == 1 { guard ue() != nil, ue() != nil else { return nil } }
            guard let timing = bits(1) else { return nil }
            if timing == 1 { guard bits(32) != nil, bits(32) != nil, bits(1) != nil else { return nil } }
            guard let nalHrd = bits(1) else { return nil }
            if nalHrd == 1 { guard hrd() else { return nil } }
            guard let vclHrd = bits(1) else { return nil }
            if vclHrd == 1 { guard hrd() else { return nil } }
            if nalHrd == 1 || vclHrd == 1 { guard bits(1) != nil else { return nil } }
            guard bits(1) != nil, let restriction = r.bits(1) else { return nil }   // pic_struct_present
            if restriction == 1 {
                guard let a = r.bits(1), let b = r.ue(), let c = r.ue(), let d = r.ue(), let e = r.ue(),
                      r.ue() != nil, r.ue() != nil else { return nil }
                (mvOverBoundaries, maxBytesPerPicDenom, maxBitsPerMbDenom, log2MaxMvH, log2MaxMvV) = (a, b, c, d, e)
            }
        } else {
            // aspect ratio, overscan, video signal, chroma location, timing, NAL HRD,
            // VCL HRD, pic_struct: all absent.
            w.bits(0, 8)
        }
        w.bits(1, 1)   // bitstream_restriction_flag
        w.bits(mvOverBoundaries, 1)
        w.ue(maxBytesPerPicDenom)
        w.ue(maxBitsPerMbDenom)
        w.ue(log2MaxMvH)
        w.ue(log2MaxMvV)
        w.ue(0)                          // max_num_reorder_frames
        w.ue(max(1, maxRefFrames))       // max_dec_frame_buffering
        w.trailingBits()

        return Data([bytes[0]] + addEmulationPrevention(w.bytes))
    }

    /// Strips the 0x03 that follows every 0x00 0x00 in a NAL payload.
    static func removeEmulationPrevention(_ bytes: [UInt8]) -> [UInt8] {
        var out: [UInt8] = []
        out.reserveCapacity(bytes.count)
        var zeros = 0
        for byte in bytes {
            if zeros >= 2 && byte == 3 {
                zeros = 0
                continue
            }
            zeros = byte == 0 ? zeros + 1 : 0
            out.append(byte)
        }
        return out
    }

    /// Inserts 0x03 wherever 0x00 0x00 would be followed by a byte <= 0x03.
    static func addEmulationPrevention(_ bytes: [UInt8]) -> [UInt8] {
        var out: [UInt8] = []
        out.reserveCapacity(bytes.count + 4)
        var zeros = 0
        for byte in bytes {
            if zeros >= 2 && byte <= 3 {
                out.append(3)
                zeros = 0
            }
            zeros = byte == 0 ? zeros + 1 : 0
            out.append(byte)
        }
        return out
    }
}

private struct BitReader {
    let bytes: [UInt8]
    var pos = 0   // in bits

    init(_ bytes: [UInt8]) { self.bytes = bytes }

    mutating func bits(_ n: Int) -> UInt32? {
        guard pos + n <= bytes.count * 8 else { return nil }
        var v: UInt32 = 0
        for _ in 0..<n {
            v = v << 1 | UInt32(bytes[pos >> 3] >> (7 - UInt8(pos & 7)) & 1)
            pos += 1
        }
        return v
    }

    mutating func ue() -> UInt32? {
        var zeros = 0
        while true {
            guard let bit = bits(1) else { return nil }
            if bit == 1 { break }
            zeros += 1
            guard zeros < 32 else { return nil }
        }
        guard let rest = bits(zeros) else { return nil }
        return (1 << zeros) - 1 + rest
    }

    mutating func se() -> Int32? {
        guard let k = ue() else { return nil }
        return k & 1 == 1 ? Int32((k + 1) / 2) : -Int32(k / 2)
    }
}

private struct BitWriter {
    var bytes: [UInt8] = []
    private var count = 0   // bits written

    mutating func bits(_ value: UInt32, _ n: Int) {
        for i in stride(from: n - 1, through: 0, by: -1) {
            if count & 7 == 0 { bytes.append(0) }
            if value >> i & 1 == 1 { bytes[bytes.count - 1] |= 0x80 >> UInt8(count & 7) }
            count += 1
        }
    }

    mutating func ue(_ value: UInt32) {
        let v = UInt64(value) + 1
        let length = 64 - v.leadingZeroBitCount
        bits(0, length - 1)
        for i in stride(from: length - 1, through: 0, by: -1) { bits(UInt32(v >> i & 1), 1) }
    }

    mutating func se(_ value: Int32) {
        ue(value > 0 ? UInt32(value) * 2 - 1 : UInt32(-value) * 2)
    }

    mutating func trailingBits() {
        bits(1, 1)
        while count & 7 != 0 { bits(0, 1) }
    }
}
