#pragma once
// DXVK hand-off ("vk path").
//
// With DXVK's d3d9.dll in the game folder instead of dgVoodoo's, the game's D3D9 calls become Vulkan and there is no D3D11
// device to copy from (the dgVoodoo GPU path in gpupath.h finds nothing). DXVK has a public interop API for exactly this
// case (ID3D9VkInteropDevice / ID3D9VkInteropTexture, src/d3d9/d3d9_interfaces.h in the DXVK repository): it hands out
// the VkImage behind a D3D9 surface plus the instance, device and queue, so the eye picture goes to SteamVR as a Vulkan
// texture with no copy at all and no CPU involved.
//
// The submit sequence is the one Proton's vrclient uses for D3D11 games running on DXVK (vrclient_x64/vrcompositor_manual.c):
//   transition the image to TRANSFER_SRC_OPTIMAL -> FlushRenderingCommands -> LockSubmissionQueue -> IVRCompositor::Submit
//   (left and right half of the same image, via texture bounds) -> ReleaseSubmissionQueue -> transition back.
// SteamVR uses the game's own Vulkan queue during Submit and PostPresentHandoff, so DXVK must not touch the queue at the
// same time (the lock). With explicit timing mode "application performs PostPresentHandoff", WaitGetPoses does not use
// the queue, so only PostPresentHandoff needs the lock as well (again as Proton does it).
//
// Requirements, all checked at runtime with a log line when one is missing:
//  * openvr_api.dll must already be loaded when DXVK creates its Vulkan instance (inside Direct3DCreate9): DXVK asks
//    SteamVR then which Vulkan extensions the compositor needs. PreloadOpenVR() does that from the Direct3DCreate9 hook.
//  * SteamVR must be running when the game starts, otherwise DXVK cannot ask and the extensions are missing.
//  * The submitted image needs VK_IMAGE_USAGE_SAMPLED_BIT. DXVK gives that to TEXTURES but not to plain render-target
//    surfaces, so with DXVK the VR render target is created as a render-target texture (see VRRedirectBegin).
//    When the game renders at its own resolution (no VR target), the back buffer is copied into an own texture first.

#include <windows.h>
#include <d3d9.h>
#include <cstdint>
#include <cstring>

namespace vkp {

// ---- the few Vulkan types the interop API needs (no Vulkan SDK required). Non-dispatchable handles are 64-bit on x86.
typedef struct VkInstance_T* VkInstance;
typedef struct VkPhysicalDevice_T* VkPhysicalDevice;
typedef struct VkDevice_T* VkDevice;
typedef struct VkQueue_T* VkQueue;
typedef uint64_t VkImage;
typedef int32_t VkImageLayout;
enum : int32_t { LAYOUT_UNDEFINED = 0, LAYOUT_TRANSFER_SRC_OPTIMAL = 6 };
enum : uint32_t { USAGE_TRANSFER_SRC = 0x1, USAGE_SAMPLED = 0x4, ASPECT_COLOR = 0x1 };
enum : int32_t { STRUCTURE_TYPE_IMAGE_CREATE_INFO = 14 };
struct VkExtent3D { uint32_t width, height, depth; };
struct VkImageCreateInfo {          // Vulkan 1.0 layout
    int32_t sType; const void* pNext; uint32_t flags; int32_t imageType; int32_t format; VkExtent3D extent;
    uint32_t mipLevels; uint32_t arrayLayers; int32_t samples; int32_t tiling; uint32_t usage; int32_t sharingMode;
    uint32_t queueFamilyIndexCount; const uint32_t* pQueueFamilyIndices; int32_t initialLayout;
};
struct VkImageSubresourceRange { uint32_t aspectMask, baseMipLevel, levelCount, baseArrayLayer, layerCount; };

// ---- DXVK interop interfaces (method order and IIDs exactly as in DXVK's d3d9_interfaces.h)
// {d56344f5-8d35-46fd-806d-94c351b472c1}
static const GUID IID_InteropTexture = { 0xd56344f5, 0x8d35, 0x46fd, { 0x80, 0x6d, 0x94, 0xc3, 0x51, 0xb4, 0x72, 0xc1 } };
// {2eaa4b89-0107-4bdb-87f7-0f541c493ce0}
static const GUID IID_InteropDevice = { 0x2eaa4b89, 0x0107, 0x4bdb, { 0x87, 0xf7, 0x0f, 0x54, 0x1c, 0x49, 0x3c, 0xe0 } };

struct InteropTexture : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetVulkanImageInfo(VkImage* pHandle, VkImageLayout* pLayout, VkImageCreateInfo* pInfo) = 0;
};
struct InteropDevice : public IUnknown {
    virtual void STDMETHODCALLTYPE GetVulkanHandles(VkInstance* pInstance, VkPhysicalDevice* pPhysDev, VkDevice* pDevice) = 0;
    virtual void STDMETHODCALLTYPE GetSubmissionQueue(VkQueue* pQueue, uint32_t* pQueueIndex, uint32_t* pQueueFamilyIndex) = 0;
    virtual void STDMETHODCALLTYPE TransitionTextureLayout(InteropTexture* pTexture, const VkImageSubresourceRange* pSubresources,
                                                           VkImageLayout OldLayout, VkImageLayout NewLayout) = 0;
    virtual void STDMETHODCALLTYPE FlushRenderingCommands() = 0;
    virtual void STDMETHODCALLTYPE LockSubmissionQueue() = 0;
    virtual void STDMETHODCALLTYPE ReleaseSubmissionQueue() = 0;
    virtual void STDMETHODCALLTYPE LockDevice() = 0;
    virtual void STDMETHODCALLTYPE UnlockDevice() = 0;
    // (WaitForResource and CreateImage follow; not used)
};

