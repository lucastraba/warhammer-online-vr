#!/bin/sh
# Start WAR.exe under Wine the way the ProjectWAR Launcher does: log in to the local server, then pass the
# session token on the command line (the Launcher itself crashes under Mono).
#   usage: war-run.sh <account> <password>      (an account made with ../server/create-account.py)
# Env: WAR_ROOT (default ~/ross-vr; the game is in $WAR_ROOT/war, the prefix in $WAR_ROOT/wineprefix-war),
#      WAR_DISPLAY (X display, default :55 = a private Xvfb started on demand), WINEDLLOVERRIDES, DXVK_*.
# Log: $WAR_ROOT/war-client.log (fresh per run). Detached:  systemd-run --user --unit=war-client war-run.sh <acct> <pw>
set -e
U=${1:?usage: war-run.sh <account> <password>}; P=${2:?usage: war-run.sh <account> <password>}
HERE=$(cd "$(dirname "$0")" && pwd)
WAR_ROOT=${WAR_ROOT:-$HOME/ross-vr}
LOG=$WAR_ROOT/war-client.log
exec > "$LOG" 2>&1
export WINEPREFIX=${WINEPREFIX:-$WAR_ROOT/wineprefix-war} WINEDEBUG=${WINEDEBUG:--all}
export DISPLAY=${WAR_DISPLAY:-:55}
if [ "$DISPLAY" = ":55" ] && ! systemctl --user is-active -q war-xvfb; then
  systemctl --user reset-failed war-xvfb 2>/dev/null || true
  systemd-run --user --unit=war-xvfb /usr/bin/Xvfb :55 -screen 0 1600x900x24 -nolisten tcp
  sleep 2
fi
TOK=$(python3 -I "$HERE/../server/test-login.py" "$U" "$P" | sed -n "s/^token //p")
[ -n "$TOK" ] || { echo "login failed (servers up? account exists?)"; exit 1; }
A=$(printf %s "$U" | base64 -w0); T=$(printf %s "$TOK" | base64 -w0)
cd "$WAR_ROOT/war"
echo "war-run: $(date -Is) display=$DISPLAY overrides=${WINEDLLOVERRIDES:-none}"
exec wine WAR.exe --acctname="$A" --sesstoken="$T"
