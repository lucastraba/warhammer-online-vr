# fakevr: run WARVR without a headset

fakevr is a fake OpenVR runtime packaged as a 32-bit `openvr_api.dll`. It is a test double: WARVR (or any
32-bit app using the OpenVR C API) loads it instead of Valve's DLL and gets a "headset" that:

- initialises as Scene, Other or Background without SteamVR or a headset;
- reports a fixed render size, field of view, IPD and refresh rate (Valve Index-like defaults, configurable);
- paces `WaitGetPoses` like a compositor at a fixed refresh rate (90 Hz by default) and counts missed display slots;
- hands out a head pose that is either still or slowly sweeping left/right, so camera and culling code gets exercised;
- checks every `Submit` (texture type, handle, bounds, double submits) and returns the same error codes SteamVR uses;
- writes frame-time statistics to `fakevr.log` once per second and a summary at shutdown;
- can save what each eye submitted as BMP files.

The point is comparable numbers. Ross, or anyone without a headset, can run the same scene on two builds or two
machines and compare app frame rate, CPU frame time and missed frames. It also works under Wine on Linux.

## Install

1. In the Warhammer client folder (where `WARVR.dll` and `openvr_api.dll` are), rename Valve's `openvr_api.dll` to
   `openvr_api.dll.real`.
2. Put fakevr's `openvr_api.dll` there instead.
3. Optional: copy `fakevr.ini.example` to `fakevr.ini` in the same folder and edit it.
4. Start the game the usual way (injector, then the client) and press Ctrl+Shift+Numpad* to turn VR on.
   SteamVR doesn't need to be running.
5. To go back to the real headset, delete fakevr's DLL and rename `openvr_api.dll.real` back.

`fakevr.log` (and any `fakevr_dump_*.bmp`) appear next to the DLL. fakevr uses the folder it was loaded from, not
the working directory.

## Configuration (`fakevr.ini`)

Every key and its default is in [`fakevr.ini.example`](fakevr.ini.example). The main ones:

| key | default | meaning |
|---|---|---|
| `refresh_hz` | 90 | simulated display refresh rate; `WaitGetPoses` releases once per slot |
| `width`, `height` | 2016, 2240 | `GetRecommendedRenderTargetSize` |
| `ipd` | 0.063 | metres; eyes at x = ∓ipd/2 |
| `fov` | `index` | `index`, `narrow`, `wide`, or set `left_l/left_r/left_t/left_b/right_*` directly |
| `cant_deg` | 0 | rotate each eye outward (canted displays) |
| `pose` | `static` | `static` (identity at y=0) or `sweep` (`sweep_deg`, `sweep_period_s`, `bob_m`) |
| `head_yaw_deg`, `head_pitch_deg` | `0` | `pose=static` only: a fixed head turn (+yaw = right, +pitch = up), to look at what an off-centre head sees |
| `turn_after_submits` | `0` | hold the head straight until this many submits were accepted, so the app's recenter at VR start sees the turn as a turn |
| `dump_every` | 0 | write a BMP per eye every N frames (0 = off) |
| `fail_init` | 0 | 1 = `VR_InitInternal` fails with `fail_init_error` (108 HmdNotFound) |
| `log` | `on` | `off` = no log file |

fakevr re-reads the file on every `VR_InitInternal`, so you can change it between VR off/on toggles.

## Reading `fakevr.log`

```
[     1.357] stats: app_fps=88.9 submits/s=176.7 cpu_ms avg=0.48 max=19.09 missed=2 no_submit=0 submit_err=0 polls/s=0 | totals frames=89 missed=2 err=0
```

One line per second while a session runs (timestamps are seconds since the DLL loaded):

- `app_fps`: `WaitGetPoses` cycles per second, i.e. frames the game produced. At most `refresh_hz`.
- `submits/s`: successful `Submit` calls (2 per frame when both eyes go out).
- `cpu_ms avg/max`: time from `WaitGetPoses` returning to the game's next `WaitGetPoses` call. This is the app's
  whole frame (game, WARVR, rendering both eyes, submit) without the wait for the display. If it stays under
  1000/`refresh_hz` ms (11.1 ms at 90 Hz), the game holds full rate.
- `missed`: display slots that went by without a new frame. A real compositor would reproject or repeat
  the previous frame there. One frame of 12-22 ms at 90 Hz costs one slot.
- `no_submit`: frames that ended (next `WaitGetPoses`) without both eyes submitted.
- `submit_err`: rejected `Submit` calls. The first 20 are logged one by one with the reason (`submit error 102
  (InvalidTexture): ...`); after that they're only counted.
- `polls/s`: `GetDeviceToAbsoluteTrackingPose` calls (pose-reader / "Other" mode).

Also logged:

- the effective config, each `VR_InitInternal` / `VR_GetGenericInterface` call and its result;
- every distinct submitted texture per eye (`D3D11 texture 2016x2240 fmt=87 ... samples=1/0`) and every distinct
  bounds rectangle;
- each first call to an OpenVR function fakevr only stubs (`note: unimplemented ...`);
- a nested `VR_InitInternal` while a session is running, as a `WARNING`. DXVK does this to look for the compositor.

At `VR_ShutdownInternal` (or process exit) a summary is written:

