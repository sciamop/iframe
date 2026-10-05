#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrandr.h>

#include "host.h"

struct Capture {
    Display *dpy;
    Window root;
    int x, y, width, height;          // monitor rectangle in root coordinates (width/height even)
    int refresh_hz;
    XImage *image;
    XShmSegmentInfo shm;
    bool has_damage;
    int damage_event;
    Damage damage;
    int fixes_event;
    XFixesCursorImage *cursor;        // cached until the cursor changes shape
    bool cursor_dirty;
    int pointer_x, pointer_y;
    uint64_t last_full_check;
};

static int ignore_errors(Display *d, XErrorEvent *e) { (void)d; (void)e; return 0; }

/// Picks monitor `index` (-1 = the primary one) from XRandR, falling back to the whole root.
static void pick_monitor(Capture *c, int index) {
    c->x = 0;
    c->y = 0;
    c->width = DisplayWidth(c->dpy, DefaultScreen(c->dpy));
    c->height = DisplayHeight(c->dpy, DefaultScreen(c->dpy));
    c->refresh_hz = 60;
    int count = 0;
    XRRMonitorInfo *monitors = XRRGetMonitors(c->dpy, c->root, True, &count);
    if (monitors && count > 0) {
        int pick = 0;
        for (int i = 0; i < count; i++)
            if (index < 0 ? monitors[i].primary : i == index) pick = i;
        if (index >= count) host_log("monitor %d does not exist; using %d", index, pick);
        c->x = monitors[pick].x;
        c->y = monitors[pick].y;
        c->width = monitors[pick].width;
        c->height = monitors[pick].height;
        char *name = XGetAtomName(c->dpy, monitors[pick].name);
        host_log("capturing monitor %d (%s) %dx%d+%d+%d of %d", pick, name ? name : "?", c->width, c->height,
                 c->x, c->y, count);
        if (name) XFree(name);
    }
    if (monitors) XRRFreeMonitors(monitors);

    // Refresh rate of the CRTC that shows this monitor.
    XRRScreenResources *res = XRRGetScreenResourcesCurrent(c->dpy, c->root);
    if (res) {
        for (int i = 0; i < res->ncrtc; i++) {
            XRRCrtcInfo *crtc = XRRGetCrtcInfo(c->dpy, res, res->crtcs[i]);
            if (!crtc) continue;
            if (crtc->mode && crtc->x == c->x && crtc->y == c->y) {
                for (int m = 0; m < res->nmode; m++) {
                    XRRModeInfo *mode = &res->modes[m];
                    if (mode->id == crtc->mode && mode->hTotal && mode->vTotal) {
                        double hz = (double)mode->dotClock / ((double)mode->hTotal * mode->vTotal);
                        if (hz > 1) c->refresh_hz = (int)(hz + 0.5);
                    }
                }
            }
            XRRFreeCrtcInfo(crtc);
        }
        XRRFreeScreenResources(res);
    }
    c->width &= ~1;
    c->height &= ~1;
}

Capture *capture_open(const char *display, int monitor, char *err, size_t err_cap) {
    Capture *c = calloc(1, sizeof *c);
    c->dpy = XOpenDisplay(display);
    if (!c->dpy) {
        snprintf(err, err_cap, "can't open X display %s", display ? display : "$DISPLAY");
        free(c);
        return NULL;
    }
    c->root = DefaultRootWindow(c->dpy);
    if (!XShmQueryExtension(c->dpy)) {
        snprintf(err, err_cap, "X server has no MIT-SHM");
        capture_close(c);
        return NULL;
    }
    pick_monitor(c, monitor);

    Visual *visual = DefaultVisual(c->dpy, DefaultScreen(c->dpy));
    int depth = DefaultDepth(c->dpy, DefaultScreen(c->dpy));
    c->image = XShmCreateImage(c->dpy, visual, depth, ZPixmap, NULL, &c->shm, c->width, c->height);
    if (!c->image || c->image->bits_per_pixel != 32) {
        snprintf(err, err_cap, "need a 24/32-bit TrueColor display (got %d bpp)", c->image ? c->image->bits_per_pixel : 0);
        capture_close(c);
        return NULL;
    }
    c->shm.shmid = shmget(IPC_PRIVATE, (size_t)c->image->bytes_per_line * c->image->height, IPC_CREAT | 0600);
    c->shm.shmaddr = c->image->data = shmat(c->shm.shmid, NULL, 0);
    c->shm.readOnly = False;
    XShmAttach(c->dpy, &c->shm);
    XSync(c->dpy, False);
    shmctl(c->shm.shmid, IPC_RMID, NULL);  // freed once both sides detach

    int error_base;
    if (XDamageQueryExtension(c->dpy, &c->damage_event, &error_base)) {
        c->damage = XDamageCreate(c->dpy, c->root, XDamageReportNonEmpty);
        c->has_damage = true;
    } else {
        host_log("no XDamage; checking the whole screen every frame");
    }
    if (XFixesQueryExtension(c->dpy, &c->fixes_event, &error_base))
        XFixesSelectCursorInput(c->dpy, c->root, XFixesDisplayCursorNotifyMask);
    c->cursor_dirty = true;
    c->pointer_x = c->pointer_y = -1;
    XSetErrorHandler(ignore_errors);
    return c;
}

