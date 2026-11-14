#!/system/bin/sh
# Test the basic (targetSdk 35) build installed as local.fancurve.test, with the full app stopped.
# Expect: no fan_mode writes; Retroid tile Off = curve; charger forces Smart and the app yields (no reclaim);
# unplug -> Retroid restores Off -> curve resumes; Stop leaves the fan at 24% with the tile on Off.
F=/sys/class/gpio5_pwm2
PKG=local.fancurve.test
LOG=/data/user_de/0/$PKG/files/fan_curve.log
s() { echo "$(date +%H:%M:%S) $1 mode=$(settings get system fan_mode) state=$(cat $F/state) duty=$(cat $F/duty) svc=$(dumpsys activity services $PKG | grep -c 'isForeground=true')"; }
trap 'dumpsys battery reset; s reset' EXIT

s before
pm grant $PKG android.permission.POST_NOTIFICATIONS
am start -n $PKG/local.fancurve.MainActivity --ez start true > /dev/null; sleep 6; s "start(tile Smart)"
settings put system fan_mode 0; sleep 12; s "tile Off"
dumpsys battery set ac 1; dumpsys battery set status 2; sleep 12; s "charger in"
dumpsys battery set status 3; dumpsys battery set ac 0; sleep 12; s "charger out"
am start -n $PKG/local.fancurve.MainActivity > /dev/null; sleep 2
screencap -p /data/local/tmp/basic.png
am startservice -n $PKG/local.fancurve.FanService -a local.fancurve.STOP > /dev/null 2>&1
echo "--- app log ---"; run-as $PKG cat $LOG | cut -c1-110
