#!/bin/bash
# Builds iframe-linux-host and runs it as a systemd user service in your X session.
# Separate from the Mac host: its own binary, unit (iframe-linux-host.service) and PIN file
# (~/.config/iframe/linux-host-pin).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
UNIT=iframe-linux-host.service
PIN_FILE="${XDG_CONFIG_HOME:-$HOME/.config}/iframe/linux-host-pin"

echo "==> building"
make -C "$ROOT" >/dev/null
make -C "$ROOT" install >/dev/null
echo "    installed ~/.local/bin/iframe-linux-host"

if [ ! -s "$PIN_FILE" ]; then
    mkdir -p "$(dirname "$PIN_FILE")"
    ( umask 077; printf '%06d\n' $(( $(od -An -N4 -tu4 /dev/urandom) % 1000000 )) > "$PIN_FILE" )
    echo "==> new PIN in $PIN_FILE"
fi

echo "==> installing $UNIT"
mkdir -p "$HOME/.config/systemd/user"
sed "s|^Environment=DISPLAY=.*|Environment=DISPLAY=${DISPLAY:-:0}|" "$ROOT/systemd/$UNIT" \
    > "$HOME/.config/systemd/user/$UNIT"
systemctl --user daemon-reload
systemctl --user enable --now "$UNIT"
systemctl --user restart "$UNIT"

cat <<MSG

  iframe-linux-host is running and starts with your session.
  PIN:  $(cat "$PIN_FILE")
  Logs: journalctl --user -u iframe-linux-host -f
  Stop: systemctl --user disable --now iframe-linux-host

  It listens on port 7878 on every interface (LAN included), protected only by the PIN.
MSG
