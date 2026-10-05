import Foundation
import Network

/// Length-prefixed message framing over a TCP NWConnection.
final class MessageChannel {
    let connection: NWConnection
    var onMessage: ((MsgType, Data) -> Void)?
    var onStateChange: ((NWConnection.State) -> Void)?
    private let queue: DispatchQueue

    /// TCP tuned for interactive video: no Nagle delay, fast dead-peer detection,
    /// and the Wi-Fi video access category (WMM AC_VI) so frames jump the queue on the radio.
    static func parameters() -> NWParameters {
        let tcp = NWProtocolTCP.Options()
        tcp.noDelay = true
        tcp.enableKeepalive = true
        tcp.keepaliveIdle = 5
        tcp.keepaliveInterval = 2
        tcp.keepaliveCount = 3
        let params = NWParameters(tls: nil, tcp: tcp)
        params.serviceClass = .interactiveVideo
        return params
    }

    init(connection: NWConnection, queue: DispatchQueue) {
        self.connection = connection
        self.queue = queue
    }

    func start() {
        connection.stateUpdateHandler = { [weak self] state in self?.onStateChange?(state) }
        connection.start(queue: queue)
        receiveHeader()
    }

    func cancel() {
        connection.cancel()
    }

    func send(_ type: MsgType, _ payload: Data = Data(), completion: ((NWError?) -> Void)? = nil) {
        var packet = Data(capacity: 5 + payload.count)
        packet.append(type.rawValue)
        withUnsafeBytes(of: UInt32(payload.count).bigEndian) { packet.append(contentsOf: $0) }
        packet.append(payload)
        connection.send(content: packet, completion: .contentProcessed { error in completion?(error) })
    }

    func send<T: Encodable>(_ type: MsgType, json value: T) {
        guard let data = try? JSONEncoder().encode(value) else { return }
        send(type, data)
    }

    private func receiveHeader() {
        connection.receive(minimumIncompleteLength: 5, maximumLength: 5) { [weak self] data, _, isComplete, error in
            guard let self else { return }
            guard let data, data.count == 5, error == nil else {
                if isComplete || error != nil { self.connection.cancel() }
                return
            }
            let b = [UInt8](data)
            let length = Int(b[1]) << 24 | Int(b[2]) << 16 | Int(b[3]) << 8 | Int(b[4])
            guard length <= IFrame.maxMessageSize else {
                self.connection.cancel()
                return
            }
            if length == 0 {
                self.deliver(b[0], Data())
                self.receiveHeader()
            } else {
                self.receiveBody(type: b[0], length: length)
            }
        }
    }

    private func receiveBody(type: UInt8, length: Int) {
        connection.receive(minimumIncompleteLength: length, maximumLength: length) { [weak self] data, _, _, error in
            guard let self else { return }
            guard let data, data.count == length, error == nil else {
                self.connection.cancel()
                return
            }
            self.deliver(type, data)
            self.receiveHeader()
        }
    }

    private func deliver(_ raw: UInt8, _ data: Data) {
        guard let type = MsgType(rawValue: raw) else { return }
        onMessage?(type, data)
    }
}
