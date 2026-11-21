// openvr_head.h - OpenVR session for WARVR (C API, loaded at runtime, 32-bit safe).
//
// Two modes:
//   * Other  : pose reading only (head tracking steers the camera on the monitor).
//   * Scene  : full VR application. Adds the compositor: WaitGetPoses + Submit of per-eye textures.
//
// WAR.exe is 32-bit; SteamVR (and so the Pimax via Pimax Play's SteamVR mode) still supports 32-bit OpenVR
// clients. Put the 32-bit openvr_api.dll next to WARVR.dll (the build copies it).
#pragma once
#define OPENVR_API_NODLL           // we bind by GetProcAddress; do not dllimport the S_API functions
#include <windows.h>
#ifdef __WIN32
#undef __WIN32                     // MinGW defines this; the C header would then typedef char bool (breaks C++)
#endif
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#endif
#include "../third_party/openvr/openvr_capi.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#include <cstdio>
#include <cstring>
#include "vrmath.h"

class OpenVRHead {
public:
    using LogFn = void (*)(const char* fmt, ...);

    struct Pose { vrm::Mat3 rot; float pos[3]; bool valid; };

    bool Init(const char* dllDir, LogFn log, EVRApplicationType type = EVRApplicationType_VRApplication_Other)
    {
        if (ready_ && type_ == type) return true;
        if (ready_) Shutdown();                              // switching Other <-> Scene
        log_ = log;
        if (!dll_) {
            char path[MAX_PATH]{};
            _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\openvr_api.dll", dllDir ? dllDir : ".");
            dll_ = LoadLibraryA(path);
            if (!dll_) dll_ = LoadLibraryA("openvr_api.dll");
            if (!dll_) { log_("OpenVR: cannot load openvr_api.dll (err=%lu) tried '%s'\n", GetLastError(), path); return false; }
            pInit_     = reinterpret_cast<PFN_Init>(reinterpret_cast<void*>(GetProcAddress(dll_, "VR_InitInternal")));
            pShutdown_ = reinterpret_cast<PFN_Shutdown>(reinterpret_cast<void*>(GetProcAddress(dll_, "VR_ShutdownInternal")));
            pGetIface_ = reinterpret_cast<PFN_GetIface>(reinterpret_cast<void*>(GetProcAddress(dll_, "VR_GetGenericInterface")));
            pErrText_  = reinterpret_cast<PFN_ErrText>(reinterpret_cast<void*>(GetProcAddress(dll_, "VR_GetVRInitErrorAsEnglishDescription")));
        }
        if (!pInit_ || !pShutdown_ || !pGetIface_) {
            log_("OpenVR: openvr_api.dll is missing exports (wrong file? must be the 32-bit build)\n");
            return false;
        }
        EVRInitError err = EVRInitError_VRInitError_None;
        pInit_(&err, type);
        if (err != EVRInitError_VRInitError_None) {
            log_("OpenVR: VR_InitInternal(%s) failed err=%d (%s). Is SteamVR running with the headset on?\n",
                 type == EVRApplicationType_VRApplication_Scene ? "scene" : "other", (int)err, pErrText_ ? pErrText_(err) : "?");
            return false;
        }
        inited_ = true;
        type_ = type;

        sys_ = reinterpret_cast<VR_IVRSystem_FnTable*>(GetTable(IVRSystem_Version));
        if (!sys_) { pShutdown_(); inited_ = false; return false; }
        if (type == EVRApplicationType_VRApplication_Scene) {
            comp_ = reinterpret_cast<VR_IVRCompositor_FnTable*>(GetTable(IVRCompositor_Version));
            if (!comp_) { pShutdown_(); inited_ = false; sys_ = nullptr; return false; }
            comp_->SetTrackingSpace(ETrackingUniverseOrigin_TrackingUniverseSeated);
        }
        ready_ = true;
        log_("OpenVR: initialised as %s (%s%s)\n", type == EVRApplicationType_VRApplication_Scene ? "SCENE app" : "pose reader",
             IVRSystem_Version, comp_ ? ", compositor ok" : "");
        return true;
    }

    bool ready() const { return ready_; }
    bool isScene() const { return ready_ && type_ == EVRApplicationType_VRApplication_Scene && comp_; }

    // Pose-reader mode: latest HMD pose (seated universe).
    bool Poll(float predictSeconds, Pose* out)
    {
        if (!ready_) return false;
        TrackedDevicePose_t pose;
        std::memset(&pose, 0, sizeof(pose));
        sys_->GetDeviceToAbsoluteTrackingPose(ETrackingUniverseOrigin_TrackingUniverseSeated, predictSeconds, &pose, 1);
        return Extract(pose, out);
    }

    // Scene mode: blocks until the compositor wants the next frame; returns the pose to render it with.
    EVRCompositorError WaitPoses(Pose* out)
    {
        TrackedDevicePose_t poses[k_unMaxTrackedDeviceCount];
        std::memset(poses, 0, sizeof(poses));
        EVRCompositorError e = comp_->WaitGetPoses(poses, k_unMaxTrackedDeviceCount, nullptr, 0);
        if (e == EVRCompositorError_VRCompositorError_None) Extract(poses[k_unTrackedDeviceIndex_Hmd], out);
        return e;
    }

