// iFrame Linux client: connects to iframe-host, decodes the stream with FFmpeg (NVDEC /
// VAAPI / Vulkan when available) and shows it in an SDL window, forwarding mouse and keyboard.
//
// Threads:
//   main     SDL events, input, rendering
//   network  connect / reconnect, receive, decode, ack (like the iPad: decode is synchronous,
//            so the ack goes out the moment a frame is ready)
//   timer    once a second: ping + stats

#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <SDL2/SDL.h>
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>

#include "iframe.h"
#include "net.h"

enum CmdKey { CMD_ALT, CMD_SUPER, CMD_CTRL };

static struct {
    char host[256];
    int port;
    char pin[64];
    double scale;          // pixels per Mac point (DisplayRequest.uiScale); 0 = the Mac's own display
    bool fullscreen;
    int window_w, window_h;
    bool hw;
    bool h264_only;
    enum CmdKey cmd_key;
    bool cmd_key_given;
    double scroll_speed;
    bool invert_scroll;
    bool local_cursor;
    bool vsync;
    bool print_stats;
    bool view_only;
} opt = {
    .port = IFRAME_DEFAULT_PORT,
    .scale = 1,
    .fullscreen = true,
    .window_w = 1600,
    .window_h = 900,
    .hw = true,
    .cmd_key = CMD_ALT,
    .scroll_speed = 1,
};

static Uint32 EV_FRAME, EV_WELCOME, EV_STATUS, EV_STATS, EV_FATAL;

static atomic_bool quitting;

// After the host ends a session cleanly (another device took over, or it stopped), wait for the
// user instead of reconnecting: reconnecting at once would take the session straight back.
static atomic_bool waiting_for_user;
static SDL_sem *reconnect_sem;

// Socket, shared by every thread that sends.
static SDL_mutex *send_lock;
static int sock = -1;

// Latest decoded frame, handed from the network thread to the main thread.
static SDL_mutex *frame_lock;
static AVFrame *pending_frame;
static bool frame_pending;

// Session state.
static SDL_mutex *state_lock;
static Welcome welcome;
static bool have_welcome;
static HostStats host_stats;
static int frames_decoded;
static uint64_t decode_nanos;
static double rtt_ms;
static char status_text[256];
static char decoder_name[96];
static atomic_int request_w, request_h;   // display size to ask for (drawable pixels)
static int max_fps = 60;

// MARK: - Sending

static bool send_msg(uint8_t type, const void *payload, uint32_t len) {
    if (opt.view_only && type >= MSG_MOUSE_MOVE && type <= MSG_TEXT) return true;
    uint8_t stackbuf[256];
    uint8_t *buf = len + 5 <= sizeof stackbuf ? stackbuf : malloc(len + 5);
    if (!buf) return false;
    buf[0] = type;
    put_u32(buf + 1, len);
    if (len) memcpy(buf + 5, payload, len);
    SDL_LockMutex(send_lock);
    bool ok = sock >= 0 && net_write_full(sock, buf, len + 5);
    SDL_UnlockMutex(send_lock);
    if (buf != stackbuf) free(buf);
    return ok;
}

static void send_mouse_move(float x, float y) {
    uint8_t p[8];
    put_f32(p, x);
    put_f32(p + 4, y);
    send_msg(MSG_MOUSE_MOVE, p, sizeof p);
}

static void send_mouse_button(uint8_t button, bool down, float x, float y) {
    uint8_t p[10] = { button, down };
    put_f32(p + 2, x);
    put_f32(p + 6, y);
    send_msg(MSG_MOUSE_BUTTON, p, sizeof p);
}

static void send_scroll(float dx, float dy) {
    uint8_t p[8];
    put_f32(p, dx);
    put_f32(p + 4, dy);
    send_msg(MSG_SCROLL, p, sizeof p);
}

static void send_key(uint16_t code, uint8_t action, uint32_t mods) {
    uint8_t p[7];
    put_u16(p, code);
    p[2] = action;
    put_u32(p + 3, mods);
    send_msg(MSG_KEY, p, sizeof p);
}

static void send_display_request(int w, int h) {
    if (opt.scale <= 0 || w <= 0 || h <= 0) return;
    char json[128];
    int n = snprintf(json, sizeof json, "{\"width\":%d,\"height\":%d,\"uiScale\":%g}", w, h, opt.scale);
    send_msg(MSG_DISPLAY, json, n);
}

static void send_hello(void) {
    char pin[160], name[160], host[64] = "Linux";
    gethostname(host, sizeof host - 1);
    json_escape(opt.pin, pin, sizeof pin);
    json_escape(host, name, sizeof name);
    char json[512];
    int n = snprintf(json, sizeof json,
                     "{\"version\":%d,\"pin\":\"%s\",\"name\":\"%s\",\"os\":\"linux\",\"supportsHEVC\":%s,\"maxFPS\":%d,"
                     "\"display\":{\"width\":%d,\"height\":%d,\"uiScale\":%g}}",
                     IFRAME_PROTOCOL_VERSION, pin, name, opt.h264_only ? "false" : "true", max_fps,
                     atomic_load(&request_w), atomic_load(&request_h), opt.scale);
    send_msg(MSG_HELLO, json, n);
}

