Experimental VR mod for Warhammer Online: Age of Reckoning
by Ross Scott

## About this fork
This fork adds a build for people who don't compile, plus two experimental options aimed at the CPU bottleneck. They stay off unless you turn them on, so with your usual dgVoodoo setup the mod works as before.

-Download: the latest release on the Releases page of this fork (WARVR.dll, WARVRInjector.exe, openvr_api.dll, an example WARVR.ini). The files in bin\ below are the original build.

-lazy_state=on in WARVR.ini (with dgVoodoo): skips some of the extra work the mod does for every object.

-DXVK instead of dgVoodoo: see docs\DXVK.md.

-HUD shape: Ctrl+Shift+H switches the HUD between flat (as before), curved (bent around you, so the minimap and chat at the edges face you) and low (curved, lower and leaned back, a glance down). hud_style=flat|curved|low in WARVR.ini picks the one it starts with.

-draw_distance now also works without VR (flat screen).

-Tested only under Wine on Linux with a fake headset (tools\fakevr, tools\linux-rig), not yet on Windows or with a real headset. Like the original, written with Claude.

-Problems or logs (%TEMP%\WARVR.log): open an issue on this fork.

## Description:
This mod will activate full 3D VR rendering when inside Warhammer Online and should scale to any resolution.

This mod is 100% vibe-coded from Claude Sonnet 5.5, so it could be a mess code-wise.

There are no plans to support or develop this further, but you are welcome to do anything you want with it.  Hopefully someone who knows what they're doing will improve upon it and make this obsolete.

## Warning:
Running this mod on the "Return of Reckoning" server is NOT recommended!  You may get banned if you do.

## Requirements:
-A functional client of Warhammer Online: Age of Reckoning and a private or local server to connect to.

-VR headset capable of running OpenVR / SteamVR games. (tested on PCVR)

-A copy of dgvoodoo2 no later than 2.78.2.

-Windows (it might be possible for Linux, but is completely untested)

## Known issues:
-It has some sort of CPU bottleneck in some areas, causing a framerate drop.

-No motion controls have been added, just default game controls. 

-The HUD operates as a floating plane and is not optimized for VR use, but is functional. 

-It is possible some effects may still be bugged for VR, it has not been thoroughly tested.

-Occasional client crashes when loading characters (or possible zones), it's recommended not to activate it until you're fully in the game and disable it before loading new zones. 

## Installation:
This mod assumes you have a Warhammer Online - Age of Reckoning server already running.  If not, unsupported instructions are included in extras\server emulator instructions.txt

1. Copy the following to your Warhammer client directory:
third_party\openvr\bin\win32\openvr_api.dll
bin\WarVRInjector.exe
bin\WARVR.dll
2. Download dgvoodoo2 version no later than 2.78.2.  This mod was made using 2.78.1.
Source code:
https://github.com/dege-diosg/dgVoodoo2/releases
Binary for 2.78.1:
https://www.t2e.pl/t2e-download/dgvoodoo2.1046
3. Copy the contents from dgvoodoo2 from MS\x86 directory to your Warhammer client directory.
4. Run the included dgVoodooCpl.exe control panel
Click the "Add" button and navigate to your Warhammer client folder.
On the DirectX Tab change VRAM to 4096 (may not be necessary, but I recall it helping).
(optional) uncheck "dgvoodoo Watermark"
Press apply
5. Run WARVRInjector.exe prior to launching the game
6. Fully load a character in the game
7. Put on your VR headset.
8. Press Ctrl+Shift+Numpad* to enable / disable VR.  You may need to alt-tab back into the client.  Press again to disable.

## Additional features:
Creating WARVR.ini in the client directory has some additional options by adding these lines when the injector is launched:

draw_distance=1.0
(This extends the game draw distance beyond the default maximum.  1.3 looks good, but will reduce performance.

log=on
This outputs WARVR.log in your Windows temp directory, useful for diagnosing potential problems 

## Hotkeys:
Ctrl+Shift+Numpad* = activate / deactivate VR

Ctrl+Shift+M = show / hide mouse cursor

Ctrl+Shift+PageUp / Pagedown = change world scale

Ctrl+Shift+Del / End = move HUD forward or backwards in 3D space

Ctrl+Shift+ Numpad / = recenter headtracking

Ctrl+Shift+Numpad - = lock vertical camera motion so that your view stays level in VR, press again to disable

## Third-party software
- OpenVR SDK (openvr_capi.h, openvr_api.dll) - Copyright (c) 2015, Valve Corporation,
  BSD 3-Clause licence. https://github.com/ValveSoftware/openvr
  Full licence text: third_party/openvr/LICENSE

This project is an unofficial fan modification. It is not affiliated with or endorsed by
Valve, EA/Mythic, or the author of dgVoodoo2. SteamVR and OpenVR are trademarks of Valve.

