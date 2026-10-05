// A stand-in for iframe-host, for testing the Linux client without a Mac.
//
// Speaks the same protocol: checks the PIN, sends WELCOME, then FORMAT + length-prefixed
// HEVC/H.264 frames of a moving test pattern sized to the client's display request, with
// ack-driven flow control (max 3 in flight). Logs every input message it receives.
//
//   build/fake-host [--port 7878] [--pin 1234] [--codec hevc|h264] [--fps 60]

#define _GNU_SOURCE
#include <getopt.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>

#include "../src/iframe.h"
#include "../src/net.h"

static int port = IFRAME_DEFAULT_PORT, fps = 60;
static const char *pin = "1234";
static int want_codec = CODEC_HEVC;

static pthread_mutex_t send_lock = PTHREAD_MUTEX_INITIALIZER;
static int client = -1;
static atomic_uint last_acked, last_sent;
static atomic_int req_w = 1280, req_h = 800;
static atomic_bool restart, want_keyframe, alive;
static int mouse_x = -1, mouse_y = -1;   // pixels, drawn as a cursor

static void send_msg(uint8_t type, const void *p, uint32_t len) {
    uint8_t h[5] = { type };
    put_u32(h + 1, len);
    pthread_mutex_lock(&send_lock);
    net_write_full(client, h, 5);
    if (len) net_write_full(client, p, len);
    pthread_mutex_unlock(&send_lock);
}

static const AVCodec *find_encoder(int codec, const char **name) {
    static const char *hevc[] = { "hevc_nvenc", "libx265", "hevc_vaapi", NULL };
    static const char *h264[] = { "h264_nvenc", "libx264", NULL };
    for (const char **n = codec == CODEC_HEVC ? hevc : h264; *n; n++) {
        const AVCodec *c = avcodec_find_encoder_by_name(*n);
        if (c) { *name = *n; return c; }
    }
    return NULL;
}

static AVCodecContext *open_encoder(int w, int h, const char **name) {
    const AVCodec *codec = find_encoder(want_codec, name);
    if (!codec) return NULL;
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    ctx->width = w;
    ctx->height = h;
    ctx->time_base = (AVRational){1, fps};
    ctx->framerate = (AVRational){fps, 1};
    ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    ctx->max_b_frames = 0;
    ctx->gop_size = 100000;   // keyframes only on demand, like the host
    ctx->bit_rate = 8000000;
    ctx->color_primaries = AVCOL_PRI_BT709;
    ctx->colorspace = AVCOL_SPC_BT709;
    ctx->color_trc = AVCOL_TRC_BT709;
    if (strstr(*name, "nvenc")) {
        av_opt_set(ctx->priv_data, "preset", "p1", 0);
        av_opt_set(ctx->priv_data, "tune", "ull", 0);
        av_opt_set(ctx->priv_data, "zerolatency", "1", 0);
    } else if (!strcmp(*name, "libx265")) {
        av_opt_set(ctx->priv_data, "preset", "ultrafast", 0);
        av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);
        av_opt_set(ctx->priv_data, "x265-params", "log-level=error:repeat-headers=1", 0);
    } else if (!strcmp(*name, "libx264")) {
        av_opt_set(ctx->priv_data, "preset", "ultrafast", 0);
        av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);
    }
    if (avcodec_open2(ctx, codec, NULL) < 0) { avcodec_free_context(&ctx); return NULL; }
    return ctx;
}

static bool is_parameter_set(int codec, const uint8_t *nal) {
    if (codec == CODEC_HEVC) {
        int t = (nal[0] >> 1) & 0x3F;
        return t == 32 || t == 33 || t == 34;   // VPS SPS PPS
    }
    int t = nal[0] & 0x1F;
    return t == 7 || t == 8;                     // SPS PPS
}

