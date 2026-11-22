#!/bin/bash
# Builds ProjectWAR's WarZone LOS library (WorldServer P/Invokes "WarZone64.dll") as libWarZone64.so.
# Builds from a copy so the git tree stays clean; the copy gets two Linux fixes:
#  - Win32 shim headers (scripts/warzone-compat) instead of windows.h/psapi.h/mmsystem.h
#  - dead, uncompilable 'tagA/tagB' initialisers removed from the bundled glm 0.9.3 tvec3
#    (MSVC never instantiates those templates, GCC rejects them at parse time)
set -euo pipefail
SRC=${SRC:-$HOME/ross-vr/server/src}
HERE=$(cd "$(dirname "$0")" && pwd)
# Windows.h (capital W) is generated: a case-only twin of windows.h would break a checkout on Windows.
[ -e "$HERE/warzone-compat/Windows.h" ] || printf '#pragma once\n#include "windows.h"\n' > "$HERE/warzone-compat/Windows.h"
BUILD=$HOME/ross-vr/server/build
OUT=${1:-$BUILD/libWarZone64.so}
rm -rf "$BUILD/warzone-src"; mkdir -p "$BUILD/warzone-src"
cp -r "$SRC/WarZone/WarZone/." "$BUILD/warzone-src/"
cd "$BUILD/warzone-src"
perl -0pi -e 's/,\s*tagA\(s\.tagA\),\s*tagB\(s\.tagB\)//; s/\s*tagA\(s\.tagA\),\s*tagA\(s\.tagB\),//' glm/core/type_vec3.inl
g++ -O2 -std=gnu++14 -fPIC -shared -fpermissive -w -fvisibility=hidden \
  -I"$HERE/warzone-compat" -I. \
  Source.cpp ZoneManager.cpp KDTreeCPU.cpp KDTreeNode.cpp Intersections.cpp Util.cpp \
  -o "$OUT"
nm -D --defined-only "$OUT" | grep -E " T (InitZones|LoadZone|UnLoadZone|SegmentIntersect|TerrainIntersect|GetFixture|SetFixture)"
