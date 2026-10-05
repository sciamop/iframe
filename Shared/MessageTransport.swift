import Foundation

/// A bidirectional stream of protocol messages. The host serves native clients over raw TCP
/// (MessageChannel) and browsers over secure WebSockets (WebSocketTransport) through this.
protocol MessageTransport: AnyObject {
    var onMessage: ((MsgType, Data) -> Void)? { get set }
    /// Called once when the transport closes for any reason (peer, error, or cancel()).
    var onClosed: (() -> Void)? { get set }
    var peerDescription: String { get }
    func start()
    func cancel()
    func sendMessage(_ type: MsgType, _ payload: Data, completion: ((Bool) -> Void)?)
}

extension MessageTransport {
    func sendMessage(_ type: MsgType, _ payload: Data = Data()) {
        sendMessage(type, payload, completion: nil)
    }

    func sendMessage<T: Encodable>(_ type: MsgType, json value: T) {
        guard let data = try? JSONEncoder().encode(value) else { return }
        sendMessage(type, data, completion: nil)
    }
}
