#!/usr/bin/env python3
"""Creates a game account the same way the Launcher's "Create Account" button does:
CL_CREATE (0x05) to the LauncherServer on 127.0.0.1:8000. Usage: create-account.py <user> <pass>
(The launcher lower-cases both; GM level is set separately with SQL.)"""
import socket, struct, sys
user, pw = sys.argv[1].lower(), sys.argv[2].lower()
def s(x):
    b = x.encode('utf-8'); return struct.pack('>I', len(x)) + b
payload = s(user) + s(pw)
pkt = struct.pack('>I', 4 + len(payload)) + bytes([0x05]) + payload   # size = Length - opcode byte
c = socket.create_connection(('127.0.0.1', 8000), timeout=10)
c.sendall(pkt)
try:
    r = c.recv(64)
    print('reply', r.hex())
    if len(r) >= 6:
        print({0: 'ACCOUNT_NAME_BUSY (name taken)', 1: 'ACCOUNT_NAME_SUCCESS', 2: 'ACCOUNT_BANNED'}.get(r[5], r[5]))
except socket.timeout:
    print('no reply')
