#include "host.h"  // first: winsock2.h has to come before windows.h

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <objbase.h>

// Same latency design as Host/Streamer.swift and LinuxHost/src/streamer.c:
//  - Only frames that changed are captured (Desktop Duplication reports presents); a static
//    screen sends nothing.
//  - One frame at a time goes through capture → encode → send, on one thread, so nothing queues
//    up behind a stale screen.
//  - At most `max_inflight` frames may be unacknowledged. When the link backs up, frames are
//    skipped *before* encoding and the newest screen goes out as soon as an ack arrives.
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
    HANDLE thread;
    HANDLE wake;                    // auto-reset: acks, keyframe requests, stop

    SRWLOCK lock;                   // guards everything below
    bool stopping;
    bool force_keyframe;
    uint32_t last_sent, last_acked;
    uint64_t send_times[SEND_TIMES];
    int stat_frames, stat_dropped, stat_acks;
    uint64_t stat_bytes, stat_encode_ns, stat_latency_ns;
};

void streamer_ack(Streamer *s, uint32_t id) {
    uint64_t now = now_nanos();
    AcquireSRWLockExclusive(&s->lock);
    if ((int32_t)(id - s->last_acked) > 0) s->last_acked = id;
    uint64_t sent = s->send_times[id % SEND_TIMES];
    if (sent) {
        s->stat_latency_ns += now - sent;
        s->stat_acks++;
        s->send_times[id % SEND_TIMES] = 0;
    }
    ReleaseSRWLockExclusive(&s->lock);
    SetEvent(s->wake);
}

void streamer_request_keyframe(Streamer *s) {
    AcquireSRWLockExclusive(&s->lock);
    s->force_keyframe = true;
    ReleaseSRWLockExclusive(&s->lock);
    SetEvent(s->wake);
}

// Encoder callbacks (streamer thread).
typedef struct {
    Streamer *s;
    uint64_t start;
} EncodeContext;

static void on_format(void *p, int codec, const uint8_t *const *sets, const uint32_t *sizes, int count) {
    EncodeContext *ec = (EncodeContext *)p;
    size_t len = 2;
    for (int i = 0; i < count; i++) len += 4 + sizes[i];
    uint8_t *buf = (uint8_t *)malloc(len);
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
    EncodeContext *ec = (EncodeContext *)p;
    Streamer *s = ec->s;
    uint8_t *buf = (uint8_t *)malloc(13 + len);
    uint64_t now = now_nanos();
    AcquireSRWLockExclusive(&s->lock);
    uint32_t id = ++s->last_sent;
    s->send_times[id % SEND_TIMES] = now;
    s->stat_frames++;
    s->stat_bytes += len;
    s->stat_encode_ns += now - ec->start;
    ReleaseSRWLockExclusive(&s->lock);
    put_u32(buf, id);
    put_u64(buf + 4, ec->start);
    buf[12] = keyframe;
    memcpy(buf + 13, avcc, len);
    s->send(s->ctx, MSG_FRAME, buf, (uint32_t)(13 + len));
    free(buf);
}

