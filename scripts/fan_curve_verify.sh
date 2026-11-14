#!/system/bin/sh
# Verify the running fan_curve daemon: short CPU load -> fan on -> cool -> fan off,
# then the tile override (fan_mode 4 -> yield, back to 0 -> resume). Samples every 5 s.
F=/sys/class/gpio5_pwm2
s() { echo "$(date +%H:%M:%S) $1 mode=$(settings get system fan_mode) z0=$(cat /sys/class/thermal/thermal_zone0/temp) state=$(cat $F/state) duty=$(cat $F/duty) rpm=$(cat $F/speed)"; }
P=""
for i in 1 2 3 4; do ( while :; do :; done ) & P="$P $!"; done
for i in $(seq 1 8); do s load; sleep 5; done
kill $P
for i in $(seq 1 44); do s cool; sleep 5; done        # 220 s: MIN_ON 120 + OFF_HOLD 60 + margin
settings put system fan_mode 4; sleep 12; s "override-smart"
settings put system fan_mode 0; sleep 12; s "back-to-curve"
echo "--- daemon log ---"; tail -15 /data/local/tmp/fan_curve.log
echo VERIFY_DONE
