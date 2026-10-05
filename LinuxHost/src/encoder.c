#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>

#include "host.h"

struct Encoder {
    AVCodecContext *ctx;
    AVFrame *frame;
    AVFrame *gpu_frame;
    AVBufferRef *device, *frames;   // CUDA: frames are DMA'd to the GPU, then NVENC reads them in place
    AVPacket *packet;
    int codec;
    int64_t pts;
    char name[64];
    uint8_t *sets_buf, *au_buf;
    size_t cap;
};

static void no_free(void *opaque, uint8_t *data) { (void)opaque; (void)data; }

Encoder *encoder_open(int codec, int width, int height, int fps, int bitrate, char *err, size_t err_cap) {
    const char *name = codec == CODEC_HEVC ? "hevc_nvenc" : "h264_nvenc";
    const AVCodec *av = avcodec_find_encoder_by_name(name);
    if (!av) {
        snprintf(err, err_cap, "%s not available in this FFmpeg", name);
        return NULL;
    }
    Encoder *e = calloc(1, sizeof *e);
    e->codec = codec;
    AVCodecContext *ctx = e->ctx = avcodec_alloc_context3(av);
    ctx->width = width;
    ctx->height = height;
    ctx->time_base = (AVRational){1, fps};
    ctx->framerate = (AVRational){fps, 1};
    ctx->pix_fmt = AV_PIX_FMT_BGR0;     // NVENC converts to 4:2:0 on the GPU
    // Hand NVENC CUDA frames: uploading through CUDA is a DMA copy, where system-memory input
    // is copied by the CPU into NVENC's (write-combined) input buffers — ~3x slower at 1440p.
    if (av_hwdevice_ctx_create(&e->device, AV_HWDEVICE_TYPE_CUDA, NULL, NULL, 0) == 0) {
        e->frames = av_hwframe_ctx_alloc(e->device);
        AVHWFramesContext *fc = (AVHWFramesContext *)e->frames->data;
        fc->format = AV_PIX_FMT_CUDA;
        fc->sw_format = AV_PIX_FMT_BGR0;
        fc->width = width;
        fc->height = height;
        fc->initial_pool_size = 4;
        if (av_hwframe_ctx_init(e->frames) == 0) {
            ctx->pix_fmt = AV_PIX_FMT_CUDA;
            ctx->hw_frames_ctx = av_buffer_ref(e->frames);
        } else {
            av_buffer_unref(&e->frames);
        }
    }
    ctx->max_b_frames = 0;
    ctx->gop_size = fps * 3600;         // keyframes only on demand: periodic ones cause bitrate spikes
    ctx->bit_rate = bitrate;
    ctx->rc_max_rate = bitrate;
    ctx->rc_buffer_size = bitrate / 4;  // caps bursts (a window opening) at ~250 ms of link time
    // NVENC's RGB → YUV conversion uses the BT.601 matrix (measured), so that's what the stream
    // must say; clients pick their YUV → RGB matrix from it. Primaries stay sRGB/BT.709.
    ctx->color_primaries = AVCOL_PRI_BT709;
    ctx->color_trc = AVCOL_TRC_BT709;
    ctx->colorspace = AVCOL_SPC_SMPTE170M;
    ctx->color_range = AVCOL_RANGE_MPEG;
    av_opt_set(ctx->priv_data, "preset", "p2", 0);
    av_opt_set(ctx->priv_data, "tune", "ull", 0);
    av_opt_set(ctx->priv_data, "rc", "cbr", 0);
    av_opt_set(ctx->priv_data, "zerolatency", "1", 0);
    av_opt_set(ctx->priv_data, "delay", "0", 0);
    av_opt_set(ctx->priv_data, "forced-idr", "1", 0);
    av_opt_set(ctx->priv_data, "profile", "main", 0);
    if (codec == CODEC_H264) av_opt_set(ctx->priv_data, "coder", "cabac", 0);
    int rc = avcodec_open2(ctx, av, NULL);
    if (rc < 0) {
        char msg[128];
        av_strerror(rc, msg, sizeof msg);
        snprintf(err, err_cap, "can't open %s at %dx%d: %s", name, width, height, msg);
        encoder_close(e);
        return NULL;
    }
    e->frame = av_frame_alloc();
    e->gpu_frame = av_frame_alloc();
    e->packet = av_packet_alloc();
    snprintf(e->name, sizeof e->name, "%s %s%s", codec == CODEC_HEVC ? "HEVC" : "H.264", name,
             e->frames ? " (CUDA upload)" : "");
    return e;
}

void encoder_close(Encoder *e) {
    if (!e) return;
    avcodec_free_context(&e->ctx);
    av_frame_free(&e->frame);
    av_frame_free(&e->gpu_frame);
    av_buffer_unref(&e->frames);
    av_buffer_unref(&e->device);
    av_packet_free(&e->packet);
    free(e->sets_buf);
    free(e->au_buf);
    free(e);
}

