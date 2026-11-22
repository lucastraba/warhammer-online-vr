#!/usr/bin/env python3
"""Logs in like the Launcher's "Connect" button: CL_START (0x03) with sha256("user:pass") to
127.0.0.1:8000. Prints LOGIN_SUCCESS and the session token the launcher would pass to WAR.exe."""
import socket, struct, sys, hashlib
user, pw = sys.argv[1].lower(), sys.argv[2].lower()
def s(x):
    return struct.pack('>I', len(x)) + x.encode()
payload = s(user) + s(hashlib.sha256(f'{user}:{pw}'.encode()).hexdigest())
c = socket.create_connection(('127.0.0.1', 8000), timeout=10)
c.sendall(struct.pack('>I', 4 + len(payload)) + bytes([0x03]) + payload)
r = b''
try:
    while True:
        d = c.recv(4096)
        if not d: break
        r += d
except socket.timeout:
    pass
names = {0: 'LOGIN_SUCCESS', 1: 'LOGIN_INVALID_USERNAME_PASSWORD', 2: 'LOGIN_BANNED', 3: 'LOGIN_NOT_ACTIVE', 4: 'LOGIN_PATCHER_NOT_ALLOWED'}
print('opcode', r[4] if len(r) > 4 else None, 'result', names.get(r[5]) if len(r) > 5 else None)
if len(r) > 10:
    n = struct.unpack('>I', r[6:10])[0]; print('token', r[10:10 + n].decode(errors='replace'))
