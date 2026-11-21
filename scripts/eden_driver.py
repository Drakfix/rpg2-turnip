"""Select the GPU driver Eden uses for one game, unattended, by reading the screen.

    python scripts/eden_driver.py --game smo --driver v5rc2
    python scripts/eden_driver.py --game botw --driver stock

Opens Eden, searches the game, long-presses its card (properties), opens the GPU driver
manager, then pages through the list using Windows OCR (scripts/win_ocr.ps1) to find the
driver's card and taps only its radio button. The radio button is verified by colour
afterwards (selected = bright lavender). Trash icons, the Fetch/Install buttons and
everything else are never tapped. Driver keys are regexes matched against card titles
(titles scroll sideways, so they only need a distinctive fragment).
"""
import argparse, os, re, subprocess, sys, time
from pathlib import Path

import cv2

sys.path.insert(0, str(Path(__file__).resolve().parent))
import eden_variant as ev  # noqa: E402  (shares adb helpers and game profiles)

ROOT = ev.ROOT
DRIVERS = {
    "stock": r"System GPU driver",
    "v4": r"A22 TUNED",          # the only one of ours without "(TUNED-"
    "v4x": r"v4x",
    "v5rc": r"v5rc\)",
    "v5rc2": r"v5rc2",
    "v5ub": r"v5ub",                              # v5 + patch 0005 (UBWC swapchain, opt-in prop)
    "t30": r"T30",
    # Third-party builds studied 2026-11-20 (credit: their authors; see docs/FINDINGS.md)
    "turnipz": r"TurnipZ",                        # AlexSanz0902, TurnipZ 0.4.0
    "steven_r2": r"27\.0\.[0O]-R2",               # StevenMXZ, Turnip v27.0.0-R2
    "balemuni": r"apex-v2",                       # Balemuni, Apex Universal V2
}
SAFE_Y = (230, 800)             # below the title bar, above the Fetch/Install buttons


def ocr(png):
    out = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                          str(ROOT / "scripts" / "win_ocr.ps1"), str(png)],
                         capture_output=True, text=True, encoding="utf-8", errors="replace").stdout or ""
    lines = []
    for l in out.splitlines():
        if "\t" in l:
            box, text = l.split("\t", 1)
            x, y, w, h = map(int, box.split())
            lines.append((x, y, w, h, text))
    return lines


def radio_selected(png, x, y):
    im = cv2.imread(str(png))
    b, g, r = im[y - 6:y + 7, x - 6:x + 7].mean(axis=(0, 1))
    return r > 180 and b > 150


ASSETS = Path(__file__).resolve().parent / "eden_assets"


def find_radios(png):
    """Centres of the driver cards' radio buttons (selected or not) on a screenshot."""
    import numpy as np
    im = cv2.imread(str(png))
    found = []
    for name in ("radio_off", "radio_on"):
        t = cv2.imread(str(ASSETS / f"{name}.png"))
        res = cv2.matchTemplate(im, t, cv2.TM_CCOEFF_NORMED)
        ys, xs = np.where(res > 0.85)
        for x, y in zip(xs, ys):
            cx, cy = int(x) + t.shape[1] // 2, int(y) + t.shape[0] // 2
            if all(abs(cx - fx) > 20 or abs(cy - fy) > 20 for fx, fy in found):
                found.append((cx, cy))
    return sorted(found, key=lambda p: (p[1], p[0]))


def card_texts(frames, radios):
    """Join the OCR text of every card over several frames (card titles scroll sideways,
    so any single frame may show only part of a name)."""
    texts = {r: "" for r in radios}
    for png in frames:
        for x, y, w, h, text in ocr(png):
            for rx, ry in radios:
                same_col = (x < 960) == (rx < 960)
                if same_col and ry - 100 <= y <= ry + 60:
                    texts[(rx, ry)] += " " + text
    return texts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", choices=sorted(ev.GAMES), required=True)
    ap.add_argument("--driver", choices=sorted(DRIVERS), required=True)
    a = ap.parse_args()
    g = ev.GAMES[a.game]
    ev.pick_device()
    sh, shot = ev.sh, ev.shot

    sh(f"am force-stop {ev.PKG}"); time.sleep(2)
    sh(f"am start -n {ev.PKG}/org.yuzu.yuzu_emu.ui.main.MainActivity"); time.sleep(5)
    ev.open_game_card(g)
    sh("input swipe 960 600 960 600 1000"); time.sleep(2)      # long press: properties
    p = shot(f"drv-{a.game}-props")
    hits = [l for l in ocr(p) if "GPU driver manager" in l[4]]
    if not hits:
        sys.exit("properties page not found (no 'GPU driver manager' on screen); nothing tapped")
    x, y, w, h, _ = hits[0]
    sh(f"input tap {x + w // 2} {y + h // 2}"); time.sleep(2)

    # Scroll to the top, then page down until the driver's card is in the safe band.
    for _ in range(8):
        sh("input swipe 1450 350 1450 900 300"); time.sleep(0.6)
    pat = re.compile(DRIVERS[a.driver])
    for page in range(16):
        time.sleep(1)
        frames = []
        for f in range(4):
            frames.append(shot(f"drv-{a.game}-list{page}-{f}"))
            time.sleep(0.9)
        p = frames[0]
        radios = [r for r in find_radios(p) if SAFE_Y[0] <= r[1] <= SAFE_Y[1]]
        texts = card_texts(frames, radios)
        matches = [r for r in radios if pat.search(texts[r])]
        if len(matches) > 1:
            sys.exit(f"{a.driver!r} matches several cards {matches}; nothing tapped")
        for rx, ry in matches:
            text = texts[(rx, ry)].strip()[:80]
            if radio_selected(p, rx, ry):
                print(f"{a.driver} already selected for {a.game} ({text!r})")
            else:
                sh(f"input tap {rx} {ry}"); time.sleep(1.2)
                p2 = shot(f"drv-{a.game}-picked")
                if not radio_selected(p2, rx, ry):
                    sys.exit(f"tapped {a.driver} at ({rx},{ry}) but it does not show as selected")
                print(f"selected {a.driver} for {a.game} ({text!r})")
            sh("input keyevent BACK"); time.sleep(1)
            sh("input keyevent BACK"); time.sleep(1)
            return
        sh("input swipe 1450 750 1450 450 400")  # page down a little (cards are ~290 px)
    sys.exit(f"driver {a.driver!r} not found in the list; nothing selected")


if __name__ == "__main__":
    main()