/// Annex B -> (parameter sets, length-prefixed access unit without them), like VideoToolbox's split.
static void send_packet(AVPacket *pkt, uint32_t id) {
    const uint8_t *d = pkt->data, *end = d + pkt->size;
    const uint8_t *nals[64];
    size_t sizes[64];
    int n = 0;
    const uint8_t *p = d;
    while (p + 3 <= end && n < 64) {
        if (p[0] == 0 && p[1] == 0 && (p[2] == 1 || (p + 4 <= end && p[2] == 0 && p[3] == 1))) {
            p += p[2] == 1 ? 3 : 4;
            nals[n++] = p;
        } else {
            p++;
        }
    }
    if (n == 0) return;
    sizes[n - 1] = end - nals[n - 1];
    // Each NAL ends where the next start code (3 or 4 bytes) begins.
    for (int i = 0; i + 1 < n; i++) {
        const uint8_t *next = nals[i + 1] - 3;
        if (next > nals[i] && next[-1] == 0) next--;
        sizes[i] = next - nals[i];
    }

    bool key = pkt->flags & AV_PKT_FLAG_KEY;
    uint8_t *format = malloc(pkt->size + 64), *frame = malloc(pkt->size + 64 + 13);
    size_t flen = 2, alen = 13;
    int sets = 0;
    for (int i = 0; i < n; i++) {
        if (is_parameter_set(want_codec, nals[i])) {
            put_u32(format + flen, sizes[i]);
            memcpy(format + flen + 4, nals[i], sizes[i]);
            flen += 4 + sizes[i];
            sets++;
        } else {
            put_u32(frame + alen, sizes[i]);
            memcpy(frame + alen + 4, nals[i], sizes[i]);
            alen += 4 + sizes[i];
        }
    }
    if (key && sets) {
        format[0] = want_codec;
        format[1] = sets;
        send_msg(MSG_FORMAT, format, flen);
    }
    put_u32(frame, id);
    put_u64(frame + 4, now_nanos());
    frame[12] = key;
    send_msg(MSG_FRAME, frame, alen);
    free(format);
    free(frame);
}

static void draw(AVFrame *f, int t) {
    int w = f->width, h = f->height;
    for (int y = 0; y < h; y++) {
        uint8_t *row = f->data[0] + y * f->linesize[0];
        for (int x = 0; x < w; x++) row[x] = (uint8_t)(((x / 64) + (y / 64)) % 2 ? 60 : 190);
    }
    for (int y = 0; y < h / 2; y++) {
        memset(f->data[1] + y * f->linesize[1], 128 + (int)(60 * ((float)y / (h / 2)) - 30), w / 2);
        memset(f->data[2] + y * f->linesize[2], 128 + (int)(60 * ((float)(t % 120) / 120) - 30), w / 2);
    }
    // A bar sweeping across shows motion; a white square marks where the client's mouse is.
    int bx = (t * 8) % w;
    for (int y = 0; y < h; y++)
        for (int x = bx; x < bx + 16 && x < w; x++) f->data[0][y * f->linesize[0] + x] = 235;
    if (mouse_x >= 0)
        for (int y = mouse_y; y < mouse_y + 20 && y < h; y++)
            for (int x = mouse_x; x < mouse_x + 20 && x < w; x++) f->data[0][y * f->linesize[0] + x] = 255;
}

static void *stream_thread(void *unused) {
    while (atomic_load(&alive)) {
        int w = atomic_load(&req_w) & ~1, h = atomic_load(&req_h) & ~1;
        const char *name = "?";
        AVCodecContext *enc = open_encoder(w, h, &name);
        if (!enc) { fprintf(stderr, "fake-host: no encoder\n"); exit(1); }
        char welcome[256];
        int n = snprintf(welcome, sizeof welcome,
                         "{\"width\":%d,\"height\":%d,\"pointWidth\":%d,\"pointHeight\":%d,\"codec\":%d,\"fps\":%d,"
                         "\"hostName\":\"Fake \\\"Mac\\\"\",\"isVirtual\":true}",
                         w, h, w / 2, h / 2, want_codec, fps);
        send_msg(MSG_WELCOME, welcome, n);
        fprintf(stderr, "fake-host: streaming %dx%d with %s\n", w, h, name);
        AVFrame *frame = av_frame_alloc();
        frame->width = w; frame->height = h; frame->format = AV_PIX_FMT_YUV420P;
        av_frame_get_buffer(frame, 0);
        AVPacket *pkt = av_packet_alloc();
        int t = 0, sent = 0, skipped = 0;
        uint64_t next = now_nanos(), stats_at = now_nanos() + 1000000000ull;
        atomic_store(&restart, false);
        while (atomic_load(&alive) && !atomic_load(&restart)) {
            next += 1000000000ull / fps;
            uint64_t now = now_nanos();
            if (next > now) usleep((next - now) / 1000);
            t++;
            if (atomic_load(&last_sent) - atomic_load(&last_acked) >= 3) { skipped++; continue; }
            av_frame_make_writable(frame);
            draw(frame, t);
            frame->pts = t;
            frame->pict_type = AV_PICTURE_TYPE_NONE;
            if (atomic_exchange(&want_keyframe, false) || sent == 0) {
                frame->pict_type = AV_PICTURE_TYPE_I;
                frame->flags |= AV_FRAME_FLAG_KEY;
            } else {
                frame->flags &= ~AV_FRAME_FLAG_KEY;
            }
            avcodec_send_frame(enc, frame);
            while (avcodec_receive_packet(enc, pkt) == 0) {
                uint32_t id = atomic_fetch_add(&last_sent, 1) + 1;
                send_packet(pkt, id);
                av_packet_unref(pkt);
                sent++;
            }
            if (now_nanos() > stats_at) {
                stats_at += 1000000000ull;
                char s[160];
                int m = snprintf(s, sizeof s, "{\"fps\":%d,\"mbps\":8,\"targetMbps\":8,\"encodeMs\":1,\"latencyMs\":2,"
                                 "\"dropped\":%d}", sent, skipped);
                send_msg(MSG_STATS, s, m);
                sent = sent ? 1 : 0;
                skipped = 0;
            }
        }
        av_packet_free(&pkt);
        av_frame_free(&frame);
        avcodec_free_context(&enc);
    }
    return NULL;
}

