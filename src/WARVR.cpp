// WARVR phase 4: head-tracked camera + diagnostics.
//
// Kept from phase 3 (proven by your log): the WAR loader hook -> Direct3DCreate9 wrapper ->
// CreateDevice hook -> device vtable hooks.
//
// New in phase 4:
//   * Per-pass tracking (perspective vs ortho, vertex shader active or not) so we learn whether
//     WAR's world is drawn with fixed-function + SetTransform (camera can be steered from here)
//     or with shaders (needs constant-register hooks instead).
//   * One-frame capture (Ctrl+Shift+F10): every transform / render target / viewport / clear /
//     shader / draw of a single frame, in order.
//   * One-shot device probes (present params, caps, backbuffer, shared-resource support, D3D9Ex).
//   * Head-tracked camera (Ctrl+Shift+F12) using OpenVR: HMD rotation is applied on top of WAR's
//     own view matrix in every perspective pass. No stereo yet; this proves camera control.
//
// Hotkeys: hold Ctrl+Shift and tap (registered with Windows, so the game never sees them; remap in WARVR.ini):
//   Numpad *   VR mode on/off (headset display + tracking)     Numpad -   recenter
//   C          capture one frame to the log                    Numpad +   20-degree rotation test
//   Numpad /  or  main-keyboard /   recenter too (alternate keys; capture is C)
//   Insert     head tracking on the monitor only (no headset image)
//   PageUp / PageDown   world scale (units per metre) x1.1 / /1.1 -- calibrate until the world feels life-size
//   End / Delete        HUD closer / farther
//   Home       engine culling follows your head on/off (default on)
//   P          head POSITION tracking on/off
//   M          mouse cursor in VR on/off
// Every hotkey beeps so you know it registered.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <mutex>
#include <set>
#include <unordered_map>
#include <map>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#if defined(_MSC_VER)
#include <intrin.h>
#define RETADDR() _ReturnAddress()
#else
#define RETADDR() __builtin_return_address(0)
#endif
#include "vrmath.h"
#include "shaderscan.h"
#include "openvr_head.h"
#include "vrdisplay.h"
#include "gpupath.h"
#include "vkpath.h"
#if defined(__GNUC__)
#pragma GCC target("sse2")
#endif
#include <emmintrin.h>

// ---------------------------------------------------------------- globals
static HMODULE g_self = nullptr;
static HANDLE g_worker = nullptr;
static std::atomic<bool> g_running{true};
static FILE* g_log = nullptr;

// ---- performance counters: only measured while the log is open (log=on), so they cost nothing otherwise ----
enum { PF_FRAME, PF_STEREO, PF_ISSUE, PF_SCAN, PF_BEFORE, PF_WAIT, PF_STATE, PF_COUNT };
static int64_t g_pf[PF_COUNT] = {};
static unsigned g_pfFrames = 0;
static inline int64_t PerfTick() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
// The game's own state calls (SetTexture, SetRenderState, shader constants) are timed on every 16th call only and scaled up, so the
// measurement itself costs almost nothing. It shows how much of the frame is spent inside dgVoodoo for ordinary API calls.
static unsigned g_stateCalls = 0, g_stateCallsTotal = 0;
#define PF_SAMPLE_BEGIN(ts) int64_t ts = 0; if (g_log) { ++g_stateCallsTotal; if (!(++g_stateCalls & 15)) ts = PerfTick(); }
#define PF_SAMPLE_END(ts) do { if (ts) g_pf[PF_STATE] += (PerfTick() - (ts)) * 16; } while (0)
static std::mutex g_logMutex;
static bool g_flushEachLine = true;

using Direct3DCreate9_t = IDirect3D9* (WINAPI*)(UINT);
using CreateDevice_t = HRESULT (WINAPI*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
using Reset_t = HRESULT (WINAPI*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using Present_t = HRESULT (WINAPI*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using SetRenderTarget_t = HRESULT (WINAPI*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
using BeginScene_t = HRESULT (WINAPI*)(IDirect3DDevice9*);
using Clear_t = HRESULT (WINAPI*)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
using SetTransform_t = HRESULT (WINAPI*)(IDirect3DDevice9*, D3DTRANSFORMSTATETYPE, const D3DMATRIX*);
using SetViewport_t = HRESULT (WINAPI*)(IDirect3DDevice9*, const D3DVIEWPORT9*);
using DrawPrimitive_t = HRESULT (WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
using DrawIndexedPrimitive_t = HRESULT (WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
using DrawPrimitiveUP_t = HRESULT (WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
using DrawIndexedPrimitiveUP_t = HRESULT (WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT);
using SetFVF_t = HRESULT (WINAPI*)(IDirect3DDevice9*, DWORD);
using SetVertexShader_t = HRESULT (WINAPI*)(IDirect3DDevice9*, IDirect3DVertexShader9*);
using SetPixelShader_t = HRESULT (WINAPI*)(IDirect3DDevice9*, IDirect3DPixelShader9*);
using SetVSConstF_t = HRESULT (WINAPI*)(IDirect3DDevice9*, UINT, const float*, UINT);
using SetDepthStencilSurface_t = HRESULT (WINAPI*)(IDirect3DDevice9*, IDirect3DSurface9*);
using SetScissorRect_t = HRESULT (WINAPI*)(IDirect3DDevice9*, const RECT*);
using SetPSConstF_t = HRESULT (WINAPI*)(IDirect3DDevice9*, UINT, const float*, UINT);
using SetTexture_t = HRESULT (WINAPI*)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
using SetRenderState_t = HRESULT (WINAPI*)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD);
using CreateVS_t = HRESULT (WINAPI*)(IDirect3DDevice9*, const DWORD*, IDirect3DVertexShader9**);
using CreatePS_t = HRESULT (WINAPI*)(IDirect3DDevice9*, const DWORD*, IDirect3DPixelShader9**);
using StretchRect_t = HRESULT (WINAPI*)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const RECT*, D3DTEXTUREFILTERTYPE);

// IDirect3DDevice9 vtable slots (verified against the SDK d3d9.h).
enum : size_t {
    SLOT_RESET = 16, SLOT_PRESENT = 17, SLOT_SETRENDERTARGET = 37, SLOT_BEGINSCENE = 41, SLOT_CLEAR = 43,
    SLOT_SETTRANSFORM = 44, SLOT_SETVIEWPORT = 47, SLOT_DRAWPRIM = 81, SLOT_DRAWINDEXEDPRIM = 82,
    SLOT_DRAWPRIMUP = 83, SLOT_DRAWINDEXEDPRIMUP = 84, SLOT_SETFVF = 89, SLOT_SETVS = 92,
    SLOT_SETVSCONSTF = 94, SLOT_SETPS = 107, SLOT_SETDEPTHSTENCIL = 39, SLOT_SETSCISSOR = 75,
    SLOT_STRETCHRECT = 34, SLOT_SETTEXTURE = 65, SLOT_SETPSCONSTF = 109, SLOT_SETRENDERSTATE = 57, SLOT_CREATEVS = 91, SLOT_CREATEPS = 106,
};
static constexpr size_t SLOT_CREATEDEVICE = 16;   // IDirect3D9

static Direct3DCreate9_t g_origCreate9 = nullptr;
static CreateDevice_t g_origCreateDevice = nullptr;
static Reset_t g_origReset = nullptr;
static Present_t g_origPresent = nullptr;
static SetRenderTarget_t g_origSetRenderTarget = nullptr;
static BeginScene_t g_origBeginScene = nullptr;
static Clear_t g_origClear = nullptr;
static SetDepthStencilSurface_t g_origSetDS = nullptr;
static SetScissorRect_t g_origSetScissor = nullptr;
static SetTransform_t g_origSetTransform = nullptr;
static SetViewport_t g_origSetViewport = nullptr;
static DrawPrimitive_t g_origDrawPrimitive = nullptr;
static DrawIndexedPrimitive_t g_origDrawIndexedPrimitive = nullptr;
static DrawPrimitiveUP_t g_origDrawPrimitiveUP = nullptr;
static DrawIndexedPrimitiveUP_t g_origDrawIndexedPrimitiveUP = nullptr;
static SetFVF_t g_origSetFVF = nullptr;
static SetVertexShader_t g_origSetVertexShader = nullptr;
static SetPixelShader_t g_origSetPixelShader = nullptr;
static SetVSConstF_t g_origSetVSConstF = nullptr;
static SetPSConstF_t g_origSetPSConstF = nullptr;
static SetTexture_t g_origSetTexture = nullptr;
static SetRenderState_t g_origSetRenderState = nullptr;
static CreateVS_t g_origCreateVS = nullptr;
static CreatePS_t g_origCreatePS = nullptr;
// Which float constants each of the game's shaders reads (parsed from the bytecode when it creates them), so only those are patched.
static std::unordered_map<void*, ss::ConstMask> g_vsMasks, g_psMasks;
static SRWLOCK g_maskLock = SRWLOCK_INIT;
static const ss::ConstMask* g_curVSMask = nullptr;   // masks of the shaders currently set (nullptr = unknown: patch everything)
static const ss::ConstMask* g_curPSMask = nullptr;
static unsigned g_shaderAnalysed[2] = {}, g_shaderRefused[2] = {}, g_patchesSkipped = 0;
static DWORD g_gameCW = 0xF;                     // the colour-write mask the GAME last asked for (the device may hold it with alpha masked off)
static int g_ownRS = 0;                          // >0 while the mod's own passes run: their SetRenderState calls are not the game's
struct OwnRS { OwnRS() { ++g_ownRS; } ~OwnRS() { --g_ownRS; } };
static DWORD g_rsShadow[256] = {};              // the game's render states as last set (only tracked with log=on, for the capture)
static StretchRect_t g_origStretchRect = nullptr;

// loader hook (unchanged from phase 3)
static uintptr_t g_loaderAddress = 0;
static void* g_loaderTrampoline = nullptr;
static bool g_loaderHooked = false;

// render-thread state
static unsigned long long g_frame = 0;
static bool g_lastProjPersp = false;
static bool g_havePendingView = false;
static D3DMATRIX g_pendingView{};
// The game's ORIGINAL (unrotated) transforms, always kept. Shader passes read a combined
// View*Projection from vertex-shader constants c0..c3; we check that against these and, when it
// matches, replace it with (View*Q)*Projection.
static D3DMATRIX g_origView{}, g_origProj{};
static bool g_haveOrigView = false, g_haveOrigProj = false;
// Is the current render target the main (back-buffer sized) one? Offscreen passes
// (minimap / 256x256 / 512x512 / shadow) are never steered.
static bool g_rtIsMain = true;
static UINT g_bbW = 0, g_bbH = 0;
static UINT g_curRTw = 0, g_curRTh = 0;
static unsigned g_framePerspDraws = 0;
// automatic captures so every log contains an in-world frame even if a hotkey never works
static bool g_autoWorldCaptured = false;
static unsigned g_worldFrames = 0;
static int g_captureCountdown = -1;

// Steering method. 0 = rotate inside WAR's own SetCamera (WAR.exe+0x38C850) so the engine derives everything
// (view, billboard vectors, sky, skinned meshes, saved camera states) from the rotated camera.
// 1 = older method: patch SetTransform and vertex-shader constants c0..c3 after the fact.
static std::atomic<int> g_steerMode{0};
static bool g_cameraHookInstalled = false;
static void* g_setCameraTramp = nullptr;
static unsigned g_setCameraCalls = 0, g_setCameraRotated = 0, g_setCameraDerived = 0;
static unsigned g_camLogBudget = 40;
static bool g_vsActive = false, g_psActive = false;
static DWORD g_curFVF = 0;
static bool g_probesDone = false;

struct Stats {
    unsigned scenes, setTransform, viewsRotated, drawPerspFFP, drawPerspVS, drawOrthoFFP, drawOrthoVS,
             drawUP, vsConstCalls, rtChanges, clears,
             vpMatchT, vpMatchN, vpNoMatch, vpPatched;      // VS c0..c3 == transpose / straight / neither of V*P
};
static Stats g_stats{};
static std::map<uint64_t, unsigned> g_vscHist;   // (startReg<<32|count4f) -> calls

// who calls SetTransform (identifies WAR's camera code)
static std::map<uint64_t, unsigned> g_callerHist;   // (kind<<32 | return address) -> calls. kind 0=VIEW 1=PROJ persp 2=PROJ ortho
// the main world pass of the most recent frame (largest-detail perspective pass with a normal, non-mirrored view)
static bool g_frameBestValid = false;
static D3DMATRIX g_frameBestView{}, g_frameBestProj{};
static D3DMATRIX g_mainView{}, g_mainProj{};
static std::atomic<bool> g_mainValid{false};
static std::atomic<bool> g_scanRequest{false};

// capture
static std::atomic<bool> g_captureRequest{false};
static bool g_capturing = false;
static unsigned g_captureLines = 0;
static constexpr unsigned kCaptureMaxLines = 30000;

// VR / camera
static OpenVRHead g_vr;

// ---- VR display (phase 6a): alternate-eye rendering. Each game frame renders ONE eye; its image is read back,
// uploaded to our own D3D11 device and, once both eyes are fresh, submitted to the OpenVR compositor.
static VRDisplay g_vrDisplay;
static std::atomic<bool> g_vrWanted{false};
static bool g_vrActive = false;
static int g_vrEye = 0;                          // eye the CURRENT game frame renders (0 left, 1 right)
static OpenVRHead::Pose g_vrPose{};
static bool g_vrHavePose = false;
static bool g_vrHaveEye[2] = { false, false };
static float g_unitsPerMeter = 32.f;             // WAR world units per metre: calibrate with PageUp/PageDown
static UINT g_eyeSizeCfg = 0;                    // 0 = auto (back-buffer height)
static bool g_positional = true;
static float g_eyeFrustum[2][4];                 // per eye: x0 x1 y0 y1 in WAR (Gamebryo) convention
static float g_eyeToHead[2][3];                  // metres, OpenVR head space
static bool g_eyeParamsOk = false;
static vrm::V3 g_headDeltaCam = { 0, 0, 0 };     // head translation since recenter, camera frame, metres
static float g_refPos[3] = { 0, 0, 0 };
static IDirect3DSurface9* g_capRT = nullptr;     // GPU stretch target (eye size)
static IDirect3DSurface9* g_capSys = nullptr;    // system-memory readback
static std::vector<uint32_t> g_capBuf;
static unsigned g_vrPairs = 0, g_vrCapFail = 0, g_vrSubmitErr = 0, g_vrFramesInPeriod = 0;
static double g_vrCapMsSum = 0;
static double g_tStretch = 0, g_tGetRTD = 0, g_tLock = 0, g_tConvert = 0, g_tUpload = 0;   // capture stage times (ms, summed per period)
static unsigned g_tSamples = 0;
// auto-recenter: put the headset on after starting VR, or any SteamVR tracking reset, must not leave the view
// pointing away from where the game drew (the "purple screen" of round 6b).
static bool g_wornPrev = false;
static bool g_autoRecenterOnWorn = false;   // INI auto_recenter=1 re-enables recentering when the headset is put on
static bool g_autoRecenterPending = false;
static double g_wornAtMs = 0, g_stillSinceMs = -1, g_prevPoseMs = 0;
static vrm::Mat3 g_prevHeadRot = vrm::identity();
static bool g_havePrevPose = false;
static float g_prevPosePos[3] = {};
static float g_prevPoseYaw = 0.f;

// ---- simultaneous stereo (phase 6b): every world draw is issued twice, once into each half of the render target
// with that eye's view-projection; HUD (2D) draws are issued twice into a small rectangle per eye so the HUD floats
// at a chosen distance. INI: stereo=sbs|aer, hud=3d|fill, hud_distance, hud_width, hud_vertical.
static bool g_stereoSBS = true;
static int g_hudMode = 0;                        // 0 = floating 3D rectangle, 1 = fill the whole eye view
static float g_hudDistance = 1.5f, g_hudWidth = 2.4f, g_hudVert = 0.0f;   // metres
static float g_regs[256][4];                     // shadow of the vertex-shader float constants WAR uploaded
static float g_pregs[256][4];                    // shadow of the pixel-shader float constants (phase 7d)
static vrm::ScanKeys g_scanKeys = {};            // what the register scanners look for (left-eye matrices), and a version that changes with them
static unsigned g_scanKeyVer = 1;
static vrm::RegScan g_vsScan = []{ vrm::RegScan r; r.init(256); return r; }();     // incremental scanners over the shadow constant registers
static vrm::RegScan g_psScan = []{ vrm::RegScan r; r.init(224); return r; }();
static vrm::MatchCache g_vsCache, g_psCache;     // verified matches in the shadow registers, kept between draws (phase 7j)
// Bring both scanners and their match caches up to date. Always call refresh and cacheUpdate as a pair.
static inline void RefreshScans(bool withPS);
static vrm::EyeMats g_eyeL, g_eyeR;              // left / right eye View, Projection, VP and camera position for the current pass (phase 7d)
static unsigned g_patchStats[5] = {};            // VS patches applied per kind (VP, VIEW, PROJ, VP*bias, CAMPOS)
static unsigned g_patchStatsPS[5] = {};          // the same for pixel-shader constants
static unsigned g_extraPatchDraws = 0;           // right-eye draws that needed more than the plain VP replaced
static int g_hudAnchor = 2;                      // 2 = HUD is a flat panel fixed in the game's frame, drawn as a real 3D quad (INI hud_anchor=world);
                                                 // 1 = viewport billboard anchored in the game's frame (hud_anchor=billboard); 0 = locked to the headset (head)
static IDirect3DTexture9* g_hudTex = nullptr;    // the HUD is rendered into this once per frame (phase 7e) ...
static IDirect3DSurface9* g_hudSurf = nullptr;
static UINT g_hudTexW = 0, g_hudTexH = 0;
static bool g_hudBound = false;                  // ... and this is true while it is the render target instead of the game's main target
static bool g_hudClearedThisFrame = false, g_hudUsed = false;
static bool g_hudBoundFwd() { return g_hudBound; }
static void ApplyColorWriteMask(IDirect3DDevice9* s);   // defined with the render-state hook
static IDirect3DSurface9* g_hudSavedRT = nullptr; static IDirect3DSurface9* g_hudSavedDS = nullptr;
static DWORD g_hudSavedBlend[4] = {};            // SEPARATEALPHABLENDENABLE, SRCBLENDALPHA, DESTBLENDALPHA, BLENDOPALPHA of the game
// ---- planar water reflection (phase 7f; INI water=on|off) ----
static int g_waterMode = 1;
static IDirect3DSurface9* g_lastOffscreenRT = nullptr;   // the last non-main render target the game set (non-owning)
static IDirect3DSurface9* g_reflSurf = nullptr;          // the reflection map (render target) ...
static IDirect3DBaseTexture9* g_reflTex = nullptr;       // ... and the texture the water shaders sample it through (non-owning pointers)
static unsigned g_reflStageMask = 0;                     // texture stages that currently hold the reflection map
static bool g_reflHandled = false;                       // this frame's reflection camera was steered for VR
static float g_reflS = 1.f;                              // half width (tan) of the symmetric frustum the map was rendered with
static unsigned g_reflCams = 0, g_waterDraws = 0, g_reflTexSearches = 0;
static RECT g_gameScissor{}; static bool g_haveGameScissor = false;      // the game's last scissor rect, unscaled (game coordinates)
static RECT g_mainScissor{};                     // the last scissor rect passed on for the MAIN target (scaled)
static D3DVIEWPORT9 g_gameVP{};                  // the viewport the GAME asked for (in its own coordinates)
static D3DVIEWPORT9 g_devVP{};                   // the viewport actually set on the device (scaled when we redirect)

// ---- phase 7a: GPU hand-off, desktop skip, mouse cursor, VR-resolution render target
static bool g_gpuPathWanted = true;              // INI gpu_path=auto|off
static bool g_gpuActive = false;                 // the last frame went through the GPU path
static unsigned g_gpuFrames = 0, g_gpuFails = 0, g_gpuFailStreak = 0, g_vrFrameNo = 0, g_desktopSkipped = 0;
static const char* g_gpuWhy = "";
static unsigned g_desktopEvery = 0;              // INI desktop_every: present to the desktop every Nth frame, 0 = never while in VR
static bool g_cursorOn = true;                   // INI cursor=on|off, key M
static float g_cursorSize = 0.045f;              // arrow height as a fraction of the HUD height
static HWND g_gameHwnd = nullptr;
static int g_resMode = 2;                        // INI vr_resolution: 0 = desktop, 1 = steamvr (always), 2 = auto (steamvr when the GPU hand-off is available)
static double g_maxPixels = 0, g_renderScale = 1.0;   // 0 = no pixel budget: SteamVR's exact size, only clamped to what the D3D9 device allows
static unsigned g_gpuSubmitErrStreak = 0;        // consecutive Submit() errors while the GPU hand-off was in use
// DXVK hand-off (vkpath.h): used when DXVK's d3d9.dll is the D3D9 layer instead of dgVoodoo.
static bool g_vkPathWanted = true;               // INI vk_path=auto|off
static bool g_autoVr = false;                    // INI auto_vr=on: switch VR on once, at the first in-world frame (testing without hotkeys)
static std::atomic<bool> g_autoVrRequest{false}; // set by Present, run by the worker thread like a hotkey
static bool g_isDxvk = false;                    // the d3d9.dll the game uses is DXVK (decided in the Direct3DCreate9 hook)
static vkp::Handoff g_vk;
static bool g_vkActive = false;                  // the last frame went to SteamVR as a Vulkan image
static unsigned g_vkFrames = 0, g_vkFails = 0, g_vkSubmitErrStreak = 0;
static const char* g_vkWhy = "";
static bool g_redirect = false;                  // the game's main render target is replaced by our own VR-sized one
static IDirect3DSurface9 *g_vrRT = nullptr, *g_vrDS = nullptr, *g_bbSurf = nullptr, *g_autoDS = nullptr;
static UINT g_logW = 0, g_logH = 0;              // the size the GAME believes its screen is
static UINT g_wantRTW = 0, g_wantRTH = 0;        // VR render target size we want to (re)create
static bool g_haveGameVP = false;
static unsigned g_xfVersion = 0, g_rightVersion = ~0u;
static D3DMATRIX g_viewR{}, g_projR{};
static bool g_rightValid = false, g_projCheckLogged = false;
static unsigned g_frameMainPerspDraws = 0;
static void* g_curVS = nullptr;                  // vertex shader currently set by the game
static void* g_curPS = nullptr;                  // pixel shader currently set by the game
static UINT g_lastUpStart = 0, g_lastUpCount = 0; // the most recent constant upload
static std::map<void*, unsigned> g_unmatchedByVS; // shaders whose view-projection we could not find, per report period
static unsigned g_unmatchedLogged = 0;
static unsigned g_stereoCapLogged = 0, g_texLogged = 0;
static std::set<std::pair<void*, void*>> g_regDumped;     // (vs, ps) pairs whose constants were already dumped in this capture
static unsigned g_dupWorld = 0, g_dupWorldFFP = 0, g_dupUnmatched = 0, g_dupHud = 0, g_dupFill = 0, g_projMismatch = 0;
static std::atomic<bool> g_headOn{false};
static std::atomic<bool> g_pitchLock{false};     // hotkey Ctrl+Shift+Numpad-: the mouse no longer tilts the view (the headset still does)
static std::atomic<bool> g_perturb{false};
static std::atomic<bool> g_recenterRequest{false};
static bool g_haveRef = false;
static float g_refYaw = 0.f;
static vrm::Mat3 g_frameQ = vrm::identity();
static bool g_frameQValid = false;
static vrm::Mat3 g_perturbQ = vrm::identity();

static void BeepAsync(unsigned freq, unsigned ms);

// ---------------------------------------------------------------- logging
static void Log(const char* fmt, ...)
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    if (g_flushEachLine) fflush(g_log);
}

static void Cap(const char* fmt, ...)      // capture-frame line (capped)
{
    if (!g_capturing || g_captureLines >= kCaptureMaxLines) return;
    ++g_captureLines;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    Log("  %s\n", buf);
}

static std::string ModuleDir()
{
    char p[MAX_PATH]{};
    GetModuleFileNameA(g_self, p, MAX_PATH);
    char* s = strrchr(p, '\\');
    if (s) *s = 0;
    return p;
}

// "WAR.exe+0x1234", "d3d9.dll+0x5678" or "private memory" for an address.
static std::string Describe(const void* addr)
{
    HMODULE m = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(addr), &m) && m) {
        char path[MAX_PATH]{};
        GetModuleFileNameA(m, path, MAX_PATH);
        const char* base = strrchr(path, '\\');
        char out[MAX_PATH + 32];
        _snprintf_s(out, sizeof(out), _TRUNCATE, "%s+0x%lX", base ? base + 1 : path,
                    (unsigned long)(reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(m)));
        return out;
    }
    return "private memory (heap/stack/allocated)";
}

// ---------------------------------------------------------------- vtable patching
static void PatchVtable(void** table, size_t slot, void* replacement, void** original)
{
    DWORD oldProtect = 0;
    VirtualProtect(&table[slot], sizeof(void*), PAGE_READWRITE, &oldProtect);
    if (original && !*original) *original = table[slot];
    table[slot] = replacement;
    DWORD ignored = 0;
    VirtualProtect(&table[slot], sizeof(void*), oldProtect, &ignored);
}
#define PATCH(vt, slot, hook, orig) PatchVtable(vt, slot, reinterpret_cast<void*>(&hook), reinterpret_cast<void**>(&orig))

// ---------------------------------------------------------------- camera override
static const vrm::Mat3* ActiveRotation()
{
    if (!g_rtIsMain) return nullptr;
    if (g_perturb.load()) return &g_perturbQ;
    if (g_headOn.load() && g_frameQValid) return &g_frameQ;
    return nullptr;
}

// A VIEW arrives before the PROJECTION that tells us what kind of pass it is. We pass the original
// VIEW straight through, remember it, and once the pass type is known re-issue a rotated VIEW for
// perspective passes only. Shadow (ortho) and UI (ortho) passes are never touched.
static void ResolvePendingView(IDirect3DDevice9* dev, bool perspective)
{
    if (!g_havePendingView) return;
    g_havePendingView = false;
    if (!perspective) return;
    if (g_steerMode.load() != 1) return;                     // camera-hook mode: do not rotate twice
    const vrm::Mat3* q = ActiveRotation();
    if (!q) return;
    D3DMATRIX out;
    vrm::applyToView(g_pendingView.m, *q, out.m);
    g_origSetTransform(dev, D3DTS_VIEW, &out);
    ++g_stats.viewsRotated;
}

static HRESULT WINAPI HookSetTransform(IDirect3DDevice9* self, D3DTRANSFORMSTATETYPE state, const D3DMATRIX* matrix)
{
    ++g_stats.setTransform;
    const uintptr_t caller = reinterpret_cast<uintptr_t>(RETADDR());
    if (matrix && state == D3DTS_VIEW) {
        ++g_callerHist[(uint64_t(0) << 32) | caller];
        if (g_capturing) Cap("  caller=%s", Describe(reinterpret_cast<void*>(caller)).c_str());
        g_pendingView = *matrix;
        g_havePendingView = true;
        g_origView = *matrix; g_haveOrigView = true; ++g_xfVersion;
        if (g_capturing) Cap("SETXFORM VIEW  t-row=(%.1f %.1f %.1f)", matrix->m[3][0], matrix->m[3][1], matrix->m[3][2]);
        return g_origSetTransform(self, state, matrix);
    }
    if (matrix && state == D3DTS_PROJECTION) {
        bool persp = vrm::isPerspective(matrix->m);
        g_lastProjPersp = persp;
        g_origProj = *matrix; g_haveOrigProj = true; ++g_xfVersion;
        ++g_callerHist[(uint64_t(persp ? 1 : 2) << 32) | caller];
        if (g_capturing) Cap("  caller=%s", Describe(reinterpret_cast<void*>(caller)).c_str());
        if (persp && g_rtIsMain && g_havePendingView && vrm::det3(g_pendingView.m) < 0.f &&
            (std::fabs(g_pendingView.m[3][0]) + std::fabs(g_pendingView.m[3][1]) + std::fabs(g_pendingView.m[3][2])) > 1.f) {
            g_frameBestView = g_pendingView; g_frameBestProj = *matrix; g_frameBestValid = true;
        }
        if (g_capturing) {
            float zn, zf, fov, asp;
            if (vrm::decodePerspective(matrix->m, &zn, &zf, &fov, &asp))
                Cap("SETXFORM PROJ  perspective near=%.1f far=%.0f fovY=%.1f aspect=%.3f", zn, zf, fov, asp);
            else
                Cap("SETXFORM PROJ  ortho/other  m00=%.5g m11=%.5g m22=%.5g m33=%.3g", matrix->m[0][0], matrix->m[1][1], matrix->m[2][2], matrix->m[3][3]);
        }
        ResolvePendingView(self, persp);
        return g_origSetTransform(self, state, matrix);
    }
    if (g_capturing && state == D3DTS_WORLD) Cap("SETXFORM WORLD");
    return g_origSetTransform(self, state, matrix);
}

// ---------------------------------------------------------------- pass / shader tracking
static void OnDraw(IDirect3DDevice9* self, const char* kind, UINT prims)
{
    if (g_havePendingView) ResolvePendingView(self, g_lastProjPersp);
    if (g_lastProjPersp && g_rtIsMain) ++g_frameMainPerspDraws;
    if (g_lastProjPersp) { ++g_framePerspDraws; if (g_vsActive) ++g_stats.drawPerspVS; else ++g_stats.drawPerspFFP; }
    else                 { if (g_vsActive) ++g_stats.drawOrthoVS;  else ++g_stats.drawOrthoFFP; }
    if (g_capturing)
        Cap("DRAW %-11s prims=%u %s vs=%d ps=%d fvf=0x%lX | zenable=%lu zwrite=%lu zfunc=%lu blend=%lu src=%lu dst=%lu atest=%lu aref=%lu afunc=%lu cwrite=0x%lX cull=%lu stencil=%lu clipplane=0x%lX",
            kind, prims, g_lastProjPersp ? "PERSP" : "ortho", g_vsActive ? 1 : 0, g_psActive ? 1 : 0, (unsigned long)g_curFVF,
            (unsigned long)g_rsShadow[D3DRS_ZENABLE], (unsigned long)g_rsShadow[D3DRS_ZWRITEENABLE], (unsigned long)g_rsShadow[D3DRS_ZFUNC],
            (unsigned long)g_rsShadow[D3DRS_ALPHABLENDENABLE], (unsigned long)g_rsShadow[D3DRS_SRCBLEND], (unsigned long)g_rsShadow[D3DRS_DESTBLEND],
            (unsigned long)g_rsShadow[D3DRS_ALPHATESTENABLE], (unsigned long)g_rsShadow[D3DRS_ALPHAREF], (unsigned long)g_rsShadow[D3DRS_ALPHAFUNC],
            (unsigned long)g_rsShadow[D3DRS_COLORWRITEENABLE], (unsigned long)g_rsShadow[D3DRS_CULLMODE], (unsigned long)g_rsShadow[D3DRS_STENCILENABLE],
            (unsigned long)g_rsShadow[D3DRS_CLIPPLANEENABLE]);
}

// ---- lazy state restore (INI lazy_state=on, experimental) ---------------------------------------------------
// After the right-eye copy of a world draw the device still has the right eye's viewport and constant patches. Restoring
// them at once costs 1 + (number of patches) D3D calls per world draw, every one of which goes through the translation layer
// on the game thread. With lazy_state=on they are restored only when something actually needs the game's values: the next
// draw, a Clear, the frame's end. Patched registers the game uploads again in the meantime need no restore at all.
// The game's own values are always in the shadow registers (g_regs / g_pregs), so a restore is just a re-upload from there.
static bool g_lazyState = false;
static bool g_vpDirty = false;                   // device viewport is an eye viewport, not g_devVP
struct LazyPatch { int16_t reg, count; bool ps; };
static LazyPatch g_lazyPending[32];
static int g_lazyN = 0;
static unsigned g_lazySavedCalls = 0, g_lazyFlushedCalls = 0;
static void LazyCovered(bool ps, UINT start, UINT count)         // the game re-uploaded [start, start+count): those patches are gone
{
    for (int i = 0; i < g_lazyN; ) {
        const LazyPatch& p = g_lazyPending[i];
        if (p.ps == ps && (UINT)p.reg >= start && (UINT)(p.reg + p.count) <= start + count) { g_lazyPending[i] = g_lazyPending[--g_lazyN]; ++g_lazySavedCalls; }
        else ++i;
    }
}
static void LazyFlushConsts(IDirect3DDevice9* dev)
{
    for (int i = 0; i < g_lazyN; ++i) {
        const LazyPatch& p = g_lazyPending[i];
        if (p.ps) g_origSetPSConstF(dev, (UINT)p.reg, g_pregs[p.reg], (UINT)p.count);
        else      g_origSetVSConstF(dev, (UINT)p.reg, g_regs[p.reg], (UINT)p.count);
    }
    g_lazyFlushedCalls += (unsigned)g_lazyN;
    g_lazyN = 0;
}
static void LazyFlushVP(IDirect3DDevice9* dev) { if (g_vpDirty) { g_origSetViewport(dev, &g_devVP); g_vpDirty = false; ++g_lazyFlushedCalls; } }
static void LazyFlush(IDirect3DDevice9* dev) { LazyFlushConsts(dev); LazyFlushVP(dev); }
static void LazyForget() { g_lazyN = 0; g_vpDirty = false; }

// ---- stereo draw duplication -----------------------------------------------------------------------------
static void SetVP(IDirect3DDevice9* dev, int x, int y, int w, int h)
{
    D3DVIEWPORT9 vp = g_devVP;
    if (!(vp.MinZ >= 0.f && vp.MaxZ <= 1.f && vp.MinZ < vp.MaxZ)) { vp.MinZ = 0.f; vp.MaxZ = 1.f; }   // never inherit a corrupt depth range
    vp.X = (DWORD)x; vp.Y = (DWORD)y; vp.Width = (DWORD)w; vp.Height = (DWORD)h;
    g_origSetViewport(dev, &vp);
}

static void EyeRegion(int eye, int rtW, int rtH, int* x, int* y, int* w, int* h)
{
    if (g_stereoSBS) { *w = rtW / 2; *h = rtH; *x = eye ? rtW / 2 : 0; *y = 0; }
    else { *x = 0; *y = 0; *w = rtW; *h = rtH; }
}

// Right-eye view/projection from the game's (left-eye) ones. Valid only if our model of the engine's projection
// reproduces the engine's own left-eye projection exactly, otherwise this pass is left alone.
static void UpdateRightEye()
{
    if (g_rightVersion == g_xfVersion) return;
    g_rightVersion = g_xfVersion;
    g_rightValid = false;
    if (!g_haveOrigView || !g_haveOrigProj || !vrm::isPerspective(g_origProj.m)) return;
    float worst = 0.f;
    const bool ok = vrm::rightEyeMatrices(g_origView.m, g_origProj.m, g_eyeFrustum[0], g_eyeFrustum[1],
                                          vrm::eyeOffsetCamera(g_eyeToHead[0]), vrm::eyeOffsetCamera(g_eyeToHead[1]),
                                          g_unitsPerMeter, g_viewR.m, g_projR.m, &worst);
    if (!g_projCheckLogged) {
        g_projCheckLogged = true;
        Log("STEREO check: engine projection m00=%.5f m20=%.5f m11=%.5f m21=%.5f; worst relative difference to the left-eye model = %.3g -> %s\n",
            g_origProj.m[0][0], g_origProj.m[2][0], g_origProj.m[1][1], g_origProj.m[2][1], worst, ok ? "model matches, stereo enabled for this pass" : "MISMATCH, pass left alone");
    }
    if (!ok) { ++g_projMismatch; return; }
    g_rightValid = true;
    vrm::makeEyeMats(g_origView.m, g_origProj.m, &g_eyeL);
    vrm::makeEyeMats(g_viewR.m, g_projR.m, &g_eyeR);
    vrm::makeScanKeys(g_eyeL, &g_scanKeys); ++g_scanKeyVer;
}

// Where an eye's 2D content goes: the whole eye region, or (for HUD draws, hud=3d) a rectangle inside it that floats
// hud_distance metres ahead with the right parallax for this eye.
static void EyeViewportRect(int eye, int rtW, int rtH, bool hud, int* ox, int* oy, int* ow, int* oh)
{
    int x, y, w, h;
    EyeRegion(eye, rtW, rtH, &x, &y, &w, &h);
    if (hud && g_hudMode == 0) {
        const vrm::V3 eo = vrm::eyeOffsetCamera(g_eyeToHead[eye]);
        const float uiAspect = g_logH ? (float)g_logW / (float)g_logH : (g_bbH ? (float)g_bbW / (float)g_bbH : 2.37f);
        const vrm::PixelRect r = vrm::hudRectPixels(g_eyeFrustum[eye], eo.x, eo.y, g_hudDistance, g_hudWidth, uiAspect, g_hudVert, w, h);
        x += r.x; y += r.y; w = r.w; h = r.h;
    }
    *ox = x; *oy = y; *ow = w; *oh = h;
}

// ---- the HUD as a texture (hud_anchor=world) ---------------------------------------------------------------------
// All of the game's HUD draws of a frame are rendered ONCE into g_hudTex (the whole 2D UI at a resolution of our choosing). At the
// end of the frame it is drawn as a flat quad per eye, placed in space with that eye's real projection, so it behaves like a
// physical panel: it stays put, looks tilted from the side, and leaves the view naturally.
static void VRHudRelease()
{
    if (g_hudSavedRT) { g_hudSavedRT->Release(); g_hudSavedRT = nullptr; }
    if (g_hudSavedDS) { g_hudSavedDS->Release(); g_hudSavedDS = nullptr; }
    if (g_hudSurf) { g_hudSurf->Release(); g_hudSurf = nullptr; }
    if (g_hudTex) { g_hudTex->Release(); g_hudTex = nullptr; }
    g_hudBound = false; g_hudClearedThisFrame = false; g_hudUsed = false; g_hudTexW = g_hudTexH = 0;
}

static bool VRHudCreate(IDirect3DDevice9* dev)
{
    if (g_hudSurf) return true;
    const float aspect = g_logH ? (float)g_logW / (float)g_logH : 16.f / 9.f;
    UINT w = g_logW ? g_logW * 2u : 2048u;
    w = std::min(4096u, std::max(1536u, w)) & ~1u;
    UINT h = (UINT)((float)w / aspect + 0.5f) & ~1u;
    HRESULT hr = dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_hudTex, nullptr);
    if (SUCCEEDED(hr) && g_hudTex) hr = g_hudTex->GetSurfaceLevel(0, &g_hudSurf);
    if (FAILED(hr) || !g_hudSurf) {
        Log("HUD texture %ux%u could not be created (hr=0x%08lX): falling back to the billboard HUD\n", w, h, (unsigned long)hr);
        VRHudRelease(); g_hudAnchor = 1; return false;
    }
    g_hudTexW = w; g_hudTexH = h;
    Log("HUD texture %ux%u created (the UI is rendered into it once per frame and drawn as a panel in space)\n", w, h);
    return true;
}

static void VRHudRestoreStates(IDirect3DDevice9* dev)
{
    dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, g_hudSavedBlend[0]);
    dev->SetRenderState(D3DRS_SRCBLENDALPHA, g_hudSavedBlend[1]);
    dev->SetRenderState(D3DRS_DESTBLENDALPHA, g_hudSavedBlend[2]);
    dev->SetRenderState(D3DRS_BLENDOPALPHA, g_hudSavedBlend[3]);
}

