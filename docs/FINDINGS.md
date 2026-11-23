# Findings

A running log of what has been measured on the Retroid Pocket G2 and what it means.
Newest first. Raw runs are in [`results/`](../results).

## Device facts

- SoC: Snapdragon G2 Gen 2 (`SG6275P`, board `pineapple`), GPU node `qcom,adreno-gpu-gen7-11-0`,
  `gpu_model=Adreno22`. Stock Vulkan driver 762.31 reports chip id `0x43030b00`.
- Mesa maps that id to `FD735` (a7xx gen2, 3 CCUs), the same entry as the Snapdragon 8s Gen 3.
- CPU: 3× A520 (cores 0–2, 1.9 GHz), 4× A720 (3–6, 2.6 GHz), 1× X4 (7, 2.8 GHz). No SVE.
- GPU governor `msm-adreno-tz`, 255–950 MHz; it cannot be pinned without root.

## 2026-11-23: Issue #1 follow-up (TP UBWC flag hint, Trails in the Sky 2nd Chapter Demo)

**`enable_tp_ubwc_flag_hint=0` in Eden** (v6-rc1, hint on vs off, 20 s flicker capture per run):

| | Hint on (default) | Hint off |
|---|---|---|
| Odyssey one-frame flashes | 0, 0 | 6, 9 |
| Odyssey, UBWC swapchain off too | 0 | 49, 61 |
| BOTW one-frame flashes | 0 | 107, 157 |

Speed is the same, but with the hint off the image jumps for single frames in both games, with or
without patch 0005. So the hint stays on by default; turning it off is a per-game workaround.

**Black-frame flicker in the Trails demo under GameNative** (~40% black frames on the title screen) is
not the driver: identical on v6-rc1, v5 and T30, and with Wrapper or Wrapper-v2. It is GameNative's
**Gamescope** display server: Display Server = X11 with Vulkan rendering fixes it (SurfaceFlinger
rendering brings it back). Measured with `scripts/gn_trails_probe.py` + `scripts/black_frames.py`.

Test note: Android `debug.mesa.*` properties don't reach Turnip inside GameNative (it runs under
Wine/Box64); set options in the container's **Environment** tab instead (e.g. `FD_DEV_FEATURES`).

## 2026-11-21: UBWC swapchain buffers (idea from StevenMXZ): −13 to −16% GPU work

**Finding.** `dumpsys SurfaceFlinger` with Odyssey running in Eden:

| Driver | Game-window buffers | Layer composition |
|---|---|---|
| Stock Qualcomm 762.31 | usage `0x10000b00`, `RGBA_8888_UBWC`, compressed | `DEVICE/DEVICE` |
| Turnip (v5, and every community Turnip) | usage `0xb00`, `RGBA_8888`, linear | `DEVICE/CLIENT` |

Qualcomm's driver adds gralloc's private usage bit `0x10000000` (Qualcomm "alloc UBWC") to
swapchain buffers; Mesa doesn't. The G2's panel is mounted rotated 90°, and with linear buffers the
display hardware doesn't take the game layer: SurfaceFlinger falls back to compositing (copying and
rotating) the full 1920×1080 frame **on the GPU, every frame**. StevenMXZ's patch notes
(`add_ubwc_swapchain_usage.py`) pointed us at the missing usage bit.

**Patch 0005** (independent implementation; currently in `patches-exp/`, opt-in with
`debug.mesa.android.ubwc.swapchain=1`): adds the bit for colour-attachment swapchain buffers in all
three paths Android uses (`GetSwapchainGrallocUsage`, `…Usage2`, and the
`VkAndroidHardwareBufferUsageANDROID` query, only for optimal-tiled, GPU-framebuffer buffers without
CPU access). Mesa's import side already reads Qualcomm's gralloc handle (`gmsm` marker + UBWC flag
`0x08000000`) and switches the image to `DRM_FORMAT_MOD_QCOM_COMPRESSED`. On the G2 the log shows
all four swapchain buffers imported as UBWC, SurfaceFlinger reports `RGBA_8888_UBWC` +
`DEVICE/DEVICE` like the stock driver, and the picture is correct.

**Measured** (v5 + 0005, switch off vs on, interleaved, cooldown before every run):

