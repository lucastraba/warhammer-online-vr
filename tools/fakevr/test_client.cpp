// test_client.cpp - verification harness for fakevr (the fake openvr_api.dll).
//
// Loads openvr_api.dll from its own folder exactly like src/openvr_head.h does (LoadLibraryA +
// GetProcAddress, C FnTable API), then exercises init/failure paths, both function tables, pacing,
// Submit validation (D3D11 + Vulkan), cumulative stats, the log file and the BMP dumps.
// Writes fakevr.ini next to the DLL for each phase. Exit code 0 only if every check passed.
//
// Build: tools/fakevr/build.sh.  Run: test_client.exe (Windows, or Wine inside a private X server).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define OPENVR_API_NODLL
#include <windows.h>
#include <d3d11.h>
#ifdef __WIN32
#undef __WIN32
#endif
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#endif
#include "../../third_party/openvr/openvr_capi.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int g_checks = 0, g_fails = 0;
static void Check(bool ok, const char* fmt, ...)
{
    ++g_checks;
    if (!ok) ++g_fails;
    char buf[1024];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    printf("%s: %s\n", ok ? "PASS" : "FAIL", buf);
    fflush(stdout);
}
// Quiet variant for per-frame checks: only failures are printed (first 20).
static int g_quietFails = 0;
static bool Q(bool ok, const char* fmt, ...)
{
    if (ok) return true;
    ++g_quietFails;
    if (g_quietFails <= 20) {
        char buf[1024];
        va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
        printf("  frame-check failed: %s\n", buf);
    }
    return false;
}
static void Info(const char* fmt, ...)
{
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n"); fflush(stdout);
}
static bool Near(double a, double b, double eps = 1e-5) { return std::fabs(a - b) <= eps; }

static std::string g_dir;
static std::string PathIn(const char* name) { return g_dir + "\\" + name; }
static void WriteIni(const char* text)
{
    FILE* f = fopen(PathIn("fakevr.ini").c_str(), "wb");
    if (f) { fputs(text, f); fclose(f); }
}
static void DeleteIni() { DeleteFileA(PathIn("fakevr.ini").c_str()); }
static std::string ReadFile(const std::string& p)
{
    std::string s;
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return s;
    char buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    fclose(f);
    return s;
}

// Same signatures as openvr_head.h.
using PFN_Init     = intptr_t (*)(EVRInitError*, EVRApplicationType);
using PFN_Shutdown = void (*)();
using PFN_GetIface = intptr_t (*)(const char*, EVRInitError*);
using PFN_ErrText  = const char* (*)(EVRInitError);
using PFN_ErrSym   = const char* (*)(EVRInitError);
using PFN_Valid    = bool (*)(const char*);
using PFN_Bool     = bool (*)();

static PFN_Init pInit; static PFN_Shutdown pShutdown; static PFN_GetIface pGetIface; static PFN_ErrText pErrText;

static void* GetTable(const char* version, EVRInitError* errOut)
{
    char name[128]{};
    _snprintf(name, sizeof(name), "FnTable:%s", version);
    EVRInitError err = EVRInitError_VRInitError_None;
    void* p = reinterpret_cast<void*>(pGetIface(name, &err));
    if (errOut) *errOut = err;
    return err == EVRInitError_VRInitError_None ? p : nullptr;
}

static bool AllSlotsSet(const void* table, size_t size, size_t* nullAt)
{
    const void* const* s = static_cast<const void* const*>(table);
    for (size_t i = 0; i < size / sizeof(void*); ++i) if (!s[i]) { *nullAt = i; return false; }
    return true;
}

// ---- D3D11 helpers -----------------------------------------------------------------------------
struct RGBA8 { uint8_t r, g, b; };
static ID3D11Texture2D* MakeTex(ID3D11Device* dev, UINT w, UINT h, DXGI_FORMAT fmt, RGBA8 leftHalf, RGBA8 rightHalf)
{
    std::vector<uint8_t> px((size_t)w * h * 4);
    const bool bgra = fmt == DXGI_FORMAT_B8G8R8A8_UNORM;
    for (UINT y = 0; y < h; ++y)
        for (UINT x = 0; x < w; ++x) {
            RGBA8 c = x < w / 2 ? leftHalf : rightHalf;
            uint8_t* p = &px[((size_t)y * w + x) * 4];
            if (bgra) { p[0] = c.b; p[1] = c.g; p[2] = c.r; } else { p[0] = c.r; p[1] = c.g; p[2] = c.b; }
            p[3] = 255;
        }
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = fmt; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    D3D11_SUBRESOURCE_DATA init{px.data(), w * 4, 0};
    ID3D11Texture2D* t = nullptr;
    HRESULT hr = dev->CreateTexture2D(&d, &init, &t);
    if (FAILED(hr)) { Info("  CreateTexture2D(%ux%u fmt %d) failed hr=0x%08lx", w, h, (int)fmt, (unsigned long)hr); return nullptr; }
    return t;
}

