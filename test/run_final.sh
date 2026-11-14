# Runs on the device: sh run_final.sh [frames]
# Head-to-head of stock vs every driver under /data/local/tmp/drv, one scenario per process.
cd /data/local/tmp
FRAMES=${1:-200}
LIST="stock:/vendor/lib64/hw/vulkan.adreno.so"
for d in drv/*; do
  so=$(ls $d/*.so 2>/dev/null | head -1)
  [ -n "$so" ] && LIST="$LIST $(basename $d):/data/local/tmp/$so"
done
for sc in "drawcalls 1" "fill 4" "heavy_fs 4" "multipass 4"; do
  set -- $sc
  echo "#### $1 x$2"
  sh run_isolated.sh $FRAMES $1 $2 $LIST | grep -v SERIES
done
echo "#### pipelines"
for e in $LIST; do
  echo "== ${e%%:*}"
  ./tu_bench ${e#*:} 8 pipelines | grep BENCH
done
