#!/system/bin/sh
# Fan-off thermal characterisation for the Retroid Pocket G2. Run detached:
#   nohup sh /data/local/tmp/fan_off_thermal.sh > /data/local/tmp/fan_off_thermal.log 2>&1 < /dev/null &
# Phases (fan OFF): idle home screen -> NES (RetroArch/Mesen) -> heavy (NES + Vulkan bench + CPU spinners)
# then fan back to the original mode (Smart) for a cool-down phase.
# Guard: CPU > CPU_LIMIT, any sys-therm > SKIN_LIMIT or battery > BATT_LIMIT ends fan-off at once.
# Always restores fan_mode and screen_off_timeout on exit.
# Env overrides: GAME_ROM, GAME_CORE, GAME_TAG (phase label), IDLE_S, GAME_S, HEAVY_S (0 = skip), COOL_S
IDLE_S=${IDLE_S:-360}; GAME_S=${GAME_S:-480}; HEAVY_S=${HEAVY_S:-360}; COOL_S=${COOL_S:-240}
CPU_LIMIT=62000; CPU_HARD=75000; SKIN_LIMIT=46000; BATT_LIMIT=42000
HOT=0   # consecutive samples over CPU_LIMIT (core temps spike briefly; trip on 2 in a row or one over CPU_HARD)
F=/sys/class/gpio5_pwm2
ROM=${GAME_ROM:-"/storage/emulated/0/ROMs/nes/Super Mario Bros. (Pure Controls).nes"}
CORE=${GAME_CORE:-/data/user/0/com.retroarch.aarch64/cores/mesen_libretro_android.so}
TAG=${GAME_TAG:-nes}
CFG=/storage/emulated/0/Android/data/com.retroarch.aarch64/files/retroarch.cfg

ORIG_MODE=$(settings get system fan_mode)
ORIG_TIMEOUT=$(settings get system screen_off_timeout)
LOADPIDS=""
FANOFF=0

# Resolve zones by type once.
CPUZ=""; GPUZ=""; SKINZ=""; Z0=/sys/class/thermal/thermal_zone0/temp; BATZ=""
for z in /sys/class/thermal/thermal_zone*; do
  t=$(cat $z/type)
  case "$t" in
    cpu-*) CPUZ="$CPUZ $z/temp";;
    gpuss-*) GPUZ="$GPUZ $z/temp";;
    sys-therm-*) v=$(cat $z/temp); [ "$v" -gt 0 ] && SKINZ="$SKINZ $z/temp";;
    battery) BATZ=$z/temp;;
  esac
done
zmax() { m=-99999; for f in "$@"; do v=$(cat $f 2>/dev/null) || continue; [ "$v" -gt "$m" ] && m=$v; done; echo $m; }

stop_load() { for p in $LOADPIDS; do kill $p 2>/dev/null; done; LOADPIDS=""; pkill -f tu_bench 2>/dev/null; }
fan_restore() { settings put system fan_mode "$ORIG_MODE"; FANOFF=0; }
cleanup() {
  stop_load
  fan_restore
  settings put system screen_off_timeout "$ORIG_TIMEOUT"
  input keyevent KEYCODE_HOME
  sleep 6
  echo "RESTORED fan_mode=$(settings get system fan_mode) timeout=$(settings get system screen_off_timeout) state=$(cat $F/state) duty=$(cat $F/duty) rpm=$(cat $F/speed)"
}
trap cleanup EXIT
trap 'exit 1' INT TERM HUP

PHASE=init; T0=$(date +%s)
sample() {
  c=$(zmax $CPUZ); g=$(zmax $GPUZ); s=$(zmax $SKINZ); b=$(cat $BATZ 2>/dev/null)
  echo "$(( $(date +%s) - T0 )),$PHASE,$(cat $Z0),$c,$g,$s,$b,$(cat $F/state),$(cat $F/duty),$(cat $F/speed),$(settings get system fan_mode)"
  if [ "$c" -gt $CPU_LIMIT ]; then HOT=$((HOT+1)); else HOT=0; fi
  if [ "$FANOFF" = 1 ] && { [ "$HOT" -ge 2 ] || [ "$c" -gt $CPU_HARD ] || [ "$s" -gt $SKIN_LIMIT ] || [ "${b:-0}" -gt $BATT_LIMIT ]; }; then
    echo "GUARD tripped in $PHASE: cpu=$c skin=$s batt=$b -> fan back to mode $ORIG_MODE"
    fan_restore; return 1
  fi
  return 0
}
run_phase() { PHASE=$1; end=$(( $(date +%s) + $2 )); while [ $(date +%s) -lt $end ]; do sample || return 1; sleep 5; done; return 0; }

echo "orig_mode=$ORIG_MODE orig_timeout=$ORIG_TIMEOUT $(dumpsys battery | grep -E ' status| level' | tr -s ' ' | tr '\n' ' ')"
echo "skin zones:$SKINZ"
echo "t_s,phase,zone0,cpu_max,gpu_max,skin_max,battery,state,duty,rpm,fan_mode"
settings put system screen_off_timeout 1800000
input keyevent KEYCODE_WAKEUP; input keyevent KEYCODE_HOME
PHASE=baseline_smart; sample

settings put system fan_mode 0; FANOFF=1; sleep 3
run_phase idle_fanoff $IDLE_S && {
  am start -n com.retroarch.aarch64/com.retroarch.browser.retroactivity.RetroActivityFuture \
    -e ROM "$ROM" -e LIBRETRO "$CORE" -e CONFIGFILE "$CFG" \
    -e IME com.android.inputmethod.latin/.LatinIME -e DATADIR /data/user/0/com.retroarch.aarch64 \
    --activity-clear-top > /dev/null 2>&1
  sleep 8
  echo "foreground: $(dumpsys activity activities | grep -m1 -E 'topResumedActivity|mResumedActivity' | tr -s ' ')"
  run_phase ${TAG}_fanoff $GAME_S
} && [ "$HEAVY_S" -gt 0 ] && {
  cd /data/local/tmp
  ./tu_bench /vendor/lib64/hw/vulkan.adreno.so 1000000 heavy_fs 2 > /dev/null 2>&1 & LOADPIDS="$LOADPIDS $!"
  for i in 1 2 3; do ( while :; do :; done ) & LOADPIDS="$LOADPIDS $!"; done
  run_phase heavy_fanoff $HEAVY_S
}
stop_load
[ "$FANOFF" = 1 ] && fan_restore
run_phase cooldown_smart $COOL_S
echo DONE
