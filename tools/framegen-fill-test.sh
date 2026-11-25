#!/usr/bin/env bash
# Frame generation "fill" test (#256): repeat detection, the capture-rate fill
# mode, the learnt game cadence and scene-cut detection, end to end through
# the app (test pattern -> FrameGenStage -> outputs), with the cheap Blend
# method so it runs on a software GL (llvmpipe) in CI.
#
# Phases (each a fresh app instance, 60 fps capture, 640x480):
#   game30  'motion' pattern drawing 30 pictures/s with periodic pauses:
#           game rate ~30, capture ~60, repeats skipped, fill output near
#           the capture rate, cadence ~33 ms.
#   game60  every capture a new picture: no repeats, pass-through (no
#           added latency).
#   cuts    inverted-colour scene cut every second: cuts counted, pairs
#           across them not interpolated.
#
# Usage:  tools/framegen-fill-test.sh [path-to-retrocapture-binary]
# Env:    FILL_PORT (default 8096), FILL_KEEP=1 keeps the work dir,
#         RC_TEST_REAL_DISPLAY=1 uses the real display (debugging).
set -u

BIN="${1:-$(cd "$(dirname "$0")/.." && pwd)/build-linux-x86_64/bin/retrocapture}"
PORT="${FILL_PORT:-8096}"
WORK="$(mktemp -d /tmp/rc-fill.XXXXXX)"
APP_PID=""
FAILS=0

stop_app() {
    [ -n "$APP_PID" ] || return 0
    if [ "$USE_XVFB" = 1 ]; then
        pkill -TERM -s "$APP_PID" -x retrocapture 2>/dev/null || kill -TERM -- -"$APP_PID" 2>/dev/null
    else
        kill -TERM "$APP_PID" 2>/dev/null
    fi
    for _ in $(seq 1 20); do kill -0 "$APP_PID" 2>/dev/null || break; sleep 0.5; done
    kill -9 -- -"$APP_PID" 2>/dev/null || kill -9 "$APP_PID" 2>/dev/null
    wait "$APP_PID" 2>/dev/null
    APP_PID=""
}
cleanup() {
    stop_app
    # Only ever remove the mktemp directory this script created.
    case "$WORK" in
        /tmp/rc-fill.*)
            if [ "${FILL_KEEP:-0}" = 1 ]; then echo "   (kept $WORK)"; else rm -rf "$WORK"; fi ;;
    esac
}
trap cleanup EXIT

fail() { echo "❌ FILL FAIL: $*"; exit 1; }
check() { # check <label> <python condition on the vars> <values...>
    if python3 -c "$2"; then echo "   [ok] $1"; else echo "   [X]  $1"; FAILS=$((FAILS + 1)); fi
}

for tool in curl python3; do
    command -v "$tool" >/dev/null 2>&1 || fail "missing required tool: $tool"
done
[ -x "$BIN" ] || fail "binary not found/executable: $BIN"

# Headless, isolated from the desktop session (#240), as smoke-test.sh.
LAUNCH_PREFIX=()
USE_XVFB=0
if [ "$(uname)" != "Darwin" ] && [ "${RC_TEST_REAL_DISPLAY:-0}" != 1 ]; then
    command -v xvfb-run >/dev/null 2>&1 || fail "xvfb-run not installed"
    unset WAYLAND_DISPLAY XDG_SESSION_TYPE DISPLAY
    mkdir -p "$WORK/runtime" && chmod 700 "$WORK/runtime"
    export XDG_RUNTIME_DIR="$WORK/runtime"
    export DBUS_SESSION_BUS_ADDRESS="disabled:"
    LAUNCH_PREFIX=(setsid xvfb-run -a -s "-screen 0 1280x720x24")
    USE_XVFB=1
fi

