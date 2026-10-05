import Network
import SwiftUI

@main
struct IFrameApp: App {
    @StateObject private var session = StreamSession()
    @Environment(\.scenePhase) private var scenePhase

    var body: some Scene {
        WindowGroup {
            Group {
                if session.phase == .streaming {
                    StreamScreen()
                } else {
                    ConnectView()
                }
            }
            .environmentObject(session)
            .onChange(of: scenePhase) { _, phase in
                // iOS suspends sockets in the background; drop cleanly instead of freezing.
                if phase == .background, session.phase != .idle { session.disconnect() }
            }
        }
    }
}

final class HostBrowser: ObservableObject {
    struct Found: Identifiable, Hashable {
        let id: String
        let name: String
        let endpoint: NWEndpoint
    }

    @Published private(set) var hosts: [Found] = []
    private var browser: NWBrowser?

    func start() {
        guard browser == nil else { return }
        let browser = NWBrowser(for: .bonjour(type: IFrame.serviceType, domain: nil), using: .tcp)
        browser.browseResultsChangedHandler = { [weak self] results, _ in
            let found = results.compactMap { result -> Found? in
                guard case let .service(name, _, _, _) = result.endpoint else { return nil }
                return Found(id: "\(result.endpoint)", name: name, endpoint: result.endpoint)
            }
            self?.hosts = found.sorted { $0.name < $1.name }
        }
        browser.start(queue: .main)
        self.browser = browser
    }

    func stop() {
        browser?.cancel()
        browser = nil
    }
}
