#!/system/bin/sh
# Launch RetroArch content: sh ra_launch.sh [core .so name] [rom path]  (defaults: Snes9x + SMW hack)
set -- "${1:-snes9x_libretro_android.so}" "${2:-/storage/emulated/0/ROMs/SNES/Mario Game (SMW Hack).sfc}"
am start -n com.retroarch.aarch64/com.retroarch.browser.retroactivity.RetroActivityFuture \
  -e ROM "$2" -e LIBRETRO "/data/user/0/com.retroarch.aarch64/cores/$1" \
  -e CONFIGFILE /storage/emulated/0/Android/data/com.retroarch.aarch64/files/retroarch.cfg \
  -e IME com.android.inputmethod.latin/.LatinIME -e DATADIR /data/user/0/com.retroarch.aarch64 \
  --activity-clear-top
sleep 10
dumpsys activity activities | grep -m1 topResumedActivity
echo "launched_at_log_t=$(tail -1 /data/local/tmp/snes_thermal.log | cut -d, -f1)"
screencap -p /data/local/tmp/snes_check.png
