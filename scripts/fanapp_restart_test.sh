#!/system/bin/sh
# Simulated restarts: kill the app process (Android restarts the sticky service -> same start-up path as boot).
#  A) curve was in control, charger in, Retroid forced Smart at boot  -> app should take it back
#  B) user had picked Smart before the restart                          -> app should leave Smart alone
PKG=local.fancurve
LOG=/data/user_de/0/$PKG/files/fan_curve.log
s() { echo "$(date +%H:%M:%S) $1 mode=$(settings get system fan_mode) want=$(run-as $PKG cat /data/user_de/0/$PKG/shared_prefs/state.xml | grep -o 'want_control" value="[a-z]*' | cut -d'"' -f3)"; }
restart() {
  n=$(run-as $PKG grep -c ' start pid=' $LOG)
  run-as $PKG kill -9 $(pidof $PKG)
  settings put system fan_mode "$1"          # the boot-time state
  i=0; while [ "$(run-as $PKG grep -c ' start pid=' $LOG)" = "$n" ] && [ $i -lt 40 ]; do sleep 1; i=$((i+1)); done
  echo "  service restarted after ${i}s"; sleep 4
}

echo "== A: curve in control, Retroid forces Smart at boot (charger in)"
s before
restart 4; s "after-restart"

echo "== B: user picks Smart, then restart"
sleep 20; settings put system fan_mode 4; sleep 12; s "user-smart"   # after the 20 s start-up window
restart 4; s "after-restart"
settings put system fan_mode 0; sleep 12; s "user-off-again"
echo "--- app log ---"; run-as $PKG tail -12 $LOG | cut -c1-100
