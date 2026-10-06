import Network
import SwiftUI

// The iPad connect screen, adapted for a Mac window: one centered column, the mark up top,
// labeled rounded fields, and a single pill-shaped action.

struct MacConnectView: View {
    @EnvironmentObject private var session: StreamSession
    @EnvironmentObject private var window: MainWindow
    @StateObject private var browser = HostBrowser()
    @AppStorage("pin") private var pin = ""
    @AppStorage("manualHost") private var manualHost = ""
    @AppStorage("macDensity") private var density = MacDensity.match.rawValue
    @AppStorage("lastHostName") private var lastHostName = ""
    @AppStorage("savedHosts") private var savedHostsJSON = "[]"
    @State private var pendingSave: SavedHost?
    @State private var selectedHostID: String?
    @State private var showingError = false
    @State private var errorMessage = ""

    private var selectedHost: HostBrowser.Found? {
        browser.hosts.first { $0.id == selectedHostID }
    }

    private var trimmedAddress: String { manualHost.trimmingCharacters(in: .whitespaces) }

    private var savedHosts: [SavedHost] {
        get { (try? JSONDecoder().decode([SavedHost].self, from: Data(savedHostsJSON.utf8))) ?? [] }
        nonmutating set {
            savedHostsJSON = (try? String(decoding: JSONEncoder().encode(newValue), as: UTF8.self)) ?? "[]"
        }
    }

    private var targetName: String? {
        if !trimmedAddress.isEmpty { return trimmedAddress }
        return selectedHost?.name
    }

    private var canConnect: Bool {
        !pin.isEmpty && targetName != nil && session.phase != .connecting
    }

    var body: some View {
        ZStack {
            Color(nsColor: .windowBackgroundColor).ignoresSafeArea()

            ScrollView {
                VStack(alignment: .leading, spacing: 0) {
                    header
                    fields
                    connectButton
                    footer
                }
                .frame(maxWidth: 440)
                .padding(.horizontal, 32)
                .frame(maxWidth: .infinity)
            }

            if session.phase == .connecting {
                connectingCard
            }
        }
        .frame(minWidth: 420, minHeight: 520)
        .onAppear {
            browser.start()
            // `open iFrame.app --args -manualHost <addr> -pin <pin> -autoConnect YES` connects at launch
            // (launch arguments override the saved settings for that run).
            if UserDefaults.standard.bool(forKey: "autoConnect"), session.phase == .idle {
                UserDefaults.standard.removeObject(forKey: "autoConnect")
                DispatchQueue.main.async(execute: connect)
            }
        }
        .onDisappear { browser.stop() }
        .onChange(of: browser.hosts) { _, hosts in
            // Preselect the Mac used last time, or the only one around.
            guard selectedHostID == nil || !hosts.contains(where: { $0.id == selectedHostID }) else { return }
            selectedHostID = (hosts.first { $0.name == lastHostName } ?? (hosts.count == 1 ? hosts.first : nil))?.id
        }
        .onChange(of: session.phase) { _, phase in
            // Only bookmark addresses that actually worked, most recent first.
            if phase == .streaming, let saved = pendingSave {
                savedHosts = [saved] + savedHosts.filter { $0.address != saved.address }
            }
            if phase != .connecting { pendingSave = nil }
            if case .failed(let message) = phase {
                errorMessage = message
                showingError = true
            }
        }
        .alert("Couldn't connect", isPresented: $showingError) {
            Button("OK") {}
        } message: {
            Text(errorMessage)
        }
    }

    // MARK: Sections

    private var header: some View {
        VStack(spacing: 12) {
            Image("IFrameMark")
                .resizable()
                .aspectRatio(contentMode: .fit)
                .frame(width: 112)
            Text("iFrame")
                .font(.system(size: 30, weight: .bold))
            Text("Another Mac, in a window. Run iframe-host on it to get started.")
                .font(.system(size: 11))
                .foregroundColor(Color(nsColor: .tertiaryLabelColor))
                .multilineTextAlignment(.center)
        }
        .frame(maxWidth: .infinity)
        .padding(.top, 36)
        .padding(.bottom, 28)
    }

    private var fields: some View {
        VStack(alignment: .leading, spacing: 20) {
            VStack(alignment: .leading, spacing: 8) {
                FieldLabel("Mac:")
                if browser.hosts.isEmpty && savedHosts.isEmpty {
                    HStack(spacing: 10) {
                        ProgressView().controlSize(.small)
                        Text("Looking for Macs running iframe-host…")
                            .foregroundColor(Color(nsColor: .secondaryLabelColor))
                    }
                    .font(.system(size: 13))
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .frameFieldBackground()
                } else {
                    VStack(spacing: 6) {
                        ForEach(browser.hosts) { host in
                            HostRow(name: host.name,
                                    selected: host.id == selectedHostID && trimmedAddress.isEmpty) {
                                selectedHostID = host.id
                                manualHost = ""
                            }
                        }
                        ForEach(savedHosts) { saved in
                            HostRow(name: saved.address, symbol: "bookmark",
                                    selected: saved.address == trimmedAddress) {
                                manualHost = saved.address
                                pin = saved.pin
                            }
                            .contextMenu {
                                Button("Forget \(saved.address)", systemImage: "trash", role: .destructive) {
                                    savedHosts = savedHosts.filter { $0.address != saved.address }
                                    if trimmedAddress == saved.address { manualHost = "" }
                                }
                            }
                        }
                    }
                }
            }

            FrameInputField(label: "PIN:", placeholder: "Shown by iframe-host", text: $pin, monospaced: true)
                .onSubmit(connect)

            VStack(alignment: .leading, spacing: 8) {
                FieldLabel("Mac display:")
                Menu {
                    Picker("Mac display", selection: $density) {
                        ForEach(MacDensity.allCases) { choice in
                            Text(choice.label).tag(choice.rawValue)
                        }
                    }
                    .pickerStyle(.inline)
                } label: {
                    Text(MacDensity(rawValue: density)?.label ?? MacDensity.match.label)
                        .font(.system(size: 13))
                }
                .menuStyle(.borderlessButton)
                .frameFieldBackground()
                Text(density > 0
                     ? "The Mac gets a virtual display shaped like this window. Resize or go full screen and it follows."
                     : "Shows the Mac's own screen, scaled to fit this window.")
                    .font(.system(size: 11))
                    .foregroundColor(Color(nsColor: .tertiaryLabelColor))
            }

            FrameInputField(label: "Or connect by address (optional):",
                            placeholder: "192.168.1.20, mac-mini.local, Tailscale name",
                            text: $manualHost)
                .onSubmit(connect)
        }
    }

