"""Run one Turnip option variant in BOTW (Eden) at the Akkala Citadel autosave, unattended.

    python scripts/eden_botw_variant.py --label v4-sysmem --tu-debug sysmem
    python scripts/eden_botw_variant.py --label v4-bandwidth --algo bandwidth
    python scripts/eden_botw_variant.py --label v4-base                 # no overrides

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
    return p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--label", required=True)
    ap.add_argument("--tu-debug", default="")
    ap.add_argument("--algo", default="")
    ap.add_argument("--ir3-debug", default="")
    ap.add_argument("--seconds", type=int, default=60)
    ap.add_argument("--load-wait", type=int, default=45)
    ap.add_argument("--no-record", action="store_true")
    ap.add_argument("--flicker", type=int, default=0,
                    help="also screen-record this many seconds and score texture flicker")
    a = ap.parse_args()

    serials = [l.split("\t")[0] for l in adb("devices").splitlines()[1:] if l.endswith("\tdevice")]
    if not serials:
        sys.exit("no device")
    # Prefer a plain IP:port, then the mDNS name without a " (2)" duplicate suffix.
    serials.sort(key=lambda s: (not re.match(r"\d+\.\d+\.\d+\.\d+:\d+$", s), " (" in s))
    os.environ["ANDROID_SERIAL"] = serials[0]

    props = {"debug.mesa.tu.debug": a.tu_debug,
             "debug.mesa.tu.autotune.algo": a.algo,
             "debug.mesa.ir3.shader.debug": a.ir3_debug}
    for k, v in props.items():
        sh(f"setprop {k} '{v}'")
    print("props:", {k: sh(f"getprop {k}").strip() for k in props})

    sh(f"am force-stop {PKG}")
    time.sleep(2)
    adb("logcat", "-c")
    sh(f"am start -n {PKG}/org.yuzu.yuzu_emu.ui.main.MainActivity")
    time.sleep(5)
    # The carousel keeps its scroll position and the search is fuzzy ("Breath" also
    # matches Sonic X Shadow), so: clear, search the full title (BOTW ranks first),
    # then scroll the carousel back to its first card.
    sh("input tap 1426 125"); time.sleep(1)     # clear (x) in the search bar
    sh("input tap 860 127"); time.sleep(1)
    sh("input text Breath%sof%sthe%sWild"); time.sleep(1.5)
    sh("input keyevent BACK"); time.sleep(1.5)
    for _ in range(4):
        sh("input swipe 300 600 1600 600 200"); time.sleep(0.8)
    time.sleep(1)
    shot(f"{a.label}-0-list")
    sh("input tap 960 600")           # the centred first card
    time.sleep(8)
    if "Loading custom settings for The Legend of Zelda: Breath of the Wild" not in adb("logcat", "-d"):
        sh(f"am force-stop {PKG}")
        sys.exit("BOTW did not start (wrong card?); Eden stopped, no input sent")

    def guard(step):
        if "EmulationActivity" not in sh("dumpsys window | grep mCurrentFocus"):
            shot(f"{a.label}-abort")
            sys.exit(f"Eden emulation not in the foreground before '{step}'; stopping without input")

    time.sleep(32)
    guard("drawer")
    sh("input swipe 5 540 960 540 400")  # Eden drawer
    time.sleep(1.5)
    guard("show controller")
    sh("input tap 257 462")           # Show controller
    time.sleep(1.2)
    sh("input tap 1400 500")          # close drawer
    time.sleep(1.2)
    guard("continue")
    press(1460, 855); time.sleep(3)   # Continue
    shot(f"{a.label}-1-saves")
    guard("select save")
    press(1460, 855); time.sleep(3)   # latest save
    guard("start")
    press(1460, 855)                  # Start
    time.sleep(a.load_wait)
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
