#include "host.h"  // first: winsock2.h has to come before windows.h

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <d3d11_4.h>
#include <dxgi1_5.h>
#include <wincodec.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

struct Capture {
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput1> output;
    DXGI_OUTPUT_DESC desc;
    LUID luid;
    char adapter_name[128];
    int width, height, refresh_hz;

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIOutputDuplication> dup;
    ComPtr<ID3D11Texture2D> frame;      // our copy of the newest desktop image
    ULONGLONG retry_at;                 // after losing the duplication (UAC, lock screen, mode change)
    bool warned_lost;

    // Pointer shape, as Desktop Duplication last reported it, until it's converted and sent.
    uint8_t *shape;
    UINT shape_cap, shape_len;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO shape_info;
    bool shape_pending;
};

typedef struct {
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput1> output;
    DXGI_OUTPUT_DESC desc;
    DXGI_ADAPTER_DESC1 adapter_desc;
} Monitor;

// Every monitor attached to the desktop, in adapter order. The primary is the one at (0, 0).
static int enumerate(Monitor *out, int max) {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return 0;
    int n = 0;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; n < max && factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; a++) {
        DXGI_ADAPTER_DESC1 ad;
        adapter->GetDesc1(&ad);
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; n < max && adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; o++) {
            DXGI_OUTPUT_DESC desc;
            ComPtr<IDXGIOutput1> output1;
            if (FAILED(output->GetDesc(&desc)) || !desc.AttachedToDesktop || FAILED(output.As(&output1))) continue;
            out[n].adapter = adapter;
            out[n].output = output1;
            out[n].desc = desc;
            out[n].adapter_desc = ad;
            n++;
        }
    }
    return n;
}