static void push_event(Uint32 type) {
    SDL_Event e = { .type = type };
    SDL_PushEvent(&e);
}

static void set_status(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void set_status(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    SDL_LockMutex(state_lock);
    vsnprintf(status_text, sizeof status_text, fmt, ap);
    fprintf(stderr, "iframe: %s\n", status_text);
    SDL_UnlockMutex(state_lock);
    va_end(ap);
    push_event(EV_STATUS);
}

// MARK: - Network thread

static void handle_format(Decoder *dec, const uint8_t *p, uint32_t len) {
    if (len < 2) return;
    int codec = p[0], count = p[1];
    const uint8_t *sets[16];
    uint32_t sizes[16];
    uint32_t off = 2;
    if (count > 16) return;
    for (int i = 0; i < count; i++) {
        if (off + 4 > len) return;
        sizes[i] = get_u32(p + off);
        off += 4;
        if (sizes[i] > len - off) return;
        sets[i] = p + off;
        off += sizes[i];
    }
    if (!decoder_set_format(dec, codec, sets, sizes, count)) {
        fprintf(stderr, "iframe: could not create a %s decoder\n", codec == CODEC_HEVC ? "HEVC" : "H.264");
        return;
    }
    SDL_LockMutex(state_lock);
    bool changed = strcmp(decoder_name, decoder_description(dec)) != 0;
    snprintf(decoder_name, sizeof decoder_name, "%s", decoder_description(dec));
    SDL_UnlockMutex(state_lock);
    if (changed) fprintf(stderr, "iframe: decoding %s\n", decoder_description(dec));
}

static void handle_frame(Decoder *dec, const uint8_t *p, uint32_t len, uint64_t *last_keyframe_request) {
    if (len < 13) return;
    uint32_t id = get_u32(p);
    AVFrame *out = NULL;
    uint64_t start = now_nanos();
    bool ok = decoder_decode(dec, p + 13, len - 13, &out);
    uint64_t elapsed = now_nanos() - start;

    uint8_t ack[8];
    put_u32(ack, id);
    put_u32(ack + 4, (uint32_t)(elapsed / 1000 > UINT32_MAX ? UINT32_MAX : elapsed / 1000));
    send_msg(MSG_ACK, ack, sizeof ack);

    if (!ok) {
        uint64_t now = now_nanos();
        if (now - *last_keyframe_request > 250000000ull) {
            *last_keyframe_request = now;
            send_msg(MSG_REQUEST_KEYFRAME, NULL, 0);
        }
        return;
    }
    SDL_LockMutex(state_lock);
    frames_decoded++;
    decode_nanos += elapsed;
    SDL_UnlockMutex(state_lock);

    SDL_LockMutex(frame_lock);
    av_frame_unref(pending_frame);
    av_frame_ref(pending_frame, out);
    bool notify = !frame_pending;
    frame_pending = true;
    SDL_UnlockMutex(frame_lock);
    if (notify) push_event(EV_FRAME);
}

static void handle_welcome(const uint8_t *p, uint32_t len) {
    const char *j = (const char *)p;
    Welcome w = {0};
    double v;
    if (json_number(j, len, "width", &v)) w.width = (int)v;
    if (json_number(j, len, "height", &v)) w.height = (int)v;
    if (json_number(j, len, "pointWidth", &v)) w.point_width = v;
    if (json_number(j, len, "pointHeight", &v)) w.point_height = v;
    if (json_number(j, len, "codec", &v)) w.codec = (int)v;
    if (json_number(j, len, "fps", &v)) w.fps = (int)v;
    json_string(j, len, "hostName", w.host_name, sizeof w.host_name);
    json_bool(j, len, "isVirtual", &w.is_virtual);
    char os[32] = "";
    json_string(j, len, "os", os, sizeof os);
    w.is_linux = strcmp(os, "linux") == 0;
    SDL_LockMutex(state_lock);
    welcome = w;
    have_welcome = true;
    SDL_UnlockMutex(state_lock);
    fprintf(stderr, "iframe: streaming %dx%d %s @ %d fps from %s (%s display, %.0fx%.0f pt)\n", w.width, w.height,
            w.codec == CODEC_HEVC ? "HEVC" : "H.264", w.fps, w.host_name, w.is_virtual ? "virtual" : "existing",
            w.point_width, w.point_height);
    push_event(EV_WELCOME);
}

static void handle_stats(const uint8_t *p, uint32_t len) {
    const char *j = (const char *)p;
    HostStats s = {0};
    double v;
    json_number(j, len, "fps", &s.fps);
    json_number(j, len, "mbps", &s.mbps);
    json_number(j, len, "targetMbps", &s.target_mbps);
    json_number(j, len, "encodeMs", &s.encode_ms);
    json_number(j, len, "latencyMs", &s.latency_ms);
    if (json_number(j, len, "dropped", &v)) s.dropped = (int)v;
    SDL_LockMutex(state_lock);
    host_stats = s;
    SDL_UnlockMutex(state_lock);
}

static void sleep_unless_quitting(int ms) {
    for (int t = 0; t < ms && !atomic_load(&quitting); t += 50) SDL_Delay(50);
}

static int network_thread(void *unused) {
    (void)unused;
    uint8_t *body = NULL;
    size_t body_cap = 0;
    bool ever_connected = false;

    while (!atomic_load(&quitting)) {
        set_status("Connecting to %s:%d…", opt.host, opt.port);
        char err[256];
        int fd = net_connect(opt.host, opt.port, 2000, err, sizeof err);
        if (fd < 0) {
            set_status("%s — retrying", err);
            sleep_unless_quitting(1000);
            continue;
        }
        SDL_LockMutex(send_lock);
        sock = fd;
        SDL_UnlockMutex(send_lock);
        if (atomic_load(&quitting)) break;

        send_hello();
        Decoder *dec = decoder_new(opt.hw);
        uint64_t last_keyframe_request = 0;
        bool auth_failed = false, streamed = false, closed_by_host = false;

        for (;;) {
            uint8_t header[5];
            if (!net_read_full(fd, header, 5)) {
                closed_by_host = streamed && errno == 0;
                break;
            }
            uint32_t len = get_u32(header + 1);
            if (len > IFRAME_MAX_MESSAGE) break;
            if (len > body_cap) {
                free(body);
                body_cap = len + len / 2;
                body = malloc(body_cap);
                if (!body) { body_cap = 0; break; }
            }
            if (len && !net_read_full(fd, body, len)) {
                closed_by_host = streamed && errno == 0;
                break;
            }

            switch (header[0]) {
            case MSG_WELCOME:
                ever_connected = true;
                streamed = true;
                handle_welcome(body, len);
                break;
            case MSG_AUTH_FAILED: auth_failed = true; break;
            case MSG_FORMAT: handle_format(dec, body, len); break;
            case MSG_FRAME: handle_frame(dec, body, len, &last_keyframe_request); break;
            case MSG_STATS: handle_stats(body, len); break;
            case MSG_PONG:
                if (len >= 8) {
                    SDL_LockMutex(state_lock);
                    rtt_ms = (now_nanos() - get_u64(body)) / 1e6;
                    SDL_UnlockMutex(state_lock);
                }
                break;
            default: break;  // textFocus: no on-screen keyboard to raise
            }
            if (auth_failed) break;
        }

        SDL_LockMutex(send_lock);
        close(sock);
        sock = -1;
        SDL_UnlockMutex(send_lock);
        decoder_free(dec);
        SDL_LockMutex(state_lock);
        have_welcome = false;
        SDL_UnlockMutex(state_lock);
        SDL_LockMutex(frame_lock);
        av_frame_unref(pending_frame);
        frame_pending = false;
        SDL_UnlockMutex(frame_lock);
        push_event(EV_WELCOME);  // clears the picture

        if (atomic_load(&quitting)) break;
        if (auth_failed) {
            set_status("Wrong PIN. Use the PIN the host printed when it started.");
            push_event(EV_FATAL);
            break;
        }
        if (closed_by_host) {
            set_status("%s ended the session (another device connected, or the host stopped) — click or press a "
                       "key to reconnect", opt.host);
            while (SDL_SemTryWait(reconnect_sem) == 0) {}
            atomic_store(&waiting_for_user, true);
            while (!atomic_load(&quitting) && SDL_SemWaitTimeout(reconnect_sem, 200) != 0) {}
            atomic_store(&waiting_for_user, false);
            continue;
        }
        set_status(ever_connected ? "Disconnected from %s — reconnecting" : "%s closed the connection — retrying",
                   opt.host);
        sleep_unless_quitting(1000);
    }
    free(body);
    return 0;
}

// MARK: - Stats timer

static Uint32 tick(Uint32 interval, void *unused) {
    (void)unused;
    uint8_t p[8];
    put_u64(p, now_nanos());
    send_msg(MSG_PING, p, sizeof p);
    push_event(EV_STATS);
    return interval;
}

// MARK: - Config (~/.config/iframe/linux-client)

static void config_path(char *out, size_t cap) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg && *xdg) snprintf(out, cap, "%s/iframe/linux-client", xdg);
    else snprintf(out, cap, "%s/.config/iframe/linux-client", home ? home : ".");
}