    private var connectButton: some View {
        Button(action: connect) {
            Text(targetName.map { "Connect to \($0)" } ?? "Connect")
                .lineLimit(1)
                .font(.system(size: 15, weight: .semibold))
                .foregroundColor(canConnect ? .iframeInk : Color(nsColor: .secondaryLabelColor))
                .frame(maxWidth: .infinity)
                .frame(height: 44)
                .background(canConnect ? Color.iframeTeal : Color.gray.opacity(0.3))
                .cornerRadius(22)
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .keyboardShortcut(.defaultAction)
        .disabled(!canConnect)
        .padding(.top, 28)
    }

    private var footer: some View {
        VStack(spacing: 0) {
            Divider().padding(.top, 24)
            Text("Default port \(String(IFrame.defaultPort)). Use host:port to override. While streaming, ⌃⌥⌘D disconnects and ⌃⌥⌘F toggles full screen; every other shortcut goes to the remote Mac.")
                .font(.system(size: 11))
                .foregroundColor(Color(nsColor: .tertiaryLabelColor))
                .multilineTextAlignment(.center)
                .padding(.top, 12)
                .padding(.bottom, 20)
        }
        .frame(maxWidth: .infinity)
    }

    private var connectingCard: some View {
        ZStack {
            Color.black.opacity(0.4).ignoresSafeArea()
            VStack(spacing: 20) {
                IFrameLoadingView(size: 64)
                Text("Connecting to \(session.hostLabel)…")
                    .font(.system(size: 15, weight: .semibold))
                Button("Cancel") { session.disconnect() }
                    .keyboardShortcut(.cancelAction)
            }
            .padding(32)
            .background(Color(nsColor: .windowBackgroundColor))
            .cornerRadius(16)
            .shadow(radius: 20)
        }
        .transition(.opacity)
    }

    // MARK: Actions

    private func connect() {
        guard canConnect else { return }
        let choice = MacDensity(rawValue: density) ?? .match
        if !trimmedAddress.isEmpty {
            guard let (endpoint, host) = SavedHost.endpoint(for: trimmedAddress) else { return }
            pendingSave = SavedHost(address: trimmedAddress, pin: pin)
            session.connect(to: endpoint, pin: pin, label: host, window: window, density: choice)
        } else if let host = selectedHost {
            lastHostName = host.name
            session.connect(to: host.endpoint, pin: pin, label: host.name, window: window, density: choice)
        }
    }
}

// MARK: - Components

private struct FieldLabel: View {
    let text: String
    init(_ text: String) { self.text = text }

    var body: some View {
        Text(text)
            .font(.system(size: 13))
            .foregroundColor(Color(nsColor: .secondaryLabelColor))
    }
}

private struct FrameInputField: View {
    let label: String
    let placeholder: String
    @Binding var text: String
    var monospaced = false

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            FieldLabel(label)
            TextField(placeholder, text: $text)
                .textFieldStyle(.plain)
                .autocorrectionDisabled()
                .font(monospaced ? .system(size: 13).monospacedDigit() : .system(size: 13))
                .frameFieldBackground()
        }
    }
}

private struct HostRow: View {
    let name: String
    var symbol = "macmini"
    let selected: Bool
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            HStack(spacing: 10) {
                Image(systemName: symbol)
                    .font(.system(size: 15))
                    .foregroundColor(selected ? .iframeTeal : Color(nsColor: .secondaryLabelColor))
                Text(name)
                    .font(.system(size: 13))
                    .foregroundColor(.primary)
                Spacer()
                Image(systemName: selected ? "checkmark.circle.fill" : "circle")
                    .font(.system(size: 16))
                    .foregroundColor(selected ? .iframeTeal : Color(nsColor: .tertiaryLabelColor))
            }
            .frameFieldBackground(highlighted: selected)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
    }
}

private extension View {
    /// The rounded, hairline-bordered field box used throughout the iPad app.
    func frameFieldBackground(highlighted: Bool = false) -> some View {
        padding(.horizontal, 12)
            .padding(.vertical, 10)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(Color(nsColor: .controlBackgroundColor))
            .cornerRadius(10)
            .overlay(
                RoundedRectangle(cornerRadius: 10)
                    .stroke(highlighted ? Color.iframeTeal : Color(nsColor: .separatorColor),
                            lineWidth: highlighted ? 2 : 1)
            )
    }
}
