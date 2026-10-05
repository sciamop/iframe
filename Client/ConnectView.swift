import Network
import SwiftUI

// Visual language shared with Pinry Saver: one centered column (max 480 pt) on the system
// background, a big mark up top, labeled rounded fields, and a single pill-shaped action.

extension Color {
    static let iframeTeal = Color(red: 0x2E / 255, green: 0xE6 / 255, blue: 0xD6 / 255)  // #2EE6D6
    /// Text on teal: the accent is light, so dark text reads better than white.
    static let iframeInk = Color(red: 0x04 / 255, green: 0x16 / 255, blue: 0x1A / 255)
}

struct ConnectView: View {
    @EnvironmentObject private var session: StreamSession
    @StateObject private var browser = HostBrowser()
    @AppStorage("pin") private var pin = ""
    @AppStorage("manualHost") private var manualHost = ""
    @AppStorage("uiScale") private var uiScale = 2.0
    @AppStorage("lastHostName") private var lastHostName = ""
    @State private var selectedHostID: String?
    @State private var showingError = false
    @State private var errorMessage = ""

    private var selectedHost: HostBrowser.Found? {
        browser.hosts.first { $0.id == selectedHostID }
    }

    private var trimmedAddress: String { manualHost.trimmingCharacters(in: .whitespaces) }

    private var targetName: String? {
        if !trimmedAddress.isEmpty { return trimmedAddress }
        return selectedHost?.name
    }

    private var canConnect: Bool {
        !pin.isEmpty && targetName != nil && session.phase != .connecting
    }

    var body: some View {
        ZStack {
            Color(uiColor: .systemBackground).ignoresSafeArea()

            ScrollView {
                HStack {
                    Spacer()
                    VStack(alignment: .leading, spacing: 0) {
                        header
                        fields
                        connectButton
                        Spacer(minLength: 20)
                        footer
                    }
                    .frame(maxWidth: 480)
                    .padding(.horizontal, 32)
                    Spacer()
                }
            }
            .scrollDismissesKeyboard(.interactively)

            if session.phase == .connecting {
                connectingCard
            }
        }
        .onAppear { browser.start() }
        .onDisappear { browser.stop() }
        .onChange(of: browser.hosts) { _, hosts in
            // Preselect the Mac used last time, or the only one around.
            guard selectedHostID == nil || !hosts.contains(where: { $0.id == selectedHostID }) else { return }
            selectedHostID = (hosts.first { $0.name == lastHostName } ?? (hosts.count == 1 ? hosts.first : nil))?.id
        }
        .onChange(of: session.phase) { _, phase in
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
        VStack(spacing: 16) {
            Image("IFrameMark")
                .resizable()
                .aspectRatio(contentMode: .fit)
                .frame(width: min(480, UIScreen.main.bounds.width - 64) * 0.36)
            Text("iFrame")
                .font(.system(size: 34, weight: .bold))
            Text("Your Mac, on this iPad. Run iframe-host on the Mac to get started.")
                .font(.system(size: 11))
                .foregroundColor(Color(uiColor: .tertiaryLabel))
                .multilineTextAlignment(.center)
        }
        .frame(maxWidth: .infinity)
        .padding(.top, 56)
        .padding(.bottom, 32)
    }

    private var fields: some View {
        VStack(alignment: .leading, spacing: 24) {
            VStack(alignment: .leading, spacing: 8) {
                FieldLabel("Mac:")
                if browser.hosts.isEmpty {
                    HStack(spacing: 10) {
                        ProgressView()
                        Text("Looking for Macs running iframe-host…")
                            .foregroundColor(Color(uiColor: .secondaryLabel))
                    }
                    .font(.system(size: 15))
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .frameFieldBackground()
                } else {
                    VStack(spacing: 8) {
                        ForEach(browser.hosts) { host in
                            HostRow(name: host.name,
                                    selected: host.id == selectedHostID && trimmedAddress.isEmpty) {
                                selectedHostID = host.id
                                manualHost = ""
                            }
                        }
                    }
                }
            }

            FrameInputField(label: "PIN:", placeholder: "Shown by iframe-host", text: $pin,
                            keyboardType: .numberPad, monospaced: true)

            VStack(alignment: .leading, spacing: 8) {
                FieldLabel("Mac display:")
                Menu {
                    Picker("Mac display", selection: $uiScale) {
                        Text("Match iPad · sharpest").tag(2.0)
                        Text("More space").tag(1.6)
                        Text("Most space").tag(1.33)
                        Text("Use the Mac's own display").tag(0.0)
                    }
                } label: {
                    HStack {
                        Text(displayLabel)
                            .foregroundColor(.primary)
                        Spacer()
                        Image(systemName: "chevron.up.chevron.down")
                            .font(.system(size: 13, weight: .semibold))
                            .foregroundColor(Color(uiColor: .secondaryLabel))
                    }
                    .font(.system(size: 15))
                    .frameFieldBackground()
                }
            }

            FrameInputField(label: "Or connect by address (optional):",
                            placeholder: "192.168.1.20, mac-mini.local, Tailscale name",
                            text: $manualHost, keyboardType: .URL)
        }
    }

    private var displayLabel: String {
        switch uiScale {
        case 2.0: return "Match iPad · sharpest"
        case 1.6: return "More space"
        case 1.33: return "Most space"
        default: return "Use the Mac's own display"
        }
    }

    private var connectButton: some View {
        Button(action: connect) {
            Text(targetName.map { "Connect to \($0)" } ?? "Connect")
                .lineLimit(1)
                .font(.system(size: 17, weight: .semibold))
                .foregroundColor(canConnect ? .iframeInk : Color(uiColor: .secondaryLabel))
                .frame(maxWidth: .infinity)
                .frame(height: 56)
                .background(canConnect ? Color.iframeTeal : Color.gray.opacity(0.3))
                .cornerRadius(28)
        }
        .disabled(!canConnect)
        .padding(.top, 32)
    }

    private var footer: some View {
        VStack(spacing: 0) {
            Divider().padding(.top, 20)
            Text("Default port \(String(IFrame.defaultPort)). Use host:port to override.")
                .font(.system(size: 13))
                .foregroundColor(Color(uiColor: .tertiaryLabel))
                .padding(.top, 12)
                .padding(.bottom, 20)
        }
        .frame(maxWidth: .infinity)
    }

    private var connectingCard: some View {
        ZStack {
            Color.black.opacity(0.5).ignoresSafeArea()
            VStack(spacing: 24) {
                IFrameLoadingView(size: 80)
                Text("Connecting to \(session.hostLabel)…")
                    .font(.system(size: 17, weight: .semibold))
                    .foregroundColor(.primary)
                Button("Cancel") { session.disconnect() }
                    .font(.system(size: 15))
                    .foregroundColor(Color(uiColor: .secondaryLabel))
            }
            .padding(40)
            .background(Color(uiColor: .systemBackground))
            .cornerRadius(20)
            .shadow(radius: 20)
        }
        .transition(.opacity)
    }

    // MARK: Actions

    private func connect() {
        guard canConnect else { return }
        if !trimmedAddress.isEmpty {
            var host = trimmedAddress
            var port = IFrame.defaultPort
            if let colon = host.lastIndex(of: ":"), !host.contains("::"),
               let p = UInt16(host[host.index(after: colon)...]) {
                port = p
                host = String(host[..<colon])
            }
            guard let nwPort = NWEndpoint.Port(rawValue: port) else { return }
            session.connect(to: .hostPort(host: NWEndpoint.Host(host), port: nwPort), pin: pin, label: host, uiScale: uiScale)
        } else if let host = selectedHost {
            lastHostName = host.name
            session.connect(to: host.endpoint, pin: pin, label: host.name, uiScale: uiScale)
        }
    }
}

// MARK: - Components

private struct FieldLabel: View {
    let text: String
    init(_ text: String) { self.text = text }

