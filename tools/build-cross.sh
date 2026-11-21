#!/usr/bin/env bash
# Cross-compile WARVR.dll and WARVRInjector.exe for 32-bit Windows from Linux or macOS, and run the unit tests natively.
# Needs: a C++ compiler for the tests, and either `zig` on PATH or `uvx` (uv) to fetch it (pip package "ziglang").
# Usage: tools/build-cross.sh [output-dir]      (default: build-cross/)
set -euo pipefail
cd "$(dirname "$0")/.."
out="${1:-build-cross}"
mkdir -p "$out"

if command -v zig >/dev/null 2>&1; then ZIG=(zig)
elif command -v uvx >/dev/null 2>&1; then ZIG=(uvx --from ziglang python -m ziglang)
else echo "need zig (or uv: https://docs.astral.sh/uv/)" >&2; exit 1; fi

CXX="${CXX:-c++}"
for t in vrmath_test shaderscan_test hookutil_test; do
  "$CXX" -std=c++17 -O1 -o "$out/$t" "tests/$t.cpp"
  "$out/$t" > "$out/$t.log" || { cat "$out/$t.log"; echo "$t FAILED" >&2; exit 1; }
  tail -n 1 "$out/$t.log"
done

WIN=(-target x86-windows-gnu -std=c++17 -O2 -DNDEBUG)
"${ZIG[@]}" c++ "${WIN[@]}" -shared -o "$out/WARVR.dll" src/WARVR.cpp -luser32
"${ZIG[@]}" c++ "${WIN[@]}" -o "$out/WARVRInjector.exe" src/injector.cpp
cp third_party/openvr/bin/win32/openvr_api.dll "$out/"
rm -f "$out"/*.pdb "$out"/*.lib
echo "built: $out/WARVR.dll $out/WARVRInjector.exe $out/openvr_api.dll"