static int refresh_hz(const DXGI_OUTPUT_DESC &desc) {
    DEVMODEW mode = {};
    mode.dmSize = sizeof mode;
    if (EnumDisplaySettingsW(desc.DeviceName, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1)
        return (int)mode.dmDisplayFrequency;
    return 60;
}

static bool is_primary(const DXGI_OUTPUT_DESC &desc) {
    return desc.DesktopCoordinates.left == 0 && desc.DesktopCoordinates.top == 0;
}

void capture_list_monitors(void) {
    Monitor m[16];
    int n = enumerate(m, 16);
    if (!n) printf("  No monitors found.\n");
    for (int i = 0; i < n; i++) {
        RECT r = m[i].desc.DesktopCoordinates;
        printf("  %d: %ldx%ld at (%ld, %ld), %d Hz, on %ls%s\n", i, r.right - r.left, r.bottom - r.top, r.left,
               r.top, refresh_hz(m[i].desc), m[i].adapter_desc.Description, is_primary(m[i].desc) ? " (primary)" : "");
    }
}

static HRESULT duplicate(Capture *c) {
    c->dup.Reset();
    desktop_follow_input();  // the sign-in screen and UAC prompts are on another desktop
    // DuplicateOutput1 asks for BGRA explicitly, which also works when the desktop is HDR.
    ComPtr<IDXGIOutput5> output5;
    if (SUCCEEDED(c->output.As(&output5))) {
        DXGI_FORMAT formats[] = { DXGI_FORMAT_B8G8R8A8_UNORM };
        HRESULT hr = output5->DuplicateOutput1(c->device.Get(), 0, 1, formats, &c->dup);
        if (SUCCEEDED(hr)) return hr;
    }
    return c->output->DuplicateOutput(c->device.Get(), &c->dup);
}

// monitor: -1 = the primary. -1 when there's no such monitor.
static int pick_monitor(const Monitor *m, int n, int monitor) {
    if (monitor >= 0) return monitor < n ? monitor : -1;
    for (int i = 0; i < n; i++)
        if (is_primary(m[i].desc)) return i;
    return n ? 0 : -1;
}

bool capture_match_mode(int monitor, int width, int height) {
    Monitor m[16];
    int n = enumerate(m, 16), pick = pick_monitor(m, n, monitor);
    if (pick < 0 || width <= 0 || height <= 0) return false;
    const wchar_t *device = m[pick].desc.DeviceName;
    DEVMODEW cur = {};
    cur.dmSize = sizeof cur;
    if (!EnumDisplaySettingsW(device, ENUM_CURRENT_SETTINGS, &cur)) return false;

    // The closest shape wins (within 1%, so 16:10 and 1680x1050 count as one), then the largest
    // that fits the client, else the smallest that doesn't; then the current refresh rate, else
    // the highest.
    double want = log((double)width / height);
    DEVMODEW best = {}, mode = {};
    double best_shape = 1e9;
    mode.dmSize = sizeof mode;
    for (DWORD i = 0; EnumDisplaySettingsW(device, i, &mode); i++) {
        if (mode.dmBitsPerPel != cur.dmBitsPerPel || mode.dmPelsWidth < 640 || mode.dmPelsHeight < 480) continue;
        double shape = fabs(log((double)mode.dmPelsWidth / mode.dmPelsHeight) - want);
        if (shape > best_shape + 0.01) continue;
        bool better;
        if (shape < best_shape - 0.01) {
            better = true;
        } else {
            DWORD area = mode.dmPelsWidth * mode.dmPelsHeight, best_area = best.dmPelsWidth * best.dmPelsHeight;
            bool fits = (int)mode.dmPelsWidth <= width && (int)mode.dmPelsHeight <= height;
            bool best_fits = (int)best.dmPelsWidth <= width && (int)best.dmPelsHeight <= height;
            if (area != best_area || fits != best_fits)
                better = fits != best_fits ? fits : fits ? area > best_area : area < best_area;
            else if (mode.dmDisplayFrequency == cur.dmDisplayFrequency || best.dmDisplayFrequency == cur.dmDisplayFrequency)
                better = mode.dmDisplayFrequency == cur.dmDisplayFrequency;
            else
                better = mode.dmDisplayFrequency > best.dmDisplayFrequency;
        }
        if (better) {
            best = mode;
            if (shape < best_shape) best_shape = shape;
        }
    }
    if (!best.dmPelsWidth) return false;
    if (best.dmPelsWidth == cur.dmPelsWidth && best.dmPelsHeight == cur.dmPelsHeight &&
        best.dmDisplayFrequency == cur.dmDisplayFrequency)
        return false;
    best.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY | DM_BITSPERPEL;
    // Not saved in the registry: capture_restore_mode (or signing out, or a restart) undoes it.
    LONG rc = ChangeDisplaySettingsExW(device, &best, NULL, 0, NULL);
    if (rc != DISP_CHANGE_SUCCESSFUL) {
        host_log("couldn't switch the monitor to %lux%lu (%ld)", best.dmPelsWidth, best.dmPelsHeight, rc);
        return false;
    }
    host_log("switched the monitor from %lux%lu to %lux%lu @ %lu Hz for a %dx%d client", cur.dmPelsWidth,
             cur.dmPelsHeight, best.dmPelsWidth, best.dmPelsHeight, best.dmDisplayFrequency, width, height);
    return true;
}

void capture_restore_mode(void) {
    LONG rc = ChangeDisplaySettingsExW(NULL, NULL, NULL, 0, NULL);
    if (rc == DISP_CHANGE_SUCCESSFUL) host_log("restored the monitor's resolution");
    else host_log("couldn't restore the monitor's resolution (%ld)", rc);
}

Capture *capture_open(int monitor, char *err, size_t err_cap, bool *blocked) {
    if (blocked) *blocked = false;
    Monitor m[16];
    int n = enumerate(m, 16), pick = pick_monitor(m, n, monitor);
    if (pick < 0) {
        snprintf(err, err_cap, n ? "no monitor %d (there are %d)" : "no monitors found", monitor, n);
        return NULL;
    }
    Capture *c = new Capture();
    c->adapter = m[pick].adapter;
    c->output = m[pick].output;
    c->desc = m[pick].desc;
    c->luid = m[pick].adapter_desc.AdapterLuid;
    snprintf(c->adapter_name, sizeof c->adapter_name, "%ls", m[pick].adapter_desc.Description);
    c->width = c->desc.DesktopCoordinates.right - c->desc.DesktopCoordinates.left;
    c->height = c->desc.DesktopCoordinates.bottom - c->desc.DesktopCoordinates.top;
    c->refresh_hz = refresh_hz(c->desc);

    // Capture and encode on the GPU that drives the monitor, so frames never cross the bus.
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    HRESULT hr = D3D11CreateDevice(c->adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, NULL,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels, 2,
                                   D3D11_SDK_VERSION, &c->device, NULL, &c->context);
    if (FAILED(hr)) {
        snprintf(err, err_cap, "can't create a Direct3D 11 device on %s (0x%08lx)", c->adapter_name, hr);
        delete c;
        return NULL;
    }
    // Media Foundation's encoder uses the device from its own threads.
    ComPtr<ID3D11Multithread> mt;
    if (SUCCEEDED(c->device.As(&mt))) mt->SetMultithreadProtected(TRUE);

    hr = duplicate(c);
    if (FAILED(hr)) {
        if (blocked) *blocked = hr == E_ACCESSDENIED;
        snprintf(err, err_cap,
                 hr == E_ACCESSDENIED ? "can't capture the screen right now (lock screen or UAC prompt?)"
                 : hr == DXGI_ERROR_UNSUPPORTED ? "Desktop Duplication isn't supported on %s"
                 : "can't capture the monitor on %s (0x%08lx)",
                 c->adapter_name, hr);
        delete c;
        return NULL;
    }
    DXGI_OUTDUPL_DESC dd;
    c->dup->GetDesc(&dd);
    if (dd.Rotation != DXGI_MODE_ROTATION_IDENTITY && dd.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED)
        host_log("this monitor is rotated; the stream will be sideways (not supported yet)");
    return c;
}

void capture_close(Capture *c) {
    if (!c) return;
    free(c->shape);
    delete c;
}

int capture_width(Capture *c) { return c->width; }
int capture_height(Capture *c) { return c->height; }
int capture_refresh_hz(Capture *c) { return c->refresh_hz; }
const char *capture_adapter_name(Capture *c) { return c->adapter_name; }
RECT capture_monitor_rect(Capture *c) { return c->desc.DesktopCoordinates; }
ID3D11Device *capture_device(Capture *c) { return c->device.Get(); }
LUID capture_adapter_luid(Capture *c) { return c->luid; }
ID3D11Texture2D *capture_frame(Capture *c) { return c->frame.Get(); }

static void copy_frame(Capture *c, IDXGIResource *resource) {
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&tex)))) return;
    D3D11_TEXTURE2D_DESC td;
    tex->GetDesc(&td);
    D3D11_TEXTURE2D_DESC have = {};
    if (c->frame) c->frame->GetDesc(&have);
    if (!c->frame || have.Width != td.Width || have.Height != td.Height || have.Format != td.Format) {
        if (c->frame) host_log("the monitor changed to %ux%u", td.Width, td.Height);
        D3D11_TEXTURE2D_DESC fd = {};
        fd.Width = td.Width;
        fd.Height = td.Height;
        fd.MipLevels = 1;
        fd.ArraySize = 1;
        fd.Format = td.Format;
        fd.SampleDesc.Count = 1;
        fd.Usage = D3D11_USAGE_DEFAULT;
        fd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        c->frame.Reset();
        if (FAILED(c->device->CreateTexture2D(&fd, NULL, &c->frame))) return;
    }
    c->context->CopyResource(c->frame.Get(), tex.Get());
}

