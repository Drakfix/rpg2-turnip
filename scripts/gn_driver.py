"""Set the Vulkan driver of AC Revelations' GameNative container, unattended.

    python scripts/gn_driver.py --driver v5rc2      # v4, v4-263, v5rc2, t30

Opens GameNative > AC Revelations > gear > Edit container > Graphics, opens "Graphics Driver
Version", searches the driver, taps the matching entry (found by Windows OCR), saves, then
reopens the config to verify. Never touches the uninstall (trash) button next to the gear.
"""
import argparse, re, sys, time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import eden_variant as ev                 # noqa: E402
from eden_driver import ocr               # noqa: E402
from gn_acr_run import wait_for, tap_ac_card, PKG  # noqa: E402

DRIVERS = {   # search text, regex the chosen entry must match
    "v4": ("A22", r"A22 TUNED .?4 \(Mesa 27"),      # OCR often reads "v4" as '"4'
    "v4-263": ("A22", r"A22 TUNED .?4 \(Mesa 26"),
    "v5rc2": ("v5rc2", r"v5rc2"),
    "t30": ("T30", r"Driver T30"),
    "v6rc1": ("v6rc1", r"v6rc[1lI]"),
}


def open_graphics(tag):
    sh = ev.sh
    sh(f"am force-stop {PKG}"); time.sleep(2)
    sh(f"monkey -p {PKG} -c android.intent.category.LAUNCHER 1"); time.sleep(8)
    text = wait_for(tag, r"All \(\d+\)|Enjoying GameNative", 30)
    if "Enjoying" in text:
        sh("input tap 911 800"); time.sleep(1.5)
    tap_ac_card(tag)
    text = wait_for(tag, r"Compatibility|Enjoying GameNative", 15)
    if "Enjoying" in text:
        sh("input tap 911 800"); time.sleep(1.5); tap_ac_card(tag)
    wait_for(tag, r"Assassins Creed Revel|Unknown . 2011", 15)
    sh("input tap 1658 523"); time.sleep(2.5)              # gear (NOT the trash at x=1793)
    wait_for(tag, r"Edit container", 10)
    sh("input tap 1396 323"); time.sleep(2.5)              # Edit container
    wait_for(tag, r"Revelations Config", 10)
    sh("input tap 307 196"); time.sleep(2)                 # Graphics tab
    return wait_for(tag, r"Graphics Driver Version", 10)


def current(tag):
    p = ev.shot(tag)
    for x, y, w, h, t in ocr(p):
        if 490 < y < 540 and x < 200:
            return t
    return ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--driver", choices=sorted(DRIVERS), required=True)
    a = ap.parse_args()
    ev.pick_device()
    sh = ev.sh
    search, want = DRIVERS[a.driver]
    open_graphics("gnd-open")
    if re.search(want, current("gnd-cur")):
        print(f"{a.driver} already set"); sh("input tap 62 72"); time.sleep(1)
        sh(f"am force-stop {PKG}"); return
    sh("input tap 960 495"); time.sleep(2)                 # driver version dropdown
    sh("input tap 1540 98"); time.sleep(1)                 # its search box
    sh(f"input text {search}"); time.sleep(1.5)
    p = ev.shot("gnd-list")
    hits = [(x, y, w, h, t) for x, y, w, h, t in ocr(p) if x > 1200 and y > 180 and re.search(want, t)]
    if len(hits) != 1:
        sh("input keyevent BACK"); sh("input keyevent BACK"); sh("input tap 62 72")
        sys.exit(f"expected one match for {want!r}, got {[h[4] for h in hits]}; nothing changed")
    x, y, w, h, t = hits[0]
    sh(f"input tap {x + w // 2} {y + h // 2}"); time.sleep(1.5)
    if "mInputShown=true" in sh("dumpsys input_method | grep mInputShown"):
        sh("input keyevent BACK"); time.sleep(1)           # hide keyboard (only if shown:
                                                           # BACK on the config = discard dialog)
    p = ev.shot("gnd-picked")
    if any("Unsaved Changes" in t for *_, t in ocr(p)):
        sh("input tap 1053 642")                           # Cancel (keep editing)
        time.sleep(1)
    if not re.search(want, current("gnd-picked")):
        sys.exit("selection did not stick; closing without saving");
    sh("input swipe 1856 72 1856 72 120"); time.sleep(2.5)  # save
    open_graphics("gnd-verify")
    got = current("gnd-verify")
    sh("input tap 62 72"); time.sleep(1)
    sh(f"am force-stop {PKG}")
    if not re.search(want, got):
        sys.exit(f"after saving the config shows {got!r}, not {a.driver}")
    print(f"set {a.driver} ({got})")


if __name__ == "__main__":
    main()
