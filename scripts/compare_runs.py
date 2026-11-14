"""Compare saved gamemon runs (and matching eden_threads samples) for one game.

    python scripts/compare_runs.py [--pkg com.miHoYo.Yuanshen] [--match akkala]

GPU work per frame = mean(MHz x busy) / FPS, in millions of GPU cycles. Lower means
the driver needs less GPU time for the same frame, which matters at a frame cap
where FPS cannot show the difference. GPU and VK columns are the CPU % of
Eden's "GPU" and "VulkanWorker" threads from eden_threads.py.
"""
import argparse, glob, json, os, statistics as st
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pkg", default="com.miHoYo.Yuanshen")
    ap.add_argument("--match", default="")
    a = ap.parse_args()
    d = ROOT / "results" / a.pkg
    threads = {}
    for f in sorted(glob.glob(str(d / "threads-*.json"))):
        t = json.load(open(f))
        threads[t["label"]] = t["threads"]
    print(f"{'label':34s} {'fps':>5s} {'MHz':>5s} {'busy':>5s} {'work':>6s} {'GPU':>5s} {'VK':>5s} {'proc':>6s}  gpuC")
    for f in sorted(glob.glob(str(d / "*.json")), key=os.path.getmtime):
        if os.path.basename(f).startswith("threads-") or a.match not in os.path.basename(f):
            continue
        r = json.load(open(f))
        s = r.get("samples") or []
        if not s or not r.get("avg_fps"):
            continue
        work = st.mean(x["mhz"] * x["busy"] / 100 for x in s) / r["avg_fps"]
        th = threads.get(r["label"], {})
        tot = sum(th.values()) if th else 0
        print(f"{r['label']:34s} {r['avg_fps']:5.2f} {r['gpu_mhz']:5.0f} {r['gpu_busy']:5.1f} {work:6.2f} "
              f"{th.get('GPU', 0):5.1f} {th.get('VulkanWorker', 0):5.1f} {tot:6.1f}  {r.get('gpu_c_start')}->{r.get('gpu_c_end')}")


if __name__ == "__main__":
    main()
