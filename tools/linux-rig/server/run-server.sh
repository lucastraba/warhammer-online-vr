#!/bin/bash
# Runs one ProjectWAR server under Mono (used by the war-*.service user units).
# Under Mono the servers run "non-interactive" (no console commands); stop = SIGTERM.
set -euo pipefail
NAME=$1                                     # AccountCacher | LauncherServer | LobbyServer | WorldServer
BIN=$HOME/ross-vr/server/src/bin/Release
export PROJECTWAR_BIND_ADDRESS=127.0.0.1    # game-facing listeners on loopback only (patched TCPManager)
cd "$BIN"
# everything except AccountCacher needs AccountCacher's RPC port (127.0.0.1:6800)
if [ "$NAME" != AccountCacher ]; then
  for i in $(seq 1 120); do ss -ltn | grep -q '127.0.0.1:6800 ' && break; sleep 1; done
fi
exec mono --debug "$NAME.exe" < /dev/null