static void load_config(void) {
    char path[PATH_MAX];
    config_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *v = eq + 1;
        if (!strcmp(line, "host")) snprintf(opt.host, sizeof opt.host, "%s", v);
        else if (!strcmp(line, "port")) opt.port = atoi(v);
        else if (!strcmp(line, "pin")) snprintf(opt.pin, sizeof opt.pin, "%s", v);
        else if (!strcmp(line, "scale")) opt.scale = atof(v);
    }
    fclose(f);
}

static void save_config(void) {
    char path[PATH_MAX], dir[PATH_MAX];
    config_path(path, sizeof path);
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = 0;
        char *parent = strrchr(dir, '/');
        if (parent) { *parent = 0; mkdir(dir, 0700); *parent = '/'; }
        mkdir(dir, 0700);
    }
    FILE *f = fopen(path, "w");
    if (!f) return;
    chmod(path, 0600);
    fprintf(f, "host=%s\nport=%d\npin=%s\nscale=%g\n", opt.host, opt.port, opt.pin, opt.scale);
    fclose(f);
}

// MARK: - Input

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static int tex_w, tex_h, tex_format;
static SDL_YUV_CONVERSION_MODE tex_yuv_mode = SDL_YUV_CONVERSION_BT709;
static AVFrame *shown_frame;
static bool have_picture;
static bool keyboard_grab;
static bool welcome_is_linux;

