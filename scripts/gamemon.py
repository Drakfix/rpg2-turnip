"""Record frame pacing and GPU load on the Retroid Pocket G2 while a game runs.

Run on the PC with the game already in the foreground on the device:

    python scripts/gamemon.py --label tuned-v2 --seconds 60
    python scripts/gamemon.py --compare            # table of every saved run

Each run reads SurfaceFlinger's per-frame present times for the foreground app
plus the GPU clock, busy ratio and temperature once a second (read-only), saves
results/<game>/<label>-<time>.json and prints a summary.

Every run also logs the fan for the G2 Fan Curve app (github.com/Drakfix/g2-fan-curve):
fan duty/RPM and who drove it (curve or a stock Retroid mode), skin and battery
temperature per second, and the app's own event log (fan on/off, charger, ...)
in "fan_log". So driver benchmarks double as fan-curve logs.

Notes on reading the numbers:
  * FPS here is how often the emulator presents a new image. Most emulators
    present once per emulated frame, but some repeat frames, so keep the
    emulator's own FPS counter as a cross-check.
  * Compare runs at the same spot in the same game, and look at GPU MHz/busy:
    a driver that needs less GPU for the same FPS shows a lower clock or load.
"""
import argparse
import json
import os
import re
import statistics
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ADB = ROOT / "tools" / "platform-tools" / "adb.exe"
DEV_SCRIPT = ROOT / "test" / "gamemon_dev.sh"
RESULTS = ROOT / "results"
PENDING = 9223372036854775807  # INT64_MAX: frame not presented yet


def adb(*args, **kw):
    return subprocess.run([str(ADB), *args], capture_output=True, text=True, **kw)


def ensure_connected():
    """Make sure adb has the G2; reconnect over mDNS if needed. Exits with a specific reason."""
    def pick_serial():
        # The same G2 can show up twice (once by IP:port, once by its mDNS name), which makes
        # plain adb commands fail with "more than one device". Pin every later call to one of them.
        serials = re.findall(r"^(\S+)\tdevice$", adb("devices").stdout, re.MULTILINE)
        if serials:
            os.environ["ANDROID_SERIAL"] = serials[0]
        return bool(serials)

    os.environ.pop("ANDROID_SERIAL", None)
    if pick_serial():
        return
    # After a restart the G2 advertises a new port and adb does not always pick it up.
    endpoints = re.findall(r"_adb-tls-connect\._tcp\s+(\S+:\d+)", adb("mdns", "services").stdout)
    answers = []
    for ep in endpoints:
        r = adb("connect", ep)
        answers.append((ep, (r.stdout + r.stderr).strip()))
        if pick_serial():
            print(f"reconnected to the G2 at {ep}")
            return
    if not endpoints:
        sys.exit("G2 not found on the network.\n"
                 "On the G2: Settings > Developer options > Wireless debugging must be ON,\n"
                 "the screen awake, and the G2 on the same Wi-Fi as this PC.")
    # "failed to connect" (not "refused") means the G2 answered but rejected this PC's key.
    rejected = [ep for ep, msg in answers if msg.startswith("failed to connect")]
    if rejected:
        sys.exit("Wireless debugging is on, but the G2 no longer trusts this PC, so it must be paired again.\n"
                 "On the G2: Wireless debugging > 'Pair device with pairing code' (shows IP:port and a 6-digit code).\n"
                 "On this PC, from the project folder, using the port shown on that pairing screen:\n"
                 "    tools\\platform-tools\\adb.exe pair <G2 IP>:<pairing port>\n"
                 "then type the code when asked and run this script again.")
    sys.exit("Could not connect to the G2: " + "; ".join(f"{ep}: {msg}" for ep, msg in answers))


def foreground_package():
    out = adb("shell", "dumpsys activity activities").stdout
    m = re.search(r"topResumedActivity=ActivityRecord\{\S+ \S+ ([\w.]+)/", out)
    return m.group(1) if m else None


def percentile(sorted_vals, p):
    if not sorted_vals:
        return float("nan")
    k = min(len(sorted_vals) - 1, max(0, round(p / 100 * (len(sorted_vals) - 1))))
    return sorted_vals[k]


