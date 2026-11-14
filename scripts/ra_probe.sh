#!/system/bin/sh
# Read-only: list SNES ROMs and RetroArch history entries that used Snes9x.
ls /sdcard/ROMs/SNES | head -40
echo ---
grep -B3 -A3 'snes9x' /sdcard/RetroArch/playlists/builtin/content_history.lpl | grep -E '"path"|"label"' | head -20
dumpsys battery | grep -E ' status| level'
