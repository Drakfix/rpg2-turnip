#!/system/bin/sh
# Custom fan curve for the Retroid Pocket G2 (prototype, no root).
#
# Start:  setsid nohup sh /data/local/tmp/fan_curve.sh > /dev/null 2>&1 < /dev/null &
# Stop:   sh /data/local/tmp/fan_curve_stop.sh
# Log:    /data/local/tmp/fan_curve.log
#
# How it works: sets `settings system fan_mode` to 0 (Off) so RPSettings stops its Smart loop,
# then drives /sys/class/gpio5_pwm2 itself. The quick-settings fan tile is the override:
#   tile = Off  -> this curve is in control
#   any other   -> the script yields and RPSettings runs the fan (also happens on charger plug-in).
# On exit it puts the original fan mode back.

F=/sys/class/gpio5_pwm2
Z0=/sys/class/thermal/thermal_zone0/temp      # the sensor RPSettings Smart mode uses
BL=/sys/class/backlight/panel0-backlight/brightness
DIR=/data/local/tmp
LOG=$DIR/fan_curve.log
PIDF=$DIR/fan_curve.pid
ORIGF=$DIR/fan_curve.origmode

INTERVAL=3
# --- curve (temperatures in m°C, duty in ns of a 50000 ns period: 5000 = 10%) ---
ON_Z=47000;  ON_SKIN=41000        # fan starts when smoothed zone0 or skin reaches these
OFF_Z=44000; OFF_SKIN=39000       # fan may stop when both are below these ...
OFF_HOLD=60; MIN_ON=120           # ... for OFF_HOLD s, and only after MIN_ON s of running
KICK=10000; KICK_S=2              # 20% for 2 s to start the fan reliably (it needs >=10% from rest)
MIN_DUTY=6000                     # 12%: lowest running duty (stalls at <=8%)
FAILSAFE=25000                    # 50% if the sensor can't be read
EMERG_Z=85000; EMERG_SKIN=46000; EMERG_DUTY=25000   # 50%: capped like stock Smart (it never goes above 50%)
# zone0 points: 47 °C -> 12%, 55 -> 24%, 70+ -> 50% (cap)
# skin  points: 41 °C -> 12%, 44 -> 24%, 46 -> 50%

lerp() { # x x0 x1 y0 y1
  if [ $1 -le $2 ]; then echo $4; elif [ $1 -ge $3 ]; then echo $5
  else echo $(( $4 + ($5 - $4) * ($1 - $2) / ($3 - $2) )); fi; }
curve_z() {
  if [ $1 -lt 55000 ]; then lerp $1 47000 55000 6000 12000
  elif [ $1 -lt 70000 ]; then lerp $1 55000 70000 12000 25000
  else echo 25000; fi; }
curve_skin() {
  if [ $1 -lt 41000 ]; then echo 0
  elif [ $1 -lt 44000 ]; then lerp $1 41000 44000 6000 12000
  else lerp $1 44000 46000 12000 25000; fi; }
max() { [ $1 -ge $2 ] && echo $1 || echo $2; }

log() { echo "$(date '+%m-%d %H:%M:%S') $*" >> $LOG; }
rotate() { [ -f $LOG ] && [ $(wc -c < $LOG) -gt 2000000 ] && mv $LOG $LOG.1; }

# Single instance
if [ -f $PIDF ] && kill -0 $(cat $PIDF) 2>/dev/null; then echo "already running (pid $(cat $PIDF))"; exit 1; fi
echo $$ > $PIDF

SKINZ=""
for z in /sys/class/thermal/thermal_zone*; do
  case "$(cat $z/type)" in sys-therm-*) v=$(cat $z/temp); [ "$v" -gt 0 ] && SKINZ="$SKINZ $z/temp";; esac
done
skin() { m=0; for f in $SKINZ; do v=$(cat $f 2>/dev/null) || continue; [ "$v" -gt $m ] && m=$v; done; echo $m; }

ORIG=$(settings get system fan_mode)
[ "$ORIG" = 0 ] && [ -f $ORIGF ] && ORIG=$(cat $ORIGF)   # restarted while already in control
[ "$ORIG" = 0 ] && ORIG=4
echo $ORIG > $ORIGF

