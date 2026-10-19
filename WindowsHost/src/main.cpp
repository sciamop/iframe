// iframe-windows-host: accepts iFrame clients, authenticates them by PIN and streams one monitor
// to the active one.

#include "host.h"  // first: winsock2.h has to come before the networking headers

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <iphlpapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windns.h>
#include <ws2tcpip.h>

static struct {
    int port = IFRAME_DEFAULT_PORT;
    int fps = 120;
    double mbps = 0;             // 0 = auto
    int codec = -1;              // -1 = HEVC if the client can decode it
    int monitor = -1;            // -1 = primary
    int max_inflight = 3;
    char pin[64] = "";
    enum CmdMode cmd = CMD_AUTO;
    bool publish = true;
    bool match_resolution = true;    // switch the monitor to the client's shape while it's connected
    wchar_t pin_file[MAX_PATH] = L"";
    wchar_t log_file[MAX_PATH] = L"";
    wchar_t quit_event[128] = L"";   // set by the service to stop us
} cfg;

static SRWLOCK log_lock = SRWLOCK_INIT;

void host_log(const char *fmt, ...) {
    time_t t = time(NULL);
    struct tm tm;
    localtime_s(&tm, &t);
    char stamp[16];
    strftime(stamp, sizeof stamp, "%H:%M:%S", &tm);
    va_list ap;
    va_start(ap, fmt);
    char msg[1024];
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    AcquireSRWLockExclusive(&log_lock);
    printf("[%s] %s\n", stamp, msg);
    fflush(stdout);
    ReleaseSRWLockExclusive(&log_lock);
}

// MARK: - Sessions

typedef struct Session {
    SOCKET fd;
    SRWLOCK send_lock;
    char name[192];
    bool authenticated;
    bool pc_client;              // Linux or Windows: its Ctrl, Alt and Super/Windows keys map one-to-one
    Capture *cap;
    Encoder *enc;
    Streamer *streamer;
} Session;

static SRWLOCK server_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE sessions_done = CONDITION_VARIABLE_INIT;
static Session *active;
static Session *sessions[64];            // every live connection, so shutdown can close them all
static int session_count;
static int failed_attempts;
static char host_name[128] = "Windows";
static Input *input;                     // shared; only the active session drives it
static SRWLOCK input_lock = SRWLOCK_INIT;
static SRWLOCK mode_lock = SRWLOCK_INIT;
static Session *mode_owner;              // the session the monitor's resolution was switched for

/// Shapes the monitor like the client's screen (width x height pixels). uiScale <= 0 means the
/// client wants this display as it is.
static void match_mode(Session *s, int width, int height, double ui_scale) {
    AcquireSRWLockExclusive(&mode_lock);
    desktop_follow_input();
    if (cfg.match_resolution && ui_scale > 0 && width > 0 && height > 0) {
        // A client replacing another takes over restoring the resolution, even when it's the same.
        if (capture_match_mode(cfg.monitor, width, height) || mode_owner) mode_owner = s;
    } else if (mode_owner) {
        capture_restore_mode();
        mode_owner = NULL;
    }
    ReleaseSRWLockExclusive(&mode_lock);
}

static void restore_mode(Session *s) {
    AcquireSRWLockExclusive(&mode_lock);
    if (mode_owner == s) {
        desktop_follow_input();
        capture_restore_mode();
        mode_owner = NULL;
    }
    ReleaseSRWLockExclusive(&mode_lock);
}

static void session_send(void *ctx, uint8_t type, const uint8_t *payload, uint32_t len) {
    Session *s = (Session *)ctx;
    uint8_t header[5] = { type };
    put_u32(header + 1, len);
    AcquireSRWLockExclusive(&s->send_lock);
    if (net_write_full(s->fd, header, 5) && len) net_write_full(s->fd, payload, len);
    ReleaseSRWLockExclusive(&s->send_lock);
}

static bool constant_time_equals(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    if (la != lb) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < la; i++) diff |= (unsigned char)a[i] ^ (unsigned char)b[i];
    return diff == 0;
}

