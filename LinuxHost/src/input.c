#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>

#include "host.h"

// Clients send macOS virtual key codes. kVK → USB HID usage (the inverse of the client's
// table in Linux/src/keymap.c) → Linux evdev code (the kernel's usbkbd table) → X keycode (+8).
static const unsigned char usb_kbd_keycode[256] = {
      0,  0,  0,  0, 30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38,
     50, 49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44,  2,  3,
      4,  5,  6,  7,  8,  9, 10, 11, 28,  1, 14, 15, 57, 12, 13, 26,
     27, 43, 43, 39, 40, 41, 51, 52, 53, 58, 59, 60, 61, 62, 63, 64,
     65, 66, 67, 68, 87, 88, 99, 70,119,110,102,104,111,107,109,106,
    105,108,103, 69, 98, 55, 74, 78, 96, 79, 80, 81, 75, 76, 77, 71,
     72, 73, 82, 83, 86,127,116,117,183,184,185,186,187,188,189,190,
    191,192,193,194,134,138,130,132,128,129,131,137,133,135,136,113,
    115,114,  0,  0,  0,121,  0, 89, 93,124, 92, 94, 95,  0,  0,  0,
    122,123, 90, 91, 85,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     29, 42, 56,125, 97, 54,100,126,164,166,165,163,161,115,114,113,
    150,158,159,128,136,177,178,176,142,152,173,140,
};

enum { EV_LCTRL = 29, EV_LSHIFT = 42, EV_LALT = 56, EV_LMETA = 125, EV_RCTRL = 97, EV_RMETA = 126 };

struct Input {
    Display *dpy;
    int rx, ry, rw, rh;
    enum CmdMode cmd;
    int kvk_to_evdev[128];
    bool held[256];                 // evdev codes we pressed
    unsigned buttons;               // X buttons we pressed
    float scroll_x, scroll_y;
    // Unused keycodes borrowed to type characters with no key. Apps read the new mapping only
    // when they get to the key event, so a borrowed key stays mapped until the whole text is
    // typed (and a pause), and consecutive characters use different keys.
    int scratch[16];
    int nscratch, next_scratch;
    unsigned used_scratch;
};

Input *input_open(const char *display) {
    Input *in = calloc(1, sizeof *in);
    in->dpy = XOpenDisplay(display);
    int ev, err, major, minor;
    if (!in->dpy || !XTestQueryExtension(in->dpy, &ev, &err, &major, &minor)) {
        host_log("XTest unavailable; input disabled");
        if (in->dpy) XCloseDisplay(in->dpy);
        in->dpy = NULL;
        return in;
    }
    XTestGrabControl(in->dpy, True);  // keep injecting even if another client grabs the server
    for (int i = 0; i < 128; i++) in->kvk_to_evdev[i] = 0;
    for (int usage = 0xE7; usage >= 0; usage--) {   // descending, so the lowest usage wins
        int kvk = keymap_mac_code(usage);
        if (kvk >= 0 && kvk < 128 && usb_kbd_keycode[usage]) in->kvk_to_evdev[kvk] = usb_kbd_keycode[usage];
    }

    // A keycode with no symbols, for typing arbitrary Unicode (xdotool's trick).
    int min, max, per;
    XDisplayKeycodes(in->dpy, &min, &max);
    KeySym *map = XGetKeyboardMapping(in->dpy, min, max - min + 1, &per);
    for (int kc = max; kc >= min && map && in->nscratch < 16; kc--) {
        bool empty = true;
        for (int i = 0; i < per; i++)
            if (map[(kc - min) * per + i] != NoSymbol) empty = false;
        if (empty) in->scratch[in->nscratch++] = kc;
    }
    if (map) XFree(map);
    return in;
}

void input_close(Input *in) {
    if (!in) return;
    if (in->dpy) XCloseDisplay(in->dpy);
    free(in);
}

void input_set_region(Input *in, int x, int y, int w, int h) {
    in->rx = x; in->ry = y; in->rw = w; in->rh = h;
}

void input_set_cmd(Input *in, enum CmdMode mode) { in->cmd = mode; }

static void flush(Input *in) { XFlush(in->dpy); }

static void point(Input *in, float x, float y, int *px, int *py) {
    x = fminf(fmaxf(x, 0), 1);
    y = fminf(fmaxf(y, 0), 1);
    *px = in->rx + (int)lroundf(x * (in->rw - 1));
    *py = in->ry + (int)lroundf(y * (in->rh - 1));
}