static bool mac_keys_down[128];
static uint32_t buttons_down;
static int swallowed_scancode = -1;

/// The video's rectangle inside a w×h area, letterboxed to keep its aspect ratio.
static SDL_FRect video_rect(int w, int h) {
    int vw = have_picture ? shown_frame->width : w, vh = have_picture ? shown_frame->height : h;
    if (vw <= 0 || vh <= 0) return (SDL_FRect){0, 0, w, h};
    float s = fminf((float)w / vw, (float)h / vh);
    float rw = vw * s, rh = vh * s;
    return (SDL_FRect){(w - rw) / 2, (h - rh) / 2, rw, rh};
}

static void normalized(int x, int y, float *nx, float *ny) {
    int w, h;
    SDL_GetWindowSize(window, &w, &h);
    SDL_FRect r = video_rect(w, h);
    *nx = r.w > 0 ? fminf(fmaxf((x - r.x) / r.w, 0), 1) : 0;
    *ny = r.h > 0 ? fminf(fmaxf((y - r.y) / r.h, 0), 1) : 0;
}

static int remap_usage(int usage) {
    // A Linux host maps ⌘ back to Super, so every key lands where it is on this keyboard.
    if (!opt.cmd_key_given && welcome_is_linux) return usage;
    // HID: 0xE0 lctrl, 0xE2 lalt, 0xE3 lgui; 0xE4 rctrl, 0xE6 ralt, 0xE7 rgui. The table maps
    // gui -> command and alt -> option, so remapping is a swap.
    int left = opt.cmd_key == CMD_ALT ? 0xE2 : opt.cmd_key == CMD_CTRL ? 0xE0 : 0xE3;
    int right = left + 4;
    if (usage == left) return 0xE3;
    if (usage == 0xE3) return left;
    if (usage == right) return 0xE7;
    if (usage == 0xE7) return right;
    return usage;
}

static uint32_t current_mods(void) {
    uint32_t mods = 0;
    for (int c = 0; c < 128; c++)
        if (mac_keys_down[c]) mods |= keymap_modifier(c);
    if (SDL_GetModState() & KMOD_CAPS) mods |= MOD_CAPS;
    return mods;
}

/// Lets go of everything on the Mac, e.g. when the window loses focus mid-shortcut.
static void release_all(void) {
    float x = 0, y = 0;
    int mx, my;
    SDL_GetMouseState(&mx, &my);
    normalized(mx, my, &x, &y);
    for (int b = 0; b < 32; b++)
        if (buttons_down & (1u << b)) send_mouse_button(b, false, x, y);
    buttons_down = 0;
    for (int c = 0; c < 128; c++) {
        if (!mac_keys_down[c]) continue;
        mac_keys_down[c] = false;
        send_key(c, KEY_UP, current_mods());
    }
}

static void set_keyboard_grab(bool on) {
    keyboard_grab = on;
    SDL_SetWindowKeyboardGrab(window, on ? SDL_TRUE : SDL_FALSE);
}

