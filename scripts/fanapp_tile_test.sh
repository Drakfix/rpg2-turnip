#!/system/bin/sh
# Tests for the targetSdk-22 build: Stop restores Smart, Start takes over, charger reclaim on/off,
# and the "Fan curve" quick-settings tile (added with cmd statusbar, clicked with click-tile).
F=/sys/class/gpio5_pwm2
PKG=local.fancurve
TILE=$PKG/.FanTileService
PREFS=/data/user_de/0/$PKG/shared_prefs/state.xml
s() { echo "$(date +%H:%M:%S) $1 mode=$(settings get system fan_mode) state=$(cat $F/state) duty=$(cat $F/duty) svc=$(dumpsys activity services $PKG | grep -c 'isForeground=true')"; }
trap 'dumpsys battery reset; s reset' EXIT

echo "== stop from notification action / app"
am start -n $PKG/.MainActivity --ez start true > /dev/null; sleep 5; s started
am startservice -n $PKG/.FanService -a local.fancurve.STOP > /dev/null 2>&1   # not exported: expected to fail
cmd statusbar add-tile $TILE; sleep 2
cmd statusbar click-tile $TILE; sleep 6; s "tile-click(stop)"
cmd statusbar click-tile $TILE; sleep 6; s "tile-click(start)"

echo "== charger with keep_on_charger=true (default)"
dumpsys battery set ac 1; dumpsys battery set status 2; sleep 8; s plugged
dumpsys battery set status 3; dumpsys battery set ac 0; sleep 6; s unplugged

echo "== Retroid tile to Smart while running, then plug (should NOT reclaim: user chose Smart)"
settings put system fan_mode 4; sleep 12; s "user-smart"
dumpsys battery set ac 1; dumpsys battery set status 2; sleep 8; s "plugged(user-smart)"
dumpsys battery set status 3; dumpsys battery set ac 0; sleep 6; s "unplugged"
settings put system fan_mode 0; sleep 12; s "user-off"
echo "--- app log ---"; run-as $PKG tail -14 /data/user_de/0/$PKG/files/fan_curve.log