def record(pkg, seconds, label, match=None):
    adb("push", str(DEV_SCRIPT), "/data/local/tmp/gamemon_dev.sh")
    start = adb("shell", "date +'%m-%d %H:%M:%S.000'").stdout.strip()  # device clock, for the app's own log lines
    proc = subprocess.Popen(
        [str(ADB), "shell", f"sh /data/local/tmp/gamemon_dev.sh '{match or pkg}' {seconds}"],
        stdout=subprocess.PIPE, text=True, errors="replace")

    frames = {}      # layer -> set of present timestamps (ns)
    baseline = {}    # layer -> newest timestamp already in the buffer when we started
    samples = []     # per-second GPU samples
    layer = None
    seen_in_block = {}
    first_block = True
    for line in proc.stdout:
        line = line.rstrip("\n")
        if line.startswith("@S "):
            p = line.split()
            # @S i gpuclk busy total gpu_temp zone0 [fan_duty fan_rpm fan_mode skin_max battery_tenths]
            try:
                busy, total = float(p[3]), float(p[4])
                smp = {"t": int(p[1]), "mhz": float(p[2]) / 1e6,
                       "busy": 100 * busy / total if total else 0.0,
                       "gpu_c": float(p[5]) / 1000,
                       "cpu_c": float(p[6]) / 1000 if len(p) > 6 else None}
                if len(p) > 11:
                    duty, rpm = int(p[7]), int(p[8])
                    smp.update({"fan_pct": 100 * duty / FAN_PERIOD if duty >= 0 else None,
                                "fan_rpm": rpm if rpm >= 0 else None,
                                "fan_mode": int(p[9]) if p[9].lstrip("-").isdigit() else None,
                                "skin_c": int(p[10]) / 1000 if int(p[10]) > 0 else None,
                                "batt_c": int(p[11]) / 10 if int(p[11]) > 0 else None})
                samples.append(smp)
            except (ValueError, IndexError):
                pass
            seen_in_block = {}
        elif line.startswith("@L "):
            layer = line[3:]
            seen_in_block[layer] = []
        elif line == "@E":
            for lay, ts in seen_in_block.items():
                if first_block or lay not in baseline:
                    baseline[lay] = max(ts) if ts else 0
                frames.setdefault(lay, set()).update(t for t in ts if t > baseline[lay])
            first_block = False
            done = samples[-1]["t"] + 1 if samples else 0
            best = max(frames.values(), key=len, default=set())
            print(f"\r  {done:>3}/{seconds}s  frames captured: {len(best)}   ", end="", flush=True)
        elif layer and line and line[0].isdigit():
            parts = line.split()
            if len(parts) == 3:
                actual = int(parts[1])
                if 0 < actual < PENDING:
                    seen_in_block[layer].append(actual)
    proc.wait()
    print()

    app = app_metrics(start)
    if len(app) < 2 and (not frames or not any(frames.values())):
        print("No new frames were presented by", pkg, "during the run.")
        print("Layers seen:", list(baseline) or "none")
        return None

    s = samples[2:] or samples  # skip the first seconds of GPU stats
    res = {
        "label": label, "package": pkg, "seconds": seconds,
        "time": time.strftime("%Y-%m-%d %H:%M:%S"),
        "gpu_mhz": statistics.mean(x["mhz"] for x in s) if s else 0,
        "gpu_busy": statistics.mean(x["busy"] for x in s) if s else 0,
        "gpu_c_start": samples[0]["gpu_c"] if samples else None,
        "gpu_c_end": samples[-1]["gpu_c"] if samples else None,
        "samples": samples,
    }
    res.update(fan_summary(samples, start))
    if len(app) >= 2:
        # The app measures its own frames (GameNative does); trust that over SurfaceFlinger,
        # which does not track frame times for its game surface.
        # Each line covers the previous 10 s, so the first two reach back before the recording
        # started (often a loading screen or the walk to the spot). Keep them in the file but
        # leave them out of the averages.
        all_app, app = app, (app[2:] if len(app) > 6 else app)
        p95 = statistics.mean(r["p95_ms"] for r in app)
        res.update({
            "source": "app", "app_samples": all_app,
            "avg_fps": statistics.mean(r["fps"] for r in app),
            "low_fps": 1000 / p95 if p95 else 0.0,     # from the 95th-percentile frame time
            "frame_ms": p95,
            "cpu_pct": statistics.mean(r["cpu"] for r in app),
            "app_gpu_pct": statistics.mean(r["gpu"] for r in app),
        })
    else:
        best_layer = max(frames, key=lambda k: len(frames[k]))
        ts = sorted(frames[best_layer])
        dts = sorted((b - a) / 1e6 for a, b in zip(ts, ts[1:]))  # frame times, ms
        span = (ts[-1] - ts[0]) / 1e9 if len(ts) > 1 else 0
        p99 = percentile(dts, 99)
        res.update({
            "source": "sf", "layer": best_layer, "frames": len(ts),
            "avg_fps": (len(ts) - 1) / span if span else 0.0,
            "low_fps": 1000 / p99 if dts and p99 else 0.0,  # from the 99th-percentile frame time
            "frame_ms": statistics.median(dts) if dts else float("nan"),
            "cpu_pct": None,
        })
    out_dir = RESULTS / pkg
    out_dir.mkdir(parents=True, exist_ok=True)
    out = out_dir / f"{label}-{time.strftime('%Y%m%d-%H%M%S')}.json"
    out.write_text(json.dumps(res, indent=1))
    print_rows([res])
    print()
    print(f"saved {out.relative_to(ROOT)}")
    return res