// What Submit needs to know about one frame's image.
struct Frame {
    VkImage image = 0; VkImageLayout layout = LAYOUT_UNDEFINED;
    uint32_t width = 0, height = 0, format = 0, samples = 1, mips = 1, layers = 1, usage = 0;
    InteropTexture* tex = nullptr;     // not owned
};

using LogFn = void (*)(const char*, ...);

// ---- detection --------------------------------------------------------------------------------------------------------

// Does the module that implements `fn` look like DXVK? Its binary contains its DXVK_* environment-variable names
// (DXVK_HUD, DXVK_LOG_LEVEL, ...); dgVoodoo's and Windows' d3d9.dll do not. Cheap (one mapped read of a few MB, once).
inline bool ModuleLooksLikeDxvk(const void* fn, char* pathOut = nullptr, size_t pathCap = 0)
{
    HMODULE m = nullptr;
    if (!fn || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)fn, &m) || !m) return false;
    char path[MAX_PATH] = {};
    if (!GetModuleFileNameA(m, path, MAX_PATH)) return false;
    if (pathOut && pathCap) { strncpy(pathOut, path, pathCap - 1); pathOut[pathCap - 1] = 0; }
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    LARGE_INTEGER sz{};
    if (GetFileSizeEx(f, &sz) && sz.QuadPart > 16 && sz.QuadPart < (256ll << 20)) {
        HANDLE map = CreateFileMappingA(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (map) {
            const unsigned char* p = (const unsigned char*)MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0);
            if (p) {
                static const char kNeedle[] = "DXVK_";
                const size_t n = (size_t)sz.QuadPart, k = sizeof(kNeedle) - 1;
                for (size_t i = 0; i + k <= n && !found; ++i)
                    if (p[i] == 'D' && memcmp(p + i, kNeedle, k) == 0) found = true;
                UnmapViewOfFile(p);
            }
            CloseHandle(map);
        }
    }
    CloseHandle(f);
    return found;
}

// Load openvr_api.dll (from the mod's folder) so that DXVK finds it when it creates its Vulkan instance.
// Loading it does not start SteamVR; DXVK only connects as a background app if SteamVR is already running.
inline HMODULE PreloadOpenVR(const char* dir, LogFn log)
{
    if (HMODULE h = GetModuleHandleA("openvr_api.dll")) return h;
    char p[MAX_PATH + 32];
    snprintf(p, sizeof(p), "%s\\openvr_api.dll", dir);
    HMODULE h = LoadLibraryA(p);
    if (!h) h = LoadLibraryA("openvr_api.dll");
    log("VK path: preloaded openvr_api.dll for DXVK -> %s (err=%lu)\n", h ? "ok" : "FAILED", h ? 0ul : GetLastError());
    return h;
}

// ---- the hand-off -----------------------------------------------------------------------------------------------------

struct Handoff {
    InteropDevice* dev = nullptr;                 // owned reference
    VkInstance inst = nullptr; VkPhysicalDevice phys = nullptr; VkDevice device = nullptr;
    VkQueue queue = nullptr; uint32_t queueIndex = 0, queueFamily = 0;
    IDirect3DTexture9* copyTex = nullptr;          // only when the picture is not already in a texture (no VR target)
    IDirect3DSurface9* copySurf = nullptr;
    UINT copyW = 0, copyH = 0;
    bool explicitTimingSet = false;

