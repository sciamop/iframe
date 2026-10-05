#!/bin/bash
# Builds iframe-host into a signed "iFrame Host.app" and runs it as a LaunchAgent in your
# GUI login session. Permissions (Screen Recording, Accessibility) attach to the app itself,
# so they work no matter how you reach the Mac (SSH, etc.) and survive rebuilds and reboots.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP="$HOME/Applications/iFrame Host.app"
LABEL="com.toddfaulls.iframe.host"
AGENT="$HOME/Library/LaunchAgents/$LABEL.plist"
LOG="$HOME/Library/Logs/iframe-host.log"
PIN_FILE="$HOME/.config/iframe/pin"
DOMAIN="gui/$(id -u)"

# A stable signing identity keeps macOS from forgetting permissions on every rebuild.
IDENTITY="${IFRAME_SIGN_IDENTITY:-$(security find-identity -v -p codesigning | awk -F'"' '/Apple Development|Developer ID Application/ {print $2; exit}')}"
IDENTITY="${IDENTITY:--}"

echo "==> building"
swift build -c release --package-path "$ROOT" 2>&1 | grep -E "error|Compiling|Build complete" || true
BIN="$ROOT/.build/release/iframe-host"
[ -x "$BIN" ] || { echo "build failed"; exit 1; }

echo "==> assembling $APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BIN" "$APP/Contents/MacOS/iframe-host"
cp "$ROOT/Design/iFrame.icns" "$APP/Contents/Resources/iFrame.icns"
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleIdentifier</key><string>$LABEL</string>
  <key>CFBundleName</key><string>iFrame Host</string>
  <key>CFBundleDisplayName</key><string>iFrame Host</string>
  <key>CFBundleExecutable</key><string>iframe-host</string>
  <key>CFBundleIconFile</key><string>iFrame</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>0.2</string>
  <key>CFBundleVersion</key><string>2</string>
  <key>LSMinimumSystemVersion</key><string>14.0</string>
  <key>LSUIElement</key><true/>
</dict></plist>
PLIST

echo "==> signing with: $IDENTITY"
if ! codesign --force --sign "$IDENTITY" --identifier "$LABEL" "$APP" 2>/dev/null; then
  echo "    signing failed. Over SSH the login keychain is locked; run this first:"
  echo "      security unlock-keychain ~/Library/Keychains/login.keychain-db"
  echo "    (Not falling back to ad-hoc signing: that would silently revoke iFrame Host's permissions.)"
  exit 1
fi

mkdir -p "$(dirname "$PIN_FILE")"
[ -s "$PIN_FILE" ] || printf '%06d\n' $((RANDOM * 32768 % 1000000 + RANDOM % 1000)) | cut -c1-6 > "$PIN_FILE"
chmod 600 "$PIN_FILE"
PIN="$(tr -d '[:space:]' < "$PIN_FILE")"

echo "==> installing LaunchAgent"
mkdir -p "$(dirname "$AGENT")"
cat > "$AGENT" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>Label</key><string>$LABEL</string>
  <key>ProgramArguments</key><array>
    <string>$APP/Contents/MacOS/iframe-host</string>
    <string>--pin</string><string>$PIN</string>
    $(for a in "$@"; do printf '<string>%s</string>' "$a"; done)
  </array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>ThrottleInterval</key><integer>5</integer>
  <key>ProcessType</key><string>Interactive</string>
  <key>LimitLoadToSessionType</key><string>Aqua</string>
  <key>StandardOutPath</key><string>$LOG</string>
  <key>StandardErrorPath</key><string>$LOG</string>
</dict></plist>
PLIST

launchctl bootout "$DOMAIN/$LABEL" 2>/dev/null || true
sleep 0.5
launchctl bootstrap "$DOMAIN" "$AGENT"

echo
echo "iFrame Host is running (log: $LOG)"
echo "PIN: $PIN"
echo
echo "First time only: on the Mac's screen, open System Settings → Privacy & Security and enable"
echo "\"iFrame Host\" under Screen & System Audio Recording and under Accessibility, then run:"
echo "  launchctl kickstart -k $DOMAIN/$LABEL"