cleanup() {
  [ "$(settings get system fan_mode)" = 0 ] && settings put system fan_mode "$ORIG"
  log "stop: restored fan_mode=$ORIG"
  rm -f $PIDF
}
trap cleanup EXIT
trap 'exit 0' INT TERM HUP

settings put system fan_mode 0
log "start pid=$$ orig_mode=$ORIG skin_zones=$SKINZ"

FAN=0; EMA=-1; ON_SINCE=0; COOL_SINCE=0; YIELD=0; DUTY=0; LASTLOG=0; SCREEN=1
while :; do
  NOW=$(date +%s)
  MODE=$(settings get system fan_mode)
  if [ "$MODE" != 0 ]; then
    [ $YIELD = 0 ] && log "yield: fan_mode=$MODE (tile/charger), RPSettings in control"
    YIELD=1; FAN=0; EMA=-1; sleep 10; continue
  fi
  if [ $YIELD = 1 ]; then log "resume: fan_mode back to 0"; YIELD=0; fi

  if [ "$(cat $BL 2>/dev/null)" = 0 ]; then   # screen off: fan off, poll slowly
    [ $SCREEN = 1 ] && { log "screen off -> fan off"; echo 0 > $F/duty; echo 0 > $F/state; }
    SCREEN=0; FAN=0; EMA=-1; sleep 10; continue
  fi
  [ $SCREEN = 0 ] && log "screen on"; SCREEN=1

  Z=$(cat $Z0 2>/dev/null); S=$(skin)
  case "$Z" in ''|*[!0-9]*) Z=0;; esac
  if [ "$Z" -le 0 ]; then
    TARGET=$FAILSAFE; WHY=failsafe
  else
    if [ $EMA -lt 0 ]; then EMA=$Z; else EMA=$(( EMA + (Z - EMA) * 15 / 100 )); fi   # ~20 s smoothing
    if [ $EMA -ge $EMERG_Z ] || [ $S -ge $EMERG_SKIN ]; then
      TARGET=$EMERG_DUTY; WHY=emergency
    elif [ $FAN = 0 ]; then
      if [ $EMA -ge $ON_Z ] || [ $S -ge $ON_SKIN ]; then TARGET=$(max $(max $(curve_z $EMA) $(curve_skin $S)) $MIN_DUTY); WHY=on
      else TARGET=0; WHY=off; fi
    else
      if [ $EMA -lt $OFF_Z ] && [ $S -lt $OFF_SKIN ]; then
        [ $COOL_SINCE = 0 ] && COOL_SINCE=$NOW
      else COOL_SINCE=0; fi
      if [ $COOL_SINCE != 0 ] && [ $((NOW - COOL_SINCE)) -ge $OFF_HOLD ] && [ $((NOW - ON_SINCE)) -ge $MIN_ON ]; then
        TARGET=0; WHY=off
      else
        TARGET=$(max $(max $(curve_z $EMA) $(curve_skin $S)) $MIN_DUTY); WHY=run
      fi
    fi
  fi

  if [ $TARGET -gt 0 ] && [ $FAN = 0 ]; then
    echo 1 > $F/state; echo $KICK > $F/duty; sleep $KICK_S
    FAN=1; ON_SINCE=$NOW; COOL_SINCE=0
    log "fan ON ($WHY) z0=$Z ema=$EMA skin=$S duty=$TARGET"
  elif [ $TARGET = 0 ] && [ $FAN = 1 ]; then
    FAN=0; log "fan OFF z0=$Z ema=$EMA skin=$S ran=$((NOW - ON_SINCE))s"
  fi
  if [ $TARGET = 0 ]; then
    [ "$(cat $F/state)" != 0 ] && { echo 0 > $F/duty; echo 0 > $F/state; }
  else
    [ "$(cat $F/state)" != 1 ] && echo 1 > $F/state
    [ "$(cat $F/duty)" != $TARGET ] && echo $TARGET > $F/duty
  fi
  DUTY=$TARGET

  if [ $((NOW - LASTLOG)) -ge 30 ]; then
    log "z0=$Z ema=$EMA skin=$S duty=$DUTY rpm=$(cat $F/speed) $WHY"; LASTLOG=$NOW; rotate
  fi
  sleep $INTERVAL
done