static bool VRHudBind(IDirect3DDevice9* dev)
{
    if (g_hudBound) return true;
    if (!VRHudCreate(dev)) return false;
    dev->GetRenderTarget(0, &g_hudSavedRT);
    if (FAILED(dev->GetDepthStencilSurface(&g_hudSavedDS))) g_hudSavedDS = nullptr;
    g_origSetRenderTarget(dev, 0, g_hudSurf);
    if (g_origSetDS) g_origSetDS(dev, nullptr);                  // the HUD needs no depth, and a depth buffer smaller than the target is invalid
    dev->GetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, &g_hudSavedBlend[0]);
    dev->GetRenderState(D3DRS_SRCBLENDALPHA, &g_hudSavedBlend[1]);
    dev->GetRenderState(D3DRS_DESTBLENDALPHA, &g_hudSavedBlend[2]);
    dev->GetRenderState(D3DRS_BLENDOPALPHA, &g_hudSavedBlend[3]);
    // Colour is blended exactly as the game asks; alpha accumulates as coverage ("over"), so the finished HUD can be composited premultiplied.
    dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
    dev->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD);
    g_hudBound = true;
    g_origSetRenderState(dev, D3DRS_COLORWRITEENABLE, g_gameCW);                // the HUD texture keeps its alpha
    if (!g_hudClearedThisFrame) { g_origClear(dev, 0, nullptr, D3DCLEAR_TARGET, 0x00000000, 1.f, 0); g_hudClearedThisFrame = true; }
    return true;
}

// Put the game's main target (and depth buffer) back.
static void VRHudUnbind(IDirect3DDevice9* dev)
{
    if (!g_hudBound) return;
    VRHudRestoreStates(dev);
    if (g_hudSavedRT) g_origSetRenderTarget(dev, 0, g_hudSavedRT);
    if (g_origSetDS) g_origSetDS(dev, g_hudSavedDS);
    if (g_hudSavedRT) { g_hudSavedRT->Release(); g_hudSavedRT = nullptr; }
    if (g_hudSavedDS) { g_hudSavedDS->Release(); g_hudSavedDS = nullptr; }
    g_hudBound = false;
    ApplyColorWriteMask(dev);                                                    // back on the main target: alpha is protected again
    if (g_haveGameScissor) g_origSetScissor(dev, &g_mainScissor);
}

// The game itself switches render target while ours is bound: give its depth buffer back and forget ours.
static void VRHudGameTookOver(IDirect3DDevice9* dev)
{
    if (!g_hudBound) return;
    VRHudRestoreStates(dev);
    if (g_origSetDS) g_origSetDS(dev, g_hudSavedDS);
    if (g_hudSavedRT) { g_hudSavedRT->Release(); g_hudSavedRT = nullptr; }
    if (g_hudSavedDS) { g_hudSavedDS->Release(); g_hudSavedDS = nullptr; }
    g_hudBound = false;
}

static bool HudAsTexture() { return g_hudAnchor == 2 && g_hudMode == 0 && g_stereoSBS; }

// Keep only the patches whose registers the current shader can read (everything if it was not analysed).
static inline int FilterPatches(vrm::RegPatch* p, int n, const ss::ConstMask* mask)
{
    if (!mask) return n;
    int w = 0;
    for (int i = 0; i < n; ++i) if (mask->usesAny(p[i].reg, p[i].count)) p[w++] = p[i]; else ++g_patchesSkipped;
    return w;
}

static inline void RefreshScans(bool withPS)
{
    g_vsScan.refresh(g_regs, g_scanKeys, g_scanKeyVer);
    vrm::cacheUpdate(g_vsCache, g_regs, 256, g_vsScan, g_vsScan.lastLo, g_vsScan.lastHi, g_vsScan.lastAll, g_scanKeyVer, g_eyeL);
    if (withPS) {
        g_psScan.refresh(g_pregs, g_scanKeys, g_scanKeyVer);
        vrm::cacheUpdate(g_psCache, g_pregs, 224, g_psScan, g_psScan.lastLo, g_psScan.lastHi, g_psScan.lastAll, g_scanKeyVer, g_eyeL);
    }
}

// Where the HUD goes for one eye. Head-locked mode (hud_anchor=head): the old behaviour, always fully inside the eye region.
// World mode (default): the rectangle hangs in the game's facing direction, so it slides across the view when the head turns;
// it may then reach past the eye region, in which case `clip` is set and the caller must scissor to `clipRc` (absolute pixels).
// Returns false when nothing of it is visible in this eye (or it cannot be expressed as a viewport on this target).
static bool HudPlacement(int eye, int rtW, int rtH, int* ox, int* oy, int* ow, int* oh, bool* clip, RECT* clipRc)
{
    *clip = false;
    if (g_hudAnchor == 0 || g_hudMode != 0) { EyeViewportRect(eye, rtW, rtH, true, ox, oy, ow, oh); return true; }
    int rx, ry, rw, rh;
    EyeRegion(eye, rtW, rtH, &rx, &ry, &rw, &rh);
    const vrm::V3 eo = vrm::eyeOffsetCamera(g_eyeToHead[eye]);
    const float uiAspect = g_logH ? (float)g_logW / (float)g_logH : (g_bbH ? (float)g_bbW / (float)g_bbH : 2.37f);
    const vrm::Mat3 q = (g_headOn.load() && g_frameQValid) ? g_frameQ : vrm::identity();
    const vrm::V3 hp = (g_positional && g_headOn.load() && g_frameQValid) ? g_headDeltaCam : vrm::V3{ 0, 0, 0 };
    int r[4];
    if (!vrm::hudAnchoredRect(g_eyeFrustum[eye], eo, q, hp, g_hudDistance, g_hudWidth, uiAspect, g_hudVert, rw, rh, r)) return false;
    const int x0 = rx + r[0], y0 = ry + r[1], x1 = x0 + r[2], y1 = y0 + r[3];
    if (x1 <= rx || x0 >= rx + rw || y1 <= ry || y0 >= ry + rh) return false;                 // entirely outside this eye's view
    if (x0 < 0 || y0 < 0 || x1 > rtW || y1 > rtH) return false;                              // a viewport cannot leave the render target
    if (r[2] > 16384 || r[3] > 16384) return false;
    *ox = x0; *oy = y0; *ow = r[2]; *oh = r[3];
    if (x0 < rx || y0 < ry || x1 > rx + rw || y1 > ry + rh) { *clip = true; clipRc->left = rx; clipRc->top = ry; clipRc->right = rx + rw; clipRc->bottom = ry + rh; }
    return true;
}

// Capture only: dump the shader constants of the first draw of every (vertex shader, pixel shader) pair, so the camera-dependent
// ones can be identified offline.
static void DumpRegsOnce(void* vs, void* ps)
{
    if (!g_capturing || g_regDumped.size() >= 80 || !g_regDumped.insert({ vs, ps }).second) return;
    const unsigned nvs = std::min<unsigned>(g_lastUpStart + g_lastUpCount, 48u);
    Cap("REGDUMP vs=%p ps=%p  (vertex constants c0..c%u, then pixel constants c0..c11)", vs, ps, nvs ? nvs - 1 : 0);
    for (unsigned r = 0; r < nvs; ++r) Cap("   VS c%-3u [%11.5g %11.5g %11.5g %11.5g]", r, g_regs[r][0], g_regs[r][1], g_regs[r][2], g_regs[r][3]);
    for (unsigned r = 0; r < 12; ++r) Cap("   PS c%-3u [%11.5g %11.5g %11.5g %11.5g]", r, g_pregs[r][0], g_pregs[r][1], g_pregs[r][2], g_pregs[r][3]);
}

// 0 = leave alone, 1 = world (perspective) draw, 2 = HUD (2D after the world), 3 = 2D before any world draw
static int ClassifyDraw()
{
    if (!g_vrActive || !g_eyeParamsOk || !g_rtIsMain) return 0;
    if (g_lastProjPersp) return g_stereoSBS ? 1 : 0;
    return g_frameMainPerspDraws > 0 ? 2 : 3;
}

template <class F>
static HRESULT StereoDrawImpl(IDirect3DDevice9* dev, F&& issue)
{
    const int kind = ClassifyDraw();
    if (g_lazyN || g_vpDirty) {                                // lazy_state: the left-eye copy and every other draw need the game's constants;
        LazyFlushConsts(dev);                                  // a world draw sets its own viewport right away, everything else needs the game's
        if (kind != 1) LazyFlushVP(dev);
    }
    if (kind == 0) return issue();
    if (!g_haveGameVP) { if (FAILED(dev->GetViewport(&g_gameVP))) return issue(); g_haveGameVP = true; g_devVP = g_gameVP; }
    const int rtW = (int)(g_curRTw ? g_curRTw : g_bbW), rtH = (int)(g_curRTh ? g_curRTh : g_bbH);
    if (rtW < 64 || rtH < 64) return issue();
    int x, y, w, h;
    HRESULT hr;

    if (kind == 1 && g_hudBound) VRHudUnbind(dev);             // a world draw after the HUD (name tags etc.): back to the main target

    if ((kind == 2 || kind == 3) && HudAsTexture() && VRHudBind(dev)) {   // the whole HUD once, into the HUD texture. kind 3 (2D with no world drawn yet) is a loading/login screen: it hangs in the same panel
        SetVP(dev, 0, 0, (int)g_hudTexW, (int)g_hudTexH);
        RECT hs{};
        if (g_haveGameScissor && g_logW && g_logH) {           // the game's scissor rects are in its own pixels: scale them to the HUD texture
            long in[4] = { g_gameScissor.left, g_gameScissor.top, g_gameScissor.right, g_gameScissor.bottom }, out[4];
            vrm::scaleRect(in, (float)g_hudTexW / (float)g_logW, (float)g_hudTexH / (float)g_logH, out);
            hs = { out[0], out[1], out[2], out[3] };
            g_origSetScissor(dev, &hs);
        }
        hr = issue();
        g_origSetViewport(dev, &g_devVP);
        ++g_dupHud; g_hudUsed = true;
        return hr;
    }

    if (kind == 1) {                                           // world: left half, then right half with the right eye's matrices
        UpdateRightEye();

        // Water that samples the planar reflection map: both eyes use the SYMMETRIC projection the map was rendered with, so they look it up
        // identically. The viewport is widened (and scissored to the eye) so every vertex still lands exactly where the eye's own projection puts it.
        if (g_waterMode && g_reflHandled && g_reflStageMask && g_vsActive && g_rightValid) {
            float psym[4][4]; memcpy(psym, g_origProj.m, sizeof(psym));
            psym[0][0] = 1.f / g_reflS; psym[2][0] = 0.f;
            vrm::EyeMats tg[2];
            vrm::makeEyeMats(g_origView.m, psym, &tg[0]); vrm::makeEyeMats(g_viewR.m, psym, &tg[1]);
            int vpr[2][4], reg[2][4]; bool ok = true;
            for (int e = 0; e < 2; ++e) {
                EyeRegion(e, rtW, rtH, &reg[e][0], &reg[e][1], &reg[e][2], &reg[e][3]);
                vrm::symmetricViewport(g_eyeFrustum[e], g_reflS, reg[e][0], reg[e][1], reg[e][2], reg[e][3], vpr[e]);
                if (vpr[e][0] < 0 || vpr[e][0] + vpr[e][2] > rtW || vpr[e][2] > 16384) ok = false;
            }
            vrm::RegPatch pv[2][16], pp[2][16]; int nv[2] = {}, np[2] = {};
            const int64_t tScanW = g_log ? PerfTick() : 0;
            RefreshScans(g_psActive);
            if (ok) for (int e = 0; e < 2; ++e) {
                nv[e] = vrm::buildEyePatchesCached(g_regs, 256, g_eyeL, tg[e], g_vsCache, pv[e], 16);
                bool haveVP = false; for (int i = 0; i < nv[e]; ++i) if (pv[e][i].kind == vrm::PK_VP) haveVP = true;
                if (!haveVP) ok = false;
                nv[e] = FilterPatches(pv[e], nv[e], g_curVSMask);
                np[e] = g_psActive ? vrm::buildEyePatchesCached(g_pregs, 224, g_eyeL, tg[e], g_psCache, pp[e], 16) : 0;
                np[e] = FilterPatches(pp[e], np[e], g_curPSMask);
            }
            if (g_log) g_pf[PF_SCAN] += PerfTick() - tScanW;
            if (ok) {
                DWORD sOn = 0; RECT sRc{}; dev->GetRenderState(D3DRS_SCISSORTESTENABLE, &sOn); dev->GetScissorRect(&sRc);
                for (int e = 0; e < 2; ++e) {
                    float savedV[16][16], savedP[16][16];
                    for (int i = 0; i < nv[e]; ++i) { memcpy(savedV[i], g_regs[pv[e][i].reg], pv[e][i].count * 4 * sizeof(float)); g_origSetVSConstF(dev, (UINT)pv[e][i].reg, pv[e][i].v, (UINT)pv[e][i].count); }
                    for (int i = 0; i < np[e]; ++i) { memcpy(savedP[i], g_pregs[pp[e][i].reg], pp[e][i].count * 4 * sizeof(float)); g_origSetPSConstF(dev, (UINT)pp[e][i].reg, pp[e][i].v, (UINT)pp[e][i].count); }
                    RECT rc = { reg[e][0], reg[e][1], reg[e][0] + reg[e][2], reg[e][1] + reg[e][3] };
                    g_origSetScissor(dev, &rc); dev->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
                    SetVP(dev, vpr[e][0], vpr[e][1], vpr[e][2], vpr[e][3]);
                    hr = issue();
                    for (int i = np[e] - 1; i >= 0; --i) g_origSetPSConstF(dev, (UINT)pp[e][i].reg, savedP[i], (UINT)pp[e][i].count);
                    for (int i = nv[e] - 1; i >= 0; --i) g_origSetVSConstF(dev, (UINT)pv[e][i].reg, savedV[i], (UINT)pv[e][i].count);
                }
                dev->SetRenderState(D3DRS_SCISSORTESTENABLE, sOn); g_origSetScissor(dev, &sRc);
                g_origSetViewport(dev, &g_devVP);
                ++g_waterDraws; ++g_dupWorld;
                if (g_capturing && g_stereoCapLogged < 3000) { ++g_stereoCapLogged; Cap("WATER draw: vs=%p ps=%p symmetric projection S=%.4f, viewports x=%d w=%d / x=%d w=%d", g_curVS, g_curPS, g_reflS, vpr[0][0], vpr[0][2], vpr[1][0], vpr[1][2]); }
                return hr;
            }
        }

        EyeRegion(0, rtW, rtH, &x, &y, &w, &h); SetVP(dev, x, y, w, h);
        hr = issue();
        if (g_rightValid) {
            if (g_vsActive) {
                // Everything in the shader constants that depends on the camera is replaced by its right-eye value for this one draw:
                // the VP block (every copy), View, Projection, VP*bias (projective-texture / reflection matrices) and the camera position,
                // in both the vertex-shader and the pixel-shader constants.
                vrm::RegPatch pv[16], pp[16];
                const int64_t tScan = g_log ? PerfTick() : 0;
                RefreshScans(g_psActive);
                int nv = vrm::buildEyePatchesCached(g_regs, 256, g_eyeL, g_eyeR, g_vsCache, pv, 16);
                bool haveVP = false;
                for (int i = 0; i < nv; ++i) if (pv[i].kind == vrm::PK_VP) haveVP = true;
                if (haveVP) {
                    nv = FilterPatches(pv, nv, g_curVSMask);
                    int npx = g_psActive ? vrm::buildEyePatchesCached(g_pregs, 224, g_eyeL, g_eyeR, g_psCache, pp, 16) : 0;
                    npx = FilterPatches(pp, npx, g_curPSMask);
                    if (g_log) g_pf[PF_SCAN] += PerfTick() - tScan;
                    float savedV[16][16], savedP[16][16];
                    for (int i = 0; i < nv; ++i) {
                        memcpy(savedV[i], g_regs[pv[i].reg], pv[i].count * 4 * sizeof(float));
                        g_origSetVSConstF(dev, (UINT)pv[i].reg, pv[i].v, (UINT)pv[i].count);
                        ++g_patchStats[pv[i].kind];
                    }
                    for (int i = 0; i < npx; ++i) {
                        memcpy(savedP[i], g_pregs[pp[i].reg], pp[i].count * 4 * sizeof(float));
                        g_origSetPSConstF(dev, (UINT)pp[i].reg, pp[i].v, (UINT)pp[i].count);
                        ++g_patchStatsPS[pp[i].kind];
                    }
                    if (nv > 1 || npx > 0) ++g_extraPatchDraws;
                    EyeRegion(1, rtW, rtH, &x, &y, &w, &h); SetVP(dev, x, y, w, h);
                    issue();
                    if (g_lazyState && g_lazyN + nv + npx <= (int)(sizeof(g_lazyPending) / sizeof(g_lazyPending[0]))) {
                        // leave the right eye's values in place; restored from the shadow registers when needed (see LazyFlush)
                        for (int i = 0; i < npx; ++i) g_lazyPending[g_lazyN++] = { (int16_t)pp[i].reg, (int16_t)pp[i].count, true };
                        for (int i = 0; i < nv; ++i)  g_lazyPending[g_lazyN++] = { (int16_t)pv[i].reg, (int16_t)pv[i].count, false };
                    } else {
                        for (int i = npx - 1; i >= 0; --i) g_origSetPSConstF(dev, (UINT)pp[i].reg, savedP[i], (UINT)pp[i].count);
                        for (int i = nv - 1; i >= 0; --i) g_origSetVSConstF(dev, (UINT)pv[i].reg, savedV[i], (UINT)pv[i].count);
                    }
                    ++g_dupWorld;
                    if (g_capturing && g_stereoCapLogged < 3000) {
                        ++g_stereoCapLogged;
                        char buf[256]; int n = 0; buf[0] = 0;
                        for (int i = 0; i < nv && n < 200; ++i) n += _snprintf_s(buf + n, sizeof(buf) - n, _TRUNCATE, " %s@c%d", vrm::patchKindName(pv[i].kind), pv[i].reg);
                        char bufp[160]; int m = 0; bufp[0] = 0;
                        for (int i = 0; i < npx && m < 120; ++i) m += _snprintf_s(bufp + m, sizeof(bufp) - m, _TRUNCATE, " %s@c%d", vrm::patchKindName(pp[i].kind), pp[i].reg);
                        Cap("STEREO dup: vs=%p ps=%p VS patches:%s | PS patches:%s", g_curVS, g_curPS, buf, m ? bufp : " none");
                    }
                    DumpRegsOnce(g_curVS, g_curPS);
                } else {
                    if (g_log) g_pf[PF_SCAN] += PerfTick() - tScan;
                    ++g_dupUnmatched; ++g_unmatchedByVS[g_curVS];
                    if (g_unmatchedLogged < 14) {
                        ++g_unmatchedLogged;
                        Log("STEREO unmatched draw #%u: vs=%p last upload c%u x%u. VP (transposed) we looked for, row0=[%.4g %.4g %.4g %.4g]; registers c0..c7:\n",
                            g_unmatchedLogged, g_curVS, g_lastUpStart, g_lastUpCount, g_eyeL.VP[0][0], g_eyeL.VP[1][0], g_eyeL.VP[2][0], g_eyeL.VP[3][0]);
                        for (int r = 0; r < 8; ++r) Log("      c%d = [%.5g %.5g %.5g %.5g]\n", r, g_regs[r][0], g_regs[r][1], g_regs[r][2], g_regs[r][3]);
                    }
                }
            } else {                                           // fixed-function draw (name tags etc.)
                g_origSetTransform(dev, D3DTS_VIEW, &g_viewR);
                g_origSetTransform(dev, D3DTS_PROJECTION, &g_projR);
                EyeRegion(1, rtW, rtH, &x, &y, &w, &h); SetVP(dev, x, y, w, h);
                issue();
                g_origSetTransform(dev, D3DTS_VIEW, &g_origView);
                g_origSetTransform(dev, D3DTS_PROJECTION, &g_origProj);
                ++g_dupWorldFFP;
            }
        } else ++g_dupUnmatched;
        if (g_lazyState) g_vpDirty = true;                     // restored by the next draw that needs it
        else g_origSetViewport(dev, &g_devVP);
        return hr;
    }

    // 2D draws
    if (!g_stereoSBS && (kind == 3 || g_hudMode == 1)) return issue();    // AER: nothing to rearrange
    hr = S_OK;
    const int eyes = g_stereoSBS ? 2 : 1;
    DWORD savedScissorOn = 0; RECT savedScissor{}; bool haveSavedScissor = false;
    for (int e = 0; e < eyes; ++e) {
        const int eye = g_stereoSBS ? e : g_vrEye;
        bool clip = false; RECT clipRc{};
        if (kind == 2) { if (!HudPlacement(eye, rtW, rtH, &x, &y, &w, &h, &clip, &clipRc)) continue; }   // HUD not in this eye's view
        else EyeViewportRect(eye, rtW, rtH, false, &x, &y, &w, &h);
        SetVP(dev, x, y, w, h);
        if (clip) {                                                      // the HUD reaches past this eye's region: keep it out of the other eye
            if (!haveSavedScissor) { dev->GetRenderState(D3DRS_SCISSORTESTENABLE, &savedScissorOn); dev->GetScissorRect(&savedScissor); haveSavedScissor = true; }
            g_origSetScissor(dev, &clipRc);
            dev->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
        }
        hr = issue();
        if (clip) { dev->SetRenderState(D3DRS_SCISSORTESTENABLE, savedScissorOn); g_origSetScissor(dev, &savedScissor); }
    }
    g_origSetViewport(dev, &g_devVP);
    if (kind == 2) ++g_dupHud; else ++g_dupFill;
    return hr;
}