static bool start_stream(Session *s, bool supports_hevc, int max_fps, bool local_cursor) {
    char err[256];
    // Capture can be blocked for a moment, e.g. right after a resolution change.
    bool blocked = false;
    for (int tries = 0; tries < 10; tries++) {
        s->cap = capture_open(cfg.monitor, err, sizeof err, &blocked);
        if (s->cap || !blocked) break;
        Sleep(300);
    }
    if (!s->cap) {
        host_log("could not start stream: %s", err);
        return false;
    }
    int w = capture_width(s->cap), h = capture_height(s->cap);
    int fps = cfg.fps;
    if (max_fps > 0 && max_fps < fps) fps = max_fps;
    if (capture_refresh_hz(s->cap) < fps) fps = capture_refresh_hz(s->cap);
    if (fps < 24) fps = 24;
    int codec = cfg.codec >= 0 ? cfg.codec : supports_hevc ? CODEC_HEVC : CODEC_H264;

    // ~0.1 bits/pixel at 60 fps for HEVC (more for H.264), sublinear in frame rate; the Mac's formula.
    double bpp = (codec == CODEC_HEVC ? 0.1 : 0.15) * sqrt(60.0 / fps);
    double auto_mbps = (double)w * h * fps * bpp / 1e6;
    double mbps = cfg.mbps > 0 ? cfg.mbps : fmin(fmax(auto_mbps, 8), 120);

    s->enc = encoder_open(capture_device(s->cap), capture_adapter_luid(s->cap), codec, w, h, fps, (int)(mbps * 1e6),
                          err, sizeof err);
    if (!s->enc && codec == CODEC_HEVC && cfg.codec < 0) {
        host_log("%s; trying H.264", err);
        codec = CODEC_H264;
        s->enc = encoder_open(capture_device(s->cap), capture_adapter_luid(s->cap), codec, w, h, fps,
                              (int)(mbps * 1e6), err, sizeof err);
    }
    if (!s->enc) {
        host_log("could not start stream: %s", err);
        return false;
    }

    enum CmdMode cmd = cfg.cmd != CMD_AUTO ? cfg.cmd : s->pc_client ? CMD_WIN : CMD_CTRL;
    AcquireSRWLockExclusive(&input_lock);
    desktop_follow_input();
    input_set_region(input, capture_monitor_rect(s->cap));
    input_set_cmd(input, cmd);
    input_keep_awake(input);
    ReleaseSRWLockExclusive(&input_lock);

    char name[256], json[512];
    json_escape(host_name, name, sizeof name);
    // "os" is an addition the iPad and Mac apps ignore; the Windows and Linux clients use it to map keys 1:1.
    int n = snprintf(json, sizeof json,
                     "{\"width\":%d,\"height\":%d,\"pointWidth\":%d,\"pointHeight\":%d,\"codec\":%d,\"fps\":%d,"
                     "\"hostName\":\"%s\",\"isVirtual\":false,\"os\":\"windows\"}",
                     w, h, w, h, codec, fps, name);
    session_send(s, MSG_WELCOME, (const uint8_t *)json, n);

    StreamConfig sc = {};
    sc.fps = fps;
    sc.bitrate = (int)(mbps * 1e6);
    sc.min_bitrate = 2000000;
    sc.max_bitrate = (int)(mbps * 1.5e6);
    sc.max_inflight = cfg.max_inflight;
    sc.codec = codec;
    sc.local_cursor = local_cursor;
    s->streamer = streamer_start(s->cap, s->enc, sc, session_send, s);
    host_log("streaming %dx%d @ %d fps on %s, %s, start %.0f Mbps, ⌘ → %s, cursor %s", w, h, fps,
             capture_adapter_name(s->cap), encoder_name(s->enc), mbps, cmd == CMD_WIN ? "Windows key" : "Ctrl",
             local_cursor ? "drawn by the client" : "not shown (this client can't draw it)");
    return true;
}

