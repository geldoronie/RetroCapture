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
# Then runs a SECOND pass with a shader preset applied (#186) and asserts the
# shaded output actually differs from the raw pass — i.e. the shader pipeline
# really ran. This guards the class of regression behind #184, where the
# per-pass render loop returned early and applyShader handed back the input
# texture unchanged: the original capture/stream smoke-test stayed green
# because it never applied a shader.
#
# Behaviour-preserving refactors must keep this green. Exit 0 = pass.
#
# Usage:  tools/smoke-test.sh [path-to-retrocapture-binary]
# Default binary: build-linux-x86_64/bin/retrocapture
# Env:
#   SMOKE_PORT    stream port (default 8080)
#   SMOKE_PRESET  preset to apply in the shader pass, relative to
#                 shaders/shaders_glsl/ (default crt/crt-hyllian-glow.glslp,
#                 a 6-pass CRT preset with no external LUT textures).
#                 Set SMOKE_PRESET=none to skip the shader pass.
#
# Needs: ffmpeg, ffprobe, curl, python3. A display — uses $DISPLAY if set,
# otherwise falls back to xvfb-run (install xvfb in CI).

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BIN="${1:-$REPO_ROOT/build-linux-x86_64/bin/retrocapture}"
# The app is launched with --stream-port "$PORT" (honored since #163), so a
# non-default SMOKE_PORT both avoids clashing with a live instance and doubles
# as a regression check that the bind actually follows the flag.
PORT="${SMOKE_PORT:-8080}"
SMOKE_PRESET="${SMOKE_PRESET:-crt/crt-hyllian-glow.glslp}"
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

# launch_app <xdg-subdir> <extra args...> — boots the binary with isolated XDG
# dirs, waits for the stream port and the test-pattern source swap. Sets the
# globals APP_PID and APP_LOG. Each call uses a fresh XDG root so a saved
# shader/config can't perturb the run.
APP_LOG=""
launch_app() {
    local sub="$1"; shift
    local cfg="$WORK/$sub/cfg" data="$WORK/$sub/data"
    mkdir -p "$cfg" "$data"
    XDG_CONFIG_HOME="$cfg" XDG_DATA_HOME="$data" \
        $LAUNCH_PREFIX "$BIN" --source test --stream-enable --hide-ui \
        --stream-port "$PORT" --width "$W" --height "$H" "$@" \
        > "$WORK/$sub-app.log" 2>&1 &
    APP_PID=$!

    local ready=0
    for _ in $(seq 1 40); do
        if ! kill -0 "$APP_PID" 2>/dev/null; then
            cat "$WORK/$sub-app.log"; fail "app exited during startup ($sub)"
        fi
        if port_up; then ready=1; break; fi
        sleep 0.5
    done
    [ "$ready" = 1 ] || { tail -20 "$WORK/$sub-app.log"; fail "stream port $PORT never came up ($sub)"; }

    # The stream port comes up during init() (while the default backend is still
    # attached); the test-pattern source is swapped in just after, via
    # setSourceType. Wait for that swap before capturing so we don't grab the
    # brief opening frames from whatever the platform factory opened first.
    APP_LOG=$(find "$data" -name retrocapture.log 2>/dev/null | head -1)
    if [ -n "$APP_LOG" ]; then
        local swapped=0
        for _ in $(seq 1 20); do
            grep -q "VideoCaptureTestPattern opened" "$APP_LOG" && { swapped=1; break; }
            sleep 0.5
        done
        [ "$swapped" = 1 ] || fail "test-pattern source was not used (check $APP_LOG)"
    else
        note "app log not found ($sub) — relying on content assertions"
    fi
    # Small settle so the synthetic frames are flowing through the encoder.
    sleep 1
}

# stop_app — kill the current instance and wait for the port to free up so the
# next launch_app binds cleanly.
stop_app() {
    [ -n "$APP_PID" ] && kill "$APP_PID" 2>/dev/null
    [ -n "$APP_PID" ] && wait "$APP_PID" 2>/dev/null
    APP_PID=""
    for _ in $(seq 1 20); do
        port_up || return 0
        sleep 0.5
    done
    note "warning: port $PORT still up after stop"
}

# capture_stream <out.ts> — pull a few seconds of /stream.
capture_stream() {
    local out="$1"
    timeout 6 curl -s -o "$out" "http://127.0.0.1:$PORT/stream"
    [ -s "$out" ] || fail "no stream data captured ($out)"
    note "captured $(wc -c < "$out") bytes of MPEG-TS"
}

# =============================================================================
# Pass 1 — raw test pattern (no shader). The original #149 pipeline check.
# =============================================================================
echo "── pass 1: raw test pattern (no shader)"
launch_app raw

capture_stream "$WORK/stream.ts"

res=$(ffprobe -v error -select_streams v -show_entries stream=width,height \
      -of csv=p=0 "$WORK/stream.ts" 2>/dev/null | head -1)
