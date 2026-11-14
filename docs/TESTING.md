# Testing on the G2

How measurements in this repo are taken, and the traps that produced wrong numbers before.

## Connection

Wireless debugging over Wi-Fi, no root. The scripts pick the device automatically and
reconnect through mDNS after a restart. If `adb connect` says "failed to connect" while the
port is open, the G2 has dropped this PC's pairing: pair again with
`adb pair <G2 IP>:<pairing port>` and type the code shown on the G2.

Only write to `/data/local/tmp` and `/sdcard/Download`. Never write under `Android/data`
(doing that once broke the adopted SD card).

## Reading numbers correctly

- **The GPU clock governor decides most of the variance.** It can't be pinned, so the same
  driver can read 11 ms or 17 ms depending on what ran before. Only compare runs at a similar
  clock, alternate drivers (A-B-A), and repeat.
- **At a frame cap, FPS hides driver differences.** Compare GPU work per frame
  (mean MHz × busy / FPS) and the CPU of Eden's `GPU` and `VulkanWorker` threads instead.
- **Thermals drift during a session.** Put a baseline at the start and the end of every batch.
- **Fresh vs warm shader caches.** A new driver build (or a new `IR3_SHADER_DEBUG` value with
  patch 0003) starts with empty caches; let it settle (75 s) before recording.

## Mesa options without rebuilding

Mesa reads `TU_DEBUG`, `TU_AUTOTUNE_ALGO`, `IR3_SHADER_DEBUG` and the rest from Android
properties: lowercase, `_` → `.`, prefixed with `debug.mesa.`

```bash
adb shell setprop debug.mesa.tu.debug sysmem
adb shell setprop debug.mesa.tu.autotune.algo profiled_imm
adb shell setprop debug.mesa.ir3.shader.debug nopreamble
```

Turnip reads them once per process, so the emulator must be force-stopped between variants.
They are cleared on reboot; set them to `''` to clear them by hand.

## Driving Eden unattended

Eden ignores injected key and gamepad events, so automation uses its touch overlay
(screen is 1920×1080 landscape):

- In-game menu: swipe in from the left edge. *Show controller* turns the overlay on
  (it starts hidden on every launch).
- Overlay A button about (1460, 855).
- After a restart the game carousel position changes: search for the game first, then tap
  the centred card. A long press on the card opens the game's properties
  (GPU driver manager, per-game settings).
- Eden sometimes crashes at launch in `NPad::ControllerUpdate` when a motion-sensor event
  arrives early. That's an Eden bug, not the driver; the scripts retry.

`scripts/eden_variant.py` does all of this and refuses to send input if the expected game or
screen isn't in front. `--game botw` (default) loads the latest BOTW save; `--game smo`
resumes Super Mario Odyssey (it always spawns at the Odyssey ship). New games are a profile
in its `GAMES` table:

```bash
python scripts/eden_variant.py --label v5-base
python scripts/eden_variant.py --game smo --label smo-v5rc2 --flicker 20
python scripts/eden_variant.py --label v5-optpre --ir3-debug optpreamble --load-wait 75
python scripts/compare_runs.py --match v5-
```

### Switching drivers per game

`scripts/eden_driver.py` selects the driver Eden uses for one game: it long-presses the game's
card, opens the GPU driver manager and pages through the list. Card titles are read with the
Windows built-in OCR engine (`scripts/win_ocr.ps1`, nothing to install); radio buttons are found
by image matching (`scripts/eden_assets/`). It taps only the matching card's radio button, never
the trash icons or the Fetch/Install buttons, and checks afterwards that the radio button really
shows as selected.

```bash
python scripts/eden_driver.py --game smo --driver v5rc2   # stock, v4, v4x, v5rc, v5rc2, t30
```

## Catching texture flicker

`--flicker N` on `eden_variant.py` screen-records N seconds after loading and runs
`scripts/flicker_score.py`. Look at `flash_events(>=100 blocks)` and `max_flash_blocks`:
rain and particles stay under about 50 blocks, while a surface rendered wrong for one frame
shows hundreds. A heat map of where pixels spiked is saved next to the video.

```bash
python scripts/eden_variant.py --label test --flicker 20 --no-record
```

## Correctness checks

```bash
adb push out/libvulkan_freedreno.so.<variant> /data/local/tmp/drv/x.so
adb shell 'cd /data/local/tmp && ./tu_smoke drv/x.so && ./tu_smoke drv/x.so --trig'
adb shell 'cd /data/local/tmp && sh run_fsmodes.sh 120 4 a:drv/a.so b:drv/b.so'
```

`tu_smoke --ext` lists the exposed extensions; diff it between drivers when one launches a
game and another doesn't.
