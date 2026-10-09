#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "host.h"

// Same latency design as Host/Streamer.swift:
//  - Only frames that changed are captured (XDamage + pointer motion); a static screen sends nothing.
//  - One frame at a time goes through capture → encode → send, on one thread, so nothing queues
//    up behind a stale screen.
//  - At most `max_inflight` frames may be unacknowledged. When the link backs up, frames are
//    skipped *before* capture/encode and the newest screen goes out as soon as an ack arrives.
//  - When the screen goes idle the last frame is re-encoded twice (150 ms, then 400 ms later)
//    so text sharpens without spending bandwidth while things move.
//  - Bitrate backs off quickly on drops and creeps back up after 3 s without them.

#define SEND_TIMES 256

struct Streamer {
    Capture *cap;
    Encoder *enc;
    StreamConfig cfg;
    SendFn send;
    void *ctx;
    pthread_t thread;
    int wake_fd;                    // eventfd: acks, keyframe requests, stop

    pthread_mutex_t lock;           // guards everything below
    bool stopping;
    bool force_keyframe;
    uint32_t last_sent, last_acked;
    uint64_t send_times[SEND_TIMES];
    int stat_frames, stat_dropped, stat_acks;
    uint64_t stat_bytes, stat_encode_ns, stat_latency_ns;
};

static void wake(Streamer *s) {
    uint64_t one = 1;
    ssize_t rc = write(s->wake_fd, &one, sizeof one);
    (void)rc;
}

void streamer_ack(Streamer *s, uint32_t id) {
    uint64_t now = now_nanos();
    pthread_mutex_lock(&s->lock);
    if ((int32_t)(id - s->last_acked) > 0) s->last_acked = id;
    uint64_t sent = s->send_times[id % SEND_TIMES];
    if (sent) {
        s->stat_latency_ns += now - sent;
        s->stat_acks++;
        s->send_times[id % SEND_TIMES] = 0;
    }
    pthread_mutex_unlock(&s->lock);
    wake(s);
}

void streamer_request_keyframe(Streamer *s) {
    pthread_mutex_lock(&s->lock);
    s->force_keyframe = true;
    pthread_mutex_unlock(&s->lock);
    wake(s);
}

// Encoder callbacks (streamer thread).
typedef struct {
    Streamer *s;
    uint64_t start;
    size_t bytes;
} EncodeContext;

static void on_format(void *p, int codec, const uint8_t *const *sets, const uint32_t *sizes, int count) {
    EncodeContext *ec = p;
    size_t len = 2;
    for (int i = 0; i < count; i++) len += 4 + sizes[i];
    uint8_t *buf = malloc(len);
    buf[0] = (uint8_t)codec;
    buf[1] = (uint8_t)count;
    size_t off = 2;
    for (int i = 0; i < count; i++) {
        put_u32(buf + off, sizes[i]);
        memcpy(buf + off + 4, sets[i], sizes[i]);
        off += 4 + sizes[i];
    }
    ec->s->send(ec->s->ctx, MSG_FORMAT, buf, (uint32_t)len);
    free(buf);
}

static void on_frame(void *p, bool keyframe, const uint8_t *avcc, size_t len) {
    EncodeContext *ec = p;
    Streamer *s = ec->s;
    uint8_t *buf = malloc(13 + len);
    uint64_t now = now_nanos();
    pthread_mutex_lock(&s->lock);
    uint32_t id = ++s->last_sent;
    s->send_times[id % SEND_TIMES] = now;
    s->stat_frames++;
    s->stat_bytes += len;
    s->stat_encode_ns += now - ec->start;
    pthread_mutex_unlock(&s->lock);
    put_u32(buf, id);
    put_u64(buf + 4, ec->start);
    buf[12] = keyframe;
    memcpy(buf + 13, avcc, len);
    s->send(s->ctx, MSG_FRAME, buf, (uint32_t)(13 + len));
    free(buf);
}

static void send_stats(Streamer *s, int *bitrate, uint64_t last_drop) {
    pthread_mutex_lock(&s->lock);
    int frames = s->stat_frames, dropped = s->stat_dropped, acks = s->stat_acks;
    uint64_t bytes = s->stat_bytes, enc_ns = s->stat_encode_ns, lat_ns = s->stat_latency_ns;
    s->stat_frames = s->stat_dropped = s->stat_acks = 0;
    s->stat_bytes = s->stat_encode_ns = s->stat_latency_ns = 0;
    pthread_mutex_unlock(&s->lock);

    int target = *bitrate;
    if (dropped > 2) {
        target = (int)(*bitrate * 0.75);
        if (target < s->cfg.min_bitrate) target = s->cfg.min_bitrate;
    } else if (now_nanos() - last_drop > 3000000000ull && frames > s->cfg.fps / 4) {
        target = (int)(*bitrate * 1.1);
        if (target > s->cfg.max_bitrate) target = s->cfg.max_bitrate;
    }
    if (target != *bitrate) {
        *bitrate = target;
        encoder_set_bitrate(s->enc, target);
    }
    char json[256];
    int n = snprintf(json, sizeof json,
                     "{\"fps\":%d,\"mbps\":%.3f,\"targetMbps\":%.3f,\"encodeMs\":%.3f,\"latencyMs\":%.3f,\"dropped\":%d}",
                     frames, bytes * 8 / 1e6, *bitrate / 1e6, frames ? enc_ns / (double)frames / 1e6 : 0,
                     acks ? lat_ns / (double)acks / 1e6 : 0, dropped);
    s->send(s->ctx, MSG_STATS, (const uint8_t *)json, (uint32_t)n);
}

