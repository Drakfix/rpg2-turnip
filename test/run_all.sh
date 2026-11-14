# Runs on the device: sh /data/local/tmp/run_all.sh [frames] [rounds] [scale]
# Smoke-tests then benchmarks every driver under /data/local/tmp/drv plus the stock blob.
cd /data/local/tmp
FRAMES=${1:-80}
ROUNDS=${2:-1}
SCALE=${3:-1}
LIST="stock:/vendor/lib64/hw/vulkan.adreno.so"
for d in drv/*; do
  so=$(ls $d/*.so 2>/dev/null | head -1)
  [ -n "$so" ] && LIST="$LIST $(basename $d):/data/local/tmp/$so"
done
echo "#### smoke"
for e in $LIST; do
  name=${e%%:*}; so=${e#*:}
  r=$(./tu_smoke $so 2>&1 | grep -E 'deviceName|apiVersion|extensions|RESULT|FAIL' | tr '\n' '|')
  echo "$name => $r"
done
r=1
while [ $r -le $ROUNDS ]; do
  echo "#### bench round $r (frames=$FRAMES scale=$SCALE)"
  for e in $LIST; do
    name=${e%%:*}; so=${e#*:}
    echo "== $name  gpu_temp=$(cat /sys/class/kgsl/kgsl-3d0/temp 2>/dev/null)"
    ./tu_bench $so $FRAMES all $SCALE 2>&1 | grep -E 'BENCH|FAIL'
  done
  r=$((r+1))
done
