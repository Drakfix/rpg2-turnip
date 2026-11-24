#!/usr/bin/env bash
# Builds test/tu_smoke and test/tu_bench for Android arm64.
# Output: ~/turnip-build/test/{tu_smoke,tu_bench}, copied to <repo>/out/
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="${ROOT:-$HOME/turnip-build}"
TC="$ROOT/ndk/toolchains/llvm/prebuilt/linux-x86_64/bin"
OUT="$ROOT/test"
mkdir -p "$OUT" "$HERE/../out"

spv() { # spv <varname> <shader file>
  glslangValidator -V --vn "$1" -o "$OUT/$1.h" "$HERE/shaders/$2" >/dev/null
}
spv tri_vert tri.vert
spv tri_frag tri.frag
spv sq_comp sq.comp
spv trig_comp trig.comp
spv quad_vert quad.vert
spv simple_frag simple.frag
spv heavy_frag heavy.frag
spv tex_frag tex.frag
spv gbuf_frag gbuf.frag
spv light_frag light.frag

for t in tu_smoke tu_bench; do
  "$TC/aarch64-linux-android34-clang" -O2 -Wall -Wno-unused-variable -I"$OUT" \
    -o "$OUT/$t" "$HERE/$t.c" -ldl -lm
  cp "$OUT/$t" "$HERE/../out/$t"
  ls -la "$OUT/$t"
done
