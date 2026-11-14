#!/usr/bin/env bash
# Sets up a no-sudo Turnip build environment under ~/turnip-build
set -uo pipefail
ROOT="$HOME/turnip-build"
mkdir -p "$ROOT"/{local,debs,dl}
cd "$ROOT"

echo "=== [1/4] user-local Ubuntu packages (no sudo) ==="
cd "$ROOT/debs"
for p in bison flex m4 libfl2 libfl-dev zip unzip patchelf; do
  if ls "${p}"_*.deb >/dev/null 2>&1; then echo "have $p"; continue; fi
  apt-get download "$p" 2>&1 | tail -1 || echo "FAILED apt download $p"
done
for d in *.deb; do dpkg -x "$d" "$ROOT/local"; done
export PATH="$ROOT/local/usr/bin:$PATH"
export BISON_PKGDATADIR="$ROOT/local/usr/share/bison"
export M4="$ROOT/local/usr/bin/m4"
for c in bison flex m4 zip unzip patchelf; do printf '%s: ' "$c"; command -v "$c" || echo MISSING; done
bison --version | head -1; flex --version

echo "=== [2/4] python venv (meson, mako) ==="
cd "$ROOT"
# --without-pip + system pip: works even when python3-venv/ensurepip is not installed
[ -x venv/bin/python ] || python3 -m venv --without-pip venv
pip3 --python ./venv/bin/python install -q meson mako pyyaml packaging
./venv/bin/meson --version

echo "=== [3/4] Android NDK ==="
if [ ! -d "$ROOT/ndk" ]; then
  NDK_VER=""
  for v in r30 r29 r28c r27d; do
    code=$(curl -s -o /dev/null -I -w '%{http_code}' "https://dl.google.com/android/repository/android-ndk-${v}-linux.zip")
    echo "ndk $v -> HTTP $code"
    if [ "$code" = "200" ]; then NDK_VER="$v"; break; fi
  done
  [ -n "$NDK_VER" ] || { echo "NO NDK FOUND"; exit 1; }
  curl -L --fail -o "dl/ndk.zip" "https://dl.google.com/android/repository/android-ndk-${NDK_VER}-linux.zip"
  unzip -q dl/ndk.zip -d "$ROOT/ndk-tmp"
  mv "$ROOT"/ndk-tmp/android-ndk-* "$ROOT/ndk"
  rmdir "$ROOT/ndk-tmp"; rm -f dl/ndk.zip
  echo "$NDK_VER" > "$ROOT/ndk/.ndk_version_tag"
fi
cat "$ROOT/ndk/source.properties"

echo "=== [4/4] Mesa source ==="
if [ ! -d "$ROOT/mesa/.git" ]; then
  git clone --depth 200 https://gitlab.freedesktop.org/mesa/mesa.git "$ROOT/mesa" 2>&1 | tail -2
fi
cd "$ROOT/mesa" && git log --oneline -1 && cat VERSION
echo "=== SETUP DONE ==="
