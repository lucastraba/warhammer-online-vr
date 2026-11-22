#!/bin/sh
# WAR + WARVR: start WARVRInjector.exe (it waits for WAR.exe), then the game through war-run.sh, in the same Wine
# prefix and on the same display. Arguments and env as for war-run.sh. Injector output: $WAR_ROOT/war-injector.log
HERE=$(cd "$(dirname "$0")" && pwd)
WAR_ROOT=${WAR_ROOT:-$HOME/ross-vr}
export WINEPREFIX=${WINEPREFIX:-$WAR_ROOT/wineprefix-war} WINEDEBUG=${WINEDEBUG:--all} DISPLAY=${WAR_DISPLAY:-:55}
cd "$WAR_ROOT/war"
wine WARVRInjector.exe > "$WAR_ROOT/war-injector.log" 2>&1 &
sleep 3
exec "$HERE/war-run.sh" "$@"
