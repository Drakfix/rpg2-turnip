#!/usr/bin/env bash
# Build Mesa Turnip (KGSL backend) for Android arm64 and package it as an
# AdrenoTools driver zip for the Retroid Pocket G2 (Snapdragon G2 Gen 2 / Adreno A22).
#
# Run inside WSL/Linux after scripts/setup_env.sh:
#   bash scripts/build_turnip.sh [variant-name]
#
# Environment overrides:
#   ROOT       build root (default ~/turnip-build)
#   MESA_DIR   mesa checkout (default $ROOT/mesa)
#   PATCH_DIR  directory of *.patch files applied in name order (default <repo>/patches)
#   OUT_DIR    where the zip is written (default <repo>/out)
#   API        Android API level the compiler targets (default 34)
#   PLATFORM_SDK  Android version Mesa is told it runs on (default 36). Mesa hides every
#              Vulkan extension that this Android version did not allow, so it must NOT
#              follow API: with 34, VK_KHR_maintenance5/6, map_memory2 and eight others
#              disappear and DXVK/Proton games (GameNative) refuse to start.
#   EXTRA_CFLAGS  extra C/C++ flags, e.g. "-mcpu=cortex-a720+nosve" (G2 CPU has no SVE)
set -euo pipefail

VARIANT="${1:-a22}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT="${ROOT:-$HOME/turnip-build}"
MESA_DIR="${MESA_DIR:-$ROOT/mesa}"
PATCH_DIR="${PATCH_DIR:-$REPO/patches}"
OUT_DIR="${OUT_DIR:-$REPO/out}"
API="${API:-34}"
PLATFORM_SDK="${PLATFORM_SDK:-36}"
NDK="$ROOT/ndk"
TC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
BUILD="$ROOT/build-$VARIANT"

export PATH="$ROOT/venv/bin:$ROOT/local/usr/bin:$PATH"
export BISON_PKGDATADIR="$ROOT/local/usr/share/bison"
export M4="$ROOT/local/usr/bin/m4"

[ -x "$TC/aarch64-linux-android${API}-clang" ] || { echo "NDK clang for API $API not found in $TC"; exit 1; }

cd "$MESA_DIR"
# Always start from a pristine tree so patches apply reproducibly.
git checkout -q -- .
git clean -fdq -- src
MESA_SHA="$(git rev-parse --short=10 HEAD)"
MESA_VER="$(cat VERSION)"

APPLIED=()
if compgen -G "$PATCH_DIR/*.patch" >/dev/null; then
  for p in "$PATCH_DIR"/*.patch; do
    echo ">> applying $(basename "$p")"
    git apply --whitespace=nowarn "$p"
    APPLIED+=("$(basename "$p" .patch)")
  done
fi

CROSS="$ROOT/android-aarch64-$VARIANT.ini"
cat > "$CROSS" <<EOF
[binaries]
ar = '$TC/llvm-ar'
c = ['$TC/aarch64-linux-android${API}-clang']
cpp = ['$TC/aarch64-linux-android${API}-clang++', '-fno-exceptions', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '--start-no-unused-arguments', '-static-libstdc++', '--end-no-unused-arguments']
c_ld = 'lld'
cpp_ld = 'lld'
strip = '$TC/llvm-strip'
pkg-config = ['env', 'PKG_CONFIG_LIBDIR=$ROOT/empty-pkgconfig', '/usr/bin/pkg-config']

[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'armv8'
endian = 'little'
EOF
mkdir -p "$ROOT/empty-pkgconfig"

rm -rf "$BUILD"
meson setup "$BUILD" \
  --cross-file "$CROSS" \
  -Dbuildtype=release \
  -Dplatforms=android \
  -Dplatform-sdk-version="$PLATFORM_SDK" \
  -Dandroid-stub=true \
  -Dgallium-drivers= \
  -Dvulkan-drivers=freedreno \
  -Dvulkan-beta=true \
  -Dfreedreno-kmds=kgsl \
  -Degl=disabled \
  -Dandroid-libbacktrace=disabled \
  -Dstrip=true \
  ${EXTRA_CFLAGS:+-Dc_args="$EXTRA_CFLAGS" -Dcpp_args="$EXTRA_CFLAGS"} \
  2>&1 | tee "$ROOT/meson-$VARIANT.log" | tail -40

ninja -C "$BUILD" 2>&1 | tee "$ROOT/ninja-$VARIANT.log" | tail -15

SO="$BUILD/src/freedreno/vulkan/libvulkan_freedreno.so"
[ -f "$SO" ] || { echo "build did not produce $SO"; exit 1; }

STAMP="$(date -u +%Y%m%d)"
PATCH_DESC="vanilla"
[ "${#APPLIED[@]}" -gt 0 ] && PATCH_DESC="$(IFS=,; echo "${APPLIED[*]}")"
[ -n "${EXTRA_CFLAGS:-}" ] && PATCH_DESC="$PATCH_DESC; cflags: $EXTRA_CFLAGS"

# Packaging lives in package_driver.sh: the library keeps its original name/soname and is
# only stripped. Do NOT rename it with patchelf (see the note in that script).
ROOT="$ROOT" OUT_DIR="$OUT_DIR" bash "$REPO/scripts/package_driver.sh" "$VARIANT"   "turnip-a22-$VARIANT-$MESA_SHA-$STAMP"   "Turnip A22 ($VARIANT) Mesa $MESA_VER" "1"   "Mesa Turnip for Snapdragon G2 Gen 2 / Adreno A22 (Retroid Pocket G2). Mesa $MESA_VER $MESA_SHA, patches: $PATCH_DESC. Built $STAMP."
cp "$ROOT/pkg-$VARIANT-clean/libvulkan_freedreno.so" "$OUT_DIR/libvulkan_freedreno.so.$VARIANT"
echo "ZIP=$OUT_DIR/turnip-a22-$VARIANT-$MESA_SHA-$STAMP.zip"
