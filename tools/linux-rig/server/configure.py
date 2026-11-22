#!/usr/bin/env python3
"""Post-build configuration of bin/Release for the local Linux setup (idempotent):
 - MySQL user/password in Configs/*.xml (Ross: 'check bin\\Release\\Configs\\*.xml')
 - Mono dllmap so WorldServer's [DllImport("WarZone64.dll")] loads libWarZone64.so"""
import sys, os, re
out = sys.argv[1]
DBUSER, DBPASS = os.environ.get('WAR_DB_USER', 'war'), os.environ.get('WAR_DB_PASS', 'warlocal')  # same defaults as db-setup.sh
for name in ('Account.xml', 'Lobby.xml', 'World.xml', 'Launcher.xml'):
    p = os.path.join(out, 'Configs', name)
    s = open(p, encoding='utf-8-sig').read()
    s2 = re.sub(r'<Username>[^<]*</Username>', f'<Username>{DBUSER}</Username>', s)
    s2 = re.sub(r'<Password>[^<]*</Password>', f'<Password>{DBPASS}</Password>', s2)
    if s2 != s:
        open(p, 'w', encoding='utf-8').write(s2); print('configured', name)
# WorldServer checks Directory.Exists("Zones"); the build copies deps/zones to "zones" (Linux is case-sensitive)
if not os.path.lexists(os.path.join(out, 'Zones')):
    os.symlink('zones', os.path.join(out, 'Zones')); print('symlink Zones -> zones')
cfg = os.path.join(out, 'WorldServer.exe.config')
s = open(cfg, encoding='utf-8-sig').read()
so = os.path.join(out, 'libWarZone64.so')
if 'WarZone64.dll' not in s:
    s = s.replace('</configuration>', f'  <dllmap dll="WarZone64.dll" target="{so}" os="!windows" />\n</configuration>')
    open(cfg, 'w', encoding='utf-8').write(s); print('dllmap added to WorldServer.exe.config')
