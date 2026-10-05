// iframe-linux-host: streams an X11 display to iFrame clients (iPad app, Linux client).
//
// This is a separate host from the Mac's iframe-host (Host/, Swift). The two share only the
// wire protocol; the C helpers for it (framing, JSON, key table) come from the Linux client.

#pragma once

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include "../../Linux/src/iframe.h"

void host_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// capture.c — one X11 monitor via XShm, with change detection (XDamage) and the cursor
// composited in (XFixes), since X never puts it in the framebuffer.
typedef struct Capture Capture;
Capture *capture_open(const char *display, int monitor, char *err, size_t err_cap);
void capture_close(Capture *c);
int capture_width(Capture *c);
int capture_height(Capture *c);
int capture_refresh_hz(Capture *c);
int capture_fd(Capture *c);                 // X connection, for poll()
bool capture_poll_changes(Capture *c);       // drains X events + checks the pointer; true if the screen changed
bool capture_grab(Capture *c);               // grabs the screen into the BGRX buffer
const uint8_t *capture_pixels(Capture *c, int *stride);
void capture_monitor_origin(Capture *c, int *x, int *y);

// encoder.c — NVENC through libavcodec, fed BGRX directly (NVENC converts on the GPU).
typedef struct Encoder Encoder;
typedef void (*FormatFn)(void *ctx, int codec, const uint8_t *const *sets, const uint32_t *sizes, int count);
typedef void (*FrameFn)(void *ctx, bool keyframe, const uint8_t *avcc, size_t len);
Encoder *encoder_open(int codec, int width, int height, int fps, int bitrate, char *err, size_t err_cap);
void encoder_close(Encoder *e);
int encoder_codec(Encoder *e);
const char *encoder_name(Encoder *e);
void encoder_set_bitrate(Encoder *e, int bitrate);
// Encodes one BGRX frame; output is split like VideoToolbox's: parameter sets (on keyframes)
// and an access unit of 4-byte length-prefixed NAL units.
bool encoder_encode(Encoder *e, const uint8_t *bgrx, int stride, bool keyframe, FormatFn on_format,
                    FrameFn on_frame, void *ctx);

// input.c — XTest injection.
enum CmdMode { CMD_AUTO, CMD_CTRL, CMD_SUPER };
typedef struct Input Input;
Input *input_open(const char *display);
void input_close(Input *in);
// Monitor rectangle in root coordinates that normalized positions map onto.
void input_set_region(Input *in, int x, int y, int w, int h);
// What ⌘ becomes. Mac users (iPad) expect ⌘C to copy, so CTRL; a Linux client sends its
// own Super key as ⌘, so SUPER.
void input_set_cmd(Input *in, enum CmdMode mode);
void input_move(Input *in, float x, float y);
void input_button(Input *in, int button, bool down, float x, float y);
void input_scroll(Input *in, float dx, float dy);
void input_key(Input *in, int mac_code, int action, uint32_t mods);
void input_text(Input *in, const char *utf8, size_t len);
void input_release_all(Input *in);
void input_keep_awake(Input *in);

// streamer.c — capture → encode → send, with ack-based flow control, idle refinement and
// adaptive bitrate (same design as Host/Streamer.swift).
typedef struct Streamer Streamer;
typedef struct {
    int fps;
    int bitrate, min_bitrate, max_bitrate;
    int max_inflight;
    int codec;
} StreamConfig;
typedef void (*SendFn)(void *ctx, uint8_t type, const uint8_t *payload, uint32_t len);
Streamer *streamer_start(Capture *cap, Encoder *enc, StreamConfig cfg, SendFn send, void *ctx);
void streamer_stop(Streamer *s);   // joins the thread
void streamer_ack(Streamer *s, uint32_t id);
void streamer_request_keyframe(Streamer *s);