| | off | **on** |
|---|---|---|
| Odyssey FPS | 59.6, 59.7 | 59.7, 60.0 |
| Odyssey GPU busy @ 500 MHz | 72.0%, 71.8% | **60.5%, 60.8%** |
| Odyssey work/frame | 6.04, 6.03 | **5.06, 5.07 (−16%)** |
| BOTW GPU clock | 597, 599 MHz | **516, 514 MHz** |
| BOTW work/frame | 13.53, 13.56 | **11.77, 11.75 (−13%)** |

No flicker, picture correct. In Odyssey this puts Turnip below the stock driver (5.7).

**v6-rc1** (patches 0001–0005, UBWC on by default; `debug.mesa.android.ubwc.swapchain=0` turns it
off): Eden shows UBWC buffers + `DEVICE/DEVICE` with no property set. In GameNative (AC Revelations)
the game runs correctly at the same speed, but GameNative allocates its own buffers for its X server,
and those stay linear (`modifier 0x0`), so there's no gain there and no harm. The gain is
GPU time the system compositor no longer spends; it should apply to every Vulkan game in a full-screen
window on the G2.

## 2026-11-20: ir3 Global Code Motion (idea from Balemuni's Apex notes): no effect

`nir_opt_gcm` is already in Mesa's ir3 optimisation loop, off by default, and switched with
`GCM=1` (with value numbering, what Balemuni ships) or `GCM=2`. On Android that's
`debug.mesa.gcm`. It isn't part of any cache key, so every run used a unique `IR3_SHADER_DEBUG`
string (patch 0003) to force fresh shaders. Driver: v5. Cooldown before every run.

| Setting | Odyssey FPS | Odyssey GPU busy @ 500 MHz | BOTW FPS | BOTW work/frame |
|---|---|---|---|---|
| GCM off | 59.9, 59.8 | 71.8%, 72.1% | 30.0, 30.0 | 13.74, 14.05 |
| GCM=1 | 59.9, 59.9 | 71.9%, 71.7% | 30.0, 29.1 | 14.05, 13.88 |
| GCM=2 | 59.6, 59.8 | 71.7%, 71.5% | — | — |

All within run-to-run noise; shader micro-benchmarks unchanged too, and no flicker. Not adopted.
Side result: v5's Odyssey FPS here (59.8–59.9) matches the other drivers, so the 0.4 FPS gap in the
comparison below was noise.
Note: Odyssey at 1X keeps the GPU at its 500 MHz floor, so only GPU busy % can show a difference
there. Future GPU-side tests in Odyssey should use a higher resolution.

## 2026-11-20: v5 vs the newest community drivers (TurnipZ, StevenMXZ, Balemuni)

Studied for ideas (credits below). Downloaded from each author's GitHub release, checked
(only `meta.json` + the library, aarch64, standard system libraries, standard `HMI` export;
Balemuni's zip matches the SHA-256 on their README), installed in Eden.

| Driver | Mesa base | Extensions | Source |
|---|---|---|---|
| **TUNED v5** | 27.0-dev `2b375c25fc` | 174 | this repo |
| TurnipZ 0.4.0 (AlexSanz0902) | 27.0-dev `3228709760` | 142 | not published |
| Turnip v27.0.0-R2 (StevenMXZ) | 27.0-dev `2648b2059f` | 173 | build scripts and patches, GPL-3.0 |
| Apex Universal V2 (Balemuni) | 26.3-dev `b9a2bf372b` | 140 | not published; changes described in notes |

TurnipZ and Balemuni lack 32 extensions DXVK uses (maintenance4–7, synchronization2,
dynamic_rendering, …), probably built for an older Android level (the same mistake we made
before `PLATFORM_SDK=36`), so they're unlikely to run Windows games in GameNative. Eden doesn't
need them.

**Shader micro-benchmark** (`run_fsmodes.sh`, 950 MHz): all four tie on mul/add, sqrt, exp/log and
div/mix; v5 is 14–16% faster on sin/cos, mix and dynamic-loop shaders and 7% on atan (patch 0002).
All three others still apply upstream's sin/cos range reduction (error 0.61 at 1e7).

**Games** (cooldown to ≤40 °C before every run, 20 s flicker capture + 60 s measurement):

| Driver | Odyssey FPS | Odyssey GPU MHz | Odyssey work/frame | BOTW FPS | BOTW work/frame | flashes |
|---|---|---|---|---|---|---|
| **v5** | 59.6, 59.4 | **500** | **6.02, 6.03** | 29.9 | **13.8** | 0 |
| TurnipZ 0.4.0 | 59.7, 59.9 | 624–633 | 7.47, 7.58 | 30.0 | 14.9 | 0 |
| StevenMXZ R2 | 60.0, 59.8 | 617–632 | 7.71, 7.52 | 29.2 | 15.0 | 0 |
| Balemuni V2 | 59.9, 59.9 | 635–641 | 8.00, 7.76 | 29.7 | 15.0 | 0 |

- v5 needs **20–25% less GPU work per frame in Odyssey** and **7–8% less in BOTW** than all three,
  and is the only one that stays at the 500 MHz clock floor. Same picture, cooler GPU.
- FPS is equal: Odyssey at 1X is limited by Eden's GPU-emulation thread (about 81% of a core on every
  driver), BOTW by its 30 FPS cap. v5 is about 0.4 FPS lower in Odyssey in both rounds, under 1%;
  possibly the GPU running at its floor clock. To watch at higher resolutions.
