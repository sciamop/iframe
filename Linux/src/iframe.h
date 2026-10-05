// iFrame Linux client: shared declarations.
//
// Wire protocol (see Shared/Protocol.swift): every message is
// type (u8) | payload length (u32, big endian) | payload.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IFRAME_DEFAULT_PORT 7878
#define IFRAME_PROTOCOL_VERSION 1
#define IFRAME_MAX_MESSAGE (32u << 20)

enum {
    // host -> client
    MSG_WELCOME = 0x01,
    MSG_FORMAT = 0x02,
    MSG_FRAME = 0x03,
    MSG_STATS = 0x04,
    MSG_PONG = 0x05,
    MSG_AUTH_FAILED = 0x06,
    MSG_TEXT_FOCUS = 0x07,
    // client -> host
    MSG_HELLO = 0x10,
    MSG_MOUSE_MOVE = 0x11,
    MSG_MOUSE_BUTTON = 0x12,
    MSG_SCROLL = 0x13,
    MSG_KEY = 0x14,
    MSG_TEXT = 0x15,
    MSG_REQUEST_KEYFRAME = 0x16,
    MSG_PING = 0x17,
    MSG_ACK = 0x18,
    MSG_DISPLAY = 0x19,
};

enum { CODEC_H264 = 0, CODEC_HEVC = 1 };

enum { KEY_UP = 0, KEY_DOWN = 1, KEY_REPEAT = 2 };

enum {
    MOD_SHIFT = 1 << 0,
    MOD_CONTROL = 1 << 1,
    MOD_OPTION = 1 << 2,
    MOD_COMMAND = 1 << 3,
    MOD_CAPS = 1 << 4,
};

typedef struct {
    int width, height;
    double point_width, point_height;
    int codec, fps;
    char host_name[128];
    bool is_virtual;
} Welcome;

typedef struct {
    double fps, mbps, target_mbps, encode_ms, latency_ms;
    int dropped;
} HostStats;

// Big-endian helpers.
static inline void put_u16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static inline void put_u32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static inline void put_u64(uint8_t *p, uint64_t v) { put_u32(p, v >> 32); put_u32(p + 4, (uint32_t)v); }
static inline void put_f32(uint8_t *p, float f) { union { float f; uint32_t u; } c = { .f = f }; put_u32(p, c.u); }
static inline uint32_t get_u32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static inline uint64_t get_u64(const uint8_t *p) { return (uint64_t)get_u32(p) << 32 | get_u32(p + 4); }
static inline float get_f32(const uint8_t *p) { union { float f; uint32_t u; } c = { .u = get_u32(p) }; return c.f; }

uint64_t now_nanos(void);

// json.c — just enough JSON for Welcome / HostStats, and string escaping for Hello.
bool json_number(const char *json, size_t len, const char *key, double *out);
bool json_string(const char *json, size_t len, const char *key, char *out, size_t cap);
bool json_bool(const char *json, size_t len, const char *key, bool *out);
void json_escape(const char *in, char *out, size_t cap);

// discover.c — Bonjour browsing via avahi-browse.
typedef struct {
    char name[128];
    char address[64];
    int port;
} HostEntry;
int discover_hosts(HostEntry *out, int max, int timeout_seconds);

// keymap.c — Linux keys (SDL scancodes, which are USB HID usages) to macOS kVK_ codes.
int keymap_mac_code(int hid_usage);   // -1 if unmapped
uint32_t keymap_modifier(int mac_code); // MOD_* bit for modifier keys, else 0

// video.c — FFmpeg decoder (NVDEC / VAAPI / Vulkan when available, else software).
typedef struct Decoder Decoder;
struct AVFrame;
Decoder *decoder_new(bool allow_hw);
void decoder_free(Decoder *d);
// Parameter sets as they arrive in a FORMAT message (raw NAL units, no prefixes).
bool decoder_set_format(Decoder *d, int codec, const uint8_t *const *sets, const uint32_t *sizes, int count);
// Decodes one access unit of 4-byte length-prefixed NAL units. On success *out holds a
// frame in system memory (NV12 or YUV420P), owned by the decoder until the next call.
bool decoder_decode(Decoder *d, const uint8_t *data, size_t len, struct AVFrame **out);
const char *decoder_description(Decoder *d);
