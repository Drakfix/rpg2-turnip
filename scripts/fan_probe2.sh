#!/system/bin/sh
# Read-only, second pass.
echo "=== init_fan_service.rc ==="; cat /vendor/etc/init/init_fan_service.rc
echo "=== fan_service status ==="; getprop | grep -i fan; ps -A -o PID,USER,NAME,ARGS | grep -i fan | grep -v grep
echo "=== soc:pwm-fan ==="; ls -la /sys/devices/platform/soc/soc:pwm-fan/
for f in /sys/devices/platform/soc/soc:pwm-fan/*; do [ -f "$f" ] && echo "$(basename $f)=$(cat $f 2>&1 | head -3)"; done
ls -R /sys/devices/platform/soc/soc:pwm-fan/ 2>/dev/null | head -60
echo "=== hwmon ==="; ls -la /sys/class/hwmon/
echo "=== pwm chips ==="
for p in /sys/class/pwm/pwmchip*; do echo "$p -> $(readlink -f $p) npwm=$(cat $p/npwm 2>&1)"; ls $p; for q in $p/pwm*; do [ -d "$q" ] && echo "  $q period=$(cat $q/period 2>&1) duty=$(cat $q/duty_cycle 2>&1) en=$(cat $q/enable 2>&1)"; done; done
echo "=== /sys/kernel/debug/pwm ==="; cat /sys/kernel/debug/pwm 2>&1 | head -40
echo "=== DT pwm-fan ==="
d=/proc/device-tree/soc/pwm-fan; ls $d 2>&1
for f in $d/*; do echo "$(basename $f): $(od -A n -t x1 $f 2>/dev/null | head -4 | tr -s ' ')"; done 2>/dev/null
echo "=== cooling device for fan ==="
for c in /sys/class/thermal/cooling_device*; do t=$(cat $c/type); case "$t" in *fan*|*pwm*) echo "$c $t cur=$(cat $c/cur_state) max=$(cat $c/max_state)";; esac; done
echo "=== fan_service strings ==="; ls -la /vendor/bin/fan_service; strings /vendor/bin/fan_service 2>/dev/null | grep -iE '/sys|/proc|fan|speed|temp|mode|persist|thermal|%d' | head -120
