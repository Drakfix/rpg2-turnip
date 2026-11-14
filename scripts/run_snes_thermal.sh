#!/system/bin/sh
# Fan-off thermal run with an SNES game (RetroArch Snes9x), 15 min.
export GAME_ROM="/storage/emulated/0/ROMs/SNES/Mario Game (SMW Hack).sfc"
export GAME_CORE=/data/user/0/com.retroarch.aarch64/cores/snes9x_libretro_android.so
export GAME_TAG=snes IDLE_S=120 GAME_S=900 HEAVY_S=0 COOL_S=180
exec sh /data/local/tmp/fan_off_thermal.sh
