"""Black-frame probe for Trails in the Sky 2nd Chapter Demo in GameNative (issue #1).

    python scripts/gn_trails_probe.py --label base
    python scripts/gn_trails_probe.py --label hint0 --prop debug.mesa.fd.dev.features=enable_tp_ubwc_flag_hint=0

Sets the given debug.mesa.* properties (all listed ones are cleared first), restarts
GameNative, opens the game's page via its library card (found by OCR), presses Play,
waits for the title menu ("New Game" by OCR), records 10 s and counts black frames with
scripts/black_frames.py. Then stops GameNative. Only touches the game page's Play button.
"""
import argparse, re, subprocess, sys, time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import eden_variant as ev          # noqa: E402
from eden_driver import ocr        # noqa: E402

ROOT = ev.ROOT
PKG = "app.gamenative"
PROPS = ["debug.mesa.fd.dev.features", "debug.mesa.tu.autotune.algo", "debug.mesa.tu.debug",
         "debug.mesa.android.ubwc.swapchain", "debug.mesa.ir3.shader.debug"]


def text(tag):
    return " | ".join(t for *_, t in ocr(ev.shot(tag)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--label", required=True)
    ap.add_argument("--prop", action="append", default=[], help="key=value")
    ap.add_argument("--seconds", type=int, default=10)
    a = ap.parse_args()
    ev.pick_device()
    sh = ev.sh
    for k in PROPS:
        sh(f"setprop {k} ''")
    for kv in a.prop:
        k, v = kv.split("=", 1)
        sh(f"setprop {k} '{v}'")
    sh(f"am force-stop {PKG}"); time.sleep(2)
    sh(f"monkey -p {PKG} -c android.intent.category.LAUNCHER 1"); time.sleep(9)
    def open_and_play(tag):
        for _ in range(4):                   # find the game's card, closing pop-ups
            lines = ocr(ev.shot(f"tp-{a.label}-{tag}lib"))
            if any("Enjoying GameNative" in t or "How did the game run" in t for *_, t in lines):
                hit = [l for l in lines if l[4].strip() == "Close"]
                if hit:
                    x, y, w, h, _ = hit[0]; sh(f"input tap {x + w // 2} {y + h // 2}"); time.sleep(1.5)
                    continue
            card = [l for l in lines if re.search(r"Trail|Sky 2nd|Chapter Demo", l[4])]
            if card:
                x, y, w, h, _ = card[0]
                sh(f"input tap {x + w // 2} {max(y - 80, 200)}"); time.sleep(3)
                break
            tab = [l for l in lines if re.match(r"(?i)steam \(\d+\)", l[4].strip())]
            if tab:                          # the demo sits first in the Steam tab
                x, y, w, h, _ = tab[0]; sh(f"input tap {x + w // 2} {y + h // 2}"); time.sleep(3)
                continue
            time.sleep(3)
        else:
            sys.exit("game card not found; nothing pressed")
        if not re.search(r"Nihon Falcom|Compatibility", text(f"tp-{a.label}-{tag}page")):
            sys.exit("not on the game's page; nothing pressed")
        sh("input tap 203 523")              # Play
        t0 = time.time()
        while time.time() - t0 < 150:
            time.sleep(8)
            t = text(f"tp-{a.label}-{tag}title")
            if re.search(r"New Game|Load Game|Continue", t):
                return True
            if re.search(r"All \(\d+\)|steam \(\d+\)", t, re.I) and time.time() - t0 > 20:
                return False                 # game exited back to the library
        sys.exit("title menu not reached in 150 s")

    # GameNative's first launch after a cold start often exits at once (Steam sync); retry once.
    if not open_and_play("a") and not open_and_play("b"):
        sys.exit("game exited to the library twice; stopping")
    time.sleep(8)
    remote = "/data/local/tmp/tp.mp4"
    sh(f"screenrecord --time-limit {a.seconds} --bit-rate 24000000 {remote}")
    local = ev.SCRATCH / f"tp-{a.label}.mp4"
    ev.adb("pull", remote, str(local)); sh(f"rm -f {remote}")
    r = subprocess.run([sys.executable, "-I", str(ROOT / "scripts" / "black_frames.py"), str(local)],
                       capture_output=True, text=True)
    print(f"BLACK {a.label} {' '.join(a.prop)}: {r.stdout.strip()}")
    sh(f"am force-stop {PKG}")
    for k in PROPS:
        sh(f"setprop {k} ''")


if __name__ == "__main__":
    main()
