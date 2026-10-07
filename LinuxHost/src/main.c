// iframe-linux-host: accepts iFrame clients, authenticates them by PIN and streams one X11
// monitor to the active one, injecting its mouse and keyboard with XTest.

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <ifaddrs.h>
#include <limits.h>
#include <math.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <X11/Xlib.h>

#include "host.h"
#include "../../Linux/src/net.h"

static struct {
    int port;
    int fps;
    double mbps;            // 0 = auto
    int codec;              // -1 = HEVC if the client can decode it
    int monitor;            // -1 = primary
    int max_inflight;
    char pin[64];
    const char *display;
    enum CmdMode cmd;
    bool publish;
} cfg = { .port = IFRAME_DEFAULT_PORT, .fps = 120, .codec = -1, .monitor = -1, .max_inflight = 3,
          .cmd = CMD_AUTO, .publish = true };

void host_log(const char *fmt, ...) {
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    char stamp[16];
    strftime(stamp, sizeof stamp, "%H:%M:%S", &tm);
    va_list ap;
    va_start(ap, fmt);
    char msg[1024];
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    printf("[%s] %s\n", stamp, msg);
    fflush(stdout);
}

// MARK: - Sessions

typedef struct Session {
    int fd;
    pthread_mutex_t send_lock;
    char name[192];
    bool authenticated;
    bool linux_client;
    Capture *cap;
    Encoder *enc;
    Streamer *streamer;
} Session;

static pthread_mutex_t server_lock = PTHREAD_MUTEX_INITIALIZER;
static Session *active;
static Session *sessions[64];            // every live connection, so shutdown can close them all
static int session_count;
static pthread_cond_t sessions_done = PTHREAD_COND_INITIALIZER;
static int failed_attempts;
static Input *input;                     // shared; only the active session drives it
static pthread_mutex_t input_lock = PTHREAD_MUTEX_INITIALIZER;
static char host_name[128] = "Linux";

static void session_send(void *ctx, uint8_t type, const uint8_t *payload, uint32_t len) {
    Session *s = ctx;
    uint8_t header[5] = { type };
    put_u32(header + 1, len);
    pthread_mutex_lock(&s->send_lock);
    if (net_write_full(s->fd, header, 5) && len) net_write_full(s->fd, payload, len);
    pthread_mutex_unlock(&s->send_lock);
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
    s->cap = capture_open(cfg.display, cfg.monitor, err, sizeof err);
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

    s->enc = encoder_open(codec, w, h, fps, (int)(mbps * 1e6), err, sizeof err);
    if (!s->enc && codec == CODEC_HEVC && cfg.codec < 0) {
        host_log("%s; trying H.264", err);
        codec = CODEC_H264;
        s->enc = encoder_open(codec, w, h, fps, (int)(mbps * 1e6), err, sizeof err);
    }
    if (!s->enc) {
        host_log("could not start stream: %s", err);
        return false;
    }

    int ox, oy;
    capture_monitor_origin(s->cap, &ox, &oy);
    pthread_mutex_lock(&input_lock);
    input_set_region(input, ox, oy, w, h);
    input_set_cmd(input, cfg.cmd != CMD_AUTO ? cfg.cmd : s->linux_client ? CMD_SUPER : CMD_CTRL);
    input_keep_awake(input);
    pthread_mutex_unlock(&input_lock);

    char name[256], json[512];
    json_escape(host_name, name, sizeof name);
    // "os" is an addition the iPad app ignores; the Linux client uses it to map keys 1:1.
    int n = snprintf(json, sizeof json,
                     "{\"width\":%d,\"height\":%d,\"pointWidth\":%d,\"pointHeight\":%d,\"codec\":%d,\"fps\":%d,"
                     "\"hostName\":\"%s\",\"isVirtual\":false,\"os\":\"linux\"}",
                     w, h, w, h, codec, fps, name);
    session_send(s, MSG_WELCOME, (const uint8_t *)json, n);

    StreamConfig sc = {
        .fps = fps,
        .bitrate = (int)(mbps * 1e6),
        .min_bitrate = 2000000,
        .max_bitrate = (int)(mbps * 1.5e6),
        .max_inflight = cfg.max_inflight,
        .codec = codec,
        .local_cursor = local_cursor,
    };
    s->streamer = streamer_start(s->cap, s->enc, sc, session_send, s);
    host_log("streaming %dx%d @ %d fps, %s, start %.0f Mbps, ⌘ → %s, cursor %s", w, h, fps, encoder_name(s->enc), mbps,
             (cfg.cmd != CMD_AUTO ? cfg.cmd : s->linux_client ? CMD_SUPER : CMD_CTRL) == CMD_CTRL ? "Ctrl" : "Super",
             local_cursor ? "drawn by the client" : "in the video");
    return true;
}

