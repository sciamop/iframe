#include "host.h"  // first: winsock2.h has to come before windows.h

#include <math.h>
#include <stdlib.h>
#include <string.h>

// Clients send macOS virtual key codes. kVK → USB HID usage (the inverse of the client's table in
// Linux/src/keymap.c) → PC scan code set 1, which SendInput takes directly so every key lands by
// position whatever the Windows keyboard layout is. 0xE0xx = extended key.
static const uint16_t hid_to_scan[0xE8] = {
    /* 0x00 */ 0, 0, 0, 0,
    /* a-z  */ 0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19,
               0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C,
    /* 1-0  */ 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
    /* 0x28 */ 0x1C, 0x01, 0x0E, 0x0F, 0x39, 0x0C, 0x0D, 0x1A, 0x1B, 0x2B, 0x2B, 0x27, 0x28, 0x29, 0x33, 0x34,
    /* 0x38 */ 0x35, 0x3A,
    /* F1   */ 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x57, 0x58,
    /* 0x46 */ 0xE037, 0x46, 0,                                  // print screen, scroll lock, (pause)
    /* 0x49 */ 0xE052, 0xE047, 0xE049, 0xE053, 0xE04F, 0xE051,   // insert home pgup delete end pgdn
    /* 0x4F */ 0xE04D, 0xE04B, 0xE050, 0xE048,                   // right left down up
    /* 0x53 */ 0xE045, 0xE035, 0x37, 0x4A, 0x4E, 0xE01C,         // num lock, keypad / * - + enter
    /* 0x59 */ 0x4F, 0x50, 0x51, 0x4B, 0x4C, 0x4D, 0x47, 0x48, 0x49, 0x52, 0x53,  // keypad 1-9 0 .
    /* 0x64 */ 0x56, 0xE05D, 0, 0x59,                            // non-US \, menu, (power), keypad =
    /* F13  */ 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D, 0x6E, 0x76,
    /* 0x74 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x7F */ 0xE020, 0xE030, 0xE02E,                           // mute, volume up, volume down
    /* 0x82 */ 0, 0, 0, 0, 0,
    /* 0x87 */ 0x73, 0x70, 0x7D, 0, 0, 0, 0, 0, 0,               // international 1-3 (ro, kana, yen)
    /* 0x90 */ 0xF2, 0xF1,                                       // lang 1-2 (hangul/kana, hanja/eisu)
};

enum {
    SC_LCTRL = 0x1D, SC_LSHIFT = 0x2A, SC_LALT = 0x38, SC_LWIN = 0xE05B,
    SC_RCTRL = 0xE01D, SC_RSHIFT = 0x36, SC_RALT = 0xE038, SC_RWIN = 0xE05C,
    SC_RETURN = 0x1C, SC_TAB = 0x0F, SC_BACKSPACE = 0x0E,
};

static uint16_t modifier_scan(int usage) {
    static const uint16_t mods[8] = { SC_LCTRL, SC_LSHIFT, SC_LALT, SC_LWIN, SC_RCTRL, SC_RSHIFT, SC_RALT, SC_RWIN };
    return usage >= 0xE0 && usage <= 0xE7 ? mods[usage - 0xE0] : 0;
}

struct Input {
    RECT region;
    enum CmdMode cmd;
    uint16_t kvk_to_scan[128];
    bool held[0x200];               // scan codes we pressed, indexed (scan & 0xFF) | extended << 8
    unsigned buttons;               // protocol buttons we pressed
    float scroll_x, scroll_y;
};

static int slot(uint16_t scan) { return (scan & 0xFF) | ((scan >> 8) == 0xE0 ? 0x100 : 0); }

Input *input_open(void) {
    Input *in = (Input *)calloc(1, sizeof *in);
    for (int usage = 0xE7; usage >= 0; usage--) {   // descending, so the lowest usage wins
        int kvk = keymap_mac_code(usage);
        uint16_t scan = usage >= 0xE0 ? modifier_scan(usage) : usage < 0x92 ? hid_to_scan[usage] : 0;
        if (kvk >= 0 && kvk < 128 && scan) in->kvk_to_scan[kvk] = scan;
    }
    return in;
}