static void toggle_fullscreen(void) {
    opt.fullscreen = !opt.fullscreen;
    SDL_SetWindowFullscreen(window, opt.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    set_keyboard_grab(opt.fullscreen);
}

/// Ctrl+Alt+Shift+<key> is ours, never the Mac's. Returns true if it was a hotkey.
static bool hotkey(const SDL_KeyboardEvent *k) {
    SDL_Keymod m = SDL_GetModState();
    if (!(m & KMOD_CTRL) || !(m & KMOD_ALT) || !(m & KMOD_SHIFT)) return false;
    switch (k->keysym.scancode) {
    case SDL_SCANCODE_Q: push_event(SDL_QUIT); break;
    case SDL_SCANCODE_F: toggle_fullscreen(); break;
    case SDL_SCANCODE_G:
        set_keyboard_grab(!keyboard_grab);
        fprintf(stderr, "iframe: keyboard grab %s\n", keyboard_grab ? "on" : "off");
        break;
    case SDL_SCANCODE_S: opt.print_stats = !opt.print_stats; break;
    case SDL_SCANCODE_K: send_msg(MSG_REQUEST_KEYFRAME, NULL, 0); break;
    default: return false;
    }
    return true;
}

static void handle_key(const SDL_KeyboardEvent *k) {
    bool down = k->state == SDL_PRESSED;
    if (down && !k->repeat && hotkey(k)) {
        swallowed_scancode = k->keysym.scancode;
        return;
    }
    if ((int)k->keysym.scancode == swallowed_scancode) {
        if (!down) swallowed_scancode = -1;
        return;
    }
    int code = keymap_mac_code(remap_usage(k->keysym.scancode));
    if (code < 0 || code >= 128) return;
    if (down && !k->repeat) mac_keys_down[code] = true;
    if (!down) mac_keys_down[code] = false;
    uint8_t action = !down ? KEY_UP : k->repeat ? KEY_REPEAT : KEY_DOWN;
    send_key(code, action, current_mods());
}

static int mac_button(Uint8 sdl_button) {
    switch (sdl_button) {
    case SDL_BUTTON_LEFT: return 0;
    case SDL_BUTTON_RIGHT: return 1;
    case SDL_BUTTON_MIDDLE: return 2;
    case SDL_BUTTON_X1: return 3;
    case SDL_BUTTON_X2: return 4;
    default: return -1;
    }
}

// MARK: - Rendering

static void render(void) {
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    if (have_picture && texture) {
        int w, h;
        SDL_GetRendererOutputSize(renderer, &w, &h);
        SDL_FRect r = video_rect(w, h);
        SDL_RenderCopyF(renderer, texture, NULL, &r);
    }
    SDL_RenderPresent(renderer);
}

static void show_pending_frame(void) {
    SDL_LockMutex(frame_lock);
    bool got = frame_pending;
    if (got) {
        av_frame_unref(shown_frame);
        av_frame_move_ref(shown_frame, pending_frame);
        frame_pending = false;
    }
    SDL_UnlockMutex(frame_lock);
    if (!got) return;

    AVFrame *f = shown_frame;
    int format;
    switch (f->format) {
    case AV_PIX_FMT_NV12: format = SDL_PIXELFORMAT_NV12; break;
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P: format = SDL_PIXELFORMAT_IYUV; break;
    default: {
        static bool warned;
        if (!warned) fprintf(stderr, "iframe: unsupported decoded format %d\n", f->format);
        warned = true;
        return;
    }
    }
    // Follow the stream's matrix: the Mac host sends BT.709; NVENC (iframe-linux-host) converts
    // RGB with BT.601 and says so. SDL applies the mode when a texture is created.
    SDL_YUV_CONVERSION_MODE yuv_mode =
        f->color_range == AVCOL_RANGE_JPEG ? SDL_YUV_CONVERSION_JPEG
        : f->colorspace == AVCOL_SPC_BT470BG || f->colorspace == AVCOL_SPC_SMPTE170M ? SDL_YUV_CONVERSION_BT601
        : SDL_YUV_CONVERSION_BT709;
    if (!texture || tex_w != f->width || tex_h != f->height || tex_format != format || tex_yuv_mode != yuv_mode) {
        SDL_SetYUVConversionMode(yuv_mode);
        tex_yuv_mode = yuv_mode;
        if (texture) SDL_DestroyTexture(texture);
        texture = SDL_CreateTexture(renderer, format, SDL_TEXTUREACCESS_STREAMING, f->width, f->height);
        if (!texture) {
            fprintf(stderr, "iframe: SDL_CreateTexture: %s\n", SDL_GetError());
            return;
        }
        tex_w = f->width;
        tex_h = f->height;
        tex_format = format;
    }
    if (format == SDL_PIXELFORMAT_NV12)
        SDL_UpdateNVTexture(texture, NULL, f->data[0], f->linesize[0], f->data[1], f->linesize[1]);
    else
        SDL_UpdateYUVTexture(texture, NULL, f->data[0], f->linesize[0], f->data[1], f->linesize[1], f->data[2],
                             f->linesize[2]);
    have_picture = true;
    render();
}

static void update_title(bool per_second) {
    char title[512];
    SDL_LockMutex(state_lock);
    int frames = frames_decoded;
    double dec_ms = frames ? decode_nanos / (double)frames / 1e6 : 0;
    if (per_second) {
        frames_decoded = 0;
        decode_nanos = 0;
    }
    if (have_welcome) {
        snprintf(title, sizeof title, "iFrame — %s · %dx%d %s · %d fps · %.1f Mbps · rtt %.1f ms · dec %.1f ms",
                 welcome.host_name, welcome.width, welcome.height, decoder_name, frames, host_stats.mbps, rtt_ms,
                 dec_ms);
        if (per_second && opt.print_stats)
            printf("host %3.0f fps %6.2f Mbps (target %5.1f) enc %4.2f ms ack %5.2f ms drop %d | client %3d fps dec "
                   "%4.2f ms rtt %5.2f ms\n",
                   host_stats.fps, host_stats.mbps, host_stats.target_mbps, host_stats.encode_ms,
                   host_stats.latency_ms, host_stats.dropped, frames, dec_ms, rtt_ms);
    } else {
        snprintf(title, sizeof title, "iFrame — %s", status_text);
    }
    SDL_UnlockMutex(state_lock);
    if (per_second && opt.print_stats) fflush(stdout);
    SDL_SetWindowTitle(window, title);
}

// MARK: - Startup

static void usage(FILE *f) {
    fprintf(f,
            "usage: iframe-client [host[:port]] [options]\n"
            "\n"
            "Connects to iframe-host on a Mac. With no host, finds one on the LAN (Bonjour) or\n"
            "reuses the last one.\n"
            "\n"
            "  -p, --pin PIN          PIN printed by iframe-host (or $IFRAME_PIN; asked for if missing)\n"
            "  -s, --scale S          pixels per Mac point for the virtual display: 1 = most space (default),\n"
            "                         2 = Retina-sharp, 1.33 / 1.6 in between, 0 = stream the Mac's own display\n"
            "  -w, --window WxH       start in a window (default: fullscreen); the Mac display follows its size\n"
            "      --cmd-key KEY      which key is ⌘ on a Mac: alt (default), super, or ctrl\n"
            "                         (on a Linux host every key maps 1:1 unless this is given)\n"
            "      --scroll-speed X   wheel multiplier (default 1)\n"
            "      --invert-scroll    reverse wheel direction\n"
            "      --local-cursor     show the Linux cursor over the stream too\n"
            "      --no-hw            software decoding only\n"
            "      --h264             ask for H.264 instead of HEVC\n"
            "      --vsync            sync presentation to the monitor (smoother, adds latency)\n"
            "      --stats            print per-second stats\n"
            "      --view-only        watch without sending mouse or keyboard\n"
            "  -l, --list             list Macs on the LAN and exit\n"
            "\n"
            "Hotkeys (Ctrl+Alt+Shift + key): F fullscreen, G keyboard grab, S stats, K keyframe, Q quit.\n");
}

static void split_host_port(const char *arg) {
    snprintf(opt.host, sizeof opt.host, "%s", arg);
    char *colon = strrchr(opt.host, ':');
    if (colon && !strchr(colon + 1, ']') && strchr(opt.host, ':') == colon) {
        opt.port = atoi(colon + 1);
        *colon = 0;
    }
    if (opt.host[0] == '[') {  // [v6]:port
        memmove(opt.host, opt.host + 1, strlen(opt.host));
        char *end = strchr(opt.host, ']');
        if (end) {
            if (end[1] == ':') opt.port = atoi(end + 2);
            *end = 0;
        }
    }
}

static void prompt_pin(void) {
    if (!isatty(STDIN_FILENO)) return;
    fprintf(stderr, "PIN for %s: ", opt.host);
    struct termios old, quiet;
    tcgetattr(STDIN_FILENO, &old);
    quiet = old;
    quiet.c_lflag &= ~ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &quiet);
    if (fgets(opt.pin, sizeof opt.pin, stdin)) opt.pin[strcspn(opt.pin, "\r\n")] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &old);
    fprintf(stderr, "\n");
}