FAN_PERIOD = 50000  # gpio5_pwm2 period in ns; duty 5000 = 10%
FAN_APP = "io.github.drakfix.g2fancurve"
FAN_MODES = {0: "curve", 1: "quiet", 4: "smart", 5: "sport", 6: "custom"}


def fan_summary(samples, start):
    """Fan/thermal/battery figures for the run plus the G2 Fan Curve app's own log lines.

    "controller" is who drove the fan: "curve" (Retroid fan_mode 0 with the G2 Fan Curve app running),
    "off" (fan_mode 0 but no curve app), or Retroid's stock mode name. Runs where it changed show "mixed".
    """
    fs = [x for x in samples if x.get("fan_pct") is not None]
    if not fs:
        return {}
    ver = re.search(r"versionName=(\S+)", adb("shell", f"dumpsys package {FAN_APP}").stdout or "")
    running = bool((adb("shell", f"pidof {FAN_APP}").stdout or "").strip())
    modes = {x["fan_mode"] for x in fs if x.get("fan_mode") is not None}
    if len(modes) > 1:
        controller = "mixed"
    elif modes == {0}:
        controller = "curve" if running else "off"
    else:
        controller = FAN_MODES.get(next(iter(modes), None), "?")
    skins = [x["skin_c"] for x in fs if x.get("skin_c")]
    batts = [x["batt_c"] for x in fs if x.get("batt_c")]
    log = adb("shell", f"logcat -d -v time -s FanCurve -T '{start}'").stdout or ""
    return {
        "fan_controller": controller,
        "fan_app_version": ver[1] if ver else None,
        "fan_on_pct": 100 * sum(1 for x in fs if x["fan_pct"] > 0) / len(fs),
        "fan_avg_pct": statistics.mean(x["fan_pct"] for x in fs),
        "fan_max_pct": max(x["fan_pct"] for x in fs),
        "fan_max_rpm": max((x["fan_rpm"] or 0) for x in fs),
        "skin_c_start": skins[0] if skins else None, "skin_c_max": max(skins) if skins else None,
        "batt_c_start": batts[0] if batts else None, "batt_c_max": max(batts) if batts else None,
        "fan_log": [l for l in log.splitlines() if "FanCurve" in l],
    }


APP_PERF = re.compile(r"PowerMetrics.*?fps=([\d.]+) p95=([\d.]+)ms .*?cpu=(\d+)%.*?gpu=(\d+)%")


