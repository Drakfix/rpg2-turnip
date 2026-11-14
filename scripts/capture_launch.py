"""Capture the G2's system log around one game launch, to see why a driver fails.

    python scripts/capture_launch.py --label gamenative-tuned-v2

1. Start this script (it notes the device's current time; nothing is cleared or changed).
2. On the G2, launch the game with the driver under test and wait until it fails
   (closes, freezes, or stays on a black screen for ~20 s).
3. Press Enter here. The log from that window is saved under logs/ and the lines
   most likely to explain the failure are printed.

Read-only: it only runs `date` and `logcat -d` on the device.
"""
import argparse
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gamemon import ROOT, adb, ensure_connected, foreground_package  # noqa: E402

INTERESTING = re.compile(
    r"Fatal signal|SIGSEGV|SIGILL|SIGABRT|SIGBUS|\bF DEBUG\b|backtrace:|#\d\d pc |Abort message|"
    r"\bMESA\b|\bTU\b|turnip|freedreno|kgsl|adreno|vulkan|\bVk[A-Z]|vk_icd|wrapper|adrenotools|"
    r"dxvk|vkd3d|wined3d|\bwine\b|box64|fex|"
    r"dlopen|cannot locate symbol|library .* not found|UnsatisfiedLink|CANNOT LINK|"
    r"VK_ERROR|ERROR_INCOMPATIBLE|ERROR_INITIALIZATION|No such device|is unsupported",
    re.IGNORECASE)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--label", required=True, help="e.g. gamenative-tuned-v2, gamenative-purple-t30")
    ap.add_argument("--lines", type=int, default=120, help="how many highlighted lines to print")
    a = ap.parse_args()
    ensure_connected()

    start = adb("shell", "date +'%m-%d %H:%M:%S.000'").stdout.strip()
    if not re.fullmatch(r"\d\d-\d\d \d\d:\d\d:\d\d\.000", start):
        sys.exit(f"could not read the device clock (got {start!r})")
    print(f"Device time {start}. Launch the game on the G2 now; when it has failed, press Enter here.")
    try:
        input()
    except EOFError:
        sys.exit("needs an interactive terminal (press Enter after the failure)")

    pkg = foreground_package()
    log = adb("shell", f"logcat -d -b main,system,crash -v threadtime -T '{start}'")
    text = log.stdout if isinstance(log.stdout, str) else log.stdout.decode("utf-8", "replace")
    out_dir = ROOT / "logs"
    out_dir.mkdir(exist_ok=True)
    label = re.sub(r"[^\w.-]+", "_", a.label)
    out = out_dir / f"{label}-{time.strftime('%Y%m%d-%H%M%S')}.txt"
    out.write_text(text, encoding="utf-8", errors="replace")

    lines = text.splitlines()
    hits = [ln for ln in lines if INTERESTING.search(ln)]
    print(f"foreground app now: {pkg}")
    print(f"{len(lines)} log lines captured, {len(hits)} look relevant; saved {out.relative_to(ROOT)}\n")
    for ln in hits[-a.lines:]:
        print(ln[:240])
    if not hits:
        print("(nothing matched the usual driver/crash patterns; the full log is in the saved file)")


if __name__ == "__main__":
    main()
