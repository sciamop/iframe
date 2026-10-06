import Foundation
import Network

/// A Mac reached by address rather than Bonjour, remembered after it first connects.
struct SavedHost: Codable, Identifiable, Equatable {
    var address: String
    var pin: String
    var id: String { address }

    /// Parses "host" or "host:port" (default port otherwise). Returns the endpoint and the bare host.
    static func endpoint(for address: String) -> (NWEndpoint, String)? {
        var host = address
        var port = IFrame.defaultPort
        if let colon = host.lastIndex(of: ":"), !host.contains("::"),
           let p = UInt16(host[host.index(after: colon)...]) {
            port = p
            host = String(host[..<colon])
        }
        guard !host.isEmpty, let nwPort = NWEndpoint.Port(rawValue: port) else { return nil }
        return (.hostPort(host: NWEndpoint.Host(host), port: nwPort), host)
    }
}