static bool handle_hello(Session *s, const char *j, size_t len) {
    double version = 0, max_fps = 0;
    char pin[128] = "", name[96] = "client", os[32] = "";
    bool hevc = false, local_cursor = false;
    json_number(j, len, "version", &version);
    json_string(j, len, "pin", pin, sizeof pin);
    json_string(j, len, "name", name, sizeof name);
    json_string(j, len, "os", os, sizeof os);
    json_bool(j, len, "supportsHEVC", &hevc);
    json_number(j, len, "maxFPS", &max_fps);
    json_bool(j, len, "localCursor", &local_cursor);
    char peer[INET6_ADDRSTRLEN + 8];
    snprintf(peer, sizeof peer, "%.*s", (int)sizeof peer - 1, s->name);  // still just the address
    snprintf(s->name, sizeof s->name, "%s (%s)", name, peer);
    s->linux_client = strcmp(os, "linux") == 0;

    pthread_mutex_lock(&server_lock);
    bool ok = (int)version == IFRAME_PROTOCOL_VERSION && constant_time_equals(pin, cfg.pin);
    int delay_ms = 0;
    if (!ok) {
        failed_attempts++;
        delay_ms = failed_attempts * 500 > 10000 ? 10000 : failed_attempts * 500;  // slows down PIN guessing
    } else {
        failed_attempts = 0;
        if (active && active != s) {
            host_log("closing %s: replaced by %s", active->name, name);
            shutdown(active->fd, SHUT_RDWR);
        }
        active = s;
    }
    pthread_mutex_unlock(&server_lock);

    if (!ok) {
        host_log("rejected %s: wrong PIN or protocol version", s->name);
        usleep(delay_ms * 1000);
        session_send(s, MSG_AUTH_FAILED, NULL, 0);
        return false;
    }
    s->authenticated = true;
    host_log("%s connected", s->name);
    return start_stream(s, hevc, (int)max_fps, local_cursor);
}

static bool is_active(Session *s) {
    pthread_mutex_lock(&server_lock);
    bool yes = active == s;
    pthread_mutex_unlock(&server_lock);
    return yes;
}

static void handle_message(Session *s, uint8_t type, const uint8_t *p, uint32_t len, uint64_t *last_awake) {
    if (type == MSG_ACK) {
        if (len >= 4 && s->streamer) streamer_ack(s->streamer, get_u32(p));
        return;
    }
    if (type == MSG_PING) {
        session_send(s, MSG_PONG, p, len);
        uint64_t now = now_nanos();
        if (now - *last_awake > 30000000000ull && is_active(s)) {
            *last_awake = now;
            pthread_mutex_lock(&input_lock);
            input_keep_awake(input);
            pthread_mutex_unlock(&input_lock);
        }
        return;
    }
    if (type == MSG_REQUEST_KEYFRAME) {
        if (s->streamer) streamer_request_keyframe(s->streamer);
        return;
    }
    if (type == MSG_DISPLAY) {
        host_log("%s asked for a %.*s display; streaming the existing monitor", s->name, (int)len, (const char *)p);
        return;
    }
    if (!is_active(s)) return;
    pthread_mutex_lock(&input_lock);
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
    pthread_mutex_unlock(&input_lock);
}

static void *session_thread(void *arg) {
    Session *s = arg;
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
            body = realloc(body, cap);
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
    pthread_mutex_lock(&server_lock);
    bool was_active = active == s;
    if (was_active) active = NULL;
    pthread_mutex_unlock(&server_lock);
    bool last = false;
    if (was_active) {
        pthread_mutex_lock(&input_lock);
        input_release_all(input);
        pthread_mutex_unlock(&input_lock);
    }
    if (s->authenticated) host_log("%s disconnected", s->name);
    pthread_mutex_lock(&server_lock);
    close(s->fd);
    for (int i = 0; i < session_count; i++)
        if (sessions[i] == s) sessions[i] = sessions[--session_count];
    last = session_count == 0;
    pthread_mutex_unlock(&server_lock);
    if (last) pthread_cond_broadcast(&sessions_done);
    pthread_mutex_destroy(&s->send_lock);
    free(body);
    free(s);
    return NULL;
}

// MARK: - Startup

