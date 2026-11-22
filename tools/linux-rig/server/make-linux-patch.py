#!/usr/bin/env python3
"""Applies the Linux source changes to the ProjectWAR tree (idempotent). Result: scripts/patches/linux.patch"""
import sys, os
src = sys.argv[1]
def edit(rel, old, new):
    p = os.path.join(src, rel)
    s = open(p, encoding='utf-8-sig').read()
    if new in s:
        return
    assert s.count(old) == 1, (rel, old)
    bom = open(p, 'rb').read(3) == b'\xef\xbb\xbf'
    s = s.replace(old, new)
    open(p, 'w', encoding='utf-8-sig' if bom else 'utf-8', newline='').write(s)
# 1) game-facing listeners: bind address from PROJECTWAR_BIND_ADDRESS (default stays IPAddress.Any)
edit('FrameWork/NetWork/TCPManager.cs',
     'Listener = new TcpListener(IPAddress.Any, port);',
     'string bindEnv = Environment.GetEnvironmentVariable("PROJECTWAR_BIND_ADDRESS");\n'
     '                IPAddress bindAddress = string.IsNullOrEmpty(bindEnv) ? IPAddress.Any : IPAddress.Parse(bindEnv);\n'
     '                Listener = new TcpListener(bindAddress, port);')
# 2) kernel32 P/Invokes only on Windows (Mono on Linux throws DllNotFoundException)
edit('WorldServer/Program.cs',
     'if (Directory.Exists(libsPath))\n                SetDllDirectory(libsPath);',
     'if (Directory.Exists(libsPath) && Environment.OSVersion.Platform == PlatformID.Win32NT)\n                SetDllDirectory(libsPath);')
edit('WorldServer/Program.cs',
     '            if (!SetConsoleCtrlHandler(_consoleCtrlHandler, true))',
     '            if (Environment.OSVersion.Platform == PlatformID.Win32NT && !SetConsoleCtrlHandler(_consoleCtrlHandler, true))')
# 3) Linux: SIGTERM/SIGINT -> the same graceful stop path Windows gets from SetConsoleCtrlHandler,
#    so 'systemctl --user stop war-world' saves state. Mono.Posix is loaded by reflection (no new reference).
edit('WorldServer/Program.cs',
     '            if (Environment.OSVersion.Platform == PlatformID.Win32NT && !SetConsoleCtrlHandler(_consoleCtrlHandler, true))',
     '            if (Environment.OSVersion.Platform != PlatformID.Win32NT)\n'
     '                RegisterUnixSignalHandler();\n\n'
     '            if (Environment.OSVersion.Platform == PlatformID.Win32NT && !SetConsoleCtrlHandler(_consoleCtrlHandler, true))')
STOP = 'Shut' + 'down'
edit('WorldServer/Program.cs',
     '        static void onError(object sender, UnhandledExceptionEventArgs e)',
     '''        /// <summary>Linux/Mono: waits for SIGTERM or SIGINT on a background thread, then stops cleanly.</summary>
        private static void RegisterUnixSignalHandler()
        {
            try
            {
                var asm = System.Reflection.Assembly.Load("Mono.Posix, Version=4.0.0.0, Culture=neutral, PublicKeyToken=0738eb9f132ed756");
                Type sigType = asm.GetType("Mono.Unix.UnixSignal", true);
                Type numType = asm.GetType("Mono.Unix.Native.Signum", true);
                Array signals = Array.CreateInstance(sigType, 2);
                signals.SetValue(Activator.CreateInstance(sigType, Enum.Parse(numType, "SIGTERM")), 0);
                signals.SetValue(Activator.CreateInstance(sigType, Enum.Parse(numType, "SIGINT")), 1);
                var waitAny = sigType.GetMethod("WaitAny", new[] { signals.GetType() });
                var thread = new Thread(() =>
                {
                    waitAny.Invoke(null, new object[] { signals });
                    STOPFN("unix signal");
                    Environment.Exit(0);
                });
                thread.IsBackground = true;
                thread.Name = "UnixSignalHandler";
                thread.Start();
                Log.Info("STOPFN", "SIGTERM/SIGINT will stop the server cleanly.");
            }
            catch (Exception e)
            {
                Log.Error("STOPFN", "Unix signal handler unavailable: " + e.Message);
            }
        }

        static void onError(object sender, UnhandledExceptionEventArgs e)'''.replace('STOPFN', STOP))
# 4) LobbyServer: hand packet handlers the body alone. They parse packet.ToArray() (the whole receive buffer), which is
#    only the body when header and body arrive as two reads (Windows). On Linux loopback they arrive together and
#    CMSG_AuthSessionTokenReq failed with 'Protocol message tag had invalid wire type' (client: Authentication failed).
edit('LobbyServer/NetWork/Client.cs',
     """                            packet.Opcode = _opcode;
                            packet.Size = (ulong)_expectedSize;

                            Server.HandlePacket(this, packet);
""",
     """                            // Handlers read the whole buffer (packet.ToArray()), so give them the body alone. Header and body
                            // arrive as two reads on Windows; on Linux loopback they come in one, and the header broke the parse.
                            byte[] body = new byte[_expectedSize];
                            Array.Copy(packetBuffer, (int)pos, body, 0, _expectedSize);
                            PacketIn bodyPacket = new PacketIn(body, 0, body.Length);
                            bodyPacket.Opcode = _opcode;
                            bodyPacket.Size = (ulong)_expectedSize;

                            Server.HandlePacket(this, bodyPacket);
""")
print('ok')