/// Payload: hotspot x, y, size w, h (u16, points), then the PNG — what the Mac host sends.
static void send_cursor(Streamer *s) {
    CursorImage shape;
    if (!capture_cursor_shape(s->cap, &shape)) return;
    uint8_t *buf = malloc(8 + shape.png_len);
    if (buf) {
        put_u16(buf, shape.hot_x);
        put_u16(buf + 2, shape.hot_y);
        put_u16(buf + 4, shape.width);
        put_u16(buf + 6, shape.height);
        memcpy(buf + 8, shape.png, shape.png_len);
        s->send(s->ctx, MSG_CURSOR, buf, (uint32_t)(8 + shape.png_len));
        free(buf);
    }
    free(shape.png);
}

static void *run(void *arg) {
    Streamer *s = arg;
    const uint64_t interval = 1000000000ull / s->cfg.fps;
    uint64_t next_frame = 0, next_stats = now_nanos() + 1000000000ull, last_drop = 0;
    uint64_t refine_at = 0;
    int refine_passes = 2;           // nothing to refine until the first capture
    bool dirty = true, have_frame = false;
    int bitrate = s->cfg.bitrate;

    for (;;) {
        uint64_t now = now_nanos();
        if (capture_poll_changes(s->cap)) dirty = true;
        if (s->cfg.local_cursor) send_cursor(s);

        pthread_mutex_lock(&s->lock);
        bool stopping = s->stopping;
        bool keyframe = s->force_keyframe;
        bool window_full = (int32_t)(s->last_sent - s->last_acked) >= s->cfg.max_inflight;
        pthread_mutex_unlock(&s->lock);
        if (stopping) break;

        if (now >= next_stats) {
            next_stats += 1000000000ull;
            send_stats(s, &bitrate, last_drop);
        }

        bool refine = have_frame && refine_passes < 2 && now >= refine_at;
        bool want = dirty || refine || (keyframe && have_frame);
        uint64_t wait_until = now + interval;   // the pointer is polled at the frame rate
        if (refine_passes < 2 && refine_at < wait_until) wait_until = refine_at;

        if (want && now >= next_frame) {
            if (window_full) {
                // Skip before encoding; the next ack wakes us and the newest screen goes out.
                // Counted once per frame slot, like the Mac counting skipped capture frames.
                if (now - last_drop >= interval) {
                    pthread_mutex_lock(&s->lock);
                    s->stat_dropped++;
                    pthread_mutex_unlock(&s->lock);
                    last_drop = now;
                }
                wait_until = now + interval;
            } else {
                if (dirty || !have_frame) {
                    if (capture_grab(s->cap)) {
                        have_frame = true;
                        refine_passes = 0;
                        refine_at = now + 150000000ull;
                    }
                    dirty = false;
                } else if (refine) {
                    refine_passes++;
                    refine_at = now + 400000000ull;
                }
                if (have_frame) {
                    pthread_mutex_lock(&s->lock);
                    keyframe = s->force_keyframe;
                    s->force_keyframe = false;
                    pthread_mutex_unlock(&s->lock);
                    int stride;
                    const uint8_t *pixels = capture_pixels(s->cap, &stride);
                    EncodeContext ec = { .s = s, .start = now_nanos() };
                    if (!encoder_encode(s->enc, pixels, stride, keyframe, on_format, on_frame, &ec) && keyframe) {
                        pthread_mutex_lock(&s->lock);
                        s->force_keyframe = true;
                        pthread_mutex_unlock(&s->lock);
                    }
                }
                next_frame = now + interval;
                continue;
            }
        } else if (want) {
            wait_until = next_frame;
        }

        now = now_nanos();
        int timeout_ms = wait_until > now ? (int)((wait_until - now + 999999) / 1000000) : 0;
        struct pollfd fds[2] = { { .fd = capture_fd(s->cap), .events = POLLIN }, { .fd = s->wake_fd, .events = POLLIN } };
        if (poll(fds, 2, timeout_ms) > 0 && (fds[1].revents & POLLIN)) {
            uint64_t v;
            ssize_t rc = read(s->wake_fd, &v, sizeof v);
            (void)rc;
        }
    }
    return NULL;
}

Streamer *streamer_start(Capture *cap, Encoder *enc, StreamConfig cfg, SendFn send, void *ctx) {
    Streamer *s = calloc(1, sizeof *s);
    s->cap = cap;
    s->enc = enc;
    s->cfg = cfg;
    s->send = send;
    s->ctx = ctx;
    s->force_keyframe = true;
    s->wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    capture_set_local_cursor(cap, cfg.local_cursor);
    pthread_mutex_init(&s->lock, NULL);
    pthread_create(&s->thread, NULL, run, s);
    return s;
}

void streamer_stop(Streamer *s) {
    if (!s) return;
    pthread_mutex_lock(&s->lock);
    s->stopping = true;
    pthread_mutex_unlock(&s->lock);
    wake(s);
    pthread_join(s->thread, NULL);
    close(s->wake_fd);
    pthread_mutex_destroy(&s->lock);
    free(s);
}
