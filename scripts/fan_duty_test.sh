#!/system/bin/sh
# Fan duty sweep for the Retroid Pocket G2. Run detached:
#   nohup sh /data/local/tmp/fan_duty_test.sh > /data/local/tmp/fan_duty_test.log 2>&1 &
# Temporarily sets `settings system fan_mode` to 0 (Off) so RPSettings stops its
# Smart loop, drives /sys/class/gpio5_pwm2 directly, then restores the original
# mode. Aborts and restores if thermal_zone1 goes above LIMIT.
F=/sys/class/gpio5_pwm2
Z=/sys/class/thermal/thermal_zone0/temp   # what RPSettings Smart mode reads on the G2
LIMIT=55000
cpumax() { m=0; for z in /sys/class/thermal/thermal_zone*; do case "$(cat $z/type)" in cpu-*) t=$(cat $z/temp); [ "$t" -gt "$m" ] && m=$t;; esac; done; echo $m; }
SETTLE=${SETTLE:-6}
ORIG_MODE=$(settings get system fan_mode)

restore() {
  settings put system fan_mode "$ORIG_MODE"
  sleep 6
  echo "RESTORED fan_mode=$(settings get system fan_mode) state=$(cat $F/state) duty=$(cat $F/duty) rpm=$(cat $F/speed)"
}
trap restore EXIT
trap 'exit 1' INT TERM HUP

row() { echo "$1 state=$(cat $F/state) duty=$(cat $F/duty) rpm=$(cat $F/speed) zone0=$(cat $Z) cpumax=$(cpumax)"; }
guard() { t=$(cpumax); [ "$t" -gt "$LIMIT" ] && { echo "ABORT temp $t > $LIMIT"; exit 1; }; }
step() { guard; echo "$1" > $F/duty; sleep "$SETTLE"; a=$(cat $F/speed); sleep 2; row "duty_set=$1 rpm_a=$a"; }

echo "orig_mode=$ORIG_MODE battery: $(dumpsys battery | grep -E 'status|AC powered|USB powered' | tr -s ' ' | tr '\n' ' ')"
row baseline

settings put system fan_mode 0; sleep 6
row "mode0(off)"

echo 1 > $F/state; sleep 1
echo "--- ramp down (running fan) ---"
for d in 12000 10000 8000 7000 6000 5000 4000 3000 2000 1000 0; do step $d; done

echo "--- start from stop (spin-up threshold) ---"
for d in 2000 3000 4000 5000 6000 7000 8000; do
  guard; echo 0 > $F/duty; sleep 5
  echo $d > $F/duty; sleep "$SETTLE"; row "start_from_0 duty_set=$d"
done

echo "--- state=0 with duty kept ---"
echo 12000 > $F/duty; sleep 4; echo 0 > $F/state; sleep "$SETTLE"; row "state0"
echo DONE
