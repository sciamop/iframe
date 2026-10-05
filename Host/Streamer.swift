import Foundation
import CoreMedia
import CoreVideo
import ScreenCaptureKit
import VideoToolbox

/// Captures a display with ScreenCaptureKit and encodes it on the Apple silicon media engine.
///
/// Latency design:
///  - Capture is delivered as IOSurface-backed 4:2:0 buffers, which the encoder consumes directly
///    (no color conversion, no CPU copies).
///  - The encoder runs in real-time mode with no frame reordering. (VideoToolbox's
///    "low-latency rate control" mode is measurably *slower* on M-series chips: ~13 ms/frame
///    at iPad Pro resolution vs ~5.5 ms in standard real-time mode.)
///  - At most one frame is ever inside the encoder. If capture outruns it, only the newest
///    frame waits, so nothing queues up behind a stale screen.
///  - Network flow control is driven by client acks: at most `maxInflight` frames may be
///    unacknowledged. When the link backs up we skip capture frames *before* encoding (keeping
///    the reference chain intact) instead of letting them pile up in socket buffers, and always
///    send the newest screen as soon as the window reopens.
///  - When the screen goes idle we re-encode the last frame a couple of times so text sharpens
///    to near-lossless without spending bandwidth while things are moving.
final class Streamer: NSObject, SCStreamOutput, SCStreamDelegate {
    struct Config {
        var fps: Int
        var bitrate: Int
        var minBitrate: Int
        var maxBitrate: Int
        var maxInflight: Int
    }

    let display: SCDisplay
    let width: Int
    let height: Int
    let fps: Int
    private(set) var codec: VideoCodec
    private(set) var encoderDescription = ""

    var onFormat: ((VideoCodec, [Data]) -> Void)?
    var onFrame: ((UInt32, UInt64, Bool, Data) -> Void)?
    var onStats: ((HostStats) -> Void)?
    var onStop: ((Error?) -> Void)?

    private let config: Config
    private var bitrate: Int
    private var stream: SCStream?
    private var encoder: VTCompressionSession?
    private let queue = DispatchQueue(label: "iframe.capture", qos: .userInteractive)

    // Shared between the capture queue, the encoder callback and the network queue.
    private let lock = NSLock()
    private var encodingCount = 0
    private var lastSentId: UInt32 = 0
    private var lastAckedId: UInt32 = 0
    private var sendTimes: [UInt32: UInt64] = [:]
    private var forceKeyframe = true
    private var statFrames = 0
    private var statBytes = 0
    private var statEncodeNanos: UInt64 = 0
    private var statLatencyNanos: UInt64 = 0
    private var statAcks = 0
    private var statDropped = 0

    // Capture queue only.
    private var latestBuffer: CVPixelBuffer?
    private var pendingBuffer: CVPixelBuffer?
    private var refineWork: DispatchWorkItem?
    private var refinePasses = 0
    private var lastDropTime: UInt64 = 0
    private var statsTimer: DispatchSourceTimer?
    private var stopped = false

    init(display: SCDisplay, codec preferred: VideoCodec, config: Config, captureSize: (width: Int, height: Int)? = nil) throws {
        self.display = display
        self.config = config
        self.fps = config.fps
        self.bitrate = config.bitrate
        let mode = CGDisplayCopyDisplayMode(display.displayID)
        self.width = (captureSize?.width ?? mode?.pixelWidth ?? display.width) & ~1
        self.height = (captureSize?.height ?? mode?.pixelHeight ?? display.height) & ~1
        self.codec = preferred
        super.init()

        var attempts: [(VideoCodec, Bool)] = [(preferred, false), (preferred, true)]
        if preferred == .hevc { attempts += [(.h264, false), (.h264, true)] }
        for (codec, lowLatency) in attempts {
            if let session = Self.makeSession(codec: codec, width: width, height: height, lowLatency: lowLatency) {
                self.codec = codec
                self.encoder = session
                self.encoderDescription = "\(codec.name) hardware encoder"
                    + (lowLatency ? ", low-latency rate control" : ", standard rate control")
                configure(session)
                return
            }
        }
        throw HostError("could not create a hardware video encoder for \(width)x\(height)")
    }

    // MARK: Encoder