- Eden's submission-thread CPU is the same on all (35–38%): no CPU-side gain to borrow here.
- No one-frame flashes for any driver at these spots.

**Ideas taken from them, still to test on the G2** (credited when used):
- StevenMXZ: request UBWC-compressed swapchain buffers like Qualcomm's driver does (done 2026-11-21, patch 0005, see above)
  (`add_ubwc_swapchain_usage.py`), and check the gralloc UBWC detection (`fix_gralloc_flushall.py`).
  GPL-3.0: we'll write our own implementation, or keep any copied code GPL-3.0 and marked as such.
- Balemuni: ir3 Global Code Motion (tested 2026-11-20, no effect, see above) and a
  4 GB shader cache.

## 2026-11-20: TUNED v5 released

v5-rc2 promoted to **TUNED v5** (GitHub release `v5`, marked Latest). Same driver binary as
v5-rc2 (sha256 `b18c8794a70eca71…`), only the package name in `meta.json` changed. Verified in
BOTW, Super Mario Odyssey and AC Revelations (sections below).

## 2026-11-14: Assassin's Creed Revelations (GameNative / DXVK), v5rc2 vs T30 and v4

Setup: Constantinople square where the save loads, Proton 10 arm64ec + DXVK + Gamescope,
Wrapper-v2. Each run: cooldown to ≤40 °C first (`scripts/cooldown.py`), 20 s flicker capture,
60 s measurement. Automated with `scripts/gn_acr_run.py` and `scripts/gn_driver.py`.

| Driver | FPS | GPU MHz | GPU busy | CPU |
|---|---|---|---|---|
| **v5rc2** | 35.5, 37.7, 36.2 | 632–646 | 65–68% | 54–57% |
| T30 | 35.4, 37.0 | 636–637 | 66–67% | 56% |
| TUNED v4 | 36.3 (1 run) | 630 | 66% | 55% |

- **No regression and no gain.** All three are within run-to-run noise. GameNative's own HUD shows
  "LIMIT CPU": the x86 translation is the bottleneck here, as with Far Cry 2, so GPU-side driver
  changes can't move FPS in this scene.
- The flicker detector isn't usable in this scene: T30 runs showed "flash events", but on
  inspection they were townspeople walking past (HUD and scene intact). No glitches were seen in
  v5rc2 screenshots.
- Thermals: the hottest sensor (sys-therm) reaches about 58 °C after a minute of AC with the fan
  curve at 70%; it takes 3 minutes to cool back to 40 °C.
- v5rc2 has to be imported separately in GameNative (Settings > Driver Manager > Import ZIP);
  it then appears in each game's *Graphics Driver Version* list.

## 2026-11-14: Super Mario Odyssey (Eden), v5rc2 vs v4, T30 and stock

Setup: Wooded Kingdom, Resume spawns at the Odyssey ship (foggy forest), 1X resolution,
60 FPS target, standing still. Drivers interleaved in two rounds (v4 → T30 → stock → v5rc2),
each run 20 s flicker capture + 60 s measurement. Drivers switched automatically with
`scripts/eden_driver.py`.