// Returns "" if the BMP is w x h, 24 bpp, and every pixel equals expect; else a reason.
static std::string CheckBmp(const std::string& path, int w, int h, RGBA8 expect)
{
    std::string s = ReadFile(path);
    if (s.size() < 54) return "missing or truncated";
    const uint8_t* b = (const uint8_t*)s.data();
    auto rd32 = [&](int o) { return (int32_t)(b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | ((uint32_t)b[o + 3] << 24)); };
    if (b[0] != 'B' || b[1] != 'M') return "not a BMP";
    int bw = rd32(18), bh = rd32(22), bpp = b[28] | (b[29] << 8), off = rd32(10);
    char r[160];
    if (bw != w || bh != h || bpp != 24) { snprintf(r, sizeof(r), "dims %dx%d bpp %d, want %dx%d 24", bw, bh, bpp, w, h); return r; }
    int row = (w * 3 + 3) & ~3;
    if ((int)s.size() < off + row * h) return "short pixel data";
    long bad = 0; int fx = -1, fy = -1; uint8_t got[3] = {};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const uint8_t* p = b + off + y * row + x * 3;
            if (p[0] != expect.b || p[1] != expect.g || p[2] != expect.r) {
                if (!bad) { fx = x; fy = y; memcpy(got, p, 3); }
                ++bad;
            }
        }
    if (bad) { snprintf(r, sizeof(r), "%ld wrong pixels, first at (%d,%d) BGR=%02x%02x%02x", bad, fx, fy, got[0], got[1], got[2]); return r; }
    return "";
}