void capture_close(Capture *c) {
    if (!c) return;
    if (c->cursor) XFree(c->cursor);
    if (c->has_damage) XDamageDestroy(c->dpy, c->damage);
    if (c->image) {
        if (c->shm.shmaddr) {
            XShmDetach(c->dpy, &c->shm);
            shmdt(c->shm.shmaddr);
        }
        c->image->data = NULL;
        XDestroyImage(c->image);
    }
    if (c->dpy) XCloseDisplay(c->dpy);
    free(c);
}

int capture_width(Capture *c) { return c->width; }
int capture_height(Capture *c) { return c->height; }
int capture_refresh_hz(Capture *c) { return c->refresh_hz; }
int capture_fd(Capture *c) { return ConnectionNumber(c->dpy); }
void capture_monitor_origin(Capture *c, int *x, int *y) { *x = c->x; *y = c->y; }

static bool near_monitor(Capture *c, int x, int y) {
    return x >= c->x - 64 && x < c->x + c->width + 64 && y >= c->y - 64 && y < c->y + c->height + 64;
}

bool capture_poll_changes(Capture *c) {
    bool changed = !c->has_damage;
    while (XPending(c->dpy)) {
        XEvent e;
        XNextEvent(c->dpy, &e);
        if (c->has_damage && e.type == c->damage_event + XDamageNotify) {
            XDamageNotifyEvent *d = (XDamageNotifyEvent *)&e;
            // Only damage that touches our monitor counts.
            if (d->area.x < c->x + c->width && d->area.x + d->area.width > c->x &&
                d->area.y < c->y + c->height && d->area.y + d->area.height > c->y)
                changed = true;
            XDamageSubtract(c->dpy, c->damage, None, None);
        } else if (e.type == c->fixes_event + XFixesCursorNotify) {
            c->cursor_dirty = true;
            changed = true;
        }
    }
    // X has no event for pointer motion on the root without grabbing, so ask.
    Window root_ret, child;
    int rx, ry, wx, wy;
    unsigned int mask;
    if (XQueryPointer(c->dpy, c->root, &root_ret, &child, &rx, &ry, &wx, &wy, &mask) &&
        (rx != c->pointer_x || ry != c->pointer_y)) {
        // A cursor near the edge can still overlap the monitor, hence the margin.
        if (near_monitor(c, rx, ry) || near_monitor(c, c->pointer_x, c->pointer_y)) changed = true;
        c->pointer_x = rx;
        c->pointer_y = ry;
    }
    return changed;
}

static void draw_cursor(Capture *c) {
    if (c->cursor_dirty) {
        if (c->cursor) XFree(c->cursor);
        c->cursor = XFixesGetCursorImage(c->dpy);
        c->cursor_dirty = false;
    }
    XFixesCursorImage *cur = c->cursor;
    if (!cur) return;
    int ox = c->pointer_x - cur->xhot - c->x, oy = c->pointer_y - cur->yhot - c->y;
    uint8_t *base = (uint8_t *)c->image->data;
    int stride = c->image->bytes_per_line;
    for (int y = 0; y < cur->height; y++) {
        int py = oy + y;
        if (py < 0 || py >= c->height) continue;
        uint32_t *row = (uint32_t *)(base + (size_t)py * stride);
        for (int x = 0; x < cur->width; x++) {
            int px = ox + x;
            if (px < 0 || px >= c->width) continue;
            // XFixes pixels are premultiplied ARGB in an unsigned long.
            uint32_t s = (uint32_t)cur->pixels[y * cur->width + x];
            uint32_t a = s >> 24;
            if (a == 0) continue;
            uint32_t d = row[px];
            uint32_t r = ((s >> 16) & 0xFF) + (((d >> 16) & 0xFF) * (255 - a) + 127) / 255;
            uint32_t g = ((s >> 8) & 0xFF) + (((d >> 8) & 0xFF) * (255 - a) + 127) / 255;
            uint32_t b = (s & 0xFF) + ((d & 0xFF) * (255 - a) + 127) / 255;
            row[px] = (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
        }
    }
}

bool capture_grab(Capture *c) {
    if (!XShmGetImage(c->dpy, c->root, c->image, c->x, c->y, AllPlanes)) return false;
    draw_cursor(c);
    return true;
}

const uint8_t *capture_pixels(Capture *c, int *stride) {
    *stride = c->image->bytes_per_line;
    return (const uint8_t *)c->image->data;
}
