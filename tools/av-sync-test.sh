#!/bin/bash
# A/V sync measurement (#196) — the yardstick for the A/V sync overhaul (#201).
#
# Boots RetroCapture with the synthetic sync sources: `--source test` flashes
# the bottom band of the frame and `--audio-source test` beeps, both during
# the first 100 ms of every whole second of the same monotonic clock. While
# it runs, the script pulls /stream AND makes a recording (via the REST API),
# then measures, for each output, the audio-minus-video offset of every
# flash/beep pair (tools/av-sync-analyze.py): mean, max, spread and the drift
# slope in ms/min.
#
# `--test-audio-skew-ppm` makes the fake audio device's clock run fast/slow
# against the system clock, the way a real capture card's audio crystal does
# (#124). A correct pipeline keeps audio locked to the capture timeline no
# matter the skew; a pipeline that trusts the sample count drifts by exactly
# the skew (1000 ppm = 60 ms/min).
#
# Usage:  tools/av-sync-test.sh [path-to-retrocapture-binary]
# Env:
#   AVSYNC_SECONDS     capture length (default 40)
#   AVSYNC_SKEW_PPM    fake audio-device clock error (default 1000)
#   AVSYNC_FRAGMENT_MS fake audio-device fragment size (default 20; the
#                      pre-#197 PulseAudio capture delivered 100 ms)
#   AVSYNC_PORT        stream port (default 8091)
#   AVSYNC_FPS         capture / stream fps (default 60)
#   AVSYNC_STREAM_FPS  stream fps when it differs from the capture's (#247:
#                      e.g. 30 fps source + --frame-gen 2x at 60)
#   AVSYNC_PATTERN     test-pattern variant (RETROCAPTURE_TEST_PATTERN, e.g.
#                      "motion"); the sync flash band is in every variant
#                      except levels / interlaced
#   AVSYNC_WIDTH / AVSYNC_HEIGHT  capture size (default 1280x720)
#   AVSYNC_RECORD_FPS  recording fps (default: the app's)
#   AVSYNC_EXTRA_ARGS  extra app arguments (word-split), e.g.
#                      "--frame-gen 2x --frame-gen-method motion"
#   AVSYNC_MAX_MEAN_MS / AVSYNC_MAX_ABS_MS / AVSYNC_MAX_DRIFT  thresholds
#                      (defaults 40 ms / 80 ms / 5 ms per min)
#   AVSYNC_MAX_GLITCHES  allowed frame-order glitches (default 0): extra flash
#                      onsets inside one second = frames encoded out of order
#   AVSYNC_REPORT_ONLY=1  print the numbers but always exit 0 (baselines)
#   AVSYNC_KEEP=1      keep the work dir (captured files) for inspection
#
# Needs: ffmpeg, ffprobe, curl, python3 + numpy, xvfb-run on Linux (runs
# headless and isolated like tools/smoke-test.sh). Exit 0 = in sync.

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BIN="${1:-$REPO_ROOT/build-linux-x86_64/bin/retrocapture}"
BIN="$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")"
SECONDS_TO_RUN="${AVSYNC_SECONDS:-40}"
SKEW="${AVSYNC_SKEW_PPM:-1000}"
PORT="${AVSYNC_PORT:-8091}"
FPS="${AVSYNC_FPS:-60}"
STREAM_FPS="${AVSYNC_STREAM_FPS:-$FPS}"
read -r -a EXTRA_ARGS <<< "${AVSYNC_EXTRA_ARGS:-}"
FRAGMENT_MS="${AVSYNC_FRAGMENT_MS:-20}"
W="${AVSYNC_WIDTH:-1280}"; H="${AVSYNC_HEIGHT:-720}"
WORK="$(mktemp -d /tmp/rc-avsync.XXXXXX)"
APP_PID=""
CURL_PID=""

# Stop the app under test. Under xvfb-run the app is a grandchild: killing
# only xvfb-run orphaned it, and the orphan kept serving the port, so the
# next launch silently tested the OLD process (#214). The launch runs in its
# own session (setsid), so we can signal exactly the app, and kill the whole
# session as a last resort.
stop_signal() {
    [ -n "$APP_PID" ] || return 0
    if [ "$USE_XVFB" = 1 ]; then
        pkill -TERM -s "$APP_PID" -x retrocapture 2>/dev/null || kill -TERM -- -"$APP_PID" 2>/dev/null
    else
        kill -TERM "$APP_PID" 2>/dev/null
    fi
}
force_kill() {
    [ -n "$APP_PID" ] || return 0
    kill -9 -- -"$APP_PID" 2>/dev/null || kill -9 "$APP_PID" 2>/dev/null
}