def app_metrics(start):
    """Performance lines the app itself logged since `start` (device time).

    GameNative prints one every ~5 s: "fps=44.8 p95=24.2ms ... cpu=58%(PROC_STAT) gpu=59% ...".
    """
    out = adb("shell", f"logcat -d -v time -s PowerMetrics -T '{start}'").stdout or ""
    rows = []
    for m in filter(None, map(APP_PERF.search, out.splitlines())):
        fps, p95, cpu, gpu = float(m[1]), float(m[2]), int(m[3]), int(m[4])
        if fps > 0:
            rows.append({"fps": fps, "p95_ms": p95, "cpu": cpu, "gpu": gpu})
    return rows


def print_rows(rows):
    """low fps: from the 99th-percentile frame time (src sf) or the 95th (src app).
    fan: who drove it and average/max duty; skin/batt: start->max °C (blank for runs before fan logging)."""
    print(f"{'label':<24}{'avg fps':>8}{'low fps':>8}{'GPU MHz':>9}{'GPU %':>7}{'CPU %':>7}{'GPU C':>10}{'src':>5}"
          f"  {'fan (avg/max %)':<17}{'skin C':>9}{'batt C':>11}")
    for r in rows:
        temp = f"{r['gpu_c_start']:.0f}->{r['gpu_c_end']:.0f}" if r.get("gpu_c_start") is not None else "?"
        cpu = f"{r['cpu_pct']:.0f}" if r.get("cpu_pct") is not None else "-"
        low = r.get("low_fps", r.get("low1_fps", 0.0))
        fan = skin = batt = ""
        if r.get("fan_controller"):
            fan = f"{r['fan_controller']} {r['fan_avg_pct']:.0f}/{r['fan_max_pct']:.0f}"
        if r.get("skin_c_max") is not None:
            skin = f"{r['skin_c_start']:.0f}->{r['skin_c_max']:.0f}"
        if r.get("batt_c_max") is not None:
            batt = f"{r['batt_c_start']:.1f}->{r['batt_c_max']:.1f}"
        print(f"{r['label'][:23]:<24}{r['avg_fps']:>8.1f}{low:>8.1f}{r['gpu_mhz']:>9.0f}{r['gpu_busy']:>7.0f}"
              f"{cpu:>7}{temp:>10}{r.get('source', 'sf'):>5}  {fan:<17}{skin:>9}{batt:>11}")


def compare():
    if not RESULTS.exists():
        print("no results yet")
        return
    for game in sorted(p for p in RESULTS.iterdir() if p.is_dir()):
        # utf-8-sig: tolerate result files that were re-saved by a Windows tool with a BOM
        rows = [json.loads(f.read_text(encoding="utf-8-sig")) for f in sorted(game.glob("*.json"))]
        if rows:
            print(f"\n== {game.name}")
            print_rows(rows)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--label", help="name for this run, e.g. tuned-v2, baseline, stevenmxz-r3")
    ap.add_argument("--seconds", type=int, default=60)
    ap.add_argument("--pkg", help="app package (default: whatever is in the foreground)")
    ap.add_argument("--layer", help="match SurfaceFlinger layers by this text instead of the package name "
                                    "(only if a run reports no frames; see `adb shell dumpsys SurfaceFlinger --list`)")
    ap.add_argument("--compare", action="store_true", help="print a table of all saved runs")
    a = ap.parse_args()
    if a.compare:
        compare()
        return
    if not a.label:
        ap.error("--label is required (or use --compare)")
    ensure_connected()
    pkg = a.pkg or foreground_package()
    if not pkg:
        sys.exit("could not detect the foreground app; pass --pkg")
    print(f"recording {pkg} for {a.seconds}s as '{a.label}'")
    if a.layer and not re.fullmatch(r"[\w.\[\]()/#: -]+", a.layer):
        sys.exit("--layer may only contain letters, digits and . [ ] ( ) / # : - space")
    record(pkg, a.seconds, re.sub(r"[^\w.-]+", "_", a.label), a.layer)


if __name__ == "__main__":
    main()
