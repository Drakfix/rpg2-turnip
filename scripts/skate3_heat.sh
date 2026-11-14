#!/system/bin/sh
# Heavy-game heat test with the fan curve app in control (50% cap): runs the Skate 3 port and samples
# temperatures, fan and clock caps every 10 s. Run detached:
#   setsid nohup sh /data/local/tmp/skate3_heat.sh > /data/local/tmp/skate3_heat.log 2>&1 < /dev/null &
# Guard: skin > SKIN_LIMIT or battery > BATT_LIMIT closes the game. Always restores the screen timeout,
# closes the game and goes HOME on exit.
GAME_S=${GAME_S:-720}; SKIN_LIMIT=${SKIN_LIMIT:-52000}; BATT_LIMIT=45000
PKG=chat.buku.skate3
F=/sys/class/gpio5_pwm2; Z0=/sys/class/thermal/thermal_zone0/temp; KG=/sys/class/kgsl/kgsl-3d0
ORIG_TIMEOUT=$(settings get system screen_off_timeout)

CPUZ=""; GPUZ=""; SKINZ=""; BATZ=""
for z in /sys/class/thermal/thermal_zone*; do
  case "$(cat $z/type)" in
    cpu-*) CPUZ="$CPUZ $z/temp";;
    gpuss-*) GPUZ="$GPUZ $z/temp";;
    sys-therm-*) v=$(cat $z/temp); [ "$v" -gt 0 ] && SKINZ="$SKINZ $z/temp";;
    battery) BATZ=$z/temp;;
  esac
done
zmax() { m=-99999; for f in "$@"; do v=$(cat $f 2>/dev/null) || continue; [ "$v" -gt "$m" ] && m=$v; done; echo $m; }
caps() { for p in 0 3 7; do echo -n "$(( $(cat /sys/devices/system/cpu/cpufreq/policy$p/scaling_max_freq) / 1000 ))/"; done
         echo -n "$(( $(cat $KG/max_gpuclk) / 1000000 ))"; }

cleanup() {
  am force-stop $PKG
  settings put system screen_off_timeout "$ORIG_TIMEOUT"
  input keyevent KEYCODE_HOME
  echo "DONE timeout=$(settings get system screen_off_timeout) mode=$(settings get system fan_mode)"
}
trap cleanup EXIT
trap 'exit 1' INT TERM HUP

echo "t_s,zone0,cpu_max,gpu_max,skin_max,battery,duty,rpm,fan_mode,caps_mhz(p0/p3/p7/gpu),gpu_cur_mhz,fg"
settings put system screen_off_timeout 1800000
input keyevent KEYCODE_WAKEUP
am force-stop $PKG; sleep 2
monkey -p $PKG -c android.intent.category.LAUNCHER 1 > /dev/null 2>&1
T0=$(date +%s)
while [ $(( $(date +%s) - T0 )) -lt $GAME_S ]; do
  s=$(zmax $SKINZ); b=$(cat $BATZ 2>/dev/null)
  fg=$(dumpsys activity activities | grep -m1 topResumedActivity | grep -o "[a-z0-9.]*/[A-Za-z0-9.]*" | head -1)
  echo "$(( $(date +%s) - T0 )),$(cat $Z0),$(zmax $CPUZ),$(zmax $GPUZ),$s,$b,$(cat $F/duty),$(cat $F/speed),$(settings get system fan_mode),$(caps),$(( $(cat $KG/gpuclk) / 1000000 )),$fg"
  if [ "$s" -gt $SKIN_LIMIT ] || [ "${b:-0}" -gt $BATT_LIMIT ]; then echo "GUARD skin=$s batt=$b -> closing game"; exit 0; fi
  sleep 10
done