| Driver | FPS (runs) | GPU MHz | GPU work / frame | Eden `GPU` thread | `VulkanWorker` | flashes |
|---|---|---|---|---|---|---|
| **v5rc2** | 58.9, 59.5, 57.9 | **500** | **6.0, 6.0, 6.1** | 81% | 37% | 0 |
| TUNED v4 | 59.0, 58.6 | 577–612 | 7.45, 7.17 | 81–82% | 37% | 0 |
| T30 | 58.8 | 662 | 8.70 | 81% | 38% | 0 |
| Stock 762.31 | 56.7, 56.6 | 500 | 5.66, 5.70 | 79% | **51–53%** | 0 |

- **v5rc2 needs about 18% less GPU work per frame than v4 and 31% less than T30.** The governor
  holds it at the 500 MHz floor (v4 577–612 MHz, T30 662 MHz), so it runs cooler for the
  same picture. It's within 6% of the stock driver.
- **FPS is limited by Eden's GPU thread** (about 81% of a core on every Turnip build), not by
  the GPU, so all Turnip builds land at about 59 FPS. The stock driver loses about 2.3 FPS because
  its submission thread costs 51–53% of a core vs about 37% for Turnip.
- No one-frame flashes with any driver here, including v4's `profiled` autotune.
- The `500 MHz` rows are at the governor's minimum clock, so their work figure is an upper
  bound; the real GPU need may be lower still.

## 2026-11-14: BOTW terrain flicker → autotune default changed to `prefer_sysmem`

Reported: ground textures in BOTW flicker on TUNED v2/v4/v5rc and on the stock driver,
but not on MrPurple T30 or whitebelyash Turnip V2.

- Eden's whole startup log (features, workarounds, per-game overrides) is identical for T30
  and v5rc apart from the driver name, so Eden takes the same path; the driver differs.
- `scripts/flicker_score.py` scores 12–20 s screen recordings of a static scene: it counts
  16×16 blocks that change for exactly one frame and return. A surface rendered wrong for one
  frame lights up hundreds at once; rain and particles stay under about 50.
- Patch 0001's `profiled` autotune chooses GMEM or SYSMEM per render pass at random, weighted
  by timings, and never drops below a 1% chance for the other mode. In BOTW the occasional
  GMEM frame renders a floor/terrain pass differently: one-frame flashes of 700+ blocks.
  T30 and V2 use upstream's `bandwidth` estimate, which doesn't flip.

Rainy dock save, 20 s capture + 60 s measurement per run:

| Autotune | Flash events | max blocks | GPU work / frame |
|---|---|---|---|
| `profiled` (v4, v5rc) | 10, 1 (plus 1, 2 in 12 s runs) | 731–775 | 12.75, 12.86 |
| `prefer_sysmem` | 0, 0 | 19–21 | 12.63, 11.77 |
| `bandwidth` | 0, 0 | 19–54 | 12.64, 12.52 |
| **v5rc2** (patch 0001 → `prefer_sysmem`) | **0, 0** | 20–23 | **12.54, 11.69** |

**Play-tested by the user on v5rc2:** the terrain flicker is gone, and so is a long-standing
Eden issue where puddles and water rendered milky white unless the game was started in
*Accurate* GPU mode and switched to *Balanced* afterwards. With v5rc2, water renders correctly
in Balanced from the start. That points to the same cause: some water/reflection pass rendered
differently whenever it landed in GMEM.

`prefer_sysmem` was chosen over `bandwidth` because it never uses GMEM (so it can't hit this
kind of difference anywhere) and avoids the 2.5× GMEM slowdown on draw-heavy passes found on
2026-11-10. It is still switchable with `debug.mesa.tu.autotune.algo`.
The stock driver flickering too suggests it has a similar adaptive mode choice.

## 2026-11-13: closing the GPU gap to the stock driver (BOTW)

Setup: Eden (Switch emulator), Zelda: BOTW 1.6.0, Akkala Citadel autosave, 1X resolution,
30 FPS cap, standing still, 60 s per run, 75 s settle after loading.
Metric: GPU work per frame = mean(MHz × busy%) / FPS, in millions of cycles.
Base runs of the same build vary by about ±2%.

### Drivers compared

| Driver | FPS | MHz | busy | work/frame | Eden `GPU` thread | `VulkanWorker` |
|---|---|---|---|---|---|---|
| Stock 762.31 | 28.9 | 645 | 68% | 15.3 | 70% | **69%** |
| T30 | 29.9 | 840 | 71% | 19.9 | 70% | 41% |
| TUNED v4 (two runs) | 29.8 / 29.9 | 710 | 71% | 16.9 / 17.0 | 70% | 40–41% |