// Draw hooks call this. With the log closed it is the plain function; with log=on it times the whole duplicated draw and the actual D3D calls.
template <class F>
static HRESULT StereoDraw(IDirect3DDevice9* dev, F&& issue)
{
    if (!g_log) return StereoDrawImpl(dev, issue);
    auto timed = [&] { const int64_t t = PerfTick(); const HRESULT h = issue(); g_pf[PF_ISSUE] += PerfTick() - t; return h; };
    const int64_t t0 = PerfTick();
    const HRESULT h = StereoDrawImpl(dev, timed);
    g_pf[PF_STEREO] += PerfTick() - t0;
    return h;
}

static HRESULT WINAPI HookDrawPrimitive(IDirect3DDevice9* s, D3DPRIMITIVETYPE t, UINT a, UINT n)
{ OnDraw(s, "Prim", n); return StereoDraw(s, [&] { return g_origDrawPrimitive(s, t, a, n); }); }

static HRESULT WINAPI HookDrawIndexedPrimitive(IDirect3DDevice9* s, D3DPRIMITIVETYPE t, INT b, UINT mi, UINT nv, UINT si, UINT n)
{ OnDraw(s, "IndexedPrim", n); return StereoDraw(s, [&] { return g_origDrawIndexedPrimitive(s, t, b, mi, nv, si, n); }); }

static HRESULT WINAPI HookDrawPrimitiveUP(IDirect3DDevice9* s, D3DPRIMITIVETYPE t, UINT n, const void* d, UINT st)
{ ++g_stats.drawUP; OnDraw(s, "PrimUP", n); return StereoDraw(s, [&] { return g_origDrawPrimitiveUP(s, t, n, d, st); }); }

static HRESULT WINAPI HookDrawIndexedPrimitiveUP(IDirect3DDevice9* s, D3DPRIMITIVETYPE t, UINT mi, UINT nv, UINT n, const void* id, D3DFORMAT f, const void* vd, UINT st)
{ ++g_stats.drawUP; OnDraw(s, "IdxPrimUP", n); return StereoDraw(s, [&] { return g_origDrawIndexedPrimitiveUP(s, t, mi, nv, n, id, f, vd, st); }); }

static HRESULT WINAPI HookSetFVF(IDirect3DDevice9* s, DWORD fvf)
{ g_curFVF = fvf; return g_origSetFVF(s, fvf); }

static HRESULT WINAPI HookCreateVertexShader(IDirect3DDevice9* s, const DWORD* fn, IDirect3DVertexShader9** pp)
{
    const HRESULT hr = g_origCreateVS(s, fn, pp);
    if (SUCCEEDED(hr) && pp && *pp && fn) {
        ss::ConstMask m; const bool ok = ss::analyse(reinterpret_cast<const uint32_t*>(fn), 1u << 16, &m);
        AcquireSRWLockExclusive(&g_maskLock); g_vsMasks[(void*)*pp] = m; ReleaseSRWLockExclusive(&g_maskLock);
        ++g_shaderAnalysed[0]; if (!ok) ++g_shaderRefused[0];
    }
    return hr;
}
static HRESULT WINAPI HookCreatePixelShader(IDirect3DDevice9* s, const DWORD* fn, IDirect3DPixelShader9** pp)
{
    const HRESULT hr = g_origCreatePS(s, fn, pp);
    if (SUCCEEDED(hr) && pp && *pp && fn) {
        ss::ConstMask m; const bool ok = ss::analyse(reinterpret_cast<const uint32_t*>(fn), 1u << 16, &m);
        AcquireSRWLockExclusive(&g_maskLock); g_psMasks[(void*)*pp] = m; ReleaseSRWLockExclusive(&g_maskLock);
        ++g_shaderAnalysed[1]; if (!ok) ++g_shaderRefused[1];
    }
    return hr;
}
static const ss::ConstMask* LookupMask(std::unordered_map<void*, ss::ConstMask>& map, void* key)
{
    if (!key) return nullptr;
    AcquireSRWLockShared(&g_maskLock);
    auto it = map.find(key);
    const ss::ConstMask* r = (it != map.end() && it->second.valid) ? &it->second : nullptr;      // node addresses are stable
    ReleaseSRWLockShared(&g_maskLock);
    return r;
}

static HRESULT WINAPI HookSetVertexShader(IDirect3DDevice9* s, IDirect3DVertexShader9* sh)
{
    g_vsActive = (sh != nullptr);
    g_curVS = (void*)sh;
    g_curVSMask = LookupMask(g_vsMasks, (void*)sh);
    Cap("SET_VS %p", (void*)sh);
    return g_origSetVertexShader(s, sh);
}

static HRESULT WINAPI HookSetPixelShader(IDirect3DDevice9* s, IDirect3DPixelShader9* sh)
{
    g_psActive = (sh != nullptr);
    g_curPS = (void*)sh;
    g_curPSMask = LookupMask(g_psMasks, (void*)sh);
    Cap("SET_PS %p", (void*)sh);
    return g_origSetPixelShader(s, sh);
}

// WAR uploads a block starting at c0 before every draw. For world passes c0..c3 hold the combined
// View*Projection (verified against your log: c0 == first column of V*P). Four registers, each one
// column of V*P  ==  transpose(V*P) when read as rows. Layout is re-verified on every call.
static HRESULT WINAPI HookSetVSConstF(IDirect3DDevice9* s, UINT start, const float* data, UINT count)
{
    ++g_stats.vsConstCalls;
    if (g_log) ++g_vscHist[(uint64_t(start) << 32) | count];

    const float* send = data;
    float patched[256 * 4];
    const char* tag = "";
    // Classifying the uploaded VP block is only needed for the log and for the old "constants" steering mode.
    if ((g_log || g_steerMode.load() == 1) && start == 0 && count >= 4 && count <= 256 && data && g_haveOrigView && g_haveOrigProj && g_lastProjPersp) {
        D3DMATRIX vp; vrm::mul4(g_origView.m, g_origProj.m, vp.m);
        const int layout = vrm::detectVPLayout(vp.m, data);
        if (layout == 1) { ++g_stats.vpMatchT; tag = " VP=transposed"; }
        else if (layout == 2) { ++g_stats.vpMatchN; tag = " VP=straight"; }
        else { ++g_stats.vpNoMatch; tag = " VP=no-match"; }
        const vrm::Mat3* q = (layout && g_steerMode.load() == 1) ? ActiveRotation() : nullptr;
        if (q) {
            D3DMATRIX v2, vp2;
            vrm::applyToView(g_origView.m, *q, v2.m);
            vrm::mul4(v2.m, g_origProj.m, vp2.m);
            memcpy(patched, data, count * 4 * sizeof(float));
            vrm::writeVPRegs(vp2.m, layout, patched);
            send = patched;
            ++g_stats.vpPatched;
            tag = layout == 1 ? " VP=transposed PATCHED" : " VP=straight PATCHED";
        }
    }
    if (g_capturing && data && count)
        Cap("VS_CONST start=c%u count=%u  first=[%.5g %.5g %.5g %.5g]%s", start, count, data[0], data[1], data[2], data[3], tag);
    if (send && count && start + count <= 256) { memcpy(g_regs[start], send, count * 4 * sizeof(float)); g_vsScan.touch((int)start, (int)count); }
    if (g_lazyN) LazyCovered(false, start, count);
    g_lastUpStart = start; g_lastUpCount = count;
    PF_SAMPLE_BEGIN(pfs);
    const HRESULT pfr = g_origSetVSConstF(s, start, send, count);
    PF_SAMPLE_END(pfs);
    return pfr;
}

// Pixel-shader constants are mirrored the same way, so camera-dependent ones can be replaced for the right eye too.
static HRESULT WINAPI HookSetPSConstF(IDirect3DDevice9* s, UINT start, const float* data, UINT count)
{
    if (data && count && start + count <= 256) { memcpy(g_pregs[start], data, count * 4 * sizeof(float)); g_psScan.touch((int)start, (int)count); }
    if (g_lazyN) LazyCovered(true, start, count);
    if (g_capturing && data && count) Cap("PS_CONST start=c%u count=%u  first=[%.5g %.5g %.5g %.5g]", start, count, data[0], data[1], data[2], data[3]);
    PF_SAMPLE_BEGIN(pfs);
    const HRESULT pfr = g_origSetPSConstF(s, start, data, count);
    PF_SAMPLE_END(pfs);
    return pfr;
}

// Capture-only diagnostics: which textures a draw samples (a screen-sized render target here means a screen-space effect),
// and in-frame copies of the scene.
// The game's real back buffer is X8R8G8B8: it has no alpha channel, so reading destination alpha (the pistol's particles blend with
// SRCALPHA / DESTALPHA) always gives 1. Our VR target must be A8R8G8B8 (the format dgVoodoo maps well), whose alpha channel earlier
// alpha-blended draws would fill with junk. To behave like the X8 buffer, nothing may write its alpha (here: the colour-write mask loses the
// alpha bit while the game draws into it) and clears set it to 1 (HookClear).
static bool g_hudBoundFwd();    // defined with the HUD code
static inline bool MainAlphaMasked() { return g_redirect && g_rtIsMain && !g_hudBoundFwd(); }
static HRESULT WINAPI HookSetRenderState(IDirect3DDevice9* s, D3DRENDERSTATETYPE st, DWORD v)
{
    if ((unsigned)st < 256) g_rsShadow[st] = v;
    if (st == D3DRS_COLORWRITEENABLE && !g_ownRS) { g_gameCW = v; if (MainAlphaMasked()) v &= 0x7; }
    PF_SAMPLE_BEGIN(pfs);
    const HRESULT pfr = g_origSetRenderState(s, st, v);
    PF_SAMPLE_END(pfs);
    return pfr;
}
// Re-establish the right mask after the render target changed.
static void ApplyColorWriteMask(IDirect3DDevice9* s)
{
    g_origSetRenderState(s, D3DRS_COLORWRITEENABLE, MainAlphaMasked() ? (g_gameCW & 0x7) : g_gameCW);
}

static HRESULT WINAPI HookSetTexture(IDirect3DDevice9* s, DWORD stage, IDirect3DBaseTexture9* tex)
{
    if (stage < 16 && g_reflSurf) {                          // which stages hold the water reflection map?
        bool isRefl = false;
        if (tex) {
            if (tex == g_reflTex) isRefl = true;
            else if (!g_reflTex && g_reflTexSearches < 4000 && tex->GetType() == D3DRTYPE_TEXTURE) {
                ++g_reflTexSearches;
                IDirect3DSurface9* lv = nullptr;
                if (SUCCEEDED(static_cast<IDirect3DTexture9*>(tex)->GetSurfaceLevel(0, &lv)) && lv) {
                    if (lv == g_reflSurf) { g_reflTex = tex; isRefl = true; }
                    lv->Release();
                }
            }
        }
        if (isRefl) g_reflStageMask |= 1u << stage; else g_reflStageMask &= ~(1u << stage);
    }
    if (g_capturing && g_texLogged < 6000) {
        ++g_texLogged;
        if (!tex) Cap("SET_TEX s%lu = none", (unsigned long)stage);
        else if (tex->GetType() == D3DRTYPE_TEXTURE) {
            D3DSURFACE_DESC d{};
            if (SUCCEEDED(static_cast<IDirect3DTexture9*>(tex)->GetLevelDesc(0, &d)))
                Cap("SET_TEX s%lu = %p  %ux%u fmt=%d%s", (unsigned long)stage, (void*)tex, d.Width, d.Height, (int)d.Format, (d.Usage & D3DUSAGE_RENDERTARGET) ? "  RENDER-TARGET" : "");
        } else Cap("SET_TEX s%lu = %p type=%d", (unsigned long)stage, (void*)tex, (int)tex->GetType());
    }
    PF_SAMPLE_BEGIN(pfs);
    const HRESULT pfr = g_origSetTexture(s, stage, tex);
    PF_SAMPLE_END(pfs);
    return pfr;
}

static HRESULT WINAPI HookStretchRect(IDirect3DDevice9* s, IDirect3DSurface9* src, const RECT* sr, IDirect3DSurface9* dst, const RECT* dr, D3DTEXTUREFILTERTYPE f)
{
    if (g_capturing) {
        D3DSURFACE_DESC a{}, b{};
        if (src) src->GetDesc(&a);
        if (dst) dst->GetDesc(&b);
        Cap("STRETCHRECT %ux%u (%ld,%ld-%ld,%ld) -> %ux%u (%ld,%ld-%ld,%ld)", a.Width, a.Height, sr ? sr->left : 0, sr ? sr->top : 0, sr ? sr->right : 0, sr ? sr->bottom : 0,
            b.Width, b.Height, dr ? dr->left : 0, dr ? dr->top : 0, dr ? dr->right : 0, dr ? dr->bottom : 0);
    }
    return g_origStretchRect(s, src, sr, dst, dr, f);
}

static HRESULT WINAPI HookSetRenderTarget(IDirect3DDevice9* s, DWORD idx, IDirect3DSurface9* rt)
{
    ++g_stats.rtChanges;
    IDirect3DSurface9* use = rt;
    if (g_redirect && idx == 0 && rt && rt == g_bbSurf) use = g_vrRT;       // the game's "back buffer" is really our VR target
    if (idx == 0 && use) {
        D3DSURFACE_DESC dd{};
        if (SUCCEEDED(use->GetDesc(&dd))) {
            if (!g_bbW) { g_bbW = dd.Width; g_bbH = dd.Height; }
            g_curRTw = dd.Width; g_curRTh = dd.Height;
            g_rtIsMain = (unsigned long long)dd.Width * dd.Height * 2ull >= (unsigned long long)g_bbW * g_bbH;
        }
    }
    if (g_capturing) {
        D3DSURFACE_DESC d{};
        if (use && SUCCEEDED(use->GetDesc(&d)))
            Cap("SET_RT idx=%lu surf=%p%s %ux%u fmt=%d msaa=%d usage=0x%lX", (unsigned long)idx, (void*)use, use != rt ? " (redirected)" : "", d.Width, d.Height, (int)d.Format, (int)d.MultiSampleType, (unsigned long)d.Usage);
        else
            Cap("SET_RT idx=%lu surf=%p", (unsigned long)idx, (void*)rt);
    }
    if (idx == 0 && g_hudBound) VRHudGameTookOver(s);
    if (idx == 0 && use && !g_rtIsMain) g_lastOffscreenRT = use;
    if (idx == 0) g_vpDirty = false;                                         // D3D9 resets the viewport to the whole new target anyway
    HRESULT hr = g_origSetRenderTarget(s, idx, use);
    if (g_redirect && idx == 0 && use == g_vrRT && g_origSetDS) g_origSetDS(s, g_vrDS);   // our (large) depth buffer goes with it
    if (idx == 0 && g_redirect) ApplyColorWriteMask(s);
    return hr;
}

// The game also (re)sets its depth buffer; ours must stay in place while the redirect is active.
static HRESULT WINAPI HookSetDepthStencil(IDirect3DDevice9* s, IDirect3DSurface9* ds)
{
    if (g_redirect && ds && ds == g_autoDS) ds = g_vrDS;
    return g_origSetDS(s, ds);
}

// Everything the game does in its own screen coordinates is scaled to the real (VR-sized) target.
static HRESULT WINAPI HookSetScissorRect(IDirect3DDevice9* s, const RECT* r)
{
    if (r) { g_gameScissor = *r; g_haveGameScissor = true; }
    if (g_redirect && g_rtIsMain && r && g_logW && g_logH) {
        long in[4] = { r->left, r->top, r->right, r->bottom }, out[4];
        vrm::scaleRect(in, (float)g_bbW / (float)g_logW, (float)g_bbH / (float)g_logH, out);
        RECT sc = { out[0], out[1], out[2], out[3] };
        g_mainScissor = sc;
        return g_origSetScissor(s, &sc);
    }
    if (r) g_mainScissor = *r;
    return g_origSetScissor(s, r);
}

static HRESULT WINAPI HookSetViewport(IDirect3DDevice9* s, const D3DVIEWPORT9* vp)
{
    if (vp) { g_gameVP = *vp; g_haveGameVP = true; }
    if (g_capturing && vp) Cap("SET_VIEWPORT x=%lu y=%lu w=%lu h=%lu z=%.3f..%.3f", (unsigned long)vp->X, (unsigned long)vp->Y, (unsigned long)vp->Width, (unsigned long)vp->Height, vp->MinZ, vp->MaxZ);
    D3DVIEWPORT9 scaled;
    const D3DVIEWPORT9* use = vp;
    if (g_redirect && g_rtIsMain && vp && g_logW && g_logH) {
        const double sx = (double)g_bbW / (double)g_logW, sy = (double)g_bbH / (double)g_logH;
        scaled = *vp;
        scaled.X = (DWORD)std::lround(vp->X * sx); scaled.Y = (DWORD)std::lround(vp->Y * sy);
        scaled.Width = (DWORD)std::lround(vp->Width * sx); scaled.Height = (DWORD)std::lround(vp->Height * sy);
        use = &scaled;
    }
    if (use) g_devVP = *use;
    g_vpDirty = false;
    return g_origSetViewport(s, use);
}

static HRESULT WINAPI HookClear(IDirect3DDevice9* s, DWORD c, const D3DRECT* r, DWORD f, D3DCOLOR col, float z, DWORD st)
{
    ++g_stats.clears;
    LazyFlushVP(s);                                                            // Clear without rects clears the viewport
    if (g_redirect && g_rtIsMain && (f & D3DCLEAR_TARGET)) col |= 0xFF000000u;       // X8 semantics: alpha is always 1
    Cap("CLEAR rects=%lu flags=0x%lX color=0x%08lX z=%.3f", (unsigned long)c, (unsigned long)f, (unsigned long)col, z);
    if (g_redirect && g_rtIsMain && c && r && c <= 16 && g_logW && g_logH) {
        D3DRECT sc[16];
        const float sx = (float)g_bbW / (float)g_logW, sy = (float)g_bbH / (float)g_logH;
        for (DWORD i = 0; i < c; ++i) {
            long in[4] = { r[i].x1, r[i].y1, r[i].x2, r[i].y2 }, out[4];
            vrm::scaleRect(in, sx, sy, out);
            sc[i].x1 = out[0]; sc[i].y1 = out[1]; sc[i].x2 = out[2]; sc[i].y2 = out[3];
        }
        return g_origClear(s, c, sc, f, col, z, st);
    }
    return g_origClear(s, c, r, f, col, z, st);
}

// ---------------------------------------------------------------- engine camera hook
// WAR.exe+0x38C850 is RenderContext::SetCamera (a virtual method, thiscall, 6 stack args, ret 0x18):
//     SetCamera(this, pos, forward, up, right, frustum, flags)
// It builds the view matrix from the four vectors, builds the projection from `frustum`
// ({x0, x1, y0, y1, near, far} as tangents at unit distance, byte flag at +0x18), and calls the D3D
// SetTransform for both. Everything else (billboard vectors at +0x820, saved camera states, sky,
// skinned-mesh constants, the VP products) derives from the context, so rotating the INPUT vectors
// here steers everything at once. Disassembled from the supplied WAR.exe.
#if defined(_MSC_VER)
#define WAR_THISCALL __fastcall      // ecx=this, edx unused, args on stack, callee pops: same as thiscall
#else
#define WAR_THISCALL __attribute__((fastcall))
#endif
using Vec3f = vrm::V3;      // identical layout: three floats
using SetCamera_t = void (WAR_THISCALL*)(void* self, void* edx, const Vec3f* pos, const Vec3f* fwd,
                                          const Vec3f* up, const Vec3f* right, const float* frustum, DWORD flags);

static vrm::OutputRing g_outRing;

static bool IsWritable(const void* p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    const DWORD w = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (mbi.State == MEM_COMMIT) && (mbi.Protect & w) && !(mbi.Protect & PAGE_GUARD) &&
           (static_cast<const char*>(p) + n <= static_cast<const char*>(mbi.BaseAddress) + mbi.RegionSize);
}

// ---- camera takeover: make the engine's OWN culling follow the head -----------------------------------------
// WAR calls SetCamera from SetCameraData(NiCamera*) (WAR.exe+0x2EF6F0), and the very next thing the caller does is
// cull the scene with that same camera object (culler->Process(camera, scene, visibleArray)). So whatever we write
// into the camera object from inside this hook is what the culler sees. Layout (disassembly + the live pointers in
// your log): rotation 3x3 row-major at +0x60 (direction = column 0, up = 1, right = 2), world translation at +0x84,
// view frustum {left, right, top, bottom, near, far} at +0xF0. Everything is restored at Present.
struct CamTakeover {
    uint8_t* base = nullptr;
    float rot[9], pos[3], fr[4];     // the game's original values, saved before the first modification of a frame
    float wRot[9], wPos[3], wFr[4];  // what WE wrote, so we only undo it if the game has not written the object since
    float farOrig = 0, wFar = 0;     // the object's far plane (+0x104) as the game had it / as we wrote it (draw_distance); 0 = untouched
    bool modified = false;
    int verified = 0;                // 0 unknown, 1 layout confirmed against SetCamera's arguments, -1 refused
};
static CamTakeover g_cam;
static float g_drawDistScale = 1.0f;             // INI draw_distance: multiplies the main camera's far plane (1 = the game's own setting)
static bool g_drawDistFlat = true;               // INI draw_distance_flat: also apply draw_distance when VR is off (on by default)
static bool g_flatFarLogged = false;
static float g_lastFarEngine = 0, g_lastFarUsed = 0;
static CamTakeover g_camRefl;                    // the reflection camera object (planar water), same layout as the main camera
static bool g_gameBasisValid = false;            // the game's own (unsteered) camera basis, as last passed to SetCamera for the main view
static Vec3f g_gameR{}, g_gameU{}, g_gameF{};
static std::atomic<bool> g_cullTakeover{true};    // the engine's culling follows the head (tested working); INI cull=off or Home disables
static float g_unionFrustum[4] = {};
static unsigned g_camTakeovers = 0, g_camRefused = 0;

// True if p lies on the CURRENT thread's stack. The engine makes temporary camera copies on its stack; those must
// never be treated as "the camera object" (round-6c bug: rotated twice, and "restored" into dead stack memory).
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
#endif
static bool IsOnStack(const void* p)
{
    const NT_TIB* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    return p >= tib->StackLimit && p < tib->StackBase;
}
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