static bool handle_hello(Session *s, const char *j, size_t len) {
    double version = 0, max_fps = 0, width = 0, height = 0, ui_scale = 0;
    char pin[128] = "", name[96] = "client", os[32] = "";
    bool hevc = false, local_cursor = false;
    json_number(j, len, "version", &version);
    json_string(j, len, "pin", pin, sizeof pin);
    json_string(j, len, "name", name, sizeof name);
    json_string(j, len, "os", os, sizeof os);
    json_bool(j, len, "supportsHEVC", &hevc);
    json_number(j, len, "maxFPS", &max_fps);
    json_bool(j, len, "localCursor", &local_cursor);
    // The client's screen, in "display" (the only object in a hello, so its keys are unique).
    json_number(j, len, "width", &width);
    json_number(j, len, "height", &height);
    json_number(j, len, "uiScale", &ui_scale);
    char peer[64];
    snprintf(peer, sizeof peer, "%s", s->name);  // still just the address
    snprintf(s->name, sizeof s->name, "%s (%s)", name, peer);
    s->pc_client = strcmp(os, "linux") == 0 || strcmp(os, "windows") == 0;

    AcquireSRWLockExclusive(&server_lock);
    bool ok = (int)version == IFRAME_PROTOCOL_VERSION && constant_time_equals(pin, cfg.pin);
    int delay_ms = 0;
    if (!ok) {
        failed_attempts++;
        delay_ms = failed_attempts * 500 > 10000 ? 10000 : failed_attempts * 500;  // slows down PIN guessing
    } else {
        failed_attempts = 0;
        if (active && active != s) {
            host_log("closing %s: replaced by %s", active->name, name);
            shutdown(active->fd, SD_BOTH);
        }
        active = s;
    }
    ReleaseSRWLockExclusive(&server_lock);

    if (!ok) {
        host_log("rejected %s: wrong PIN or protocol version", s->name);
        Sleep(delay_ms);
        session_send(s, MSG_AUTH_FAILED, NULL, 0);
        return false;
    }
    s->authenticated = true;
    host_log("%s connected", s->name);
    match_mode(s, (int)width, (int)height, ui_scale);
    return start_stream(s, hevc, (int)max_fps, local_cursor);
}

static bool is_active(Session *s) {
    AcquireSRWLockShared(&server_lock);
    bool yes = active == s;
    ReleaseSRWLockShared(&server_lock);
    return yes;
}

static void handle_message(Session *s, uint8_t type, const uint8_t *p, uint32_t len, uint64_t *last_awake) {
    if (type == MSG_ACK) {
        if (len >= 4 && s->streamer) streamer_ack(s->streamer, get_u32(p));
        return;
    }
    if (type == MSG_REQUEST_KEYFRAME) {
        if (s->streamer) streamer_request_keyframe(s->streamer);
        return;
    }
    if (type == MSG_PING) {
        session_send(s, MSG_PONG, p, len);
        uint64_t now = now_nanos();
        if (now - *last_awake > 30000000000ull && is_active(s)) {
            *last_awake = now;
            AcquireSRWLockExclusive(&input_lock);
            input_keep_awake(input);
            ReleaseSRWLockExclusive(&input_lock);
        }
        return;
    }
    if (type == MSG_DISPLAY) {
        // Clients only reshape virtual displays, and this host says it has none.
        host_log("%s asked for a %.*s display; keeping the current resolution", s->name, (int)len, (const char *)p);
        return;
    }
    if (!is_active(s)) return;
    AcquireSRWLockExclusive(&input_lock);
    desktop_follow_input();
    switch (type) {
    case MSG_MOUSE_MOVE:
        if (len >= 8) input_move(input, get_f32(p), get_f32(p + 4));
        break;
    case MSG_MOUSE_BUTTON:
        if (len >= 10) input_button(input, p[0], p[1] != 0, get_f32(p + 2), get_f32(p + 6));
        break;
    case MSG_SCROLL:
        if (len >= 8) input_scroll(input, get_f32(p), get_f32(p + 4));
        break;
    case MSG_KEY:
        if (len >= 7) input_key(input, p[0] << 8 | p[1], p[2], get_u32(p + 3));
        break;
    case MSG_TEXT:
        input_text(input, (const char *)p, len);
        break;
    default:
        break;
    }
    ReleaseSRWLockExclusive(&input_lock);
}

