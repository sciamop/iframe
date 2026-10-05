import Foundation
import CoreMedia
import CoreVideo
import VideoToolbox

/// Hardware H.264/HEVC decoder. Frames are decoded synchronously on the caller's queue
/// so the caller knows the moment a frame is ready (and can ack it to the host).
final class VideoDecoder {
    private(set) var formatDescription: CMVideoFormatDescription?
    private var session: VTDecompressionSession?
    var onFrame: ((CVPixelBuffer) -> Void)?

    deinit { invalidate() }

    func invalidate() {
        if let session { VTDecompressionSessionInvalidate(session) }
        session = nil
    }

    @discardableResult
    func setFormat(codec: VideoCodec, parameterSets: [Data]) -> Bool {
        guard !parameterSets.isEmpty else { return false }
        let buffers = parameterSets.map { [UInt8]($0) }
        let storage = buffers.map { bytes -> UnsafeMutablePointer<UInt8> in
            let p = UnsafeMutablePointer<UInt8>.allocate(capacity: max(bytes.count, 1))
            p.initialize(from: bytes, count: bytes.count)
            return p
        }
        defer { storage.forEach { $0.deallocate() } }
        let pointers = storage.map { UnsafePointer($0) }
        let sizes = buffers.map(\.count)

        var format: CMFormatDescription?
        let status: OSStatus
        switch codec {
        case .h264:
            status = CMVideoFormatDescriptionCreateFromH264ParameterSets(
                allocator: kCFAllocatorDefault, parameterSetCount: pointers.count,
                parameterSetPointers: pointers, parameterSetSizes: sizes,
                nalUnitHeaderLength: 4, formatDescriptionOut: &format)
        case .hevc:
            status = CMVideoFormatDescriptionCreateFromHEVCParameterSets(
                allocator: kCFAllocatorDefault, parameterSetCount: pointers.count,
                parameterSetPointers: pointers, parameterSetSizes: sizes,
                nalUnitHeaderLength: 4, extensions: nil, formatDescriptionOut: &format)
        }
        guard status == noErr, let format else { return false }

        if let session {
            if let current = formatDescription, CMFormatDescriptionEqual(current, otherFormatDescription: format) {
                return true
            }
            if VTDecompressionSessionCanAcceptFormatDescription(session, formatDescription: format) {
                formatDescription = format
                return true
            }
        }

        invalidate()
        formatDescription = format
        let attributes: [CFString: Any] = [
            kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
            kCVPixelBufferIOSurfacePropertiesKey: [:] as CFDictionary,
            kCVPixelBufferMetalCompatibilityKey: true,
        ]
        var newSession: VTDecompressionSession?
        let createStatus = VTDecompressionSessionCreate(
            allocator: kCFAllocatorDefault, formatDescription: format, decoderSpecification: nil,
            imageBufferAttributes: attributes as CFDictionary, outputCallback: nil,
            decompressionSessionOut: &newSession)
        guard createStatus == noErr, let newSession else { return false }
        VTSessionSetProperty(newSession, key: kVTDecompressionPropertyKey_RealTime, value: kCFBooleanTrue)
        session = newSession
        return true
    }

    var videoSize: CGSize {
        guard let formatDescription else { return .zero }
        let d = CMVideoFormatDescriptionGetDimensions(formatDescription)
        return CGSize(width: Int(d.width), height: Int(d.height))
    }

    /// Decodes one access unit. Returns false if it could not be decoded (caller should request a keyframe).
    func decode(_ data: Data) -> Bool {
        guard let session, let format = formatDescription, !data.isEmpty else { return false }

        var block: CMBlockBuffer?
        var status = CMBlockBufferCreateWithMemoryBlock(
            allocator: kCFAllocatorDefault, memoryBlock: nil, blockLength: data.count,
            blockAllocator: kCFAllocatorDefault, customBlockSource: nil, offsetToData: 0,
            dataLength: data.count, flags: kCMBlockBufferAssureMemoryNowFlag, blockBufferOut: &block)
        guard status == kCMBlockBufferNoErr, let block else { return false }
        status = data.withUnsafeBytes {
            CMBlockBufferReplaceDataBytes(with: $0.baseAddress!, blockBuffer: block,
                                          offsetIntoDestination: 0, dataLength: data.count)
        }
        guard status == kCMBlockBufferNoErr else { return false }

        var sample: CMSampleBuffer?
        var size = data.count
        status = CMSampleBufferCreateReady(
            allocator: kCFAllocatorDefault, dataBuffer: block, formatDescription: format,
            sampleCount: 1, sampleTimingEntryCount: 0, sampleTimingArray: nil,
            sampleSizeEntryCount: 1, sampleSizeArray: &size, sampleBufferOut: &sample)
        guard status == noErr, let sample else { return false }

        // Without kVTDecodeFrame_EnableAsynchronousDecompression the handler runs before this returns.
        var decoded = false
        let decodeStatus = VTDecompressionSessionDecodeFrame(
            session, sampleBuffer: sample, flags: [._1xRealTimePlayback], infoFlagsOut: nil
        ) { [weak self] status, _, imageBuffer, _, _ in
            guard status == noErr, let imageBuffer else { return }
            decoded = true
            self?.onFrame?(imageBuffer)
        }
        if decodeStatus == kVTInvalidSessionErr {
            // iOS tears decoders down when the app is backgrounded; rebuild on the next format message.
            invalidate()
        }
        return decodeStatus == noErr && decoded
    }
}