    var body: some View {
        Text(text)
            .font(.system(size: 15, weight: .regular))
            .foregroundColor(Color(uiColor: .secondaryLabel))
    }
}

private struct FrameInputField: View {
    let label: String
    let placeholder: String
    @Binding var text: String
    var keyboardType: UIKeyboardType = .default
    var monospaced = false

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            FieldLabel(label)
            TextField(placeholder, text: $text)
                .keyboardType(keyboardType)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
                .font(monospaced ? .system(size: 15).monospacedDigit() : .system(size: 15))
                .frameFieldBackground()
        }
    }
}

private struct HostRow: View {
    let name: String
    let selected: Bool
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            HStack(spacing: 12) {
                Image(systemName: "macmini")
                    .font(.system(size: 17))
                    .foregroundColor(selected ? .iframeTeal : Color(uiColor: .secondaryLabel))
                Text(name)
                    .font(.system(size: 15))
                    .foregroundColor(.primary)
                Spacer()
                Image(systemName: selected ? "checkmark.circle.fill" : "circle")
                    .font(.system(size: 20))
                    .foregroundColor(selected ? .iframeTeal : Color(uiColor: .tertiaryLabel))
            }
            .frameFieldBackground(highlighted: selected)
        }
        .buttonStyle(.plain)
    }
}

private extension View {
    /// The rounded, hairline-bordered field box used throughout Pinry Saver.
    func frameFieldBackground(highlighted: Bool = false) -> some View {
        padding(.horizontal, 16)
            .padding(.vertical, 14)
            .background(Color(uiColor: .secondarySystemBackground))
            .cornerRadius(12)
            .overlay(
                RoundedRectangle(cornerRadius: 12)
                    .stroke(highlighted ? Color.iframeTeal : Color(uiColor: .separator),
                            lineWidth: highlighted ? 2 : 1)
            )
    }
}

/// The cursor mark streaking up and to the right, with a short motion trail
/// (iFrame's take on Pinry's falling-pin loader).
struct IFrameLoadingView: View {
    let size: CGFloat
    @State private var progress: CGFloat = -1

    var body: some View {
        ZStack {
            ForEach(0..<3, id: \.self) { index in
                Image("IFrameMark")
                    .resizable()
                    .aspectRatio(contentMode: .fit)
                    .frame(width: size * 0.7, height: size * 0.7)
                    .opacity(1.0 - CGFloat(index) * 0.3)
                    .offset(x: (progress - CGFloat(index) * 0.18) * size,
                            y: -(progress - CGFloat(index) * 0.18) * size)
            }
        }
        .frame(width: size, height: size)
        .clipped()
        .drawingGroup()
        .onAppear {
            progress = -1
            withAnimation(.easeInOut(duration: 0.6).repeatForever(autoreverses: false)) {
                progress = 1
            }
        }
    }
}
