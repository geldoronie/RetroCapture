#!/bin/bash
# Build a `.app` bundle for macOS and a tarball for distribution.
#
# Equivalent of the AppImage / installer scripts for Linux / Windows:
# produces `dist/RetroCapture-<version>-alpha-macos-x86_64.tar.gz`
# containing `RetroCapture.app/` with binary, Info.plist (with camera /
# microphone permission descriptions so the consent prompts actually
# appear), shaders, web assets, ssl certs.
#
# Does NOT codesign or notarise — that's a follow-up. Bundle is good
# enough to install on the maintainer's Mac and trigger the macOS
# privacy prompts. Does NOT relocate Homebrew dylibs into the bundle
# either; running on a different Mac requires the same Homebrew
# prefix. That's also a follow-up.
#
# Usage:
#   tools/build-macos-bundle.sh [--clean] [Release|Debug]

set -euo pipefail

CLEAN_BUILD=false
BUILD_TYPE="Release"

for arg in "$@"; do
    case "$arg" in
        --clean|-c) CLEAN_BUILD=true ;;
        Release|Debug) BUILD_TYPE="$arg" ;;
        --help|-h)
            cat <<EOF
Usage: $0 [--clean|-c] [Release|Debug]
  --clean, -c   Wipe the build dir before building
  Release       Optimised build (default)
  Debug         Debug-symbol build

Produces dist/RetroCapture-<version>-alpha-macos-<arch>.tar.gz with
a runnable .app bundle inside.
EOF
            exit 0
            ;;
        *)
            echo "❌ Unknown argument: $arg" >&2
            exit 1
            ;;
    esac
done

if [[ "$OSTYPE" != "darwin"* ]]; then
    echo "❌ build-macos-bundle.sh only runs on macOS" >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

if [ ! -f "CMakeLists.txt" ]; then
    echo "❌ Run this from the repo root" >&2
    exit 1
fi

ARCH="$(uname -m)"
VERSION=$(grep -E "^project\(RetroCapture VERSION" CMakeLists.txt | sed -E 's/.*VERSION ([0-9.]+[^ ]*).*/\1/')
RELEASE_VERSION="${VERSION}-alpha"

echo "🍎 RetroCapture macOS bundle"
echo "   Version : $RELEASE_VERSION"
echo "   Arch    : $ARCH"
echo "   Type    : $BUILD_TYPE"
echo ""

# Step 1 — produce the binary via the existing build script. That
# script handles dependency checks, the cmake configure, and the
# parallel build. We just consume the binary it leaves at
# build-macos-<arch>/bin/retrocapture.
if [ "$CLEAN_BUILD" = true ]; then
    bash "$SCRIPT_DIR/build-macos.sh" --clean "$BUILD_TYPE"
else
    bash "$SCRIPT_DIR/build-macos.sh" "$BUILD_TYPE"
fi

BUILD_DIR="build-macos-${ARCH}"
BIN="$REPO_ROOT/$BUILD_DIR/bin/retrocapture"
if [ ! -x "$BIN" ]; then
    echo "❌ Expected binary at $BIN but it isn't there" >&2
    exit 1
fi

# Step 2 — assemble the .app bundle in a temp staging dir.
STAGE="$REPO_ROOT/$BUILD_DIR/bundle-stage"
APP_NAME="RetroCapture.app"
APP_DIR="$STAGE/$APP_NAME"

rm -rf "$STAGE"
mkdir -p "$APP_DIR/Contents/MacOS"
mkdir -p "$APP_DIR/Contents/Resources"

echo "📦 Assembling $APP_NAME ..."

# 2a — binary
cp "$BIN" "$APP_DIR/Contents/MacOS/retrocapture"
chmod +x "$APP_DIR/Contents/MacOS/retrocapture"

# 2b — Info.plist (substitute @RETROCAPTURE_VERSION@)
sed -e "s/@RETROCAPTURE_VERSION@/$RELEASE_VERSION/g" \
    "$SCRIPT_DIR/macos/Info.plist.in" \
    > "$APP_DIR/Contents/Info.plist"

