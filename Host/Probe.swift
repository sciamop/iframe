import CoreMedia
import Foundation
import Network
import VideoToolbox

/// A headless test client: connects, decodes every frame with the hardware decoder,
/// acks like the iPad app does, and prints per-second stats. Never sends input.
func runProbe() -> Never {
    let host = arguments.first.flatMap { $0.hasPrefix("--") ? nil : $0 } ?? "127.0.0.1"
    let port = option("--port").flatMap(UInt16.init) ?? IFrame.defaultPort
    let pin = option("--pin") ?? ""
    let seconds = option("--seconds").flatMap(Double.init) ?? 10
    let skipDecode = arguments.contains("--no-decode")
    // e.g. --screen 2388x1668@2 to exercise the virtual display like an 11" iPad Pro would.
    let screen: DisplayRequest? = option("--screen").flatMap { spec in
        let parts = spec.split(separator: "@")
        let dims = parts[0].split(separator: "x").compactMap { Int($0) }
        guard dims.count == 2 else { return nil }
        return DisplayRequest(width: dims[0], height: dims[1], uiScale: parts.count > 1 ? Double(parts[1]) ?? 2 : 2)
    }

    let queue = DispatchQueue(label: "iframe.probe", qos: .userInteractive)
    let connection = NWConnection(host: NWEndpoint.Host(host), port: NWEndpoint.Port(rawValue: port)!,
                                  using: MessageChannel.parameters())
    let channel = MessageChannel(connection: connection, queue: queue)
    let decoder = VideoDecoder()

    var frames = 0, bytes = 0, keyframes = 0, failures = 0
    var decodeNanos: UInt64 = 0
    var totalFrames = 0, totalBytes = 0
    var rtts: [Double] = []
    var firstFrameAt: UInt64?
    let startedAt = nowNanos()

    func summary() -> Never {
        let elapsed = Double(nowNanos() - (firstFrameAt ?? startedAt)) / 1e9
        let sorted = rtts.sorted()
        let median = sorted.isEmpty ? 0 : sorted[sorted.count / 2]
        print(String(format: "\nprobe summary: %d frames, %.1f MB, avg %.1f Mbps, median RTT %.2f ms, %d decode failures",
                     totalFrames, Double(totalBytes) / 1e6, elapsed > 0 ? Double(totalBytes) * 8 / elapsed / 1e6 : 0,
                     median, failures))
        exit(totalFrames > 0 ? 0 : 3)
    }

    channel.onStateChange = { state in
        switch state {
        case .ready:
            print("probe: connected to \(host):\(port)")
            channel.send(.hello, json: Hello(
                version: IFrame.protocolVersion, pin: pin, name: "iframe-probe",
                supportsHEVC: VTIsHardwareDecodeSupported(kCMVideoCodecType_HEVC), maxFPS: 120,
                display: screen))
            let timer = DispatchSource.makeTimerSource(queue: queue)
            timer.schedule(deadline: .now() + 0.25, repeating: 0.25)
            timer.setEventHandler {
                var w = ByteWriter()
                w.u64(nowNanos())
                channel.send(.ping, w.data)
            }
            timer.resume()
            objc_setAssociatedObject(channel, "timer", timer, .OBJC_ASSOCIATION_RETAIN)
        case .failed(let error), .waiting(let error):
            print("probe: connection failed: \(error)")
            exit(1)
        case .cancelled:
            print("probe: disconnected")
            summary()
        default:
            break
        }
    }

    channel.onMessage = { type, data in
        switch type {
        case .welcome:
            if let w = try? JSONDecoder().decode(Welcome.self, from: data) {
                print("probe: streaming \(w.width)x\(w.height) \(w.codec.name) @ \(w.fps) fps from \(w.hostName)")
            }
        case .authFailed:
            print("probe: wrong PIN")
            exit(2)
        case .format:
            if let (codec, sets) = Wire.parseFormat(data), !decoder.setFormat(codec: codec, parameterSets: sets) {
                print("probe: could not create decoder")
            }
        case .frame:
            guard let frame = Wire.parseFrame(data) else { return }
            if firstFrameAt == nil { firstFrameAt = nowNanos() }
            let t0 = nowNanos()
            let ok = skipDecode ? true : decoder.decode(frame.payload)
            let dt = nowNanos() - t0
            channel.send(.ack, Wire.ack(id: frame.id, decodeMicros: UInt32(min(dt / 1000, UInt64(UInt32.max)))))
            if ok {
                frames += 1
                decodeNanos += dt
            } else {
                failures += 1
                channel.send(.requestKeyframe)
            }
            if frame.isKeyframe { keyframes += 1 }
            bytes += frame.payload.count
            totalFrames += 1
            totalBytes += frame.payload.count
        case .stats:
            guard let s = try? JSONDecoder().decode(HostStats.self, from: data) else { return }
            let rtt = rtts.suffix(4).reduce(0, +) / Double(max(1, min(4, rtts.count)))
            print(String(format: "host %3.0f fps %6.2f Mbps (target %5.1f) enc %4.2f ms ack %5.2f ms drop %d | client %3d fps dec %4.2f ms rtt %5.2f ms key %d",
                         s.fps, s.mbps, s.targetMbps, s.encodeMs, s.latencyMs, s.dropped,
                         frames, frames > 0 ? Double(decodeNanos) / Double(frames) / 1e6 : 0, rtt, keyframes))
            frames = 0; bytes = 0; decodeNanos = 0; keyframes = 0
        case .pong:
            var r = ByteReader(data)
            if let sent = r.u64() { rtts.append(Double(nowNanos() - sent) / 1e6) }
        default:
            break
        }
    }

    channel.start()
    queue.asyncAfter(deadline: .now() + seconds) { summary() }
    dispatchMain()
}
