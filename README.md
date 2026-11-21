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
| **TUNED v6-rc1** (pre-release) | main `2b375c25fc` (27.0-devel) | 0001–0005 | v5 + UBWC screen buffers: a further 13–16% less GPU work in Eden (Odyssey, BOTW), because the screen no longer needs a GPU copy per frame. GameNative: same as v5. |
| **TUNED v5** (recommended) | main `2b375c25fc` (27.0-devel) | 0001–0004 | Tested in BOTW, Super Mario Odyssey (Eden) and AC Revelations (GameNative/DXVK). 10–18% less GPU work per frame than v4, no BOTW terrain flicker, water no longer milky in Eden's Balanced mode. Same binary as v5-rc2. |
| TUNED v4 | main `2b375c25fc`, also a 26.3.0 build | 0001 (old `profiled` version), 0002 | Known issue: terrain flicker in BOTW. Kept for comparison. |
| TUNED v5rc / v5rc2 | main `2b375c25fc` | — | Release candidates, superseded by v5. |

## What the patches do

| Patch | Effect on the A22 | Evidence |
|---|---|---|
| [0001](patches/0001-tu-autotune-default-to-prefer-sysmem-on-adreno-a22.patch) autotune → `prefer_sysmem` | Renders in SYSMEM instead of guessing GMEM vs SYSMEM per render pass. (v4/v5rc used `profiled`, whose random re-tries of GMEM caused BOTW's terrain flicker.) | Draw-heavy synthetic pass: GMEM 16–19 ms vs SYSMEM 7 ms. BOTW: no one-frame flashes, same or less GPU work than `profiled` or `bandwidth`. |
| [0002](patches/0002-ir3-skip-sin-cos-range-reduction-on-adreno-a22.patch) skip sin/cos range reduction | Uses the native sin/cos like Qualcomm's driver does. | Bit-identical to the blob over 262144 inputs; a sin/cos-heavy shader was about 17% faster. |
| [0003](patches/0003-tu-hash-ir3-shader-debug-into-cache-uuid.patch) hash `IR3_SHADER_DEBUG` into the cache UUID | Test plumbing only: compiler options can be A/B tested without stale cached shaders. No effect when unset. | — |
| [0004](patches/0004-ir3-skip-opt-preamble-on-adreno-a22.patch) skip `nir_opt_preamble` | Stops hoisting uniform code into the shader preamble, which costs the A22 more than it saves (descriptor prefetch and early preamble are kept). | BOTW: 15.8 vs 17.5 Mcycles/frame with the pass, on par with the blob (15.3). |
| [0005](patches/0005-vk-android-request-ubwc-swapchain-buffers.patch) UBWC swapchain buffers | Asks gralloc for compressed screen buffers (usage bit `0x10000000`) like Qualcomm's driver. With linear buffers the G2's rotated panel forces Android to composite the game on the GPU every frame. Idea: StevenMXZ; own implementation. `debug.mesa.android.ubwc.swapchain=0` disables it. | Odyssey 6.04 → 5.06 Mcycles/frame (−16%, below stock), BOTW 13.5 → 11.8 (−13%); `DEVICE/DEVICE` composition like stock. |

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
| **TUNED v5** | 29.9 | **15.8** | 41% |

So v5 matches the stock driver's GPU efficiency and keeps Turnip's lower CPU cost.

Super Mario Odyssey in Eden (Wooded Kingdom, 1X, 60 FPS target):

| Driver | FPS | GPU clock | GPU work / frame | Eden VulkanWorker CPU |
|---|---|---|---|---|
| Qualcomm 762.31 (stock) | 56.7 | 500 MHz | 5.7 | 52% |
| MrPurple T30 | 58.8 | 662 MHz | 8.7 | 38% |
| TUNED v4 | 58.8 | 577–612 MHz | 7.2–7.5 | 37% |
| **TUNED v5** | **58.8** | **500 MHz** | **6.0** | 37% |

Here FPS is limited by Eden's own GPU-emulation thread, so all Turnip builds reach about 59 FPS;
v5 gets there at the lowest GPU clock (18% less GPU work than v4, 31% less than T30), while the
stock driver loses about 2 FPS to its higher CPU cost.

Windows games in GameNative are limited by the CPU (x86 translation): Assassin's Creed
Revelations runs at 35–38 FPS on v5, T30 and v4 alike, and Far Cry 2 at 44–45 FPS on v4 and
T30. v5 brings no regression there, and GPU-side tuning has little left to give. Full notes and every run: [docs/FINDINGS.md](docs/FINDINGS.md).

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
- `scripts/eden_variant.py`: unattended A/B runs. Sets Mesa options through
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
patches on top.

Community drivers we benchmarked against and studied for ideas (thank you!):
- MrPurple666: [purple-turnip](https://github.com/MrPurple666/purple-turnip) (T30)
- StevenMXZ: [Adreno-Tools-Drivers](https://github.com/StevenMXZ/Adreno-Tools-Drivers)
  (26.2 R4, 26.3 R3, 27.0.0-R2). His notes on UBWC swapchain buffers are a lead we're following.
- whitebelyash: [AdrenoToolsDrivers](https://github.com/whitebelyash/AdrenoToolsDrivers) (Turnip 26.0)
- AlexSanz0902: [TurnipZ](https://github.com/AlexSanz0902/turnip-sd865-z) (0.4.0)
- Balemuni: [Balemuni's Aurora](https://github.com/Balemuni/Balemunis-Aurora) (Apex Universal V2).
  The GCM and shader-cache ideas we're testing come from its notes.
- K11MCH1: [AdrenoToolsDrivers](https://github.com/K11MCH1/AdrenoToolsDrivers)

Any idea or code we take from another driver is credited in the patch that uses it.
Comparisons: [docs/FINDINGS.md](docs/FINDINGS.md).

## License

Scripts, tools and patches: [MIT](LICENSE). Mesa itself is MIT licensed.