    // Per-eye field of view as tangents (left<0<right; top/bottom exactly as OpenVR reports them) and the
    // eye position relative to the head centre in metres (OpenVR head space: +x right, +y up, -z forward).
    void EyeParams(int eye, float* left, float* right, float* top, float* bottom, float eyeToHead[3], float rot[9] = nullptr)
    {
        sys_->GetProjectionRaw(eye ? EVREye_Eye_Right : EVREye_Eye_Left, left, right, top, bottom);
        HmdMatrix34_t m = sys_->GetEyeToHeadTransform(eye ? EVREye_Eye_Right : EVREye_Eye_Left);
        eyeToHead[0] = m.m[0][3]; eyeToHead[1] = m.m[1][3]; eyeToHead[2] = m.m[2][3];
        if (rot) for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) rot[i * 3 + j] = m.m[i][j];
    }
    // -1 unknown, 0 idle, 1 headset worn / in use, 3 standby ...
    int HmdActivity() { return sys_ ? (int)sys_->GetTrackedDeviceActivityLevel(k_unTrackedDeviceIndex_Hmd) : -1; }
    float DisplayFrequency()
    {
        if (!sys_) return 0.f;
        ETrackedPropertyError err = ETrackedPropertyError_TrackedProp_Success;
        float f = sys_->GetFloatTrackedDeviceProperty(k_unTrackedDeviceIndex_Hmd, ETrackedDeviceProperty_Prop_DisplayFrequency_Float, &err);
        return err == ETrackedPropertyError_TrackedProp_Success ? f : 0.f;
    }
    // Cumulative compositor statistics (presented / dropped / reprojected frames, target frame time, app GPU time).
    bool Stats(Compositor_CumulativeStats* out)
    {
        if (!comp_) return false;
        std::memset(out, 0, sizeof(*out));
        comp_->GetCumulativeStats(out, sizeof(*out));
        return true;
    }
    void RecommendedSize(uint32_t* w, uint32_t* h) { sys_->GetRecommendedRenderTargetSize(w, h); }
    int AdapterIndex() { int32_t idx = 0; sys_->GetDXGIOutputInfo(&idx); return idx; }

    // d3d11Texture is an ID3D11Texture2D*.
    EVRCompositorError Submit(int eye, void* d3d11Texture)
    {
        Texture_t t; t.handle = d3d11Texture; t.eType = ETextureType_TextureType_DirectX; t.eColorSpace = EColorSpace_ColorSpace_Gamma;
        VRTextureBounds_t b = { 0.f, 0.f, 1.f, 1.f };
        return comp_->Submit(eye ? EVREye_Eye_Right : EVREye_Eye_Left, &t, &b, EVRSubmitFlags_Submit_Default);
    }
    // DXVK hand-off: one Vulkan image holding both eyes side by side; each eye is submitted with its half as the bounds.
    EVRCompositorError SubmitVulkan(int eye, VRVulkanTextureData_t* data, float u0, float u1)
    {
        Texture_t t; t.handle = data; t.eType = ETextureType_TextureType_Vulkan; t.eColorSpace = EColorSpace_ColorSpace_Gamma;
        VRTextureBounds_t b = { u0, 0.f, u1, 1.f };
        return comp_->Submit(eye ? EVREye_Eye_Right : EVREye_Eye_Left, &t, &b, EVRSubmitFlags_Submit_Default);
    }
    // Vulkan submitters: with "application performs PostPresentHandoff", WaitGetPoses stops touching the app's queue.
    void SetExplicitTiming(EVRCompositorTimingMode m) { if (comp_ && comp_->SetExplicitTimingMode) comp_->SetExplicitTimingMode(m); }
    void PostPresentHandoff() { if (comp_) comp_->PostPresentHandoff(); }

    void Shutdown()
    {
        if (inited_ && pShutdown_) pShutdown_();
        inited_ = ready_ = false; sys_ = nullptr; comp_ = nullptr;
    }

private:
    using PFN_Init     = intptr_t (*)(EVRInitError*, EVRApplicationType);
    using PFN_Shutdown = void (*)();
    using PFN_GetIface = intptr_t (*)(const char*, EVRInitError*);
    using PFN_ErrText  = const char* (*)(EVRInitError);

    void* GetTable(const char* version)
    {
        char name[128]{};
        _snprintf_s(name, sizeof(name), _TRUNCATE, "FnTable:%s", version);
        EVRInitError err = EVRInitError_VRInitError_None;
        void* p = reinterpret_cast<void*>(pGetIface_(name, &err));
        if (!p || err != EVRInitError_VRInitError_None)
            log_("OpenVR: GetGenericInterface(%s) failed err=%d (%s)\n", name, (int)err, pErrText_ ? pErrText_(err) : "?");
        return err == EVRInitError_VRInitError_None ? p : nullptr;
    }
    static bool Extract(const TrackedDevicePose_t& p, Pose* out)
    {
        out->valid = p.bPoseIsValid && p.bDeviceIsConnected;
        if (!out->valid) return false;
        out->rot = vrm::fromOpenVR(p.mDeviceToAbsoluteTracking.m);
        for (int i = 0; i < 3; ++i) out->pos[i] = p.mDeviceToAbsoluteTracking.m[i][3];
        return true;
    }

    HMODULE dll_ = nullptr;
    PFN_Init pInit_ = nullptr; PFN_Shutdown pShutdown_ = nullptr;
    PFN_GetIface pGetIface_ = nullptr; PFN_ErrText pErrText_ = nullptr;
    VR_IVRSystem_FnTable* sys_ = nullptr;
    VR_IVRCompositor_FnTable* comp_ = nullptr;
    EVRApplicationType type_ = EVRApplicationType_VRApplication_Other;
    bool inited_ = false, ready_ = false;
    LogFn log_ = nullptr;
};
