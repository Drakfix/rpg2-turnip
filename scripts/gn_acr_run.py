"""One unattended Assassin's Creed Revelations run in GameNative (record + flicker check).

    python scripts/gn_acr_run.py --label acr-v5rc2-r1 [--flicker 20] [--seconds 60]

Launches GameNative, opens AC Revelations (top of the "All" library), presses Play, then
drives the game with GameNative's on-screen controller: Start at "PRESS START", A on the
offline notice, A on STORY MODE, A on the save (Load). Every press is gated on Windows OCR
of the screen (scripts/win_ocr.ps1) showing the expected text, so it never presses
anything on an unexpected screen; on the save list it only ever presses A (Load), never X
(which deletes the save). Then it records with gamemon.py and optionally scores flicker.
The driver is whatever the game's container config has selected (set it in GameNative).
"""
import argparse, os, re, subprocess, sys, time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import eden_variant as ev        # noqa: E402  adb helpers
from eden_driver import ocr     # noqa: E402

ROOT = ev.ROOT
PKG = "app.gamenative"
START = (1032, 968)      # on-screen controller "menu" (Start) button
BTN_A = (1673, 567)


def screen_text(tag):
    p = ev.shot(tag)
    return p, " | ".join(t for *_, t in ocr(p))


def wait_for(tag, pattern, timeout, every=5):
    t0 = time.time()
    while time.time() - t0 < timeout:
        p, text = screen_text(tag)
        if re.search(pattern, text, re.I):
            return text
        time.sleep(every)
    sys.exit(f"[{tag}] did not see /{pattern}/ within {timeout}s; last screen: {text[:200]}")


def overlay_visible(png):
    """True if GameNative's on-screen controller is drawn: its A/B/X button outlines are
    thin light rings, brighter than the pixels just inside and outside them."""
    import math
    import cv2
    g = cv2.cvtColor(cv2.imread(str(png)), cv2.COLOR_BGR2GRAY).astype(int)
    rings = 0
    for cx, cy in ((1673, 567), (1785, 425), (1560, 425)):
        best = 0
        for r in range(50, 64):
            hits = 0
            for k in range(16):
                ang = 2 * math.pi * k / 16
                v = lambda rr: g[int(cy + rr * math.sin(ang)), int(cx + rr * math.cos(ang))]
                if v(r) - max(v(r - 6), v(r + 6)) > 20:
                    hits += 1
            best = max(best, hits)
        rings += best >= 10
    return rings >= 2


def press(xy, ms=150):
    ev.sh(f"input swipe {xy[0]} {xy[1]} {xy[0]} {xy[1]} {ms}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--label", required=True)
    ap.add_argument("--seconds", type=int, default=60)
    ap.add_argument("--flicker", type=int, default=0)
    ap.add_argument("--settle", type=int, default=20, help="seconds in-game before recording")
    a = ap.parse_args()
    ev.pick_device()
    sh = ev.sh

    sh(f"am force-stop {PKG}"); time.sleep(2)
    sh(f"monkey -p {PKG} -c android.intent.category.LAUNCHER 1"); time.sleep(8)
    text = wait_for(f"{a.label}-lib", r"All \(\d+\)|Enjoying GameNative", 30)
    if "Enjoying" in text:                      # membership pop-up: press Close only
        sh("input tap 911 800"); time.sleep(1.5)
    sh("input tap 955 300"); time.sleep(3)      # AC card (top middle of "All")
    text = wait_for(f"{a.label}-page", r"Compatibility|Enjoying GameNative", 15)
    if "Enjoying" in text:
        sh("input tap 911 800"); time.sleep(1.5)
        sh("input tap 955 300"); time.sleep(3)
    wait_for(f"{a.label}-page", r"Assassins Creed Revel|Unknown . 2011", 15)
    wait_for(f"{a.label}-page", r"Compatibility", 15)
    adb_log_mark = sh("date +'%m-%d %H:%M:%S.000'").strip()
    sh("input tap 203 523")                     # Play
    wait_for(f"{a.label}-title", r"PRESS START", 150, every=8)

    # GameNative's on-screen controller is off on every launch. Turn it on via the Quick
    # Menu, but only after the pixel check says it is hidden (the toggle is a toggle).
    p = ev.shot(f"{a.label}-title")
    if not overlay_visible(p):
        sh("input keyevent BACK"); time.sleep(2)
        wait_for(f"{a.label}-qm", r"Quick Menu", 10)
        sh("input tap 98 677"); time.sleep(1.5)   # Controller section
        wait_for(f"{a.label}-qm", r"On-screen Controller", 10)
        sh("input tap 565 666"); time.sleep(2.5)  # On-screen Controller (closes the menu)
        if not overlay_visible(ev.shot(f"{a.label}-title")):
            sys.exit("on-screen controller still hidden after toggling; stopping")
    # (no second PRESS START check: the overlay's Start button covers that text)
    press(START); time.sleep(6)
    wait_for(f"{a.label}-offline", r"OFFLINE|offline mode", 20, every=3)
    press(BTN_A); time.sleep(5)
    wait_for(f"{a.label}-menu", r"STORY MODE", 20, every=3)
    press(BTN_A); time.sleep(4)
    wait_for(f"{a.label}-saves", r"Load", 20, every=3)    # save list: A=Load, X=Delete
    press(BTN_A)
    wait_for(f"{a.label}-ingame", r"Throw knife|Fast walk|Gentle push|Attack", 120, every=8)
    time.sleep(a.settle)
    ev.shot(f"{a.label}-2-ingame")
    log = subprocess.run([str(ev.ADB), "shell", f"logcat -d -T '{adb_log_mark}'"], capture_output=True,
                         text=True, encoding="utf-8", errors="replace").stdout or ""
    drv = sorted(set(re.findall(r"(turnip Mesa driver [\d.]+|PurpleVK[^,\n]*)", log)))
    print("driver hints:", drv[:3])

    if a.flicker:
        remote = "/data/local/tmp/flicker.mp4"
        sh(f"screenrecord --time-limit {a.flicker} --bit-rate 24000000 {remote}")
        local = ev.SCRATCH / f"{a.label}-flicker.mp4"
        ev.adb("pull", remote, str(local)); sh(f"rm -f {remote}")
        r = subprocess.run([sys.executable, str(ROOT / "scripts" / "flicker_score.py"), str(local),
                            "--save-map", str(ev.SCRATCH / f"{a.label}-flickermap.png")],
                           capture_output=True, text=True)
        print(f"FLICKER {a.label}: {r.stdout.strip()} {r.stderr.strip()[-300:]}")
    g = subprocess.run([sys.executable, str(ROOT / "scripts" / "gamemon.py"), "--label", a.label,
                        "--seconds", str(a.seconds)], capture_output=True, text=True)
    print("\n".join(g.stdout.splitlines()[-3:]))
    sh(f"am force-stop {PKG}")


if __name__ == "__main__":
    main()
