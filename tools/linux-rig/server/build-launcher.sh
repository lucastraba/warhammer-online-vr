#!/bin/bash
# Builds the client-side Launcher (bin/Release/Launcher/Launcher.exe, the folder Ross copies into the
# WAR client folder). Its csproj calls the Windows SDK resgen.exe by absolute C:\ path, so this builds a
# generated copy of the project file (Launcher/Launcher.linux.csproj, untracked) without that target;
# resources are written in the preserialized format instead (read at runtime by System.Resources.Extensions.dll,
# which the project already references). Mono's resgen cannot be used: it rejects LauncherForm.resx
# ("No valid icon entry were found").
set -euo pipefail
SRC=${SRC:-$HOME/ross-vr/server/src}
LOG=$HOME/ross-vr/server/logs/build-Launcher.log
export DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1
cd "$SRC/Launcher"
python3 -I - <<'EOF'
s = open('Launcher.csproj', encoding='utf-8-sig').read()
start = s.index('<Target Name="CompileResourcesWithClassicResgen"')
end = s.index('</Target>', start) + len('</Target>')
new = ''  # classic-resgen target dropped; MSBuild writes preserialized resources (System.Resources.Extensions)
open('Launcher.linux.csproj', 'w', encoding='utf-8').write(s[:start] + new + s[end:])
EOF
python3 -I "$HOME/ross-vr/server/scripts/fix-case.py" "$SRC" Launcher
dotnet msbuild Launcher.linux.csproj -nologo -v:m -p:Configuration=Release -p:Platform=x64 \
  -p:FrameworkPathOverride=/usr/lib/mono/4.8-api -p:SolutionDir="$SRC/" \
  -p:PostBuildEvent= -p:GenerateResourceUsePreserializedResources=true > "$LOG" 2>&1 || { grep -E "error" "$LOG" | sort -u | head -20; exit 1; }
grep -- "-> " "$LOG" | sort -u
ls -la "$SRC/bin/Release/Launcher/"
# System.Resources.Extensions 4.6.0 (reads the preserialized resources) needs System.Memory 4.0.1.1 and
# friends; the project does not ship them. Drop the exact assembly versions it binds to into libs/.
python3 -I - "$SRC/bin/Release/Launcher/libs" <<'EOF'
import sys, io, os, zipfile, urllib.request
dest = sys.argv[1]
for pid, ver, path in [('System.Memory', '4.5.4', 'lib/net461/System.Memory.dll'),
                       ('System.Buffers', '4.5.1', 'lib/net461/System.Buffers.dll'),
                       ('System.Runtime.CompilerServices.Unsafe', '4.5.3', 'lib/net461/System.Runtime.CompilerServices.Unsafe.dll'),
                       ('System.Numerics.Vectors', '4.5.0', 'lib/net46/System.Numerics.Vectors.dll')]:
    url = f'https://api.nuget.org/v3-flatcontainer/{pid.lower()}/{ver}/{pid.lower()}.{ver}.nupkg'
    z = zipfile.ZipFile(io.BytesIO(urllib.request.urlopen(url, timeout=120).read()))
    open(os.path.join(dest, os.path.basename(path)), 'wb').write(z.read(path))
    print('libs/', os.path.basename(path), 'from', pid, ver)
EOF
