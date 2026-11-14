"""Wait until the G2 has cooled down before the next test run (read-only).

    python scripts/cooldown.py                 # wait with the default limits
    python scripts/cooldown.py --check         # print temperatures once and exit
    python scripts/cooldown.py --stop-pkg app.gamenative   # force-stop a game first

Reads the hottest GPU (gpuss-*), CPU (cpu-*/cpuss-*) and skin (sys-therm-*) thermal zones and
the battery temperature. Waits until all are at or under the targets, printing progress every
30 s, so batches give the device a rest between runs instead of stacking heat. The fan is left
to whatever controls it (the G2 Fan Curve app or Retroid's own mode).

Exit codes: 0 cooled down, 2 still too hot after --max-wait (caller should stop the batch).
"""
import argparse, sys, time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import eden_variant as ev  # noqa: E402

READ = r'''for z in /sys/class/thermal/thermal_zone*; do t=$(cat $z/type 2>/dev/null); case $t in gpuss-*|cpu-*|cpuss-*|sys-therm-*) echo "$t $(cat $z/temp 2>/dev/null)";; esac; done; dumpsys battery | grep -E "^  temperature|^  level"'''


def read():
    gpu = cpu = skin = 0.0
    batt = level = None
    for line in ev.sh(READ).splitlines():
        p = line.split()
        if len(p) < 2:
            continue
        if p[0] == "temperature:":
            batt = int(p[1]) / 10
        elif p[0] == "level:":
            level = int(p[1])
        else:
            try:
                c = int(p[1]) / 1000
            except ValueError:
                continue
            if c < -20:            # unpopulated sensor (-40.96)
                continue
            if p[0].startswith("gpuss"):
                gpu = max(gpu, c)
            elif p[0].startswith("sys-therm"):
                skin = max(skin, c)
            else:
                cpu = max(cpu, c)
    return {"gpu": gpu, "cpu": cpu, "skin": skin, "batt": batt, "level": level}


def fmt(t):
    return (f"GPU {t['gpu']:.1f}  CPU {t['cpu']:.1f}  skin {t['skin']:.1f}  "
            f"battery {t['batt']:.1f} C ({t['level']}%)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gpu", type=float, default=45)
    ap.add_argument("--cpu", type=float, default=48)
    ap.add_argument("--skin", type=float, default=40)
    ap.add_argument("--batt", type=float, default=37)
    ap.add_argument("--max-wait", type=int, default=900, help="seconds")
    ap.add_argument("--stop-pkg", help="force-stop this app before cooling")
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    ev.pick_device()
    if a.stop_pkg:
        ev.sh(f"am force-stop {a.stop_pkg}")
    t = read()
    if a.check:
        print(fmt(t))
        return
    start = time.time()
    last = 0
    while True:
        ok = (t["gpu"] <= a.gpu and t["cpu"] <= a.cpu and t["skin"] <= a.skin
              and (t["batt"] is None or t["batt"] <= a.batt))
        waited = time.time() - start
        if ok:
            print(f"cool after {waited:.0f} s: {fmt(t)}")
            return
        if waited > a.max_wait:
            print(f"STILL HOT after {waited:.0f} s: {fmt(t)}")
            sys.exit(2)
        if waited - last >= 30 or last == 0:
            print(f"cooling ({waited:.0f} s): {fmt(t)}", flush=True)
            last = waited or 1
        time.sleep(10)
        t = read()


if __name__ == "__main__":
    main()
