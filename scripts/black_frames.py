"""Count black / near-black frames in a screen recording (whole-screen dropouts).

    python scripts/black_frames.py capture.mp4 [--thresh 12]

A frame counts as "black" when the mean brightness of the area below the top 12% (the HUD
overlays live there) is under --thresh (0-255) while the recording's median frame is clearly
brighter. Prints counts, the longest black run in frames, and the first few indices.
"""
import argparse
import cv2
import numpy as np


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("video")
    ap.add_argument("--thresh", type=float, default=12)
    a = ap.parse_args()
    cap = cv2.VideoCapture(a.video)
    means = []
    while True:
        ok, f = cap.read()
        if not ok:
            break
        g = cv2.cvtColor(f, cv2.COLOR_BGR2GRAY)
        h = g.shape[0]
        means.append(float(g[int(h * 0.12):].mean()))
    m = np.array(means)
    if len(m) == 0:
        raise SystemExit("no frames")
    black = m < a.thresh
    runs, cur = [], 0
    for b in black:
        cur = cur + 1 if b else 0
        if cur:
            if cur == 1:
                runs.append(1)
            else:
                runs[-1] = cur
    # transitions from non-black to black = number of separate dropouts
    drops = int(np.sum(black[1:] & ~black[:-1]) + (1 if black[0] else 0))
    print(f"frames={len(m)} median_brightness={np.median(m):.1f} black_frames={int(black.sum())} "
          f"dropouts={drops} longest_run={max(runs) if runs else 0} "
          f"first={list(np.where(black)[0][:10])}")


if __name__ == "__main__":
    main()
