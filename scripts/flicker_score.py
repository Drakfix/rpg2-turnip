"""Score temporal flicker in a screen recording of a (mostly static) game scene.

    python scripts/flicker_score.py capture.mp4 [--thresh 28] [--save-map map.png]

Flicker = a pixel that jumps away from its value and comes straight back on the next
frame (A -> B -> A). Normal motion (wind, particles, an enemy moving) mostly does not
return within one frame, so those spikes are a fair flicker signature. Duplicate frames
(the game runs at 30 FPS, the recorder at up to 60) are dropped first.

Prints spikes per frame for the whole picture and for the lower half (where the terrain
is at the BOTW benchmark spot). --save-map writes a heat map of where spikes happened.
"""
import argparse
import cv2
import numpy as np


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("video")
    ap.add_argument("--thresh", type=int, default=28, help="per-pixel jump (0-255) that counts")
    ap.add_argument("--scale", type=int, default=4, help="downscale factor before analysis")
    ap.add_argument("--save-map")
    a = ap.parse_args()

    cap = cv2.VideoCapture(a.video)
    frames = []
    prev = None
    while True:
        ok, f = cap.read()
        if not ok:
            break
        g = cv2.cvtColor(f, cv2.COLOR_BGR2GRAY)
        g = cv2.resize(g, (g.shape[1] // a.scale, g.shape[0] // a.scale), interpolation=cv2.INTER_AREA)
        g = g.astype(np.int16)
        if prev is not None and np.mean(np.abs(g - prev)) < 0.3:
            continue  # duplicate frame
        frames.append(g)
        prev = g
    if len(frames) < 3:
        raise SystemExit(f"only {len(frames)} distinct frames")

    h, w = frames[0].shape
    top = int(h * 0.12)          # skip the FPS/HUD overlay at the top
    heat = np.zeros((h, w), np.float64)
    total = lower = 0
    t = a.thresh
    for f0, f1, f2 in zip(frames, frames[1:], frames[2:]):
        d01 = f1 - f0
        d12 = f2 - f1
        spike = (np.abs(d01) > t) & (np.abs(d12) > t) & (np.sign(d01) != np.sign(d12)) \
                & (np.abs(f2 - f0) < t // 2)
        spike[:top] = False
        heat += spike
        total += int(spike.sum())
        lower += int(spike[h // 2:].sum())
    n = len(frames) - 2

    # Block-level one-frame flashes: 16x16-pixel blocks (at full 1080p) whose mean
    # brightness jumps and returns. Particles touch a few blocks; a surface rendered
    # wrong for one frame (the BOTW terrain flicker) lights up hundreds at once.
    bw, bh = 120, 68
    blocks = [cv2.resize(f.astype(np.float32), (bw, bh), interpolation=cv2.INTER_AREA) for f in frames]
    flashes = []
    for k in range(1, len(blocks) - 1):
        d1 = blocks[k] - blocks[k - 1]
        d2 = blocks[k + 1] - blocks[k]
        m = (np.abs(d1) > 6) & (np.abs(d2) > 6) & (np.sign(d1) != np.sign(d2)) \
            & (np.abs(blocks[k + 1] - blocks[k - 1]) < 3)
        m[: int(bh * 0.12)] = False
        flashes.append(int(m.sum()))
    big = sum(1 for c in flashes if c >= 100)
    print(f"frames={len(frames)} spikes/frame={total / n:.1f} lower_half={lower / n:.1f} "
          f"flash_events(>=100 blocks)={big} max_flash_blocks={max(flashes)}")
    if a.save_map:
        m = (255 * heat / max(heat.max(), 1)).astype(np.uint8)
        cv2.imwrite(a.save_map, cv2.applyColorMap(m, cv2.COLORMAP_INFERNO))


if __name__ == "__main__":
    main()
