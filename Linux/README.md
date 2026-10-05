# iFrame Linux client

A Linux client for iFrame hosts. It speaks the same protocol as the iPad app, so nothing on the
host changes. It connects to the Mac's `iframe-host`, which builds a virtual display shaped like
your Linux window or monitor, and to [`iframe-linux-host`](../LinuxHost/README.md), which streams
another Linux desktop.

```
iframe-host (Mac) ──TCP──► iframe-client (Linux)
                            FFmpeg decode: NVDEC → VAAPI → Vulkan → software
                            SDL2 window (NV12 texture), mouse / wheel / keyboard back to the Mac
```

Written in C against SDL2 and FFmpeg's libavcodec. Bonjour discovery shells out to `avahi-browse`.

## Build

```sh
sudo apt install libsdl2-dev libavcodec-dev libavutil-dev avahi-utils   # Debian / Ubuntu / Mint
make -C Linux
make -C Linux install        # → ~/.local/bin/iframe-client (PREFIX=... to change)
```

## Run

```sh
iframe-client                          # find a Mac on the LAN (or reuse the last one), fullscreen
iframe-client 192.168.1.20 --pin 1234  # by address; host:port works too
iframe-client --window 1600x900        # windowed; the Mac's display follows the window's size
iframe-client --list                   # Macs advertising _iframe._tcp
```

The PIN comes from `--pin`, `$IFRAME_PIN`, or a prompt. After a successful connection the host,
port, PIN and scale are saved to `~/.config/iframe/linux-client` (mode 600), so a bare
`iframe-client` reconnects next time. A saved PIN is only ever sent to the host it was saved for.

If the connection fails (the Mac sleeps, Wi-Fi drops), the client keeps retrying every second.
If the host ends the session cleanly (another device connected, or the host stopped), the
client waits for a click or key press before reconnecting. Reconnecting straight away would
take the session back from the iPad that just connected. A wrong PIN exits with status 2.

### Display size (`--scale`)

The host creates a virtual display at your window's exact pixel size. `--scale` sets the pixels
per Mac point, the same setting as the iPad's density picker:

| `--scale` | Mac desktop on a 2560×1440 monitor | |
|---|---|---|
| `1` (default) | 2560×1440 points | most space, like a normal monitor |
| `1.33`, `1.6` | 1920×1080, 1600×900 | in between |
| `2` | 1280×720 | Retina-sharp, big UI (good on 4K) |
| `0` | — | stream the Mac's own display instead (letterboxed) |

In windowed mode, resizing the window reshapes the Mac's display about 0.4 s after you stop
dragging.

### Keyboard

Keys are sent by position (USB HID usage → macOS key code), so the Mac's own keyboard layout
applies. The ⌘ key defaults to **Alt**, which sits where ⌘ does on a Mac keyboard. The other
modifier then becomes ⌥:

| `--cmd-key` | ⌘ | ⌥ | ⌃ |
|---|---|---|---|
| `alt` (default) | Alt | Super | Ctrl |
| `super` | Super | Alt | Ctrl |
| `ctrl` | Ctrl | Alt | Super |

In fullscreen the client grabs the keyboard, so Alt+Tab, Super and friends go to the Mac.

**On a Linux host** (`iframe-linux-host`), every key maps 1:1: Ctrl, Alt and Super arrive as
themselves. The host says it's Linux in its welcome message, and the client then stops mapping
Alt to ⌘. Passing `--cmd-key` explicitly keeps your mapping.

**Hotkeys** (always local, never sent): Ctrl+Alt+Shift plus

| key | |
|---|---|
| F | toggle fullscreen |
| G | toggle keyboard grab |
| S | toggle per-second stats on stdout |
| K | ask for a keyframe |
| Q | quit |

The window title shows the host, resolution, decoder, fps, bitrate, round trip and decode time.

### Other options

`--scroll-speed X`, `--invert-scroll`, `--local-cursor` (show the Linux pointer over the stream
as well; the host's cursor is part of the video), `--view-only` (watch without sending mouse or
keyboard), `--no-hw`, `--h264`, `--vsync`, `--stats`.

Colours follow the stream's own YUV matrix: BT.709 from the Mac, BT.601 from NVENC.

## Testing without a Mac

`tools/fake-host.c` speaks the host side of the protocol. It checks the PIN, follows display
requests, does ack-based flow control, and streams a test pattern encoded with NVENC / x265 /
x264. A white square marks where the client says the mouse is, and every input message is logged.

```sh
make -C Linux build/fake-host
Linux/build/fake-host --pin 1234 --fps 120 &
Linux/build/iframe-client 127.0.0.1 --pin 1234 --window 1280x800 --stats
```

## Notes / next steps

- **GPU → CPU → GPU.** Hardware-decoded frames are downloaded to system memory and uploaded to
  an SDL texture. On an RTX 3060 Ti that is about 5–6 ms per frame at 2560×1440, including the
  download. CUDA/VAAPI → GL interop would make it zero-copy.
- The text-focus message, used by the iPad to raise its on-screen keyboard, is ignored.
- It is X11/Wayland-agnostic through SDL. Keyboard grab under Wayland depends on the compositor.
- Same limits as the host: no encryption (PIN only), no audio, no clipboard.
