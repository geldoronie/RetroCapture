#!/usr/bin/env python3
"""A/V sync analyzer for tools/av-sync-test.sh (#196).

Reads a media file produced from the synthetic sync sources
(`--source test --audio-source test`): the video's bottom band flashes
white and the audio beeps (1 kHz) during the first 100 ms of every whole
second of the capture clock. For every flash onset it finds the nearest
beep onset and reports the offset (audio - video, positive = audio late).

Usage: av-sync-analyze.py <media-file> <label> [--max-mean-ms N]
       [--max-abs-ms N] [--max-drift-ms-per-min N] [--json out.json]
Exit 0 = within thresholds, 1 = out of thresholds, 2 = could not analyze.
"""
import argparse
import json
import subprocess
import sys

import numpy as np

AUDIO_RATE = 48000


def ffprobe_json(path, *args):
    out = subprocess.run(["ffprobe", "-v", "error", "-of", "json", *args, path],
                         capture_output=True, text=True, check=True).stdout
    return json.loads(out)


def video_onsets(path):
    info = ffprobe_json(path, "-select_streams", "v:0", "-show_entries", "stream=width,height")
    st = info["streams"][0]
    w, h = int(st["width"]), int(st["height"])
    # Presentation time of every decoded frame, in decode-output order.
    frames = ffprobe_json(path, "-select_streams", "v:0", "-show_entries",
                          "frame=best_effort_timestamp_time")["frames"]
    pts = np.array([float(f["best_effort_timestamp_time"]) for f in frames
                    if f.get("best_effort_timestamp_time") not in (None, "N/A")])
    band_h = max(2, h // 16)
    # Mean luma of a strip inside the flash band (bottom 1/8 of the frame),
    # one value per decoded frame, no frame-rate conversion.
    raw = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", path, "-map", "0:v:0", "-fps_mode", "passthrough",
         "-vf", f"crop={w}:{band_h}:0:{h - band_h - 2},scale=16:1,format=gray",
         "-f", "rawvideo", "-"],
        capture_output=True, check=True).stdout
    luma = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 16).mean(axis=1)
    n = min(len(luma), len(pts))
    luma, pts = luma[:n], pts[:n]
    bright = luma > 128
    raw_onsets = [pts[i] for i in range(1, n) if bright[i] and not bright[i - 1]]
    # One flash per second. Extra onsets inside a second mean flash and
    # non-flash frames were interleaved, i.e. frames came out of capture
    # order (stale frames encoded after newer ones) — a visible stutter.
    onsets, glitches = [], 0
    for t in raw_onsets:
        if onsets and t - onsets[-1] < 0.5:
            glitches += 1
        else:
            onsets.append(t)
    # Frames whose presentation time goes backwards / repeats.
    non_monotonic = int((np.diff(pts) <= 0).sum()) if n > 1 else 0
    return np.array(onsets), n, pts, glitches, non_monotonic


def audio_onsets(path):
    info = ffprobe_json(path, "-select_streams", "a:0", "-show_entries", "stream=start_time")
    if not info.get("streams"):
        return None
    start = float(info["streams"][0].get("start_time") or 0.0)
    # aresample=async pads/trims to the stream timestamps, so sample i is at
    # start + i / rate even across gaps in the audio timeline.
    raw = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", path, "-map", "0:a:0",
         "-af", f"aresample={AUDIO_RATE}:async=1:first_pts=0", "-ac", "1",
         "-f", "s16le", "-"],
        capture_output=True, check=True).stdout
    pcm = np.abs(np.frombuffer(raw, dtype=np.int16).astype(np.float32))
    if pcm.size == 0:
        return np.array([])
    # 1 ms envelope.
    win = AUDIO_RATE // 1000
    m = pcm.size // win
    env = pcm[: m * win].reshape(m, win).max(axis=1)
    loud = env > 0.15 * 32767
    onsets = []
    quiet_run = 1000
    for i in range(m):
        if loud[i]:
            if quiet_run >= 300:  # >= 300 ms of silence before an onset
                onsets.append(start + i / 1000.0)
            quiet_run = 0
        else:
            quiet_run += 1
    return np.array(onsets)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument("label")
    ap.add_argument("--max-mean-ms", type=float, default=40.0)
    ap.add_argument("--max-abs-ms", type=float, default=80.0)
    ap.add_argument("--max-drift-ms-per-min", type=float, default=5.0)
    ap.add_argument("--max-glitches", type=int, default=0)
    ap.add_argument("--json")
    a = ap.parse_args()

    try:
        v, nframes, vpts, glitches, non_mono = video_onsets(a.path)
        au = audio_onsets(a.path)
    except subprocess.CalledProcessError as e:
        print(f"   [X]  {a.label}: could not decode ({e})")
        return 2
    if au is None:
        print(f"   [X]  {a.label}: no audio stream")
        return 2
    if len(v) < 5 or len(au) < 5:
        print(f"   [X]  {a.label}: too few onsets (flash={len(v)} beep={len(au)})")
        return 2

    pairs = []
    for t in v:
        j = int(np.argmin(np.abs(au - t)))
        d = au[j] - t
        if abs(d) < 0.5:
            pairs.append((t, d * 1000.0))
    if len(pairs) < 5:
        print(f"   [X]  {a.label}: could not pair flashes with beeps")
        return 2
    t = np.array([p[0] for p in pairs])
    off = np.array([p[1] for p in pairs])
    slope_ms_per_s = np.polyfit(t, off, 1)[0] if len(t) > 2 else 0.0
    drift = slope_ms_per_s * 60.0
    duration = float(vpts[-1] - vpts[0]) if len(vpts) > 1 else 0.0
    fps = (nframes - 1) / duration if duration > 0 else 0.0
    res = {
        "label": a.label,
        "pairs": len(pairs),
        "duration_s": round(duration, 2),
        "video_fps": round(fps, 2),
        "mean_ms": round(float(off.mean()), 1),
        "first_ms": round(float(off[0]), 1),
        "last_ms": round(float(off[-1]), 1),
        "max_abs_ms": round(float(np.abs(off).max()), 1),
        "stdev_ms": round(float(off.std()), 1),
        "drift_ms_per_min": round(float(drift), 1),
        "order_glitches": glitches,
        "non_monotonic_pts": non_mono,
    }
    ok = (abs(res["mean_ms"]) <= a.max_mean_ms and res["max_abs_ms"] <= a.max_abs_ms
          and abs(res["drift_ms_per_min"]) <= a.max_drift_ms_per_min
          and glitches <= a.max_glitches)
    print(("   [ok] " if ok else "   [X]  ") +
          f"{a.label}: offset mean {res['mean_ms']:+.1f} ms (first {res['first_ms']:+.1f}, "
          f"last {res['last_ms']:+.1f}, max|{res['max_abs_ms']:.1f}|, sd {res['stdev_ms']:.1f}), "
          f"drift {res['drift_ms_per_min']:+.1f} ms/min, {res['pairs']} pairs, "
          f"{res['duration_s']} s @ {res['video_fps']} fps, "
          f"{glitches} frame-order glitches")
    if a.json:
        with open(a.json, "w") as f:
            json.dump(res, f)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
