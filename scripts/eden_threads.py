"""Average per-thread CPU of the Eden process over a few `top -H` samples (read-only).

    python scripts/eden_threads.py [--samples 6] [--interval 5] [--label name]

Prints the busiest threads (GPU = Eden's GPU emulation thread, which is where the
Vulkan driver's command recording runs; VulkanWorker = submission thread;
CPUCore_N = guest CPU cores). Compare the GPU + VulkanWorker share between drivers
at the same spot: the game work is the same, so a difference is driver CPU cost.
With --label the averages are saved to results/<pkg>/threads-<label>-<time>.json.
"""
import argparse, json, os, re, subprocess, time
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ADB = ROOT / "tools" / "platform-tools" / "adb.exe"
PKG = "com.miHoYo.Yuanshen"


def adb(*args):
    return subprocess.run([str(ADB), *args], capture_output=True, text=True).stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--samples", type=int, default=6)
    ap.add_argument("--interval", type=int, default=5)
    ap.add_argument("--pkg", default=PKG)
    ap.add_argument("--label")
    a = ap.parse_args()
    serials = [l.split()[0] for l in adb("devices").splitlines()[1:] if l.endswith("device")]
    if serials:
        os.environ["ANDROID_SERIAL"] = next((s for s in serials if re.match(r"\d+\.\d+\.\d+\.\d+:\d+", s)), serials[0])
    pid = adb("shell", "pidof", a.pkg).strip()
    if not pid:
        raise SystemExit(f"{a.pkg} is not running")
    # n+1 iterations: top's first sample is since thread start, so drop it.
    out = adb("shell", f"top -H -p {pid} -d {a.interval} -n {a.samples + 1} -b")
    blocks = re.split(r"\n(?=\s*TID\s)", out)[2:]
    tot = defaultdict(float)
    for b in blocks:
        for line in b.splitlines()[1:]:
            f = line.split()
            if len(f) < 12 or not f[0].isdigit():
                continue
            tot[f[-2]] += float(f[8])
    n = max(len(blocks), 1)
    avg = {k: v / n for k, v in sorted(tot.items(), key=lambda kv: -kv[1]) if v / n >= 1.0}
    for k, v in list(avg.items())[:14]:
        print(f"{k:18s} {v:6.1f} %")
    print(f"{'(process total)':18s} {sum(tot.values()) / n:6.1f} %   over {n} x {a.interval}s")
    if a.label:
        d = ROOT / "results" / a.pkg
        d.mkdir(parents=True, exist_ok=True)
        p = d / f"threads-{a.label}-{time.strftime('%Y%m%d-%H%M%S')}.json"
        p.write_text(json.dumps({"label": a.label, "samples": n, "interval": a.interval, "threads": avg}, indent=1))
        print("saved", p.relative_to(ROOT))


if __name__ == "__main__":
    main()