static DWORD WINAPI session_thread(void *arg) {
    Session *s = (Session *)arg;
    uint8_t *body = NULL;
    size_t cap = 0;
    uint64_t last_awake = now_nanos();
    for (;;) {
        uint8_t header[5];
        if (!net_read_full(s->fd, header, 5)) break;
        uint32_t len = get_u32(header + 1);
        if (len > (1u << 20)) break;  // clients only send small messages
        if (len + 1 > cap) {
            cap = len + 1;
            body = (uint8_t *)realloc(body, cap);
        }
        if (len && !net_read_full(s->fd, body, len)) break;
        body[len] = 0;
        if (!s->authenticated) {
            if (header[0] != MSG_HELLO || !handle_hello(s, (const char *)body, len)) break;
            continue;
        }
        handle_message(s, header[0], body, len, &last_awake);
    }

    streamer_stop(s->streamer);
    encoder_close(s->enc);
    capture_close(s->cap);
    if (s->authenticated) host_log("%s disconnected", s->name);
    AcquireSRWLockExclusive(&server_lock);
    bool was_active = active == s;
    if (was_active) active = NULL;
    ReleaseSRWLockExclusive(&server_lock);
    if (was_active) {
        AcquireSRWLockExclusive(&input_lock);
        input_release_all(input);
        ReleaseSRWLockExclusive(&input_lock);
    }
    restore_mode(s);
    AcquireSRWLockExclusive(&server_lock);
    closesocket(s->fd);
    for (int i = 0; i < session_count; i++)
        if (sessions[i] == s) sessions[i] = sessions[--session_count];
    bool last = session_count == 0;
    ReleaseSRWLockExclusive(&server_lock);
    if (last) WakeAllConditionVariable(&sessions_done);
    free(body);
    free(s);
    return 0;
}

// MARK: - Startup

static void tune(SOCKET fd) {
    int one = 1, idle = 5, interval = 2, count = 3, sndbuf = 4 << 20;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (const char *)&one, sizeof one);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, (const char *)&idle, sizeof idle);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, (const char *)&interval, sizeof interval);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, (const char *)&count, sizeof count);
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (const char *)&sndbuf, sizeof sndbuf);
}

// Advertises _iframe._tcp over Bonjour through Windows' own mDNS responder (Windows 10 1809+).
static DNS_SERVICE_REGISTER_REQUEST registration;

static void WINAPI on_registered(DWORD status, void *ctx, DNS_SERVICE_INSTANCE *instance) {
    if (status != ERROR_SUCCESS) host_log("Bonjour registration failed (%lu); clients can still connect by address", status);
    if (instance) DnsServiceFreeInstance(instance);
}

static void publish(void) {
    wchar_t service[256], target[160], wname[128];
    MultiByteToWideChar(CP_UTF8, 0, host_name, -1, wname, 128);
    swprintf(service, 256, L"%ls._iframe._tcp.local", wname);
    swprintf(target, 160, L"%ls.local", wname);
    const wchar_t *keys[] = { L"os" }, *values[] = { L"windows" };
    DNS_SERVICE_INSTANCE *instance = DnsServiceConstructInstance(service, target, NULL, NULL, (WORD)cfg.port, 0, 0, 1,
                                                                 (PCWSTR *)keys, (PCWSTR *)values);
    registration.Version = DNS_QUERY_REQUEST_VERSION1;
    registration.pServiceInstance = instance;
    registration.pRegisterCompletionCallback = on_registered;
    DWORD rc = DnsServiceRegister(&registration, NULL);
    if (rc != DNS_REQUEST_PENDING) host_log("Bonjour unavailable (%lu); clients can still connect by address", rc);
}

static void unpublish(void) {
    if (!registration.pServiceInstance) return;
    registration.pRegisterCompletionCallback = NULL;
    DnsServiceDeRegister(&registration, NULL);
    DnsServiceFreeInstance(registration.pServiceInstance);
}

/// --pin-file, else %APPDATA%\iframe\windows-host-pin.
static void pin_path(wchar_t *out, size_t cap) {
    wchar_t *appdata = NULL;
    if (cfg.pin_file[0]) {
        swprintf(out, cap, L"%ls", cfg.pin_file);
    } else if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, NULL, &appdata))) {
        swprintf(out, cap, L"%ls\\iframe\\windows-host-pin", appdata);
        CoTaskMemFree(appdata);
    } else {
        swprintf(out, cap, L"windows-host-pin");
    }
}

