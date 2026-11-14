#!/system/bin/sh
# Simulated battery temperature (dumpsys battery set temp, tenths of °C): 37.5 °C should run the fan at ~55%,
# 39.5 °C at 70%; back to 30 °C it should stop after MIN_ON (120 s) + OFF_HOLD (60 s). Always resets on exit.
F=/sys/class/gpio5_pwm2
s() { echo "$(date +%H:%M:%S) $1 batt=$(dumpsys battery | grep temperature | tr -dc 0-9) mode=$(settings get system fan_mode) state=$(cat $F/state) duty=$(cat $F/duty)"; }
trap 'dumpsys battery reset; s reset' EXIT
s before
dumpsys battery set temp 375; sleep 8; s "37.5C"
dumpsys battery set temp 395; sleep 8; s "39.5C"
dumpsys battery set temp 365; sleep 8; s "36.5C(hold)"
dumpsys battery set temp 300
for i in $(seq 1 12); do sleep 15; s "30C"; done
echo "--- app log ---"; run-as local.fancurve tail -6 /data/user_de/0/local.fancurve/files/fan_curve.log
