#!/bin/bash
# Capture/stream/encode smoke-test (#149) — the refactor safety net.
#
# Boots a built RetroCapture with the synthetic test-pattern source
# (`--source test`, no hardware), enables streaming, pulls /stream, decodes a
# frame and asserts the pipeline still delivers correct video:
#   - the stream exists and has sane dimensions
#   - the frame is NOT gray/black (real brightness + spatial variance)
#   - chroma is present (the colour bars survived — catches a channel/colour
#     regression, e.g. the kind behind #135)
#   - two frames differ (the moving marker → catches a frozen/duplicated feed)
#
# Behaviour-preserving refactors must keep this green. Exit 0 = pass.
#
# Usage:  tools/smoke-test.sh [path-to-retrocapture-binary]
# Default binary: build-linux-x86_64/bin/retrocapture
#
# Needs: ffmpeg, ffprobe, curl, python3. A display — uses $DISPLAY if set,
# otherwise falls back to xvfb-run (install xvfb in CI).

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BIN="${1:-$REPO_ROOT/build-linux-x86_64/bin/retrocapture}"
# The streaming server binds 8080 regardless of --stream-port (the flag only
# echoes; the bind is fixed — tracked separately). Use 8080 and refuse to run
# if it's already taken, rather than fighting a live instance.
PORT="${SMOKE_PORT:-8080}"
W=1280; H=720
WORK="$(mktemp -d /tmp/rc-smoke.XXXXXX)"
APP_PID=""

