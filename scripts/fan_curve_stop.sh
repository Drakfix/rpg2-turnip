#!/system/bin/sh
# Stop the custom fan curve and give the fan back to RPSettings.
DIR=/data/local/tmp
if [ -f $DIR/fan_curve.pid ] && kill -0 $(cat $DIR/fan_curve.pid) 2>/dev/null; then
  P=$(cat $DIR/fan_curve.pid); kill $P
  i=0; while kill -0 $P 2>/dev/null && [ $i -lt 15 ]; do sleep 1; i=$((i+1)); done   # it exits after its current sleep
fi
# Belt and braces: if the daemon was killed hard, restore the mode ourselves.
if [ "$(settings get system fan_mode)" = 0 ]; then
  settings put system fan_mode "$(cat $DIR/fan_curve.origmode 2>/dev/null || echo 4)"
fi
rm -f $DIR/fan_curve.pid
sleep 5
echo "fan_mode=$(settings get system fan_mode) state=$(cat /sys/class/gpio5_pwm2/state) duty=$(cat /sys/class/gpio5_pwm2/duty) rpm=$(cat /sys/class/gpio5_pwm2/speed)"
