#!/usr/bin/env python3
"""The csproj files were written on case-insensitive Windows. For every <Compile/Content/None Include>
path that does not exist with that exact case on Linux, add a symlink with the csproj's spelling
pointing at the real file (found case-insensitively). Tracked files are not modified."""
import sys, os, re, glob
src = sys.argv[1]
made = 0
for proj in sys.argv[2:]:
    pdir = os.path.join(src, proj)
    text = open(glob.glob(os.path.join(pdir, '*.csproj'))[0], encoding='utf-8-sig').read()
    for inc in re.findall(r'<(?:Compile|Content|None|EmbeddedResource)\s+Include="([^"*]+)"', text):
        rel = inc.replace('\\', '/')
        path = os.path.normpath(os.path.join(pdir, rel))
        if os.path.exists(path):
            continue
        # resolve case-insensitively, component by component
        cur = pdir
        for part in os.path.relpath(path, pdir).split('/'):
            if part == '..':
                cur = os.path.dirname(cur); continue
            cand = os.path.join(cur, part)
            if not os.path.exists(cand) and os.path.isdir(cur):
                hits = [e for e in os.listdir(cur) if e.lower() == part.lower()]
                if hits:
                    cand = os.path.join(cur, hits[0])
            cur = cand
        if os.path.exists(cur) and not os.path.exists(path):
            if os.path.isdir(os.path.dirname(path)):
                os.symlink(os.path.relpath(cur, os.path.dirname(path)), path)
                made += 1; print('link', os.path.relpath(path, src), '->', os.path.relpath(cur, src))
            else:
                print('dir case mismatch, not handled:', rel)
        else:
            print('MISSING', proj, rel)
print(made, 'links')