static void send_stats(Streamer *s, int *bitrate, uint64_t last_drop) {
    AcquireSRWLockExclusive(&s->lock);
    int frames = s->stat_frames, dropped = s->stat_dropped, acks = s->stat_acks;
    uint64_t bytes = s->stat_bytes, enc_ns = s->stat_encode_ns, lat_ns = s->stat_latency_ns;
    s->stat_frames = s->stat_dropped = s->stat_acks = 0;
    s->stat_bytes = s->stat_encode_ns = s->stat_latency_ns = 0;
    ReleaseSRWLockExclusive(&s->lock);

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
    uint8_t *buf = (uint8_t *)malloc(8 + shape.png_len);
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

static DWORD WINAPI run(void *arg) {
    Streamer *s = (Streamer *)arg;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);  // WIC, for cursor PNGs
    const uint64_t interval = 1000000000ull / s->cfg.fps;
    uint64_t next_frame = 0, next_stats = now_nanos() + 1000000000ull, last_drop = 0;
    uint64_t refine_at = 0;
    int refine_passes = 2;           // nothing to refine until the first capture
    bool dirty = false, have_frame = false;
    int bitrate = s->cfg.bitrate;

    // The first image: Desktop Duplication delivers the whole screen on its first frame.
    for (int i = 0; i < 20 && !capture_frame(s->cap); i++) capture_wait(s->cap, 50);
    have_frame = capture_frame(s->cap) != NULL;
    dirty = have_frame;

    for (;;) {
        uint64_t now = now_nanos();
        if (s->cfg.local_cursor) send_cursor(s);

        AcquireSRWLockExclusive(&s->lock);
        bool stopping = s->stopping;
        bool keyframe = s->force_keyframe;
        bool window_full = (int32_t)(s->last_sent - s->last_acked) >= s->cfg.max_inflight;
        ReleaseSRWLockExclusive(&s->lock);
        if (stopping) break;

        if (now >= next_stats) {
            next_stats += 1000000000ull;
            send_stats(s, &bitrate, last_drop);
        }

        bool refine = have_frame && refine_passes < 2 && now >= refine_at;
        bool want = dirty || refine || (keyframe && have_frame);
        uint64_t wait_until = next_stats;
        if (refine_passes < 2 && refine_at < wait_until) wait_until = refine_at;

        if (want && now >= next_frame) {
            if (window_full) {
                // Skip before encoding; the next ack wakes us and the newest screen goes out.
                // Counted once per frame slot, like the Mac counting skipped capture frames.
                if (now - last_drop >= interval) {
                    AcquireSRWLockExclusive(&s->lock);
                    s->stat_dropped++;
                    ReleaseSRWLockExclusive(&s->lock);
                    last_drop = now;
                }
                WaitForSingleObject(s->wake, (DWORD)(interval / 1000000 + 1));
                continue;
            }
            if (dirty) {
                refine_passes = 0;
                refine_at = now + 150000000ull;
                dirty = false;
            } else if (refine) {
                refine_passes++;
                refine_at = now + 400000000ull;
            }
            AcquireSRWLockExclusive(&s->lock);
            keyframe = s->force_keyframe;
            s->force_keyframe = false;
            ReleaseSRWLockExclusive(&s->lock);
            EncodeContext ec = { s, now_nanos() };
            if (!encoder_encode(s->enc, capture_frame(s->cap), keyframe, on_format, on_frame, &ec) && keyframe) {
                AcquireSRWLockExclusive(&s->lock);
                s->force_keyframe = true;
                ReleaseSRWLockExclusive(&s->lock);
            }
            next_frame = now + interval;
            continue;
        }
        if (want) wait_until = next_frame;

        // Wait for the screen to change, an ack/keyframe request/stop, or the next deadline.
        // Desktop Duplication can't wait on an event, so its waits are kept short.
        now = now_nanos();
        int timeout_ms = wait_until > now ? (int)((wait_until - now + 999999) / 1000000) : 0;
        if (timeout_ms > 8) timeout_ms = 8;
        if (WaitForSingleObject(s->wake, 0) == WAIT_OBJECT_0) continue;
        if (capture_wait(s->cap, timeout_ms)) {
            dirty = true;
            have_frame = true;
        }
    }
    CoUninitialize();
    return 0;
}

Streamer *streamer_start(Capture *cap, Encoder *enc, StreamConfig cfg, SendFn send, void *ctx) {
    Streamer *s = (Streamer *)calloc(1, sizeof *s);
    s->cap = cap;
    s->enc = enc;
    s->cfg = cfg;
    s->send = send;
    s->ctx = ctx;
    s->force_keyframe = true;
    s->wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    InitializeSRWLock(&s->lock);
    s->thread = CreateThread(NULL, 0, run, s, 0, NULL);
    SetThreadPriority(s->thread, THREAD_PRIORITY_ABOVE_NORMAL);
    return s;
}

void streamer_stop(Streamer *s) {
    if (!s) return;
    AcquireSRWLockExclusive(&s->lock);
    s->stopping = true;
    ReleaseSRWLockExclusive(&s->lock);
    SetEvent(s->wake);
    WaitForSingleObject(s->thread, INFINITE);
    CloseHandle(s->thread);
    CloseHandle(s->wake);
    free(s);
}