static void tune(int fd) {
    int one = 1, idle = 5, interval = 2, count = 3, tos = 0x80, priority = 5, sndbuf = 4 << 20;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof interval);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof count);
    setsockopt(fd, IPPROTO_IP, IP_TOS, &tos, sizeof tos);
    setsockopt(fd, IPPROTO_IPV6, IPV6_TCLASS, &tos, sizeof tos);
    setsockopt(fd, SOL_SOCKET, SO_PRIORITY, &priority, sizeof priority);
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof sndbuf);
}

static pid_t publisher = -1;

/// Advertises _iframe._tcp over Bonjour so clients find this machine, via avahi-publish.
static void publish(void) {
    char port[16];
    snprintf(port, sizeof port, "%d", cfg.port);
    publisher = fork();
    if (publisher == 0) {
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) dup2(devnull, STDERR_FILENO);
        execlp("avahi-publish", "avahi-publish", "-s", host_name, "_iframe._tcp", port, "os=linux", (char *)NULL);
        _exit(127);
    }
}

static void load_pin(void) {
    if (cfg.pin[0]) return;
    char path[PATH_MAX];
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (xdg && *xdg) snprintf(path, sizeof path, "%s/iframe/linux-host-pin", xdg);
    else snprintf(path, sizeof path, "%s/.config/iframe/linux-host-pin", home ? home : ".");
    FILE *f = fopen(path, "r");
    if (f) {
        if (fgets(cfg.pin, sizeof cfg.pin, f)) cfg.pin[strcspn(cfg.pin, "\r\n \t")] = 0;
        fclose(f);
    }
    if (!cfg.pin[0]) {
        uint32_t r = 0;
        if (getrandom(&r, sizeof r, 0) != sizeof r) r = (uint32_t)time(NULL) ^ (uint32_t)getpid();
        snprintf(cfg.pin, sizeof cfg.pin, "%06u", r % 1000000);
    }
}

static void print_addresses(void) {
    struct ifaddrs *list, *ifa;
    if (getifaddrs(&list) != 0) return;
    char out[512] = "";
    for (ifa = list; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET || (ifa->ifa_flags & IFF_LOOPBACK)) continue;
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &((struct sockaddr_in *)ifa->ifa_addr)->sin_addr, ip, sizeof ip);
        size_t n = strlen(out);
        snprintf(out + n, sizeof out - n, "%s%s (%s)", n ? ", " : "", ip, ifa->ifa_name);
    }
    freeifaddrs(list);
    printf("  Addresses: %s\n", out[0] ? out : "(none found)");
}

static void usage(FILE *f) {
    fprintf(f,
            "iframe-linux-host — stream this Linux (X11) desktop to iFrame clients\n"
            "\n"
            "  --port <n>        TCP port (default %d)\n"
            "  --fps <n>         max frame rate (default 120; capped by monitor refresh and client)\n"
            "  --mbps <n>        starting bitrate (default: auto from resolution)\n"
            "  --codec <c>       hevc | h264 (default: hevc if the client can decode it)\n"
            "  --monitor <n>     XRandR monitor index (default: the primary monitor)\n"
            "  --display <d>     X display (default $DISPLAY)\n"
            "  --inflight <n>    max unacknowledged frames before skipping (default 3)\n"
            "  --pin <digits>    PIN (default: ~/.config/iframe/linux-host-pin, else random each launch)\n"
            "  --cmd-key <k>     what ⌘ becomes: auto (default: Ctrl for the iPad, Super for the Linux\n"
            "                    client, which sends its own Ctrl as ⌃), ctrl, or super\n"
            "  --no-publish      don't advertise over Bonjour\n",
            IFRAME_DEFAULT_PORT);
}

static volatile sig_atomic_t quitting;
static void on_signal(int sig) { (void)sig; quitting = 1; }