bool capture_wait(Capture *c, int timeout_ms) {
    if (!c->dup) {
        // Lost (secure desktop, mode change, fullscreen exclusive app): retry a few times a second.
        ULONGLONG now = GetTickCount64();
        if (now < c->retry_at || FAILED(duplicate(c))) {
            c->retry_at = now + 250;
            Sleep(timeout_ms < 250 ? timeout_ms : 250);
            return false;
        }
        host_log("capturing again");
        c->warned_lost = false;
    }
    DXGI_OUTDUPL_FRAME_INFO info;
    ComPtr<IDXGIResource> resource;
    HRESULT hr = c->dup->AcquireNextFrame(timeout_ms, &info, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false;
    if (FAILED(hr)) {
        if (!c->warned_lost) host_log("lost the screen (0x%08lx); retrying", hr);
        c->warned_lost = true;
        c->dup.Reset();
        c->retry_at = 0;
        return false;
    }
    bool changed = info.LastPresentTime.QuadPart != 0 || !c->frame;
    if (changed) copy_frame(c, resource.Get());
    if (info.PointerShapeBufferSize) {
        if (info.PointerShapeBufferSize > c->shape_cap) {
            free(c->shape);
            c->shape_cap = info.PointerShapeBufferSize;
            c->shape = (uint8_t *)malloc(c->shape_cap);
        }
        if (c->shape && SUCCEEDED(c->dup->GetFramePointerShape(c->shape_cap, c->shape, &c->shape_len, &c->shape_info)))
            c->shape_pending = true;
    }
    c->dup->ReleaseFrame();
    return changed;
}

// MARK: - Cursor

/// Straight-alpha BGRA → PNG with WIC.
static uint8_t *encode_png(const uint8_t *bgra, int w, int h, size_t *len) {
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return NULL;
    ComPtr<IStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(CreateStreamOnHGlobal(NULL, TRUE, &stream)) ||
        FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, NULL, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, NULL)) || FAILED(frame->Initialize(NULL)) ||
        FAILED(frame->SetSize(w, h)))
        return NULL;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetPixelFormat(&format)) || format != GUID_WICPixelFormat32bppBGRA ||
        FAILED(frame->WritePixels(h, w * 4, w * h * 4, (BYTE *)bgra)) || FAILED(frame->Commit()) ||
        FAILED(encoder->Commit()))
        return NULL;
    HGLOBAL mem;
    if (FAILED(GetHGlobalFromStream(stream.Get(), &mem))) return NULL;
    STATSTG stat;
    stream->Stat(&stat, STATFLAG_NONAME);
    *len = (size_t)stat.cbSize.QuadPart;
    uint8_t *out = (uint8_t *)malloc(*len);
    void *src = GlobalLock(mem);
    if (out && src) memcpy(out, src, *len);
    GlobalUnlock(mem);
    return out;
}