```
summary: app_frames=301 avg_app_fps=86.83 display_slots=312 missed_slots(reprojected)=11 (3.53%) frames_without_both_eyes(dropped)=0
summary: submits_ok=602 submit_errors=8 InvalidTexture=4 IndexOutOfRange=1 AlreadySubmitted=1 InvalidBounds=2 dumps=24 dump_failures=0 pose_polls=0
summary: app_cpu_frame_ms n=300 avg=0.780 p50=0.1 p95=8.8 p99=16.1 max=19.090
```

`avg_app_fps` is measured over the display slots since the first `WaitGetPoses`, so loading time before VR started
doesn't count. Percentiles have 0.1 ms resolution.

### Getting comparable numbers

Use the same `fakevr.ini`, the same spot in the game, the same camera, and `pose=static` (or `sweep` with the same
settings). Let it run for at least 30 s and compare the summary lines. Leave `dump_every=0` for timing runs: a dump
waits for the GPU on that frame.

## Compositor statistics (`GetCumulativeStats`)

fakevr fills `Compositor_CumulativeStats` from its own counters (only as many bytes as the caller's
`nStatsSizeInBytes`):

| field | fakevr meaning |
|---|---|
| `m_nNumFramePresents` | display slots since the first `WaitGetPoses` = app frames + missed slots |
| `m_nNumReprojectedFrames` | missed slots |
| `m_nNumDroppedFrames` | frames that ended without both eyes submitted |
| `m_nNumFrameSubmits` | successful `Submit` calls (per eye) |
| `m_flSumTargetFrameTimes` | presents × (1000 / refresh_hz) ms |
| `m_flSumApplicationCPUTimeMS` | sum of the app CPU frame times above |
| `m_nPid` | the calling process |

Everything else is 0. These are fakevr's definitions; SteamVR counts some of them differently.

## What is implemented

- Exports (cdecl, undecorated, like Valve's DLL): `VR_InitInternal`, `VR_InitInternal2`, `VR_ShutdownInternal`,
  `VR_IsHmdPresent`, `VR_IsRuntimeInstalled`, `VR_GetRuntimePath`, `VR_GetGenericInterface`,
  `VR_IsInterfaceVersionValid`, `VR_GetInitToken`, `VR_GetVRInitErrorAsSymbol`,
  `VR_GetVRInitErrorAsEnglishDescription`.
- `VR_GetGenericInterface` serves only `FnTable:IVRSystem_026` and `FnTable:IVRCompositor_029` (the versions in
  `third_party/openvr/openvr_capi.h`). Any other name, including the C++ interface names DXVK asks for
  (`IVRCompositor_029`), returns null with `VRInitError_Init_InterfaceNotFound`. Before init it returns
  `VRInitError_Init_NotInitialized`.
- Both tables are complete: every slot points somewhere. The functions WARVR uses are implemented, plus the usual
  extras (projection matrix, vsync timing, device class and properties, events, controller state, ...). Everything
  else is a `__stdcall` stub that returns 0/false/error and logs its first call.
- `Submit` checks:
  - DirectX: the handle must be an `ID3D11Texture2D`.
  - Vulkan: image, device, physical device, instance and queue must all be non-null.
  - Bounds must be inside [0,1] and not empty.
  - Eye must be 0 or 1.
  - The same eye can't be submitted twice in one frame (`AlreadySubmitted`).

  Other texture types are accepted without checks.
- A second `VR_InitInternal` while a session is running (DXVK's pattern) keeps the running session; a Scene session is
  never downgraded. Like Valve's DLL there is no reference count: the next `VR_ShutdownInternal` ends the session.

## Limitations

- No compositor and no headset image: nothing is displayed, no lens distortion, no chaperone, no overlays, no mirror
  window. `GetMirrorTextureD3D11` fails.
- No motion controllers or input: only device 0 (the HMD) exists.
- Vulkan submits (WARVR's DXVK path) are checked but never read back or dumped.
- Dumps: D3D11 textures in BGRA8/RGBA8 (UNORM, sRGB or typeless) only. MSAA textures are resolved first (implemented but not covered by the test). The copy
  runs on the texture's own device immediate context, from the thread that calls `Submit` (the render thread).
- The timing comes from a fixed-rate timer, not a GPU vsync. It measures the app's CPU side and pacing. It does not
  measure GPU time or the latency a real compositor would add.
- A garbage (non-null, invalid) texture pointer still crashes, the same as with SteamVR.
- Built against the UCRT (`api-ms-win-crt-*`): fine on Windows 10/11 and Wine. Windows 7 needs the UCRT update.

## Build and test (Linux, zig)

```
tools/fakevr/build.sh [outdir]          # default outdir: $TMPDIR/fakevr-build (never commit binaries)
```

This builds `openvr_api.dll` (from `fakevr.cpp` and `fakevr.def`), plus two harnesses:

- `test_client.exe`: loads the DLL the way `src/openvr_head.h` does, checks init and failure paths, both tables,
  pacing, the `Submit` error paths, `GetCumulativeStats` and the log. It runs 300 frames of D3D11 submits, alternating
  separate red/blue eye textures with one green|yellow side-by-side texture plus bounds, and checks the pixels in the
  BMP dumps.
- `head_smoke.exe`: drives the mod's own `OpenVRHead` class against fakevr.

Both exit 0 only if every check passes. To run them under Wine without touching a real desktop, put the three files
in one folder and run:

```
xvfb-run -a -s "-screen 0 1280x800x24" env WINEPREFIX=$HOME/fakevr-wineprefix WINEDEBUG=-all wine test_client.exe
```

`test_client.exe` writes and deletes `fakevr.ini` in its folder, so don't run it in a folder whose `fakevr.ini` you
want to keep.
