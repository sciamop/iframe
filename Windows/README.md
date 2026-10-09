# iFrame for Windows

A Windows 10/11 x64 client for `iframe-host` on a Mac or `iframe-linux-host` on a Linux PC ([LinuxHost](../LinuxHost/README.md)). The portable app includes its runtime; users do not need Node, Python, FFmpeg, or a separate codec installation.

## Connect

1. Start the host. On a Mac, follow the repository README and grant Screen Recording and Accessibility permissions; on Linux, see [LinuxHost](../LinuxHost/README.md).
2. Open `iFrame-0.1.0-Windows-x64.exe` from `Windows/dist`.
3. Choose a nearby computer (marked Mac or Linux), or enter its IP address, Tailscale address, or hostname. The default TCP port is **7878**. Discovery uses mDNS on the local network; manual addresses also work when discovery is unavailable. Select **☆ Save** next to the port to keep an address under **Saved computers**; select a saved computer to fill in its address and port.
4. Enter the host PIN, choose the display size/density, and click **Connect**.

The app remembers saved Macs, the last address and display settings, but never saves the PIN. Preferences are stored in Electron's per-user app data directory. Windows may ask to allow local network access for discovery. The distributed executable is unsigned.

**Retina** requests a virtual Mac display at the selected pixel size with two pixels per Mac point. **Native** gives more desktop space. **Mirror** uses the Mac's existing display. Display size is chosen at connection time; resizing the app scales the picture without restarting the Mac display. Reconnect to change resolution or density. A Linux host streams its monitor as it is, so these settings apply only to Macs.

## Controls

| Input | Action |
| --- | --- |
| Mouse / trackpad | Move, left/right/middle click, drag, vertical/horizontal scroll |
| Keyboard | Physical key passthrough using the Mac's keyboard layout |
| Ctrl (default) | Mac Command; Ctrl+C / Ctrl+V become Command+C / Command+V |
| Alt | Mac Option |
| Windows key, fullscreen | Mac Command; the Start menu and Windows+ shortcuts are captured while the stream is fullscreen and focused |
| Windows key, windowed (default mapping) | Mac Control, when Windows does not intercept it |
| Linux host | Keys map one-to-one: Ctrl → Ctrl, Alt → Alt, Windows key → Super (fullscreen), so Super opens the desktop's menu; the Ctrl/Command option is ignored |
| Ctrl+Alt+F | Toggle fullscreen |
| Ctrl+Alt+R | Release keyboard focus to the toolbar |
| Ctrl+Alt+Q | Disconnect |
| Send text | Send pasted or typed Unicode text to the focused Mac field |
| Refresh | Request a new keyframe |

Disable **On a Mac, use Ctrl for Command shortcuts** for literal Ctrl → Control and Windows → Command mapping. Windows reserves shortcuts such as Alt+Tab, Windows+L, and Ctrl+Alt+Delete; these stay local. Held keys and mouse buttons are released when focus leaves the desktop. The host also releases input when the connection ends.

## Build and test

Requires Windows x64 and Node.js **22.12 or newer** with npm. Run from `Windows`:

```powershell
npm ci
npm test
npm run test:smoke
npm start
npm run dist
```

`dist/iFrame-0.1.0-Windows-x64.exe` is the portable executable. `dist/win-unpacked/iFrame.exe` can also be launched directly, provided its neighboring files remain in place. `npm run pack` builds only that unpacked directory. Packaging downloads Electron and the NSIS packaging tools on first use. The packaged app also accepts `--smoke-test` to run the integration test offscreen and write a result to `test-output` in the current directory.

The Node tests exercise Swift-compatible wire bytes, fragmented TCP reads, input mapping, malformed data, handshake, acknowledgements, and authentication failures. The Electron smoke test starts a loopback mock host, decodes real H.264 key and delta frames one at a time, checks pixels and acknowledgements, forwards input, and exercises disconnect/reconnect and PIN errors. It renders offscreen and writes screenshots to `test-output/`; it needs no Mac or network credentials.

`test/desktop.h264` is a synthetic four-frame test pattern, generated with:

```powershell
ffmpeg -f lavfi -i testsrc2=size=160x96:rate=60 -frames:v 4 -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p -x264-params aud=1:keyint=60:bframes=0 -f h264 test/desktop.h264
```

FFmpeg is only needed to regenerate that fixture. Real Mac capture, permission behavior, mDNS across your network, and sustained high-resolution performance must be checked with a running Mac host.

## Implementation and limits

- Uses protocol version 1 without host changes: length-prefixed TCP, PIN hello, virtual-display request, video format/frame messages, decode ACKs, ping, input, and keyframe requests.
- Requests **H.264**. WebCodecs selects a compatible hardware or software decoder. A host forced to HEVC produces an actionable error; use automatic codec selection or `--codec h264` on the host.
- Converts four-byte AVCC NAL lengths to Annex B and injects SPS/PPS on keyframes. Frames are drawn directly to a canvas and acknowledged after decode and drawing, preserving the host's flow control. Malformed frames trigger throttled keyframe recovery; repeated decoder errors close the session.
- The renderer is sandboxed with context isolation, no Node integration, a restrictive content security policy, and a narrow validated IPC bridge. Socket framing is bounded at the host protocol's 32 MiB maximum.
- The host protocol is **not encrypted**. Use a trusted LAN or Tailscale; do not expose the host directly to the Internet.
- No audio, automatic clipboard synchronization, or HEVC. (The Mac's cursor is drawn locally as the Windows pointer, in the Mac's current shape.) The Send text button is an explicit text transfer, not clipboard sync.
- This is an Electron client, not a native WinUI application. It includes Chromium and therefore has a larger distribution and memory footprint.

MIT, matching the repository license. Bundled Electron and dependency licenses are included in the distribution.
