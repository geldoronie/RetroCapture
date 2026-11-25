#!/bin/bash
# Frame generation quality + cost (#247).
#
# For each input clip (60 fps), keeps every STEP-th frame, regenerates the
# frames in between with the app's GPU interpolator (`--framegen-bench`) and
# scores every regenerated frame against the real one it replaces:
#   repeat  — the nearest real frame (what a plain rate conversion shows)
#   blend   — 50/50 (linear) blend of the two neighbours
#   motion  — the optical-flow interpolator (frame generation "Motion")
#   motion_nopick — same, without the edge-aware vector pick (ablation)
# PSNR and SSIM come from ffmpeg's psnr / ssim filters (Y'CbCr, all planes).
# The bench also prints the GPU time of each stage (glFinish-timed).
#
# Inputs (FG_INPUTS, space separated):
#   motion    the "motion" test pattern: scrolling textured background,
#             sprites on curved paths, static HUD (rendered offline at exact
#             60 fps instants)
#   pan       a real game screenshot (docs/sonic-no-shaders-mattias.png),
#             1.5x pixel-art upscale, panned 5 px/frame right + 2 px/frame
#             down (a camera scroll over real pixels)
#   testsrc2  ffmpeg's testsrc2 pattern
#   v4l2      FG_V4L2_DEVICE (read-only capture of a live device, e.g. a
#             v4l2loopback virtual camera), FG_V4L2_SIZE
#
# Usage:  tools/framegen-quality-test.sh [path-to-retrocapture-binary]
# Env:    FG_SIZE (default 1280x720), FG_FRAMES (61), FG_STEP (2),
#         FG_INPUTS (default "motion pan testsrc2"), FG_KEEP=1 keep work dir,
#         FG_PORT (stream port the app is told to use, never opened; 8197)
# Needs:  ffmpeg, xvfb-run (Linux).

set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BIN="${1:-$REPO_ROOT/build-linux-x86_64/bin/retrocapture}"
SIZE="${FG_SIZE:-1280x720}"
FRAMES="${FG_FRAMES:-61}"
STEP="${FG_STEP:-2}"
INPUTS="${FG_INPUTS:-motion pan testsrc2}"
PORT="${FG_PORT:-8197}"
W="${SIZE%x*}"; H="${SIZE#*x}"
WORK="$(mktemp -d /tmp/rc-framegen.XXXXXX)"
cleanup() { if [ "${FG_KEEP:-0}" = 1 ]; then echo "(kept $WORK)"; else rm -rf "$WORK"; fi; }
trap cleanup EXIT
fail() { echo "FRAMEGEN-QUALITY FAIL: $*"; exit 1; }

[ -x "$BIN" ] || fail "binary not found: $BIN"
command -v ffmpeg >/dev/null || fail "ffmpeg is required"
# Same isolation as the smoke / av-sync tests (#240): headless private
# session unless RC_TEST_REAL_DISPLAY=1.
LAUNCH=()
if [ "$(uname)" != "Darwin" ] && [ "${RC_TEST_REAL_DISPLAY:-0}" != 1 ]; then
    command -v xvfb-run >/dev/null || fail "xvfb-run not installed"
    unset WAYLAND_DISPLAY XDG_SESSION_TYPE DISPLAY
    mkdir -p "$WORK/runtime" && chmod 700 "$WORK/runtime"
    export XDG_RUNTIME_DIR="$WORK/runtime"
    export DBUS_SESSION_BUS_ADDRESS="disabled:"
    LAUNCH=(xvfb-run -a -s "-screen 0 1920x1080x24")
fi

make_input() { # name -> $WORK/<name>/in.rgb (or "motion" = synthetic)
    local name="$1" dir="$WORK/$1"
    mkdir -p "$dir"
    case "$name" in
        motion) echo "motion" ;;
        pan)
            ffmpeg -v error -y -loop 1 -framerate 60 -i "$REPO_ROOT/docs/sonic-no-shaders-mattias.png" \
                -vf "scale=-2:$((H * 3 / 2)):flags=neighbor,crop=$W:$H:x='n*5':y='$((H / 16))+n*2'" \
                -frames:v "$FRAMES" -pix_fmt rgb24 -f rawvideo "$dir/in.rgb" || return 1
            echo "$dir/in.rgb" ;;
        testsrc2)
            ffmpeg -v error -y -f lavfi -i "testsrc2=size=${W}x${H}:rate=60" -frames:v "$FRAMES" \
                -pix_fmt rgb24 -f rawvideo "$dir/in.rgb" || return 1
            echo "$dir/in.rgb" ;;
        v4l2)
            local dev="${FG_V4L2_DEVICE:-/dev/video2}" vsize="${FG_V4L2_SIZE:-1920x1080}"
            ffmpeg -v error -y -f v4l2 -video_size "$vsize" -i "$dev" -frames:v "$FRAMES" \
                -vf "scale=$W:$H" -pix_fmt rgb24 -f rawvideo "$dir/in.rgb" || return 1
            echo "$dir/in.rgb" ;;
        *) return 1 ;;
    esac
}

score() { # ref.rgb test.rgb -> "psnr ssim"
    ffmpeg -hide_banner -nostats -f rawvideo -pix_fmt rgb24 -s "$SIZE" -i "$1" \
        -f rawvideo -pix_fmt rgb24 -s "$SIZE" -i "$2" \
        -lavfi "[1:v]split[a][b];[0:v]split[c][d];[a][c]ssim;[b][d]psnr" -f null - 2>&1 |
        awk '/SSIM/ { for (i=1;i<=NF;i++) if ($i ~ /^All:/) { sub("All:","",$i); s=$i } }
             /PSNR/ { for (i=1;i<=NF;i++) if ($i ~ /^average:/) { sub("average:","",$i); p=$i } }
             END { printf "%s %s\n", p, s }'
}

echo "framegen-quality: $BIN"
echo "  ${SIZE}, ${FRAMES} frames @ 60 fps, keep 1 of ${STEP} (regenerate $((STEP - 1)) per pair)"
printf "  %-9s %-14s %10s %8s\n" input method "PSNR(dB)" SSIM
for name in $INPUTS; do
    src="$(make_input "$name")" || { echo "  $name: could not produce input, skipped"; continue; }
    dir="$WORK/$name"
    mkdir -p "$dir/cfg" "$dir/data" "$dir/run"
    (cd "$dir/run" && XDG_CONFIG_HOME="$dir/cfg" XDG_DATA_HOME="$dir/data" \
        ${LAUNCH[@]+"${LAUNCH[@]}"} "$BIN" --source test --hide-ui --stream-port "$PORT" \
        --framegen-bench "in=$src,size=$SIZE,frames=$FRAMES,step=$STEP,out=$dir") > "$dir/bench.log" 2>&1 \
        || { tail -20 "$dir/bench.log"; fail "bench failed on $name"; }
    for m in repeat blend motion motion_nopick; do
        read -r psnr ssim < <(score "$dir/ref.rgb" "$dir/$m.rgb")
        printf "  %-9s %-14s %10s %8s\n" "$name" "$m" "$psnr" "$ssim"
    done
    grep '^BENCH' "$dir/bench.log" | sed 's/^/    /'
done