[ -n "$res" ] || fail "no decodable video stream"
note "stream resolution: $res"

# decode two frames (1s apart) for spatial + temporal checks
ffmpeg -v error -ss 2 -i "$WORK/stream.ts" -frames:v 1 -f rawvideo -pix_fmt rgb24 "$WORK/a.rgb" 2>/dev/null
ffmpeg -v error -ss 4 -i "$WORK/stream.ts" -frames:v 1 -f rawvideo -pix_fmt rgb24 "$WORK/b.rgb" 2>/dev/null
[ -s "$WORK/a.rgb" ] || fail "could not decode a video frame"

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

stop_app

# =============================================================================
# Pass 2 — shader applied (#186). Asserts the shader pipeline actually ran by
# requiring the shaded output to differ substantially from pass 1's raw frame.
# This is the check the capture/stream smoke-test lacked when #184 shipped.
# =============================================================================
if [ "$SMOKE_PRESET" = "none" ]; then
    echo "── pass 2: skipped (SMOKE_PRESET=none)"
    echo "✅ SMOKE PASS"
    exit 0
fi

PRESET_PATH="$REPO_ROOT/shaders/shaders_glsl/$SMOKE_PRESET"
[ -f "$PRESET_PATH" ] || fail "preset not found: $PRESET_PATH (set SMOKE_PRESET or 'none' to skip)"

echo "── pass 2: shader applied ($SMOKE_PRESET)"
launch_app shaded --preset "$PRESET_PATH"

# Fail fast and clearly if the preset didn't load / compile on this GL, so a
# shader-compile problem isn't misreported as "shader had no effect".
if [ -n "$APP_LOG" ]; then
    grep -q "m_shaderActive = true" "$APP_LOG" || \
        fail "preset did not activate (no 'm_shaderActive = true' in log)"
    if grep -qE "Failed to compile|No valid pass found" "$APP_LOG"; then
        grep -E "Failed to compile|No valid pass found" "$APP_LOG" | head -3
        fail "shader failed to compile/link on this GL — cannot validate shader output"
    fi
fi

capture_stream "$WORK/shaded.ts"

sres=$(ffprobe -v error -select_streams v -show_entries stream=width,height \
       -of csv=p=0 "$WORK/shaded.ts" 2>/dev/null | head -1)
[ -n "$sres" ] || fail "no decodable shaded video stream"
note "shaded stream resolution: $sres"

# Capture the shaded frame at the same offset as pass 1's frame so the
# moving-marker position is comparable — the shader's transformation is what
# must dominate the difference, not the marker.
ffmpeg -v error -ss 2 -i "$WORK/shaded.ts" -frames:v 1 -f rawvideo -pix_fmt rgb24 "$WORK/s.rgb" 2>/dev/null
[ -s "$WORK/s.rgb" ] || fail "could not decode a shaded video frame"

python3 - "$WORK/a.rgb" "$WORK/s.rgb" "$res" "$sres" <<'PY' || exit 1
import sys, numpy as np
raw_path, sh_path, res, sres = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
rw, rh = (int(x) for x in res.split(','))
sw, sh = (int(x) for x in sres.split(','))

raw = np.fromfile(raw_path, dtype=np.uint8)
sha = np.fromfile(sh_path, dtype=np.uint8)
if sha.size < sw*sh*3:
    print("❌ SMOKE FAIL: shaded frame too small"); sys.exit(1)
sha = sha[:sw*sh*3].reshape(sh, sw, 3).astype(float)

ok = True
def check(cond, msg):
    global ok
    print(("   [ok] " if cond else "   [X]  ") + msg)
    ok = ok and cond

# 1) the shaded frame is valid video (not black, has spatial detail).
check(sha.max() >= 60,  f"shaded frame not black (max={sha.max():.0f}, need >=60)")
check(sha.std() >= 10,  f"shaded frame has detail (std={sha.std():.1f}, need >=10)")

# 2) the shader actually altered the image. Compare against pass 1's raw frame
#    at matching dimensions. The moving marker alone moves the mean by <1;
#    a real shader (CRT mask / scanlines / glow) transforms essentially every
#    pixel, so the mean abs diff lands in the tens. A threshold of 5 sits far
#    above marker-only motion and far below a working shader's effect — so a
#    "shader did nothing" regression (#184) fails here.
if rw == sw and rh == sh and raw.size >= rw*rh*3:
    raw = raw[:rw*rh*3].reshape(rh, rw, 3).astype(float)
    diff = np.abs(raw - sha).mean()
    check(diff >= 5.0, f"shader changed the image vs raw (mean diff={diff:.2f}, need >=5)")
else:
    # Dimensions differ → the shader at least re-scaled the output, which by
    # itself proves the pipeline ran. Still assert validity above.
    print(f"   [ok] shaded dims {sw}x{sh} differ from raw {rw}x{rh} (shader rescaled output)")

sys.exit(0 if ok else 1)
PY

stop_app
echo "✅ SMOKE PASS"
