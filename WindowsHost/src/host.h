// iframe-windows-host: streams a Windows desktop to iFrame clients (iPad app, Mac, Windows and
// Linux clients).
//
// This is a separate host from the Mac's iframe-host (Host/, Swift) and iframe-linux-host
// (LinuxHost/, C). They share only the wire protocol; the C helpers for it (JSON, key table)
// come from the Linux client.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// WinUser.h's RegisterHotKey flags share names with the protocol's modifier bits.
#undef MOD_SHIFT
#undef MOD_CONTROL

extern "C" {
#include "../../Linux/src/iframe.h"
}

void host_log(_Printf_format_string_ const char *fmt, ...);

// net.cpp — blocking socket helpers.
bool net_read_full(SOCKET fd, void *buf, size_t len);
bool net_write_full(SOCKET fd, const void *buf, size_t len);

struct ID3D11Device;
struct ID3D11Texture2D;

// capture.cpp — one monitor via DXGI Desktop Duplication, on the GPU that drives it. Frames stay
// in GPU memory; Windows reports when the screen changes, so a static screen costs nothing.
// The pointer is never in the frames: Windows hands its shape over separately, which goes out
// as a PNG for the client to draw.
struct Capture;
// Prints every monitor and the GPU that drives it.
void capture_list_monitors(void);
// monitor: -1 = the primary monitor, else an index from capture_list_monitors().
// blocked (optional) is set when the desktop can't be captured for now (lock screen, UAC).
Capture *capture_open(int monitor, char *err, size_t err_cap, bool *blocked = NULL);
void capture_close(Capture *c);
// Switches the monitor to its mode closest in shape, then size, to a width x height client screen.
// True when it changed the mode; capture_restore_mode puts back the saved one.
bool capture_match_mode(int monitor, int width, int height);
void capture_restore_mode(void);
int capture_width(Capture *c);
int capture_height(Capture *c);
int capture_refresh_hz(Capture *c);
const char *capture_adapter_name(Capture *c);
// The monitor's rectangle in virtual-desktop pixels, which input positions map onto.
RECT capture_monitor_rect(Capture *c);
ID3D11Device *capture_device(Capture *c);
LUID capture_adapter_luid(Capture *c);
// Waits up to timeout_ms for Windows to report a change. True when the screen image changed;
// the newest image is then in capture_frame(). Pointer updates are collected along the way.
bool capture_wait(Capture *c, int timeout_ms);
// BGRA, the last image captured (NULL before the first). Stays valid until the next capture_wait.
ID3D11Texture2D *capture_frame(Capture *c);
typedef struct {
    uint8_t *png;                    // caller frees
    size_t png_len;
    int width, height, hot_x, hot_y; // in points, which on this host are pixels
} CursorImage;
// True (and fills `shape`) when the cursor's shape changed since the last call.
bool capture_cursor_shape(Capture *c, CursorImage *shape);

// encoder.cpp — the GPU's hardware encoder through Media Foundation. A video processor converts
// the BGRA frame to NV12 (BT.709) on the GPU first, so frames never leave video memory.
struct Encoder;
typedef void (*FormatFn)(void *ctx, int codec, const uint8_t *const *sets, const uint32_t *sizes, int count);
typedef void (*FrameFn)(void *ctx, bool keyframe, const uint8_t *avcc, size_t len);
Encoder *encoder_open(ID3D11Device *device, LUID adapter, int codec, int width, int height, int fps, int bitrate,
                      char *err, size_t err_cap);
void encoder_close(Encoder *e);
int encoder_codec(Encoder *e);
const char *encoder_name(Encoder *e);
void encoder_set_bitrate(Encoder *e, int bitrate);
// Encodes one frame; output is split like VideoToolbox's: parameter sets (on keyframes) and an
// access unit of 4-byte length-prefixed NAL units.
bool encoder_encode(Encoder *e, ID3D11Texture2D *bgra, bool keyframe, FormatFn on_format, FrameFn on_frame,
                    void *ctx);

// input.cpp — SendInput injection. Keys go in by scan code, so they land by position whatever the
// keyboard layout. Windows won't let it type into elevated windows unless the host runs elevated.
enum CmdMode { CMD_AUTO, CMD_CTRL, CMD_WIN };
struct Input;
Input *input_open(void);
void input_close(Input *in);
// Monitor rectangle in virtual-desktop pixels that normalized positions map onto.
void input_set_region(Input *in, RECT region);
// What ⌘ becomes. Mac users (iPad, Mac) expect ⌘C to copy, so CTRL; Windows and Linux clients
// send their own Windows/Super key as ⌘, so WIN.
void input_set_cmd(Input *in, enum CmdMode mode);
void input_move(Input *in, float x, float y);
void input_button(Input *in, int button, bool down, float x, float y);
void input_scroll(Input *in, float dx, float dy);
void input_key(Input *in, int mac_code, int action, uint32_t mods);
void input_text(Input *in, const char *utf8, size_t len);
void input_release_all(Input *in);
void input_keep_awake(Input *in);

// service.cpp — Windows only lets SYSTEM capture and control the secure desktop (the lock and
// sign-in screens, UAC prompts). Installed as a service, the host runs as SYSTEM in the console
// session and its capture and input threads follow whichever desktop has the input.
int service_run(const wchar_t *child_args);
// Moves the calling thread to the desktop that currently receives input. A host that isn't SYSTEM
// can't open the secure desktop, and stays where it is.
void desktop_follow_input(void);

// streamer.cpp — capture → encode → send, with ack-based flow control, idle refinement and
// adaptive bitrate (same design as Host/Streamer.swift and LinuxHost/src/streamer.c).
struct Streamer;
typedef struct {
    int fps;
    int bitrate, min_bitrate, max_bitrate;
    int max_inflight;
    int codec;
    bool local_cursor;               // send MSG_CURSOR shapes; the client draws the pointer
} StreamConfig;
typedef void (*SendFn)(void *ctx, uint8_t type, const uint8_t *payload, uint32_t len);
Streamer *streamer_start(Capture *cap, Encoder *enc, StreamConfig cfg, SendFn send, void *ctx);
void streamer_stop(Streamer *s);   // joins the thread
void streamer_ack(Streamer *s, uint32_t id);
void streamer_request_keyframe(Streamer *s);
