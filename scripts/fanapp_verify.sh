#!/system/bin/sh
# Verify the G2 Fan Curve app: 40 s of 4-core CPU load -> fan on/ramp -> cool -> fan off,
# then the tile override (fan_mode 4 -> yield, back to 0 -> resume). Samples every 5 s.
F=/sys/class/gpio5_pwm2
s() { echo "$(date +%H:%M:%S) $1 mode=$(settings get system fan_mode) z0=$(cat /sys/class/thermal/thermal_zone0/temp) state=$(cat $F/state) duty=$(cat $F/duty) rpm=$(cat $F/speed)"; }
for i in 1 2 3 4; do timeout 40 sh -c 'while :; do :; done' & done
for i in $(seq 1 8); do s load; sleep 5; done
wait
for i in $(seq 1 44); do s cool; sleep 5; done        # 220 s: MIN_ON 120 + OFF_HOLD 60 + margin
settings put system fan_mode 4; sleep 14; s override-smart
settings put system fan_mode 0; sleep 14; s back-to-curve
echo "--- app log ---"; run-as local.fancurve tail -12 /data/user_de/0/local.fancurve/files/fan_curve.log
echo VERIFY_DONE
