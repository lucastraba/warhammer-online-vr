#!/usr/bin/env python3
"""packages.config restore without nuget.exe (nuget.exe hung under Mono 6.14 on hal).
Downloads every <package id version> listed in */packages.config from nuget.org and
extracts it into <src>/packages/<Id>.<Version>/ the way 'nuget restore' does."""
import sys, os, re, glob, io, zipfile, urllib.request
src = sys.argv[1]
pkgs = set()
for pc in glob.glob(os.path.join(src, '*', 'packages.config')):
    for m in re.finditer(r'<package\s+id="([^"]+)"\s+version="([^"]+)"', open(pc, encoding='utf-8-sig').read()):
        pkgs.add((m.group(1), m.group(2)))
dest = os.path.join(src, 'packages')
os.makedirs(dest, exist_ok=True)
for pid, ver in sorted(pkgs):
    d = os.path.join(dest, f'{pid}.{ver}')
    if os.path.isdir(d) and os.listdir(d):
        continue
    url = f'https://api.nuget.org/v3-flatcontainer/{pid.lower()}/{ver.lower()}/{pid.lower()}.{ver.lower()}.nupkg'
    data = urllib.request.urlopen(url, timeout=120).read()
    os.makedirs(d, exist_ok=True)
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        for n in z.namelist():
            if n.startswith(('_rels/', 'package/')) or n == '[Content_Types].xml':
                continue
            z.extract(n, d)
    open(os.path.join(d, f'{pid}.{ver}.nupkg'), 'wb').write(data)
    print('restored', pid, ver)
print(len(pkgs), 'packages')