static void handle_client(int fd) {
    client = fd;
    atomic_store(&last_acked, 0);
    atomic_store(&last_sent, 0);
    uint8_t h[5], buf[4096];
    pthread_t streamer;
    bool streaming = false;
    while (net_read_full(fd, h, 5)) {
        uint32_t len = get_u32(h + 1);
        if (len > sizeof buf - 1 || (len && !net_read_full(fd, buf, len))) break;
        buf[len] = 0;
        switch (h[0]) {
        case MSG_HELLO: {
            char got[64] = "";
            json_string((char *)buf, len, "pin", got, sizeof got);
            fprintf(stderr, "fake-host: hello %s\n", buf);
            if (strcmp(got, pin) != 0) {
                send_msg(MSG_AUTH_FAILED, NULL, 0);
                goto done;
            }
            double v;
            if (json_number((char *)buf, len, "width", &v)) atomic_store(&req_w, (int)v);
            if (json_number((char *)buf, len, "height", &v)) atomic_store(&req_h, (int)v);
            atomic_store(&alive, true);
            pthread_create(&streamer, NULL, stream_thread, NULL);
            streaming = true;
            break;
        }
        case MSG_ACK: atomic_store(&last_acked, get_u32(buf)); break;
        case MSG_PING: send_msg(MSG_PONG, buf, len); break;
        case MSG_REQUEST_KEYFRAME: fprintf(stderr, "fake-host: keyframe requested\n"); atomic_store(&want_keyframe, true); break;
        case MSG_MOUSE_MOVE:
            mouse_x = (int)(get_f32(buf) * atomic_load(&req_w));
            mouse_y = (int)(get_f32(buf + 4) * atomic_load(&req_h));
            break;
        case MSG_MOUSE_BUTTON:
            fprintf(stderr, "fake-host: button %d %s at %.3f,%.3f\n", buf[0], buf[1] ? "down" : "up", get_f32(buf + 2), get_f32(buf + 6));
            break;
        case MSG_SCROLL: fprintf(stderr, "fake-host: scroll %.1f,%.1f\n", get_f32(buf), get_f32(buf + 4)); break;
        case MSG_KEY:
            fprintf(stderr, "fake-host: key 0x%02x %s mods 0x%x\n", buf[0] << 8 | buf[1],
                    buf[2] == 0 ? "up" : buf[2] == 1 ? "down" : "repeat", get_u32(buf + 3));
            break;
        case MSG_DISPLAY: {
            double v;
            fprintf(stderr, "fake-host: display %s\n", buf);
            if (json_number((char *)buf, len, "width", &v)) atomic_store(&req_w, (int)v);
            if (json_number((char *)buf, len, "height", &v)) atomic_store(&req_h, (int)v);
            atomic_store(&restart, true);
            break;
        }
        default: fprintf(stderr, "fake-host: message 0x%02x (%u bytes)\n", h[0], len);
        }
    }
done:
    atomic_store(&alive, false);
    if (streaming) pthread_join(streamer, NULL);
    close(fd);
    fprintf(stderr, "fake-host: client gone\n");
}

int main(int argc, char **argv) {
    static const struct option o[] = { {"port", 1, 0, 'p'}, {"pin", 1, 0, 'P'}, {"codec", 1, 0, 'c'}, {"fps", 1, 0, 'f'}, {0} };
    for (int c; (c = getopt_long(argc, argv, "", o, NULL)) != -1;) {
        if (c == 'p') port = atoi(optarg);
        else if (c == 'P') pin = optarg;
        else if (c == 'c') want_codec = strcmp(optarg, "h264") ? CODEC_HEVC : CODEC_H264;
        else if (c == 'f') fps = atoi(optarg);
    }
    int ls = socket(AF_INET6, SOCK_STREAM, 0), one = 1, zero = 0;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(ls, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);
    struct sockaddr_in6 addr = { .sin6_family = AF_INET6, .sin6_port = htons(port), .sin6_addr = in6addr_any };
    if (bind(ls, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(ls, 4) < 0) { perror("fake-host"); return 1; }
    fprintf(stderr, "fake-host: listening on %d, PIN %s\n", port, pin);
    for (;;) {
        int fd = accept(ls, NULL, NULL);
        if (fd < 0) continue;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        handle_client(fd);
    }
}