/// iframe-windows-hostw.exe (the login task, or the service's host) and the service have no
/// console: their output goes to --log, else %LOCALAPPDATA%\iframe\windows-host.log. It keeps one
/// older copy past 4 MB.
static void log_to_file_without_console(void) {
    if (!cfg.log_file[0] && GetStdHandle(STD_OUTPUT_HANDLE)) return;  // a console, or redirected by whoever started us
    wchar_t path[MAX_PATH], dir[MAX_PATH], old[MAX_PATH];
    wchar_t *local = NULL;
    if (cfg.log_file[0]) {
        swprintf(path, MAX_PATH, L"%ls", cfg.log_file);
    } else if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, NULL, &local))) {
        swprintf(path, MAX_PATH, L"%ls\\iframe\\windows-host.log", local);
        CoTaskMemFree(local);
    } else {
        return;
    }
    swprintf(dir, MAX_PATH, L"%ls", path);
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (slash) *slash = 0;
    CreateDirectoryW(dir, NULL);
    swprintf(old, MAX_PATH, L"%.*ls.old.log", (int)(wcslen(path) > 4 ? wcslen(path) - 4 : wcslen(path)), path);
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &info) && (info.nFileSizeHigh || info.nFileSizeLow > (4u << 20)))
        MoveFileExW(path, old, MOVEFILE_REPLACE_EXISTING);
    FILE *f = _wfreopen(path, L"a", stdout);
    if (f) _wfreopen(path, L"a", stderr);
}

static void load_pin(void) {
    if (cfg.pin[0]) return;
    wchar_t path[MAX_PATH];
    pin_path(path, MAX_PATH);
    FILE *f = _wfopen(path, L"r");
    if (f) {
        if (fgets(cfg.pin, sizeof cfg.pin, f)) cfg.pin[strcspn(cfg.pin, "\r\n \t")] = 0;
        fclose(f);
    }
    if (!cfg.pin[0]) {
        unsigned r = 0;
        rand_s(&r);
        snprintf(cfg.pin, sizeof cfg.pin, "%06u", r % 1000000);
    }
}

static void print_addresses(void) {
    ULONG size = 16 << 10;
    IP_ADAPTER_ADDRESSES *list = (IP_ADAPTER_ADDRESSES *)malloc(size);
    char out[512] = "";
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, NULL,
                             list, &size) == NO_ERROR) {
        for (IP_ADAPTER_ADDRESSES *a = list; a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u; u = u->Next) {
                char ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &((sockaddr_in *)u->Address.lpSockaddr)->sin_addr, ip, sizeof ip);
                size_t n = strlen(out);
                snprintf(out + n, sizeof out - n, "%s%s (%ls)", n ? ", " : "", ip, a->FriendlyName);
            }
        }
    }
    free(list);
    printf("  Addresses: %s\n", out[0] ? out : "(none found)");
}

static void usage(FILE *f) {
    wchar_t path[MAX_PATH];
    pin_path(path, MAX_PATH);
    fprintf(f,
            "iframe-windows-host — stream this Windows desktop to iFrame clients\n"
            "\n"
            "  --port <n>        TCP port (default %d)\n"
            "  --fps <n>         max frame rate (default 120; capped by monitor refresh and client)\n"
            "  --mbps <n>        starting bitrate (default: auto from resolution)\n"
            "  --codec <c>       hevc | h264 (default: hevc if the client can decode it)\n"
            "  --monitor <n>     monitor index from --list (default: the primary monitor)\n"
            "  --list            list monitors and the GPU driving each, then exit\n"
            "  --inflight <n>    max unacknowledged frames before skipping (default 3)\n"
            "  --pin <digits>    PIN (default: the one in --pin-file, else random each launch)\n"
            "  --pin-file <path> where the PIN is kept (default %ls)\n"
            "  --cmd-key <k>     what ⌘ becomes: auto (default: Ctrl for the iPad and Mac, the Windows key\n"
            "                    for Windows and Linux clients, which send their own Ctrl as ⌃), ctrl, or win\n"
            "  --no-publish      don't advertise over Bonjour\n"
            "  --keep-resolution don't switch the monitor to the client's screen shape\n"
            "  --log <path>      log here when there's no console (default %%LOCALAPPDATA%%\\iframe\\windows-host.log)\n"
            "  --service         run as the Windows service install.ps1 sets up: it starts the host as\n"
            "                    SYSTEM, so it can show and control the lock and sign-in screens\n",
            IFRAME_DEFAULT_PORT, path);
}

