#!/system/bin/sh
# Simulated charger plug/unplug: RPSettings should force fan_mode 4 on plug and put 0 back on unplug;
# the fan curve app should yield and resume. Always resets the battery simulation on exit.
F=/sys/class/gpio5_pwm2
s() { echo "$(date +%H:%M:%S) $1 mode=$(settings get system fan_mode) state=$(cat $F/state) duty=$(cat $F/duty) rpm=$(cat $F/speed)"; }
trap 'dumpsys battery reset; s reset' EXIT
settings put system fan_mode 0; sleep 12; s before
dumpsys battery set ac 1; dumpsys battery set status 2      # plug: plugged first, then charging
for i in 1 2 3 4; do sleep 4; s plugged; done
dumpsys battery set status 3; dumpsys battery set ac 0      # unplug: stop charging first (real unplugs change both at once)
for i in 1 2 3 4 5; do sleep 4; s unplugged; done
echo "--- app log ---"; run-as local.fancurve tail -5 /data/user_de/0/local.fancurve/files/fan_curve.log