static void DeleteDumps()
{
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(PathIn("fakevr_dump_*.bmp").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do DeleteFileA(PathIn(fd.cFileName).c_str()); while (FindNextFileA(h, &fd));
    FindClose(h);
}
static int CountDumps()
{
    int n = 0;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(PathIn("fakevr_dump_*.bmp").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do ++n; while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}

static double QpcMs()
{
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER v; QueryPerformanceCounter(&v);
    return (double)v.QuadPart * 1000.0 / (double)f.QuadPart;
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    char exe[MAX_PATH]; GetModuleFileNameA(nullptr, exe, MAX_PATH);
    g_dir = exe; g_dir = g_dir.substr(0, g_dir.find_last_of("\\/"));
    Info("fakevr test_client, dir %s", g_dir.c_str());

    // ---------------------------------------------------------------------------------- load
    DeleteIni();
    HMODULE dll = LoadLibraryA("openvr_api.dll");
    Check(dll != nullptr, "LoadLibraryA(openvr_api.dll) (err=%lu)", dll ? 0ul : GetLastError());
    if (!dll) { printf("RESULT: FAIL (cannot load DLL)\n"); return 1; }
    const char* exports[] = {"VR_InitInternal", "VR_InitInternal2", "VR_ShutdownInternal", "VR_IsHmdPresent", "VR_IsRuntimeInstalled",
                             "VR_GetRuntimePath", "VR_GetGenericInterface", "VR_IsInterfaceVersionValid", "VR_GetInitToken",
                             "VR_GetVRInitErrorAsSymbol", "VR_GetVRInitErrorAsEnglishDescription"};
    int found = 0;
    for (const char* e : exports) { if (GetProcAddress(dll, e)) ++found; else Info("  missing export %s", e); }
    Check(found == 11, "all 11 exports resolve by undecorated name (%d/11)", found);
    pInit     = reinterpret_cast<PFN_Init>(reinterpret_cast<void*>(GetProcAddress(dll, "VR_InitInternal")));
    pShutdown = reinterpret_cast<PFN_Shutdown>(reinterpret_cast<void*>(GetProcAddress(dll, "VR_ShutdownInternal")));
    pGetIface = reinterpret_cast<PFN_GetIface>(reinterpret_cast<void*>(GetProcAddress(dll, "VR_GetGenericInterface")));
    pErrText  = reinterpret_cast<PFN_ErrText>(reinterpret_cast<void*>(GetProcAddress(dll, "VR_GetVRInitErrorAsEnglishDescription")));
    auto pErrSym = reinterpret_cast<PFN_ErrSym>(reinterpret_cast<void*>(GetProcAddress(dll, "VR_GetVRInitErrorAsSymbol")));
    auto pValid = reinterpret_cast<PFN_Valid>(reinterpret_cast<void*>(GetProcAddress(dll, "VR_IsInterfaceVersionValid")));
    auto pHmdPresent = reinterpret_cast<PFN_Bool>(reinterpret_cast<void*>(GetProcAddress(dll, "VR_IsHmdPresent")));
    auto pRuntimeInstalled = reinterpret_cast<PFN_Bool>(reinterpret_cast<void*>(GetProcAddress(dll, "VR_IsRuntimeInstalled")));
    if (!pInit || !pShutdown || !pGetIface || !pErrText || !pErrSym || !pValid || !pHmdPresent || !pRuntimeInstalled) {
        printf("RESULT: FAIL (exports missing)\n"); return 1;
    }
    Check(pRuntimeInstalled(), "VR_IsRuntimeInstalled() == true");

    // ---------------------------------------------------------------------------------- before init
    {
        EVRInitError e = EVRInitError_VRInitError_None;
        void* p = GetTable(IVRSystem_Version, &e);
        Check(!p && e == EVRInitError_VRInitError_Init_NotInitialized, "GetGenericInterface before init -> nullptr, err=%d (%s)", (int)e, pErrSym(e));
    }

    // ---------------------------------------------------------------------------------- fail_init
    {
        WriteIni("; test: forced init failure\nfail_init=1\n");
        Check(!pHmdPresent(), "VR_IsHmdPresent() == false with fail_init=1");
        EVRInitError e = EVRInitError_VRInitError_None;
        intptr_t tok = pInit(&e, EVRApplicationType_VRApplication_Scene);
        Check(e == EVRInitError_VRInitError_Init_HmdNotFound && tok == 0, "fail_init=1: VR_InitInternal(Scene) -> err=%d (%s) \"%s\"", (int)e, pErrSym(e), pErrText(e));
        DeleteIni();
    }

    // ---------------------------------------------------------------------------------- defaults, Scene
    VR_IVRSystem_FnTable* sys = nullptr;
    VR_IVRCompositor_FnTable* comp = nullptr;
    {
        Check(pHmdPresent(), "VR_IsHmdPresent() == true (no ini)");
        EVRInitError e = EVRInitError_VRInitError_None;
        intptr_t tok = pInit(&e, EVRApplicationType_VRApplication_Scene);
        Check(e == EVRInitError_VRInitError_None && tok != 0, "init: VR_InitInternal(Scene) -> err=%d (%s), token %ld", (int)e, pErrSym(e), (long)tok);
        sys = reinterpret_cast<VR_IVRSystem_FnTable*>(GetTable(IVRSystem_Version, &e));
        Check(sys != nullptr, "GetGenericInterface(FnTable:%s) -> %p err=%d", IVRSystem_Version, (void*)sys, (int)e);
        comp = reinterpret_cast<VR_IVRCompositor_FnTable*>(GetTable(IVRCompositor_Version, &e));
        Check(comp != nullptr, "GetGenericInterface(FnTable:%s) -> %p err=%d", IVRCompositor_Version, (void*)comp, (int)e);
        if (!sys || !comp) { printf("RESULT: FAIL (no tables)\n"); return 1; }
        size_t at = 0;
        bool ok = AllSlotsSet(sys, sizeof(*sys), &at);
        Check(ok, "IVRSystem FnTable: all %u slots non-null%s", (unsigned)(sizeof(*sys) / sizeof(void*)), ok ? "" : " (null found)");
        ok = AllSlotsSet(comp, sizeof(*comp), &at);
        Check(ok, "IVRCompositor FnTable: all %u slots non-null%s", (unsigned)(sizeof(*comp) / sizeof(void*)), ok ? "" : " (null found)");

        const char* bad[] = {"IVRCompositor_029", "IVRSystem_026", "FnTable:IVROverlay_028", "FnTable:IVRSystem_025", "FnTable:IVRCompositor_028"};
        for (const char* n : bad) {
            EVRInitError be = EVRInitError_VRInitError_None;
            intptr_t p = pGetIface(n, &be);
            Check(p == 0 && be == EVRInitError_VRInitError_Init_InterfaceNotFound, "GetGenericInterface(%s) -> nullptr, err=%d (%s)", n, (int)be, pErrSym(be));
        }
        Check(pValid("FnTable:IVRSystem_026") && pValid("FnTable:IVRCompositor_029") && !pValid("IVRCompositor_029") && !pValid("FnTable:IVROverlay_028"),
              "VR_IsInterfaceVersionValid: true for served FnTables, false otherwise");
        Check(!strcmp(pErrSym(EVRInitError_VRInitError_Init_InterfaceNotFound), "VRInitError_Init_InterfaceNotFound"), "VR_GetVRInitErrorAsSymbol(105) = %s",
              pErrSym(EVRInitError_VRInitError_Init_InterfaceNotFound));

        comp->SetTrackingSpace(ETrackingUniverseOrigin_TrackingUniverseSeated);     // what openvr_head.h does
        Check(comp->GetTrackingSpace() == ETrackingUniverseOrigin_TrackingUniverseSeated, "SetTrackingSpace(Seated) stored");

        uint32_t w = 0, h = 0;
        sys->GetRecommendedRenderTargetSize(&w, &h);
        Check(w == 2016 && h == 2240, "sizes: GetRecommendedRenderTargetSize = %ux%u (default 2016x2240)", w, h);
        float t[2][4];
        for (int eye = 0; eye < 2; ++eye) sys->GetProjectionRaw((EVREye)eye, &t[eye][0], &t[eye][1], &t[eye][2], &t[eye][3]);
        Info("projection L: l=%.3f r=%.3f t=%.3f b=%.3f | R: l=%.3f r=%.3f t=%.3f b=%.3f", t[0][0], t[0][1], t[0][2], t[0][3], t[1][0], t[1][1], t[1][2], t[1][3]);
        Check(Near(t[0][0], -1.39) && Near(t[0][1], 1.24) && Near(t[0][2], -1.47) && Near(t[0][3], 1.46) &&
              Near(t[1][0], -1.24) && Near(t[1][1], 1.39) && Near(t[1][2], -1.47) && Near(t[1][3], 1.46),
              "GetProjectionRaw: Index-like defaults, right eye mirrored in x, top negative");
        for (int eye = 0; eye < 2; ++eye) {
            HmdMatrix34_t m = sys->GetEyeToHeadTransform((EVREye)eye);
            bool ident = Near(m.m[0][0], 1) && Near(m.m[1][1], 1) && Near(m.m[2][2], 1) && Near(m.m[0][1], 0) && Near(m.m[0][2], 0) &&
                         Near(m.m[1][0], 0) && Near(m.m[1][2], 0) && Near(m.m[2][0], 0) && Near(m.m[2][1], 0);
            Info("eye-to-head %s: t=(%.4f, %.4f, %.4f) rot %s", eye ? "R" : "L", m.m[0][3], m.m[1][3], m.m[2][3], ident ? "identity" : "NOT identity");
            Check(ident && Near(m.m[0][3], eye ? 0.0315 : -0.0315) && Near(m.m[1][3], 0) && Near(m.m[2][3], 0),
                  "GetEyeToHeadTransform(%s): identity rotation, x = %+.4f (ipd 0.063)", eye ? "R" : "L", eye ? 0.0315 : -0.0315);
        }
        Check(sys->GetTrackedDeviceActivityLevel(0) == EDeviceActivityLevel_k_EDeviceActivityLevel_UserInteraction, "GetTrackedDeviceActivityLevel(HMD) == UserInteraction");
        ETrackedPropertyError pe = ETrackedPropertyError_TrackedProp_UnknownProperty;
        float hz = sys->GetFloatTrackedDeviceProperty(0, ETrackedDeviceProperty_Prop_DisplayFrequency_Float, &pe);
        Check(pe == ETrackedPropertyError_TrackedProp_Success && Near(hz, 90.0), "Prop_DisplayFrequency_Float = %.3f (err %d)", hz, (int)pe);
        int32_t ad = -1; sys->GetDXGIOutputInfo(&ad);
        Check(ad == 0, "GetDXGIOutputInfo -> %d", ad);
        TrackedDevicePose_t pose; memset(&pose, 0xCD, sizeof(pose));
        sys->GetDeviceToAbsoluteTrackingPose(ETrackingUniverseOrigin_TrackingUniverseSeated, 0.f, &pose, 1);
        Check(pose.bPoseIsValid && pose.bDeviceIsConnected && Near(pose.mDeviceToAbsoluteTracking.m[0][0], 1) &&
              Near(pose.mDeviceToAbsoluteTracking.m[1][3], 0) && Near(pose.mDeviceToAbsoluteTracking.m[2][2], 1),
              "GetDeviceToAbsoluteTrackingPose (pose=static): valid, identity at y=0");
        // A couple of generic stubs (must return cleanly with stdcall stack cleanup).
        Check(!sys->IsDisplayOnDesktop() && comp->CanRenderScene() && sys->GetSortedTrackedDeviceIndicesOfClass(ETrackedDeviceClass_TrackedDeviceClass_HMD, nullptr, 0, 0) == 1,
              "misc IVRSystem/IVRCompositor calls return sane values");
        HmdColor_t fade = comp->GetCurrentFadeColor(false);
        Check(fade.r == 0 && fade.a == 0, "generic stub (GetCurrentFadeColor) returns zero struct");
        pShutdown();
    }

    // ---------------------------------------------------------------------------------- Other / Background
    {
        EVRInitError e = EVRInitError_VRInitError_None;
        pInit(&e, EVRApplicationType_VRApplication_Other);
        Check(e == EVRInitError_VRInitError_None, "VR_InitInternal(Other) -> %d", (int)e);
        TrackedDevicePose_t p[k_unMaxTrackedDeviceCount];
        EVRCompositorError ce = comp->WaitGetPoses(p, k_unMaxTrackedDeviceCount, nullptr, 0);
        Check(ce == EVRCompositorError_VRCompositorError_IsNotSceneApplication, "WaitGetPoses as Other app -> %d (IsNotSceneApplication)", (int)ce);
        pShutdown();
        pInit(&e, EVRApplicationType_VRApplication_Background);
        Check(e == EVRInitError_VRInitError_None, "VR_InitInternal(Background) -> %d", (int)e);
        pShutdown();
    }

    // ---------------------------------------------------------------------------------- main run
    const UINT W = 640, H = 720;
    const int kFrames = 300, kDumpEvery = 25;
    DeleteDumps();
    WriteIni("; test_client main run\nwidth=640\nheight=720\npose=sweep\nsweep_deg=20\nsweep_period_s=2\ndump_every=25\n");
    EVRInitError ie = EVRInitError_VRInitError_None;
    pInit(&ie, EVRApplicationType_VRApplication_Scene);
    Check(ie == EVRInitError_VRInitError_None, "main run: VR_InitInternal(Scene) -> %d", (int)ie);
    sys = reinterpret_cast<VR_IVRSystem_FnTable*>(GetTable(IVRSystem_Version, nullptr));
    comp = reinterpret_cast<VR_IVRCompositor_FnTable*>(GetTable(IVRCompositor_Version, nullptr));
    if (!sys || !comp) { printf("RESULT: FAIL (no tables in main run)\n"); return 1; }
    comp->SetTrackingSpace(ETrackingUniverseOrigin_TrackingUniverseSeated);
    {
        uint32_t w = 0, h = 0;
        sys->GetRecommendedRenderTargetSize(&w, &h);
        Check(w == W && h == H, "sizes: fakevr.ini width/height honoured: %ux%u", w, h);
    }

    // D3D11 device: hardware, then WARP.
    ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx);
    const char* drv = "hardware";
    if (FAILED(hr)) {
        Info("D3D11CreateDevice(HARDWARE) failed hr=0x%08lx, trying WARP", (unsigned long)hr);
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx);
        drv = "WARP";
    }
    const bool d3d = SUCCEEDED(hr) && dev;
    Info("D3D11 device: %s (%s, feature level 0x%x, hr=0x%08lx)", d3d ? "OK" : "UNAVAILABLE", drv, (unsigned)fl, (unsigned long)hr);
    Check(d3d, "D3D11CreateDevice succeeded (%s)", drv);

    const RGBA8 red{255, 0, 0}, blue{0, 0, 255}, green{0, 255, 0}, yellow{255, 255, 0};
    ID3D11Texture2D *texL = nullptr, *texR = nullptr, *texR_rgba = nullptr, *texSBS = nullptr;
    if (d3d) {
        texL = MakeTex(dev, W, H, DXGI_FORMAT_B8G8R8A8_UNORM, red, red);
        texR = MakeTex(dev, W, H, DXGI_FORMAT_B8G8R8A8_UNORM, blue, blue);
        texR_rgba = MakeTex(dev, W, H, DXGI_FORMAT_R8G8B8A8_UNORM, blue, blue);
        texSBS = MakeTex(dev, 2 * W, H, DXGI_FORMAT_B8G8R8A8_UNORM, green, yellow);
        Check(texL && texR && texR_rgba && texSBS, "created textures: L red BGRA8, R blue BGRA8 (+RGBA8 variant), SBS green|yellow BGRA8 %ux%u", 2 * W, H);
    }
    const bool haveTex = d3d && texL && texR && texR_rgba && texSBS;

    // Fake Vulkan handles (fakevr only checks they are non-null).
    int dummyObj[4];
    VRVulkanTextureData_t vk{};
    vk.m_nImage = 0x1234; vk.m_pDevice = (VkDevice_T*)&dummyObj[0]; vk.m_pPhysicalDevice = (VkPhysicalDevice_T*)&dummyObj[1];
    vk.m_pInstance = (VkInstance_T*)&dummyObj[2]; vk.m_pQueue = (VkQueue_T*)&dummyObj[3];
    vk.m_nQueueFamilyIndex = 0; vk.m_nWidth = W; vk.m_nHeight = H; vk.m_nFormat = 37 /*VK_FORMAT_R8G8B8A8_UNORM*/; vk.m_nSampleCount = 1;

    TrackedDevicePose_t poses[k_unMaxTrackedDeviceCount];
    int expectErrors = 0;
    // ---- frame 1: error paths + Vulkan validation
    {
        EVRCompositorError ce = comp->WaitGetPoses(poses, k_unMaxTrackedDeviceCount, nullptr, 0);
        Check(ce == EVRCompositorError_VRCompositorError_None, "frame 1: WaitGetPoses -> %d", (int)ce);
        Texture_t t{}; t.eColorSpace = EColorSpace_ColorSpace_Gamma;
        VRTextureBounds_t full{0.f, 0.f, 1.f, 1.f};
        ce = comp->Submit(EVREye_Eye_Left, nullptr, &full, EVRSubmitFlags_Submit_Default);
        Check(ce == EVRCompositorError_VRCompositorError_InvalidTexture, "Submit(null Texture_t) -> %d (InvalidTexture 102)", (int)ce); ++expectErrors;
        t.eType = ETextureType_TextureType_DirectX; t.handle = nullptr;
        ce = comp->Submit(EVREye_Eye_Left, &t, &full, EVRSubmitFlags_Submit_Default);
        Check(ce == EVRCompositorError_VRCompositorError_InvalidTexture, "Submit(DirectX, null handle) -> %d (InvalidTexture 102)", (int)ce); ++expectErrors;
        if (d3d) {
            t.handle = dev;     // a COM object that is not an ID3D11Texture2D
            ce = comp->Submit(EVREye_Eye_Left, &t, &full, EVRSubmitFlags_Submit_Default);
            Check(ce == EVRCompositorError_VRCompositorError_InvalidTexture, "Submit(DirectX, ID3D11Device as texture) -> %d (InvalidTexture 102)", (int)ce); ++expectErrors;
        }
        t.handle = haveTex ? (void*)texL : (void*)&vk;
        t.eType = haveTex ? ETextureType_TextureType_DirectX : ETextureType_TextureType_Vulkan;
        VRTextureBounds_t badB{0.f, 0.f, 1.5f, 1.f}, emptyB{0.5f, 0.f, 0.5f, 1.f};
        ce = comp->Submit(EVREye_Eye_Left, &t, &badB, EVRSubmitFlags_Submit_Default);
        Check(ce == EVRCompositorError_VRCompositorError_InvalidBounds, "Submit(bounds uMax=1.5) -> %d (InvalidBounds 109)", (int)ce); ++expectErrors;
        ce = comp->Submit(EVREye_Eye_Left, &t, &emptyB, EVRSubmitFlags_Submit_Default);
        Check(ce == EVRCompositorError_VRCompositorError_InvalidBounds, "Submit(empty bounds) -> %d (InvalidBounds 109)", (int)ce); ++expectErrors;
        ce = comp->Submit((EVREye)5, &t, &full, EVRSubmitFlags_Submit_Default);
        Check(ce == EVRCompositorError_VRCompositorError_IndexOutOfRange, "Submit(eye=5) -> %d (IndexOutOfRange 107)", (int)ce); ++expectErrors;
        VRVulkanTextureData_t vkBad = vk; vkBad.m_pQueue = nullptr;
        Texture_t tv{&vkBad, ETextureType_TextureType_Vulkan, EColorSpace_ColorSpace_Auto};
        ce = comp->Submit(EVREye_Eye_Left, &tv, &full, EVRSubmitFlags_Submit_Default);
        Check(ce == EVRCompositorError_VRCompositorError_InvalidTexture, "Submit(Vulkan, null queue) -> %d (InvalidTexture 102)", (int)ce); ++expectErrors;
        tv.handle = &vk;
        EVRCompositorError c1 = comp->Submit(EVREye_Eye_Left, &tv, &full, EVRSubmitFlags_Submit_Default);
        EVRCompositorError c2 = comp->Submit(EVREye_Eye_Right, &tv, &full, EVRSubmitFlags_Submit_Default);
        Check(c1 == EVRCompositorError_VRCompositorError_None && c2 == EVRCompositorError_VRCompositorError_None, "Submit(Vulkan, valid handles) L/R -> %d/%d", (int)c1, (int)c2);
        ce = comp->Submit(EVREye_Eye_Left, &tv, &full, EVRSubmitFlags_Submit_Default);
        Check(ce == EVRCompositorError_VRCompositorError_AlreadySubmitted, "Submit(L twice in one frame) -> %d (AlreadySubmitted 108)", (int)ce); ++expectErrors;
    }
    comp->SetExplicitTimingMode(EVRCompositorTimingMode_VRCompositorTimingMode_Explicit_ApplicationPerformsPostPresentHandoff);   // as WARVR's Vulkan path
    {
        // DXVK-style nested init while the Scene session runs: must keep the Scene session.
        EVRInitError ne = EVRInitError_VRInitError_None;
        intptr_t tok = pInit(&ne, EVRApplicationType_VRApplication_Background);
        EVRInitError ge = EVRInitError_VRInitError_None;
        intptr_t cpp = pGetIface("IVRCompositor_029", &ge);
        TrackedDevicePose_t p1[1];
        EVRCompositorError we = comp->GetLastPoses(p1, 1, nullptr, 0);
        Check(ne == EVRInitError_VRInitError_None && tok != 0 && cpp == 0 && ge == EVRInitError_VRInitError_Init_InterfaceNotFound && we == EVRCompositorError_VRCompositorError_None,
              "nested VR_InitInternal(Background) during Scene (DXVK pattern): OK, C++ IVRCompositor_029 -> nullptr/105, scene session kept");
    }
    Compositor_CumulativeStats s0; memset(&s0, 0, sizeof(s0));
    comp->GetCumulativeStats(&s0, sizeof(s0));

    // ---- frames 2..301
    double yawMin = 1e9, yawMax = -1e9;
    int waitErr = 0, subErr = 0, poseBad = 0, slowFrames = 0;
    const double t0 = QpcMs();
    for (int i = 0; i < kFrames; ++i) {
        EVRCompositorError ce = comp->WaitGetPoses(poses, k_unMaxTrackedDeviceCount, nullptr, 0);
        if (!Q(ce == EVRCompositorError_VRCompositorError_None, "i=%d WaitGetPoses -> %d", i, (int)ce)) ++waitErr;
        const TrackedDevicePose_t& hp = poses[k_unTrackedDeviceIndex_Hmd];
        if (!Q(hp.bPoseIsValid && hp.bDeviceIsConnected && hp.eTrackingResult == ETrackingResult_TrackingResult_Running_OK && !poses[1].bPoseIsValid,
               "i=%d HMD pose invalid or device 1 valid", i)) ++poseBad;
        double yaw = std::atan2(hp.mDeviceToAbsoluteTracking.m[0][2], hp.mDeviceToAbsoluteTracking.m[0][0]) * 180.0 / 3.14159265358979;
        if (yaw < yawMin) yawMin = yaw;
        if (yaw > yawMax) yawMax = yaw;

        EVRCompositorError a, b;
        if (haveTex) {
            Texture_t tl{nullptr, ETextureType_TextureType_DirectX, EColorSpace_ColorSpace_Gamma}, tr = tl;
            if (i % 2 == 0) {
                VRTextureBounds_t full{0.f, 0.f, 1.f, 1.f};
                tl.handle = texL; tr.handle = (i % 4 == 2) ? texR_rgba : texR;
                a = comp->Submit(EVREye_Eye_Left, &tl, &full, EVRSubmitFlags_Submit_Default);
                b = comp->Submit(EVREye_Eye_Right, &tr, &full, EVRSubmitFlags_Submit_Default);
            } else {
                VRTextureBounds_t bl{0.f, 0.f, 0.5f, 1.f}, br{0.5f, 0.f, 1.f, 1.f};
                tl.handle = tr.handle = texSBS;
                a = comp->Submit(EVREye_Eye_Left, &tl, &bl, EVRSubmitFlags_Submit_Default);
                b = comp->Submit(EVREye_Eye_Right, &tr, &br, EVRSubmitFlags_Submit_Default);
            }
        } else {
            Texture_t tv{&vk, ETextureType_TextureType_Vulkan, EColorSpace_ColorSpace_Auto};
            VRTextureBounds_t full{0.f, 0.f, 1.f, 1.f};
            a = comp->Submit(EVREye_Eye_Left, &tv, &full, EVRSubmitFlags_Submit_Default);
            b = comp->Submit(EVREye_Eye_Right, &tv, &full, EVRSubmitFlags_Submit_Default);
        }
        if (!Q(a == EVRCompositorError_VRCompositorError_None && b == EVRCompositorError_VRCompositorError_None, "i=%d Submit L/R -> %d/%d", i, (int)a, (int)b)) ++subErr;
        comp->PostPresentHandoff();
        if (i == 100 || i == 150 || i == 200 || i == 250 || i == 280) { Sleep(16); ++slowFrames; }   // a "slow" app frame: > 11.1 ms
    }
    const double loopMs = QpcMs() - t0;
    Compositor_CumulativeStats s1; memset(&s1, 0, sizeof(s1));
    comp->GetCumulativeStats(&s1, sizeof(s1));

    Check(waitErr == 0, "300 frames: every WaitGetPoses returned None (%d errors)", waitErr);
    Check(subErr == 0, "300 frames: every Submit pair returned None (%d errors) [%s]", subErr, haveTex ? "D3D11: separate on even, SBS+bounds on odd" : "Vulkan dummy handles (no D3D11)");
    Check(poseBad == 0, "300 frames: HMD pose valid+connected, other devices invalid (%d bad)", poseBad);
    Info("sweep yaw range over the run: %.2f .. %.2f deg (sweep_deg=20)", yawMin, yawMax);
    Check(yawMax > 15.0 && yawMin < -15.0 && yawMax <= 20.01 && yawMin >= -20.01, "pose=sweep moves the head yaw within +-20 deg");

    const uint32_t dFrames = kFrames;
    const uint32_t dPresents = s1.m_nNumFramePresents - s0.m_nNumFramePresents;
    const uint32_t dRepro = s1.m_nNumReprojectedFrames - s0.m_nNumReprojectedFrames;
    const uint32_t dDropped = s1.m_nNumDroppedFrames - s0.m_nNumDroppedFrames;
    const uint32_t dSubmits = s1.m_nNumFrameSubmits - s0.m_nNumFrameSubmits;
    const double dTarget = s1.m_flSumTargetFrameTimes - s0.m_flSumTargetFrameTimes;
    Info("stats: presents=%u (delta %u) reprojected=%u (delta %u) dropped=%u submits=%u (delta %u) sumTargetMs=%.3f (delta %.3f) sumAppCpuMs=%.3f pid=%u",
         s1.m_nNumFramePresents, dPresents, s1.m_nNumReprojectedFrames, dRepro, s1.m_nNumDroppedFrames, s1.m_nNumFrameSubmits, dSubmits,
         s1.m_flSumTargetFrameTimes, dTarget, s1.m_flSumApplicationCPUTimeMS, s1.m_nPid);
    Info("pacing: 300 frames in %.1f ms = %.2f fps (90 Hz target, %d deliberately slow frames)", loopMs, kFrames * 1000.0 / loopMs, slowFrames);
    Check(s1.m_nPid == GetCurrentProcessId(), "GetCumulativeStats m_nPid == our pid");
    Check(dSubmits == 600, "GetCumulativeStats: m_nNumFrameSubmits delta == 600 (got %u)", dSubmits);
    Check(s0.m_nNumFrameSubmits == 2, "GetCumulativeStats: frame-1 count == 2 valid submits, errors not counted (got %u)", s0.m_nNumFrameSubmits);
    Check(dPresents == dFrames + dRepro, "GetCumulativeStats: presents delta (%u) == frames (%u) + reprojected (%u)", dPresents, dFrames, dRepro);
    Check(dRepro >= 5 && dRepro <= 40, "missed slots from 5 slow frames: %u (expect >= 5, small jitter allowed)", dRepro);
    Check(dDropped == 0 && s1.m_nNumDroppedFrames == 0, "no frames without both eyes submitted (dropped=%u)", s1.m_nNumDroppedFrames);
    Check(Near(dTarget, dPresents * 1000.0 / 90.0, 0.01), "m_flSumTargetFrameTimes delta %.3f == presents * 11.111 ms", dTarget);
    Check(s1.m_flSumApplicationCPUTimeMS > 0 && s1.m_flSumApplicationCPUTimeMS < loopMs + 1000, "m_flSumApplicationCPUTimeMS plausible (%.1f ms)", s1.m_flSumApplicationCPUTimeMS);
    const double expectMs = (dFrames + dRepro) * 1000.0 / 90.0;
    Check(loopMs > expectMs * 0.95 && loopMs < expectMs * 1.10, "WaitGetPoses pacing: loop %.1f ms vs (frames+missed)/90Hz = %.1f ms (+-5/10%%)", loopMs, expectMs);
    {
        unsigned char raw[sizeof(Compositor_CumulativeStats)];
        memset(raw, 0xCD, sizeof(raw));
        const uint32_t part = offsetof(Compositor_CumulativeStats, m_nNumReprojectedFrames);
        comp->GetCumulativeStats(reinterpret_cast<Compositor_CumulativeStats*>(raw), part);
        bool untouched = true;
        for (size_t k = part; k < sizeof(raw); ++k) if (raw[k] != 0xCD) untouched = false;
        uint32_t pid; memcpy(&pid, raw, 4);
        Check(untouched && pid == GetCurrentProcessId(), "GetCumulativeStats respects nStatsSizeInBytes=%u (bytes beyond untouched)", part);
    }

    pShutdown();

    // ---------------------------------------------------------------------------------- dumps
    if (haveTex) {
        int ok = 0, bad = 0;
        for (int f = kDumpEvery; f <= kFrames + 1; f += kDumpEvery) {
            const int i = f - 2;                // frame f is loop index f-2 (frame 1 was the error-path frame)
            for (int eye = 0; eye < 2; ++eye) {
                RGBA8 want = (i % 2 == 0) ? (eye ? blue : red) : (eye ? yellow : green);
                char name[64]; snprintf(name, sizeof(name), "fakevr_dump_%06d_%c.bmp", f, eye ? 'R' : 'L');
                std::string why = CheckBmp(PathIn(name), (int)W, (int)H, want);
                if (why.empty()) ++ok;
                else { ++bad; Info("  dump %s: %s", name, why.c_str()); }
            }
        }
        int n = CountDumps();
        Check(bad == 0 && ok == 24 && n == 24, "BMP dumps: %d/24 correct (%d files): L red/R blue (BGRA8 and RGBA8) on separate-texture frames, L green/R yellow on SBS+bounds frames", ok, n);
    } else {
        Info("BMP dump checks skipped: no D3D11 device");
    }

    // ---------------------------------------------------------------------------------- log file
    {
        std::string log = ReadFile(PathIn("fakevr.log"));
        Check(!log.empty(), "fakevr.log exists next to the DLL (%u bytes)", (unsigned)log.size());
        int statLines = 0;
        for (size_t p = 0; (p = log.find("stats: app_fps=", p)) != std::string::npos; ++p) ++statLines;
        Check(statLines >= 2, "fakevr.log has per-second stats lines (%d)", statLines);
        for (size_t p = 0, k = 0; k < 2 && (p = log.find("stats: app_fps=", p)) != std::string::npos; ++k) {
            size_t ls = log.rfind('\n', p); ls = ls == std::string::npos ? 0 : ls + 1;
            size_t le = log.find('\n', p);
            printf("  log> %s\n", log.substr(ls, le - ls).c_str());
            p = le;
        }
        int descLines = 0;
        for (size_t p = 0; (p = log.find(": D3D11 texture ", p)) != std::string::npos; ++p) ++descLines;
        Check(descLines == 5, "texture descs logged once per distinct desc per eye, no per-frame spam (%d lines, expect L:2 + R:3)", descLines);
        char want[96];
        snprintf(want, sizeof(want), "app_frames=%d ", kFrames + 1);
        Check(log.find(want) != std::string::npos, "fakevr.log summary has %s", want);
        snprintf(want, sizeof(want), "submits_ok=%d submit_errors=%d ", 2 * kFrames + 2, expectErrors);
        Check(log.find(want) != std::string::npos, "fakevr.log summary has %s", want);
        // Echo the last summary block.
        size_t sp = log.rfind("=== fakevr session summary");
        if (sp != std::string::npos) {
            printf("---- fakevr.log (last summary) ----\n%s", log.substr(sp).c_str());
            printf("-----------------------------------\n");
        }
    }

    if (texL) texL->Release();
    if (texR) texR->Release();
    if (texR_rgba) texR_rgba->Release();
    if (texSBS) texSBS->Release();
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    DeleteIni();

    if (g_quietFails) Check(false, "%d per-frame checks failed", g_quietFails);
    printf("RESULT: %s (%d checks, %d failed)\n", g_fails ? "FAIL" : "PASS", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
