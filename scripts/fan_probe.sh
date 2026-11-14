#!/system/bin/sh
# Read-only probe of the Retroid Pocket G2 fan stack. Writes nothing on the device.
echo "=== props ==="; getprop | grep -iE 'fan|therm|retroid|handheld'
echo "=== settings ==="
for ns in system global secure; do settings list $ns 2>/dev/null | grep -iE 'fan|therm|perf' | sed "s/^/$ns: /"; done
echo "=== services ==="; service list 2>/dev/null | grep -iE 'fan|therm|retroid|handheld|odin'
echo "=== packages ==="; pm list packages -f 2>/dev/null | grep -iE 'fan|therm|retroid|handheld|odin|ayn|setting'
echo "=== processes ==="; ps -A -o PID,USER,NAME 2>/dev/null | grep -iE 'fan|therm|retroid|handheld|perf'
echo "=== sysfs fan-ish nodes ==="
find /sys/devices /sys/class -maxdepth 6 \( -iname '*fan*' -o -iname 'pwm*' \) 2>/dev/null | head -80
echo "=== hwmon ==="
for h in /sys/class/hwmon/hwmon*; do echo "$h: $(cat $h/name 2>/dev/null)"; ls $h 2>/dev/null | grep -iE 'fan|pwm' | while read f; do echo "  $f=$(cat $h/$f 2>/dev/null)"; done; done
echo "=== cooling devices ==="
for c in /sys/class/thermal/cooling_device*; do echo "$(basename $c) $(cat $c/type) cur=$(cat $c/cur_state 2>/dev/null) max=$(cat $c/max_state 2>/dev/null)"; done | grep -viE 'cpu|thermal-cpufreq|gpu'
echo "=== thermal zones (temp) ==="
for z in /sys/class/thermal/thermal_zone*; do t=$(cat $z/type); case "$t" in *cpu*|*gpu*|*skin*|*sys*|*batt*|*fan*|*therm*|*xo*|*pa*) echo "$(basename $z) $t $(cat $z/temp 2>/dev/null)";; esac; done | head -60
echo "=== config files ==="
find /vendor /odm /system/etc /product/etc /system_ext -maxdepth 4 \( -iname '*fan*' -o -iname '*thermal*' \) 2>/dev/null | head -60
