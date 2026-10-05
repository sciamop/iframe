#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>

#include "iframe.h"

// The host sends VideoToolbox's output untouched: parameter sets out of band (FORMAT) and
// access units as 4-byte length-prefixed NAL units. FFmpeg's decoders want Annex B, so each
// length prefix becomes a start code and the parameter sets ride in front of the next keyframe.

struct Decoder {
    bool allow_hw;
    int codec;                       // CODEC_*, -1 before the first FORMAT
    AVCodecContext *ctx;
    AVBufferRef *hw_device;
    enum AVPixelFormat hw_format;
    AVPacket *packet;
    AVFrame *frame;                  // as decoded (may live on the GPU)
    AVFrame *sw_frame;               // downloaded to system memory
    uint8_t *headers;                // Annex B parameter sets, prepended to the next access unit
    size_t headers_len;
    uint8_t *buf;
    size_t buf_cap;
    char description[96];
};

static const enum AVHWDeviceType hw_preference[] = {
    AV_HWDEVICE_TYPE_CUDA,    // NVDEC
    AV_HWDEVICE_TYPE_VAAPI,   // Intel / AMD
    AV_HWDEVICE_TYPE_VULKAN,
};

static enum AVPixelFormat pick_format(AVCodecContext *ctx, const enum AVPixelFormat *formats) {
    Decoder *d = ctx->opaque;
    for (const enum AVPixelFormat *p = formats; *p != AV_PIX_FMT_NONE; p++)
        if (*p == d->hw_format) return *p;
    // The hardware path refused this stream; take the first software format.
    for (const enum AVPixelFormat *p = formats; *p != AV_PIX_FMT_NONE; p++) {
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(*p);
        if (desc && !(desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *p;
    }
    return AV_PIX_FMT_NONE;
}

Decoder *decoder_new(bool allow_hw) {
    Decoder *d = calloc(1, sizeof *d);
    d->allow_hw = allow_hw;
    d->codec = -1;
    d->packet = av_packet_alloc();
    d->frame = av_frame_alloc();
    d->sw_frame = av_frame_alloc();
    snprintf(d->description, sizeof d->description, "no decoder");
    return d;
}

static void close_codec(Decoder *d) {
    avcodec_free_context(&d->ctx);
    av_buffer_unref(&d->hw_device);
    d->hw_format = AV_PIX_FMT_NONE;
}

void decoder_free(Decoder *d) {
    if (!d) return;
    close_codec(d);
    av_packet_free(&d->packet);
    av_frame_free(&d->frame);
    av_frame_free(&d->sw_frame);
    free(d->headers);
    free(d->buf);
    free(d);
}

const char *decoder_description(Decoder *d) { return d->description; }

static bool try_hw(Decoder *d, const AVCodec *codec, enum AVHWDeviceType type) {
    for (int i = 0;; i++) {
        const AVCodecHWConfig *config = avcodec_get_hw_config(codec, i);
        if (!config) return false;
        if (config->device_type == type && (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
            if (av_hwdevice_ctx_create(&d->hw_device, type, NULL, NULL, 0) < 0) return false;
            d->hw_format = config->pix_fmt;
            return true;
        }
    }
}

static bool open_codec(Decoder *d, int which) {
    close_codec(d);
    const AVCodec *codec = avcodec_find_decoder(which == CODEC_HEVC ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264);
    if (!codec) return false;
    d->ctx = avcodec_alloc_context3(codec);
    d->ctx->opaque = d;
    d->ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    d->ctx->get_format = pick_format;
    d->hw_format = AV_PIX_FMT_NONE;

    const char *hw_name = NULL;
    if (d->allow_hw) {
        for (size_t i = 0; i < sizeof hw_preference / sizeof *hw_preference; i++) {
            if (try_hw(d, codec, hw_preference[i])) {
                hw_name = av_hwdevice_get_type_name(hw_preference[i]);
                d->ctx->hw_device_ctx = av_buffer_ref(d->hw_device);
                break;
            }
        }
    }
    // Frame threading would add a frame of latency per thread; slices are free.
    d->ctx->thread_type = FF_THREAD_SLICE;
    d->ctx->thread_count = hw_name ? 1 : 0;

    if (avcodec_open2(d->ctx, codec, NULL) < 0) {
        close_codec(d);
        return false;
    }
    d->codec = which;
    snprintf(d->description, sizeof d->description, "%s %s", which == CODEC_HEVC ? "HEVC" : "H.264",
             hw_name ? hw_name : "software");
    return true;
}

static uint8_t *reserve(Decoder *d, size_t len) {
    if (len > d->buf_cap) {
        size_t cap = len + len / 2;
        uint8_t *p = realloc(d->buf, cap + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!p) return NULL;
        d->buf = p;
        d->buf_cap = cap;
    }
    return d->buf;
}

bool decoder_set_format(Decoder *d, int codec, const uint8_t *const *sets, const uint32_t *sizes, int count) {
    if (count <= 0) return false;
    if (codec != d->codec || !d->ctx) {
        if (!open_codec(d, codec)) return false;
    }
    size_t total = 0;
    for (int i = 0; i < count; i++) total += 4 + sizes[i];
    free(d->headers);
    d->headers = malloc(total);
    d->headers_len = 0;
    for (int i = 0; i < count; i++) {
        memcpy(d->headers + d->headers_len, "\0\0\0\1", 4);
        memcpy(d->headers + d->headers_len + 4, sets[i], sizes[i]);
        d->headers_len += 4 + sizes[i];
    }
    return true;
}

bool decoder_decode(Decoder *d, const uint8_t *data, size_t len, AVFrame **out) {
    if (!d->ctx || len == 0) return false;
    uint8_t *buf = reserve(d, d->headers_len + len);
    if (!buf) return false;
    size_t n = 0;
    if (d->headers_len) {
        memcpy(buf, d->headers, d->headers_len);
        n = d->headers_len;
        d->headers_len = 0;
    }
    // Length prefix -> start code, in place. Both are 4 bytes, so sizes line up.
    for (size_t off = 0; off + 4 <= len;) {
        uint32_t nal = get_u32(data + off);
        if (nal > len - off - 4) return false;
        memcpy(buf + n, "\0\0\0\1", 4);
        memcpy(buf + n + 4, data + off + 4, nal);
        n += 4 + nal;
        off += 4 + nal;
    }
    memset(buf + n, 0, AV_INPUT_BUFFER_PADDING_SIZE);

    d->packet->data = buf;
    d->packet->size = (int)n;
    int err = avcodec_send_packet(d->ctx, d->packet);
    av_packet_unref(d->packet);
    if (err < 0) return false;

    // One frame in, one frame out: the host never sends B-frames.
    bool got = false;
    while (avcodec_receive_frame(d->ctx, d->frame) == 0) {
        AVFrame *src = d->frame;
        if (src->hw_frames_ctx) {
            av_frame_unref(d->sw_frame);
            if (av_hwframe_transfer_data(d->sw_frame, src, 0) < 0) {
                av_frame_unref(d->frame);
                return false;
            }
            av_frame_copy_props(d->sw_frame, src);
            av_frame_unref(d->frame);
            src = d->sw_frame;
        }
        *out = src;
        got = true;
    }
    return got;
}