// Undo our changes - but only the parts the game has not rewritten since (otherwise we would overwrite its newer
// camera with a stale snapshot, which freezes the camera). Render thread only (called from Present/Reset).
static void RestoreCamObj(CamTakeover& c)
{
    if (!c.modified || !c.base) return;
    c.modified = false;
    if (!IsWritable(c.base + 0x60, 0xA4)) return;
    if (!memcmp(c.base + 0x60, c.wRot, sizeof(c.wRot)) && !memcmp(c.base + 0x84, c.wPos, sizeof(c.wPos))) {
        memcpy(c.base + 0x60, c.rot, sizeof(c.rot));
        memcpy(c.base + 0x84, c.pos, sizeof(c.pos));
    }
    if (!memcmp(c.base + 0xF0, c.wFr, sizeof(c.wFr))) memcpy(c.base + 0xF0, c.fr, sizeof(c.fr));
    if (c.farOrig > 0.f) {
        float* pf = reinterpret_cast<float*>(c.base + 0x104);
        if (*pf == c.wFar) *pf = c.farOrig;
        c.farOrig = 0.f;
    }
}
static void RestoreCamera() { RestoreCamObj(g_cam); RestoreCamObj(g_camRefl); }

// The engine's frustum structure is NINE dwords (36 bytes), not the 28 bytes of a plain NiFrustum:
//   x0 x1 y0 y1 near far | flag (byte, padded) | viewport MinZ | viewport MaxZ
// SetCamera reads [+0x1C]/[+0x20] as the D3D viewport depth range and block-copies all 9 dwords into its context
// (disassembled: "mov ecx,9 / rep movsd"). Any copy we hand it must therefore be the full 36 bytes, otherwise the
// depth range is stack garbage (rounds 6a-6d: purple screen, tiny window, depth order wrong).
static constexpr int kFrustumDwords = 9;
struct Steered { Vec3f pos, fwd, up, right; float fr[kFrustumDwords]; bool vr; };
static_assert(sizeof(float) * kFrustumDwords == 36, "frustum structure is 36 bytes");

// Apply head rotation (and, in VR mode, this eye's frustum, head position and eye offset) to a camera.
static void ComputeSteered(const Vec3f& pos, const Vec3f& right, const Vec3f& up, const Vec3f& fwd, const float* frustum,
                           const vrm::Mat3& q, Steered* o)
{
    Vec3f r0 = right, u0 = up, f0 = fwd;
    if (g_pitchLock.load() && g_vrActive) vrm::levelBasis(right, up, fwd, &r0, &u0, &f0);       // remove the mouse's pitch, keep its heading
    vrm::rotateBasis(q, r0, u0, f0, &o->right, &o->up, &o->fwd);
    memcpy(o->fr, frustum, sizeof(o->fr));
    if ((g_vrActive || g_drawDistFlat) && g_drawDistScale > 1.0001f && o->fr[5] > 1.f) { g_lastFarEngine = o->fr[5]; o->fr[5] *= g_drawDistScale; g_lastFarUsed = o->fr[5]; }
    o->pos = pos;
    o->vr = false;
    if (g_vrActive && g_eyeParamsOk && !g_perturb.load()) {
        const int eye = g_vrEye;
        memcpy(o->fr, g_eyeFrustum[eye], 4 * sizeof(float));
        vrm::V3 off = { 0, 0, 0 };
        if (g_positional) off = vrm::worldOffset(g_headDeltaCam, r0, u0, f0, g_unitsPerMeter);
        const vrm::V3 ev = vrm::worldOffset(vrm::eyeOffsetCamera(g_eyeToHead[eye]), o->right, o->up, o->fwd, g_unitsPerMeter);
        o->pos = { pos.x + off.x + ev.x, pos.y + off.y + ev.y, pos.z + off.z + ev.z };
        o->vr = true;
    }
}

// The planar-water reflection camera: the game camera mirrored about z. Steer it like the main camera (head rotation, head position),
// but with ONE symmetric frustum that covers both eyes, and write the same values into its camera object so the engine's culling covers
// what the map will show. See vrmath.h (planar water reflection) for why.
static bool TryReflectionCamera(const Vec3f* pos, const Vec3f* fwd, const Vec3f* up, const Vec3f* right, const float* frustum, bool camObj, Steered* S)
{
    if (!vrm::isReflectionOf(*right, *up, *fwd, g_gameR, g_gameU, g_gameF, 0.95f)) return false;
    if (std::fabs(frustum[0] + frustum[1]) > 1e-3f * std::fabs(frustum[1]) || std::fabs(frustum[2] + frustum[3]) > 1e-3f * std::fabs(frustum[2])) return false;
    { float probe[4]; if (!(vrm::symmetricFrustum(g_eyeFrustum[0], g_eyeFrustum[1], probe) > 0.1f)) return false; }   // eye frustums not known yet
    Vec3f gr, gu, gf; vrm::reflectionBasis(*right, *up, *fwd, &gr, &gu, &gf);                 // the game camera it was mirrored from
    if (g_pitchLock.load()) { Vec3f lr, lu, lf; if (vrm::levelBasis(gr, gu, gf, &lr, &lu, &lf)) { gr = lr; gu = lu; gf = lf; } }   // same level camera as the main view
    Vec3f rs, us, fs; vrm::rotateBasis(g_frameQ, gr, gu, gf, &rs, &us, &fs);                  // steered like the main camera
    vrm::V3 off = { 0, 0, 0 };
    if (g_positional) off = vrm::worldOffset(g_headDeltaCam, gr, gu, gf, g_unitsPerMeter);
    S->pos = { pos->x + off.x, pos->y + off.y, pos->z + off.z };
    vrm::reflectionBasis(rs, us, fs, &S->right, &S->up, &S->fwd);
    float sf[4]; g_reflS = vrm::symmetricFrustum(g_eyeFrustum[0], g_eyeFrustum[1], sf);
    memcpy(S->fr, frustum, sizeof(S->fr)); memcpy(S->fr, sf, sizeof(sf));
    S->vr = true;

    if (camObj && !IsOnStack(frustum) && g_camRefl.verified >= 0) {                            // culling: write the camera object too
        uint8_t* cam = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(frustum)) - 0xF0;
        bool ok = true;
        if (!(g_camRefl.modified && g_camRefl.base == cam)) {
            RestoreCamObj(g_camRefl);
            if (!IsWritable(cam + 0x60, 0xA4)) ok = false;
            else {
                const float* m = reinterpret_cast<const float*>(cam + 0x60);
                if (g_camRefl.verified == 0) {
                    const bool match = vrm::gamebryoRotMatchesBasis(m, *fwd, *up, *right);
                    Log("REFLECTION CAMERA layout check: camera object %p matches SetCamera's basis = %d\n", (void*)cam, match ? 1 : 0);
                    g_camRefl.verified = match ? 1 : -1;
                }
                if (g_camRefl.verified != 1) ok = false;
                else { memcpy(g_camRefl.rot, m, sizeof(g_camRefl.rot)); memcpy(g_camRefl.pos, cam + 0x84, sizeof(g_camRefl.pos)); memcpy(g_camRefl.fr, cam + 0xF0, sizeof(g_camRefl.fr)); g_camRefl.base = cam; }
            }
        }
        if (ok && g_camRefl.verified == 1) {
            float rot[9]; vrm::basisToGamebryoRot(S->fwd, S->up, S->right, rot);
            memcpy(cam + 0x60, rot, sizeof(rot)); memcpy(cam + 0x84, &S->pos, sizeof(S->pos)); memcpy(cam + 0xF0, sf, sizeof(sf));
            memcpy(g_camRefl.wRot, cam + 0x60, sizeof(g_camRefl.wRot)); memcpy(g_camRefl.wPos, cam + 0x84, sizeof(g_camRefl.wPos)); memcpy(g_camRefl.wFr, cam + 0xF0, sizeof(g_camRefl.wFr));
            g_camRefl.modified = true;
        }
    }

    if (g_lastOffscreenRT && g_reflSurf != g_lastOffscreenRT) {                                // this pass renders into the reflection map
        g_reflSurf = g_lastOffscreenRT; g_reflTex = nullptr; g_reflStageMask = 0; g_reflTexSearches = 0;
        IDirect3DTexture9* t = nullptr;
        if (SUCCEEDED(g_reflSurf->GetContainer(__uuidof(IDirect3DTexture9), reinterpret_cast<void**>(&t))) && t) { g_reflTex = t; t->Release(); }
        D3DSURFACE_DESC d{}; g_reflSurf->GetDesc(&d);
        Log("WATER: reflection map identified: render target %p (%ux%u fmt %d), texture %p; symmetric frustum half width %.4f\n", (void*)g_reflSurf, d.Width, d.Height, (int)d.Format, (void*)g_reflTex, g_reflS);
    }
    g_reflHandled = true; ++g_reflCams;
    g_outRing.add(S->right, S->up, S->fwd);
    return true;
}

static void WAR_THISCALL HookSetCamera(void* self, void* edx, const Vec3f* pos, const Vec3f* fwd,
                                       const Vec3f* up, const Vec3f* right, const float* frustum, DWORD flags)
{
    ++g_setCameraCalls;
    { static bool once = false; if (!once) { once = true; Log("THREAD: first SetCamera call on thread %lu\n", GetCurrentThreadId()); } }
    Steered S;
    const Vec3f *pp = pos, *pf = fwd, *pu = up, *pr = right;
    const float* pfr = frustum;
    const char* act = "idle";
    // The primary call is recognisable by its pointers: pos and frustum are fields of one NiCamera object.
    const bool camObj = pos && frustum && (reinterpret_cast<const char*>(pos) + 0x6C == reinterpret_cast<const char*>(frustum));

    if (g_steerMode.load() != 0) act = "mode-constants";
    else if (!g_rtIsMain) {
        act = "offscreen";
        if (g_waterMode && g_vrActive && g_eyeParamsOk && !g_perturb.load() && g_headOn.load() && g_frameQValid && g_gameBasisValid &&
            pos && fwd && up && right && frustum) {
            if (g_outRing.contains(*right, *up, *fwd)) act = "offscreen-derived-skip";
            else if (TryReflectionCamera(pos, fwd, up, right, frustum, camObj, &S)) {
                pp = &S.pos; pf = &S.fwd; pu = &S.up; pr = &S.right; pfr = S.fr;
                act = "REFLECTION+VR";
            }
        }
    }
    else if (!(pos && fwd && up && right)) act = "bad-args";
    else {
        bool handled = false;
        const vrm::Mat3* q = ActiveRotation();
        // Flat (VR off) with draw_distance: run the same camera path with no rotation, so only the far plane changes.
        static const vrm::Mat3 kNoRotation = vrm::identity();
        const bool flatFar = !q && !g_vrActive && g_drawDistFlat && g_drawDistScale > 1.0001f && g_rtIsMain;
        if (flatFar) q = &kNoRotation;
        if (camObj && q && g_cullTakeover.load() && g_cam.verified >= 0 && !IsOnStack(frustum)) {
            uint8_t* cam = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(frustum)) - 0xF0;
            Vec3f oPos = *pos, oFwd = *fwd, oUp = *up, oRight = *right;
            float oFr[kFrustumDwords]; memcpy(oFr, frustum, sizeof(oFr));
            bool ok = true;
            if (g_cam.modified && g_cam.base == cam) {
                // Earlier call this frame already wrote the object: steer from the originals we saved.
                const float* m = g_cam.rot;
                oPos = { g_cam.pos[0], g_cam.pos[1], g_cam.pos[2] };
                oFwd = { m[0], m[3], m[6] }; oUp = { m[1], m[4], m[7] }; oRight = { m[2], m[5], m[8] };
                memcpy(oFr, g_cam.fr, 4 * sizeof(float));              // near/flag stay as the object has them
                if (g_cam.farOrig > 0.f) oFr[5] = g_cam.farOrig;       // far: the game's value, not the one we scaled
            } else {
                RestoreCamera();
                if (!IsWritable(cam + 0x60, 0xA4)) ok = false;
                else {
                    const float* m = reinterpret_cast<const float*>(cam + 0x60);
                    if (g_cam.verified == 0) {
                        const bool match = vrm::gamebryoRotMatchesBasis(m, oFwd, oUp, oRight);
                        Log("CAMERA TAKEOVER layout check: camera object %p, rotation@+0x60 matches SetCamera's basis = %d\n", (void*)cam, match ? 1 : 0);
                        if (!match) {
                            Log("   object rotation: %.4f %.4f %.4f / %.4f %.4f %.4f / %.4f %.4f %.4f ; SetCamera fwd=(%.4f %.4f %.4f) up=(%.4f %.4f %.4f) right=(%.4f %.4f %.4f)\n",
                                m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], oFwd.x, oFwd.y, oFwd.z, oUp.x, oUp.y, oUp.z, oRight.x, oRight.y, oRight.z);
                            ++g_camRefused;
                        }
                        g_cam.verified = match ? 1 : -1;
                    }
                    if (g_cam.verified != 1) ok = false;
                    else {
                        memcpy(g_cam.rot, m, sizeof(g_cam.rot));
                        memcpy(g_cam.pos, cam + 0x84, sizeof(g_cam.pos));
                        memcpy(g_cam.fr, cam + 0xF0, sizeof(g_cam.fr));
                        g_cam.farOrig = (g_drawDistScale > 1.0001f) ? *reinterpret_cast<const float*>(cam + 0x104) : 0.f;
                        g_cam.base = cam;
                    }
                }
            }
            if (ok) {
                g_gameR = oRight; g_gameU = oUp; g_gameF = oFwd; g_gameBasisValid = true;     // the game's own camera, for recognising its reflection
                ComputeSteered(oPos, oRight, oUp, oFwd, oFr, *q, &S);
                float rot[9]; vrm::basisToGamebryoRot(S.fwd, S.up, S.right, rot);
                memcpy(cam + 0x60, rot, sizeof(rot));
                memcpy(cam + 0x84, &S.pos, sizeof(S.pos));
                if (S.vr) memcpy(cam + 0xF0, g_unionFrustum, sizeof(g_unionFrustum));   // culling frustum covers both eyes
                if (g_cam.farOrig > 0.f) { g_cam.wFar = S.fr[5]; memcpy(cam + 0x104, &S.fr[5], sizeof(float)); }   // and reaches farther
                memcpy(g_cam.wRot, cam + 0x60, sizeof(g_cam.wRot));
                memcpy(g_cam.wPos, cam + 0x84, sizeof(g_cam.wPos));
                memcpy(g_cam.wFr, cam + 0xF0, sizeof(g_cam.wFr));
                g_cam.modified = true;
                pp = &S.pos; pf = &S.fwd; pu = &S.up; pr = &S.right; pfr = S.fr;
                g_outRing.add(S.right, S.up, S.fwd);
                act = S.vr ? (g_vrEye ? "TAKEOVER+VR(R)" : "TAKEOVER+VR(L)") : (flatFar ? "TAKEOVER-FLAT-FAR" : "TAKEOVER");
                if (flatFar && !g_flatFarLogged) { g_flatFarLogged = true; Log("draw_distance (flat): main camera far plane %.0f -> %.0f\n", oFr[5], S.fr[5]); }
                ++g_camTakeovers; ++g_setCameraRotated;
                handled = true;
            } else act = "takeover-refused";
        }
        if (!handled) {
            if (g_outRing.contains(*right, *up, *fwd)) {
                act = "derived-skip"; ++g_setCameraDerived;
                if (g_vrActive && g_eyeParamsOk && g_cullTakeover.load() && frustum && !memcmp(frustum, g_unionFrustum, sizeof(g_unionFrustum))) {
                    memcpy(S.fr, frustum, sizeof(S.fr));                 // a copy made after we widened the culling frustum
                    memcpy(S.fr, g_eyeFrustum[g_vrEye], 4 * sizeof(float));
                    pfr = S.fr; act = "derived-skip+eyefrustum";
                }
            }
            else if (q) {
                ComputeSteered(*pos, *right, *up, *fwd, frustum, *q, &S);
                pp = &S.pos; pf = &S.fwd; pu = &S.up; pr = &S.right; pfr = S.fr;
                g_outRing.add(S.right, S.up, S.fwd);
                act = S.vr ? (g_vrEye ? "ROTATED+VR(R)" : "ROTATED+VR(L)") : "ROTATED";
                ++g_setCameraRotated;
            }
        }
    }
    if ((g_capturing || g_camLogBudget) && pos && fwd && up && right && frustum) {
        if (!g_capturing && g_camLogBudget) --g_camLogBudget;
        Log("  SETCAMERA rt=%ux%u act=%s pos=(%.1f %.1f %.1f) fwd=(%.3f %.3f %.3f) up=(%.3f %.3f %.3f) right=(%.3f %.3f %.3f) "
            "frustum=[x0=%.4f x1=%.4f y0=%.4f y1=%.4f near=%.1f far=%.0f flag=%u] flags=0x%lX self=%p ptrs: pos=%p frustum=%p camObj=%d caller=%s\n",
            g_curRTw, g_curRTh, act, pos->x, pos->y, pos->z, fwd->x, fwd->y, fwd->z, up->x, up->y, up->z,
            right->x, right->y, right->z, frustum[0], frustum[1], frustum[2], frustum[3], frustum[4], frustum[5],
            (unsigned)*reinterpret_cast<const unsigned char*>(reinterpret_cast<const char*>(frustum) + 0x18), (unsigned long)flags, self,
            (const void*)pos, (const void*)frustum, camObj ? 1 : 0, Describe(RETADDR()).c_str());
    }
    reinterpret_cast<SetCamera_t>(g_setCameraTramp)(self, edx, pp, pf, pu, pr, pfr, flags);
}

// Generic 5-byte-jump inline hook. `len` bytes (>=5) are copied into a trampoline, so they must be
// whole, position-independent instructions. `expect` guards against a different WAR.exe build.
static bool InstallInlineHook(uintptr_t target, size_t len, const unsigned char* expect, void* hook, void** outTramp, const char* name)
{
    BYTE* src = reinterpret_cast<BYTE*>(target);
    if (memcmp(src, expect, len) != 0) {
        char got[64]{}; for (size_t i = 0; i < len && i < 8; ++i) _snprintf_s(got + i*3, 4, _TRUNCATE, "%02X ", src[i]);
        Log("InstallInlineHook(%s): bytes at %p do not match this WAR.exe build (got %s) - NOT hooking\n", name, src, got);
        return false;
    }
    BYTE* tramp = reinterpret_cast<BYTE*>(VirtualAlloc(nullptr, len + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) { Log("InstallInlineHook(%s): VirtualAlloc failed %lu\n", name, GetLastError()); return false; }
    memcpy(tramp, src, len);
    tramp[len] = 0xE9;
    *reinterpret_cast<int32_t*>(tramp + len + 1) = static_cast<int32_t>(reinterpret_cast<uintptr_t>(src + len) - reinterpret_cast<uintptr_t>(tramp + len + 5));
    DWORD old = 0;
    if (!VirtualProtect(src, len, PAGE_EXECUTE_READWRITE, &old)) { Log("InstallInlineHook(%s): VirtualProtect failed %lu\n", name, GetLastError()); return false; }
    BYTE patch[16]; memset(patch, 0x90, sizeof(patch));
    patch[0] = 0xE9;
    *reinterpret_cast<int32_t*>(patch + 1) = static_cast<int32_t>(reinterpret_cast<uintptr_t>(hook) - (target + 5));
    *outTramp = tramp;                       // publish trampoline before the jump goes live
    memcpy(src, patch, len);
    FlushInstructionCache(GetCurrentProcess(), src, len);
    DWORD ignored = 0; VirtualProtect(src, len, old, &ignored);
    Log("InstallInlineHook(%s): hooked %p, trampoline %p\n", name, src, tramp);
    return true;
}

static void InstallCameraHook()
{
    if (g_cameraHookInstalled) return;
    HMODULE war = GetModuleHandleW(L"WAR.exe");
    if (!war) return;
    static const unsigned char expect[6] = { 0x83, 0xEC, 0x20, 0x53, 0x8B, 0xD9 };   // sub esp,20h / push ebx / mov ebx,ecx
    uintptr_t addr = reinterpret_cast<uintptr_t>(war) + 0x38C850;
    if (InstallInlineHook(addr, 6, expect, reinterpret_cast<void*>(&HookSetCamera), &g_setCameraTramp, "SetCamera")) {
        g_cameraHookInstalled = true;
    } else {
        g_steerMode = 1;
        Log("Camera hook unavailable: falling back to the older SetTransform/constants steering\n");
    }
}

// ---------------------------------------------------------------- head tracking
static void PollHead()
{
    if (!g_headOn.load() || !g_vr.ready()) { g_frameQValid = false; return; }
    OpenVRHead::Pose pose;
    if (g_vrActive) { if (!g_vrHavePose) return; pose = g_vrPose; }      // pose from the compositor for this frame pair
    else if (!g_vr.Poll(0.011f, &pose)) return;                         // pose-reader mode: keep last good Q on failure
    const vrm::Mat3& head = pose.rot;
    if (g_recenterRequest.exchange(false) || !g_haveRef) {
        float yaw;
        if (vrm::headYaw(head, &yaw)) {
            g_refYaw = yaw; g_haveRef = true;
            for (int i = 0; i < 3; ++i) g_refPos[i] = pose.pos[i];
            Log("HEAD recentered refYaw=%.1f deg refPos=(%.3f %.3f %.3f)\n", yaw * 57.29578f, g_refPos[0], g_refPos[1], g_refPos[2]);
        }
    }
    if (!g_haveRef) return;
    g_frameQ = vrm::cameraDelta(head, g_refYaw);
    g_headDeltaCam = vrm::headDeltaCamera(g_refYaw, g_refPos, pose.pos);
    g_frameQValid = true;
    if ((g_frame % 120) == 0) {
        float fy = -head.m[1][2];
        float yaw = 0.f; vrm::headYaw(head, &yaw);
        Log("HEAD yaw(abs)=%.1f pitch=%.1f  (relative yaw=%.1f) pos delta (r,u,f)=(%.3f %.3f %.3f) m\n", yaw * 57.29578f, std::asin(fy) * 57.29578f,
            (yaw - g_refYaw) * 57.29578f, g_headDeltaCam.x, g_headDeltaCam.y, g_headDeltaCam.z);
    }
}


// ---------------------------------------------------------------- camera memory finder (F8)
// Looks in WAR's own memory for the numbers the camera was using this frame, so we can find where WAR
// keeps its camera (needed later to widen the game's own culling FOV and to drive its camera directly).
// Stand still, do not touch the mouse/keyboard camera, and press F8 once.
static bool PtrInRange(const void* p, const void* base, size_t size)
{
    return p >= base && p < static_cast<const char*>(base) + size;
}

static void DumpFloatsAround(const uint8_t* mem, uintptr_t addr, int before, int after)
{
    char line[1024]; int n = 0;
    for (int i = -before; i < after; ++i) {
        float f; memcpy(&f, mem + (i + before) * 4, 4);
        n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE, "%s%.5g", (i == 0) ? " [" : " ", f);
        if (i == 0) n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE, "]");
    }
    Log("      floats around 0x%08lX:%s\n", (unsigned long)addr, line);
}

static bool NearF(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

static void RunMemoryScan()
{
    if (!g_mainValid.load()) { Log("SCAN: no main-world camera seen yet (be in the 3D world, then press F8)\n"); return; }
    const D3DMATRIX view = g_mainView, proj = g_mainProj;
    // eye position in world: -T * R^T  (view = [R;T], row-vector)
    float eye[3];
    for (int j = 0; j < 3; ++j)
        eye[j] = -(view.m[3][0]*view.m[j][0] + view.m[3][1]*view.m[j][1] + view.m[3][2]*view.m[j][2]);
    float zn = 0, zf = 0, fov = 0, asp = 0; vrm::decodePerspective(proj.m, &zn, &zf, &fov, &asp);
    Log("==== SCAN BEGIN ====\n");
    Log("SCAN target view row0=(%.6f %.6f %.6f) row1=(%.6f %.6f %.6f) row2=(%.6f %.6f %.6f)\n",
        view.m[0][0], view.m[0][1], view.m[0][2], view.m[1][0], view.m[1][1], view.m[1][2], view.m[2][0], view.m[2][1], view.m[2][2]);
    Log("SCAN target eye=(%.2f %.2f %.2f)  proj m00=%.6f m11=%.6f near=%.1f far=%.0f fovY=%.2f deg aspect=%.3f\n",
        eye[0], eye[1], eye[2], proj.m[0][0], proj.m[1][1], zn, zf, fov, asp);

    const DWORD t0 = GetTickCount();
    SYSTEM_INFO si; GetSystemInfo(&si);
    uintptr_t addr = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    const uintptr_t maxAddr = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
    HANDLE self = GetCurrentProcess();
    std::vector<uint8_t> buf(1 << 20);
    int hitsView = 0, hitsEye = 0, hitsProj = 0;
    const int kMaxHits = 24;
    unsigned long long scanned = 0;
    HMODULE war = GetModuleHandleW(L"WAR.exe");

    auto report = [&](const char* what, uintptr_t a, const uint8_t* chunk, size_t chunkLen, size_t off, int& counter) {
        if (counter >= kMaxHits) { ++counter; return; }
        ++counter;
        const void* p = reinterpret_cast<void*>(a);
        MEMORY_BASIC_INFORMATION mbi{};
        VirtualQuery(p, &mbi, sizeof(mbi));
        Log("  HIT %-9s at 0x%08lX = %s  (region base=%p size=%lu KB type=%s)\n", what, (unsigned long)a, Describe(p).c_str(),
            mbi.AllocationBase, (unsigned long)(mbi.RegionSize / 1024), mbi.Type == MEM_IMAGE ? "image" : "private");
        if (counter <= 6 && off >= 48 && off + 64 <= chunkLen) DumpFloatsAround(chunk + off - 48, a, 12, 16);
    };

    while (addr < maxAddr && g_running) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) break;
        uintptr_t regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        uintptr_t regionEnd = regionBase + mbi.RegionSize;
        const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        bool ok = mbi.State == MEM_COMMIT && (mbi.Protect & readable) && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS | PAGE_WRITECOMBINE))
                  && mbi.Type != MEM_MAPPED;
        if (ok && mbi.AllocationBase == reinterpret_cast<void*>(g_self)) ok = false;   // skip our own DLL's data
        if (ok) {
            for (uintptr_t a = regionBase; a < regionEnd && g_running; ) {
                size_t want = (size_t)std::min<uintptr_t>(buf.size(), regionEnd - a);
                SIZE_T got = 0;
                if (!ReadProcessMemory(self, reinterpret_cast<void*>(a), buf.data(), want, &got) || got < 16) { a += want; continue; }
                scanned += got;
                const float* f = reinterpret_cast<const float*>(buf.data());
                const uint32_t* u = reinterpret_cast<const uint32_t*>(buf.data());
                const size_t nf = got / 4;
                uint32_t bitsM00, bitsM11; memcpy(&bitsM00, &proj.m[0][0], 4); memcpy(&bitsM11, &proj.m[1][1], 4);
                for (size_t i = 0; i + 8 < nf; ++i) {
                    uintptr_t at = a + i * 4;
                    // view rotation: first two rows (6 floats) consecutive or row0 alone, exact bits or tiny tolerance
                    if (NearF(f[i], view.m[0][0], 1e-6f) && NearF(f[i+1], view.m[0][1], 1e-6f) && NearF(f[i+2], view.m[0][2], 1e-6f)
                        && std::fabs(f[i]) > 1e-3f && !PtrInRange(reinterpret_cast<void*>(at), buf.data(), buf.size()))
                        report("view-row0", at, buf.data(), got, i * 4, hitsView);
                    // world eye position
                    if (NearF(f[i], eye[0], 0.05f) && NearF(f[i+1], eye[1], 0.05f) && NearF(f[i+2], eye[2], 0.05f)
                        && std::fabs(eye[0]) > 1.f && !PtrInRange(reinterpret_cast<void*>(at), buf.data(), buf.size()))
                        report("eye-pos", at, buf.data(), got, i * 4, hitsEye);
                    // projection matrix: m00 0 0 0 0 m11 (exact)
                    if (u[i] == bitsM00 && u[i+1] == 0 && u[i+2] == 0 && u[i+3] == 0 && u[i+4] == 0 && u[i+5] == bitsM11)
                        report("proj", at, buf.data(), got, i * 4, hitsProj);
                }
                a += got;
            }
        }
        addr = regionEnd;
        if (addr <= regionBase) break;
    }
    (void)war;
    Log("SCAN done in %lu ms, %llu MB read. hits: view-row0=%d eye-pos=%d proj=%d (only first %d of each are listed)\n",
        (unsigned long)(GetTickCount() - t0), scanned >> 20, hitsView, hitsEye, hitsProj, kMaxHits);
    Log("==== SCAN END ====\n");
}