/// Pixels that invert the screen (the I-beam, some text cursors) can't be drawn by the client,
/// so they become black with a white outline, which stays visible on any background.
static void outline_inverted(uint8_t *bgra, const bool *inverted, int w, int h) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t *p = bgra + 4 * (y * w + x);
            if (inverted[y * w + x]) {
                p[0] = p[1] = p[2] = 0;
                p[3] = 255;
                continue;
            }
            if (p[3]) continue;
            bool next_to = false;
            for (int dy = -1; dy <= 1 && !next_to; dy++)
                for (int dx = -1; dx <= 1 && !next_to; dx++) {
                    int nx = x + dx, ny = y + dy;
                    next_to = nx >= 0 && ny >= 0 && nx < w && ny < h && inverted[ny * w + nx];
                }
            if (next_to) p[0] = p[1] = p[2] = p[3] = 255;
        }
}

bool capture_cursor_shape(Capture *c, CursorImage *out) {
    if (!c->shape_pending) return false;
    c->shape_pending = false;
    const DXGI_OUTDUPL_POINTER_SHAPE_INFO &si = c->shape_info;
    int w = (int)si.Width, h = (int)si.Height, pitch = (int)si.Pitch;
    if (si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) h /= 2;  // AND mask, then XOR mask
    if (w <= 0 || h <= 0 || w > 256 || h > 256) return false;
    uint8_t *bgra = (uint8_t *)calloc((size_t)w * h, 4);
    bool *inverted = (bool *)calloc((size_t)w * h, sizeof(bool));
    bool any_inverted = false;
    if (!bgra || !inverted) {
        free(bgra);
        free(inverted);
        return false;
    }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t *d = bgra + 4 * (y * w + x);
            if (si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
                int bit = 0x80 >> (x % 8);
                bool and_ = c->shape[y * pitch + x / 8] & bit, xor_ = c->shape[(y + h) * pitch + x / 8] & bit;
                if (!and_) {
                    d[0] = d[1] = d[2] = xor_ ? 255 : 0;
                    d[3] = 255;
                } else if (xor_) {
                    inverted[y * w + x] = any_inverted = true;
                }
            } else {
                const uint8_t *s = c->shape + y * pitch + x * 4;
                if (si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) {
                    memcpy(d, s, 4);
                } else if (s[3] == 0) {     // masked color: alpha 0 replaces the screen...
                    memcpy(d, s, 3);
                    d[3] = 255;
                } else if (s[0] | s[1] | s[2]) {  // ...and alpha 0xFF XORs it
                    inverted[y * w + x] = any_inverted = true;
                }
            }
        }
    if (any_inverted) outline_inverted(bgra, inverted, w, h);
    out->png = encode_png(bgra, w, h, &out->png_len);
    free(bgra);
    free(inverted);
    if (!out->png) return false;
    // This host streams at 1 point per pixel, so the shape's point size is its pixel size.
    out->width = w;
    out->height = h;
    out->hot_x = si.HotSpot.x;
    out->hot_y = si.HotSpot.y;
    return true;
}
