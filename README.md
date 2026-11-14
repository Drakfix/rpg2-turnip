# rpg2-turnip

Mesa **Turnip** (open-source Vulkan) drivers tuned for the **Retroid Pocket G2**
(Snapdragon G2 Gen 2, Adreno "A22"), plus the tools used to measure them on the device.

The A22 reports KGSL chip id `0x43030b00`, which upstream Mesa already treats as an
Adreno 735 (`FD735`). Upstream Turnip runs on it, but nothing in Mesa is tuned for this
chip. This project finds where Turnip loses time on the G2 against Qualcomm's own driver
and patches it, verifying each change in real games on the device.

> Status: hobby project, no warranty. Drivers are loaded per app by emulators
> (Eden, GameNative, Winlator, ...) and never replace the system driver, so switching
> back is one tap in the emulator's driver manager.

## Downloads

Grab a zip from [Releases](../../releases) and install it from the emulator's
GPU driver manager (in Eden: long-press the game → *GPU driver manager* → *Install*).

| Build | Mesa | Patches | State |
|---|---|---|---|
| **TUNED v4** | main `2b375c25fc` (27.0-devel), also a 26.3.0 build | 0001, 0002 | Stable. Works in Eden and GameNative (DXVK). |
| **TUNED v5rc** | main `2b375c25fc` | 0001–0004 | Release candidate. About 10% less GPU work per frame than v4 in BOTW; only measured in BOTW so far. |

## What the patches do

| Patch | Effect on the A22 | Evidence |
|---|---|---|
| [0001](patches/0001-tu-autotune-default-to-profiled-on-adreno-a22.patch) autotune → `profiled` | Picks SYSMEM/GMEM per render pass from measured timings instead of a bandwidth estimate. | Draw-heavy synthetic pass: GMEM 16–19 ms vs SYSMEM 7 ms. BOTW: about 3.5% less GPU work than upstream's `bandwidth`. |
| [0002](patches/0002-ir3-skip-sin-cos-range-reduction-on-adreno-a22.patch) skip sin/cos range reduction | Uses the native sin/cos like Qualcomm's driver does. | Bit-identical to the blob over 262144 inputs; a sin/cos-heavy shader was about 17% faster. |
| [0003](patches/0003-tu-hash-ir3-shader-debug-into-cache-uuid.patch) hash `IR3_SHADER_DEBUG` into the cache UUID | Test plumbing only: compiler options can be A/B tested without stale cached shaders. No effect when unset. | — |
| [0004](patches/0004-ir3-skip-opt-preamble-on-adreno-a22.patch) skip `nir_opt_preamble` | Stops hoisting uniform code into the shader preamble, which costs the A22 more than it saves (descriptor prefetch and early preamble are kept). | BOTW: 15.8 vs 17.5 Mcycles/frame with the pass, on par with the blob (15.3). |

All builds also use `-mcpu=cortex-a720+nosve` (the G2's CPU has no SVE), which cut
command-recording CPU time by about 6%, and `-Dplatform-sdk-version=36` so DXVK
(GameNative) sees every Vulkan extension it needs.

## Measured so far

Zelda: Breath of the Wild in Eden, fixed autosave (Akkala Citadel), 1X resolution,
30 FPS cap, 60 s per run. At a frame cap FPS can't show driver differences, so the
main metric is **GPU work per frame** = mean(GPU MHz × busy) / FPS, in millions of cycles.

| Driver | FPS | GPU work / frame | Eden VulkanWorker CPU |
|---|---|---|---|
| Qualcomm 762.31 (stock) | 28.9 | 15.3 | 69% |
| MrPurple T30 | 29.9 | 19.9 | 41% |
| TUNED v4 | 29.9 | 16.7–17.5 | 41% |
| **TUNED v5rc** | 29.9 | **15.8** | 41% |

So v5rc matches the stock driver's GPU efficiency and keeps Turnip's lower CPU cost.
Far Cry 2 in GameNative (v4 45.2 FPS vs T30 43.9) was CPU-bound, so GPU-side
tuning has little left to give there. Full notes and every run: [docs/FINDINGS.md](docs/FINDINGS.md).

## Building

Linux or WSL (Ubuntu 24.04 tested), no root needed:

```bash
bash scripts/setup_env.sh                 # NDK, Mesa checkout, meson/mako venv under ~/turnip-build
EXTRA_CFLAGS="-mcpu=cortex-a720+nosve" bash scripts/build_turnip.sh TUNED-v5
```

`build_turnip.sh` resets the Mesa tree, applies `patches/*.patch` in order and writes an
AdrenoTools zip to `out/`. Set `PATCH_DIR` to build with a different patch set.

## Testing on the device

Everything runs over adb (wireless debugging), without root. Put Google's
[platform-tools](https://developer.android.com/tools/releases/platform-tools) in
`tools/platform-tools/`.

- `test/tu_smoke.c`, `test/tu_bench.c`: load any driver `.so` straight from `/data/local/tmp`
  and run correctness checks (`--trig`, `--ext`) or per-shader-op timings.
- `scripts/gamemon.py`: records frame pacing (SurfaceFlinger or GameNative's own metrics),
  GPU clock/busy/temperature and fan data for whatever game is in the foreground.
  `record_run.bat` / `show_runs.bat` are double-click wrappers.
- `scripts/eden_threads.py`: per-thread CPU of Eden (its `GPU` thread runs the driver's
  command recording).
- `scripts/eden_botw_variant.py`: unattended A/B runs. Sets Mesa options through
  `debug.mesa.*` properties, restarts Eden, loads the BOTW save through the touch overlay
  and records. `scripts/compare_runs.py` prints the table.

Methodology and pitfalls (the GPU clock governor, thermals, Eden quirks): [docs/TESTING.md](docs/TESTING.md).

## Repository layout

```
patches/       Mesa patches, applied in order by build_turnip.sh
patches-exp/   experimental patches not in the default set
scripts/       build, packaging, measurement and automation
test/          on-device Vulkan smoke test and micro-benchmarks
results/       recorded runs (JSON) and thermal logs
device-info/   stock driver vulkaninfo and device probes
docs/          findings and testing notes
```

The fan research that began here became its own project:
[g2-fan-curve](https://github.com/Drakfix/g2-fan-curve).

## Credits

Turnip and ir3 are by the Mesa / freedreno developers; this repo only carries small
patches on top. Community drivers used for comparison: MrPurple (T30), StevenMXZ
(26.2 R4, 26.3 R3) and whitebelyash (Turnip 26.0).

## License

Scripts, tools and patches: [MIT](LICENSE). Mesa itself is MIT licensed.
