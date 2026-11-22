#!/bin/bash
# Linux build of ProjectWAR's four servers (replaces "build ProjectWAR.sln Release|x64" in Visual Studio).
# Compiler: .NET 10 SDK's MSBuild/Roslyn against Mono's .NET 4.8 reference assemblies. Runtime: Mono.
set -euo pipefail
ROOT=$HOME/ross-vr/server
SRC=$ROOT/src
S=$ROOT/scripts
OUT=$SRC/bin/Release
export DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1
cd "$SRC"
python3 -I "$S/make-linux-patch.py" "$SRC"                       # loopback bind option + kernel32 guards
python3 -I "$S/fix-case.py" "$SRC" Common FrameWork AccountCacher LauncherServer LobbyServer WorldServer
python3 -I "$S/nuget-restore.py" "$SRC"                          # packages.config restore
for p in AccountCacher LauncherServer LobbyServer WorldServer; do
  echo "== building $p"
  dotnet msbuild "$p/$p.csproj" -nologo -v:m -p:Configuration=Release -p:Platform=x64 \
    -p:FrameworkPathOverride=/usr/lib/mono/4.8-api -p:SolutionDir="$SRC/" -p:PostBuildEvent= \
    > "$ROOT/logs/build-$p.log" 2>&1 || { tail -30 "$ROOT/logs/build-$p.log"; exit 1; }
  grep -- "-> " "$ROOT/logs/build-$p.log" | sort -u
done
mkdir -p "$OUT/PatcherFilesDirectory" "$OUT/TempFilesDirectory"   # the skipped Windows PostBuildEvent
"$S/build-warzone.sh" "$OUT/libWarZone64.so"                       # native LOS library for Linux
python3 -I "$S/configure.py" "$OUT"                                # DB creds, dllmap
echo BUILD OK
