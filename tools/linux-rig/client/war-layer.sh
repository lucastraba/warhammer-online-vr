#!/bin/sh
# Pick the D3D9 layer WAR runs on, by copying DLLs into the game folder.
#   usage: war-layer.sh dxvk | dgvoodoo | wine
#   dxvk      DXVK d3d9.dll (WARVR's VK path: no copy, the VR image goes to SteamVR as a VkImage)
#   dgvoodoo  dgVoodoo 2 d3d9.dll on DXVK's d3d11/dxgi (WARVR's GPU path, what Windows users run). Wine's own
#             d3d11 renders dgVoodoo black/flat-shaded, so it gets DXVK underneath.
#   wine      Wine's built-in d3d9 (wined3d). Slow, no GPU hand-off.
# Layers live in $WAR_ROOT/layers/{dxvk,dgvoodoo}; fill them from the releases:
#   DXVK 3.1.1 x32/{d3d9,d3d11,dxgi}.dll -> layers/dxvk/
#   dgVoodoo2_78_2.zip MS/x86/D3D9.dll   -> layers/dgvoodoo/d3d9.dll
# Launch with WINEDLLOVERRIDES="d3d9,d3d11,dxgi=n,b" (harmless when a DLL is absent).
set -e
WAR_ROOT=${WAR_ROOT:-$HOME/ross-vr}
G=$WAR_ROOT/war; L=$WAR_ROOT/layers
rm -f "$G/d3d9.dll" "$G/d3d11.dll" "$G/dxgi.dll"
case "${1:?usage: war-layer.sh dxvk|dgvoodoo|wine}" in
  dxvk)     cp "$L/dxvk/d3d9.dll" "$G/" ;;
  dgvoodoo) cp "$L/dgvoodoo/d3d9.dll" "$L/dxvk/d3d11.dll" "$L/dxvk/dxgi.dll" "$G/"
            [ -e "$G/dgVoodoo.conf" ] || printf '[General]\r\nOutputAPI = d3d11_fl11_0\r\n\r\n[DirectX]\r\ndgVoodooWatermark = false\r\n' > "$G/dgVoodoo.conf" ;;
  wine)     ;;
  *) echo "unknown layer: $1"; exit 2 ;;
esac
echo "D3D9 layer: $1"
