# Device side of scripts/gamemon.py. Usage: sh gamemon_dev.sh <package> <seconds>
# Prints one block per second: GPU clock/load/temperature, fan duty/RPM/mode, skin and battery
# temperature, and the SurfaceFlinger frame-present history of every layer that belongs to <package>. Read-only.
PKG=$1; SECS=${2:-60}
K=/sys/class/kgsl/kgsl-3d0
F=/sys/class/gpio5_pwm2
i=0
LAYERS=""
# Board ("skin") sensors the fan curve uses: sys-therm-* zones that read a real temperature.
SKINZ=""
for z in /sys/class/thermal/thermal_zone*; do
  case "$(cat $z/type 2>/dev/null)" in sys-therm-*) [ "$(cat $z/temp 2>/dev/null)" -gt 0 ] 2>/dev/null && SKINZ="$SKINZ $z/temp";; esac
done
skin() { m=0; for f in $SKINZ; do v=$(cat $f 2>/dev/null) || continue; [ "$v" -gt $m ] && m=$v; done; echo $m; }
while [ $i -lt $SECS ]; do
  # re-resolve layers every 5 s (surfaces are recreated when a game starts or resumes)
  if [ $((i % 5)) -eq 0 ]; then
    MODE=$(settings get system fan_mode 2>/dev/null)   # Retroid fan mode: 0 = custom curve in control
    LAYERS=$(dumpsys SurfaceFlinger --list 2>/dev/null | grep -F "$PKG" | sed -e 's/^RequestedLayerState{//' -e 's/ parentId=.*$//' -e 's/ z=.*$//' -e 's/}$//' | grep -v -E 'Splash|Task=|ActivityRecord' | head -8 | tr '\n' '|')
  fi
  # @S i gpuclk busy total gpu_temp zone0 | fan_duty fan_rpm fan_mode skin_max battery_tenths_C
  echo "@S $i $(cat $K/gpuclk) $(cat $K/gpubusy) $(cat $K/temp) $(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null)" \
       "$(cat $F/duty 2>/dev/null || echo -1) $(cat $F/speed 2>/dev/null || echo -1) $MODE" \
       "$(skin) $(cat /sys/class/power_supply/battery/temp 2>/dev/null || echo -1)"
  OLDIFS=$IFS; IFS='|'
  for L in $LAYERS; do
    [ -z "$L" ] && continue
    echo "@L $L"
    dumpsys SurfaceFlinger --latency "$L" 2>/dev/null
  done
  IFS=$OLDIFS
  echo "@E"
  i=$((i+1))
  sleep 1
done
