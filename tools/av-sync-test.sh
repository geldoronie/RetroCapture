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
#   AVSYNC_MAX_MEAN_MS / AVSYNC_MAX_ABS_MS / AVSYNC_MAX_DRIFT  thresholds
#                      (defaults 40 ms / 80 ms / 5 ms per min)
#   AVSYNC_MAX_GLITCHES  allowed frame-order glitches (default 0): extra flash
#                      onsets inside one second = frames encoded out of order
#   AVSYNC_REPORT_ONLY=1  print the numbers but always exit 0 (baselines)
#   AVSYNC_KEEP=1      keep the work dir (captured files) for inspection
#
# Needs: ffmpeg, ffprobe, curl, python3 + numpy, and a display ($DISPLAY or
# xvfb-run), same as tools/smoke-test.sh. Exit 0 = in sync.

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BIN="${1:-$REPO_ROOT/build-linux-x86_64/bin/retrocapture}"
BIN="$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")"
SECONDS_TO_RUN="${AVSYNC_SECONDS:-40}"
SKEW="${AVSYNC_SKEW_PPM:-1000}"
PORT="${AVSYNC_PORT:-8091}"
FPS="${AVSYNC_FPS:-60}"
FRAGMENT_MS="${AVSYNC_FRAGMENT_MS:-20}"
W=1280; H=720
WORK="$(mktemp -d /tmp/rc-avsync.XXXXXX)"
APP_PID=""
CURL_PID=""

cleanup() {
    [ -n "$CURL_PID" ] && kill "$CURL_PID" 2>/dev/null
    [ -n "$APP_PID" ] && kill "$APP_PID" 2>/dev/null
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

LAUNCH_PREFIX=""
if [ -z "${DISPLAY:-}" ]; then
    command -v xvfb-run >/dev/null 2>&1 || fail "no DISPLAY and xvfb-run not installed"
    LAUNCH_PREFIX="xvfb-run -a"
fi

port_up() {
    local code
    code=$(curl -s -m 2 -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/" 2>/dev/null)
    [ -n "$code" ] && [ "$code" != "000" ]
}
port_up && fail "port $PORT is already serving — stop it first (or set AVSYNC_PORT)"

echo "▶ av-sync-test: $BIN"
note "${SECONDS_TO_RUN}s @ ${FPS} fps, fake audio clock skew ${SKEW} ppm, ${FRAGMENT_MS} ms fragments"

# Isolated XDG dirs + cwd so the recording (relative output path) lands here.
mkdir -p "$WORK/cfg" "$WORK/data" "$WORK/run"
(
    cd "$WORK/run" && XDG_CONFIG_HOME="$WORK/cfg" XDG_DATA_HOME="$WORK/data" \
    exec $LAUNCH_PREFIX "$BIN" --source test --audio-source test \
        --test-audio-skew-ppm "$SKEW" --test-audio-fragment-ms "$FRAGMENT_MS" --stream-enable --hide-ui \
        --stream-port "$PORT" --width "$W" --height "$H" --fps "$FPS" \
        --stream-fps "$FPS"
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
timeout $((SECONDS_TO_RUN + 4)) curl -s -o "$WORK/stream.ts" "http://127.0.0.1:$PORT/stream" &
CURL_PID=$!
sleep 1

# Record into the work dir, not the user's Videos folder.
mkdir -p "$WORK/rec"
curl -s -m 5 -X POST -H 'Content-Type: application/json' \
     -d "{\"outputPath\":\"$WORK/rec/\"}" "http://127.0.0.1:$PORT/api/v1/recording/settings" > /dev/null \
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
kill "$APP_PID" 2>/dev/null
for _ in $(seq 1 30); do kill -0 "$APP_PID" 2>/dev/null || break; sleep 0.5; done
if kill -0 "$APP_PID" 2>/dev/null; then
    kill -9 "$APP_PID" 2>/dev/null; wait "$APP_PID" 2>/dev/null; APP_PID=""
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
