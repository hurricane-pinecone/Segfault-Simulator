#!/usr/bin/env bash
# Lint the engine against GCC/libstdc++ before pushing. The dev machine is
# Clang/libc++, which pulls many standard headers in transitively; CI's GCC does
# not, so a TU that uses e.g. std::string without <string> compiles locally but
# breaks in CI ("'string' in namespace 'std' does not name a type"). This runs a
# fast -fsyntax-only GCC pass (parse only, no compile to object, no link) over the
# core and runtime sources and tests, so those missing includes surface here.
#
# Usage:
#   scripts/gcc-lint.sh                       lint every engine TU
#   scripts/gcc-lint.sh engine/src/foo.cpp    lint only the given file(s)
#
# Needs Docker running. The GCC image is built once and cached; reruns are quick.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/.." && pwd)"

if ! docker info >/dev/null 2>&1; then
  echo "gcc-lint: Docker isn't running. Start Docker and retry." >&2
  exit 1
fi

echo "gcc-lint: preparing GCC image (cached after first run)..." >&2
docker build -q -t sfs-gcc-lint - <"$here/gcc-lint.Dockerfile" >/dev/null

docker run --rm -i -v "$root":/src:ro -w /src sfs-gcc-lint bash -s "$@" <<'INNER'
set -u

# Include roots mirror engine/CMakeLists.txt (core + runtime, vendored libs) plus
# the apt SDL headers; a superset so every TU resolves regardless of layer.
INC="-Iengine/include -Iengine/src -Iengine/lib -Iengine/lib/glm -Iengine/lib/lua -Iengine/lib/imgui -Iengine/lib/tracy/public -I/usr/include/SDL2"

# Files to check: the args passed through, or every engine TU bar game.cpp (its
# imgui.h comes from Conan, not the backends-only vendored tree, so it false-fails
# this apt-only check; it is GL/window glue, out of lint scope).
if [ "$#" -gt 0 ]; then
  printf '%s\0' "$@"
else
  find engine/src engine/tests -name '*.cpp' ! -name game.cpp -print0
fi >/tmp/files

fails="$(mktemp -d)"
export INC fails

check() {
  local f="$1" out
  if ! out="$(g++ -std=c++23 -fsyntax-only $INC "$f" 2>&1)"; then
    { echo "=== FAIL: $f ==="; echo "$out"; echo; } >"$fails/$(echo "$f" | tr / _)"
  fi
}
export -f check

xargs -0 -P "$(nproc)" -n1 bash -c 'check "$0"' </tmp/files

if compgen -G "$fails/*" >/dev/null; then
  cat "$fails"/*
  n="$(ls -1 "$fails" | wc -l | tr -d ' ')"
  echo "gcc-lint: FAILED -- $n file(s) do not parse under GCC/libstdc++"
  exit 1
fi
echo "gcc-lint: clean -- all TUs parse under GCC/libstdc++"
INNER
