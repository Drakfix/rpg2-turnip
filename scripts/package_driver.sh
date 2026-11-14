#!/usr/bin/env bash
# Package an already-built Turnip variant as an AdrenoTools zip, the same way the
# community builds that work everywhere do it: the library keeps its original name
# and soname (libvulkan_freedreno.so) and is only stripped with llvm-strip.
#
# No patchelf: rewriting the soname makes patchelf add a 64K-aligned PT_LOAD and move
# sections around. Android's linker accepts that (Eden loaded it) but drivers packaged
# that way refused to start in GameNative on the Retroid Pocket G2 (2026-11-11).
#
# usage: package_driver.sh <variant> <zip basename> <display name> <packageVersion> [description]
#   variant       build directory suffix, i.e. ~/turnip-build/build-<variant>
set -euo pipefail
VARIANT="$1"; ZIPBASE="$2"; NAME="$3"; PKGVER="$4"; DESC="${5:-}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT="${ROOT:-$HOME/turnip-build}"
OUT_DIR="${OUT_DIR:-$REPO/out}"
TC="$ROOT/ndk/toolchains/llvm/prebuilt/linux-x86_64/bin"
export PATH="$ROOT/local/usr/bin:$PATH"

SO="$ROOT/build-$VARIANT/src/freedreno/vulkan/libvulkan_freedreno.so"
[ -f "$SO" ] || { echo "no build output at $SO"; exit 1; }

PKG="$ROOT/pkg-$VARIANT-clean"
rm -rf "$PKG"; mkdir -p "$PKG" "$OUT_DIR"
cp "$SO" "$PKG/libvulkan_freedreno.so"
"$TC/llvm-strip" "$PKG/libvulkan_freedreno.so"

# Mesa version and git sha as embedded in the binary itself, e.g. "Mesa 26.3.0 (git-34ad0a45ab)"
VERSTR="$(strings -n 6 "$PKG/libvulkan_freedreno.so" | grep -m1 -E '^Mesa [0-9]+\.[0-9]+' || true)"
[ -n "$DESC" ] || DESC="Mesa Turnip for Snapdragon G2 Gen 2 / Adreno A22 (Retroid Pocket G2). $VERSTR."

cat > "$PKG/meta.json" <<EOF
{
  "schemaVersion": 1,
  "name": "$NAME",
  "description": "$DESC",
  "author": "RPG2-Turnip",
  "packageVersion": "$PKGVER",
  "vendor": "Mesa",
  "driverVersion": "${VERSTR#Mesa }",
  "minApi": 28,
  "libraryName": "libvulkan_freedreno.so"
}
EOF

ZIP="$OUT_DIR/$ZIPBASE.zip"
rm -f "$ZIP"
(cd "$PKG" && zip -q -9 "$ZIP" libvulkan_freedreno.so meta.json)
echo "== $ZIPBASE: $VERSTR"
"$TC/llvm-readelf" -lW "$PKG/libvulkan_freedreno.so" | awk '$1=="LOAD"{printf "LOAD align=%s ", $NF} END{print ""}'
"$TC/llvm-readelf" -d "$PKG/libvulkan_freedreno.so" | grep -E 'SONAME' | sed 's/^ *0x[0-9a-f]* *//'
ls -la "$ZIP" | awk '{print $5, $NF}'
