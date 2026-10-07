import SwiftUI

/// Debug overlay: stream shape, frame rate, bitrate, and where the latency goes.
struct StatsView: View {
    let stats: ClientStats
    let welcome: Welcome?

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            if let w = welcome {
                Text("\(w.hostName) · \(w.width)×\(w.height) \(w.codec.name) @ \(w.fps)")
            }
            if let h = stats.host {
                Text(String(format: "%.0f fps · %.1f Mbps (target %.0f)", h.fps, h.mbps, h.targetMbps))
                Text(String(format: "encode %.1f ms · decode %.1f ms", h.encodeMs, stats.decodeMs))
                Text(String(format: "net RTT %.1f ms · frame ack %.1f ms", stats.rttMs, h.latencyMs))
                if h.dropped > 0 { Text("skipped \(h.dropped) frames (congestion)") }
            } else {
                Text("waiting for stats…")
            }
        }
        .font(.system(size: 11, design: .monospaced))
        .foregroundStyle(.white)
        .padding(8)
        .background(.black.opacity(0.6), in: RoundedRectangle(cornerRadius: 8))
    }
}
