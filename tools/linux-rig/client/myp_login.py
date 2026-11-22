#!/usr/bin/env python3
"""Point WAR at a login server: replace mythloginserviceconfig.xml inside data.myp.
Same writes as the ProjectWAR Launcher (MYPHandler.ReplaceFile): sizes at entry+12/+16, compression byte at
entry+32, data in place if it fits, otherwise appended (header copied) with the new offset. CRC left as is.
usage: myp_login.py <data.myp> <host> [port]"""
import struct, sys, zlib
H = {0x3FE03665, 0x349E2A8C}
p, host = sys.argv[1], sys.argv[2]; port = int(sys.argv[3]) if len(sys.argv) > 3 else 8048
xml = ("<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n<RootElementOfAnyName>\r\n  <MythLoginServiceConfig>\r\n"
       "    <Settings>\r\n      <ProductId>2</ProductId>\r\n      <MessageTimeoutSecs>30</MessageTimeoutSecs>\r\n    </Settings>\r\n"
       "    <RegionList>\r\n      <Region regionName=\"prod\">\r\n        <PingServer serverName=\"None\">\r\n"
       "          <Address>%s</Address>\r\n          <Port>0</Port>\r\n        </PingServer>\r\n       <LoginServerList>\r\n"
       "          <LoginServer serverName=\"Warhammer Online\">\r\n            <Address>%s</Address>\r\n            <Port>%d</Port>\r\n"
       "          </LoginServer>\r\n       </LoginServerList>\r\n      </Region>\r\n    </RegionList>\r\n  </MythLoginServiceConfig>\r\n"
       "</RootElementOfAnyName>\r\n") % (host, host, port)
new = xml.encode()
with open(p, "r+b") as f:
    def rd(o, n): f.seek(o); return f.read(n)
    off = struct.unpack("<Q", rd(12, 8))[0]; ent = None
    while off and ent is None:
        cnt, nxt = struct.unpack("<IQ", rd(off, 12))
        tab = rd(off + 12, cnt * 34)
        for i in range(cnt):
            fo, hs, cs, us, h1, h2, crc, cm = struct.unpack_from("<QIIIIIIH", tab, i * 34)
            if {h1, h2} == H: ent = (off + 12 + i * 34, fo, hs, cs, cm); break
        off = nxt
    if ent is None: sys.exit("mythloginserviceconfig.xml not in archive")
    e, fo, hs, cs, cm = ent
    old = rd(fo + hs, cs); old = zlib.decompress(old) if cm == 1 else old
    print("old login server:", old.decode("utf-8", "replace").split("<LoginServer ")[1].split("</LoginServer>")[0].split())
    if len(new) <= cs:
        f.seek(fo + hs); f.write(new)
    else:
        hdr = rd(fo, hs); f.seek(0, 2); fo = f.tell(); f.write(hdr + new)
        f.seek(e); f.write(struct.pack("<Q", fo))
    f.seek(e + 12); f.write(struct.pack("<II", len(new), len(new)))
    f.seek(e + 32); f.write(b"\x00")
    print("now: %s:%d (%d bytes, stored uncompressed)" % (host, port, len(new)))