    bool Attach(IDirect3DDevice9* d3d, LogFn log)
    {
        if (dev) return true;
        if (!d3d || FAILED(d3d->QueryInterface(IID_InteropDevice, reinterpret_cast<void**>(&dev))) || !dev) { dev = nullptr; return false; }
        dev->GetVulkanHandles(&inst, &phys, &device);
        dev->GetSubmissionQueue(&queue, &queueIndex, &queueFamily);
        log("VK path: DXVK interop device found: VkInstance=%p VkPhysicalDevice=%p VkDevice=%p VkQueue=%p (family %u, index %u)\n",
            (void*)inst, (void*)phys, (void*)device, (void*)queue, queueFamily, queueIndex);
        if (!inst || !phys || !device || !queue) { log("VK path: DXVK returned a null Vulkan handle -> not usable\n"); Detach(); return false; }
        return true;
    }
    void ReleaseCopy() { if (copySurf) { copySurf->Release(); copySurf = nullptr; } if (copyTex) { copyTex->Release(); copyTex = nullptr; } copyW = copyH = 0; }
    void Detach() { ReleaseCopy(); if (dev) { dev->Release(); dev = nullptr; } inst = nullptr; phys = nullptr; device = nullptr; queue = nullptr; explicitTimingSet = false; }
    bool Attached() const { return dev != nullptr; }

    // Describe the Vulkan image behind a D3D9 surface (a texture level). false + why when it cannot be submitted.
    bool Describe(IDirect3DSurface9* s, Frame* f, const char** why) const
    {
        *f = Frame{};
        InteropTexture* t = nullptr;
        if (FAILED(s->QueryInterface(IID_InteropTexture, reinterpret_cast<void**>(&t))) || !t) { *why = "surface has no DXVK interop interface"; return false; }
        VkImageCreateInfo ci{}; ci.sType = STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        HRESULT hr = t->GetVulkanImageInfo(&f->image, &f->layout, &ci);
        t->Release();                               // the surface keeps the texture alive; Frame::tex is a borrowed pointer
        if (FAILED(hr) || !f->image) { *why = "GetVulkanImageInfo failed"; return false; }
        f->tex = t; f->width = ci.extent.width; f->height = ci.extent.height; f->format = (uint32_t)ci.format;
        f->samples = ci.samples ? (uint32_t)ci.samples : 1u; f->mips = ci.mipLevels ? ci.mipLevels : 1u;
        f->layers = ci.arrayLayers ? ci.arrayLayers : 1u; f->usage = ci.usage;
        if (!(f->usage & USAGE_SAMPLED) || !(f->usage & USAGE_TRANSFER_SRC)) { *why = "image lacks SAMPLED/TRANSFER_SRC usage (needs a render-target TEXTURE)"; return false; }
        if (f->samples != 1) { *why = "image is multisampled"; return false; }
        return true;
    }

    // When the picture is in a surface that cannot be submitted (the real back buffer), copy it into our own render-target texture.
    IDirect3DSurface9* CopyIfNeeded(IDirect3DDevice9* d3d, IDirect3DSurface9* src, HRESULT (WINAPI* stretch)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const RECT*, D3DTEXTUREFILTERTYPE), LogFn log)
    {
        D3DSURFACE_DESC d{}; src->GetDesc(&d);
        if (!copyTex || copyW != d.Width || copyH != d.Height) {
            ReleaseCopy();
            HRESULT hr = d3d->CreateTexture(d.Width, d.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &copyTex, nullptr);
            if (FAILED(hr) || !copyTex) { log("VK path: CreateTexture(%ux%u) for the back-buffer copy failed hr=0x%08lX\n", d.Width, d.Height, (unsigned long)hr); copyTex = nullptr; return nullptr; }
            copyTex->GetSurfaceLevel(0, &copySurf); copyW = d.Width; copyH = d.Height;
            log("VK path: copying the back buffer (%ux%u) into an own texture each frame (no VR render target in use)\n", d.Width, d.Height);
        }
        if (!copySurf || FAILED(stretch(d3d, src, nullptr, copySurf, nullptr, D3DTEXF_NONE))) return nullptr;
        return copySurf;
    }
};

} // namespace vkp