void input_move(Input *in, float x, float y) {
    if (!in->dpy) return;
    int px, py;
    point(in, x, y, &px, &py);
    XTestFakeMotionEvent(in->dpy, -1, px, py, CurrentTime);
    flush(in);
}

void input_button(Input *in, int button, bool down, float x, float y) {
    if (!in->dpy) return;
    // Protocol: 0 left, 1 right, 2 middle, 3/4 back/forward. X: 1 left, 2 middle, 3 right, 8/9.
    static const int map[] = { 1, 3, 2, 8, 9 };
    if (button < 0 || button > 4) return;
    int xb = map[button];
    input_move(in, x, y);
    XTestFakeButtonEvent(in->dpy, xb, down, CurrentTime);
    if (down) in->buttons |= 1u << xb;
    else in->buttons &= ~(1u << xb);
    flush(in);
}

static void click(Input *in, int xb, int count) {
    for (int i = 0; i < count; i++) {
        XTestFakeButtonEvent(in->dpy, xb, True, CurrentTime);
        XTestFakeButtonEvent(in->dpy, xb, False, CurrentTime);
    }
}

void input_scroll(Input *in, float dx, float dy) {
    if (!in->dpy) return;
    // Core X scrolling is in clicks (buttons 4-7). Positive dy moves content down, i.e. wheel
    // up. ~40 Mac points per click, which is about what one click scrolls in X apps.
    const float step = 40;
    in->scroll_x += dx;
    in->scroll_y += dy;
    int cy = (int)(in->scroll_y / step), cx = (int)(in->scroll_x / step);
    in->scroll_y -= cy * step;
    in->scroll_x -= cx * step;
    if (cy > 0) click(in, 4, cy);
    if (cy < 0) click(in, 5, -cy);
    if (cx > 0) click(in, 6, cx);
    if (cx < 0) click(in, 7, -cx);
    if (cx || cy) flush(in);
}

static void press(Input *in, int evdev, bool down) {
    if (evdev <= 0 || evdev > 247) return;
    if (in->held[evdev] == down && down == false) return;
    XTestFakeKeyEvent(in->dpy, evdev + 8, down, CurrentTime);
    in->held[evdev] = down;
}

/// Maps the Mac modifier keys onto Linux ones. ⌃ → Ctrl, ⌥ → Alt, ⇧ → Shift always; ⌘ → Ctrl or Super.
static int remap(Input *in, int kvk, int evdev) {
    bool cmd_is_ctrl = in->cmd == CMD_CTRL;
    if (kvk == 0x37) return cmd_is_ctrl ? EV_LCTRL : EV_LMETA;
    if (kvk == 0x36) return cmd_is_ctrl ? EV_RCTRL : EV_RMETA;
    return evdev;
}

/// Makes the held modifiers match what the client says is down (clients that only send flags,
/// or a key-up lost in a focus change, would otherwise leave them stuck or missing).
static void sync_modifiers(Input *in, uint32_t mods) {
    bool cmd_is_ctrl = in->cmd == CMD_CTRL;
    bool want_ctrl = (mods & MOD_CONTROL) || (cmd_is_ctrl && (mods & MOD_COMMAND));
    bool want_meta = !cmd_is_ctrl && (mods & MOD_COMMAND);
    struct { bool want; int left, right; } groups[] = {
        { mods & MOD_SHIFT, EV_LSHIFT, 54 },
        { want_ctrl, EV_LCTRL, EV_RCTRL },
        { mods & MOD_OPTION, EV_LALT, 100 },
        { want_meta, EV_LMETA, EV_RMETA },
    };
    for (size_t i = 0; i < sizeof groups / sizeof *groups; i++) {
        bool down = in->held[groups[i].left] || in->held[groups[i].right];
        if (groups[i].want && !down) press(in, groups[i].left, true);
        if (!groups[i].want && down) {
            press(in, groups[i].left, false);
            press(in, groups[i].right, false);
        }
    }
}