void input_close(Input *in) { free(in); }
void input_set_region(Input *in, RECT region) { in->region = region; }
void input_set_cmd(Input *in, enum CmdMode mode) { in->cmd = mode; }

static void send_key(uint16_t scan, bool down) {
    INPUT ev = {};
    ev.type = INPUT_KEYBOARD;
    ev.ki.wScan = scan & 0xFF;
    ev.ki.dwFlags = KEYEVENTF_SCANCODE | ((scan >> 8) == 0xE0 ? KEYEVENTF_EXTENDEDKEY : 0) | (down ? 0 : KEYEVENTF_KEYUP);
    SendInput(1, &ev, sizeof ev);
}

static void press(Input *in, uint16_t scan, bool down) {
    if (!scan) return;
    bool *held = &in->held[slot(scan)];
    if (!down && !*held) return;
    send_key(scan, down);
    *held = down;
}

/// Absolute position on the virtual desktop, which SendInput wants as 0-65535.
static void point(Input *in, float x, float y, LONG *ax, LONG *ay) {
    x = fminf(fmaxf(x, 0), 1);
    y = fminf(fmaxf(y, 0), 1);
    LONG px = in->region.left + lroundf(x * (in->region.right - in->region.left - 1));
    LONG py = in->region.top + lroundf(y * (in->region.bottom - in->region.top - 1));
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    *ax = vw > 1 ? (LONG)(((int64_t)(px - vx) * 65535 + (vw - 1) / 2) / (vw - 1)) : 0;
    *ay = vh > 1 ? (LONG)(((int64_t)(py - vy) * 65535 + (vh - 1) / 2) / (vh - 1)) : 0;
}

static void mouse(Input *in, float x, float y, DWORD flags, DWORD data) {
    INPUT ev = {};
    ev.type = INPUT_MOUSE;
    point(in, x, y, &ev.mi.dx, &ev.mi.dy);
    ev.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | flags;
    ev.mi.mouseData = data;
    SendInput(1, &ev, sizeof ev);
}

void input_move(Input *in, float x, float y) { mouse(in, x, y, 0, 0); }

static void button_flags(int button, bool down, DWORD *flags, DWORD *data) {
    // Protocol: 0 left, 1 right, 2 middle, 3/4 back/forward.
    static const DWORD downs[] = { MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_MIDDLEDOWN,
                                   MOUSEEVENTF_XDOWN, MOUSEEVENTF_XDOWN };
    static const DWORD ups[] = { MOUSEEVENTF_LEFTUP, MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_MIDDLEUP, MOUSEEVENTF_XUP,
                                 MOUSEEVENTF_XUP };
    *flags = down ? downs[button] : ups[button];
    *data = button == 3 ? XBUTTON1 : button == 4 ? XBUTTON2 : 0;
}

void input_button(Input *in, int button, bool down, float x, float y) {
    if (button < 0 || button > 4) return;
    DWORD flags, data;
    button_flags(button, down, &flags, &data);
    mouse(in, x, y, flags, data);
    if (down) in->buttons |= 1u << button;
    else in->buttons &= ~(1u << button);
}

void input_scroll(Input *in, float dx, float dy) {
    // Positive dy moves content down, i.e. wheel up. ~40 Mac points per 120-unit notch, like the
    // Linux host's clicks; Windows takes the fractions, so trackpads scroll smoothly.
    const float per_point = WHEEL_DELTA / 40.0f;
    in->scroll_y += dy * per_point;
    in->scroll_x += dx * per_point;
    int wy = (int)in->scroll_y, wx = (int)in->scroll_x;
    in->scroll_y -= wy;
    in->scroll_x -= wx;
    INPUT ev[2] = {};
    int n = 0;
    if (wy) {
        ev[n].type = INPUT_MOUSE;
        ev[n].mi.dwFlags = MOUSEEVENTF_WHEEL;
        ev[n++].mi.mouseData = (DWORD)wy;
    }
    if (wx) {
        ev[n].type = INPUT_MOUSE;
        ev[n].mi.dwFlags = MOUSEEVENTF_HWHEEL;
        ev[n++].mi.mouseData = (DWORD)-wx;  // content moving right = scrolling left
    }
    if (n) SendInput(n, ev, sizeof *ev);
}

