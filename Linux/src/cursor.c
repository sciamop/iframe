#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>

#include "iframe.h"

// The host sends the Mac's cursor as a PNG. FFmpeg (already linked for video) decodes PNG,
// so this needs no image library.

bool cursor_decode_png(const uint8_t *png, size_t len, uint8_t **rgba, int *width, int *height) {
    *rgba = NULL;
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_PNG);
    if (!codec) return false;
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    bool ok = false;
    if (!ctx || !pkt || !frame || avcodec_open2(ctx, codec, NULL) < 0) goto done;
    if (av_new_packet(pkt, (int)len) < 0) goto done;
    memcpy(pkt->data, png, len);
    if (avcodec_send_packet(ctx, pkt) < 0 || avcodec_receive_frame(ctx, frame) < 0) goto done;

    int w = frame->width, h = frame->height;
    if (w <= 0 || h <= 0 || w > 512 || h > 512) goto done;
    if (frame->format != AV_PIX_FMT_RGBA && frame->format != AV_PIX_FMT_RGB24 && frame->format != AV_PIX_FMT_PAL8)
        goto done;
    uint8_t *out = malloc((size_t)w * h * 4);
    if (!out) goto done;
    for (int y = 0; y < h; y++) {
        const uint8_t *src = frame->data[0] + (size_t)y * frame->linesize[0];
        uint8_t *dst = out + (size_t)y * w * 4;
        if (frame->format == AV_PIX_FMT_RGBA) {
            memcpy(dst, src, (size_t)w * 4);
        } else if (frame->format == AV_PIX_FMT_PAL8) {
            const uint32_t *palette = (const uint32_t *)frame->data[1];   // native-endian ARGB
            for (int x = 0; x < w; x++) {
                uint32_t c = palette[src[x]];
                dst[4 * x] = c >> 16;
                dst[4 * x + 1] = c >> 8;
                dst[4 * x + 2] = c;
                dst[4 * x + 3] = c >> 24;
            }
        } else {
            for (int x = 0; x < w; x++) {
                dst[4 * x] = src[3 * x];
                dst[4 * x + 1] = src[3 * x + 1];
                dst[4 * x + 2] = src[3 * x + 2];
                dst[4 * x + 3] = 255;
            }
        }
    }
    *rgba = out;
    *width = w;
    *height = h;
    ok = true;
done:
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    return ok;
}