// ---------------------------------------------------------------- probes
static void RunProbes(IDirect3DDevice9* dev)
{
    Log("==== PROBES (frame %llu) ====\n", g_frame);

    D3DCAPS9 caps{};
    if (SUCCEEDED(dev->GetDeviceCaps(&caps)))
        Log("caps: VS=%lu.%lu PS=%lu.%lu maxVSConst=%lu maxTexW=%lu simultaneousRT=%lu devType=%d\n",
            D3DSHADER_VERSION_MAJOR(caps.VertexShaderVersion), D3DSHADER_VERSION_MINOR(caps.VertexShaderVersion),
            D3DSHADER_VERSION_MAJOR(caps.PixelShaderVersion), D3DSHADER_VERSION_MINOR(caps.PixelShaderVersion),
            (unsigned long)caps.MaxVertexShaderConst, (unsigned long)caps.MaxTextureWidth,
            (unsigned long)caps.NumSimultaneousRTs, (int)caps.DeviceType);

    IDirect3DSurface9* bb = nullptr;
    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
        D3DSURFACE_DESC d{};
        bb->GetDesc(&d);
        Log("backbuffer: %ux%u fmt=%d msaa=%d usage=0x%lX pool=%d\n", d.Width, d.Height, (int)d.Format, (int)d.MultiSampleType, (unsigned long)d.Usage, (int)d.Pool);
        bb->Release();
    }

    // Can this (plain D3D9) device create a shared render target? Needed for zero-copy hand-off to D3D11/OpenVR.
    // Not under DXVK: the VK path needs no shared handle, and DXVK asks the Vulkan driver for Win32 external memory
    // here; without it (Mesa under Wine) the texture comes back broken and the game crashed soon after.
    if (g_isDxvk) Log("probe shared RT: skipped (DXVK, the VK path hands off without shared handles)\n");
    else {
        HANDLE shared = nullptr;
        IDirect3DTexture9* tex = nullptr;
        HRESULT hr = dev->CreateTexture(1024, 1024, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex, &shared);
        Log("probe shared RT (A8R8G8B8, DEFAULT pool): hr=0x%08lX handle=%p\n", (unsigned long)hr, shared);
        if (tex) tex->Release();
    }
    {
        IDirect3DDevice9Ex* ex = nullptr;
        HRESULT hr = dev->QueryInterface(__uuidof(IDirect3DDevice9Ex), reinterpret_cast<void**>(&ex));
        Log("probe QueryInterface(IDirect3DDevice9Ex): hr=0x%08lX\n", (unsigned long)hr);
        if (ex) ex->Release();
    }
    HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
    char path[MAX_PATH]{};
    if (d3d9) GetModuleFileNameA(d3d9, path, MAX_PATH);
    Log("d3d9.dll base=%p path=%s\n", d3d9, path);
    Log("==== END PROBES ====\n");
}

static void LogPresentParams(const char* tag, const D3DPRESENT_PARAMETERS* pp)
{
    if (!pp) return;
    Log("%s: back=%ux%u fmt=%d count=%u msaa=%d(q=%lu) swap=%d windowed=%d autoDS=%d dsFmt=%d flags=0x%lX hz=%u interval=0x%X hwnd=%p\n",
        tag, pp->BackBufferWidth, pp->BackBufferHeight, (int)pp->BackBufferFormat, pp->BackBufferCount,
        (int)pp->MultiSampleType, (unsigned long)pp->MultiSampleQuality, (int)pp->SwapEffect, (int)pp->Windowed,
        (int)pp->EnableAutoDepthStencil, (int)pp->AutoDepthStencilFormat, (unsigned long)pp->Flags,
        pp->FullScreen_RefreshRateInHz, pp->PresentationInterval, (void*)pp->hDeviceWindow);
}

// ---------------------------------------------------------------- VR display pipeline (runs on the render thread)
static double NowMs()
{
    static LARGE_INTEGER freq = []{ LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return 1000.0 * double(t.QuadPart) / double(freq.QuadPart);
}

static void VRReleaseCapture()
{
    if (g_capRT) { g_capRT->Release(); g_capRT = nullptr; }
    if (g_capSys) { g_capSys->Release(); g_capSys = nullptr; }
}

static bool VRCreateCapture(IDirect3DDevice9* dev)
{
    const UINT w = g_vrDisplay.width(), h = g_vrDisplay.height();
    HRESULT hr = dev->CreateRenderTarget(w, h, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &g_capRT, nullptr);
    if (FAILED(hr)) { Log("VR: CreateRenderTarget(%ux%u) failed hr=0x%08lX\n", w, h, (unsigned long)hr); return false; }
    hr = dev->CreateOffscreenPlainSurface(w, h, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &g_capSys, nullptr);
    if (FAILED(hr)) { Log("VR: CreateOffscreenPlainSurface(%ux%u) failed hr=0x%08lX\n", w, h, (unsigned long)hr); VRReleaseCapture(); return false; }
    g_capBuf.assign(size_t(w) * h, 0xFF000000u);
    Log("VR: capture surfaces created (%ux%u)\n", w, h);
    return true;
}

// back buffer region -> (GPU stretch to eye size) -> system memory -> alpha forced opaque -> D3D11 eye texture.
// srcRect==nullptr takes the whole back buffer (alternate-eye mode); otherwise one half (side-by-side mode).
// Copy w*h BGRA pixels forcing alpha opaque (the compositor must not see stray alpha from the game's UI blending).
static void CopyOpaque(const uint8_t* src, size_t pitch, uint32_t* dst, UINT w, UINT h)
{
    const __m128i alpha = _mm_set1_epi32((int)0xFF000000u);
    for (UINT y = 0; y < h; ++y) {
        const uint8_t* row = src + size_t(y) * pitch;
        uint32_t* out = dst + size_t(y) * w;
        UINT x = 0;
        for (; x + 4 <= w; x += 4)
            _mm_storeu_si128(reinterpret_cast<__m128i*>(out + x), _mm_or_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(row + size_t(x) * 4)), alpha));
        for (; x < w; ++x) out[x] = reinterpret_cast<const uint32_t*>(row)[x] | 0xFF000000u;
    }
}

// back buffer region -> (GPU stretch to eye size) -> system memory -> alpha forced opaque -> D3D11 eye texture.
// srcRect==nullptr takes the whole back buffer (alternate-eye mode); otherwise one half (side-by-side mode).
static bool VRCaptureEye(IDirect3DDevice9* dev, int eye, const RECT* srcRect)
{
    if ((!g_capRT || !g_capSys) && !VRCreateCapture(dev)) return false;
    IDirect3DSurface9* bb = nullptr;
    HRESULT hr = S_OK;
    if (g_redirect && g_vrRT) bb = g_vrRT;                       // the game's picture lives in our VR target
    else {
        hr = dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
        if (FAILED(hr) || !bb) { Log("VR: GetBackBuffer failed hr=0x%08lX\n", (unsigned long)hr); return false; }
    }
    double t0 = NowMs();
    hr = dev->StretchRect(bb, srcRect, g_capRT, nullptr, D3DTEXF_LINEAR);
    if (bb != g_vrRT) bb->Release();
    if (FAILED(hr)) { Log("VR: StretchRect failed hr=0x%08lX\n", (unsigned long)hr); return false; }
    double t1 = NowMs();
    hr = dev->GetRenderTargetData(g_capRT, g_capSys);
    if (FAILED(hr)) { Log("VR: GetRenderTargetData failed hr=0x%08lX\n", (unsigned long)hr); return false; }
    double t2 = NowMs();
    D3DLOCKED_RECT lr{};
    hr = g_capSys->LockRect(&lr, nullptr, D3DLOCK_READONLY);
    if (FAILED(hr)) { Log("VR: LockRect failed hr=0x%08lX\n", (unsigned long)hr); return false; }
    double t3 = NowMs();
    const UINT w = g_vrDisplay.width(), h = g_vrDisplay.height();
    CopyOpaque(static_cast<const uint8_t*>(lr.pBits), (size_t)lr.Pitch, g_capBuf.data(), w, h);
    g_capSys->UnlockRect();
    double t4 = NowMs();
    g_vrDisplay.Upload(eye, g_capBuf.data());
    double t5 = NowMs();
    g_tStretch += t1 - t0; g_tGetRTD += t2 - t1; g_tLock += t3 - t2; g_tConvert += t4 - t3; g_tUpload += t5 - t4; ++g_tSamples;
    g_vrHaveEye[eye] = true;
    return true;
}

// ---- vr_snapshot=N: save the picture the headset gets, once, after N submitted pairs ------------------------------
// Reads the side-by-side VR target (or the back buffer) back through plain D3D9, so it works with every hand-off path
// (CPU, GPU/dgVoodoo, VK/DXVK). Written at half size as a 24-bit BMP to %TEMP%\WARVR_vr_snapshot_<pair>.bmp.
static unsigned g_snapshotAt = 0;
static void VRSnapshot(IDirect3DDevice9* dev, unsigned pair)
{
    IDirect3DSurface9* src = nullptr;
    if (g_redirect && g_vrRT) { src = g_vrRT; src->AddRef(); }
    else if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &src)) || !src) { Log("VR snapshot: no source surface\n"); return; }
    D3DSURFACE_DESC d{}; src->GetDesc(&d);
    IDirect3DSurface9* sys = nullptr;
    HRESULT hr = dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr);
    if (SUCCEEDED(hr)) hr = dev->GetRenderTargetData(src, sys);
    src->Release();
    D3DLOCKED_RECT lr{};
    if (SUCCEEDED(hr)) hr = sys->LockRect(&lr, nullptr, D3DLOCK_READONLY);
    if (FAILED(hr) || (d.Format != D3DFMT_A8R8G8B8 && d.Format != D3DFMT_X8R8G8B8)) {
        Log("VR snapshot: readback failed hr=0x%08lX (format %d)\n", (unsigned long)hr, (int)d.Format);
        if (sys) { if (SUCCEEDED(hr)) sys->UnlockRect(); sys->Release(); }
        return;
    }
    const UINT w = d.Width / 2, h = d.Height / 2, row = (w * 3 + 3) & ~3u;
    std::vector<uint8_t> img(size_t(row) * h);
    for (UINT y = 0; y < h; ++y) {                                    // BMP rows go bottom-up; 2x2 box filter
        const uint8_t* a = static_cast<const uint8_t*>(lr.pBits) + size_t(2 * y) * lr.Pitch;
        const uint8_t* b = a + lr.Pitch;
        uint8_t* o = img.data() + size_t(h - 1 - y) * row;
        for (UINT x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c)
                o[x * 3 + c] = (uint8_t)((a[x * 8 + c] + a[x * 8 + 4 + c] + b[x * 8 + c] + b[x * 8 + 4 + c] + 2) / 4);
    }
    sys->UnlockRect(); sys->Release();
    char path[MAX_PATH]; char tmp[MAX_PATH] = "."; GetTempPathA(MAX_PATH, tmp);
    snprintf(path, sizeof(path), "%sWARVR_vr_snapshot_%u.bmp", tmp, pair);
    FILE* f = fopen(path, "wb");
    if (!f) { Log("VR snapshot: cannot write %s\n", path); return; }
    const uint32_t dataSize = (uint32_t)img.size(), fileSize = 54 + dataSize;
    uint8_t hd[54] = { 'B', 'M' };
    auto put32 = [&](int at, uint32_t v) { hd[at] = (uint8_t)v; hd[at + 1] = (uint8_t)(v >> 8); hd[at + 2] = (uint8_t)(v >> 16); hd[at + 3] = (uint8_t)(v >> 24); };
    put32(2, fileSize); put32(10, 54); put32(14, 40); put32(18, w); put32(22, h); hd[26] = 1; hd[28] = 24; put32(34, dataSize);
    fwrite(hd, 1, 54, f); fwrite(img.data(), 1, img.size(), f); fclose(f);
    Log("VR snapshot: %ux%u target saved at half size (%ux%u) to %s (left half = left eye)\n", d.Width, d.Height, w, h, path);
}

// ---- VR-sized render target ("vr_resolution=steamvr") ---------------------------------------------------------
// The game draws into what it believes is its back buffer. We give it a render target + depth buffer of the size
// SteamVR wants instead (both eyes side by side), and scale the viewports/clears/scissors it sets in ITS coordinates.
static bool g_redirectQuiet = false;                    // the fit loop logs a summary instead of every failed attempt
static bool VRRedirectBegin(IDirect3DDevice9* dev, UINT w, UINT h)
{
    if (g_redirect) return true;
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) { Log("VR redirect: GetBackBuffer failed\n"); return false; }
    g_bbSurf = bb; bb->Release();                              // raw pointer, used only for identity comparison
    D3DFORMAT dsFmt = D3DFMT_D24S8;
    IDirect3DSurface9* ds = nullptr;
    g_autoDS = nullptr;
    if (SUCCEEDED(dev->GetDepthStencilSurface(&ds)) && ds) { D3DSURFACE_DESC dd{}; ds->GetDesc(&dd); dsFmt = dd.Format; g_autoDS = ds; ds->Release(); }
    HRESULT hr = E_FAIL;
    if (g_vk.Attached()) {
        // DXVK: make the target level 0 of a render-target TEXTURE. Plain render-target surfaces get no SAMPLED usage in
        // DXVK, and SteamVR needs it, so this lets the Vulkan hand-off submit our target directly with no copy.
        IDirect3DTexture9* t = nullptr;
        hr = dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &t, nullptr);
        if (SUCCEEDED(hr) && t) { hr = t->GetSurfaceLevel(0, &g_vrRT); t->Release(); }      // the surface keeps its texture alive
        if (FAILED(hr)) g_vrRT = nullptr;
    }
    if (!g_vrRT) hr = dev->CreateRenderTarget(w, h, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &g_vrRT, nullptr);
    if (FAILED(hr)) { if (!g_redirectQuiet) Log("VR redirect: CreateRenderTarget(%ux%u) failed hr=0x%08lX (%s)\n", w, h, (unsigned long)hr, hr == D3DERR_OUTOFVIDEOMEMORY ? "D3DERR_OUTOFVIDEOMEMORY" : "?"); g_vrRT = nullptr; return false; }
    hr = dev->CreateDepthStencilSurface(w, h, dsFmt, D3DMULTISAMPLE_NONE, 0, FALSE, &g_vrDS, nullptr);
    if (FAILED(hr)) { if (!g_redirectQuiet) Log("VR redirect: CreateDepthStencilSurface(%ux%u fmt %d) failed hr=0x%08lX\n", w, h, (int)dsFmt, (unsigned long)hr); g_vrRT->Release(); g_vrRT = nullptr; g_vrDS = nullptr; return false; }
    g_bbW = w; g_bbH = h; g_curRTw = w; g_curRTh = h; g_rtIsMain = true;
    g_redirect = true;
    g_origSetRenderTarget(dev, 0, g_vrRT);
    g_origSetDS(dev, g_vrDS);
    g_haveGameVP = false; g_rightVersion = ~0u;
    Log("VR redirect: the game now renders into our %ux%u target (depth format %d); it believes its screen is %ux%u\n", w, h, (int)dsFmt, g_logW, g_logH);
    return true;
}

// Create the VR-sized target as large as the device allows: clamp to the D3D9 caps (and the D3D11 hard limit of 16384),
// and if the allocation still fails (video memory) retry 10% smaller until it works. w/h are in/out (both eyes side by side).
static bool VRRedirectBeginFit(IDirect3DDevice9* dev, UINT* w, UINT* h)
{
    D3DCAPS9 caps{};
    UINT maxW = 16384, maxH = 16384;
    if (SUCCEEDED(dev->GetDeviceCaps(&caps))) {
        Log("VR redirect: the D3D9 device reports MaxTextureWidth=%lu MaxTextureHeight=%lu\n", (unsigned long)caps.MaxTextureWidth, (unsigned long)caps.MaxTextureHeight);
        if (caps.MaxTextureWidth) maxW = std::min<UINT>(maxW, caps.MaxTextureWidth);
        if (caps.MaxTextureHeight) maxH = std::min<UINT>(maxH, caps.MaxTextureHeight);
    }
    double k = 1.0;
    if (*w > maxW) k = std::min(k, (double)maxW / (double)*w);
    if (*h > maxH) k = std::min(k, (double)maxH / (double)*h);
    if (k < 1.0) Log("VR redirect: %ux%u exceeds the device limit %ux%u -> scaling by %.3f\n", *w, *h, maxW, maxH, k);
    const UINT w0 = *w, h0 = *h;
    const double needMB = 8.0 * w0 * h0 * k * k / 1048576.0;                    // colour (4 B) + depth/stencil (4 B) per pixel
    const UINT freeBefore = dev->GetAvailableTextureMem() / 1048576u;
    Log("VR redirect: the full-size target needs about %.0f MB (colour + depth). D3D9 (dgVoodoo) says %u MB of texture memory are free right now.\n", needMB, freeBefore);
    bool evicted = false;
    int tries = 0;
    g_redirectQuiet = true;
    bool ok = false;
    for (int attempt = 0; attempt < 80 && !ok; ++attempt) {
        UINT tw = (UINT)((double)w0 * k + 0.5) & ~3u, th = (UINT)((double)h0 * k + 0.5) & ~1u;     // width stays a multiple of 4 so each eye is even
        if (tw < 512 || th < 256) break;
        ++tries;
        if (VRRedirectBegin(dev, tw, th)) { *w = tw; *h = th; ok = true; break; }
        if (!evicted) {                                                         // first failure: ask D3D9 to drop managed textures and retry the SAME size once
            evicted = true;
            dev->EvictManagedResources();
            const UINT freeAfter = dev->GetAvailableTextureMem() / 1048576u;
            Log("VR redirect: allocation failed; EvictManagedResources() changed the free texture memory %u MB -> %u MB\n", freeBefore, freeAfter);
            if (VRRedirectBegin(dev, tw, th)) { *w = tw; *h = th; ok = true; break; }
        }
        k *= 0.95;
    }
    g_redirectQuiet = false;
    Log("VR redirect: %s after %d attempt(s); free texture memory now %u MB\n", ok ? "target created" : "NO target could be created", tries, dev->GetAvailableTextureMem() / 1048576u);
    if (ok && (*w < w0 || *h < h0))
        Log("VR redirect: the target is smaller than requested because D3D9 ran out of video memory. This is dgVoodoo's emulated VRAM budget, not a size limit "
            "(MaxTexture is %u). Raise the VRAM setting in dgVoodoo (dgVoodooCpl.exe -> DirectX tab, or VRAM= in dgVoodoo.conf) to give it the ~%.0f MB it needs.\n", maxW, needMB + 64.0);
    return ok;
}

static void VRRedirectEnd(IDirect3DDevice9* dev)
{
    g_reflSurf = nullptr; g_reflTex = nullptr; g_reflStageMask = 0; g_reflHandled = false; g_lastOffscreenRT = nullptr; g_reflTexSearches = 0;
    VRHudRelease();                           // default-pool HUD texture: must go before a Reset or when VR stops
    if (!g_redirect) return;
    g_redirect = false;
    if (dev) {
        if (g_bbSurf) g_origSetRenderTarget(dev, 0, g_bbSurf);
        if (g_autoDS && g_origSetDS) g_origSetDS(dev, g_autoDS);
    }
    if (g_vrRT) { g_vrRT->Release(); g_vrRT = nullptr; }
    if (g_vrDS) { g_vrDS->Release(); g_vrDS = nullptr; }
    g_bbW = g_logW; g_bbH = g_logH; g_curRTw = g_logW; g_curRTh = g_logH;
    g_haveGameVP = false; g_rightVersion = ~0u;
}

// ---- mouse cursor in VR -----------------------------------------------------------------------------------
// The game uses the Windows hardware cursor, which is not part of the rendered image, so in VR it is invisible. We
// draw an arrow at the mouse position inside the HUD rectangle (once per eye, so it has the HUD's depth).
struct CursorVertex { float x, y, z; DWORD color; };

static void VRDrawCursor(IDirect3DDevice9* dev)
{
    OwnRS ownRS;                              // our SetRenderState calls are not the game's
    if (!g_cursorOn || !g_gameHwnd || !g_eyeParamsOk) return;
    POINT p{}; RECT cr{};
    if (!GetCursorPos(&p) || !ScreenToClient(g_gameHwnd, &p) || !GetClientRect(g_gameHwnd, &cr)) return;
    const long cw = cr.right - cr.left, ch = cr.bottom - cr.top;
    if (cw <= 0 || ch <= 0 || p.x < 0 || p.y < 0 || p.x >= cw || p.y >= ch) return;          // pointer is outside the game window
    float u, v; vrm::cursorUV(p.x, p.y, cw, ch, &u, &v);
    if (!g_haveGameVP) { if (FAILED(dev->GetViewport(&g_gameVP))) return; g_haveGameVP = true; g_devVP = g_gameVP; }
    const int rtW = (int)(g_curRTw ? g_curRTw : g_bbW), rtH = (int)(g_curRTh ? g_curRTh : g_bbH);
    if (rtW < 64 || rtH < 64) return;

    // Save every piece of state we touch; the game does not re-set state it believes is unchanged.
    static const D3DRENDERSTATETYPE kRS[] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_CULLMODE, D3DRS_LIGHTING,
        D3DRS_ALPHATESTENABLE, D3DRS_STENCILENABLE, D3DRS_SCISSORTESTENABLE, D3DRS_FOGENABLE, D3DRS_COLORWRITEENABLE,
        D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_FILLMODE, D3DRS_SPECULARENABLE };
    constexpr int kN = (int)(sizeof(kRS) / sizeof(kRS[0]));
    static const DWORD kRSval[kN] = { FALSE, FALSE, FALSE, D3DCULL_NONE, FALSE, FALSE, FALSE, FALSE, FALSE, 0xF, FALSE, D3DFILL_SOLID, FALSE };
    static const struct { DWORD stage; D3DTEXTURESTAGESTATETYPE type; DWORD val; } kTSS[] = {
        { 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1 }, { 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE },
        { 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1 }, { 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE }, { 1, D3DTSS_COLOROP, D3DTOP_DISABLE } };
    constexpr int kT = (int)(sizeof(kTSS) / sizeof(kTSS[0]));
    DWORD savedRS[kN], savedTSS[kT];
    for (int i = 0; i < kN; ++i) { savedRS[i] = 0; dev->GetRenderState(kRS[i], &savedRS[i]); }
    for (int i = 0; i < kT; ++i) { savedTSS[i] = 0; dev->GetTextureStageState(kTSS[i].stage, kTSS[i].type, &savedTSS[i]); }
    IDirect3DBaseTexture9* tex0 = nullptr; dev->GetTexture(0, &tex0);
    DWORD fvf = 0; dev->GetFVF(&fvf);
    IDirect3DVertexShader9* vs = nullptr; dev->GetVertexShader(&vs);
    IDirect3DPixelShader9* ps = nullptr; dev->GetPixelShader(&ps);
    D3DMATRIX mWorld, mView, mProj;
    dev->GetTransform(D3DTS_WORLD, &mWorld); dev->GetTransform(D3DTS_VIEW, &mView); dev->GetTransform(D3DTS_PROJECTION, &mProj);

    dev->SetVertexShader(nullptr); dev->SetPixelShader(nullptr); dev->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE); dev->SetTexture(0, nullptr);
    for (int i = 0; i < kN; ++i) dev->SetRenderState(kRS[i], kRSval[i]);
    for (int i = 0; i < kT; ++i) dev->SetTextureStageState(kTSS[i].stage, kTSS[i].type, kTSS[i].val);
    D3DMATRIX ident{}; ident._11 = ident._22 = ident._33 = ident._44 = 1.f;
    g_origSetTransform(dev, D3DTS_WORLD, &ident); g_origSetTransform(dev, D3DTS_VIEW, &ident);

    RECT savedScissorRc{}; dev->GetScissorRect(&savedScissorRc);
    float tri[40]; const int nv = vrm::cursorArrowTriangles(tri);                               // 15 vertices
    const bool texMode = HudAsTexture() && VRHudBind(dev);                                       // draw the cursor into the HUD texture, once
    const int eyes = texMode ? 1 : (g_stereoSBS ? 2 : 1);
    for (int e = 0; e < eyes; ++e) {
        const int eye = g_stereoSBS ? e : g_vrEye;
        int x, y, w, h; bool clip = false; RECT clipRc{};
        if (texMode) { x = 0; y = 0; w = (int)g_hudTexW; h = (int)g_hudTexH; g_hudUsed = true; }
        else if (!HudPlacement(eye, rtW, rtH, &x, &y, &w, &h, &clip, &clipRc)) continue;     // HUD (and cursor) not in this eye's view
        SetVP(dev, x, y, w, h);
        if (clip) { g_origSetScissor(dev, &clipRc); dev->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE); }
        else dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        D3DMATRIX proj{};                                   // pixels (0..w, 0..h, y down) -> clip space, z = 0
        proj._11 = 2.f / (float)w; proj._22 = -2.f / (float)h; proj._33 = 1.f; proj._41 = -1.f; proj._42 = 1.f; proj._44 = 1.f;
        g_origSetTransform(dev, D3DTS_PROJECTION, &proj);
        const float k = g_cursorSize * (float)h / 20.f;                                         // arrow is 20 units tall
        const float cx = u * (float)w, cy = v * (float)h;
        CursorVertex verts[30];
        for (int i = 0; i < nv; ++i) {                                                           // black outline first, white arrow on top
            verts[i] = { cx - 1.3f * k + tri[2*i] * k * 1.3f, cy - 1.3f * k + tri[2*i+1] * k * 1.3f, 0.f, 0xFF000000u };
            verts[nv + i] = { cx + tri[2*i] * k, cy + tri[2*i+1] * k, 0.f, 0xFFFFFFFFu };
        }
        g_origDrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, (UINT)(2 * nv / 3), verts, sizeof(CursorVertex));
    }

    g_origSetScissor(dev, &savedScissorRc);
    g_origSetTransform(dev, D3DTS_WORLD, &mWorld); g_origSetTransform(dev, D3DTS_VIEW, &mView); g_origSetTransform(dev, D3DTS_PROJECTION, &mProj);
    for (int i = 0; i < kT; ++i) dev->SetTextureStageState(kTSS[i].stage, kTSS[i].type, savedTSS[i]);
    for (int i = 0; i < kN; ++i) dev->SetRenderState(kRS[i], savedRS[i]);
    dev->SetFVF(fvf);
    dev->SetVertexShader(vs); if (vs) vs->Release();
    dev->SetPixelShader(ps); if (ps) ps->Release();
    dev->SetTexture(0, tex0); if (tex0) tex0->Release();
    g_origSetViewport(dev, &g_devVP);
}

