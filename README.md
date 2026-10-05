# iFrame

Low-latency remote desktop for Apple silicon Macs: a native iPad app, plus a browser client for Mac, Windows and Linux. It sends a video stream instead of pixel tiles.

**Requirements:** a Mac with Apple silicon running macOS 14 or later (developed on an M4 Mac mini, macOS 15), an iPad on iPadOS 17 or later, Xcode, and [XcodeGen](https://github.com/yonaskolb/XcodeGen) plus `rsvg-convert` and ImageMagick if you regenerate icons (`brew install xcodegen librsvg imagemagick`).

```
Mac (iframe-host)                                   iPad (iFrame app)
ScreenCaptureKit, IOSurface 4:2:0 ─┐               ┌─ VideoToolbox HW decode (sync)
VideoToolbox HEVC/H.264 encoder    │  TCP, no-delay│  AVSampleBufferVideoRenderer,
  low-latency rate control,        ├──────────────►│  display-immediately, fed off-main
  no B-frames, keyframes on demand │  WMM video AC │
ack-driven flow control,           │◄──────────────┤  acks, pings, mouse/keys/scroll
  adaptive bitrate, idle refine    ┘               └─ trackpad, touch, hardware keyboard
CGEvent input injection
```

What makes it fast:
- **Zero-copy pipeline.** Capture hands IOSurfaces straight to the media engine. No color conversion and no CPU copies.
- **Low-latency rate control.** One frame in, one frame out, no reordering. Keyframes only on connect or after an error, so there are no periodic spikes.
- **Ack-based flow control.** At most 3 frames can be unacknowledged. When Wi-Fi backs up, frames are skipped *before* encoding instead of piling up in socket buffers, and the newest screen goes out as soon as the link frees up. Lag never builds up.
- **Adaptive bitrate.** It backs off quickly under congestion and recovers slowly.
- **Idle refinement.** When the screen stops changing, the last frame is re-encoded twice so text sharpens. A static screen then costs zero bandwidth.
- **Static screens send nothing.** ScreenCaptureKit only delivers frames that changed.
- **Wi-Fi video priority.** Traffic is tagged with the Wi-Fi video access category.
- **Virtual display shaped like the iPad.** The host creates a Retina display at the iPad's exact resolution and 120 Hz, and reshapes it when the iPad rotates. No letterboxing or scaling.

## Host (Mac)

```sh
scripts/install-host.sh      # build, sign, install as a LaunchAgent (starts at login, restarts if it dies)
tail -f ~/Library/Logs/iframe-host.log
launchctl kickstart -k gui/$(id -u)/com.toddfaulls.iframe.host   # restart
```

It installs `~/Applications/iFrame Host.app` (a background app with no Dock icon) and runs it in your
GUI login session, so it works headless and no matter how you reach the Mac. The PIN is kept in
`~/.config/iframe/pin`.

**Permissions (one time):** System Settings → Privacy & Security → enable **iFrame Host** under
*Screen & System Audio Recording* and *Accessibility*. They're tied to the app's signature, so they
survive rebuilds. Over SSH, run `security unlock-keychain` before reinstalling, or signing fails.

Test without an iPad: `.build/release/iframe-host probe 127.0.0.1 --pin <PIN> --screen 2732x2048@2`

## Browser client (Mac, Windows, Linux)

The host also serves a browser client over HTTPS, starting automatically with it:

```
https://<your-mac>.local:7880     (or https://<mac-ip>:7880)
```

Open it in a current Chrome, Edge, Firefox or Safari, enter the PIN, and connect. The Mac creates a virtual display matching your browser window (it reshapes as you resize or go fullscreen), at 120 Hz on high-refresh monitors.

- **Certificate:** browsers only allow hardware video decoding (WebCodecs) on secure pages, so the host makes a self-signed certificate on first run (`~/.config/iframe/web/`). Your browser warns once per machine. Choose *Advanced → Proceed* (Chrome/Edge) or *Accept the Risk* (Firefox).
- **Keyboard:** "Use Ctrl as ⌘" (on by default on Windows/Linux) makes Ctrl+C/V/Z etc. work as on a Mac. In fullscreen, Chrome and Edge also capture Esc and system shortcuts.
- **Video:** H.264, which every browser decodes in hardware. The iPad app keeps HEVC.
- `--web-port <n>` changes the port; `--no-web` turns the browser client off. Files live in `Web/`.

## Client (iPad)

```sh
xcodegen generate   # creates iFrame.xcodeproj from project.yml
open iFrame.xcodeproj
```

Building it yourself? Change `DEVELOPMENT_TEAM` and the `com.toddfaulls` bundle IDs in `project.yml` (and `LABEL` in `scripts/install-host.sh`) to your own. Then run it on your iPad. A free Apple ID works, but the install expires after 7 days.

Your Mac appears under **Mac** on the connect screen. Enter the PIN, pick a display density, and tap **Connect**.

### Controls
| Input | Action |
|---|---|
| Trackpad / mouse | Pointer, click, right click, two-finger scroll |
| Hardware keyboard | Full key passthrough including ⌘ shortcuts (except the ones iPadOS reserves, like ⌘Tab and ⌘Space) |
| Tap / two-finger tap | Click / right click |
| Drag | Move pointer |
| Long-press, then drag | Click-drag |
| Two-finger drag | Scroll |
| Three-finger tap | Toolbar (on-screen keyboard, stats, disconnect) |

## Notes
- The virtual display uses `CGVirtualDisplay`, a private CoreGraphics API (the same one DeskPad, BetterDisplay and Chromium's tests use). A future macOS could change it; run the host with `--no-virtual` to stream the Mac's existing display instead.
- `scripts/make-icon.py` generates the icon; `scripts/build-icons.sh` renders every icon size and the app's accent color from it.

## Known limits / next steps
- **Video isn't encrypted; there's only a PIN.** Use it on a trusted LAN or over Tailscale.
- **The cursor is part of the video.** A local cursor overlay would make pointing feel instant.
- **Transport is TCP.** That's fine on a LAN. UDP with FEC would handle lossy networks better.
- **No audio and no clipboard sync yet.**

## License

MIT. See [LICENSE](LICENSE).