    private static func makeSession(codec: VideoCodec, width: Int, height: Int, lowLatency: Bool) -> VTCompressionSession? {
        var spec: [CFString: Any] = [kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder: true]
        if lowLatency { spec[kVTVideoEncoderSpecification_EnableLowLatencyRateControl] = true }
        let source: [CFString: Any] = [
            kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
            kCVPixelBufferWidthKey: width,
            kCVPixelBufferHeightKey: height,
            kCVPixelBufferIOSurfacePropertiesKey: [:] as CFDictionary,
        ]
        var session: VTCompressionSession?
        let status = VTCompressionSessionCreate(
            allocator: nil, width: Int32(width), height: Int32(height),
            codecType: codec == .hevc ? kCMVideoCodecType_HEVC : kCMVideoCodecType_H264,
            encoderSpecification: spec as CFDictionary, imageBufferAttributes: source as CFDictionary,
            compressedDataAllocator: nil, outputCallback: nil, refcon: nil, compressionSessionOut: &session)
        return status == noErr ? session : nil
    }

    private func configure(_ session: VTCompressionSession) {
        func set(_ key: CFString, _ value: CFTypeRef) {
            let status = VTSessionSetProperty(session, key: key, value: value)
            if status != noErr { hostLog("encoder: \(key) not supported (\(status))") }
        }
        set(kVTCompressionPropertyKey_RealTime, kCFBooleanTrue)
        set(kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse)
        set(kVTCompressionPropertyKey_ProfileLevel,
            codec == .hevc ? kVTProfileLevel_HEVC_Main_AutoLevel : kVTProfileLevel_H264_High_AutoLevel)
        if codec == .h264 { set(kVTCompressionPropertyKey_H264EntropyMode, kVTH264EntropyMode_CABAC) }
        applyBitrate(session)
        set(kVTCompressionPropertyKey_ExpectedFrameRate, NSNumber(value: fps))
        // Keyframes only on demand (connect, decode error): periodic ones cause bitrate spikes.
        set(kVTCompressionPropertyKey_MaxKeyFrameInterval, NSNumber(value: fps * 3600))
        set(kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality, kCFBooleanTrue)
        set(kVTCompressionPropertyKey_MaximizePowerEfficiency, kCFBooleanFalse)
        set(kVTCompressionPropertyKey_ColorPrimaries, kCVImageBufferColorPrimaries_ITU_R_709_2)
        set(kVTCompressionPropertyKey_TransferFunction, kCVImageBufferTransferFunction_ITU_R_709_2)
        set(kVTCompressionPropertyKey_YCbCrMatrix, kCVImageBufferYCbCrMatrix_ITU_R_709_2)
        VTCompressionSessionPrepareToEncodeFrames(session)
    }

    /// Average target plus a hard cap on bursts (e.g. a big window opening) so a single frame
    /// can't monopolize the link for long.
    private func applyBitrate(_ session: VTCompressionSession) {
        VTSessionSetProperty(session, key: kVTCompressionPropertyKey_AverageBitRate, value: NSNumber(value: bitrate))
        let burstBytes = Double(bitrate) / 8 * 1.5 / 4
        VTSessionSetProperty(session, key: kVTCompressionPropertyKey_DataRateLimits,
                             value: [NSNumber(value: burstBytes), NSNumber(value: 0.25)] as CFArray)
    }

    // MARK: Lifecycle

    func start() async throws {
        let filter = SCContentFilter(display: display, excludingWindows: [])
        let cfg = SCStreamConfiguration()
        cfg.width = width
        cfg.height = height
        cfg.minimumFrameInterval = CMTime(value: 1, timescale: CMTimeScale(fps))
        cfg.pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
        cfg.colorMatrix = CGDisplayStream.yCbCrMatrix_ITU_R_709_2
        cfg.colorSpaceName = CGColorSpace.sRGB
        cfg.showsCursor = true
        cfg.queueDepth = 5
        cfg.capturesAudio = false
        if #available(macOS 14.0, *) { cfg.captureResolution = .best }

        let stream = SCStream(filter: filter, configuration: cfg, delegate: self)
        try stream.addStreamOutput(self, type: .screen, sampleHandlerQueue: queue)
        try await stream.startCapture()
        self.stream = stream
        queue.async { self.startStatsTimer() }
    }

