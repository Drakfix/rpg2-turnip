# Runs on the device: sh run_isolated.sh <frames> <scenario> <scale> <label:driver.so[:ENV=val]>...
# Each entry runs one scenario in its own process (fresh autotune state), preceded by a
# warm-up run of the stock driver so every entry starts from a similar GPU clock state.
# Prints the per-tenth time series ("frame_ms@gpu_MHz") so clock shifts are visible.
cd /data/local/tmp
FRAMES=$1; SC=$2; SCALE=$3; shift 3
for e in "$@"; do
  label=${e%%:*}; rest=${e#*:}; so=${rest%%:*}; envs=""
  [ "$rest" != "$so" ] && envs=${rest#*:}
  ./tu_bench /vendor/lib64/hw/vulkan.adreno.so 60 heavy_fs 2 >/dev/null 2>&1
  echo "== $label ${envs}"
  env TU_BENCH_SERIES=1 $envs ./tu_bench $so $FRAMES $SC $SCALE 2>&1 | grep -E 'BENCH|SERIES|FAIL'
done