# 2c — read-only assets that Paths::getReadOnlyAssetsDir() expects to
# find under <exe>/../Resources/ when running from the bundle.
for d in shaders web; do
    if [ -d "$REPO_ROOT/$d" ]; then
        cp -R "$REPO_ROOT/$d" "$APP_DIR/Contents/Resources/"
    fi
done
if [ -d "$REPO_ROOT/assets/i18n" ]; then
    mkdir -p "$APP_DIR/Contents/Resources/assets"
    cp -R "$REPO_ROOT/assets/i18n" "$APP_DIR/Contents/Resources/assets/"
fi
# ssl/ ships templates for the optional HTTPS web portal. Include if
# present (developer dirs only — not always populated).
if [ -d "$REPO_ROOT/ssl" ]; then
    cp -R "$REPO_ROOT/ssl" "$APP_DIR/Contents/Resources/"
fi

# 2d — virtcam DAL plug-in (#85 macOS). The plug-in itself lives at
# /Library/CoreMediaIO/Plug-Ins/DAL/ — root-owned — so we can't just
# put it inside the .app and call it done. Instead we ship the bundle
# under Resources/ along with an install helper the user runs once
# (sudo) to copy it into place. UI detects whether install has
# happened via VirtualCameraOutputMac::isPluginInstalled().
PLUGIN_BUILT="$REPO_ROOT/$BUILD_DIR/bin/RetroCaptureVCam.plugin"
if [ -d "$PLUGIN_BUILT" ]; then
    cp -R "$PLUGIN_BUILT" "$APP_DIR/Contents/Resources/"
    cp "$SCRIPT_DIR/install-virtcam-macos.sh" \
       "$APP_DIR/Contents/Resources/install-virtcam.sh"
    chmod +x "$APP_DIR/Contents/Resources/install-virtcam.sh"
    echo "   virtcam : Contents/Resources/RetroCaptureVCam.plugin + install-virtcam.sh"
else
    echo "   virtcam : (skipped — plug-in not built; pass"
    echo "            -DRETROCAPTURE_BUILD_VIRTCAM_DAL_PLUGIN=ON to cmake)"
fi

echo "   binary  : Contents/MacOS/retrocapture"
echo "   plist   : Contents/Info.plist"
echo "   assets  : Contents/Resources/{shaders,web,assets/i18n,ssl}"
echo ""

# Step 3 — tarball the bundle for distribution.
mkdir -p "$REPO_ROOT/dist"
TARBALL_NAME="RetroCapture-${RELEASE_VERSION}-macos-${ARCH}.tar.gz"
TARBALL="$REPO_ROOT/dist/$TARBALL_NAME"

echo "🗜️  Packaging $TARBALL_NAME ..."
tar -czf "$TARBALL" -C "$STAGE" "$APP_NAME"
SIZE=$(du -h "$TARBALL" | cut -f1)

echo ""
echo "✅ Bundle ready"
echo "   App     : $STAGE/$APP_NAME"
echo "   Tarball : $TARBALL ($SIZE)"
echo ""
echo "🚀 To run the bundle locally:"
echo "   open \"$STAGE/$APP_NAME\""
echo ""
echo "📤 To distribute:"
echo "   The tarball under dist/ contains a runnable .app — the receiver"
echo "   extracts it and double-clicks. The first launch will trigger the"
echo "   camera/microphone permission prompts (the NSCameraUsageDescription"
echo "   / NSMicrophoneUsageDescription strings in Info.plist drive these)."
echo ""
echo "⚠️  Known limitations (follow-up issues):"
echo "   - Not codesigned: first launch needs right-click → Open to bypass Gatekeeper."
echo "   - Homebrew dylibs (libavcodec, glfw, libpng) are linked from /opt/homebrew"
echo "     or /usr/local — the bundle works on any Mac with the same Homebrew setup,"
echo "     but a proper redistributable bundle needs install_name_tool relocation."
