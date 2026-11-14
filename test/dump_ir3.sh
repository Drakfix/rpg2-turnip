# Runs on the device: sh dump_ir3.sh <turnip.so> <fsmode number> [extra ENV=val ...]
# Writes Turnip's fragment shader disassembly (ir3) for one heavy.frag variant to /data/local/tmp/ir3_<mode>.txt
cd /data/local/tmp
SO=$1; M=$2; shift 2
rm -f mesa_ir3.log
env IR3_SHADER_DEBUG=fs,disasm MESA_LOG_FILE=/data/local/tmp/mesa_ir3.log "$@" ./tu_bench $SO 8 fsmode$M 1 > ir3_$M.stdout 2> ir3_$M.stderr
cat mesa_ir3.log ir3_$M.stderr 2>/dev/null > ir3_$M.txt
wc -l ir3_$M.txt
