#!/bin/bash
# Builds the iFrame Mac client as one Universal app (Apple silicon + Intel) and zips it for
# copying to other Macs: dist/iFrame.app and dist/iFrame-<version>-macOS-Universal.zip.
#
# Signs with your Apple Development / Developer ID identity when there is one, otherwise ad hoc.
# Set IFRAME_SIGN_IDENTITY=- to force ad hoc. Over SSH, run `security unlock-keychain` first.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
DERIVED="$ROOT/.build/mac-client"
DIST="$ROOT/dist"

IDENTITY="${IFRAME_SIGN_IDENTITY:-$(security find-identity -v -p codesigning | awk -F'"' '/Developer ID Application|Apple Development/ {print $2; exit}')}"
IDENTITY="${IDENTITY:--}"
SIGNING=()
if [ "$IDENTITY" = "-" ]; then
  SIGNING=(CODE_SIGN_STYLE=Manual CODE_SIGN_IDENTITY=- DEVELOPMENT_TEAM=)
fi

echo "==> generating project"
xcodegen generate --quiet

echo "==> building (arm64 + x86_64, signing: $IDENTITY)"
# The generic destination is what makes xcodebuild build every architecture, not just this Mac's.
APP="$DERIVED/Build/Products/Release/iFrame.app"
rm -rf "$APP"
set +e
xcodebuild -project iFrame.xcodeproj -scheme iFrameMac -configuration Release \
  -destination 'generic/platform=macOS' -derivedDataPath "$DERIVED" ${SIGNING[@]+"${SIGNING[@]}"} build \
  | grep -E "error:|warning: .*\.swift|BUILD (SUCCEEDED|FAILED)"
BUILD_STATUS=${PIPESTATUS[0]}
set -e
if [ "$BUILD_STATUS" -ne 0 ] || [ ! -d "$APP" ] || ! codesign --verify --strict "$APP" 2>/dev/null; then
  echo "build or signing failed (over SSH? run: security unlock-keychain)"
  exit 1
fi

VERSION="$(/usr/libexec/PlistBuddy -c 'Print CFBundleShortVersionString' "$APP/Contents/Info.plist")"
ZIP="$DIST/iFrame-$VERSION-macOS-Universal.zip"
mkdir -p "$DIST"
rm -rf "$DIST/iFrame.app" "$ZIP"
ditto "$APP" "$DIST/iFrame.app"
ditto -c -k --keepParent "$DIST/iFrame.app" "$ZIP"

SIGNER="$(codesign -dv "$DIST/iFrame.app" 2>&1 | awk -F= '/^Authority/ {print $2; exit}')"
echo "==> $(lipo -archs "$DIST/iFrame.app/Contents/MacOS/iFrame") · signed ${SIGNER:-ad hoc}"
echo "    $DIST/iFrame.app"
echo "    $ZIP"
