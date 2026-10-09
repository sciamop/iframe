# iframe-linux-host

Streams a Linux (X11) desktop to iFrame clients: the iPad app, the Linux client and the Windows client. It's a
**separate host** from the Mac's `iframe-host` (`Host/`, Swift). They share only the wire protocol,
so either kind of client connects to either kind of host. The C protocol helpers (framing, JSON,
key table) come from `Linux/src/`.

```
X11 (XShm grab, XDamage, XFixes cursor) ─┐                ┌─ iPad app / Linux client
NVENC HEVC/H.264 (CUDA upload, BGRX in) ├──TCP, PIN──────►│  (unchanged)
ack flow control, idle refine, ABR      ┘◄───────────────┤  mouse / keys / text
XTest input injection                                     └─
```

The design matches the Mac host:

- **Local cursor.** Clients that ask for it (all current ones do) get the XFixes cursor shape as a
  PNG whenever it changes, and the pointer is left out of the video. They draw it at their own
  pointer position, so it moves with no network delay and moving it costs no frames. Older
  clients get the cursor composited into the video instead.
- **Only changed frames are sent.** XDamage decides when to capture (plus the cursor shape and
  pointer motion when the cursor is in the video), so a static screen sends nothing.
- **One frame at a time** goes through capture, encode and send.
- **At most 3 frames unacknowledged.** When the link backs up, frames are skipped before
  encoding, and the newest screen goes out as soon as an ack arrives.
- **Idle refinement:** when the screen settles, the last frame is re-encoded twice (after
  150 ms, then 400 ms) so text sharpens.
- **Adaptive bitrate:** −25% when frames drop, +10% after 3 clean seconds.
- **Keyframes only on demand.**

## Requirements

- An X11 session (Cinnamon, MATE, Xfce, GNOME on Xorg…). Wayland isn't supported yet.
- An NVIDIA GPU, for NVENC.
- `sudo apt install libavcodec-dev libavutil-dev libx11-dev libxext-dev libxdamage-dev libxfixes-dev libxrandr-dev libxtst-dev avahi-utils`

## Install

```sh
LinuxHost/install.sh
```

This builds and installs `~/.local/bin/iframe-linux-host`, creates a PIN in
`~/.config/iframe/linux-host-pin`, and enables `iframe-linux-host.service` (systemd --user) for
the current `$DISPLAY`.

```sh
iframe-linux start | stop | restart | status | logs | pin
systemctl --user disable --now iframe-linux-host     # stop starting it at login
```

Or run it by hand: `make -C LinuxHost && LinuxHost/build/iframe-linux-host --pin 123456`.

It advertises `_iframe._tcp` over Bonjour (with `os=linux`), so it shows up in the iPad app's
host list and in `iframe-client --list`.

## Options

| | |
|---|---|
| `--port <n>` | TCP port (default 7878) |
| `--fps <n>` | max frame rate (default 120; capped by the monitor's refresh rate and the client) |
| `--mbps <n>` | starting bitrate (default: the Mac host's formula for the resolution) |
| `--codec hevc\|h264` | default HEVC when the client can decode it |
| `--monitor <n>` | XRandR monitor index (default: the primary monitor) |
| `--display <d>` | X display (default `$DISPLAY`) |
| `--inflight <n>` | max unacknowledged frames (default 3) |
| `--pin <digits>` | default: `~/.config/iframe/linux-host-pin`, else random each launch |
| `--cmd-key auto\|ctrl\|super` | what ⌘ becomes, see below |
| `--no-publish` | don't advertise over Bonjour |

## Keys

Clients send macOS key codes. Every key maps by position (macOS key code → USB HID → evdev). The
one decision is what ⌘ becomes:

- **iPad (and any client that doesn't say otherwise): ⌘ → Ctrl**, so ⌘C copies, ⌘V pastes and
  ⌘T opens a tab, as a Mac user expects. ⌃ is also Ctrl, so Ctrl+C in a terminal still works.
- **Linux and Windows clients: ⌘ → Super.** These clients say what they are (`"os":"linux"` or
  `"os":"windows"` in their hello), and the host tells the client it's Linux (`"os":"linux"` in its
  welcome). The client then sends Ctrl, Alt and Super (the Windows key) as themselves, so every
  key lands exactly where it is on your keyboard. On Windows the Windows key reaches the host
  only while the stream is fullscreen; otherwise Windows keeps it for the Start menu.

`--cmd-key ctrl|super` overrides this. Text from the iPad's on-screen keyboard is typed as
Unicode; characters with no key on the layout are typed through a temporarily remapped spare
keycode.

The `os` fields are additions to the protocol. The iPad app and the Mac host ignore keys they
don't know, so nothing on the Apple side changes.

## Performance

Measured on an RTX 3060 Ti (`:0`, Cinnamon with compositing, 2560×1080): about 3.3 ms to
capture and 5.9 ms to encode per frame, so the 60 Hz monitor is the limit. Under sustained
load at 2560×1440 it was about 4 + 7 ms.

## Not yet / next steps

- **No virtual display.** It streams an existing monitor, and the client letterboxes it.
  Display-shape requests are logged and ignored. A virtual output (evdi, or a custom-EDID
  connector) would make the desktop match the client the way it does on the Mac.
- **Capture is a CPU copy.** XShm into system memory, then DMA to the GPU. NvFBC or KMS
  grabbing would keep it on the GPU and allow 120 fps at 1440p and above.
- **The colour matrix is BT.601.** NVENC converts RGB with BT.601 and the stream says so.
  The iPad app and the Linux client both honour it.
- **No Wayland** (would need PipeWire screencast and uinput), **no audio**, **no clipboard**, **no
  encryption** (PIN only, same as the Mac host). Use it on a trusted LAN or over Tailscale.