int encoder_codec(Encoder *e) { return e->codec; }
const char *encoder_name(Encoder *e) { return e->name; }

void encoder_set_bitrate(Encoder *e, int bitrate) {
    // FFmpeg's NVENC reconfigures rate control on the next frame when these change.
    e->ctx->bit_rate = bitrate;
    e->ctx->rc_max_rate = bitrate;
    e->ctx->rc_buffer_size = bitrate / 4;
}

static bool is_parameter_set(int codec, uint8_t header) {
    if (codec == CODEC_HEVC) {
        int t = (header >> 1) & 0x3F;
        return t == 32 || t == 33 || t == 34;   // VPS SPS PPS
    }
    int t = header & 0x1F;
    return t == 7 || t == 8;                     // SPS PPS
}

static bool is_delimiter(int codec, uint8_t header) {
    return codec == CODEC_HEVC ? ((header >> 1) & 0x3F) == 35 : (header & 0x1F) == 9;
}

/// Annex B → parameter sets + length-prefixed access unit, the way VideoToolbox hands them over.
static void split(Encoder *e, const AVPacket *pkt, FormatFn on_format, FrameFn on_frame, void *ctx) {
    const uint8_t *d = pkt->data, *end = d + pkt->size;
    if ((size_t)pkt->size + 64 > e->cap) {
        e->cap = pkt->size * 2 + 64;
        e->sets_buf = realloc(e->sets_buf, e->cap);
        e->au_buf = realloc(e->au_buf, e->cap);
    }
    const uint8_t *sets[16];
    uint32_t sizes[16];
    int nsets = 0;
    size_t sets_len = 0, au_len = 0;

    const uint8_t *nal = NULL;
    for (const uint8_t *p = d;; ) {
        // Find the next start code (or the end), which also ends the current NAL.
        const uint8_t *next = p;
        while (next + 3 <= end && !(next[0] == 0 && next[1] == 0 && next[2] == 1)) next++;
        if (next + 3 > end) next = end;
        if (nal) {
            const uint8_t *nal_end = next;
            while (nal_end > nal && nal_end < end && nal_end[-1] == 0) nal_end--;  // zero_byte of a 4-byte code
            if (next == end) nal_end = end;
            size_t len = nal_end - nal;
            if (len > 0 && !is_delimiter(e->codec, nal[0])) {
                if (is_parameter_set(e->codec, nal[0]) && nsets < 16) {
                    memcpy(e->sets_buf + sets_len, nal, len);
                    sets[nsets] = e->sets_buf + sets_len;
                    sizes[nsets++] = (uint32_t)len;
                    sets_len += len;
                } else {
                    put_u32(e->au_buf + au_len, (uint32_t)len);
                    memcpy(e->au_buf + au_len + 4, nal, len);
                    au_len += 4 + len;
                }
            }
        }
        if (next == end) break;
        nal = next + 3;
        p = nal;
    }
    bool key = pkt->flags & AV_PKT_FLAG_KEY;
    if (key && nsets) on_format(ctx, e->codec, sets, sizes, nsets);
    if (au_len) on_frame(ctx, key, e->au_buf, au_len);
}

bool encoder_encode(Encoder *e, const uint8_t *bgrx, int stride, bool keyframe, FormatFn on_format,
                    FrameFn on_frame, void *ctx) {
    AVFrame *f = e->frame;
    av_frame_unref(f);
    f->format = AV_PIX_FMT_BGR0;
    f->width = e->ctx->width;
    f->height = e->ctx->height;
    // Wrap the capture buffer instead of copying it; NVENC uploads it during send_frame.
    f->buf[0] = av_buffer_create((uint8_t *)bgrx, (size_t)stride * f->height, no_free, NULL, AV_BUFFER_FLAG_READONLY);
    f->data[0] = (uint8_t *)bgrx;
    f->linesize[0] = stride;
    if (e->frames) {
        AVFrame *g = e->gpu_frame;
        av_frame_unref(g);
        if (av_hwframe_get_buffer(e->frames, g, 0) < 0 || av_hwframe_transfer_data(g, f, 0) < 0) {
            av_frame_unref(f);
            av_frame_unref(g);
            return false;
        }
        av_frame_unref(f);
        f = g;
    }
    f->pts = e->pts++;
    f->pict_type = keyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    if (keyframe) f->flags |= AV_FRAME_FLAG_KEY;
    int rc = avcodec_send_frame(e->ctx, f);
    av_frame_unref(f);
    if (rc < 0) return false;
    bool got = false;
    while (avcodec_receive_packet(e->ctx, e->packet) == 0) {
        split(e, e->packet, on_format, on_frame, ctx);
        av_packet_unref(e->packet);
        got = true;
    }
    return got;
}
