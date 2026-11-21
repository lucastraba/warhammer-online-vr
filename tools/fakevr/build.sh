#!/usr/bin/env bash
# Build the fake OpenVR runtime (openvr_api.dll) and its test client, 32-bit Windows, with zig's clang.
#   tools/fakevr/build.sh [outdir]      (default: $FAKEVR_OUT, else $TMPDIR/fakevr-build, else /tmp/fakevr-build)
# Needs: uv (uvx fetches the ziglang wheel on first use) or a `zig` on PATH. Never commit the outputs.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out="${1:-${FAKEVR_OUT:-${TMPDIR:-/tmp}/fakevr-build}}"
mkdir -p "$out"

if command -v zig >/dev/null 2>&1; then CXX=(zig c++)
else CXX=(uvx --from ziglang python -m ziglang c++); fi

common=(-target x86-windows-gnu -std=c++17 -O2 -Wall -Wno-unknown-pragmas)

echo "== openvr_api.dll (fakevr)"
"${CXX[@]}" "${common[@]}" -shared -o "$out/openvr_api.dll" \
    "$here/fakevr.cpp" "$here/fakevr.def" \
    -ld3d11 -ldxgi -lwinmm -luser32 -lkernel32

echo "== test_client.exe"
"${CXX[@]}" "${common[@]}" -o "$out/test_client.exe" \
    "$here/test_client.cpp" \
    -ld3d11 -ldxgi -luser32 -lkernel32

echo "== head_smoke.exe (drives src/openvr_head.h)"
"${CXX[@]}" "${common[@]}" -o "$out/head_smoke.exe" \
    "$here/head_smoke.cpp" \
    -luser32 -lkernel32

rm -f "$out"/*.pdb "$out"/*.lib 2>/dev/null || true
echo "== outputs in $out"
ls -l "$out"/openvr_api.dll "$out"/test_client.exe "$out"/head_smoke.exe