port_up() {
    local code
    code=$(curl -s -m 2 -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/" 2>/dev/null)
    [ -n "$code" ] && [ "$code" != "000" ]
}
port_up && fail "port $PORT is already serving — stop the running RetroCapture first (or set FILL_PORT)"

# run_phase <name> <seconds> <env assignments...> — boots the app in fill mode
# with the given test-pattern env, lets it run, saves the frame-generation
# status JSON to $WORK/<name>.json and the log to $WORK/<name>.log.
run_phase() {
    local name="$1" secs="$2"; shift 2
    mkdir -p "$WORK/$name/cfg" "$WORK/$name/data"
    env "$@" RETROCAPTURE_TEST_PATTERN=motion \
        XDG_CONFIG_HOME="$WORK/$name/cfg" XDG_DATA_HOME="$WORK/$name/data" \
        ${LAUNCH_PREFIX[@]+"${LAUNCH_PREFIX[@]}"} "$BIN" --source test --hide-ui --stream-enable \
        --stream-port "$PORT" --width 640 --height 480 --fps 60 --stream-fps 60 \
        --window-width 640 --window-height 480 --frame-gen fill --frame-gen-method blend \
        > "$WORK/$name.log" 2>&1 &
    APP_PID=$!
    local up=0
    for _ in $(seq 1 40); do
        kill -0 "$APP_PID" 2>/dev/null || { tail -20 "$WORK/$name.log"; fail "app exited during startup ($name)"; }
        port_up && { up=1; break; }
        sleep 0.5
    done
    [ "$up" = 1 ] || fail "port $PORT never came up ($name)"
    sleep "$secs"
    curl -s -m 5 "http://127.0.0.1:$PORT/api/v1/frame-generation" > "$WORK/$name.json"
    stop_app
}

# Last 10 s window of the stage's own log line: output fps, added latency, cadence.
last_window() { # last_window <log> -> "fps added cadence"
    grep -h "Frame generation: [0-9]* frames in" "$1" | tail -1 | python3 -c '
import re,sys
l=sys.stdin.read()
fps=re.search(r"\(([\d.]+) fps\)",l); add=re.search(r"added ([-\d.]+) ms",l); cad=re.search(r"game cadence ([\d.]+) ms",l)
print(fps.group(1) if fps else -1, add.group(1) if add else -1, cad.group(1) if cad else -1)'
}

echo "▶ framegen-fill-test: $BIN (port $PORT)"

echo "── game30: 30 fps game on a 60 fps capture, with pauses"
run_phase game30 22 RETROCAPTURE_TEST_GAME_FPS=30 RETROCAPTURE_TEST_PAUSE=1.5,0.5
read -r FPS ADDED CAD < <(last_window "$WORK/game30.log")
S="import json; s=json.load(open('$WORK/game30.json'))['status']"
check "game rate ~30 fps (sourceFps)"     "$S; v=s['sourceFps']; print('      sourceFps', v); assert 26 <= v <= 34"
check "capture ~60 fps (captureFps)"      "$S; assert 55 <= s['captureFps'] <= 65, s['captureFps']"
check "repeats skipped"                   "$S; assert s['repeatsSkipped'] > 100, s['repeatsSkipped']"
check "fill output >= 45 fps (got $FPS)"  "assert float('$FPS') >= 45"
check "cadence ~33 ms (got $CAD)"         "assert 28 <= float('$CAD') <= 38"
check "added latency < 40 ms (got $ADDED)" "assert float('$ADDED') < 40"
check "no false scene cuts"               "$S; assert s['sceneCuts'] == 0, s['sceneCuts']"

echo "── game60: every capture a new picture"
run_phase game60 14
read -r FPS ADDED CAD < <(last_window "$WORK/game60.log")
S="import json; s=json.load(open('$WORK/game60.json'))['status']"
check "no repeats skipped"                 "$S; assert s['repeatsSkipped'] <= 5, s['repeatsSkipped']"
check "pass-through: added < 8 ms (got $ADDED)" "assert float('$ADDED') < 8"
check "no false scene cuts"                "$S; assert s['sceneCuts'] == 0, s['sceneCuts']"

echo "── cuts: inverted-colour scene cut every second"
run_phase cuts 14 RETROCAPTURE_TEST_GAME_FPS=30 RETROCAPTURE_TEST_CUT=1
S="import json; s=json.load(open('$WORK/cuts.json'))['status']"
check "scene cuts detected (>= 8)"         "$S; assert s['sceneCuts'] >= 8, s['sceneCuts']"

if [ "$FAILS" -gt 0 ]; then
    echo "❌ FILL FAIL: $FAILS check(s) failed (FILL_KEEP=1 keeps $WORK)"
    exit 1
fi
echo "✅ FILL PASS"