// End of frame, hud_anchor=world: draw the finished HUD texture as a flat panel per eye, fixed in the game's frame.
struct HudVertex { float x, y, z, u, v; };
static void VRCompositeHud(IDirect3DDevice9* dev)
{
    OwnRS ownRS;                              // our SetRenderState calls are not the game's
    const bool had = g_hudUsed;
    g_hudUsed = false; g_hudClearedThisFrame = false;
    if (!HudAsTexture() || !g_hudSurf) { if (g_hudBound) VRHudUnbind(dev); return; }
    if (g_hudBound) VRHudUnbind(dev);                                  // the main target is current again
    if (!had) return;
    const int rtW = (int)(g_curRTw ? g_curRTw : g_bbW), rtH = (int)(g_curRTh ? g_curRTh : g_bbH);
    if (rtW < 64 || rtH < 64) return;

    static const D3DRENDERSTATETYPE kRS[] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_CULLMODE,
        D3DRS_LIGHTING, D3DRS_ALPHATESTENABLE, D3DRS_STENCILENABLE, D3DRS_SCISSORTESTENABLE, D3DRS_FOGENABLE, D3DRS_COLORWRITEENABLE,
        D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_FILLMODE, D3DRS_SPECULARENABLE };
    constexpr int kN = (int)(sizeof(kRS) / sizeof(kRS[0]));
    static const DWORD kRSval[kN] = { FALSE, FALSE, TRUE, D3DBLEND_ONE, D3DBLEND_INVSRCALPHA, D3DCULL_NONE,
        FALSE, FALSE, FALSE, FALSE, FALSE, 0xF, FALSE, D3DFILL_SOLID, FALSE };
    static const struct { DWORD stage; D3DTEXTURESTAGESTATETYPE type; DWORD val; } kTSS[] = {
        { 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1 }, { 0, D3DTSS_COLORARG1, D3DTA_TEXTURE },
        { 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1 }, { 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE },
        { 0, D3DTSS_TEXCOORDINDEX, 0 }, { 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE }, { 1, D3DTSS_COLOROP, D3DTOP_DISABLE } };
    constexpr int kT = (int)(sizeof(kTSS) / sizeof(kTSS[0]));
    static const struct { D3DSAMPLERSTATETYPE type; DWORD val; } kSS[] = {
        { D3DSAMP_MINFILTER, D3DTEXF_LINEAR }, { D3DSAMP_MAGFILTER, D3DTEXF_LINEAR }, { D3DSAMP_MIPFILTER, D3DTEXF_NONE },
        { D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP }, { D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP } };
    constexpr int kS = (int)(sizeof(kSS) / sizeof(kSS[0]));
    DWORD savedRS[kN], savedTSS[kT], savedSS[kS];
    for (int i = 0; i < kN; ++i) { savedRS[i] = 0; dev->GetRenderState(kRS[i], &savedRS[i]); }
    for (int i = 0; i < kT; ++i) { savedTSS[i] = 0; dev->GetTextureStageState(kTSS[i].stage, kTSS[i].type, &savedTSS[i]); }
    for (int i = 0; i < kS; ++i) { savedSS[i] = 0; dev->GetSamplerState(0, kSS[i].type, &savedSS[i]); }
    IDirect3DBaseTexture9* tex0 = nullptr; dev->GetTexture(0, &tex0);
    DWORD fvf = 0; dev->GetFVF(&fvf);
    IDirect3DVertexShader9* vs = nullptr; dev->GetVertexShader(&vs);
    IDirect3DPixelShader9* ps = nullptr; dev->GetPixelShader(&ps);
    D3DMATRIX mWorld, mView, mProj;
    dev->GetTransform(D3DTS_WORLD, &mWorld); dev->GetTransform(D3DTS_VIEW, &mView); dev->GetTransform(D3DTS_PROJECTION, &mProj);
    RECT savedScissorRc{}; dev->GetScissorRect(&savedScissorRc);

    dev->SetVertexShader(nullptr); dev->SetPixelShader(nullptr); dev->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1); dev->SetTexture(0, g_hudTex);
    for (int i = 0; i < kN; ++i) dev->SetRenderState(kRS[i], kRSval[i]);
    for (int i = 0; i < kT; ++i) dev->SetTextureStageState(kTSS[i].stage, kTSS[i].type, kTSS[i].val);
    for (int i = 0; i < kS; ++i) dev->SetSamplerState(0, kSS[i].type, kSS[i].val);
    D3DMATRIX ident{}; ident._11 = ident._22 = ident._33 = ident._44 = 1.f;
    g_origSetTransform(dev, D3DTS_WORLD, &ident); g_origSetTransform(dev, D3DTS_VIEW, &ident);

    const float uiAspect = g_logH ? (float)g_logW / (float)g_logH : (g_bbH ? (float)g_bbW / (float)g_bbH : 2.37f);
    const vrm::Mat3 q = (g_headOn.load() && g_frameQValid) ? g_frameQ : vrm::identity();
    const vrm::V3 hp = (g_positional && g_headOn.load() && g_frameQValid) ? g_headDeltaCam : vrm::V3{ 0, 0, 0 };
    for (int eye = 0; eye < 2; ++eye) {
        float c[4][3];
        if (!vrm::hudQuadEyeSpace(vrm::eyeOffsetCamera(g_eyeToHead[eye]), q, hp, g_hudDistance, g_hudWidth, uiAspect, g_hudVert, 0.05f, c)) continue;
        int x, y, w, h; EyeRegion(eye, rtW, rtH, &x, &y, &w, &h);
        SetVP(dev, x, y, w, h);
        float pm[4][4];
        vrm::perspFromFrustum(g_eyeFrustum[eye][0], g_eyeFrustum[eye][1], g_eyeFrustum[eye][2], g_eyeFrustum[eye][3], 0.05f, 100.f, pm);
        D3DMATRIX proj; memcpy(&proj, pm, sizeof(proj));
        g_origSetTransform(dev, D3DTS_PROJECTION, &proj);
        const HudVertex v[4] = { { c[0][0], c[0][1], c[0][2], 0.f, 0.f }, { c[1][0], c[1][1], c[1][2], 1.f, 0.f },
                                 { c[2][0], c[2][1], c[2][2], 0.f, 1.f }, { c[3][0], c[3][1], c[3][2], 1.f, 1.f } };
        g_origDrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, v, sizeof(HudVertex));
    }

    g_origSetScissor(dev, &savedScissorRc);
    g_origSetTransform(dev, D3DTS_WORLD, &mWorld); g_origSetTransform(dev, D3DTS_VIEW, &mView); g_origSetTransform(dev, D3DTS_PROJECTION, &mProj);
    for (int i = 0; i < kS; ++i) dev->SetSamplerState(0, kSS[i].type, savedSS[i]);
    for (int i = 0; i < kT; ++i) dev->SetTextureStageState(kTSS[i].stage, kTSS[i].type, savedTSS[i]);
    for (int i = 0; i < kN; ++i) dev->SetRenderState(kRS[i], savedRS[i]);
    dev->SetFVF(fvf);
    dev->SetVertexShader(vs); if (vs) vs->Release();
    dev->SetPixelShader(ps); if (ps) ps->Release();
    dev->SetTexture(0, tex0); if (tex0) tex0->Release();
    g_origSetViewport(dev, &g_devVP);
}

// GPU path only. Right before the copy: (1) make sure the game's MAIN target is the one bound, so that dgVoodoo's D3D11
// context really has it bound when we ask (dgVoodoo may apply D3D9 state lazily, at the next draw), and (2) write alpha = 1
// over the whole picture on the GPU - the CPU path used to do that per pixel in CopyOpaque, and the compositor must not see
// stray alpha left by the game's UI blending. One full-screen alpha-only quad; costs well under a millisecond.
struct PreVertex { float x, y, z, rhw; DWORD color; };
static void VRPrepareMainTarget(IDirect3DDevice9* dev)
{
    OwnRS ownRS;                              // our SetRenderState calls are not the game's
    IDirect3DSurface9* mainRT = nullptr; bool ownRef = false;
    if (g_redirect && g_vrRT) mainRT = g_vrRT;
    else { if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &mainRT)) || !mainRT) return; ownRef = true; }
    D3DSURFACE_DESC d{}; mainRT->GetDesc(&d);
    IDirect3DSurface9* cur = nullptr; dev->GetRenderTarget(0, &cur);
    if (cur != mainRT) g_origSetRenderTarget(dev, 0, mainRT);                  // not restored: the game sets its targets again next frame
    if (cur) cur->Release();
    if (ownRef) mainRT->Release();
    if (d.Width < 64 || d.Height < 64) return;

    static const D3DRENDERSTATETYPE kRS[] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_CULLMODE, D3DRS_LIGHTING,
        D3DRS_ALPHATESTENABLE, D3DRS_STENCILENABLE, D3DRS_SCISSORTESTENABLE, D3DRS_FOGENABLE, D3DRS_COLORWRITEENABLE, D3DRS_SEPARATEALPHABLENDENABLE };
    constexpr int kN = (int)(sizeof(kRS) / sizeof(kRS[0]));
    static const DWORD kRSval[kN] = { FALSE, FALSE, FALSE, D3DCULL_NONE, FALSE, FALSE, FALSE, FALSE, FALSE, D3DCOLORWRITEENABLE_ALPHA, FALSE };
    static const struct { DWORD stage; D3DTEXTURESTAGESTATETYPE type; DWORD val; } kTSS[] = {
        { 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1 }, { 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE },
        { 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1 }, { 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE }, { 1, D3DTSS_COLOROP, D3DTOP_DISABLE } };
    constexpr int kT = (int)(sizeof(kTSS) / sizeof(kTSS[0]));
    DWORD savedRS[kN], savedTSS[kT];
    for (int i = 0; i < kN; ++i) { savedRS[i] = 0; dev->GetRenderState(kRS[i], &savedRS[i]); }
    for (int i = 0; i < kT; ++i) { savedTSS[i] = 0; dev->GetTextureStageState(kTSS[i].stage, kTSS[i].type, &savedTSS[i]); }
    IDirect3DBaseTexture9* tex0 = nullptr; dev->GetTexture(0, &tex0);
    DWORD fvf = 0; dev->GetFVF(&fvf);
    IDirect3DVertexShader9* vs = nullptr; dev->GetVertexShader(&vs);
    IDirect3DPixelShader9* ps = nullptr; dev->GetPixelShader(&ps);

    dev->SetVertexShader(nullptr); dev->SetPixelShader(nullptr); dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE); dev->SetTexture(0, nullptr);
    for (int i = 0; i < kN; ++i) dev->SetRenderState(kRS[i], kRSval[i]);
    for (int i = 0; i < kT; ++i) dev->SetTextureStageState(kTSS[i].stage, kTSS[i].type, kTSS[i].val);
    SetVP(dev, 0, 0, (int)d.Width, (int)d.Height);
    const float x0 = -0.5f, y0 = -0.5f, x1 = (float)d.Width - 0.5f, y1 = (float)d.Height - 0.5f;
    const DWORD c = 0xFF000000u;
    const PreVertex quad[6] = { {x0, y0, 0.f, 1.f, c}, {x1, y0, 0.f, 1.f, c}, {x0, y1, 0.f, 1.f, c}, {x1, y0, 0.f, 1.f, c}, {x1, y1, 0.f, 1.f, c}, {x0, y1, 0.f, 1.f, c} };
    g_origDrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, quad, sizeof(PreVertex));

    for (int i = 0; i < kT; ++i) dev->SetTextureStageState(kTSS[i].stage, kTSS[i].type, savedTSS[i]);
    for (int i = 0; i < kN; ++i) dev->SetRenderState(kRS[i], savedRS[i]);
    dev->SetFVF(fvf);
    dev->SetVertexShader(vs); if (vs) vs->Release();
    dev->SetPixelShader(ps); if (ps) ps->Release();
    dev->SetTexture(0, tex0); if (tex0) tex0->Release();
    g_origSetViewport(dev, &g_devVP);
}

// DXVK hand-off: submit this frame's side-by-side picture to SteamVR as one Vulkan image (left/right half via bounds).
// Same sequence as Proton's vrclient for D3D11-on-DXVK games. Returns false (and the caller falls back) if it cannot.
static bool VRSubmitVulkan(IDirect3DDevice9* dev, EVRCompositorError* e0, EVRCompositorError* e1)
{
    VRPrepareMainTarget(dev);                 // alpha = 1 over the picture, and the main target bound
    IDirect3DSurface9* src = nullptr; bool own = false;
    if (g_redirect && g_vrRT) src = g_vrRT;
    else { if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &src)) || !src) { g_vkWhy = "GetBackBuffer failed"; return false; } own = true; }
    vkp::Frame f; const char* why = "";
    bool ok = g_vk.Describe(src, &f, &why);
    if (!ok) {                                // e.g. the real back buffer, or a plain render target: copy into our own texture
        IDirect3DSurface9* c = g_vk.CopyIfNeeded(dev, src, g_origStretchRect, Log);
        ok = c && g_vk.Describe(c, &f, &why);
        if (!c) why = "copy into an own texture failed";
    }
    if (own) src->Release();
    if (!ok) { g_vkWhy = why; return false; }
    if (!g_vk.explicitTimingSet) {
        g_vr.SetExplicitTiming(EVRCompositorTimingMode_VRCompositorTimingMode_Explicit_ApplicationPerformsPostPresentHandoff);
        g_vk.explicitTimingSet = true;
        Log("VK path: explicit timing mode set (WaitGetPoses no longer uses DXVK's queue; PostPresentHandoff runs under the queue lock)\n");
    }
    const vkp::VkImageSubresourceRange range = { vkp::ASPECT_COLOR, 0, f.mips, 0, f.layers };
    g_vk.dev->TransitionTextureLayout(f.tex, &range, f.layout, vkp::LAYOUT_TRANSFER_SRC_OPTIMAL);
    g_vk.dev->FlushRenderingCommands();
    g_vk.dev->LockSubmissionQueue();          // no D3D9 calls from here until ReleaseSubmissionQueue
    VRVulkanTextureData_t vd{};
    vd.m_nImage = f.image;
    vd.m_pDevice = reinterpret_cast<struct VkDevice_T*>(g_vk.device);
    vd.m_pPhysicalDevice = reinterpret_cast<struct VkPhysicalDevice_T*>(g_vk.phys);
    vd.m_pInstance = reinterpret_cast<struct VkInstance_T*>(g_vk.inst);
    vd.m_pQueue = reinterpret_cast<struct VkQueue_T*>(g_vk.queue);
    vd.m_nQueueFamilyIndex = g_vk.queueFamily;
    vd.m_nWidth = f.width; vd.m_nHeight = f.height; vd.m_nFormat = f.format; vd.m_nSampleCount = f.samples;
    *e0 = g_vr.SubmitVulkan(0, &vd, 0.f, 0.5f);
    *e1 = g_vr.SubmitVulkan(1, &vd, 0.5f, 1.f);
    g_vk.dev->ReleaseSubmissionQueue();
    g_vk.dev->TransitionTextureLayout(f.tex, &range, vkp::LAYOUT_TRANSFER_SRC_OPTIMAL, f.layout);
    if (!g_vkActive || g_vkFrames == 0)
        Log("VK path: ACTIVE - submitting VkImage 0x%llx %ux%u (VkFormat %u, layout %d, usage 0x%x) straight to SteamVR, both eyes from one image\n",
            (unsigned long long)f.image, f.width, f.height, f.format, (int)f.layout, f.usage);
    ++g_vkFrames; g_vkActive = true;
    return true;
}

static void VRStop(IDirect3DDevice9* dev, const char* why)
{
    Log("VR stopping (%s)\n", why);
    RestoreCamera();
    g_vrActive = false; g_vrHavePose = false; g_eyeParamsOk = false;
    g_headOn = false; g_frameQValid = false;
    g_vrHaveEye[0] = g_vrHaveEye[1] = false;
    VRReleaseCapture();
    gpu::ReleaseEyes();
    g_vk.ReleaseCopy(); g_vk.explicitTimingSet = false; g_vkActive = false;
    VRRedirectEnd(dev);
    g_vrDisplay.Shutdown();
    g_vr.Shutdown();
}

static bool VRStart(IDirect3DDevice9* dev)
{
    Log("VR starting...\n");
    if (!g_vr.Init(ModuleDir().c_str(), Log, EVRApplicationType_VRApplication_Scene)) return false;
    for (int e = 0; e < 2; ++e) {
        float l, r, t, b, rot[9]; g_vr.EyeParams(e, &l, &r, &t, &b, g_eyeToHead[e], rot);
        {
            const float tr = rot[0] + rot[4] + rot[8];
            float c = (tr - 1.f) * 0.5f; c = c > 1.f ? 1.f : (c < -1.f ? -1.f : c);
            Log("VR eye %d: eye-to-head ROTATION rows (%.4f %.4f %.4f) (%.4f %.4f %.4f) (%.4f %.4f %.4f) = %.2f degrees from identity (non-zero means canted panels, which this build ignores)\n",
                e, rot[0], rot[1], rot[2], rot[3], rot[4], rot[5], rot[6], rot[7], rot[8], std::acos(c) * 57.29578f);
        }
        vrm::openvrRawToFrustum(l, r, t, b, g_eyeFrustum[e]);
        Log("VR eye %d: OpenVR raw tangents left=%.4f right=%.4f top=%.4f bottom=%.4f (OpenVR names top/bottom BACKWARDS) -> WAR frustum x0=%.4f x1=%.4f y0(up)=%.4f y1(down)=%.4f; eye offset (head space) = (%.4f %.4f %.4f) m\n",
            e, l, r, t, b, g_eyeFrustum[e][0], g_eyeFrustum[e][1], g_eyeFrustum[e][2], g_eyeFrustum[e][3],
            g_eyeToHead[e][0], g_eyeToHead[e][1], g_eyeToHead[e][2]);
    }
    Log("VR: headset display frequency = %.1f Hz\n", g_vr.DisplayFrequency());
    uint32_t rw = 0, rh = 0; g_vr.RecommendedSize(&rw, &rh);
    g_wantRTW = g_wantRTH = 0;                          // forget a size from an earlier, failed start
    {
        // Is the GPU hand-off possible? Only then is a huge render target worth it: the CPU path squeezes every eye down to
        // eye_size (default 1600 high) before reading it back, so the extra pixels would be thrown away.
        const bool gpuOk = (g_gpuPathWanted && gpu::HaveGame()) || (g_vkPathWanted && g_vk.Attached());
        if (g_vkPathWanted && g_vk.Attached()) Log("VK path: DXVK is the D3D9 layer; eye pictures go to SteamVR as Vulkan images (no copy, no CPU readback)\n");
        const bool wantHiRes = g_resMode == 1 || (g_resMode == 2 && gpuOk);
        Log("VR: resolution mode=%s, GPU hand-off available=%d (%d D3D11 candidate(s)) -> %s\n", g_resMode == 1 ? "steamvr" : (g_resMode == 0 ? "desktop" : "auto"),
            gpuOk ? 1 : 0, gpu::CandidateCount(), wantHiRes ? "rendering at SteamVR's size" : "rendering at the game's own resolution");
        if (g_resMode == 2 && !gpuOk)
            Log("VR: (auto) staying at the game's resolution because there is no GPU hand-off; see the GPU path messages above. vr_resolution=steamvr forces the big target anyway.\n");
        if (wantHiRes) {
            unsigned ew = 0, eh = 0;
            vrm::fitEyeSize(rw, rh, g_maxPixels, g_renderScale, &ew, &eh);
            Log("VR: SteamVR recommends %ux%u per eye (%.1f Mpx for both eyes). Pixel budget %s, render_scale %.2f -> asking for %ux%u per eye (%.1f Mpx for both)\n",
                rw, rh, 2.0 * rw * rh / 1e6, g_maxPixels > 0 ? "set by max_pixels" : "none", g_renderScale, ew, eh, 2.0 * ew * eh / 1e6);
            UINT tw = 2 * ew, th = eh;
            if (VRRedirectBeginFit(dev, &tw, &th)) {
                g_wantRTW = tw; g_wantRTH = th;
                Log("VR: render target %ux%u = %ux%u per eye (SteamVR asked for %ux%u)%s\n", tw, th, tw / 2, th, rw, rh, (tw / 2 == rw && th == rh) ? " - FULL SteamVR resolution" : " - below SteamVR's number, limited by this device (see messages above)");
            }
            else Log("VR: could not create the VR-sized render target; continuing at the game's own resolution\n");
        }
    }
    UINT bbW = g_bbW, bbH = g_bbH;
    if (!bbW || !bbH) {
        IDirect3DSurface9* bb = nullptr;
        if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) { D3DSURFACE_DESC d{}; bb->GetDesc(&d); bbW = d.Width; bbH = d.Height; bb->Release(); }
    }
    if (!bbW || !bbH) { Log("VR: cannot determine back-buffer size\n"); g_vr.Shutdown(); return false; }
    UINT eyeW, eyeH;
    if (g_stereoSBS) {
        // each eye gets half of the back buffer; eye texture keeps that half's aspect, height from eye_size
        eyeH = g_eyeSizeCfg ? g_eyeSizeCfg : (bbH < 1600 ? bbH : 1600);
        if (eyeH < 256) eyeH = 256;
        if (eyeH > 4096) eyeH = 4096;
        eyeW = (UINT)((double)eyeH * (double)(bbW / 2) / (double)bbH + 0.5);
        eyeW &= ~1u;
        if (eyeW > 4096) eyeW = 4096;
    } else {
        eyeH = eyeW = g_eyeSizeCfg ? g_eyeSizeCfg : (bbH < 1600 ? bbH : 1600);
        if (eyeH < 512) eyeH = eyeW = 512;
        if (eyeH > 4096) eyeH = eyeW = 4096;
    }
    Log("VR: mode=%s hud=%s; back buffer %ux%u; recommended per-eye render size %ux%u; using eye textures %ux%u\n",
        g_stereoSBS ? "SIMULTANEOUS (side-by-side, both eyes every frame)" : "ALTERNATE-EYE", g_hudMode == 0 ? "floating 3D" : "fill",
        bbW, bbH, rw, rh, eyeW, eyeH);
    if (!g_vrDisplay.Init(g_vr.AdapterIndex(), eyeW, eyeH, Log)) { VRRedirectEnd(dev); g_vr.Shutdown(); return false; }
    gpu::ForgetDevice(g_vrDisplay.device());           // our own device went through the hooked creation function too
    if (g_gpuPathWanted) {
        void** vt = nullptr;
        if (gpu::InstallHooks(g_vrDisplay.device(), g_vrDisplay.context(), Log, &vt))
            Log("GPU path: hooks installed in the D3D11 context vtable %p (%s); waiting to learn the game's D3D11 context\n", (void*)vt, Describe(vt).c_str());
    }
    g_vrFrameNo = 0; g_gpuFrames = g_gpuFails = g_gpuFailStreak = 0; g_gpuActive = false;
    g_vkActive = false; g_vkSubmitErrStreak = 0;
    if (!VRCreateCapture(dev)) {
        if ((g_gpuPathWanted && gpu::HaveGame()) || (g_vkPathWanted && g_vk.Attached()))   // not needed while the GPU hand-off works; created lazily if the CPU path is ever used
            Log("VR: could not create the CPU-path capture surfaces (low video memory); continuing, the GPU path does not need them\n");
        else { g_vrDisplay.Shutdown(); VRRedirectEnd(dev); g_wantRTW = g_wantRTH = 0; g_vr.Shutdown(); return false; }
    }
    vrm::unionFrustum(g_eyeFrustum[0], g_eyeFrustum[1], 0.05f, g_unionFrustum);
    Log("VR: culling frustum (union of both eyes + 5%%) x0=%.4f x1=%.4f y0=%.4f y1=%.4f\n", g_unionFrustum[0], g_unionFrustum[1], g_unionFrustum[2], g_unionFrustum[3]);
    g_eyeParamsOk = true;
    g_havePrevPose = false; g_wornPrev = false; g_autoRecenterPending = false; g_stillSinceMs = -1;
    g_vrEye = 0; g_vrPairs = g_vrCapFail = g_vrSubmitErr = 0; g_vrCapMsSum = 0; g_vrFramesInPeriod = 0;
    g_haveGameVP = false; g_rightVersion = ~0u; g_projCheckLogged = false;
    g_haveRef = false; g_recenterRequest = true; g_headOn = true;
    // First pose. Blocks until the compositor is ready for a frame (can take a moment while SteamVR starts).
    EVRCompositorError e = g_vr.WaitPoses(&g_vrPose);
    g_vrHavePose = g_vrPose.valid;
    Log("VR: first WaitGetPoses -> err=%d, pose valid=%d\n", (int)e, g_vrHavePose ? 1 : 0);
    g_vrActive = true;
    g_captureCountdown = 150;                  // log one VR frame automatically a couple of seconds in
    Log("VR: ACTIVE. Units per metre = %.1f (PageUp/PageDown to calibrate), positional tracking %s\n", g_unitsPerMeter, g_positional ? "on" : "off");
    return true;
}

// Called from HookPresent BEFORE the real Present (the back buffer still holds this frame).
static void VRBeforePresent(IDirect3DDevice9* dev)
{
    if (g_vrWanted.load() != g_vrActive) {
        if (g_vrWanted.load()) { if (!VRStart(dev)) { g_vrWanted = false; BeepAsync(300, 400); Log("VR failed to start (see messages above)\n"); } else BeepAsync(1400, 150); }
        else { VRStop(dev, "requested"); BeepAsync(500, 150); }
        return;
    }
    if (!g_vrActive) return;
    if (g_wantRTW && !g_redirect) { UINT w = g_wantRTW, h = g_wantRTH; VRRedirectBeginFit(dev, &w, &h); }   // recreate after a device reset
    const double t0 = NowMs();
    VRDrawCursor(dev);
    VRCompositeHud(dev);                      // hud_anchor=world: the panel goes into the picture now, after everything else
    if (g_stereoSBS) {
        // Both eyes were drawn into the two halves of this frame's main render target.
        void* texL = nullptr; void* texR = nullptr;
        bool vkDone = false;
        EVRCompositorError e0 = EVRCompositorError_VRCompositorError_None, e1 = EVRCompositorError_VRCompositorError_None;
        if (g_vkPathWanted && g_vk.Attached()) {
            vkDone = VRSubmitVulkan(dev, &e0, &e1);
            if (!vkDone) { ++g_vkFails; g_vkActive = false; if (g_vkFails <= 3) Log("VK path: not usable this frame (%s) -> fallback\n", g_vkWhy); }
        }
        if (vkDone) {
        } else if (g_gpuPathWanted && gpu::HaveGame()) {
            // GPU path: copy each half on the GPU into textures on the game's own D3D11 device. No CPU involved.
            const int set = (int)(g_vrFrameNo & 1);
            VRPrepareMainTarget(dev);
            const gpu::CopyResult r = gpu::CopyEyes(set, g_bbW, g_bbH);
            if (r.ok) {
                texL = gpu::Eye(set, 0); texR = gpu::Eye(set, 1);
                if (!g_gpuActive || g_gpuFrames == 0) Log("GPU path: ACTIVE - copying %ux%u (format %d) straight from the game's render target, no CPU readback\n", r.srcW, r.srcH, (int)r.srcFmt);
                ++g_gpuFrames; g_gpuFailStreak = 0; g_gpuActive = true;
            } else {
                ++g_gpuFails; ++g_gpuFailStreak; g_gpuActive = false;
                if (g_gpuWhy != r.why || g_gpuFails <= 3) Log("GPU path: not usable this frame (%s; bound target %ux%u format %d, expected shape %ux%u) -> CPU fallback\n", r.why, r.srcW, r.srcH, (int)r.srcFmt, g_bbW, g_bbH);
                g_gpuWhy = r.why;
            }
        } else if (g_gpuPathWanted && g_vrFrameNo == 240 && !g_vk.Attached()) {
            {
                Log("GPU path: no D3D11 device was ever handed to the game's translation layer (creation hook calls=%u captured=%u, vtable hook hits=%u, creation hooks installed=%d). Using the CPU path.\n",
                    gpu::g_s.createCalls, gpu::g_s.createCaptured, gpu::g_s.hits, gpu::g_s.createHooked ? 1 : 0);
                if (GetModuleHandleW(L"d3d12.dll"))
                    Log("GPU path: d3d12.dll IS loaded in this process. dgVoodoo is most likely using its DIRECT3D 12 output; there is no D3D11 device to copy from. "
                        "Open dgVoodooCpl.exe -> DirectX tab -> Output API and choose Direct3D 11 (feature level 11.0), then restart the game.\n");
                else if (gpu::g_s.createCalls == 0)
                    Log("GPU path: the creation hooks saw no D3D11CreateDevice call at all. Either WARVR.dll was injected after the game's device already existed (inject earlier / start the injector first), "
                        "or dgVoodoo is not the D3D9 layer in use.\n");
            }
        }
        if (!texL && !vkDone) {                             // CPU path (always available as a fallback)
            IDirect3DSurface9* bb = nullptr; D3DSURFACE_DESC d{};
            if (g_redirect && g_vrRT) g_vrRT->GetDesc(&d);
            else {
                if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) { ++g_vrCapFail; return; }
                bb->GetDesc(&d); bb->Release();
            }
            const LONG hw = (LONG)(d.Width / 2), hh = (LONG)d.Height;
            RECT rl = { 0, 0, hw, hh }, rr = { hw, 0, 2 * hw, hh };
            if (!VRCaptureEye(dev, 0, &rl) || !VRCaptureEye(dev, 1, &rr)) { ++g_vrCapFail; return; }
            g_vrDisplay.Flush();
            texL = g_vrDisplay.eyeTexture(0); texR = g_vrDisplay.eyeTexture(1);
        }
        g_vrCapMsSum += NowMs() - t0; ++g_vrFramesInPeriod;
        if (!vkDone) { e0 = g_vr.Submit(0, texL); e1 = g_vr.Submit(1, texR); }
        if (e0 || e1) ++g_vrSubmitErr;
        if (g_vrPairs < 5 || e0 || e1) {
            static unsigned errLogs = 0;
            if (g_vrPairs < 5 || errLogs++ < 20) Log("VR: submit pair %u (%s) -> left err=%d right err=%d\n", g_vrPairs, vkDone ? "VK" : (g_gpuActive ? "GPU" : "CPU"), (int)e0, (int)e1);
        }
        if (g_snapshotAt && g_vrPairs + 1 == g_snapshotAt) VRSnapshot(dev, g_vrPairs + 1);
        if (vkDone && (e0 || e1)) {
            if (++g_vkSubmitErrStreak >= 5) {           // SteamVR refuses the Vulkan images (e.g. missing extensions): use the CPU path
                g_vkPathWanted = false; g_vkActive = false; g_vk.ReleaseCopy();
                Log("VK path: SteamVR rejected the Vulkan eye images 5 times in a row (err=%d/%d) -> switching to the CPU path for this session. "
                    "Usual cause: SteamVR was not running when the game started, so DXVK could not enable the Vulkan extensions SteamVR needs.\n", (int)e0, (int)e1);
            }
        } else if (vkDone) g_vkSubmitErrStreak = 0;
        if (g_gpuActive && (e0 || e1)) {
            if (++g_gpuSubmitErrStreak >= 5) {          // SteamVR refuses the game's textures: go back to the CPU path for good
                g_gpuPathWanted = false; g_gpuActive = false; gpu::ReleaseEyes();
                Log("GPU path: SteamVR rejected the GPU eye textures 5 times in a row (err=%d/%d) -> switching to the CPU path for this session\n", (int)e0, (int)e1);
            }
        } else g_gpuSubmitErrStreak = 0;
        ++g_vrPairs; ++g_vrFrameNo;
        return;                                            // g_vrEye stays 0: a new pose is fetched after every present
    }
    const int eye = g_vrEye;
    if (!VRCaptureEye(dev, eye, nullptr)) { ++g_vrCapFail; return; }
    g_vrCapMsSum += NowMs() - t0;
    ++g_vrFramesInPeriod;
    if (eye == 1 && g_vrHaveEye[0] && g_vrHaveEye[1]) {
        g_vrDisplay.Flush();
        EVRCompositorError e0 = g_vr.Submit(0, g_vrDisplay.eyeTexture(0));
        EVRCompositorError e1 = g_vr.Submit(1, g_vrDisplay.eyeTexture(1));
        if (e0 || e1) ++g_vrSubmitErr;
        if (g_vrPairs < 5 || e0 || e1) {
            static unsigned errLogs = 0;
            if (g_vrPairs < 5 || errLogs++ < 20) Log("VR: submit pair %u -> left err=%d right err=%d\n", g_vrPairs, (int)e0, (int)e1);
        }
        ++g_vrPairs;
    }
    g_vrEye ^= 1;
}