    func stop() {
        onFrame = nil
        onFormat = nil
        onStats = nil
        onStop = nil
        let stream = self.stream
        self.stream = nil
        stream?.stopCapture { _ in }
        queue.async {
            self.stopped = true
            self.statsTimer?.cancel()
            self.refineWork?.cancel()
            self.latestBuffer = nil
            self.pendingBuffer = nil
            if let encoder = self.encoder {
                VTCompressionSessionInvalidate(encoder)
                self.encoder = nil
            }
        }
    }

    func stream(_ stream: SCStream, didStopWithError error: Error) {
        onStop?(error)
    }

    // MARK: Capture -> encode

    func stream(_ stream: SCStream, didOutputSampleBuffer sampleBuffer: CMSampleBuffer, of type: SCStreamOutputType) {
        guard type == .screen, !stopped, CMSampleBufferIsValid(sampleBuffer),
              let infos = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, createIfNecessary: false) as? [[SCStreamFrameInfo: Any]],
              let rawStatus = infos.first?[.status] as? Int,
              SCFrameStatus(rawValue: rawStatus) == .complete,
              let pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer)
        else { return }
        latestBuffer = pixelBuffer
        refinePasses = 0
        submit(pixelBuffer)
        scheduleRefine()
    }

    /// Capture queue only.
    private func submit(_ pixelBuffer: CVPixelBuffer) {
        guard let encoder, !stopped else { return }
        lock.lock()
        if encodingCount > 0 {
            // Encoder busy: hold only the newest frame; it goes in as soon as the encoder frees up.
            lock.unlock()
            pendingBuffer = pixelBuffer
            return
        }
        if Int(lastSentId &- lastAckedId) >= config.maxInflight {
            statDropped += 1
            lock.unlock()
            pendingBuffer = pixelBuffer
            lastDropTime = nowNanos()
            return
        }
        encodingCount += 1
        let keyframe = forceKeyframe
        forceKeyframe = false
        lock.unlock()
        pendingBuffer = nil

        let start = nowNanos()
        let properties = keyframe ? [kVTEncodeFrameOptionKey_ForceKeyFrame: kCFBooleanTrue!] as CFDictionary : nil
        let status = VTCompressionSessionEncodeFrame(
            encoder, imageBuffer: pixelBuffer,
            presentationTimeStamp: CMTime(value: CMTimeValue(start), timescale: 1_000_000_000),
            duration: .invalid, frameProperties: properties, infoFlagsOut: nil
        ) { [weak self] status, _, sample in
            self?.encoded(status: status, sample: sample, start: start)
        }
        if status != noErr {
            lock.lock()
            encodingCount -= 1
            if keyframe { forceKeyframe = true }
            lock.unlock()
        }
    }

    /// Encoder callback thread. Callbacks arrive in submission order.
    private func encoded(status: OSStatus, sample: CMSampleBuffer?, start: UInt64) {
        guard status == noErr, let sample, let block = CMSampleBufferGetDataBuffer(sample) else {
            lock.lock()
            encodingCount -= 1
            if status != noErr { forceKeyframe = true }
            lock.unlock()
            submitPending()
            return
        }
        let attachments = CMSampleBufferGetSampleAttachmentsArray(sample, createIfNecessary: false) as? [[CFString: Any]]
        let isKeyframe = !(attachments?.first?[kCMSampleAttachmentKey_NotSync] as? Bool ?? false)

        let length = CMBlockBufferGetDataLength(block)
        var data = Data(count: length)
        let copyStatus = data.withUnsafeMutableBytes {
            CMBlockBufferCopyDataBytes(block, atOffset: 0, dataLength: length, destination: $0.baseAddress!)
        }
        let now = nowNanos()
        lock.lock()
        encodingCount -= 1
        guard copyStatus == kCMBlockBufferNoErr else {
            forceKeyframe = true
            lock.unlock()
            submitPending()
            return
        }
        lastSentId &+= 1
        let id = lastSentId
        sendTimes[id] = now
        statFrames += 1
        statBytes += length
        statEncodeNanos += now - start
        lock.unlock()

        if isKeyframe, let format = CMSampleBufferGetFormatDescription(sample) {
            onFormat?(codec, Self.parameterSets(format, codec: codec))
        }
        onFrame?(id, start, isKeyframe, data)
        submitPending()
    }

    private func submitPending() {
        queue.async { [weak self] in
            guard let self, let pending = self.pendingBuffer else { return }
            self.submit(pending)
        }
    }

    private func scheduleRefine() {
        refineWork?.cancel()
        guard refinePasses < 2 else { return }
        let work = DispatchWorkItem { [weak self] in
            guard let self, let buffer = self.latestBuffer else { return }
            self.refinePasses += 1
            self.submit(buffer)
            self.scheduleRefine()
        }
        refineWork = work
        queue.asyncAfter(deadline: .now() + .milliseconds(refinePasses == 0 ? 150 : 400), execute: work)
    }

    // MARK: Feedback from the client (any thread)

    func ack(_ id: UInt32) {
        let now = nowNanos()
        lock.lock()
        if id > lastAckedId { lastAckedId = id }
        if let sent = sendTimes[id] {
            statLatencyNanos += now - sent
            statAcks += 1
        }
        sendTimes = sendTimes.filter { $0.key > id }
        lock.unlock()
        submitPending()
    }

    func requestKeyframe() {
        lock.lock()
        forceKeyframe = true
        lock.unlock()
        queue.async { [weak self] in
            guard let self, let buffer = self.latestBuffer else { return }
            self.submit(buffer)
        }
    }

    // MARK: Stats + adaptive bitrate

    private func startStatsTimer() {
        let timer = DispatchSource.makeTimerSource(queue: queue)
        timer.schedule(deadline: .now() + 1, repeating: 1)
        timer.setEventHandler { [weak self] in self?.tick() }
        timer.resume()
        statsTimer = timer
    }

    private func tick() {
        lock.lock()
        let frames = statFrames, bytes = statBytes, encodeNanos = statEncodeNanos
        let latencyNanos = statLatencyNanos, acks = statAcks, dropped = statDropped
        statFrames = 0; statBytes = 0; statEncodeNanos = 0
        statLatencyNanos = 0; statAcks = 0; statDropped = 0
        lock.unlock()

        // Back off quickly when the link can't keep up, creep back up once it has been clean for a while.
        var newBitrate = bitrate
        if dropped > 2 {
            newBitrate = max(config.minBitrate, Int(Double(bitrate) * 0.75))
        } else if nowNanos() - lastDropTime > 3_000_000_000, frames > fps / 4 {
            newBitrate = min(config.maxBitrate, Int(Double(bitrate) * 1.1))
        }
        if newBitrate != bitrate, let encoder {
            bitrate = newBitrate
            applyBitrate(encoder)
        }

        onStats?(HostStats(
            fps: Double(frames),
            mbps: Double(bytes) * 8 / 1_000_000,
            targetMbps: Double(bitrate) / 1_000_000,
            encodeMs: frames > 0 ? Double(encodeNanos) / Double(frames) / 1_000_000 : 0,
            latencyMs: acks > 0 ? Double(latencyNanos) / Double(acks) / 1_000_000 : 0,
            dropped: dropped))
    }

    // MARK: Helpers

    static func parameterSets(_ format: CMFormatDescription, codec: VideoCodec) -> [Data] {
        func get(_ index: Int, _ pointer: UnsafeMutablePointer<UnsafePointer<UInt8>?>?,
                 _ size: UnsafeMutablePointer<Int>?, _ count: UnsafeMutablePointer<Int>?) -> OSStatus {
            switch codec {
            case .h264:
                return CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                    format, parameterSetIndex: index, parameterSetPointerOut: pointer,
                    parameterSetSizeOut: size, parameterSetCountOut: count, nalUnitHeaderLengthOut: nil)
            case .hevc:
                return CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
                    format, parameterSetIndex: index, parameterSetPointerOut: pointer,
                    parameterSetSizeOut: size, parameterSetCountOut: count, nalUnitHeaderLengthOut: nil)
            }
        }
        var count = 0
        guard get(0, nil, nil, &count) == noErr else { return [] }
        return (0..<count).compactMap { index in
            var pointer: UnsafePointer<UInt8>?
            var size = 0
            guard get(index, &pointer, &size, nil) == noErr, let pointer else { return nil }
            return Data(bytes: pointer, count: size)
        }
    }
}

struct HostError: LocalizedError {
    let message: String
    init(_ message: String) { self.message = message }
    var errorDescription: String? { message }
}
