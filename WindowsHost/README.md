# iframe-windows-host

Streams a Windows desktop to iFrame clients: the iPad app and the Mac, Windows and Linux clients.
It's a **separate host** from the Mac's `iframe-host` (`Host/`, Swift) and `iframe-linux-host`
(`LinuxHost/`, C). They share only the wire protocol, so any client connects to any host. The C
protocol helpers (JSON, key table) come from `Linux/src/`.

```
Desktop Duplication (GPU, changed frames only) ─┐                ┌─ iPad app / Mac, Windows,
GPU video processor BGRA → NV12 (BT.709)        ├──TCP, PIN─────►│  Linux clients (unchanged)
Media Foundation HW encoder, HEVC/H.264         │                │
ack flow control, idle refine, ABR              ┘◄───────────────┤  mouse / keys / text
SendInput (scan codes, Unicode)                                  └─
```

The design matches the Mac and Linux hosts:

- **The GPU that drives the monitor does everything.** Capture, colour conversion and encoding run
  on one GPU and frames never leave its memory. On a PC with integrated and discrete graphics, the
  host picks the encoder on the monitor's GPU (`--list` shows which GPU that is).
- **Local cursor.** Windows never puts the pointer in captured frames. Its shape goes to the
  client as a PNG whenever it changes, and the client draws it at its own pointer position, so it
  moves with no network delay. Cursors that invert the screen (the text I-beam) are drawn black
  with a white outline.
- **Only changed frames are sent.** Windows reports when the screen changes, so a static screen
  sends nothing.
- **One frame at a time** goes through capture, encode and send.
- **At most 3 frames unacknowledged.** When the link backs up, frames are skipped before
  encoding, and the newest screen goes out as soon as an ack arrives.
- **Idle refinement:** when the screen settles, the last frame is re-encoded twice (after
  150 ms, then 400 ms) so text sharpens.
- **Adaptive bitrate:** −25% when frames drop, +10% after 3 clean seconds.
- **Keyframes only on demand**, low-latency CBR, no B-frames.

## Requirements

- Windows 10 1809 or later (Windows 11 recommended), x64.
- A GPU with a hardware video encoder: NVIDIA (NVENC), AMD (VCN) or Intel (Quick Sync).
- To build: Visual Studio 2019 or later with **Desktop development with C++**, and CMake and
  Ninja (both come with that workload).

## Install

```powershell
WindowsHost\install.ps1
```

This builds the host and installs it in `%LOCALAPPDATA%\Programs\iFrame`. It also:

- creates a PIN in `%APPDATA%\iframe\windows-host-pin`;
- allows the host through Windows Firewall on private and domain networks (one UAC prompt);
- registers the **iFrame Windows host** task, which starts the host at login with no window and
  restarts it if it stops.

The log is `%LOCALAPPDATA%\iframe\windows-host.log`.

```powershell
Start-ScheduledTask 'iFrame Windows host'     # start / Stop-ScheduledTask to stop
Get-Content $env:LOCALAPPDATA\iframe\windows-host.log -Wait
Get-Content $env:APPDATA\iframe\windows-host-pin
WindowsHost\install.ps1 -Uninstall            # keeps the PIN and the log
```

Windows doesn't let a normal program control elevated windows (Task Manager, installers, admin
terminals). To control those too, install from an admin PowerShell with
`WindowsHost\install.ps1 -Elevated`, which runs the host as administrator.

Or run it by hand: `WindowsHost\build.cmd`, then `WindowsHost\build\iframe-windows-host.exe --pin 123456`.
Windows asks once whether to allow it through the firewall; allow private networks.

It advertises `_iframe._tcp` over Bonjour with `os=windows`, through Windows' own mDNS responder,
so it shows up in every client's host list.

## Options

| | |
|---|---|
| `--port <n>` | TCP port (default 7878) |
| `--fps <n>` | max frame rate (default 120; capped by the monitor's refresh rate and the client) |
| `--mbps <n>` | starting bitrate (default: the Mac host's formula for the resolution) |
| `--codec hevc\|h264` | default HEVC when the client can decode it |
| `--monitor <n>` | monitor index from `--list` (default: the primary monitor) |
| `--list` | list monitors and the GPU driving each |
| `--inflight <n>` | max unacknowledged frames (default 3) |
| `--pin <digits>` | default: `%APPDATA%\iframe\windows-host-pin`, else random each launch |
| `--cmd-key auto\|ctrl\|win` | what ⌘ becomes, see below |
| `--no-publish` | don't advertise over Bonjour |

## Keys

Clients send macOS key codes. Every key goes in by position (macOS key code → USB HID → PC scan
code), so it lands where it is on the keyboard whatever the Windows keyboard layout. The one
decision is what ⌘ becomes:

- **iPad and Mac clients: ⌘ → Ctrl**, so ⌘C copies, ⌘V pastes and ⌘T opens a tab, as a Mac user
  expects. ⌃ is also Ctrl.
- **Windows and Linux clients: ⌘ → the Windows key.** These clients say what they are in their
  hello, and the host says it's Windows in its welcome (`"os":"windows"`). They then send Ctrl,
  Alt and the Windows/Super key as themselves, so every key lands exactly where it is.

`--cmd-key ctrl|win` overrides this. Text from the iPad's on-screen keyboard is typed as Unicode.
Ctrl+Alt+Delete can't be sent: Windows reserves it.

## Performance

Measured on an RTX 4090 at 3440×1440 (NVIDIA's H.264 and HEVC encoders through Media
Foundation): about 5–6 ms to convert and encode a frame.

## Not yet / next steps

- **No virtual display.** It streams an existing monitor, and the client letterboxes it.
  Display-shape requests are logged and ignored. A matching display needs an indirect display
  driver (IddCx), which has to be signed.
- **No UAC prompts, lock screen or sign-in screen.** These are on the secure desktop, which only
  a service running as SYSTEM can capture or control. The stream pauses while one is shown and
  resumes afterwards.
- **No cursor for clients that can't draw it.** All current clients draw the cursor themselves.
  An older client sees no pointer, because Windows never puts it in the captured frames.
- **Rotated monitors** stream sideways.
- **No audio**, **no clipboard**, **no encryption** (PIN only, same as the other hosts). Use it on
  a trusted LAN or over Tailscale.