// Called after the real Present. When a new left/right pair begins, wait for the compositor and take its pose.
static void VRAfterPresent()
{
    if (!g_vrActive) return;
    if (g_vrEye == 0) {
        const int64_t tw = g_log ? PerfTick() : 0;
        if (g_vkActive && g_vk.Attached()) {      // SteamVR uses DXVK's queue here
            g_vk.dev->LockSubmissionQueue(); g_vr.PostPresentHandoff(); g_vk.dev->ReleaseSubmissionQueue();
        } else g_vr.PostPresentHandoff();
        OpenVRHead::Pose pose;
        EVRCompositorError e = g_vr.WaitPoses(&pose);
        if (g_log) g_pf[PF_WAIT] += PerfTick() - tw;
        if (e == EVRCompositorError_VRCompositorError_None && pose.valid) {
            // A tracking-space reset (or putting the headset on after VR started with it on the desk) shows up as a
            // jump; without recentering the view would point away from everything the game drew.
            float yaw = 0.f;
            if (vrm::headYaw(pose.rot, &yaw)) {
                if (g_havePrevPose && vrm::poseJumped(g_prevPosePos, pose.pos, g_prevPoseYaw, yaw)) {
                    g_recenterRequest = true;
                    Log("VR: tracking jump detected (headset put on, or a SteamVR reset) -> recentering\n");
                    BeepAsync(1000, 80);
                }
                g_prevPoseYaw = yaw; for (int i = 0; i < 3; ++i) g_prevPosePos[i] = pose.pos[i]; g_havePrevPose = true;
            }
            g_vrPose = pose; g_vrHavePose = true;
        } else { static unsigned n = 0; if (n++ < 10) Log("VR: WaitGetPoses err=%d valid=%d\n", (int)e, pose.valid ? 1 : 0); }

        // Head speed between the last two poses (used to wait until you are looking steadily before recentering).
        {
            const double now = NowMs();
            if (g_vrPose.valid && g_prevPoseMs > 0.0 && now > g_prevPoseMs) {
                const float deg = vrm::headAngleDeg(g_prevHeadRot, g_vrPose.rot);
                const float dps = deg / (float)((now - g_prevPoseMs) * 0.001);
                if (dps < 20.f) { if (g_stillSinceMs < 0.0) g_stillSinceMs = now; } else g_stillSinceMs = -1.0;
            }
            g_prevHeadRot = g_vrPose.rot; g_prevPoseMs = now;
        }
        const int act = g_vr.HmdActivity();
        if (act == 1 && !g_wornPrev && g_autoRecenterOnWorn) {
            g_autoRecenterPending = true; g_wornAtMs = NowMs();
            Log("VR: headset is being worn -> will recenter as soon as you hold still, looking straight ahead (max 8 s)\n");
            BeepAsync(800, 60);
        }
        if (act != -1) g_wornPrev = (act == 1);
        if (g_autoRecenterPending) {
            const double now = NowMs();
            if ((g_stillSinceMs >= 0.0 && now - g_stillSinceMs >= 900.0) || now - g_wornAtMs >= 8000.0) {
                g_autoRecenterPending = false; g_recenterRequest = true;
                BeepAsync(1000, 100); Log("VR: auto-recenter (head still for %.1f s)\n", g_stillSinceMs >= 0.0 ? (now - g_stillSinceMs) * 0.001 : 0.0);
            }
        }
    }
}

// ---------------------------------------------------------------- frame hooks
static HRESULT WINAPI HookBeginScene(IDirect3DDevice9* self)
{
    ++g_stats.scenes;
    g_frameBestValid = false;
    g_rtIsMain = true;                       // the device RT is the back buffer between frames
    g_curRTw = g_bbW; g_curRTh = g_bbH;
    g_framePerspDraws = 0; g_frameMainPerspDraws = 0;
    PollHead();
    if (!g_log) g_captureRequest = false;                    // no log file (log=off): nothing to capture into
    if (g_captureRequest.exchange(false)) {
        g_capturing = true; g_captureLines = 0; g_flushEachLine = false; g_stereoCapLogged = 0; g_texLogged = 0; g_regDumped.clear();
        Log("==== CAPTURE BEGIN frame %llu head=%d perturb=%d ====\n", g_frame, g_headOn.load() ? 1 : 0, g_perturb.load() ? 1 : 0);
    }
    Cap("BEGINSCENE");
    return g_origBeginScene(self);
}

static HRESULT WINAPI HookPresent(IDirect3DDevice9* self, const RECT* a, const RECT* b, HWND w, const RGNDATA* d)
{
    { static bool once = false; if (!once) { once = true; Log("THREAD: first Present on thread %lu\n", GetCurrentThreadId()); } }
    if (g_log) { static int64_t last = 0; const int64_t now = PerfTick(); if (g_vrActive && last) { g_pf[PF_FRAME] += now - last; ++g_pfFrames; } last = now; }
    LazyFlush(self);                            // our own drawing below, and the next frame, start from the game's state
    RestoreCamera();                            // give the game its own camera back before it updates it again
    ++g_frame;
    g_reflHandled = false;                    // the reflection camera is re-steered every frame
    if (g_framePerspDraws > 500) ++g_worldFrames; else g_worldFrames = 0;
    if (g_autoVr && g_worldFrames == 150) { g_autoVr = false; g_autoVrRequest = true; Log("AUTO VR: first in-world frames, switching VR on (auto_vr=on)\n"); }
    if (g_log && !g_autoWorldCaptured && g_worldFrames > 120 && !g_capturing) { g_autoWorldCaptured = true; g_captureRequest = true; Log("AUTO capture of the first in-world frame\n"); }
    if (g_captureCountdown > 0 && --g_captureCountdown == 0 && !g_capturing && g_log) { g_captureRequest = true; Log("AUTO capture after toggle\n"); }
    if (g_frameBestValid) { g_mainView = g_frameBestView; g_mainProj = g_frameBestProj; g_mainValid = true; }
    if (g_capturing) {
        Log("==== CAPTURE END (%u lines%s) ====\n", g_captureLines, g_captureLines >= kCaptureMaxLines ? ", TRUNCATED" : "");
        g_capturing = false; g_flushEachLine = true; fflush(g_log);
    }
    if (!g_probesDone && g_frame >= 180) { g_probesDone = true; RunProbes(self); }
    if ((g_frame % 300) == 0) {
        const Stats& s = g_stats;
        Log("FRAME %llu scenes=%u setXform=%u viewsRotated=%u | perspective draws: fixedfunc=%u shader=%u | ortho draws: fixedfunc=%u shader=%u | drawUP=%u | vsConstCalls=%u rtChanges=%u clears=%u | VS c0-c3 vs V*P: transposed=%u straight=%u none=%u patched=%u | head=%d\n",
            g_frame, s.scenes, s.setTransform, s.viewsRotated, s.drawPerspFFP, s.drawPerspVS, s.drawOrthoFFP, s.drawOrthoVS,
            s.drawUP, s.vsConstCalls, s.rtChanges, s.clears, s.vpMatchT, s.vpMatchN, s.vpNoMatch, s.vpPatched, g_headOn.load() ? 1 : 0);
        unsigned shown = 0;
        for (auto it = g_vscHist.begin(); it != g_vscHist.end() && shown < 12; ++it, ++shown)
            Log("   VSCONST c%u x%u : %u calls\n", (unsigned)(it->first >> 32), (unsigned)(it->first & 0xffffffffu), it->second);
        g_vscHist.clear();
        { unsigned n = 0;
          for (auto it = g_callerHist.begin(); it != g_callerHist.end() && n < 12; ++it, ++n) {
              static const char* kinds[] = {"VIEW", "PROJ-perspective", "PROJ-ortho"};
              Log("   SETXFORM %-16s called from %s  x%u\n", kinds[(it->first >> 32) % 3],
                  Describe(reinterpret_cast<void*>(static_cast<uintptr_t>(it->first & 0xffffffffu))).c_str(), it->second);
          } }
        g_callerHist.clear();
        Log("   CAMERA HOOK: steerMode=%s installed=%d SetCamera calls=%u rotated=%u derived-passed-through=%u\n", g_steerMode.load() == 0 ? "camera" : "constants",
            g_cameraHookInstalled ? 1 : 0, g_setCameraCalls, g_setCameraRotated, g_setCameraDerived);
        g_setCameraCalls = g_setCameraRotated = g_setCameraDerived = 0;
        Log("   VR: active=%d pairs=%u captureFailures=%u submitErrors=%u avgCaptureMs=%.2f | culling takeover: on=%d layoutVerified=%d takeovers=%u refused=%u\n",
            g_vrActive ? 1 : 0, g_vrPairs, g_vrCapFail, g_vrSubmitErr, g_vrFramesInPeriod ? g_vrCapMsSum / g_vrFramesInPeriod : 0.0,
            g_cullTakeover.load() ? 1 : 0, g_cam.verified, g_camTakeovers, g_camRefused);
        g_camTakeovers = 0;
        if (g_log && g_pfFrames) {
            static const double qf = []{ LARGE_INTEGER f; QueryPerformanceFrequency(&f); return (double)f.QuadPart; }();
            const double k = 1000.0 / qf / (double)g_pfFrames;
            const double frame = g_pf[PF_FRAME] * k, wait = g_pf[PF_WAIT] * k, gwait = (double)gpu::g_s.waitTicks * k;
            Log("   PERF (ms per frame, %u frames): frame %.2f = %.1f fps | game thread busy %.2f (frame minus WaitGetPoses %.2f minus GPU wait %.2f) | duplicated draws %.2f [D3D draw calls %.2f, constant scanning %.2f, other %.2f] | VRBeforePresent %.2f\n",
                g_pfFrames, frame, frame > 0 ? 1000.0 / frame : 0.0, frame - wait - gwait, wait, gwait,
                g_pf[PF_STEREO] * k, g_pf[PF_ISSUE] * k, g_pf[PF_SCAN] * k, (g_pf[PF_STEREO] - g_pf[PF_ISSUE] - g_pf[PF_SCAN]) * k, g_pf[PF_BEFORE] * k);
            Log("   PERF state calls: %.0f per frame (SetTexture, SetRenderState, vertex/pixel constants), about %.2f ms per frame inside dgVoodoo | shaders analysed VS=%u (refused %u) PS=%u (refused %u) | patches skipped as unused: %.0f per frame\n",
                (double)g_stateCallsTotal / g_pfFrames, g_pf[PF_STATE] * k, g_shaderAnalysed[0], g_shaderRefused[0], g_shaderAnalysed[1], g_shaderRefused[1], (double)g_patchesSkipped / g_pfFrames);
            if (g_lazyState) Log("   PERF lazy_state: restore calls avoided %.1f per frame (the game re-uploaded them), deferred restores done %.1f per frame\n",
                (double)g_lazySavedCalls / g_pfFrames, (double)g_lazyFlushedCalls / g_pfFrames);
            g_lazySavedCalls = g_lazyFlushedCalls = 0;
            g_stateCallsTotal = 0; g_patchesSkipped = 0;
            memset(g_pf, 0, sizeof(g_pf)); g_pfFrames = 0; gpu::g_s.waitTicks = 0;
        }
        Log("   PITCH LOCK: %s\n", g_pitchLock.load() ? "ON (mouse pitch removed from the view)" : "off");
        Log("   DRAW DISTANCE: multiplier x%.2f | last main-camera far plane: game %.0f -> used %.0f world units\n", g_drawDistScale, g_lastFarEngine, g_lastFarUsed);
        Log("   WATER: mode=%s reflection cameras steered=%u (this period) water draws with the symmetric projection=%u | map %p texture %p\n", g_waterMode ? "on" : "off", g_reflCams, g_waterDraws, (void*)g_reflSurf, (void*)g_reflTex);
        g_reflCams = 0; g_waterDraws = 0;
        Log("   EYE CONSTANTS: right-eye draws patched beyond the plain VP=%u | VS patches VP=%u VIEW=%u PROJ=%u VP*bias=%u CAMPOS=%u | PS patches VP=%u VIEW=%u PROJ=%u VP*bias=%u CAMPOS=%u | HUD anchor=%s\n",
            g_extraPatchDraws, g_patchStats[0], g_patchStats[1], g_patchStats[2], g_patchStats[3], g_patchStats[4],
            g_patchStatsPS[0], g_patchStatsPS[1], g_patchStatsPS[2], g_patchStatsPS[3], g_patchStatsPS[4], g_hudAnchor == 2 ? "world panel (texture + quad)" : (g_hudAnchor == 1 ? "billboard" : "head"));
        {
            MEMORYSTATUSEX ms{}; ms.dwLength = sizeof(ms); GlobalMemoryStatusEx(&ms);
            Log("   MEMORY: D3D9 (dgVoodoo) texture memory free=%u MB | this process's address space free=%u of %u MB\n",
                self->GetAvailableTextureMem() / 1048576u, (unsigned)(ms.ullAvailVirtual / 1048576u), (unsigned)(ms.ullTotalVirtual / 1048576u));
        }
        Log("   GPU PATH: wanted=%d active=%d frames=%u fallbacks=%u (last reason: %s) D3D11 candidates=%d (creation calls %u, captured %u, vtable hits %u) | desktop presents skipped=%u | VR render target redirect=%d %ux%u (game thinks %ux%u) | cursor=%d\n",
            g_gpuPathWanted ? 1 : 0, g_gpuActive ? 1 : 0, g_gpuFrames, g_gpuFails, g_gpuWhy, gpu::CandidateCount(), gpu::g_s.createCalls, gpu::g_s.createCaptured, gpu::g_s.hits, g_desktopSkipped,
            g_redirect ? 1 : 0, g_bbW, g_bbH, g_logW, g_logH, g_cursorOn ? 1 : 0);
        g_gpuFrames = g_gpuFails = g_desktopSkipped = 0;
        if (g_isDxvk) Log("   VK PATH (DXVK): wanted=%d attached=%d active=%d frames=%u fallbacks=%u (last reason: %s)\n",
            g_vkPathWanted ? 1 : 0, g_vk.Attached() ? 1 : 0, g_vkActive ? 1 : 0, g_vkFrames, g_vkFails, g_vkWhy);
        g_vkFrames = g_vkFails = 0;
        if (g_tSamples) {
            const double n = (double)g_tSamples;
            Log("   VR capture stages (avg ms per eye): stretch=%.2f getRenderTargetData=%.2f lock=%.2f convert=%.2f upload=%.2f\n",
                g_tStretch / n, g_tGetRTD / n, g_tLock / n, g_tConvert / n, g_tUpload / n);
        }
        g_tStretch = g_tGetRTD = g_tLock = g_tConvert = g_tUpload = 0; g_tSamples = 0;
        if (g_vrActive) {
            static Compositor_CumulativeStats prev{}; static bool havePrev = false;
            Compositor_CumulativeStats cur;
            if (g_vr.Stats(&cur)) {
                if (havePrev) {
                    const double pres = (double)(cur.m_nNumFramePresents - prev.m_nNumFramePresents);
                    const double subm = (double)(cur.m_nNumFrameSubmits - prev.m_nNumFrameSubmits);
                    Log("   SteamVR compositor (this period): display refreshes=%.0f, frames this app submitted=%.0f, dropped=%u, REPROJECTED=%u (%.0f%% of refreshes) | "
                        "target frame time sum/refresh=%.4f | app GPU ms per submitted frame=%.2f | compositor GPU ms per refresh=%.2f\n",
                        pres, subm, cur.m_nNumDroppedFrames - prev.m_nNumDroppedFrames, cur.m_nNumReprojectedFrames - prev.m_nNumReprojectedFrames,
                        pres > 0 ? 100.0 * (double)(cur.m_nNumReprojectedFrames - prev.m_nNumReprojectedFrames) / pres : 0.0,
                        pres > 0 ? (cur.m_flSumTargetFrameTimes - prev.m_flSumTargetFrameTimes) / pres : 0.0,
                        subm > 0 ? (cur.m_flSumApplicationGPUTimeMS - prev.m_flSumApplicationGPUTimeMS) / subm : 0.0,
                        pres > 0 ? (cur.m_flSumCompositorGPUTimeMS - prev.m_flSumCompositorGPUTimeMS) / pres : 0.0);
                }
                prev = cur; havePrev = true;
            }
        }
        Log("   STEREO: mode=%s duplicated world draws=%u (fixed-function %u) unmatched/skipped=%u projMismatch=%u | HUD draws=%u fill draws=%u\n",
            g_stereoSBS ? "sbs" : "aer", g_dupWorld, g_dupWorldFFP, g_dupUnmatched, g_projMismatch, g_dupHud, g_dupFill);
        { unsigned n = 0;
          for (auto it = g_unmatchedByVS.begin(); it != g_unmatchedByVS.end() && n < 8; ++it, ++n)
              Log("      unmatched shader %p : %u draws\n", it->first, it->second); }
        g_unmatchedByVS.clear();
        g_dupWorld = g_dupWorldFFP = g_dupUnmatched = g_dupHud = g_dupFill = g_projMismatch = 0;
        g_extraPatchDraws = 0; for (int i = 0; i < 5; ++i) g_patchStats[i] = g_patchStatsPS[i] = 0;
        g_vrCapMsSum = 0; g_vrFramesInPeriod = 0;
        g_stats = Stats{};
    }
    if (g_vrWanted.load() || g_vrActive) { const int64_t tb = g_log ? PerfTick() : 0; VRBeforePresent(self); if (g_log && g_vrActive) g_pf[PF_BEFORE] += PerfTick() - tb; }
    HRESULT presentHr = D3D_OK;
    // While in VR the desktop window is not needed. Presenting to it costs a blit AND waits for the monitor's vertical
    // sync, and that wait then adds to the headset wait (two waits in series), so skip it (desktop_every=N shows every Nth frame).
    if (g_vrActive && !(g_desktopEvery && (g_frame % g_desktopEvery) == 0)) ++g_desktopSkipped;
    else presentHr = g_origPresent(self, a, b, w, d);
    VRAfterPresent();
    return presentHr;
}

static HRESULT WINAPI HookReset(IDirect3DDevice9* self, D3DPRESENT_PARAMETERS* pp)
{
    LogPresentParams("Reset", pp);
    RestoreCamera();
    VRReleaseCapture();                       // default-pool surfaces must not survive a Reset; recreated lazily
    LazyForget();
    g_vk.ReleaseCopy();                       // default-pool texture: must be released before Reset
    gpu::ReleaseEyes();                       // dgVoodoo keeps its D3D11 device across a Reset (phase 7b wrongly forgot it); a NEW device would arrive through the creation hook
    VRRedirectEnd(self);
    g_vsActive = g_psActive = false; g_curFVF = 0; g_havePendingView = false; g_haveOrigView = g_haveOrigProj = false; g_rtIsMain = true; g_haveGameVP = false; g_rightVersion = ~0u;
    HRESULT hr = g_origReset(self, pp);
    Log("Reset -> hr=0x%08lX\n", (unsigned long)hr);
    return hr;
}

static void PatchDevice(IDirect3DDevice9* dev)
{
    if (!dev) return;
    void** vt = *reinterpret_cast<void***>(dev);
    PATCH(vt, SLOT_RESET, HookReset, g_origReset);
    PATCH(vt, SLOT_PRESENT, HookPresent, g_origPresent);
    PATCH(vt, SLOT_SETRENDERTARGET, HookSetRenderTarget, g_origSetRenderTarget);
    PATCH(vt, SLOT_BEGINSCENE, HookBeginScene, g_origBeginScene);
    PATCH(vt, SLOT_CLEAR, HookClear, g_origClear);
    PATCH(vt, SLOT_SETTRANSFORM, HookSetTransform, g_origSetTransform);
    PATCH(vt, SLOT_SETVIEWPORT, HookSetViewport, g_origSetViewport);
    PATCH(vt, SLOT_DRAWPRIM, HookDrawPrimitive, g_origDrawPrimitive);
    PATCH(vt, SLOT_DRAWINDEXEDPRIM, HookDrawIndexedPrimitive, g_origDrawIndexedPrimitive);
    PATCH(vt, SLOT_DRAWPRIMUP, HookDrawPrimitiveUP, g_origDrawPrimitiveUP);
    PATCH(vt, SLOT_DRAWINDEXEDPRIMUP, HookDrawIndexedPrimitiveUP, g_origDrawIndexedPrimitiveUP);
    PATCH(vt, SLOT_SETFVF, HookSetFVF, g_origSetFVF);
    PATCH(vt, SLOT_SETVS, HookSetVertexShader, g_origSetVertexShader);
    PATCH(vt, SLOT_SETPS, HookSetPixelShader, g_origSetPixelShader);
    PATCH(vt, SLOT_CREATEVS, HookCreateVertexShader, g_origCreateVS);
    PATCH(vt, SLOT_CREATEPS, HookCreatePixelShader, g_origCreatePS);
    PATCH(vt, SLOT_SETVSCONSTF, HookSetVSConstF, g_origSetVSConstF);
    PATCH(vt, SLOT_SETDEPTHSTENCIL, HookSetDepthStencil, g_origSetDS);
    PATCH(vt, SLOT_SETSCISSOR, HookSetScissorRect, g_origSetScissor);
    PATCH(vt, SLOT_SETPSCONSTF, HookSetPSConstF, g_origSetPSConstF);
    PATCH(vt, SLOT_SETTEXTURE, HookSetTexture, g_origSetTexture);
    PATCH(vt, SLOT_STRETCHRECT, HookStretchRect, g_origStretchRect);
    PATCH(vt, SLOT_SETRENDERSTATE, HookSetRenderState, g_origSetRenderState);
    Log("DEVICE HOOKED dev=%p vtbl=%p (20 methods)\n", (void*)dev, (void*)vt);
    Log("  device vtable lives in: %s\n", Describe(vt).c_str());
    Log("  real Present() lives in: %s   real SetTransform() lives in: %s\n",
        Describe(reinterpret_cast<void*>(g_origPresent)).c_str(), Describe(reinterpret_cast<void*>(g_origSetTransform)).c_str());
}

static HRESULT WINAPI HookCreateDevice(IDirect3D9* self, UINT adapter, D3DDEVTYPE type, HWND wnd, DWORD flags,
                                       D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out)
{
    LogPresentParams("CreateDevice request", pp);
    if (pp && pp->BackBufferWidth) { g_bbW = g_logW = pp->BackBufferWidth; g_bbH = g_logH = pp->BackBufferHeight; }
    g_gameHwnd = (pp && pp->hDeviceWindow) ? pp->hDeviceWindow : wnd;
    Log("CreateDevice adapter=%u type=%d behaviorFlags=0x%lX\n", adapter, (int)type, (unsigned long)flags);
    HRESULT hr = g_origCreateDevice(self, adapter, type, wnd, flags, pp, out);
    Log("CreateDevice hr=0x%08lX dev=%p\n", (unsigned long)hr, out ? (void*)*out : nullptr);
    if (SUCCEEDED(hr) && out && *out) {
        PatchDevice(*out); InstallCameraHook();
        if (g_isDxvk && g_vkPathWanted && !g_vk.Attach(*out, Log)) Log("VK path: DXVK detected but the device has no interop interface -> CPU path\n");
    }
    return hr;
}

static IDirect3D9* WINAPI HookDirect3DCreate9(UINT sdk)
{
    {
        char path[MAX_PATH] = {};
        g_isDxvk = vkp::ModuleLooksLikeDxvk(reinterpret_cast<const void*>(g_origCreate9), path, sizeof(path));
        Log("D3D9 layer: %s -> %s\n", path[0] ? path : "?", g_isDxvk ? "DXVK" : "not DXVK (dgVoodoo or Windows)");
        // DXVK asks SteamVR for the Vulkan extensions it needs while creating its instance, i.e. inside this call,
        // and only if openvr_api.dll is already loaded. So load it now.
        if (g_isDxvk && g_vkPathWanted) vkp::PreloadOpenVR(ModuleDir().c_str(), Log);
    }
    IDirect3D9* obj = g_origCreate9(sdk);
    Log("Direct3DCreate9(%u) -> %p\n", sdk, (void*)obj);
    if (!obj) return obj;
    void** vt = *reinterpret_cast<void***>(obj);
    PatchVtable(vt, SLOT_CREATEDEVICE, reinterpret_cast<void*>(&HookCreateDevice), reinterpret_cast<void**>(&g_origCreateDevice));
    Log("D3D9 CreateDevice hook installed vtbl=%p\n", (void*)vt);
    Log("  IDirect3D9 vtable lives in: %s   real CreateDevice() lives in: %s\n", Describe(vt).c_str(),
        Describe(reinterpret_cast<void*>(g_origCreateDevice)).c_str());
    return obj;
}