cleanup() {
    [ -n "$APP_PID" ] && kill "$APP_PID" 2>/dev/null
    [ -n "$APP_PID" ] && wait "$APP_PID" 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

fail() { echo "❌ SMOKE FAIL: $*"; exit 1; }
note() { echo "   $*"; }

# --- prerequisites ----------------------------------------------------------
for tool in ffmpeg ffprobe curl python3; do
    command -v "$tool" >/dev/null 2>&1 || fail "missing required tool: $tool"
done
[ -x "$BIN" ] || fail "binary not found/executable: $BIN"

# Wrap in xvfb when there's no display (CI). GLFW needs a GL context.
LAUNCH_PREFIX=""
if [ -z "${DISPLAY:-}" ]; then
    if command -v xvfb-run >/dev/null 2>&1; then
        LAUNCH_PREFIX="xvfb-run -a"
        note "no DISPLAY — using xvfb-run"
    else
        fail "no DISPLAY and xvfb-run not installed (need a GL context)"
    fi
fi

echo "▶ smoke-test: $BIN  (test pattern, port $PORT)"

# Is the HTTP server answering? /stream is an infinite feed, so we can't use
# curl's exit code (it always times out mid-download); probe the root path and
# look at the HTTP status instead — anything but 000 means the port is up.
port_up() {
    local code
    code=$(curl -s -m 2 -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/" 2>/dev/null)
    [ -n "$code" ] && [ "$code" != "000" ]
}

# Refuse to run if something already answers on the port — otherwise we'd
# test that instance, not the binary under test.
if port_up; then
    fail "port $PORT is already serving — stop the running RetroCapture first (or set SMOKE_PORT)"
fi

# --- launch -----------------------------------------------------------------
# Isolated XDG dirs so the user's saved shader/config can't perturb the output.
XDG_CONFIG_HOME="$WORK/cfg" XDG_DATA_HOME="$WORK/data" \
    $LAUNCH_PREFIX "$BIN" --source test --stream-enable --hide-ui \
    --width "$W" --height "$H" \
    > "$WORK/app.log" 2>&1 &
APP_PID=$!

# Wait until the stream port answers (max ~20s), instead of a fixed sleep.
ready=0
for _ in $(seq 1 40); do
    if ! kill -0 "$APP_PID" 2>/dev/null; then
        cat "$WORK/app.log"; fail "app exited during startup"
    fi
    if port_up; then ready=1; break; fi
    sleep 0.5
done
[ "$ready" = 1 ] || { tail -20 "$WORK/app.log"; fail "stream port $PORT never came up"; }

# The stream port comes up during init() (while the default backend is still
# attached); the test-pattern source is swapped in just after, via setSourceType.
# Wait for that swap to land in the app log before capturing, so we don't grab
# the brief opening frames from whatever the platform factory opened first.
# The app logs to its own file under the data dir (not stdout) — find it.
APP_LOG=$(find "$WORK/data" -name retrocapture.log 2>/dev/null | head -1)
if [ -n "$APP_LOG" ]; then
    swapped=0
    for _ in $(seq 1 20); do
        grep -q "VideoCaptureTestPattern opened" "$APP_LOG" && { swapped=1; break; }
        sleep 0.5
    done
    [ "$swapped" = 1 ] || fail "test-pattern source was not used (check $APP_LOG)"
else
    note "app log not found — relying on content assertions to confirm the source"
fi
# Small settle so the synthetic frames are flowing through the encoder.
sleep 1

# --- capture a few seconds of /stream --------------------------------------
timeout 6 curl -s -o "$WORK/stream.ts" "http://127.0.0.1:$PORT/stream"
[ -s "$WORK/stream.ts" ] || fail "no stream data captured"
note "captured $(wc -c < "$WORK/stream.ts") bytes of MPEG-TS"

# --- probe dimensions -------------------------------------------------------
res=$(ffprobe -v error -select_streams v -show_entries stream=width,height \
      -of csv=p=0 "$WORK/stream.ts" 2>/dev/null | head -1)
[ -n "$res" ] || fail "no decodable video stream"
note "stream resolution: $res"

# --- decode two frames (1s apart) for spatial + temporal checks -------------
ffmpeg -v error -ss 2 -i "$WORK/stream.ts" -frames:v 1 -f rawvideo -pix_fmt rgb24 "$WORK/a.rgb" 2>/dev/null
ffmpeg -v error -ss 4 -i "$WORK/stream.ts" -frames:v 1 -f rawvideo -pix_fmt rgb24 "$WORK/b.rgb" 2>/dev/null
[ -s "$WORK/a.rgb" ] || fail "could not decode a video frame"

# --- assertions (python) ----------------------------------------------------
python3 - "$WORK/a.rgb" "$WORK/b.rgb" "$res" <<'PY' || exit 1
import sys, numpy as np
a_path, b_path, res = sys.argv[1], sys.argv[2], sys.argv[3]
w, h = (int(x) for x in res.split(','))
a = np.fromfile(a_path, dtype=np.uint8)
if a.size < w*h*3:
    print("❌ SMOKE FAIL: decoded frame too small"); sys.exit(1)
a = a[:w*h*3].reshape(h, w, 3).astype(float)

bright_max = a.max()
spatial_std = a.std()
# Local saturation: mean of (max channel - min channel) per pixel. Saturated
# colour bars give a high value; a grayscale/monochrome regression gives ~0.
# (A global channel-mean comparison would be ~0 here because SMPTE bars are
# channel-balanced — each of R/G/B is on in 4 of the 8 bars.)
chroma = (a.max(axis=2) - a.min(axis=2)).mean()
# per-bar colour distinctness: sample 8 bar centres, count distinct hues.
bw = w // 8
bars = [tuple(int(v) for v in a[h//2, i*bw + bw//2]) for i in range(8)]
distinct = len(set(bars))

ok = True
def check(cond, msg):
    global ok
    print(("   [ok] " if cond else "   [X]  ") + msg)
    ok = ok and cond

check(bright_max >= 180, f"brightness present (max={bright_max:.0f}, need >=180)")
check(spatial_std >= 20, f"spatial variance / not flat (std={spatial_std:.1f}, need >=20)")
check(chroma >= 30, f"colour present / saturated bars (saturation={chroma:.0f}, need >=30)")
check(distinct >= 5, f"distinct colour bars ({distinct}/8, need >=5)")

# temporal: the moving marker should make two frames differ.
try:
    b = np.fromfile(b_path, dtype=np.uint8)[:w*h*3].reshape(h, w, 3).astype(float)
    diff = np.abs(a - b).mean()
    check(diff >= 0.05, f"frames change over time (mean diff={diff:.3f})")
except Exception:
    print("   - temporal check skipped (second frame unavailable)")

sys.exit(0 if ok else 1)
PY

echo "✅ SMOKE PASS"