cleanup() {
    [ -n "$CURL_PID" ] && kill "$CURL_PID" 2>/dev/null
    stop_signal
    for _ in $(seq 1 20); do [ -n "$APP_PID" ] && kill -0 "$APP_PID" 2>/dev/null || break; sleep 0.5; done
    force_kill
    [ -n "$APP_PID" ] && wait "$APP_PID" 2>/dev/null
    if [ "${AVSYNC_KEEP:-0}" = 1 ]; then echo "   (kept $WORK)"; else rm -rf "$WORK"; fi
}
trap cleanup EXIT

fail() { echo "❌ AV-SYNC FAIL: $*"; exit 1; }
note() { echo "   $*"; }

for tool in ffmpeg ffprobe curl python3; do
    command -v "$tool" >/dev/null 2>&1 || fail "missing required tool: $tool"
done
python3 -c "import numpy" 2>/dev/null || fail "python3 numpy is required"
[ -x "$BIN" ] || fail "binary not found/executable: $BIN"

# #240 — isolate the app under test from the user's desktop session. xvfb-run
# alone isn't enough: GLFW prefers Wayland when WAYLAND_DISPLAY is inherited
# (test windows popped up on the real desktop) and the tray registers on the
# session D-Bus. On Linux, always run headless in a private session unless
# RC_TEST_REAL_DISPLAY=1 asks for the real display (debugging).
LAUNCH_PREFIX=()
USE_XVFB=0
if [ "$(uname)" != "Darwin" ] && [ "${RC_TEST_REAL_DISPLAY:-0}" != 1 ]; then
    command -v xvfb-run >/dev/null 2>&1 || \
        fail "xvfb-run not installed (need a headless GL context; RC_TEST_REAL_DISPLAY=1 uses \$DISPLAY)"
    unset WAYLAND_DISPLAY XDG_SESSION_TYPE DISPLAY
    mkdir -p "$WORK/runtime" && chmod 700 "$WORK/runtime"
    export XDG_RUNTIME_DIR="$WORK/runtime"
    export DBUS_SESSION_BUS_ADDRESS="disabled:"
    # Explicit 24-bit screen: xvfb-run defaults to 640x480x8.
    LAUNCH_PREFIX=(setsid xvfb-run -a -s "-screen 0 1920x1080x24")
    USE_XVFB=1
fi

