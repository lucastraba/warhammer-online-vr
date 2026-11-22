# Linux test rig: WAR + WARVR without a headset or Windows

Scripts to run Warhammer Online with WARVR on a Linux machine under Wine, against a **local** ProjectWAR
server, with `tools/fakevr` standing in for SteamVR. It exists so changes to the mod can be tested end to end
(game in the world, VR on, frames handed to the "headset") before anyone with real hardware has to.

It does not replace a real test. fakevr checks what it is given, but it is not a compositor, and the frame
rates on a laptop GPU under Wine say nothing about a gaming PC.

**Never point the client at the Return of Reckoning server.** Everything here is loopback only.

## Layout

| Path | What |
|---|---|
| `server/` | Build and run ProjectWAR on Linux under Mono. Has its own [README](server/README.md). |
| `client/war-display.sh` | Private 1600x900 screen with GPU access (labwc headless + Xwayland). |
| `client/war-layer.sh` | Pick the D3D9 layer: `dxvk`, `dgvoodoo` (on DXVK's d3d11) or `wine`. |
| `client/war-run.sh` | Log in to the local server and start `WAR.exe` the way the Launcher does. |
| `client/war-vr.sh` | Same, with `WARVRInjector.exe` started first. |
| `client/myp_login.py` | Point `data.myp`'s login config at the local server (the Launcher's patcher needs WMI, which Mono lacks). |

Expected folders (override with `WAR_ROOT`, default `~/ross-vr`):
`war/` (the game), `wineprefix-war/`, `layers/{dxvk,dgvoodoo}/`, and the server tree from `server/README.md`.

## One-time setup

1. Server: follow `server/README.md`, make an account with `server/create-account.py <name> <password>`.
2. Game: unpack the client into `$WAR_ROOT/war`, then
   `python3 -I client/myp_login.py $WAR_ROOT/war/data.myp 127.0.0.1 8048`.
3. Wine prefix: `WINEPREFIX=$WAR_ROOT/wineprefix-war winetricks -q d3dx9_34`. Wine's own d3dx9 shader
   compiler fails on some of WAR's shaders (black foliage, missing models).
4. Game settings: in `war/user/UserSettings.xml` set `<Mode ... fullscreen="false">` (the headless screen has
   no 1600x900 fullscreen mode, and the game crashes looking for it) and turn sound off if the machine has
   speakers.
5. Mod: copy `WARVR.dll`, `WARVRInjector.exe`, fakevr's `openvr_api.dll` and a `WARVR.ini` into `war/`:

   ```ini
   log=on
   auto_vr=on        ; switch VR on in the world without the hotkey (keys do not reach the game here)
   vr_snapshot=40    ; save what the headset gets after 40 VR frames: %TEMP%\WARVR_vr_snapshot_40.bmp
   ```

## A run

```sh
systemd-run --user --unit=war-display client/war-display.sh
export XDG_RUNTIME_DIR=/run/user/$(id -u) WAYLAND_DISPLAY=$(cut -d' ' -f2 $XDG_RUNTIME_DIR/war-display.env)
client/war-layer.sh dxvk                       # or dgvoodoo
systemd-run --user --unit=war-client --setenv=WAR_DISPLAY=:1 \
  --setenv=WINEDLLOVERRIDES="d3d9,d3d11,dxgi=n,b" --setenv=DXVK_FILTER_DEVICE_NAME="GTX 960M" \
  "$PWD/client/war-vr.sh" <account> <password>
# at character select (about 90 s; check with grim first, an early click is lost), click PLAY:
wlrctl pointer move -3000 -3000; wlrctl pointer move 368 473; wlrctl pointer click left
grim shot.png                                  # look at the screen
```

Logs: `$WAR_ROOT/war-client.log`, `$WAR_ROOT/war-injector.log`, WARVR's
`wineprefix-war/drive_c/users/<you>/AppData/Local/Temp/WARVR.log`, fakevr's `war/fakevr.log`.

Under Xwayland, `xdotool` clicks are ignored; use `wlrctl` (pointer) and `wtype` (keys).

## Results (hal9000: i7-4720HQ, GTX 960M on NVK (Mesa 26.0.8), Wine 10, DXVK 3.1.1, dgVoodoo 2.78.2), 2026-11-22

Both hand-off paths, in the world, VR on via `auto_vr`, a 4032x2240 side-by-side target:

| Layer | WARVR path | Submits | Picture (`vr_snapshot`) | fps |
|---|---|---|---|---|
| DXVK d3d9 | VK: one VkImage, both eyes, no copy | all `err=0` | correct stereo pair, HUD in both eyes | 2.3 |
| dgVoodoo on DXVK d3d11 | GPU: D3D11 copy of the game's render target | all `err=0` | correct stereo pair, HUD in both eyes | 1.6 |

`lazy_state=on` on dgVoodoo: correct picture as well, and the log's `PERF lazy_state` line shows about 3,800
restore calls per frame avoided (the game re-sent the values itself).

The fps column only shows that both paths run. The GPU runs at its lowest clock on this driver, and
dgVoodoo here sits on DXVK instead of a native D3D11 driver, so the numbers cannot be compared.

What broke on the way, all fixed in WARVR:

- **DXVK, crash at startup**: the shared-texture probe made DXVK ask for Win32 external memory, which Mesa
  under Wine does not have. The probe is skipped under DXVK (the VK path needs no shared handles).
- **dgVoodoo, GPU path never started**: the 5-byte hook on `D3D11CreateDevice` refused Wine's prologue
  (`endbr32`) and DXVK's (`sub esp,20h; mov eax,[esp+48h]`). The hook decoder now knows `endbr32` and
  ModRM memory operands, and has tests for both.