int main(int argc, char **argv) {
    load_config();
    char saved_host[256];
    snprintf(saved_host, sizeof saved_host, "%s", opt.host);
    bool list_only = false;

    enum { O_CMD = 1000, O_SCROLL, O_INVERT, O_CURSOR, O_NOHW, O_H264, O_VSYNC, O_STATS, O_VIEW };
    static const struct option longopts[] = {
        {"pin", required_argument, 0, 'p'},     {"scale", required_argument, 0, 's'},
        {"window", required_argument, 0, 'w'},  {"list", no_argument, 0, 'l'},
        {"help", no_argument, 0, 'h'},          {"cmd-key", required_argument, 0, O_CMD},
        {"scroll-speed", required_argument, 0, O_SCROLL}, {"invert-scroll", no_argument, 0, O_INVERT},
        {"local-cursor", no_argument, 0, O_CURSOR}, {"no-hw", no_argument, 0, O_NOHW},
        {"h264", no_argument, 0, O_H264},       {"vsync", no_argument, 0, O_VSYNC},
        {"stats", no_argument, 0, O_STATS},     {"view-only", no_argument, 0, O_VIEW},
        {0, 0, 0, 0},
    };
    bool pin_given = false, scale_given = false;
    for (int c; (c = getopt_long(argc, argv, "p:s:w:lh", longopts, NULL)) != -1;) {
        switch (c) {
        case 'p': snprintf(opt.pin, sizeof opt.pin, "%s", optarg); pin_given = true; break;
        case 's': opt.scale = atof(optarg); scale_given = true; break;
        case 'w':
            if (sscanf(optarg, "%dx%d", &opt.window_w, &opt.window_h) != 2) { usage(stderr); return 64; }
            opt.fullscreen = false;
            break;
        case 'l': list_only = true; break;
        case 'h': usage(stdout); return 0;
        case O_CMD:
            if (!strcmp(optarg, "alt")) opt.cmd_key = CMD_ALT;
            else if (!strcmp(optarg, "super")) opt.cmd_key = CMD_SUPER;
            else if (!strcmp(optarg, "ctrl")) opt.cmd_key = CMD_CTRL;
            else { usage(stderr); return 64; }
            opt.cmd_key_given = true;
            break;
        case O_SCROLL: opt.scroll_speed = atof(optarg); break;
        case O_INVERT: opt.invert_scroll = true; break;
        case O_CURSOR: opt.local_cursor = true; break;
        case O_NOHW: opt.hw = false; break;
        case O_H264: opt.h264_only = true; break;
        case O_VSYNC: opt.vsync = true; break;
        case O_STATS: opt.print_stats = true; break;
        case O_VIEW: opt.view_only = true; break;
        default: usage(stderr); return 64;
        }
    }
    (void)scale_given;
    if (opt.scale > 0) opt.scale = fmin(fmax(opt.scale, 1), 2);  // the host clamps to this range too

    if (list_only || optind >= argc) {
        HostEntry hosts[16];
        int n = discover_hosts(hosts, 16, list_only ? 3 : 2);
        if (list_only) {
            if (n == 0) printf("no Macs running iframe-host found (is avahi-daemon running?)\n");
            for (int i = 0; i < n; i++) printf("%s\t%s:%d\n", hosts[i].name, hosts[i].address, hosts[i].port);
            return n > 0 ? 0 : 1;
        }
        int pick = -1;
        for (int i = 0; i < n && pick < 0; i++)
            if (!strcmp(hosts[i].address, saved_host)) pick = i;
        if (pick < 0 && n > 0) pick = 0;
        if (pick >= 0) {
            snprintf(opt.host, sizeof opt.host, "%s", hosts[pick].address);
            opt.port = hosts[pick].port;
            fprintf(stderr, "iframe: found %s at %s:%d%s\n", hosts[pick].name, opt.host, opt.port,
                    n > 1 ? " (more on the LAN: --list)" : "");
        } else if (!opt.host[0]) {
            fprintf(stderr, "iframe: no Mac found on the LAN; pass its address (iframe-client 192.168.1.20)\n");
            return 1;
        } else {
            fprintf(stderr, "iframe: nothing found on the LAN; trying the last host, %s\n", opt.host);
        }
    } else {
        split_host_port(argv[optind]);
    }
    if (!pin_given && getenv("IFRAME_PIN")) snprintf(opt.pin, sizeof opt.pin, "%s", getenv("IFRAME_PIN"));
    if (strcmp(opt.host, saved_host) != 0 && !pin_given && !getenv("IFRAME_PIN")) opt.pin[0] = 0;
    if (!opt.pin[0]) prompt_pin();
    if (!opt.pin[0]) {
        fprintf(stderr, "iframe: no PIN (use --pin or $IFRAME_PIN)\n");
        return 64;
    }

    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "1");
    SDL_SetHint(SDL_HINT_GRAB_KEYBOARD, "1");
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, opt.vsync ? "1" : "0");
    SDL_SetHint(SDL_HINT_APP_NAME, "iFrame");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "iframe: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetYUVConversionMode(SDL_YUV_CONVERSION_BT709);
    Uint32 base = SDL_RegisterEvents(5);
    EV_FRAME = base;
    EV_WELCOME = base + 1;
    EV_STATUS = base + 2;
    EV_STATS = base + 3;
    EV_FATAL = base + 4;

    Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (opt.fullscreen) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    window = SDL_CreateWindow("iFrame", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, opt.window_w, opt.window_h,
                              flags);
    if (!window) {
        fprintf(stderr, "iframe: SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | (opt.vsync ? SDL_RENDERER_PRESENTVSYNC : 0));
    if (!renderer) renderer = SDL_CreateRenderer(window, -1, 0);
    if (!renderer) {
        fprintf(stderr, "iframe: SDL_CreateRenderer: %s\n", SDL_GetError());
        return 1;
    }
    SDL_DisplayMode mode;
    if (SDL_GetCurrentDisplayMode(SDL_GetWindowDisplayIndex(window), &mode) == 0 && mode.refresh_rate > 0)
        max_fps = mode.refresh_rate;
    if (!opt.local_cursor) SDL_ShowCursor(SDL_DISABLE);
    render();
    int dw, dh;
    SDL_GetRendererOutputSize(renderer, &dw, &dh);
    atomic_store(&request_w, dw);
    atomic_store(&request_h, dh);
    if (opt.fullscreen) set_keyboard_grab(true);

    send_lock = SDL_CreateMutex();
    frame_lock = SDL_CreateMutex();
    state_lock = SDL_CreateMutex();
    reconnect_sem = SDL_CreateSemaphore(0);
    pending_frame = av_frame_alloc();
    shown_frame = av_frame_alloc();

    SDL_Thread *net = SDL_CreateThread(network_thread, "iframe-net", NULL);
    SDL_TimerID timer = SDL_AddTimer(1000, tick, NULL);

    bool saved = false;
    Uint32 resize_at = 0;   // when the window last changed size; asks the Mac to reshape after it settles
    int exit_code = 0;
    for (bool running = true; running;) {
        SDL_Event e;
        if (!SDL_WaitEventTimeout(&e, resize_at ? 100 : 1000)) e.type = 0;
        do {
            if (e.type == SDL_QUIT) {
                running = false;
            } else if (e.type == EV_FRAME) {
                show_pending_frame();
            } else if (e.type == EV_WELCOME) {
                SDL_LockMutex(state_lock);
                bool connected = have_welcome;
                welcome_is_linux = welcome.is_linux;
                SDL_UnlockMutex(state_lock);
                if (connected && !saved) {
                    save_config();
                    saved = true;
                }
                if (!connected) {
                    have_picture = false;
                    av_frame_unref(shown_frame);
                    render();
                }
                update_title(false);
            } else if (e.type == EV_STATUS) {
                update_title(false);
            } else if (e.type == EV_STATS) {
                update_title(true);
            } else if (e.type == EV_FATAL) {
                exit_code = 2;
                running = false;
            } else if (e.type == SDL_WINDOWEVENT) {
                switch (e.window.event) {
                case SDL_WINDOWEVENT_SIZE_CHANGED: resize_at = SDL_GetTicks(); render(); break;
                case SDL_WINDOWEVENT_EXPOSED: render(); break;
                case SDL_WINDOWEVENT_FOCUS_LOST: release_all(); break;
                case SDL_WINDOWEVENT_FOCUS_GAINED: if (keyboard_grab) set_keyboard_grab(true); break;
                }
            } else if ((e.type == SDL_KEYDOWN || e.type == SDL_MOUSEBUTTONDOWN) && atomic_load(&waiting_for_user)) {
                atomic_store(&waiting_for_user, false);
                SDL_SemPost(reconnect_sem);
            } else if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
                handle_key(&e.key);
            } else if (e.type == SDL_MOUSEMOTION) {
                float x, y;
                normalized(e.motion.x, e.motion.y, &x, &y);
                send_mouse_move(x, y);
            } else if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) {
                int b = mac_button(e.button.button);
                if (b >= 0) {
                    bool down = e.type == SDL_MOUSEBUTTONDOWN;
                    float x, y;
                    normalized(e.button.x, e.button.y, &x, &y);
                    if (down) buttons_down |= 1u << b;
                    else buttons_down &= ~(1u << b);
                    send_mouse_button(b, down, x, y);
                }
            } else if (e.type == SDL_MOUSEWHEEL) {
                // Mac points per wheel notch. Positive dy moves content down (wheel up), like the
                // iPad's two-finger drag.
                float step = 40 * opt.scroll_speed * (opt.invert_scroll ? -1 : 1);
                float dir = e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1 : 1;
                send_scroll(-e.wheel.preciseX * dir * step, e.wheel.preciseY * dir * step);
            }
        } while (running && SDL_PollEvent(&e));

        if (resize_at && SDL_GetTicks() - resize_at > 400) {
            resize_at = 0;
            SDL_GetRendererOutputSize(renderer, &dw, &dh);
            if (dw != atomic_load(&request_w) || dh != atomic_load(&request_h)) {
                atomic_store(&request_w, dw);
                atomic_store(&request_h, dh);
                send_display_request(dw, dh);
            }
        }
    }

    release_all();
    atomic_store(&quitting, true);
    SDL_RemoveTimer(timer);
    SDL_LockMutex(send_lock);
    if (sock >= 0) shutdown(sock, SHUT_RDWR);
    SDL_UnlockMutex(send_lock);
    SDL_WaitThread(net, NULL);
    av_frame_free(&pending_frame);
    av_frame_free(&shown_frame);
    if (texture) SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return exit_code;
}