port_up() {
    local code
    code=$(curl -s -m 2 -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/" 2>/dev/null)
    [ -n "$code" ] && [ "$code" != "000" ]
}
port_up && fail "port $PORT is already serving — stop it first (or set AVSYNC_PORT)"

echo "▶ av-sync-test: $BIN"
note "${SECONDS_TO_RUN}s @ ${FPS} fps (stream ${STREAM_FPS}), fake audio clock skew ${SKEW} ppm, ${FRAGMENT_MS} ms fragments"
[ -n "${AVSYNC_EXTRA_ARGS:-}" ] && note "extra args: ${AVSYNC_EXTRA_ARGS}"

# Isolated XDG dirs + cwd so the recording (relative output path) lands here.
mkdir -p "$WORK/cfg" "$WORK/data" "$WORK/run"
(
    cd "$WORK/run" && XDG_CONFIG_HOME="$WORK/cfg" XDG_DATA_HOME="$WORK/data" \
    RETROCAPTURE_TEST_PATTERN="${AVSYNC_PATTERN:-}" \
    exec ${LAUNCH_PREFIX[@]+"${LAUNCH_PREFIX[@]}"} "$BIN" --source test --audio-source test \
        --test-audio-skew-ppm "$SKEW" --test-audio-fragment-ms "$FRAGMENT_MS" --stream-enable --hide-ui \
        --stream-port "$PORT" --width "$W" --height "$H" --fps "$FPS" \
        --stream-fps "$STREAM_FPS" ${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}
) > "$WORK/app.log" 2>&1 &
APP_PID=$!

ready=0
for _ in $(seq 1 40); do
    kill -0 "$APP_PID" 2>/dev/null || { tail -30 "$WORK/app.log"; fail "app exited during startup"; }
    if port_up; then ready=1; break; fi
    sleep 0.5
done
[ "$ready" = 1 ] || { tail -30 "$WORK/app.log"; fail "stream port $PORT never came up"; }
grep -q "AudioCaptureTestTone opened" "$WORK/app.log" "$WORK"/data/*/retrocapture.log 2>/dev/null \
    || note "warning: could not confirm the test tone source in the logs"
sleep 2

# /stream client (also wakes the /stream encoder, which idles without one).
curl -s --max-time $((SECONDS_TO_RUN + 4)) -o "$WORK/stream.ts" "http://127.0.0.1:$PORT/stream" &
CURL_PID=$!
sleep 1

# Record into the work dir, not the user's Videos folder.
mkdir -p "$WORK/rec"
curl -s -m 5 -X POST -H 'Content-Type: application/json' \
     -d "{\"outputPath\":\"$WORK/rec/\"${AVSYNC_RECORD_FPS:+,\"fps\":$AVSYNC_RECORD_FPS}}" "http://127.0.0.1:$PORT/api/v1/recording/settings" > /dev/null \
    || fail "could not set the recording output path"
sleep 0.5
curl -s -m 5 -X POST -H 'Content-Type: application/json' \
     -d '{"action":"start"}' "http://127.0.0.1:$PORT/api/v1/recording/control" > "$WORK/rec-start.json" \
    || fail "could not start recording via the API"
sleep "$SECONDS_TO_RUN"
curl -s -m 10 -X POST -H 'Content-Type: application/json' \
     -d '{"action":"stop"}' "http://127.0.0.1:$PORT/api/v1/recording/control" > "$WORK/rec-stop.json"
wait "$CURL_PID" 2>/dev/null
CURL_PID=""
# Give the recorder time to write the trailer.
sleep 3

# SIGTERM runs the orderly shutdown; a hang there (e.g. an audio thread
# deadlock) is a failure in its own right.
stop_signal
for _ in $(seq 1 30); do kill -0 "$APP_PID" 2>/dev/null || break; sleep 0.5; done
if kill -0 "$APP_PID" 2>/dev/null; then
    force_kill; wait "$APP_PID" 2>/dev/null; APP_PID=""
    fail "app did not shut down within 15 s of SIGTERM"
fi
wait "$APP_PID" 2>/dev/null; APP_PID=""

REC=$(find "$WORK/rec" -type f \( -name '*.mp4' -o -name '*.mkv' -o -name '*.webm' \) \
          -newer "$WORK/rec-start.json" 2>/dev/null | head -1)

TH=(--max-mean-ms "${AVSYNC_MAX_MEAN_MS:-40}" --max-abs-ms "${AVSYNC_MAX_ABS_MS:-80}"
    --max-drift-ms-per-min "${AVSYNC_MAX_DRIFT:-5}" --max-glitches "${AVSYNC_MAX_GLITCHES:-0}")
rc=0
echo "── results (offset = audio − video; positive = audio late)"
if [ -s "$WORK/stream.ts" ]; then
    python3 "$SCRIPT_DIR/av-sync-analyze.py" "$WORK/stream.ts" "/stream  " "${TH[@]}" || rc=1
else
    echo "   [X]  /stream: no data captured"; rc=1
fi
if [ -n "$REC" ] && [ -s "$REC" ]; then
    python3 "$SCRIPT_DIR/av-sync-analyze.py" "$REC" "recording" "${TH[@]}" || rc=1
else
    echo "   [X]  recording: no file produced"; rc=1
fi

drops=$(grep -ho "dropped \(frame\|chunk\) (total: [0-9]*)" "$WORK/app.log" "$WORK"/data/*/retrocapture.log 2>/dev/null | tail -2 | tr '\n' ' ')
[ -n "$drops" ] && note "synchronizer drops: $drops"

if [ "${AVSYNC_REPORT_ONLY:-0}" = 1 ]; then
    echo "ℹ report-only mode (exit 0)"; exit 0
fi
[ "$rc" = 0 ] && echo "✅ AV-SYNC PASS" || echo "❌ AV-SYNC FAIL"
exit "$rc"