Turnip already uses about 28% of a core less than the blob on submission. The blob needed
about 10% less GPU work.

### Render-mode and memory options (Turnip debug properties)

| Option | work/frame | Notes |
|---|---|---|
| base (several runs) | 16.9–17.6 | |
| `TU_DEBUG=sysmem` | 17.4 | `profiled` autotune already behaves like this |
| `TU_AUTOTUNE_ALGO=bandwidth` (upstream default) | 17.8 | patch 0001 is worth about 3.5% |
| `TU_AUTOTUNE_ALGO=profiled_imm` | 16.4 | promising, needs repeats |
| `TU_DEBUG=nobinmerging` | 16.7 | within noise |
| `TU_DEBUG=nocb` | 17.3 | no change |
| `TU_DEBUG=noubwc` | 18.5 | worse |
| `TU_DEBUG=nolrzfc`, `forcecb` | 17.2, 17.8 | FPS dropped to 28 |
| `TU_DEBUG=gmem`, `nolrz` | — | BOTW froze |

### Shader compiler options (`IR3_SHADER_DEBUG`, fresh caches via patch 0003)

| Option | work/frame |
|---|---|
| base (4 runs) | 16.60, 16.82, 16.44, 16.79 |
| **`nopreamble`** (3 runs) | **15.69, 15.23, 15.18** |
| `nopreamble` + `profiled_imm` | 15.34 |
| `thread64` | 16.40 |
| `noaliastex` | 16.72 |
| `noaliasrt` | 17.52 |
| `nodescprefetch` | 18.10 |
| `noearlypreamble` | 19.71 |

The preamble optimization pass (`nir_opt_preamble`, which moves uniform math into a
per-draw preamble) is the cost. The early preamble mechanism and descriptor prefetch help.

### Result: patch 0004 (TUNED v5rc)

Skips only `nir_opt_preamble` on chip `0x43030b00`. Interleaved runs in one session:

| Setting | work/frame | mean |
|---|---|---|
| v5rc default | 16.14, 15.23, 15.94 | **15.77** |
| v5rc with `optpreamble` (v4 behaviour) | 18.13, 17.35, 16.88 | 17.45 |

That is about 10% less GPU work and level with the blob. Synthetic per-op shader benchmarks
(`tu_bench fsmodes`) are unchanged within 1%. Not yet verified: other Eden games and GameNative.

## 2026-11-11: GameNative (DXVK) and the missing extensions

- Our first builds refused to launch Windows games in GameNative while T30 worked.
  Cause: building with `-Dplatform-sdk-version=34`. Mesa hides every extension that
  Android version doesn't allow, so 11 API-35 and 3 API-36 extensions DXVK needs were missing.
  `build_turnip.sh` now uses `PLATFORM_SDK=36` independent of the compiler API level.
  Tip: when a driver works in one app but not another, diff `tu_smoke --ext` output first.
- Far Cry 2 (same spot): v4 45.2 FPS, T30 43.9 FPS, GPU about 60% busy at 835 MHz,
  CPU about 60%: CPU-bound. Pinning GameNative to cores 4–7, 3–7 or all 8 made no
  difference (session-to-session spread is 1–2 FPS).
- v4 runs Assassin's Creed Revelations without T30's graphical glitches.

## 2026-11-10: synthetic baseline

- Community drivers (whitebelyash 26.0, StevenMXZ 26.2 R4 / 26.3 R3, MrPurple T30) and
  ours all behave the same; none has A22-specific handling.
- Draw-call-heavy render pass (10k small depth-tested quads, 1080p): GMEM 16–19 ms vs
  SYSMEM 7 ms at the same clock. Upstream's `bandwidth` autotune picks GMEM → patch 0001.
  Caveat: autotune keys render passes by framebuffer, so after a workload change `profiled`
  can take 300–900 frames to re-adapt.
- Turnip was about 17% slower than the blob on a sin/cos-heavy shader because ir3 wraps every
  sin/cos argument into [-pi, pi]. The A22's native sin/cos are accurate over the full range →
  patch 0002 (bit-identical to the blob).
- Other ops tie with the blob; Turnip is faster on division/mix (about 10%) and atan/tan
  (about 24%), and about 1.8× faster at pipeline compiles.