int main(int argc, char **argv) {
    enum { O_PORT = 1000, O_FPS, O_MBPS, O_CODEC, O_MONITOR, O_DISPLAY, O_INFLIGHT, O_PIN, O_CMD, O_NOPUB };
    static const struct option longopts[] = {
        {"port", 1, 0, O_PORT}, {"fps", 1, 0, O_FPS}, {"mbps", 1, 0, O_MBPS}, {"codec", 1, 0, O_CODEC},
        {"monitor", 1, 0, O_MONITOR}, {"display", 1, 0, O_DISPLAY}, {"inflight", 1, 0, O_INFLIGHT},
        {"pin", 1, 0, O_PIN}, {"cmd-key", 1, 0, O_CMD}, {"no-publish", 0, 0, O_NOPUB}, {"help", 0, 0, 'h'},
        {0, 0, 0, 0},
    };
    for (int c; (c = getopt_long(argc, argv, "h", longopts, NULL)) != -1;) {
        switch (c) {
        case O_PORT: cfg.port = atoi(optarg); break;
        case O_FPS: cfg.fps = atoi(optarg) > 0 ? atoi(optarg) : 1; break;
        case O_MBPS: cfg.mbps = atof(optarg); break;
        case O_CODEC:
            if (!strcasecmp(optarg, "hevc") || !strcasecmp(optarg, "h265")) cfg.codec = CODEC_HEVC;
            else if (!strcasecmp(optarg, "h264") || !strcasecmp(optarg, "avc")) cfg.codec = CODEC_H264;
            break;
        case O_MONITOR: cfg.monitor = atoi(optarg); break;
        case O_DISPLAY: cfg.display = optarg; break;
        case O_INFLIGHT: cfg.max_inflight = atoi(optarg) > 0 ? atoi(optarg) : 1; break;
        case O_PIN: snprintf(cfg.pin, sizeof cfg.pin, "%s", optarg); break;
        case O_CMD:
            cfg.cmd = !strcmp(optarg, "ctrl") ? CMD_CTRL : !strcmp(optarg, "super") ? CMD_SUPER : CMD_AUTO;
            break;
        case O_NOPUB: cfg.publish = false; break;
        case 'h': usage(stdout); return 0;
        default: usage(stderr); return 64;
        }
    }
    load_pin();
    gethostname(host_name, sizeof host_name - 1);
    XInitThreads();

    // Fail early (and visibly) if the display or encoder can't work.
    char err[256];
    Capture *probe = capture_open(cfg.display, cfg.monitor, err, sizeof err);
    if (!probe) {
        host_log("%s", err);
        return 1;
    }
    capture_close(probe);
    input = input_open(cfg.display);

    int ls = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0), one = 1, zero = 0;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(ls, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);
    struct sockaddr_in6 addr = { .sin6_family = AF_INET6, .sin6_port = htons(cfg.port), .sin6_addr = in6addr_any };
    if (bind(ls, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(ls, 8) < 0) {
        host_log("can't listen on port %d: %s", cfg.port, strerror(errno));
        return 1;
    }
    struct sigaction sa = { .sa_handler = on_signal };  // no SA_RESTART: accept() returns EINTR
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    if (cfg.publish) publish();

    printf("\n  iframe-linux-host ready on port %d\n  PIN: %s\n", cfg.port, cfg.pin);
    print_addresses();
    printf("  Clients find this machine via Bonjour as \"%s\".\n\n", host_name);
    fflush(stdout);

    while (!quitting) {
        struct sockaddr_storage peer;
        socklen_t plen = sizeof peer;
        int fd = accept4(ls, (struct sockaddr *)&peer, &plen, SOCK_CLOEXEC);
        if (fd < 0) continue;
        tune(fd);
        Session *s = calloc(1, sizeof *s);
        s->fd = fd;
        pthread_mutex_init(&s->send_lock, NULL);
        char ip[INET6_ADDRSTRLEN] = "?";
        if (peer.ss_family == AF_INET6) {
            struct sockaddr_in6 *a = (struct sockaddr_in6 *)&peer;
            if (IN6_IS_ADDR_V4MAPPED(&a->sin6_addr)) inet_ntop(AF_INET, &a->sin6_addr.s6_addr[12], ip, sizeof ip);
            else inet_ntop(AF_INET6, &a->sin6_addr, ip, sizeof ip);
        }
        snprintf(s->name, sizeof s->name, "%s", ip);
        pthread_mutex_lock(&server_lock);
        bool room = session_count < (int)(sizeof sessions / sizeof *sessions);
        if (room) sessions[session_count++] = s;
        pthread_mutex_unlock(&server_lock);
        if (!room) {
            close(fd);
            pthread_mutex_destroy(&s->send_lock);
            free(s);
            continue;
        }
        pthread_t t;
        pthread_create(&t, NULL, session_thread, s);
        pthread_detach(t);
    }

    host_log("shutting down");
    close(ls);
    if (publisher > 0) kill(publisher, SIGTERM);
    // Let every session stop its stream and free the encoder before exiting: CUDA's teardown
    // deadlocks if it runs while another thread is still closing an NVENC session.
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 3;
    pthread_mutex_lock(&server_lock);
    for (int i = 0; i < session_count; i++) shutdown(sessions[i]->fd, SHUT_RDWR);
    while (session_count > 0 && pthread_cond_timedwait(&sessions_done, &server_lock, &deadline) == 0) {}
    pthread_mutex_unlock(&server_lock);
    pthread_mutex_lock(&input_lock);
    input_release_all(input);
    pthread_mutex_unlock(&input_lock);
    // Skip library destructors (CUDA's can hang at exit); everything that matters is released.
    _exit(0);
}
