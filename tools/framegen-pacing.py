#!/usr/bin/env python3
"""Output rate and frame pacing of a recorded / streamed file (#247).

Reads the video presentation timestamps with ffprobe and prints the frame
count, the average rate, and the frame-interval statistics (mean, standard
deviation, min / max, and how many intervals are off the median by more than
25 %). With frame generation on, a 30 fps source at 2x must read ~60 fps with
an even interval (small deviation, no off-median intervals).

Usage: tools/framegen-pacing.py <file> [label] [--skip-seconds S]
"""
import argparse
import json
import statistics
import subprocess
import sys


def main():
    ap = argparse.ArgumentParser(description='Output rate and frame pacing (#247).')
    ap.add_argument('file')
    ap.add_argument('label', nargs='?')
    ap.add_argument('--skip-seconds', type=float, default=1.0,
                    help='ignore the first S seconds (start-up), default 1')
    a = ap.parse_args()
    path, label, skip = a.file, a.label or a.file, a.skip_seconds
    out = subprocess.run(
        ['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_entries',
         'packet=pts_time', '-of', 'json', path],
        capture_output=True, text=True, check=False)
    try:
        pkts = json.loads(out.stdout).get('packets', [])
    except json.JSONDecodeError:
        pkts = []
    pts = sorted(float(p['pts_time']) for p in pkts if p.get('pts_time') not in (None, 'N/A'))
    if len(pts) < 3:
        print(f'  [X]  {label}: not enough video frames ({len(pts)})')
        return 1
    t0 = pts[0] + skip
    pts = [t for t in pts if t >= t0]
    iv = [(b - a) * 1000.0 for a, b in zip(pts, pts[1:])]
    dur = pts[-1] - pts[0]
    med = statistics.median(iv)
    off = sum(1 for x in iv if abs(x - med) > 0.25 * med)
    print(f'  {label}: {len(pts)} frames in {dur:.2f} s = {(len(pts) - 1) / dur:.2f} fps; '
          f'interval {statistics.mean(iv):.3f} +- {statistics.pstdev(iv):.3f} ms '
          f'[{min(iv):.2f} .. {max(iv):.2f}], median {med:.3f} ms, '
          f'{off} interval(s) off the median by >25 %')
    return 0


if __name__ == '__main__':
    sys.exit(main())
