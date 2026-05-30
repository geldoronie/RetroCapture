#!/bin/bash
# Install RetroCaptureVCam.plugin into the system DAL directory so
# every CoreMediaIO consumer (OBS, Discord, Zoom, Chrome legacy
# path...) picks it up on next launch.
#
# Runs from inside the .app's Resources directory: the build
# bundles this script there next to the .plugin. Equivalent of
# the NSIS regsvr32 step on Windows.
#
# Requires sudo — /Library/CoreMediaIO/Plug-Ins/DAL/ is
# root-owned and not user-writable.
#
# Usage:
#   sudo /Applications/RetroCapture.app/Contents/Resources/install-virtcam.sh
#
# Idempotent: re-running replaces an existing copy of the plug-in
# (atomic rename so a half-installed plug-in never gets loaded).

set -euo pipefail

DAL_DIR="/Library/CoreMediaIO/Plug-Ins/DAL"
PLUGIN_NAME="RetroCaptureVCam.plugin"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_PLUGIN="$SCRIPT_DIR/$PLUGIN_NAME"

if [ ! -d "$SRC_PLUGIN" ]; then
    echo "❌ $SRC_PLUGIN not found." >&2
    echo "   Run this script from inside the .app's Resources directory." >&2
    exit 1
fi

if [ "$EUID" -ne 0 ]; then
    echo "❌ This script needs sudo (the install path is root-owned)." >&2
    echo "   Try: sudo $0" >&2
    exit 1
fi

echo "📷 RetroCapture virtcam install"
echo "   Source : $SRC_PLUGIN"
echo "   Target : $DAL_DIR/$PLUGIN_NAME"
echo ""

mkdir -p "$DAL_DIR"

# Atomic-ish replace: copy into a temp name, then rename. If the
# copy fails the existing install (if any) stays untouched.
TMP_TARGET="$DAL_DIR/.${PLUGIN_NAME}.installing.$$"
rm -rf "$TMP_TARGET"
cp -R "$SRC_PLUGIN" "$TMP_TARGET"

# Standard ownership/perms for /Library/CoreMediaIO/Plug-Ins/DAL
# bundles (root:wheel, 755). The rest of the bundle's contents
# inherit the install's mode bits already.
chown -R root:wheel "$TMP_TARGET"
chmod -R 755 "$TMP_TARGET"

# Now atomically swap in.
rm -rf "$DAL_DIR/$PLUGIN_NAME"
mv "$TMP_TARGET" "$DAL_DIR/$PLUGIN_NAME"

echo "✅ Installed."
echo ""
echo "⚠️  Already-running consumers (OBS, Discord, Zoom, ...) need to"
echo "    quit + relaunch before they re-scan the DAL directory and see"
echo "    the camera. Mac apps that ship their own CMIO bypass may take"
echo "    additional steps."
echo ""
echo "To uninstall: sudo rm -rf $DAL_DIR/$PLUGIN_NAME"