void input_key(Input *in, int kvk, int action, uint32_t mods) {
    if (!in->dpy || kvk < 0 || kvk >= 128) return;
    int evdev = remap(in, kvk, in->kvk_to_evdev[kvk]);
    if (!evdev) return;
    bool is_modifier = keymap_modifier(kvk) != 0;
    if (action != KEY_UP && !is_modifier && kvk != 0x39) sync_modifiers(in, mods);
    if (action == KEY_REPEAT) {
        // X generates its own autorepeat from a held key; an explicit repeat is a press.
        XTestFakeKeyEvent(in->dpy, evdev + 8, False, CurrentTime);
        XTestFakeKeyEvent(in->dpy, evdev + 8, True, CurrentTime);
        in->held[evdev] = true;
    } else {
        press(in, evdev, action == KEY_DOWN);
    }
    flush(in);
}

static uint32_t next_codepoint(const unsigned char **p, const unsigned char *end) {
    const unsigned char *s = *p;
    uint32_t c = *s++;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (extra) c &= 0x3F >> extra;
    while (extra-- && s < end) c = c << 6 | (*s++ & 0x3F);
    *p = s;
    return c;
}

static void type_keysym(Input *in, KeySym sym) {
    KeyCode kc = XKeysymToKeycode(in->dpy, sym);
    if (kc) {
        bool shift = XkbKeycodeToKeysym(in->dpy, kc, 0, 0) != sym && XkbKeycodeToKeysym(in->dpy, kc, 0, 1) == sym;
        bool shift_held = in->held[EV_LSHIFT] || in->held[54];
        if (shift && !shift_held) XTestFakeKeyEvent(in->dpy, EV_LSHIFT + 8, True, CurrentTime);
        XTestFakeKeyEvent(in->dpy, kc, True, CurrentTime);
        XTestFakeKeyEvent(in->dpy, kc, False, CurrentTime);
        if (shift && !shift_held) XTestFakeKeyEvent(in->dpy, EV_LSHIFT + 8, False, CurrentTime);
        return;
    }
    if (!in->nscratch) return;
    int slot = in->next_scratch;
    in->next_scratch = (slot + 1) % in->nscratch;
    if (in->used_scratch & (1u << slot)) {
        // Wrapped around the pool: give apps time to read the old mapping before reusing it.
        XSync(in->dpy, False);
        usleep(30000);
    }
    in->used_scratch |= 1u << slot;
    int borrowed = in->scratch[slot];
    KeySym syms[2] = { sym, sym };
    XChangeKeyboardMapping(in->dpy, borrowed, 2, syms, 1);
    XSync(in->dpy, False);
    XTestFakeKeyEvent(in->dpy, borrowed, True, CurrentTime);
    XTestFakeKeyEvent(in->dpy, borrowed, False, CurrentTime);
}

static void restore_scratch(Input *in) {
    if (!in->used_scratch) return;
    XSync(in->dpy, False);
    usleep(30000);
    KeySym none[2] = { NoSymbol, NoSymbol };
    for (int i = 0; i < in->nscratch; i++)
        if (in->used_scratch & (1u << i)) XChangeKeyboardMapping(in->dpy, in->scratch[i], 2, none, 1);
    in->used_scratch = 0;
    in->next_scratch = 0;
}

/// Types Unicode text (the iPad's on-screen keyboard sends these).
void input_text(Input *in, const char *utf8, size_t len) {
    if (!in->dpy) return;
    const unsigned char *p = (const unsigned char *)utf8, *end = p + len;
    while (p < end) {
        uint32_t c = next_codepoint(&p, end);
        KeySym sym;
        if (c == '\n' || c == '\r') sym = XK_Return;
        else if (c == '\t') sym = XK_Tab;
        else if (c == '\b') sym = XK_BackSpace;
        else if ((c >= 0x20 && c < 0x7F) || (c >= 0xA0 && c <= 0xFF)) sym = c;  // Latin-1 keysyms are the code points
        else sym = 0x01000000 | c;
        type_keysym(in, sym);
    }
    restore_scratch(in);
    flush(in);
}

void input_release_all(Input *in) {
    if (!in->dpy) return;
    for (int b = 0; b < 32; b++)
        if (in->buttons & (1u << b)) XTestFakeButtonEvent(in->dpy, b, False, CurrentTime);
    in->buttons = 0;
    for (int k = 0; k < 256; k++)
        if (in->held[k]) press(in, k, false);
    flush(in);
}

void input_keep_awake(Input *in) {
    if (!in->dpy) return;
    XForceScreenSaver(in->dpy, ScreenSaverReset);
    flush(in);
}
