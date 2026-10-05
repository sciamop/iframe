import ApplicationServices
import CoreGraphics
import Foundation
import SystemConfiguration

setvbuf(stdout, nil, _IOLBF, 0)

func hostLog(_ message: String) {
    let formatter = DateFormatter()
    formatter.dateFormat = "HH:mm:ss"
    print("[\(formatter.string(from: Date()))] \(message)")
}

let usage = """
iframe-host — low-latency Mac screen streaming for Apple silicon

USAGE
  iframe-host [options]                 run the host
  iframe-host probe [host] [options]    connect as a test client and report stats

HOST OPTIONS
  --port <n>        TCP port (default \(IFrame.defaultPort))
  --fps <n>         max frame rate (default 120; capped by display refresh and client)
  --no-virtual      stream the existing display instead of creating one shaped like the client
  --mbps <n>        starting bitrate in Mbps (default: auto from resolution)
  --codec <c>       hevc | h264 (default: hevc if the client can decode it)
  --display <n>     display index, 0 = main (default 0)
  --inflight <n>    max unacknowledged frames before skipping (default 3)
  --pin <digits>    fixed PIN (default: random each launch)
  --web-port <n>    HTTPS port for the browser client (default 7880)
  --no-web          don't serve the browser client
  --web-root <dir>  serve the browser client from this folder (default: bundled)

PROBE OPTIONS
  --pin <digits>  --port <n>  --seconds <n>  --screen <w>x<h>[@scale]
"""

var arguments = Array(CommandLine.arguments.dropFirst())

func option(_ name: String) -> String? {
    guard let index = arguments.firstIndex(of: name), index + 1 < arguments.count else { return nil }
    return arguments[index + 1]
}

if arguments.contains("--help") || arguments.contains("-h") {
    print(usage)
    exit(0)
}

if arguments.first == "probe" {
    arguments.removeFirst()
    runProbe()
}

var config = HostConfig()
if let v = option("--port").flatMap(UInt16.init) { config.port = v }
if let v = option("--fps").flatMap(Int.init) { config.fps = max(1, v) }
if let v = option("--mbps").flatMap(Double.init) { config.mbps = v }
if let v = option("--display").flatMap(Int.init) { config.displayIndex = max(0, v) }
if arguments.contains("--no-virtual") { config.virtualDisplay = false }
if let v = option("--inflight").flatMap(Int.init) { config.maxInflight = max(1, v) }
switch option("--codec")?.lowercased() {
case "hevc", "h265": config.codec = .hevc
case "h264", "avc": config.codec = .h264
default: break
}
config.pin = option("--pin") ?? String(format: "%06d", Int.random(in: 0..<1_000_000))

let appName = Bundle.main.bundleIdentifier == nil ? "your terminal app" : "iFrame Host"
if !CGPreflightScreenCaptureAccess() {
    CGRequestScreenCaptureAccess()
    hostLog("⚠️  Screen Recording permission needed: System Settings → Privacy & Security → Screen & System Audio Recording → enable \(appName), then relaunch.")
}
let axPrompt = [kAXTrustedCheckOptionPrompt.takeUnretainedValue(): true] as CFDictionary
if !AXIsProcessTrustedWithOptions(axPrompt) {
    hostLog("⚠️  Accessibility permission needed for mouse/keyboard: System Settings → Privacy & Security → Accessibility → enable \(appName).")
}

let server = HostServer(config: config)
do {
    try server.start()
} catch {
    hostLog("failed to start: \(error.localizedDescription)")
    exit(1)
}

let addresses = localIPv4Addresses()
let webPort = option("--web-port").flatMap(UInt16.init) ?? 7880
var webServer: WebServer?
var webURLs: [String] = []
if !arguments.contains("--no-web") {
    if let root = webRoot() {
        let ips = addresses.map { String($0.split(separator: " ")[0]) }
        // Not ProcessInfo.hostName: it does a reverse DNS lookup that can hang for minutes.
        let bonjourName = (SCDynamicStoreCopyLocalHostName(nil) as String? ?? "localhost") + ".local"
        do {
            let certDir = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent(".config/iframe/web")
            let identity = try WebServer.loadIdentity(directory: certDir, names: ["localhost", bonjourName],
                                                      addresses: ["127.0.0.1"] + ips)
            let web = WebServer(port: webPort, webRoot: root, identity: identity) { server.accept(webSocket: $0) }
            try web.start()
            webServer = web
            webURLs = ([bonjourName] + ips).map { "https://\($0):\(webPort)" }
        } catch {
            hostLog("browser client disabled: \(error.localizedDescription)")
        }
    } else {
        hostLog("browser client disabled: web client files not found (use --web-root)")
    }
}

print("""

  iframe-host ready on port \(config.port)
  PIN: \(config.pin)
  Addresses: \(addresses.isEmpty ? "(none found)" : addresses.joined(separator: ", "))
  The iPad app finds this Mac automatically via Bonjour.
\(webURLs.isEmpty ? "" : "  Browser client: " + webURLs.joined(separator: "  ") + "\n")
""")

let signalSource = DispatchSource.makeSignalSource(signal: SIGINT, queue: .main)
signal(SIGINT, SIG_IGN)
signalSource.setEventHandler {
    server.shutdown()
    exit(0)
}
signalSource.resume()

dispatchMain()

/// The browser client's files: --web-root, else bundled in the app, else the source tree (dev builds).
func webRoot() -> URL? {
    let fm = FileManager.default
    var candidates: [URL] = []
    if let path = option("--web-root") { candidates.append(URL(fileURLWithPath: path)) }
    if let resources = Bundle.main.resourceURL { candidates.append(resources.appendingPathComponent("web")) }
    let exe = URL(fileURLWithPath: CommandLine.arguments[0]).resolvingSymlinksInPath()
    candidates.append(exe.deletingLastPathComponent().appendingPathComponent("../../../Web"))  // .build/release/iframe-host
    return candidates.first { fm.fileExists(atPath: $0.appendingPathComponent("index.html").path) }?.standardizedFileURL
}

func localIPv4Addresses() -> [String] {
    var result: [String] = []
    var ifaddr: UnsafeMutablePointer<ifaddrs>?
    guard getifaddrs(&ifaddr) == 0, let first = ifaddr else { return [] }
    defer { freeifaddrs(ifaddr) }
    for pointer in sequence(first: first, next: { $0.pointee.ifa_next }) {
        let interface = pointer.pointee
        guard let addr = interface.ifa_addr, addr.pointee.sa_family == UInt8(AF_INET) else { continue }
        let name = String(cString: interface.ifa_name)
        guard name.hasPrefix("en") || name.hasPrefix("utun") || name.hasPrefix("bridge") else { continue }
        var host = [CChar](repeating: 0, count: Int(NI_MAXHOST))
        if getnameinfo(addr, socklen_t(addr.pointee.sa_len), &host, socklen_t(host.count), nil, 0, NI_NUMERICHOST) == 0 {
            result.append("\(String(cString: host)) (\(name))")
        }
    }
    return result
}
