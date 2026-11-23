"""Run one Turnip option variant in BOTW (Eden) at the Akkala Citadel autosave, unattended.

    python scripts/eden_variant.py --label v4-sysmem --tu-debug sysmem
    python scripts/eden_variant.py --label v4-bandwidth --algo bandwidth
    python scripts/eden_variant.py --label v4-base                 # no overrides

Steps: set the debug.mesa.* properties (transient, cleared on reboot; empty = unset),
force-stop Eden (Turnip reads its options once per process), launch Eden, tap the BOTW
card, show the touch overlay, press A three times (Continue, latest save, Start), wait
for the world, screenshot, then record with gamemon.py + eden_threads.py.
Uses whatever GPU driver Eden has selected; check the logcat line it prints.
Coordinates are for the G2 (1920x1080 landscape); see memory rpg2-eden-adb-driving.
"""
import argparse, os, re, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ADB = ROOT / "tools" / "platform-tools" / "adb.exe"
PKG = "com.miHoYo.Yuanshen"
SCRATCH = Path(os.environ.get("EDEN_SHOTS", ROOT / "results" / PKG / "shots"))


def adb(*a, **kw):
    return subprocess.run([str(ADB), *a], capture_output=True, text=True, **kw).stdout


def sh(cmd):
    return adb("shell", cmd)


def press(x, y, ms=150):
    sh(f"input swipe {x} {y} {x} {y} {ms}")


def shot(name):
    SCRATCH.mkdir(parents=True, exist_ok=True)
    p = SCRATCH / f"{name}.png"
    with open(p, "wb") as f:
        subprocess.run([str(ADB), "exec-out", "screencap", "-p"], stdout=f)
    if p.stat().st_size < 1000:   # empty/garbage capture: the G2 went offline or asleep
        sys.exit(f"screenshot '{name}' is empty: the G2 is offline or asleep; stopping")
    return p


# Per-game launch profiles. search: typed into Eden's search (%s = space; the full title
# ranks the game first). title: must appear in Eden's "Loading custom settings for" log
# line before any input is sent. show_ctrl_y: "Show controller" in Eden's drawer (it sits
# lower when the drawer title wraps to two lines). boot: seconds from launch to the
# game's title menu. presses: (wait after) for each overlay-A press from the title menu
# into the game. load_wait: the --load-wait default.
GAMES = {
    "botw": dict(search="Breath%sof%sthe%sWild",
                 title="The Legend of Zelda: Breath of the Wild",
                 show_ctrl_y=462, boot=40, presses=[3, 3, 0], load_wait=45),
    # Resume always spawns at the Odyssey ship in the current kingdom.
    "smo": dict(search="Super%sMario%sOdyssey", title="SUPER MARIO ODYSSEY",
                show_ctrl_y=380, boot=40, presses=[0], load_wait=40),
}


def pick_device():
    serials = [l.split("\t")[0] for l in adb("devices").splitlines()[1:] if l.endswith("\tdevice")]
    if not serials:
        sys.exit("no device")
    # Prefer a plain IP:port, then the mDNS name without a " (2)" duplicate suffix.
    serials.sort(key=lambda s: (not re.match(r"\d+\.\d+\.\d+\.\d+:\d+$", s), " (" in s))
    os.environ["ANDROID_SERIAL"] = serials[0]
    # The G2 sleeps after 60 s idle (e.g. while cooling down); wake it before any input.
    sh("input keyevent KEYCODE_WAKEUP")
    time.sleep(1)


