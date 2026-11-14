# Runs on the device: sh run_fsmodes.sh [frames] [scale] <label:driver.so[:ENV=val]>...
# Times each per-operation fragment shader variant on each driver, alternating
# drivers per variant so both sit at a similar GPU clock / temperature.
cd /data/local/tmp
FRAMES=${1:-120}; SCALE=${2:-4}; shift 2
for m in 0 1 2 3 4 5 6 7 100; do
  for e in "$@"; do
    label=${e%%:*}; rest=${e#*:}; so=${rest%%:*}; envs=""
    [ "$rest" != "$so" ] && envs=${rest#*:}
    r=$(env $envs ./tu_bench $so $FRAMES fsmode$m $SCALE 2>&1 | grep -E 'BENCH|FAIL')
    echo "$label $r"
  done
done