/// Maps the Mac modifier keys onto PC ones. ⌃ → Ctrl, ⌥ → Alt, ⇧ → Shift always; ⌘ → Ctrl or Windows.
static uint16_t remap(Input *in, int kvk, uint16_t scan) {
    bool cmd_is_ctrl = in->cmd == CMD_CTRL;
    if (kvk == 0x37) return cmd_is_ctrl ? SC_LCTRL : SC_LWIN;
    if (kvk == 0x36) return cmd_is_ctrl ? SC_RCTRL : SC_RWIN;
    return scan;
}

/// Makes the held modifiers match what the client says is down (clients that only send flags,
/// or a key-up lost in a focus change, would otherwise leave them stuck or missing).
static void sync_modifiers(Input *in, uint32_t mods) {
    bool cmd_is_ctrl = in->cmd == CMD_CTRL;
    bool want_ctrl = (mods & MOD_CONTROL) || (cmd_is_ctrl && (mods & MOD_COMMAND));
    bool want_win = !cmd_is_ctrl && (mods & MOD_COMMAND);
    struct { bool want; uint16_t left, right; } groups[] = {
        { (mods & MOD_SHIFT) != 0, SC_LSHIFT, SC_RSHIFT },
        { want_ctrl, SC_LCTRL, SC_RCTRL },
        { (mods & MOD_OPTION) != 0, SC_LALT, SC_RALT },
        { want_win, SC_LWIN, SC_RWIN },
    };
    for (auto &g : groups) {
        bool down = in->held[slot(g.left)] || in->held[slot(g.right)];
        if (g.want && !down) press(in, g.left, true);
        if (!g.want && down) {
            press(in, g.left, false);
            press(in, g.right, false);
        }
    }
}

void input_key(Input *in, int kvk, int action, uint32_t mods) {
    if (kvk < 0 || kvk >= 128) return;
    uint16_t scan = remap(in, kvk, in->kvk_to_scan[kvk]);
    if (!scan) return;
    bool is_modifier = keymap_modifier(kvk) != 0;
    if (action != KEY_UP && !is_modifier && kvk != 0x39) sync_modifiers(in, mods);
    // Windows doesn't autorepeat injected keys, so a repeat is just another key-down.
    press(in, scan, action != KEY_UP);
}

static void type_unit(WCHAR unit) {
    INPUT ev[2] = {};
    for (int i = 0; i < 2; i++) {
        ev[i].type = INPUT_KEYBOARD;
        ev[i].ki.wScan = unit;
        ev[i].ki.dwFlags = KEYEVENTF_UNICODE | (i ? KEYEVENTF_KEYUP : 0);
    }
    SendInput(2, ev, sizeof *ev);
}

/// Types Unicode text (the iPad's on-screen keyboard sends these). Windows types any character
/// directly, whatever the keyboard layout.
void input_text(Input *in, const char *utf8, size_t len) {
    (void)in;
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, (int)len, NULL, 0);
    if (n <= 0) return;
    WCHAR *text = (WCHAR *)malloc(n * sizeof(WCHAR));
    MultiByteToWideChar(CP_UTF8, 0, utf8, (int)len, text, n);
    for (int i = 0; i < n; i++) {
        WCHAR c = text[i];
        uint16_t key = c == '\n' || c == '\r' ? SC_RETURN : c == '\t' ? SC_TAB : c == '\b' ? SC_BACKSPACE : 0;
        if (c == '\r' && i + 1 < n && text[i + 1] == '\n') continue;
        if (key) {
            send_key(key, true);
            send_key(key, false);
        } else {
            type_unit(c);
        }
    }
    free(text);
}

void input_release_all(Input *in) {
    for (int b = 0; b < 5; b++)
        if (in->buttons & (1u << b)) {
            INPUT ev = {};
            ev.type = INPUT_MOUSE;
            button_flags(b, false, &ev.mi.dwFlags, &ev.mi.mouseData);
            SendInput(1, &ev, sizeof ev);
        }
    in->buttons = 0;
    for (int s = 0; s < 0x200; s++)
        if (in->held[s]) press(in, (uint16_t)((s & 0xFF) | (s & 0x100 ? 0xE000 : 0)), false);
}

void input_keep_awake(Input *in) {
    (void)in;
    // Resets the idle timers once, so the display doesn't sleep while a client is watching.
    SetThreadExecutionState(ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
}
