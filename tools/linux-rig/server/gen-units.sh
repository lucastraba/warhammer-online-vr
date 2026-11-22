#!/bin/bash
# Generates the systemd --user units into ./units
set -euo pipefail
cd "$(dirname "$0")"; mkdir -p units
for pair in war-accountcacher:AccountCacher war-launcher:LauncherServer war-lobby:LobbyServer war-world:WorldServer; do
  u=${pair%%:*}; n=${pair##*:}
  after=""; [ "$n" != AccountCacher ] && after=$'After=war-accountcacher.service\nRequires=war-accountcacher.service'
  to=30; [ "$n" = WorldServer ] && to=120
  cat > "units/$u.service" <<EOF
[Unit]
Description=ProjectWAR $n (Warhammer Online private server, local only)
PartOf=war-server.target
After=network.target
$after

[Service]
Type=simple
ExecStart=$PWD/run-server.sh $n
KillSignal=SIGTERM
TimeoutStopSec=$to
Restart=on-failure
RestartSec=15

[Install]
WantedBy=war-server.target
EOF
done
cat > units/war-server.target <<'EOF'
[Unit]
Description=ProjectWAR private server (all four servers)
Wants=war-accountcacher.service war-launcher.service war-lobby.service war-world.service
EOF
