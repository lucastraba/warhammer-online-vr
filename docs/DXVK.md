# Running WARVR on DXVK instead of dgVoodoo (experimental)

**Status: tested by Ross Scott on Windows with SteamVR and a headset (2026-11-22): 90 fps across the board, including
the spots where dgVoodoo dropped frames, with GPU use up to about 80%.** Before that it was checked under Wine with a fake
headset (`tools/fakevr`), see [tools/linux-rig/README.md](../tools/linux-rig/README.md). If something goes wrong, set
`log=on` and send `%TEMP%\WARVR.log`.

## Why

WAR is limited by one CPU thread. With dgVoodoo, every D3D9 call the game makes (twice per object in VR) is translated to
D3D11 on that same thread. DXVK translates D3D9 to Vulkan and does most of that work on its own worker thread, so the game
thread has less to do. Flat-screen WAR players report noticeably higher frame rates with DXVK. In VR the mod issues every
world draw twice, so the translation cost matters even more.

DXVK also lets the mod hand the finished picture to SteamVR as a Vulkan image: no copy, no CPU readback. With dgVoodoo the
mod has to find dgVoodoo's private D3D11 device first (the "GPU path"), and falls back to a slow CPU copy if it can't.

## Setup

1. Start **SteamVR first**, then the game. (DXVK asks SteamVR which Vulkan extensions it needs at the moment the game
   creates its D3D9 device. If SteamVR isn't running then, the hand-off can't work and the mod falls back to the CPU path.)
2. Remove dgVoodoo's `D3D9.dll` (and its `D3DImm.dll`, `DDraw.dll`) from the Warhammer folder. Keep a backup.
3. Download DXVK from https://github.com/doitsujin/dxvk/releases and copy **`x32\d3d9.dll`** into the Warhammer folder
   (WAR.exe is 32-bit; the x64 one will not load). DXVK needs a Vulkan 1.3 capable GPU and driver.
4. Install WARVR as usual (`WARVR.dll`, `WARVRInjector.exe`, `openvr_api.dll`), run the injector, load a character,
   press `Ctrl+Shift+Numpad*`.

Nothing else changes; `vk_path=auto` is the default. `vk_path=off` in WARVR.ini disables the hand-off.

## Texture filtering

dgVoodoo can force anisotropic filtering; DXVK doesn't by default, so distant textures can look softer. To force it, create
a text file `dxvk.conf` next to `WAR.exe` (where DXVK's `d3d9.dll` is) containing:

```
d3d9.samplerAnisotropy = 16
```

DXVK reads it at startup (its own log shows `Found config file: dxvk.conf` and `d3d9.samplerAnisotropy = 16`).

## DXVK's log file

DXVK writes its own log, `WAR_d3d9.log`, next to `WAR.exe` every time. That one is DXVK's, not the mod's: `WARVR.log` (in
`%TEMP%`) is still only written with `log=on`, as before, so the two never both appear unless you ask for the mod's log.
DXVK's log can't be switched off from `WARVR.ini` or `dxvk.conf`; set the environment variable `DXVK_LOG_LEVEL=none` instead
(for example `setx DXVK_LOG_LEVEL none` in a command prompt, then start the launcher again). Remove it if you ever need
DXVK's log for a problem.

## What to look for in the log

```
D3D9 layer: ...\d3d9.dll -> DXVK
VK path: preloaded openvr_api.dll for DXVK -> ok
VK path: DXVK interop device found: VkInstance=... VkQueue=... (family 0, index 0)
VK path: DXVK is the D3D9 layer; eye pictures go to SteamVR as Vulkan images (no copy, no CPU readback)
VK path: explicit timing mode set ...
VK path: ACTIVE - submitting VkImage 0x... 12480x6280 (VkFormat 44, layout ..., usage 0x...) straight to SteamVR
VR: submit pair 0 (VK) -> left err=0 right err=0
```

and every 300 frames a `VK PATH (DXVK): ... active=1 frames=...` line next to the usual `PERF` lines. Compare the
`PERF ... game thread busy` number with a dgVoodoo run in the same spot.

If SteamVR rejects the images five times in a row, the log says so and the mod switches to the CPU path for the session.

## How it works (for developers)

`src/vkpath.h`. DXVK exposes `ID3D9VkInteropDevice` / `ID3D9VkInteropTexture` (DXVK `src/d3d9/d3d9_interfaces.h`). Per frame:
transition the VR render target to `TRANSFER_SRC_OPTIMAL` -> `FlushRenderingCommands` -> `LockSubmissionQueue` ->
`IVRCompositor::Submit` twice with `TextureType_Vulkan` and the left/right half as texture bounds -> `ReleaseSubmissionQueue`
-> transition back. Explicit timing mode `ApplicationPerformsPostPresentHandoff` keeps `WaitGetPoses` off DXVK's queue;
`PostPresentHandoff` runs under the queue lock. This matches Proton's `vrclient_x64/vrcompositor_manual.c`.
SteamVR requires `SAMPLED | TRANSFER_SRC` image usage, which DXVK only gives to textures, so under DXVK the VR render
target is created as a render-target texture instead of a plain render-target surface.

Known cost: `FlushRenderingCommands` waits for DXVK's worker thread to finish the frame before the hand-off.
