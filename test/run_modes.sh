# Runs on the device: sh run_modes.sh <driver.so> [frames] [scenario|all] [scale]
# A/B tests Turnip render-mode settings on one driver build.
cd /data/local/tmp
SO=$1; FRAMES=${2:-80}; SC=${3:-all}; SCALE=${4:-1}
run() { # run <label> <env assignments...>
  label=$1; shift
  echo "== $label"
  env "$@" ./tu_bench $SO $FRAMES $SC $SCALE 2>&1 | grep -E 'BENCH|FAIL'
}
run "autotune=bandwidth" TU_AUTOTUNE_ALGO=bandwidth
run "autotune=profiled" TU_AUTOTUNE_ALGO=profiled
run "autotune=profiled +tune_small" TU_AUTOTUNE_ALGO=profiled TU_AUTOTUNE_FLAGS=tune_small
run "autotune=prefer_sysmem" TU_AUTOTUNE_ALGO=prefer_sysmem
run "autotune=prefer_gmem" TU_AUTOTUNE_ALGO=prefer_gmem
run "TU_DEBUG=sysmem" TU_DEBUG=sysmem
run "TU_DEBUG=gmem" TU_DEBUG=gmem
echo "== stock"
./tu_bench /vendor/lib64/hw/vulkan.adreno.so $FRAMES $SC $SCALE 2>&1 | grep -E 'BENCH|FAIL'
