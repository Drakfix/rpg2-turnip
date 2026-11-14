# Findings

A running log of what has been measured on the Retroid Pocket G2 and what it means.
Newest first. Raw runs are in [`results/`](../results).

## Device facts

- SoC: Snapdragon G2 Gen 2 (`SG6275P`, board `pineapple`), GPU node `qcom,adreno-gpu-gen7-11-0`,
  `gpu_model=Adreno22`. Stock Vulkan driver 762.31 reports chip id `0x43030b00`.
- Mesa maps that id to `FD735` (a7xx gen2, 3 CCUs), the same entry as the Snapdragon 8s Gen 3.
- CPU: 3× A520 (cores 0–2, 1.9 GHz), 4× A720 (3–6, 2.6 GHz), 1× X4 (7, 2.8 GHz). No SVE.
- GPU governor `msm-adreno-tz`, 255–950 MHz; it cannot be pinned without root.

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