// ---------------------------------------------------------------- WAR loader hook (unchanged from phase 3)
static bool WINAPI HookWARCreate9Loader();

static bool PatchCreate9Slot(uintptr_t slot)
{
    auto current = *reinterpret_cast<Direct3DCreate9_t*>(slot);
    Log("PatchCreate9Slot: slot=%p current=%p\n", (void*)slot, (void*)current);
    if (!current) return false;
    if (current == &HookDirect3DCreate9) return true;

    g_origCreate9 = current;
    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(slot), sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        Log("PatchCreate9Slot: VirtualProtect failed=%lu\n", GetLastError());
        return false;
    }
    *reinterpret_cast<Direct3DCreate9_t*>(slot) = &HookDirect3DCreate9;
    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<void*>(slot), sizeof(void*), oldProtect, &ignored);
    Log("WAR Create9 pointer patched slot=%p original=%p replacement=%p\n",
        (void*)slot, reinterpret_cast<void*>(g_origCreate9), reinterpret_cast<void*>(&HookDirect3DCreate9));
    return true;
}

static bool InstallLoaderHook(uintptr_t loader, uintptr_t slot)
{
    if (g_loaderHooked) return true;
    constexpr SIZE_T kPatchLen = 7;
    BYTE* src = reinterpret_cast<BYTE*>(loader);

    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(src, &mbi, sizeof(mbi))) {
        Log("InstallLoaderHook: VirtualQuery failed=%lu\n", GetLastError());
        return false;
    }
    DWORD prot = mbi.Protect;
    if (!(prot & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
        Log("InstallLoaderHook: loader not executable protect=0x%08lX\n", prot);
        return false;
    }

    BYTE* tramp = reinterpret_cast<BYTE*>(VirtualAlloc(nullptr, kPatchLen + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) {
        Log("InstallLoaderHook: VirtualAlloc failed=%lu\n", GetLastError());
        return false;
    }
    memcpy(tramp, src, kPatchLen);
    tramp[kPatchLen] = 0xE9;
    *reinterpret_cast<int32_t*>(tramp + kPatchLen + 1) =
        static_cast<int32_t>((reinterpret_cast<uintptr_t>(src + kPatchLen)) - (reinterpret_cast<uintptr_t>(tramp + kPatchLen + 5)));

    DWORD oldProtect = 0;
    if (!VirtualProtect(src, kPatchLen, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Log("InstallLoaderHook: VirtualProtect failed=%lu\n", GetLastError());
        VirtualFree(tramp, 0, MEM_RELEASE);
        return false;
    }
    BYTE patch[kPatchLen] = {};
    patch[0] = 0xE9;
    *reinterpret_cast<int32_t*>(patch + 1) =
        static_cast<int32_t>(reinterpret_cast<uintptr_t>(&HookWARCreate9Loader) - (loader + 5));
    patch[5] = 0x90;
    patch[6] = 0x90;
    memcpy(src, patch, kPatchLen);
    FlushInstructionCache(GetCurrentProcess(), src, kPatchLen);
    DWORD ignored = 0;
    VirtualProtect(src, kPatchLen, oldProtect, &ignored);

    g_loaderAddress = loader;
    g_loaderTrampoline = tramp;
    g_loaderHooked = true;
    Log("WAR loader hooked loader=%p trampoline=%p slot=%p\n", (void*)loader, tramp, (void*)slot);
    PatchCreate9Slot(slot);
    return true;
}

static bool PatchWARCreate9()
{
    HMODULE war = GetModuleHandleW(L"WAR.exe");
    if (!war) { Log("PatchWARCreate9: WAR.exe not present\n"); return false; }
    auto base = reinterpret_cast<uintptr_t>(war);
    uintptr_t slot = base + 0x009693A8;
    uintptr_t loader = base + 0x00388230;
    Log("PatchWARCreate9: WAR base=%p slot=%p loader=%p\n", (void*)base, (void*)slot, (void*)loader);
    if (!g_loaderHooked && !InstallLoaderHook(loader, slot)) return false;
    return PatchCreate9Slot(slot) || g_loaderHooked;
}

static bool WINAPI HookWARCreate9Loader()
{
    using LoaderFn = bool (WINAPI*)();
    auto original = reinterpret_cast<LoaderFn>(g_loaderTrampoline);
    Log("WAR Create9 loader entered trampoline=%p\n", g_loaderTrampoline);
    bool result = original ? original() : false;
    HMODULE war = GetModuleHandleW(L"WAR.exe");
    if (war) {
        uintptr_t slot = reinterpret_cast<uintptr_t>(war) + 0x009693A8;
        auto resolved = *reinterpret_cast<Direct3DCreate9_t*>(slot);
        Log("WAR Create9 loader returned=%d; slot=%p value=%p\n", result ? 1 : 0, (void*)slot, (void*)resolved);
        PatchCreate9Slot(slot);
    }
    return result;
}

// ---------------------------------------------------------------- startup / worker
// Logging is OFF unless WARVR.ini contains  log=on  (also 1 / true / yes). Without a log file the capture hotkey does nothing.
static bool IniLogWanted()
{
    std::string path = ModuleDir() + "\\WARVR.ini";
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "r") != 0 || !f) return false;
    bool on = false;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char* eq = strchr(line, '=');
        if (!eq || line[0] == ';' || line[0] == '#') continue;
        *eq = 0; char* val = eq + 1;
        for (char* p = val; *p; ++p) if (*p == '\r' || *p == '\n' || *p == ' ') *p = 0;
        char* key = line; while (*key == ' ' || *key == '\t') ++key;
        if (_stricmp(key, "log") == 0) on = (_stricmp(val, "on") == 0 || _stricmp(val, "1") == 0 || _stricmp(val, "true") == 0 || _stricmp(val, "yes") == 0);
    }
    fclose(f);
    return on;
}

static void OpenLogEarly()
{
    if (g_log) return;
    static int wanted = -1;
    if (wanted < 0) wanted = IniLogWanted() ? 1 : 0;
    if (!wanted) return;
    char path[MAX_PATH]{};
    char tmp[MAX_PATH]{};
    DWORD n = GetTempPathA(MAX_PATH, tmp);
    if (!n || n >= MAX_PATH) return;
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%sWARVR.log", tmp);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "a") == 0 && f) {
        g_log = f;
        Log("\n=== WARVR PHASE 7l ATTACHED ===\n");
        Log("PID=%lu WAR base=%p DLL=%p log=%s\n", GetCurrentProcessId(), (void*)GetModuleHandleW(L"WAR.exe"), (void*)g_self, path);
    }
}

// ---------------------------------------------------------------- hotkeys
// All actions are Ctrl+Shift + <key>. Defaults are the numpad operator keys (they do not depend on
// NumLock and the game does not bind them). Override any of them in WARVR.ini next to WARVR.dll:
//     capture=F6
//     scan=NUMPAD_MULTIPLY
//     recenter=HOME
//     test=INSERT
//     head=PAGEUP
// Key names: A-Z, 0-9, F1-F24, NUMPAD0-9, NUMPAD_DIVIDE/MULTIPLY/SUBTRACT/ADD/DECIMAL, INSERT, DELETE,
//            HOME, END, PAGEUP, PAGEDOWN, PAUSE, SCROLLLOCK.
enum Action { ACT_CAPTURE, ACT_SCAN, ACT_RECENTER, ACT_TEST, ACT_HEAD, ACT_VR, ACT_SCALE_UP, ACT_SCALE_DOWN, ACT_CULL, ACT_HUD_NEAR, ACT_HUD_FAR, ACT_RECENTER2, ACT_POSITIONAL, ACT_RECENTER3, ACT_CURSOR, ACT_PITCHLOCK, ACT_COUNT };
static int g_keyVk[ACT_COUNT] = { 'C', 0 /*scan: unbound*/, 0 /*recenter: Numpad - now belongs to the pitch lock; recenter2/3 remain*/, VK_ADD, VK_INSERT, VK_MULTIPLY, VK_PRIOR, VK_NEXT, VK_HOME, VK_END, VK_DELETE, VK_DIVIDE, 'P', VK_OEM_2, 'M', VK_SUBTRACT };
static const char* kActionName[ACT_COUNT] = { "capture", "scan", "recenter", "test", "head", "vr", "scale_up", "scale_down", "cull", "hud_near", "hud_far", "recenter2", "positional", "recenter3", "cursor", "pitch_lock" };
static const char* kActionDesc[ACT_COUNT] = { "capture one frame", "scan memory for camera", "recenter forward", "20-degree rotation test", "head tracking on monitor", "VR mode on/off", "world scale up", "world scale down", "culling follows head on/off", "HUD closer", "HUD farther", "recenter forward (alternate key)", "head position tracking on/off", "recenter forward (main-keyboard slash)", "VR mouse cursor on/off", "mouse pitch lock on/off (mouse no longer tilts the view; the headset does)" };

static int ParseKey(const char* n)
{
    static const struct { const char* name; int vk; } tbl[] = {
        {"NUMPAD_DIVIDE", VK_DIVIDE}, {"NUMPAD_MULTIPLY", VK_MULTIPLY}, {"NUMPAD_SUBTRACT", VK_SUBTRACT},
        {"NUMPAD_ADD", VK_ADD}, {"NUMPAD_DECIMAL", VK_DECIMAL}, {"INSERT", VK_INSERT}, {"DELETE", VK_DELETE},
        {"HOME", VK_HOME}, {"END", VK_END}, {"PAGEUP", VK_PRIOR}, {"PAGEDOWN", VK_NEXT}, {"PAUSE", VK_PAUSE},
        {"SCROLLLOCK", VK_SCROLL},
    };
    for (auto& e : tbl) if (_stricmp(n, e.name) == 0) return e.vk;
    if (strlen(n) == 1 && ((n[0] >= 'A' && n[0] <= 'Z') || (n[0] >= '0' && n[0] <= '9'))) return n[0];
    if ((n[0] == 'F' || n[0] == 'f') && atoi(n + 1) >= 1 && atoi(n + 1) <= 24) return VK_F1 + atoi(n + 1) - 1;
    if (_strnicmp(n, "NUMPAD", 6) == 0 && n[6] >= '0' && n[6] <= '9' && n[7] == 0) return VK_NUMPAD0 + (n[6] - '0');
    return 0;
}

static void LoadHotkeyConfig()
{
    std::string path = ModuleDir() + "\\WARVR.ini";
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "r") != 0 || !f) { Log("no WARVR.ini (using default hotkeys)\n"); return; }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0; char* val = eq + 1;
        for (char* p = val; *p; ++p) if (*p == '\r' || *p == '\n' || *p == ' ') *p = 0;
        if (_stricmp(line, "units_per_meter") == 0) { float v = (float)atof(val); if (v > 0.1f) { g_unitsPerMeter = v; Log("WARVR.ini: units_per_meter -> %.2f\n", v); } continue; }
        if (_stricmp(line, "eye_size") == 0) { g_eyeSizeCfg = (UINT)atoi(val); Log("WARVR.ini: eye_size -> %u\n", g_eyeSizeCfg); continue; }
        if (_stricmp(line, "positional") == 0) { g_positional = atoi(val) != 0; Log("WARVR.ini: positional -> %d\n", g_positional ? 1 : 0); continue; }
        if (_stricmp(line, "gpu_path") == 0) { g_gpuPathWanted = _stricmp(val, "off") != 0; Log("WARVR.ini: gpu_path -> %s\n", g_gpuPathWanted ? "auto" : "off"); continue; }
        if (_stricmp(line, "vr_snapshot") == 0) { g_snapshotAt = (unsigned)atoi(val); Log("WARVR.ini: vr_snapshot -> %u\n", g_snapshotAt); continue; }
        if (_stricmp(line, "auto_vr") == 0) { g_autoVr = _stricmp(val, "on") == 0 || _stricmp(val, "1") == 0; Log("WARVR.ini: auto_vr -> %s\n", g_autoVr ? "on" : "off"); continue; }
        if (_stricmp(line, "lazy_state") == 0) { g_lazyState = _stricmp(val, "on") == 0 || _stricmp(val, "1") == 0; Log("WARVR.ini: lazy_state -> %s\n", g_lazyState ? "on (experimental)" : "off"); continue; }
        if (_stricmp(line, "vk_path") == 0) { g_vkPathWanted = _stricmp(val, "off") != 0; Log("WARVR.ini: vk_path -> %s\n", g_vkPathWanted ? "auto" : "off"); continue; }
        if (_stricmp(line, "desktop_every") == 0) { g_desktopEvery = (unsigned)atoi(val); Log("WARVR.ini: desktop_every -> %u\n", g_desktopEvery); continue; }
        if (_stricmp(line, "draw_distance_flat") == 0) { g_drawDistFlat = !(_stricmp(val, "off") == 0 || _stricmp(val, "0") == 0); Log("WARVR.ini: draw_distance_flat -> %s\n", g_drawDistFlat ? "on (draw_distance also applies when VR is off)" : "off (VR only, the original behaviour)"); continue; }
        if (_stricmp(line, "draw_distance") == 0) { float v = (float)atof(val); if (v >= 1.0f && v <= 16.f) g_drawDistScale = v; Log("WARVR.ini: draw_distance -> x%.2f (far plane multiplier)\n", g_drawDistScale); continue; }
        if (_stricmp(line, "pitch_lock_start") == 0) { g_pitchLock = (_stricmp(val, "on") == 0 || _stricmp(val, "true") == 0 || _stricmp(val, "1") == 0); Log("WARVR.ini: pitch_lock_start -> %s\n", g_pitchLock.load() ? "on" : "off"); continue; }
        if (_stricmp(line, "log") == 0) { Log("WARVR.ini: log -> on\n"); continue; }
        if (_stricmp(line, "water") == 0) { g_waterMode = _stricmp(val, "off") != 0; Log("WARVR.ini: water -> %s\n", g_waterMode ? "on (planar reflection rendered for both eyes)" : "off"); continue; }
        if (_stricmp(line, "cursor") == 0) { g_cursorOn = _stricmp(val, "off") != 0; Log("WARVR.ini: cursor -> %s\n", g_cursorOn ? "on" : "off"); continue; }
        if (_stricmp(line, "cursor_size") == 0) { float v = (float)atof(val); if (v > 0.005f && v < 0.5f) g_cursorSize = v; Log("WARVR.ini: cursor_size -> %.3f\n", g_cursorSize); continue; }
        if (_stricmp(line, "vr_resolution") == 0) { g_resMode = _stricmp(val, "steamvr") == 0 ? 1 : (_stricmp(val, "desktop") == 0 ? 0 : 2); Log("WARVR.ini: vr_resolution -> %s\n", g_resMode == 1 ? "steamvr (always render at the size SteamVR recommends)" : (g_resMode == 0 ? "desktop (the game's own resolution)" : "auto (SteamVR's size when the GPU hand-off is available)")); continue; }
        if (_stricmp(line, "max_pixels") == 0) { g_maxPixels = atof(val) * 1e6; Log("WARVR.ini: max_pixels -> %.1f million (0 = no limit)\n", g_maxPixels / 1e6); continue; }
        if (_stricmp(line, "render_scale") == 0) { double v = atof(val); if (v > 0.05 && v <= 4.0) g_renderScale = v; Log("WARVR.ini: render_scale -> %.2f\n", g_renderScale); continue; }
        if (_stricmp(line, "auto_recenter") == 0) { g_autoRecenterOnWorn = atoi(val) != 0; Log("WARVR.ini: auto_recenter -> %d\n", g_autoRecenterOnWorn ? 1 : 0); continue; }
        if (_stricmp(line, "cull") == 0) { g_cullTakeover = _stricmp(val, "off") != 0; Log("WARVR.ini: cull -> %s\n", g_cullTakeover.load() ? "on" : "off"); continue; }
        if (_stricmp(line, "stereo") == 0) { g_stereoSBS = _stricmp(val, "aer") != 0; Log("WARVR.ini: stereo -> %s\n", g_stereoSBS ? "sbs (simultaneous)" : "aer (alternate eye)"); continue; }
        if (_stricmp(line, "hud") == 0) { g_hudMode = _stricmp(val, "fill") == 0 ? 1 : 0; Log("WARVR.ini: hud -> %s\n", g_hudMode ? "fill" : "3d"); continue; }
        if (_stricmp(line, "hud_anchor") == 0) { g_hudAnchor = (_stricmp(val, "head") == 0) ? 0 : (_stricmp(val, "billboard") == 0 ? 1 : 2); Log("WARVR.ini: hud_anchor -> %s\n", g_hudAnchor == 2 ? "world (a flat panel fixed in the game's frame)" : (g_hudAnchor == 1 ? "billboard (anchored in the game's frame, always faces you)" : "head (locked to the headset)")); continue; }
        if (_stricmp(line, "hud_distance") == 0) { float v = (float)atof(val); if (v > 0.2f) g_hudDistance = v; Log("WARVR.ini: hud_distance -> %.2f m\n", g_hudDistance); continue; }
        if (_stricmp(line, "hud_width") == 0) { float v = (float)atof(val); if (v > 0.1f) g_hudWidth = v; Log("WARVR.ini: hud_width -> %.2f m\n", g_hudWidth); continue; }
        if (_stricmp(line, "hud_vertical") == 0) { g_hudVert = (float)atof(val); Log("WARVR.ini: hud_vertical -> %.2f m\n", g_hudVert); continue; }
        if (_stricmp(line, "steer") == 0) {
            if (_stricmp(val, "constants") == 0) { g_steerMode = 1; Log("WARVR.ini: steer -> constants (older method)\n"); }
            else if (_stricmp(val, "camera") == 0) { g_steerMode = 0; Log("WARVR.ini: steer -> camera\n"); }
            continue;
        }
        for (int a = 0; a < ACT_COUNT; ++a)
            if (_stricmp(line, kActionName[a]) == 0) {
                int vk = ParseKey(val);
                if (vk) { g_keyVk[a] = vk; Log("WARVR.ini: %s -> %s\n", kActionName[a], val); }
                else Log("WARVR.ini: unknown key '%s' for %s\n", val, kActionName[a]);
            }
    }
    fclose(f);
}

static const char* KeyLabel(int vk)
{
    static char buf[ACT_COUNT][24]; static int n = 0;
    char* b = buf[n++ % ACT_COUNT];
    switch (vk) {
        case VK_DIVIDE: return "Numpad /"; case VK_MULTIPLY: return "Numpad *"; case VK_SUBTRACT: return "Numpad -";
        case VK_ADD: return "Numpad +"; case VK_DECIMAL: return "Numpad ."; case VK_INSERT: return "Insert";
        case VK_DELETE: return "Delete"; case VK_HOME: return "Home"; case VK_END: return "End";
        case VK_PRIOR: return "PageUp"; case VK_NEXT: return "PageDown"; case VK_PAUSE: return "Pause";
        case VK_SCROLL: return "ScrollLock";
    }
    if (vk >= VK_F1 && vk <= VK_F24) _snprintf_s(b, 24, _TRUNCATE, "F%d", vk - VK_F1 + 1);
    else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) _snprintf_s(b, 24, _TRUNCATE, "Numpad %d", vk - VK_NUMPAD0);
    else _snprintf_s(b, 24, _TRUNCATE, "%c", vk);
    return b;
}

// Beeps run on their own short thread: Beep() can block for a long time if the default audio device is
// switched or busy (SteamVR switches it to the headset), and it must never stall hotkey handling.
static DWORD WINAPI BeepThread(void* p)
{
    unsigned packed = static_cast<unsigned>(reinterpret_cast<uintptr_t>(p));
    Beep(packed >> 16, packed & 0xFFFF);
    return 0;
}
static void BeepAsync(unsigned freq, unsigned ms)
{
    HANDLE h = CreateThread(nullptr, 0, BeepThread, reinterpret_cast<void*>(static_cast<uintptr_t>((freq << 16) | ms)), 0, nullptr);
    if (h) CloseHandle(h);
}

static DWORD g_lastFire[ACT_COUNT] = {};
static void RunAction(Action a, const char* via)
{
    DWORD now = GetTickCount();
    if (now - g_lastFire[a] < 300) return;                 // same keypress seen by both detectors
    g_lastFire[a] = now;
    Log("HOTKEY %s (%s)\n", kActionName[a], via);
    switch (a) {
    case ACT_SCAN:    BeepAsync(1200, 80); RunMemoryScan(); BeepAsync(1500, 120); break;
    case ACT_PITCHLOCK: {
        const bool now = !g_pitchLock.load(); g_pitchLock = now;
        BeepAsync(now ? 1400 : 700, 90);
        Log("   mouse pitch lock -> %s\n", now ? "ON: the game camera is kept level, the headset still looks up and down" : "off");
        break;
    }
    case ACT_RECENTER: case ACT_RECENTER2: case ACT_RECENTER3: BeepAsync(1000, 80); g_recenterRequest = true; break;
    case ACT_CAPTURE: BeepAsync(1200, 80); g_captureRequest = true; break;
    case ACT_TEST: {
        bool v = !g_perturb.load(); g_perturb = v; BeepAsync(v ? 1400 : 500, 120);
        Log("rotation test=%d\n", v ? 1 : 0);
        if (v) g_captureCountdown = 90;
        break; }
    case ACT_VR:
        g_vrWanted = !g_vrWanted.load();                 // the render thread starts/stops it on the next frame
        Log("VR mode requested %s\n", g_vrWanted.load() ? "ON" : "OFF");
        break;
    case ACT_SCALE_UP: case ACT_SCALE_DOWN:
        g_unitsPerMeter *= (a == ACT_SCALE_UP) ? 1.25f : (1.f / 1.25f);
        BeepAsync(a == ACT_SCALE_UP ? 1300 : 700, 60);
        Log("world scale = %.2f units per metre\n", g_unitsPerMeter);
        break;
    case ACT_HUD_NEAR: case ACT_HUD_FAR:
        g_hudDistance *= (a == ACT_HUD_NEAR) ? (1.f / 1.1f) : 1.1f;
        if (g_hudDistance < 0.3f) g_hudDistance = 0.3f;
        BeepAsync(a == ACT_HUD_NEAR ? 900 : 600, 60);
        Log("HUD distance = %.2f m (width %.2f m)\n", g_hudDistance, g_hudWidth);
        break;
    case ACT_CURSOR: {
        g_cursorOn = !g_cursorOn; BeepAsync(g_cursorOn ? 1400 : 500, 120);
        Log("VR mouse cursor = %d\n", g_cursorOn ? 1 : 0);
        break; }
    case ACT_POSITIONAL: {
        g_positional = !g_positional; BeepAsync(g_positional ? 1400 : 500, 120);
        Log("positional tracking = %d\n", g_positional ? 1 : 0);
        break; }
    case ACT_CULL: {
        bool v = !g_cullTakeover.load(); g_cullTakeover = v; BeepAsync(v ? 1400 : 500, 120);   // Present restores the camera
        Log("culling follows head = %d\n", v ? 1 : 0);
        break; }
    case ACT_HEAD:
        if (g_vrActive || g_vrWanted.load()) { Log("head-only mode ignored: VR mode is active\n"); break; }
        if (g_headOn.load()) { g_headOn = false; BeepAsync(500, 120); Log("head tracking OFF\n"); }
        else {
            if (!g_vr.ready()) g_vr.Init(ModuleDir().c_str(), Log);
            if (g_vr.ready()) { g_haveRef = false; g_recenterRequest = true; g_headOn = true; BeepAsync(1400, 120); Log("head tracking ON\n"); g_captureCountdown = 90; }
            else { BeepAsync(300, 400); Log("head tracking unavailable (see OpenVR message above)\n"); }
        }
        break;
    default: break;
    }
}

static bool g_keyPrev[ACT_COUNT] = {};
static bool PolledTrigger(Action a)                 // rising edge of Ctrl+Shift+key via GetAsyncKeyState
{
    if (!g_keyVk[a]) return false;
    bool down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
                (GetAsyncKeyState(g_keyVk[a]) & 0x8000);
    bool fire = down && !g_keyPrev[a];
    g_keyPrev[a] = down;
    return fire;
}

static DWORD WINAPI Worker(void*)
{
    OpenLogEarly();
    g_perturbQ = vrm::cameraDelta(vrm::rotY(-20.f * 3.14159265f / 180.f), 0.f);   // 20 deg yaw right
    Log("=== worker started (thread %lu) ===\n", GetCurrentThreadId());
    LoadHotkeyConfig();
    if (g_gpuPathWanted) gpu::InstallCreateHooks(Log);       // must happen before dgVoodoo creates its D3D11 device
    else Log("GPU path: disabled by WARVR.ini (gpu_path=off)\n");

    bool patched = false;
    for (int i = 0; i < 600 && g_running; ++i) {
        if (GetModuleHandleW(L"WAR.exe") && PatchWARCreate9()) { patched = true; break; }
        Sleep(10);
    }
    Log("initial patch result=%d\n", patched ? 1 : 0);

    // OS-level hotkeys: delivered to this thread as WM_HOTKEY, work regardless of which window has focus,
    // and swallow the keystroke so the game does not also react to it. The polling path below is a backup.
    bool regOk[ACT_COUNT] = {};
    for (int a = 0; a < ACT_COUNT; ++a) {
        if (!g_keyVk[a]) continue;
        regOk[a] = RegisterHotKey(nullptr, 100 + a, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, g_keyVk[a]) != 0;
        Log("RegisterHotKey Ctrl+Shift+[%s] for '%s': %s (err=%lu)\n", KeyLabel(g_keyVk[a]), kActionName[a],
            regOk[a] ? "ok" : "FAILED (another program owns it; polling backup still active)", regOk[a] ? 0 : GetLastError());
    }
    Log("Hotkeys (hold Ctrl+Shift, tap key):");
    for (int a = 0; a < ACT_COUNT; ++a) if (g_keyVk[a]) Log("  [%s] %s;", KeyLabel(g_keyVk[a]), kActionDesc[a]);
    Log("\n");

    DWORD lastBeat = GetTickCount();
    bool prevChord = false;
    bool keyWasDown[256] = {};
    while (g_running) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, WM_HOTKEY, WM_HOTKEY, PM_REMOVE)) {
            int a = static_cast<int>(msg.wParam) - 100;
            if (a >= 0 && a < ACT_COUNT) RunAction(static_cast<Action>(a), "RegisterHotKey");
        }
        for (int a = 0; a < ACT_COUNT; ++a) if (PolledTrigger(static_cast<Action>(a))) RunAction(static_cast<Action>(a), "polled");
        if (g_autoVrRequest.exchange(false)) RunAction(ACT_VR, "auto_vr");

        // Diagnostics: while Ctrl+Shift are held, log every other key Windows reports as down.
        bool chord = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000);
        if (chord) {
            for (int vk = 8; vk < 255; ++vk) {
                if (vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_LSHIFT || vk == VK_RSHIFT) continue;
                bool d = (GetAsyncKeyState(vk) & 0x8000) != 0;
                if (d && !keyWasDown[vk]) Log("KEYDIAG: Ctrl+Shift+VK 0x%02X pressed (foreground window %p)\n", vk, (void*)GetForegroundWindow());
                keyWasDown[vk] = d;
            }
        } else if (prevChord) memset(keyWasDown, 0, sizeof(keyWasDown));
        prevChord = chord;

        if (GetTickCount() - lastBeat > 20000) { lastBeat = GetTickCount(); Log("heartbeat: worker alive, frame=%llu head=%d test=%d\n", g_frame, g_headOn.load() ? 1 : 0, g_perturb.load() ? 1 : 0); }
        Sleep(10);
    }
    for (int a = 0; a < ACT_COUNT; ++a) if (regOk[a]) UnregisterHotKey(nullptr, 100 + a);
    g_vr.Shutdown();
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = hinst;
        DisableThreadLibraryCalls(hinst);
        OpenLogEarly();
        g_worker = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    } else if (reason == DLL_PROCESS_DETACH) {
        g_running = false;
    }
    return TRUE;
}