static volatile LONG quitting;
static SOCKET listener = INVALID_SOCKET;

static void quit(void) {
    InterlockedExchange(&quitting, 1);
    closesocket(listener);  // wakes accept()
}

static BOOL WINAPI on_console_event(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT || type == CTRL_SHUTDOWN_EVENT) {
        quit();
        return TRUE;
    }
    return FALSE;
}

static DWORD WINAPI wait_for_quit_event(void *event) {
    WaitForSingleObject((HANDLE)event, INFINITE);
    host_log("the service asked the host to quit");
    quit();
    return 0;
}

int main(int argc, char **argv) {
    SetConsoleOutputCP(CP_UTF8);
    // Physical pixels everywhere: capture, input and the monitor list.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Paths come from the wide command line, so any characters survive.
    int wargc = 0;
    wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (!wargv || wargc != argc) return 70;
    bool service = false;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        bool has = v != NULL;
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(stdout); return 0; }
        else if (!strcmp(a, "--list")) { capture_list_monitors(); return 0; }
        else if (!strcmp(a, "--no-publish")) cfg.publish = false;
        else if (!strcmp(a, "--keep-resolution")) cfg.match_resolution = false;
        else if (!strcmp(a, "--service")) service = true;
        else if (!strcmp(a, "--pin-file") && has) swprintf(cfg.pin_file, MAX_PATH, L"%ls", wargv[++i]);
        else if (!strcmp(a, "--log") && has) swprintf(cfg.log_file, MAX_PATH, L"%ls", wargv[++i]);
        else if (!strcmp(a, "--quit-event") && has) swprintf(cfg.quit_event, 128, L"%ls", wargv[++i]);
        else if (!strcmp(a, "--port") && has) cfg.port = atoi(argv[++i]);
        else if (!strcmp(a, "--fps") && has) cfg.fps = atoi(argv[++i]) > 0 ? atoi(argv[i]) : 1;
        else if (!strcmp(a, "--mbps") && has) cfg.mbps = atof(argv[++i]);
        else if (!strcmp(a, "--codec") && has) {
            v = argv[++i];
            if (!_stricmp(v, "hevc") || !_stricmp(v, "h265")) cfg.codec = CODEC_HEVC;
            else if (!_stricmp(v, "h264") || !_stricmp(v, "avc")) cfg.codec = CODEC_H264;
        }
        else if (!strcmp(a, "--monitor") && has) cfg.monitor = atoi(argv[++i]);
        else if (!strcmp(a, "--inflight") && has) cfg.max_inflight = atoi(argv[++i]) > 0 ? atoi(argv[i]) : 1;
        else if (!strcmp(a, "--pin") && has) snprintf(cfg.pin, sizeof cfg.pin, "%s", argv[++i]);
        else if (!strcmp(a, "--cmd-key") && has) {
            v = argv[++i];
            cfg.cmd = !strcmp(v, "ctrl") ? CMD_CTRL : !strcmp(v, "win") || !strcmp(v, "super") ? CMD_WIN : CMD_AUTO;
        }
        else { usage(stderr); return 64; }
    }
    log_to_file_without_console();
    if (service) {
        // The host it starts gets the same options.
        static wchar_t args[3072];
        size_t n = 0;
        for (int i = 1; i < wargc && n < 2900; i++)
            if (wcscmp(wargv[i], L"--service"))
                n += swprintf(args + n, 3072 - n, wcschr(wargv[i], L' ') ? L" \"%ls\"" : L" %ls", wargv[i]);
        return service_run(args);
    }
    if (cfg.quit_event[0]) {
        HANDLE event = OpenEventW(SYNCHRONIZE, FALSE, cfg.quit_event);
        if (event) CloseHandle(CreateThread(NULL, 0, wait_for_quit_event, event, 0, NULL));
    }
    load_pin();
    DWORD name_len = sizeof host_name;
    GetComputerNameExA(ComputerNameDnsHostname, host_name, &name_len);

    // Fail early (and visibly) if the monitor can't be found. A locked desktop is only temporary
    // (at login it often is for a moment), so wait for it rather than exit.
    char err[256];
    bool blocked;
    for (bool logged = false;; logged = true) {
        Capture *probe = capture_open(cfg.monitor, err, sizeof err, &blocked);
        if (probe) { capture_close(probe); break; }
        if (!blocked) { host_log("%s", err); return 1; }
        if (!logged) host_log("%s; waiting for it", err);
        Sleep(2000);
    }
    input = input_open();

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    listener = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    int zero = 0;
    setsockopt(listener, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&zero, sizeof zero);
    sockaddr_in6 addr = {};
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons((u_short)cfg.port);
    addr.sin6_addr = in6addr_any;
    if (bind(listener, (sockaddr *)&addr, sizeof addr) != 0 || listen(listener, 8) != 0) {
        host_log("can't listen on port %d (error %d)", cfg.port, WSAGetLastError());
        return 1;
    }
    SetConsoleCtrlHandler(on_console_event, TRUE);
    if (cfg.publish) publish();

    printf("\n  iframe-windows-host ready on port %d\n  PIN: %s\n", cfg.port, cfg.pin);
    print_addresses();
    printf("  Clients find this machine via Bonjour as \"%s\".\n\n", host_name);
    fflush(stdout);

    while (!quitting) {
        sockaddr_storage peer;
        int plen = sizeof peer;
        SOCKET fd = accept(listener, (sockaddr *)&peer, &plen);
        if (fd == INVALID_SOCKET) continue;
        tune(fd);
        Session *s = (Session *)calloc(1, sizeof *s);
        s->fd = fd;
        InitializeSRWLock(&s->send_lock);
        char ip[INET6_ADDRSTRLEN] = "?";
        if (peer.ss_family == AF_INET6) {
            sockaddr_in6 *a = (sockaddr_in6 *)&peer;
            if (IN6_IS_ADDR_V4MAPPED(&a->sin6_addr)) inet_ntop(AF_INET, &a->sin6_addr.s6_addr[12], ip, sizeof ip);
            else inet_ntop(AF_INET6, &a->sin6_addr, ip, sizeof ip);
        }
        snprintf(s->name, sizeof s->name, "%s", ip);
        AcquireSRWLockExclusive(&server_lock);
        bool room = session_count < (int)(sizeof sessions / sizeof *sessions);
        if (room) sessions[session_count++] = s;
        ReleaseSRWLockExclusive(&server_lock);
        HANDLE t = room ? CreateThread(NULL, 0, session_thread, s, 0, NULL) : NULL;
        if (!t) {
            AcquireSRWLockExclusive(&server_lock);
            for (int i = 0; i < session_count; i++)
                if (sessions[i] == s) sessions[i] = sessions[--session_count];
            ReleaseSRWLockExclusive(&server_lock);
            closesocket(fd);
            free(s);
            continue;
        }
        CloseHandle(t);
    }

    host_log("shutting down");
    unpublish();
    // Let every session stop its stream before exiting.
    AcquireSRWLockExclusive(&server_lock);
    for (int i = 0; i < session_count; i++) shutdown(sessions[i]->fd, SD_BOTH);
    ULONGLONG deadline = GetTickCount64() + 3000;
    while (session_count > 0 && GetTickCount64() < deadline)
        SleepConditionVariableSRW(&sessions_done, &server_lock, (DWORD)(deadline - GetTickCount64()), 0);
    ReleaseSRWLockExclusive(&server_lock);
    AcquireSRWLockExclusive(&input_lock);
    input_release_all(input);
    ReleaseSRWLockExclusive(&input_lock);
    return 0;
}