def open_game_card(g):
    """From Eden's game list, filter to the game and centre its card (no launch)."""
    # Right after a G2 reboot Eden may start before the SD-card folders are reachable and
    # drop them from its game list (seen 2026-11-23). Wait for a non-empty library, and
    # never type or tap into an empty one.
    sys.path.insert(0, str(Path(__file__).resolve().parent))   # python -I drops the script dir
    from eden_driver import ocr
    for _ in range(6):
        if not any("No files were found" in t for *_, t in ocr(shot("eden-library"))):
            break
        time.sleep(10)
    else:
        sys.exit("Eden shows an empty game library (folders not reachable?); stopping without input")
    # The carousel keeps its scroll position and the search is fuzzy ("Breath" also
    # matches Sonic X Shadow), so: clear, search the full title (it ranks first),
    # then scroll the carousel back to its first card.
    sh("input tap 1426 125"); time.sleep(1)     # clear (x) in the search bar
    sh("input tap 860 127"); time.sleep(1)
    sh(f"input text {g['search']}"); time.sleep(1.5)
    sh("input keyevent BACK"); time.sleep(1.5)
    for _ in range(4):
        sh("input swipe 300 600 1600 600 200"); time.sleep(0.8)
    time.sleep(1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--label", required=True)
    ap.add_argument("--game", choices=sorted(GAMES), default="botw")
    ap.add_argument("--tu-debug", default="")
    ap.add_argument("--algo", default="")
    ap.add_argument("--ir3-debug", default="")
    ap.add_argument("--fd-features", default="", help="FD_DEV_FEATURES, e.g. enable_tp_ubwc_flag_hint=0")
    ap.add_argument("--ubwc", default="", help="1: request UBWC swapchain buffers (patch 0005)")
    ap.add_argument("--gcm", default="", help="1 or 2: ir3 nir_opt_gcm (not in any cache key: "
                    "pair with a unique --ir3-debug value so shaders recompile)")
    ap.add_argument("--seconds", type=int, default=60)
    ap.add_argument("--load-wait", type=int, default=0, help="default: per game")
    ap.add_argument("--no-record", action="store_true")
    ap.add_argument("--flicker", type=int, default=0,
                    help="also screen-record this many seconds and score texture flicker")
    a = ap.parse_args()
    g = GAMES[a.game]
    load_wait = a.load_wait or g["load_wait"]

    pick_device()

    props = {"debug.mesa.tu.debug": a.tu_debug,
             "debug.mesa.tu.autotune.algo": a.algo,
             "debug.mesa.ir3.shader.debug": a.ir3_debug,
             "debug.mesa.gcm": a.gcm,               # ir3 Global Code Motion (idea: Balemuni)
             # patch 0005: UBWC swapchain buffers (idea: StevenMXZ)
             "debug.mesa.android.ubwc.swapchain": a.ubwc,
             # freedreno device-info overrides, e.g. enable_tp_ubwc_flag_hint=0 (issue #1)
             "debug.mesa.fd.dev.features": a.fd_features}
    for k, v in props.items():
        sh(f"setprop {k} '{v}'")
    print("props:", {k: sh(f"getprop {k}").strip() for k in props})

    sh(f"am force-stop {PKG}")
    time.sleep(2)
    adb("logcat", "-c")
    sh(f"am start -n {PKG}/org.yuzu.yuzu_emu.ui.main.MainActivity")
    time.sleep(5)
    open_game_card(g)
    shot(f"{a.label}-0-list")
    sh("input tap 960 600")           # the centred first card
    time.sleep(8)
    if f"Loading custom settings for {g['title']}" not in adb("logcat", "-d"):
        sh(f"am force-stop {PKG}")
        sys.exit(f"{g['title']} did not start (wrong card?); Eden stopped, no input sent")

    def guard(step):
        if "EmulationActivity" not in sh("dumpsys window | grep mCurrentFocus"):
            shot(f"{a.label}-abort")
            sys.exit(f"Eden emulation not in the foreground before '{step}'; stopping without input")

    time.sleep(g["boot"] - 8)
    guard("drawer")
    sh("input swipe 5 540 960 540 400")  # Eden drawer
    time.sleep(1.5)
    guard("show controller")
    sh(f"input tap 257 {g['show_ctrl_y']}")  # Show controller
    time.sleep(1.2)
    sh("input tap 1400 500")          # close drawer
    time.sleep(1.2)
    for i, wait in enumerate(g["presses"]):
        guard(f"menu press {i + 1}")
        press(1460, 855); time.sleep(wait)   # overlay A
        if i == 0:
            shot(f"{a.label}-1-menu")
    time.sleep(load_wait)
    guard("record")
    p = shot(f"{a.label}-2-ingame")
    log = adb("logcat", "-d")
    for line in log.splitlines():
        if re.search(r"loading (custom|default) driver|Report: Driver|TU_DEBUG=|TU_AUTOTUNE_ALGO=|IR3_SHADER_DEBUG", line):
            print(line.strip()[:200])
    print("screenshot:", p)
    if a.flicker:
        # Screen-record the scene and score A->B->A pixel spikes (texture flicker).
        remote = "/data/local/tmp/flicker.mp4"
        sh(f"screenrecord --time-limit {a.flicker} --bit-rate 24000000 {remote}")
        local = SCRATCH / f"{a.label}-flicker.mp4"
        adb("pull", remote, str(local))
        sh(f"rm -f {remote}")
        r = subprocess.run([sys.executable, str(ROOT / "scripts" / "flicker_score.py"), str(local),
                            "--save-map", str(SCRATCH / f"{a.label}-flickermap.png")],
                           capture_output=True, text=True)
        print(f"FLICKER {a.label}: {r.stdout.strip()} {r.stderr.strip()[-300:]}")
    if a.no_record:
        return
    g = subprocess.run([sys.executable, str(ROOT / "scripts" / "gamemon.py"), "--label", a.label,
                        "--seconds", str(a.seconds)], capture_output=True, text=True)
    print("\n".join(g.stdout.splitlines()[-4:]))
    r = subprocess.run([sys.executable, "-I", str(ROOT / "scripts" / "eden_threads.py"),
                        "--label", a.label], capture_output=True, text=True)
    print(r.stdout)


if __name__ == "__main__":
    main()
