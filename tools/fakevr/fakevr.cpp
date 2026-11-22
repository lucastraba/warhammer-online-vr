// fakevr.cpp - a fake OpenVR runtime packaged as a 32-bit openvr_api.dll.
//
// Test double for WARVR (and any other client that binds the OpenVR *C* API through
// VR_GetGenericInterface("FnTable:...")). No headset, no SteamVR: it hands out an IVRSystem and an
// IVRCompositor function table, paces WaitGetPoses like a compositor running at a fixed refresh rate,
// validates every Submit, and writes frame-time statistics to fakevr.log next to the DLL.
//
// Config: fakevr.ini next to the DLL (see fakevr.ini.example). Re-read on every VR_InitInternal.
// Build:  tools/fakevr/build.sh (zig c++ -target x86-windows-gnu).
//
// Calling conventions: the S_API exports are cdecl (undecorated names via fakevr.def); every FnTable
// member is OPENVR_FNTABLE_CALLTYPE (__stdcall on Windows) - the callee pops its arguments, so a
// mismatch here corrupts the caller's stack on x86.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define OPENVR_API_NODLL
#include <windows.h>
#include <mmsystem.h>
#include <d3d11.h>
#ifdef __WIN32
#undef __WIN32                     // MinGW defines this; the C header would then typedef char bool (breaks C++)
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
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define FAKEVR_VERSION "fakevr 1.0"
#define CT OPENVR_FNTABLE_CALLTYPE

static_assert(sizeof(void*) == 4, "fakevr must be built as a 32-bit DLL (WAR.exe is 32-bit)");

// ------------------------------------------------------------------------------------------------
// Globals
// ------------------------------------------------------------------------------------------------
namespace {

struct Config {
    double refresh_hz = 90.0;
    uint32_t width = 2016, height = 2240;
    float ipd = 0.063f;
    float tan[2][4] = {{-1.39f, 1.24f, -1.47f, 1.46f},   // left eye:  l r t b (t negative: OpenVR convention)
                       {-1.24f, 1.39f, -1.47f, 1.46f}};  // right eye: mirrored in x
    char fov[32] = "index";
    float cant_deg = 0.f;                                 // eye rotation about +y (canted displays); 0 = parallel
    int pose = 0;                                         // 0 static, 1 sweep
    double sweep_deg = 30.0, sweep_period_s = 8.0, bob_m = 0.02;
    double head_yaw_deg = 0.0, head_pitch_deg = 0.0;      // pose=static: fixed head turn (+yaw = right, +pitch = up)
    uint64_t turn_after_submits = 0;                      // ...applied only after this many accepted submits (0 = from the start)
    uint32_t dump_every = 0;
    int fail_init = 0;
    int fail_init_error = EVRInitError_VRInitError_Init_HmdNotFound;
    bool log = true;
};

struct Hist {                                             // app CPU frame time histogram, 0.1 ms bins
    static const int kBins = 2000;                        // 0..200 ms, last bin = overflow
    uint32_t bin[kBins + 1];
    void add(double ms) { int i = (int)(ms * 10.0); if (i < 0) i = 0; if (i > kBins) i = kBins; ++bin[i]; }
    double pct(double p, uint32_t n) const {
        if (!n) return 0.0;
        uint64_t want = (uint64_t)std::ceil(p * n), acc = 0;
        if (want < 1) want = 1;
        for (int i = 0; i <= kBins; ++i) { acc += bin[i]; if (acc >= want) return (i + 0.5) / 10.0; }
        return kBins / 10.0;
    }
};

struct Window {                                           // one-second stats window
    int64_t start;
    uint32_t frames, submits, missed, errors, noSubmit, polls;
    double cpuSum, cpuMax; uint32_t cpuN;
};

// Small "have we logged this value already" set; after kMax distinct values it falls back to
// "changed since last time" so a pathological app still cannot flood the log every frame for long.
template <class T> struct Seen {
    static const int kMax = 8;
    T v[kMax]; int n = 0; T last; bool haveLast = false;
    bool add(const T& x) {                                // true = new, log it
        bool isNew = true;
        for (int i = 0; i < n; ++i) if (!memcmp(&v[i], &x, sizeof(T))) isNew = false;
        if (isNew && n < kMax) v[n++] = x;
        else if (isNew && haveLast && !memcmp(&last, &x, sizeof(T))) isNew = false;
        last = x; haveLast = true;
        return isNew;
    }
};
struct VkKey { uint32_t w, h, fmt, samples; };

struct Session {
    bool inited = false;
    EVRApplicationType type = EVRApplicationType_VRApplication_Other;
    ETrackingUniverseOrigin origin = ETrackingUniverseOrigin_TrackingUniverseSeated;
    int64_t t0 = 0;                                       // QPC at init
    double period = 0;                                    // QPC ticks per display slot
    // frame pacing
    uint64_t frames = 0;                                  // WaitGetPoses returns
    int64_t firstSlot = 0, lastSlot = 0;
    int64_t lastReturn = 0;                               // QPC when WaitGetPoses last returned (0 = none yet)
    uint32_t mask = 0;                                    // eyes submitted in the current frame (bit0 L, bit1 R)
    uint64_t missed = 0, dropped = 0;
    // submits
    uint64_t submitsOk = 0, submitErr = 0, polls = 0;
    uint32_t errByCode[16] = {};                          // EVRCompositorError 100..115
    uint32_t errOther = 0;
    // app CPU frame time
    double cpuSum = 0, cpuMax = 0; uint32_t cpuN = 0;
    Hist hist{};
    Window win{};
    // last poses (GetLastPoses)
    TrackedDevicePose_t lastPose{};
    // per-eye change tracking for the log
    // Logged once per distinct value per eye (apps often alternate a few textures/bounds every frame).
    Seen<D3D11_TEXTURE2D_DESC> descs[2];
    Seen<VRTextureBounds_t> bounds[2];
    Seen<VkKey> vks[2];
    uint32_t otherTypeLogged = 0;
    uint32_t errLogBudget = 20;                           // individual submit errors logged before going quiet
    uint32_t dumps = 0, dumpFails = 0;
    int timingMode = 0; bool timingModeSet = false;
};

HMODULE g_module = nullptr;
char g_dir[MAX_PATH] = ".";
CRITICAL_SECTION g_lock;
HANDLE g_log = INVALID_HANDLE_VALUE;
bool g_logTried = false;
bool g_cfgLoaded = false;
Config g_cfg;
Session g_s;
int64_t g_qpcFreq = 1, g_qpcStart = 0;
uint32_t g_initToken = 0;
bool g_timerPeriodSet = false;
bool g_exiting = false;                                  // DLL_PROCESS_DETACH at process exit: never take the lock
HANDLE g_hiresTimer = nullptr;

struct Lock { Lock() { EnterCriticalSection(&g_lock); } ~Lock() { LeaveCriticalSection(&g_lock); } };

int64_t Qpc() { LARGE_INTEGER v; QueryPerformanceCounter(&v); return v.QuadPart; }
double TicksToMs(double t) { return t * 1000.0 / (double)g_qpcFreq; }

// ------------------------------------------------------------------------------------------------
// Log (Win32 file I/O; each line is one WriteFile, so nothing is lost if the process dies)
// ------------------------------------------------------------------------------------------------
void LogRaw(const char* s, size_t n)
{
    if (!g_cfg.log) return;
    if (g_log == INVALID_HANDLE_VALUE && !g_logTried) {
        g_logTried = true;
        char path[MAX_PATH];
        _snprintf(path, sizeof(path), "%s\\fakevr.log", g_dir); path[MAX_PATH - 1] = 0;
        g_log = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (g_log == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(g_log, s, (DWORD)n, &w, nullptr);
}

void Log(const char* fmt, ...)
{
    if (!g_cfg.log) return;
    struct LogLock {
        bool held;
        LogLock() : held(!g_exiting) { if (held) EnterCriticalSection(&g_lock); }   // recursive: safe under Lock
        ~LogLock() { if (held) LeaveCriticalSection(&g_lock); }
    } ll;
    char buf[2048];
    int n = _snprintf(buf, sizeof(buf), "[%10.3f] ", TicksToMs((double)(Qpc() - g_qpcStart)) / 1000.0);
    if (n < 0) n = 0;
    va_list ap; va_start(ap, fmt);
    int m = _vsnprintf(buf + n, sizeof(buf) - n - 2, fmt, ap);
    va_end(ap);
    if (m < 0) m = (int)(sizeof(buf) - n - 3);
    n += m;
    if (n > 0 && buf[n - 1] != '\n') buf[n++] = '\n';
    LogRaw(buf, (size_t)n);
}

// ------------------------------------------------------------------------------------------------
// Config
// ------------------------------------------------------------------------------------------------
// Bounded append (MinGW's _snprintf returns -1 on truncation).
void Ap(char* b, size_t& len, size_t cap, const char* fmt, ...)
{
    if (len + 1 >= cap) return;
    va_list ap; va_start(ap, fmt);
    int n = _vsnprintf(b + len, cap - len - 1, fmt, ap);
    va_end(ap);
    if (n < 0) len = cap - 1; else len += (size_t)n;
    b[len] = 0;
}

char* Trim(char* s)
{
    while (*s == ' ' || *s == '\t') ++s;
    char* e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}

bool SetFovPreset(Config& c, const char* name)
{
    float l, r, t, b;
    if (!_stricmp(name, "index"))       { l = -1.39f; r = 1.24f; t = -1.47f; b = 1.46f; }   // Valve Index-like
    else if (!_stricmp(name, "narrow")) { l = -1.00f; r = 1.00f; t = -1.00f; b = 1.00f; }   // synthetic 90x90 deg
    else if (!_stricmp(name, "wide"))   { l = -1.80f; r = 1.50f; t = -1.50f; b = 1.50f; }   // synthetic wide headset
    else return false;
    c.tan[0][0] = l;  c.tan[0][1] = r;  c.tan[0][2] = t; c.tan[0][3] = b;
    c.tan[1][0] = -r; c.tan[1][1] = -l; c.tan[1][2] = t; c.tan[1][3] = b;
    _snprintf(c.fov, sizeof(c.fov), "%s", name); c.fov[sizeof(c.fov) - 1] = 0;
    return true;
}

void LoadConfig()
{
    Config c;
    char path[MAX_PATH];
    _snprintf(path, sizeof(path), "%s\\fakevr.ini", g_dir); path[MAX_PATH - 1] = 0;
    FILE* f = fopen(path, "rb");
    char warn[1024] = ""; size_t wl = 0;
    bool customTan = false;
    if (f) {
        char line[512];
        int ln = 0;
        while (fgets(line, sizeof(line), f)) {
            ++ln;
            char* sc = strpbrk(line, ";#"); if (sc) *sc = 0;
            char* eq = strchr(line, '=');
            char* k = Trim(line);
            if (!*k) continue;
            if (!eq) { Ap(warn, wl, sizeof(warn), " line %d: no '='", ln); continue; }
            *eq = 0; k = Trim(line); char* v = Trim(eq + 1);
            for (char* p = k; *p; ++p) *p = (char)tolower((unsigned char)*p);
            const char* tanKeys[8] = {"left_l", "left_r", "left_t", "left_b", "right_l", "right_r", "right_t", "right_b"};
            bool known = true, tanKey = false;
            for (int i = 0; i < 8; ++i)
                if (!strcmp(k, tanKeys[i])) { c.tan[i / 4][i % 4] = (float)atof(v); customTan = tanKey = true; }
            if (tanKey) {}
            else if (!strcmp(k, "refresh_hz")) c.refresh_hz = atof(v);
            else if (!strcmp(k, "width")) c.width = (uint32_t)strtoul(v, nullptr, 10);
            else if (!strcmp(k, "height")) c.height = (uint32_t)strtoul(v, nullptr, 10);
            else if (!strcmp(k, "ipd")) c.ipd = (float)atof(v);
            else if (!strcmp(k, "fov")) { if (!SetFovPreset(c, v)) Ap(warn, wl, sizeof(warn), " unknown fov preset '%s'", v); }
            else if (!strcmp(k, "cant_deg")) c.cant_deg = (float)atof(v);
            else if (!strcmp(k, "pose")) {
                if (!_stricmp(v, "static")) c.pose = 0; else if (!_stricmp(v, "sweep")) c.pose = 1;
                else Ap(warn, wl, sizeof(warn), " unknown pose '%s'", v);
            }
            else if (!strcmp(k, "sweep_deg")) c.sweep_deg = atof(v);
            else if (!strcmp(k, "sweep_period_s")) c.sweep_period_s = atof(v);
            else if (!strcmp(k, "bob_m")) c.bob_m = atof(v);
            else if (!strcmp(k, "head_yaw_deg")) c.head_yaw_deg = atof(v);
            else if (!strcmp(k, "head_pitch_deg")) c.head_pitch_deg = atof(v);
            else if (!strcmp(k, "turn_after_submits")) c.turn_after_submits = strtoull(v, nullptr, 10);
            else if (!strcmp(k, "dump_every")) c.dump_every = (uint32_t)strtoul(v, nullptr, 10);
            else if (!strcmp(k, "fail_init")) c.fail_init = atoi(v);
            else if (!strcmp(k, "fail_init_error")) c.fail_init_error = atoi(v);
            else if (!strcmp(k, "log")) c.log = !(!_stricmp(v, "off") || !strcmp(v, "0") || !_stricmp(v, "false"));
            else known = false;
            if (!known) Ap(warn, wl, sizeof(warn), " unknown key '%s'", k);
        }
        fclose(f);
    }
    if (!(c.refresh_hz >= 1.0 && c.refresh_hz <= 1000.0)) { Ap(warn, wl, sizeof(warn), " bad refresh_hz, using 90"); c.refresh_hz = 90.0; }
    if (!c.width || !c.height) { c.width = 2016; c.height = 2240; }
    if (!(c.sweep_period_s > 0.01)) c.sweep_period_s = 8.0;
    if (customTan) { _snprintf(c.fov, sizeof(c.fov), "custom"); }
    g_cfg = c;
    g_cfgLoaded = true;
    // Only log when the effective config (or its warnings) changed since the last time it was read.
    static char lastSig[1536] = "";
    char sig[1536];
    _snprintf(sig, sizeof(sig), "%d|%s|%.6f|%u|%u|%.6f|%s|%.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f|%.4f|%d|%.4f|%.4f|%.4f|%u|%d|%d|%d",
              f ? 1 : 0, warn, c.refresh_hz, c.width, c.height, c.ipd, c.fov, c.tan[0][0], c.tan[0][1], c.tan[0][2], c.tan[0][3],
              c.tan[1][0], c.tan[1][1], c.tan[1][2], c.tan[1][3], c.cant_deg, c.pose, c.sweep_deg, c.sweep_period_s, c.bob_m,
              c.dump_every, c.fail_init, c.fail_init_error, c.log ? 1 : 0);
    sig[sizeof(sig) - 1] = 0;
    if (!strcmp(sig, lastSig)) return;
    memcpy(lastSig, sig, sizeof(sig));
    Log("config: %s (%s)%s%s", path, f ? "loaded" : "not found, defaults", wl ? " warnings:" : "", warn);
    Log("config: refresh_hz=%.3f size=%ux%u ipd=%.4f fov=%s L[l=%.3f r=%.3f t=%.3f b=%.3f] R[l=%.3f r=%.3f t=%.3f b=%.3f] cant_deg=%.2f",
        c.refresh_hz, c.width, c.height, c.ipd, c.fov, c.tan[0][0], c.tan[0][1], c.tan[0][2], c.tan[0][3],
        c.tan[1][0], c.tan[1][1], c.tan[1][2], c.tan[1][3], c.cant_deg);
    Log("config: pose=%s sweep_deg=%.1f sweep_period_s=%.2f bob_m=%.3f head_yaw_deg=%.1f head_pitch_deg=%.1f turn_after_submits=%llu dump_every=%u fail_init=%d (error %d)",
        c.pose ? "sweep" : "static", c.sweep_deg, c.sweep_period_s, c.bob_m, c.head_yaw_deg, c.head_pitch_deg,
        (unsigned long long)c.turn_after_submits, c.dump_every, c.fail_init, c.fail_init_error);
}

void EnsureConfig() { if (!g_cfgLoaded) LoadConfig(); }

// ------------------------------------------------------------------------------------------------
// Poses / matrices
// ------------------------------------------------------------------------------------------------
HmdMatrix34_t RotY(double a, float x, float y, float z)
{
    HmdMatrix34_t m{};
    float c = (float)std::cos(a), s = (float)std::sin(a);
    m.m[0][0] = c;  m.m[0][1] = 0; m.m[0][2] = s; m.m[0][3] = x;
    m.m[1][0] = 0;  m.m[1][1] = 1; m.m[1][2] = 0; m.m[1][3] = y;
    m.m[2][0] = -s; m.m[2][1] = 0; m.m[2][2] = c; m.m[2][3] = z;
    return m;
}
HmdMatrix34_t Identity34() { return RotY(0, 0, 0, 0); }

// HMD pose at t seconds after init.
void HeadPose(double t, TrackedDevicePose_t* p)
{
    std::memset(p, 0, sizeof(*p));
    double yaw = 0, yawRate = 0, x = 0, y = 0, vx = 0, vy = 0;
    if (g_cfg.pose == 1) {
        const double w = 2.0 * 3.14159265358979 / g_cfg.sweep_period_s;
        const double A = g_cfg.sweep_deg * 3.14159265358979 / 180.0;
        yaw = A * std::sin(w * t);           yawRate = A * w * std::cos(w * t);
        y = g_cfg.bob_m * std::sin(2 * w * t); vy = g_cfg.bob_m * 2 * w * std::cos(2 * w * t);
        x = 0.5 * g_cfg.bob_m * std::sin(w * t); vx = 0.5 * g_cfg.bob_m * w * std::cos(w * t);
    }
    p->mDeviceToAbsoluteTracking = RotY(yaw, (float)x, (float)y, 0.f);
    if (g_cfg.pose == 0 && (g_cfg.head_yaw_deg != 0.0 || g_cfg.head_pitch_deg != 0.0) && g_s.submitsOk >= g_cfg.turn_after_submits) {
        // Fixed head turn: R = Ry(-yaw) * Rx(pitch). OpenVR looks down -z, so +yaw (right) is a negative rotation about +y.
        const double a = -g_cfg.head_yaw_deg * 3.14159265358979 / 180.0, b = g_cfg.head_pitch_deg * 3.14159265358979 / 180.0;
        const float ca = (float)std::cos(a), sa = (float)std::sin(a), cb = (float)std::cos(b), sb = (float)std::sin(b);
        HmdMatrix34_t& m = p->mDeviceToAbsoluteTracking;
        m.m[0][0] = ca;  m.m[0][1] = sa * sb; m.m[0][2] = sa * cb;
        m.m[1][0] = 0;   m.m[1][1] = cb;      m.m[1][2] = -sb;
        m.m[2][0] = -sa; m.m[2][1] = ca * sb; m.m[2][2] = ca * cb;
    }
    p->vVelocity.v[0] = (float)vx; p->vVelocity.v[1] = (float)vy;
    p->vAngularVelocity.v[1] = (float)yawRate;
    p->eTrackingResult = ETrackingResult_TrackingResult_Running_OK;
    p->bPoseIsValid = true;
    p->bDeviceIsConnected = true;
}

double SecondsSinceInit(int64_t q) { return (double)(q - g_s.t0) / (double)g_qpcFreq; }

void FillPoses(TrackedDevicePose_t* arr, uint32_t count, double t)
{
    if (!arr || !count) return;
    std::memset(arr, 0, sizeof(TrackedDevicePose_t) * count);
    HeadPose(t, &arr[0]);
}

// ------------------------------------------------------------------------------------------------
// Stats
// ------------------------------------------------------------------------------------------------
const char* CompErrName(int e)
{
    switch (e) {
    case 0: return "None"; case 1: return "RequestFailed"; case 100: return "IncompatibleVersion";
    case 101: return "DoNotHaveFocus"; case 102: return "InvalidTexture"; case 103: return "IsNotSceneApplication";
    case 104: return "TextureIsOnWrongDevice"; case 105: return "TextureUsesUnsupportedFormat";
    case 106: return "SharedTexturesNotSupported"; case 107: return "IndexOutOfRange"; case 108: return "AlreadySubmitted";
    case 109: return "InvalidBounds"; case 110: return "AlreadySet";
    default: return "Other";
    }
}

// Caller holds the lock.
void MaybeEmitSecond(int64_t now)
{
    Window& w = g_s.win;
    if (!w.start) { w.start = now; return; }
    double dt = (double)(now - w.start) / (double)g_qpcFreq;
    if (dt < 1.0) return;
    Log("stats: app_fps=%.1f submits/s=%.1f cpu_ms avg=%.2f max=%.2f missed=%u no_submit=%u submit_err=%u polls/s=%.0f | totals frames=%llu missed=%llu err=%llu",
        w.frames / dt, w.submits / dt, w.cpuN ? w.cpuSum / w.cpuN : 0.0, w.cpuMax, w.missed, w.noSubmit, w.errors, w.polls / dt,
        (unsigned long long)g_s.frames, (unsigned long long)g_s.missed, (unsigned long long)g_s.submitErr);
    std::memset(&w, 0, sizeof(w));
    w.start = now;
}

void FillCumulative(Compositor_CumulativeStats* st)
{
    std::memset(st, 0, sizeof(*st));
    st->m_nPid = GetCurrentProcessId();
    uint64_t presents = g_s.frames ? (uint64_t)(g_s.lastSlot - g_s.firstSlot + 1) : 0;   // == frames + missed
    st->m_nNumFramePresents = (uint32_t)presents;
    st->m_nNumDroppedFrames = (uint32_t)g_s.dropped;
    st->m_nNumReprojectedFrames = (uint32_t)g_s.missed;
    st->m_nNumFrameSubmits = (uint32_t)g_s.submitsOk;
    st->m_flSumTargetFrameTimes = (double)presents * TicksToMs(g_s.period);   // period fixed at init
    st->m_flSumApplicationCPUTimeMS = g_s.cpuSum;
}

void WriteSummary(const char* why)
{
    if (!g_s.inited) return;
    int64_t now = Qpc();
    double dur = SecondsSinceInit(now);
    uint64_t presents = g_s.frames ? (uint64_t)(g_s.lastSlot - g_s.firstSlot + 1) : 0;
    Log("=== fakevr session summary (%s, %s) ===", g_s.type == EVRApplicationType_VRApplication_Scene ? "scene" : "non-scene", why);
    Log("summary: duration=%.3f s refresh=%.3f Hz (%.3f ms/slot)", dur, g_cfg.refresh_hz, 1000.0 / g_cfg.refresh_hz);
    // fps over the presenting span (first WaitGetPoses .. last display slot), not including app start-up time.
    Log("summary: app_frames=%llu avg_app_fps=%.2f display_slots=%llu missed_slots(reprojected)=%llu (%.2f%%) frames_without_both_eyes(dropped)=%llu",
        (unsigned long long)g_s.frames, presents ? g_s.frames * g_cfg.refresh_hz / presents : 0.0, (unsigned long long)presents,
        (unsigned long long)g_s.missed, presents ? 100.0 * g_s.missed / presents : 0.0, (unsigned long long)g_s.dropped);
    char eb[512] = ""; size_t el = 0;
    for (int i = 0; i < 16; ++i)
        if (g_s.errByCode[i]) Ap(eb, el, sizeof(eb), " %s=%u", CompErrName(100 + i), g_s.errByCode[i]);
    if (g_s.errOther) Ap(eb, el, sizeof(eb), " other=%u", g_s.errOther);
    Log("summary: submits_ok=%llu submit_errors=%llu%s dumps=%u dump_failures=%u pose_polls=%llu",
        (unsigned long long)g_s.submitsOk, (unsigned long long)g_s.submitErr, eb, g_s.dumps, g_s.dumpFails, (unsigned long long)g_s.polls);
    Log("summary: app_cpu_frame_ms n=%u avg=%.3f p50=%.1f p95=%.1f p99=%.1f max=%.3f",
        g_s.cpuN, g_s.cpuN ? g_s.cpuSum / g_s.cpuN : 0.0, g_s.hist.pct(0.50, g_s.cpuN), g_s.hist.pct(0.95, g_s.cpuN),
        g_s.hist.pct(0.99, g_s.cpuN), g_s.cpuMax);
}

// ------------------------------------------------------------------------------------------------
// Pacing
// ------------------------------------------------------------------------------------------------
void WaitUntil(int64_t target)
{
    for (;;) {
        int64_t now = Qpc();
        int64_t rem = target - now;
        if (rem <= 0) return;
        double remMs = TicksToMs((double)rem);
        if (remMs > 2.0) {
            if (g_hiresTimer) {
                LARGE_INTEGER due; due.QuadPart = -(LONGLONG)((remMs - 1.0) * 10000.0);   // 100 ns units, relative
                if (SetWaitableTimer(g_hiresTimer, &due, 0, nullptr, nullptr, FALSE)) { WaitForSingleObject(g_hiresTimer, INFINITE); continue; }
            }
            Sleep((DWORD)(remMs - 1.5));
        } else if (remMs > 0.3) {
            Sleep(0);
        } else {
            YieldProcessor();
        }
    }
}

// ------------------------------------------------------------------------------------------------
// BMP dump of a D3D11 texture region
// ------------------------------------------------------------------------------------------------
bool IsBGRA(DXGI_FORMAT f) { return f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_TYPELESS || f == DXGI_FORMAT_B8G8R8X8_UNORM || f == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8X8_TYPELESS; }
bool IsRGBA(DXGI_FORMAT f) { return f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_R8G8B8A8_TYPELESS; }
DXGI_FORMAT Typed(DXGI_FORMAT f)
{
    if (f == DXGI_FORMAT_B8G8R8A8_TYPELESS) return DXGI_FORMAT_B8G8R8A8_UNORM;
    if (f == DXGI_FORMAT_B8G8R8X8_TYPELESS) return DXGI_FORMAT_B8G8R8X8_UNORM;
    if (f == DXGI_FORMAT_R8G8B8A8_TYPELESS) return DXGI_FORMAT_R8G8B8A8_UNORM;
    return f;
}

bool WriteBmp(const char* path, const uint8_t* src, uint32_t rowPitch, uint32_t w, uint32_t h, bool bgra, bool flipX, bool flipY)
{
    const uint32_t row = (w * 3 + 3) & ~3u;
    const uint32_t imgSize = row * h;
    uint8_t* buf = (uint8_t*)calloc(1, 54 + (size_t)imgSize);
    if (!buf) return false;
    uint8_t* hd = buf;
    auto put16 = [&](int o, uint16_t v) { hd[o] = (uint8_t)v; hd[o + 1] = (uint8_t)(v >> 8); };
    auto put32 = [&](int o, uint32_t v) { for (int i = 0; i < 4; ++i) hd[o + i] = (uint8_t)(v >> (8 * i)); };
    hd[0] = 'B'; hd[1] = 'M'; put32(2, 54 + imgSize); put32(10, 54);
    put32(14, 40); put32(18, w); put32(22, h); put16(26, 1); put16(28, 24); put32(34, imgSize);
    put32(38, 2835); put32(42, 2835);
    for (uint32_t r = 0; r < h; ++r) {                    // BMP rows are bottom-up
        uint32_t srcRow = flipY ? r : (h - 1 - r);
        const uint8_t* s = src + (size_t)srcRow * rowPitch;
        uint8_t* d = buf + 54 + (size_t)r * row;
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t* px = s + 4 * (flipX ? (w - 1 - x) : x);
            if (bgra) { d[3 * x] = px[0]; d[3 * x + 1] = px[1]; d[3 * x + 2] = px[2]; }
            else      { d[3 * x] = px[2]; d[3 * x + 1] = px[1]; d[3 * x + 2] = px[0]; }
        }
    }
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool ok = false;
    if (f != INVALID_HANDLE_VALUE) {
        DWORD wr = 0;
        ok = WriteFile(f, buf, 54 + imgSize, &wr, nullptr) && wr == 54 + imgSize;
        CloseHandle(f);
    }
    free(buf);
    return ok;
}

template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

bool DumpD3D11(ID3D11Texture2D* tex, const D3D11_TEXTURE2D_DESC& d, const VRTextureBounds_t& b, UINT arraySlice, const char* path, char* why, size_t whyLen)
{
    const bool bgra = IsBGRA(d.Format), rgba = IsRGBA(d.Format);
    if (!bgra && !rgba) { _snprintf(why, whyLen, "unsupported format %d (BGRA8/RGBA8 only)", (int)d.Format); return false; }
    float u0 = std::fmin(b.uMin, b.uMax), u1 = std::fmax(b.uMin, b.uMax);
    float v0 = std::fmin(b.vMin, b.vMax), v1 = std::fmax(b.vMin, b.vMax);
    LONG x0 = std::lround(u0 * d.Width), x1 = std::lround(u1 * d.Width);
    LONG y0 = std::lround(v0 * d.Height), y1 = std::lround(v1 * d.Height);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (LONG)d.Width) x1 = (LONG)d.Width;
    if (y1 > (LONG)d.Height) y1 = (LONG)d.Height;
    if (x1 <= x0 || y1 <= y0) { _snprintf(why, whyLen, "empty region"); return false; }
    const UINT w = (UINT)(x1 - x0), h = (UINT)(y1 - y0);

    ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    ID3D11Texture2D* resolved = nullptr; ID3D11Texture2D* stg = nullptr;
    bool ok = false;
    tex->GetDevice(&dev);
    if (!dev) { _snprintf(why, whyLen, "GetDevice failed"); return false; }
    dev->GetImmediateContext(&ctx);
    ID3D11Resource* src = tex; UINT srcSub = D3D11CalcSubresource(0, arraySlice, d.MipLevels ? d.MipLevels : 1);
    DXGI_FORMAT fmt = d.Format;
    HRESULT hr = S_OK;
    if (d.SampleDesc.Count > 1) {
        D3D11_TEXTURE2D_DESC rd{};
        rd.Width = d.Width; rd.Height = d.Height; rd.MipLevels = 1; rd.ArraySize = 1; rd.Format = Typed(d.Format);
        rd.SampleDesc.Count = 1; rd.Usage = D3D11_USAGE_DEFAULT;
        hr = dev->CreateTexture2D(&rd, nullptr, &resolved);
        if (FAILED(hr)) { _snprintf(why, whyLen, "resolve texture hr=0x%08lx", (unsigned long)hr); goto done; }
        ctx->ResolveSubresource(resolved, 0, tex, srcSub, Typed(d.Format));
        src = resolved; srcSub = 0; fmt = Typed(d.Format);
    }
    {
        D3D11_TEXTURE2D_DESC sd{};
        sd.Width = w; sd.Height = h; sd.MipLevels = 1; sd.ArraySize = 1; sd.Format = fmt;
        sd.SampleDesc.Count = 1; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        hr = dev->CreateTexture2D(&sd, nullptr, &stg);
        if (FAILED(hr)) { _snprintf(why, whyLen, "staging texture hr=0x%08lx", (unsigned long)hr); goto done; }
        D3D11_BOX box{(UINT)x0, (UINT)y0, 0, (UINT)x1, (UINT)y1, 1};
        ctx->CopySubresourceRegion(stg, 0, 0, 0, 0, src, srcSub, &box);
        D3D11_MAPPED_SUBRESOURCE m{};
        hr = ctx->Map(stg, 0, D3D11_MAP_READ, 0, &m);
        if (FAILED(hr)) { _snprintf(why, whyLen, "Map hr=0x%08lx", (unsigned long)hr); goto done; }
        ok = WriteBmp(path, (const uint8_t*)m.pData, m.RowPitch, w, h, bgra, b.uMin > b.uMax, b.vMin > b.vMax);
        ctx->Unmap(stg, 0);
        if (!ok) _snprintf(why, whyLen, "cannot write %s", path);
        else _snprintf(why, whyLen, "%ux%u region (%ld,%ld)-(%ld,%ld)", w, h, (long)x0, (long)y0, (long)x1, (long)y1);
    }
done:
    SafeRelease(stg); SafeRelease(resolved); SafeRelease(ctx); SafeRelease(dev);
    return ok;
}

// ------------------------------------------------------------------------------------------------
// Generic stubs: every FnTable slot gets one first (zero/false/error return, outputs untouched, first
// call logged); the functions fakevr actually implements overwrite their slot afterwards.
// ------------------------------------------------------------------------------------------------
const int kMaxStubs = 4096;
const char* g_stubName[kMaxStubs];
volatile LONG g_stubLogged[kMaxStubs];

void NoteStub(int id)
{
    if (id < 0 || id >= kMaxStubs) return;
    if (InterlockedExchange(&g_stubLogged[id], 1) == 0) Log("note: unimplemented %s called (stub returns 0/false/error)", g_stubName[id] ? g_stubName[id] : "?");
}

template <typename R> struct StubRet { static R get() { R r; std::memset(&r, 0, sizeof(r)); return r; } };
template <> struct StubRet<void> { static void get() {} };
template <> struct StubRet<char*> { static char* get() { static char empty[1] = {0}; return empty; } };
template <> struct StubRet<EVRCompositorError> { static EVRCompositorError get() { return EVRCompositorError_VRCompositorError_RequestFailed; } };
template <> struct StubRet<EVRInitError> { static EVRInitError get() { return EVRInitError_VRInitError_Unknown; } };

template <typename F, int ID> struct Stub;
template <typename R, typename... A, int ID> struct Stub<R(CT*)(A...), ID> {
    static R CT Fn(A...) { NoteStub(ID); return StubRet<R>::get(); }
};

#define FAKEVR_STUB(T, m)                                                         \
    do {                                                                          \
        enum { kId = __COUNTER__ };                                               \
        static_assert(kId < kMaxStubs, "too many stubs");                         \
        T.m = &Stub<decltype(T.m), kId>::Fn;                                      \
        g_stubName[kId] = #T "." #m;                                              \
    } while (0)

// Member lists generated from openvr_capi.h (IVRSystem_026 / IVRCompositor_029).
#define SYS_MEMBERS(X) X(GetRecommendedRenderTargetSize) X(GetProjectionMatrix) X(GetProjectionRaw) X(ComputeDistortion) \
    X(ComputeDistortionSet) X(GetEyeToHeadTransform) X(GetTimeSinceLastVsync) X(GetD3D9AdapterIndex) X(GetDXGIOutputInfo)      \
    X(GetOutputDevice) X(IsDisplayOnDesktop) X(SetDisplayVisibility) X(GetDeviceToAbsoluteTrackingPose)                      \
    X(GetSeatedZeroPoseToStandingAbsoluteTrackingPose) X(GetRawZeroPoseToStandingAbsoluteTrackingPose)                       \
    X(GetSortedTrackedDeviceIndicesOfClass) X(GetTrackedDeviceActivityLevel) X(ApplyTransform)                               \
    X(GetTrackedDeviceIndexForControllerRole) X(GetControllerRoleForTrackedDeviceIndex) X(GetTrackedDeviceClass)             \
    X(IsTrackedDeviceConnected) X(GetBoolTrackedDeviceProperty) X(GetFloatTrackedDeviceProperty)                             \
    X(GetInt32TrackedDeviceProperty) X(GetUint64TrackedDeviceProperty) X(GetMatrix34TrackedDeviceProperty)                   \
    X(GetArrayTrackedDeviceProperty) X(GetStringTrackedDeviceProperty) X(GetPropErrorNameFromEnum) X(PollNextEvent)          \
    X(PollNextEventWithPose) X(PollNextEventWithPoseAndOverlays) X(GetEventTypeNameFromEnum) X(GetHiddenAreaMesh)            \
    X(GetEyeTrackedFoveationCenter) X(GetEyeTrackedFoveationCenterForProjection) X(GetControllerState)                       \
    X(GetControllerStateWithPose) X(TriggerHapticPulse) X(GetButtonIdNameFromEnum) X(GetControllerAxisTypeNameFromEnum)      \
    X(IsInputAvailable) X(IsSteamVRDrawingControllers) X(ShouldApplicationPause) X(ShouldApplicationReduceRenderingWork)     \
    X(PerformFirmwareUpdate) X(AcknowledgeQuit_Exiting) X(GetAppContainerFilePaths) X(GetRuntimeVersion) X(SetSDKVersion)
#define COMP_MEMBERS(X) X(SetTrackingSpace) X(GetTrackingSpace) X(WaitGetPoses) X(GetLastPoses)                          \
    X(GetLastPoseForTrackedDeviceIndex) X(GetSubmitTexture) X(Submit) X(SubmitWithArrayIndex) X(ClearLastSubmittedFrame)    \
    X(PostPresentHandoff) X(GetFrameTiming) X(GetFrameTimings) X(GetFrameTimeRemaining) X(GetCumulativeStats)               \
    X(FadeToColor) X(GetCurrentFadeColor) X(FadeGrid) X(GetCurrentGridAlpha) X(SetSkyboxOverride) X(ClearSkyboxOverride)    \
    X(CompositorBringToFront) X(CompositorGoToBack) X(CompositorQuit) X(IsFullscreen) X(GetCurrentSceneFocusProcess)        \
    X(GetLastFrameRenderer) X(CanRenderScene) X(ShowMirrorWindow) X(HideMirrorWindow) X(IsMirrorWindowVisible)              \
    X(CompositorDumpImages) X(ShouldAppRenderWithLowResources) X(ForceInterleavedReprojectionOn) X(ForceReconnectProcess)   \
    X(SuspendRendering) X(GetMirrorTextureD3D11) X(ReleaseMirrorTextureD3D11) X(GetMirrorTextureGL)                         \
    X(ReleaseSharedGLTexture) X(LockGLSharedTextureForAccess) X(UnlockGLSharedTextureForAccess)                             \
    X(GetVulkanInstanceExtensionsRequired) X(GetVulkanDeviceExtensionsRequired) X(SetExplicitTimingMode)                    \
    X(SubmitExplicitTimingData) X(IsMotionSmoothingEnabled) X(IsMotionSmoothingSupported) X(IsCurrentSceneFocusAppLoading)  \
    X(SetStageOverride_Async) X(ClearStageOverride) X(GetCompositorBenchmarkResults) X(GetLastPosePredictionIDs)            \
    X(GetPosesForFrame)

#define COUNT_ONE(m) +1
static_assert(sizeof(VR_IVRSystem_FnTable) == (0 SYS_MEMBERS(COUNT_ONE)) * sizeof(void*), "IVRSystem FnTable layout changed: update SYS_MEMBERS");
static_assert(sizeof(VR_IVRCompositor_FnTable) == (0 COMP_MEMBERS(COUNT_ONE)) * sizeof(void*), "IVRCompositor FnTable layout changed: update COMP_MEMBERS");

VR_IVRSystem_FnTable g_sys;
VR_IVRCompositor_FnTable g_comp;
bool g_tablesBuilt = false;

// ------------------------------------------------------------------------------------------------
// IVRSystem implementation
// ------------------------------------------------------------------------------------------------
int EyeIdx(EVREye e) { return e == EVREye_Eye_Right ? 1 : 0; }

void CT Sys_GetRecommendedRenderTargetSize(uint32_t* w, uint32_t* h)
{
    EnsureConfig();
    if (w) *w = g_cfg.width;
    if (h) *h = g_cfg.height;
}
void CT Sys_GetProjectionRaw(EVREye eye, float* l, float* r, float* t, float* b)
{
    EnsureConfig();
    const float* v = g_cfg.tan[EyeIdx(eye)];
    if (l) *l = v[0];
    if (r) *r = v[1];
    if (t) *t = v[2];
    if (b) *b = v[3];
}
HmdMatrix44_t CT Sys_GetProjectionMatrix(EVREye eye, float zn, float zf)
{
    EnsureConfig();
    const float* v = g_cfg.tan[EyeIdx(eye)];
    const float l = v[0], r = v[1], t = v[2], b = v[3];
    HmdMatrix44_t m{};
    const float idx = 1.f / (r - l), idy = 1.f / (b - t), idz = 1.f / (zf - zn);
    m.m[0][0] = 2 * idx; m.m[0][2] = (r + l) * idx;
    m.m[1][1] = 2 * idy; m.m[1][2] = (b + t) * idy;
    m.m[2][2] = -zf * idz; m.m[2][3] = -zf * zn * idz;
    m.m[3][2] = -1.f;
    return m;
}
bool CT Sys_ComputeDistortion(EVREye, float u, float v, DistortionCoordinates_t* d)
{
    if (!d) return false;
    d->rfRed[0] = d->rfGreen[0] = d->rfBlue[0] = u;     // no lens: identity
    d->rfRed[1] = d->rfGreen[1] = d->rfBlue[1] = v;
    return true;
}
HmdMatrix34_t CT Sys_GetEyeToHeadTransform(EVREye eye)
{
    EnsureConfig();
    const int i = EyeIdx(eye);
    const double cant = g_cfg.cant_deg * 3.14159265358979 / 180.0 * (i ? -1.0 : 1.0);   // outward
    return RotY(cant, (i ? 0.5f : -0.5f) * g_cfg.ipd, 0.f, 0.f);
}
bool CT Sys_GetTimeSinceLastVsync(float* secs, uint64_t* frameCounter)
{
    if (!g_s.inited || g_s.period <= 0) { if (secs) *secs = 0; if (frameCounter) *frameCounter = 0; return false; }
    double ticks = (double)(Qpc() - g_s.t0);
    double n = std::floor(ticks / g_s.period);
    if (secs) *secs = (float)((ticks - n * g_s.period) / (double)g_qpcFreq);
    if (frameCounter) *frameCounter = (uint64_t)n;
    return true;
}
int32_t CT Sys_GetD3D9AdapterIndex() { return 0; }
void CT Sys_GetDXGIOutputInfo(int32_t* idx) { if (idx) *idx = 0; }
void CT Sys_GetOutputDevice(uint64_t* dev, ETextureType, VkInstance_T*) { if (dev) *dev = 0; }
bool CT Sys_IsDisplayOnDesktop() { return false; }
bool CT Sys_SetDisplayVisibility(bool) { return false; }
void CT Sys_GetDeviceToAbsoluteTrackingPose(ETrackingUniverseOrigin, float predict, TrackedDevicePose_t* arr, uint32_t count)
{
    if (!arr || !count) return;
    int64_t now = Qpc();
    {
        Lock lk;
        ++g_s.polls; ++g_s.win.polls;
        if (g_s.inited) MaybeEmitSecond(now);
    }
    FillPoses(arr, count, SecondsSinceInit(now) + (double)predict);
}
HmdMatrix34_t CT Sys_GetSeatedZeroPose() { return Identity34(); }
HmdMatrix34_t CT Sys_GetRawZeroPose() { return Identity34(); }
uint32_t CT Sys_GetSortedTrackedDeviceIndicesOfClass(ETrackedDeviceClass cls, TrackedDeviceIndex_t* arr, uint32_t n, TrackedDeviceIndex_t)
{
    if (cls != ETrackedDeviceClass_TrackedDeviceClass_HMD) return 0;
    if (arr && n >= 1) arr[0] = 0;
    return 1;
}
EDeviceActivityLevel CT Sys_GetTrackedDeviceActivityLevel(TrackedDeviceIndex_t dev)
{
    return dev == 0 ? EDeviceActivityLevel_k_EDeviceActivityLevel_UserInteraction : EDeviceActivityLevel_k_EDeviceActivityLevel_Unknown;
}
void CT Sys_ApplyTransform(TrackedDevicePose_t* out, TrackedDevicePose_t* in, HmdMatrix34_t* xf)
{
    if (!out) return;
    if (!in || !xf) { std::memset(out, 0, sizeof(*out)); return; }
    TrackedDevicePose_t r = *in;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j) {
            float s = j == 3 ? xf->m[i][3] : 0.f;
            for (int k = 0; k < 3; ++k) s += xf->m[i][k] * in->mDeviceToAbsoluteTracking.m[k][j];
            r.mDeviceToAbsoluteTracking.m[i][j] = s;
        }
    *out = r;
}
TrackedDeviceIndex_t CT Sys_GetTrackedDeviceIndexForControllerRole(ETrackedControllerRole) { return (TrackedDeviceIndex_t)k_unTrackedDeviceIndexInvalid; }
ETrackedControllerRole CT Sys_GetControllerRoleForTrackedDeviceIndex(TrackedDeviceIndex_t) { return ETrackedControllerRole_TrackedControllerRole_Invalid; }
ETrackedDeviceClass CT Sys_GetTrackedDeviceClass(TrackedDeviceIndex_t d) { return d == 0 ? ETrackedDeviceClass_TrackedDeviceClass_HMD : ETrackedDeviceClass_TrackedDeviceClass_Invalid; }
bool CT Sys_IsTrackedDeviceConnected(TrackedDeviceIndex_t d) { return d == 0; }

void SetPropErr(ETrackedPropertyError* e, ETrackedPropertyError v) { if (e) *e = v; }
ETrackedPropertyError PropDevErr(TrackedDeviceIndex_t d)
{
    return d == 0 ? ETrackedPropertyError_TrackedProp_UnknownProperty : ETrackedPropertyError_TrackedProp_InvalidDevice;
}
bool CT Sys_GetBoolProp(TrackedDeviceIndex_t d, ETrackedDeviceProperty, ETrackedPropertyError* e) { SetPropErr(e, PropDevErr(d)); return false; }
float CT Sys_GetFloatProp(TrackedDeviceIndex_t d, ETrackedDeviceProperty p, ETrackedPropertyError* e)
{
    EnsureConfig();
    if (d == 0) {
        if (p == ETrackedDeviceProperty_Prop_DisplayFrequency_Float) { SetPropErr(e, ETrackedPropertyError_TrackedProp_Success); return (float)g_cfg.refresh_hz; }
        if (p == ETrackedDeviceProperty_Prop_UserIpdMeters_Float) { SetPropErr(e, ETrackedPropertyError_TrackedProp_Success); return g_cfg.ipd; }
        if (p == ETrackedDeviceProperty_Prop_SecondsFromVsyncToPhotons_Float) { SetPropErr(e, ETrackedPropertyError_TrackedProp_Success); return (float)(1.0 / g_cfg.refresh_hz); }
    }
    SetPropErr(e, PropDevErr(d));
    return 0.f;
}
int32_t CT Sys_GetInt32Prop(TrackedDeviceIndex_t d, ETrackedDeviceProperty p, ETrackedPropertyError* e)
{
    if (d == 0 && p == ETrackedDeviceProperty_Prop_DeviceClass_Int32) { SetPropErr(e, ETrackedPropertyError_TrackedProp_Success); return ETrackedDeviceClass_TrackedDeviceClass_HMD; }
    SetPropErr(e, PropDevErr(d));
    return 0;
}
uint64_t CT Sys_GetUint64Prop(TrackedDeviceIndex_t d, ETrackedDeviceProperty, ETrackedPropertyError* e) { SetPropErr(e, PropDevErr(d)); return 0; }
HmdMatrix34_t CT Sys_GetMatrix34Prop(TrackedDeviceIndex_t d, ETrackedDeviceProperty, ETrackedPropertyError* e) { SetPropErr(e, PropDevErr(d)); return HmdMatrix34_t{}; }
uint32_t CT Sys_GetArrayProp(TrackedDeviceIndex_t d, ETrackedDeviceProperty, PropertyTypeTag_t, void*, uint32_t, ETrackedPropertyError* e) { SetPropErr(e, PropDevErr(d)); return 0; }
uint32_t CT Sys_GetStringProp(TrackedDeviceIndex_t d, ETrackedDeviceProperty p, char* buf, uint32_t n, ETrackedPropertyError* e)
{
    const char* s = nullptr;
    if (d == 0) {
        switch (p) {
        case ETrackedDeviceProperty_Prop_TrackingSystemName_String: s = "fakevr"; break;
        case ETrackedDeviceProperty_Prop_ModelNumber_String: s = "FakeVR HMD"; break;
        case ETrackedDeviceProperty_Prop_SerialNumber_String: s = "FAKEVR-0001"; break;
        case ETrackedDeviceProperty_Prop_ManufacturerName_String: s = "fakevr"; break;
        default: break;
        }
    }
    if (buf && n) buf[0] = 0;
    if (!s) { SetPropErr(e, PropDevErr(d)); return 0; }
    uint32_t need = (uint32_t)strlen(s) + 1;
    if (!buf || n < need) { SetPropErr(e, ETrackedPropertyError_TrackedProp_BufferTooSmall); return need; }
    memcpy(buf, s, need);
    SetPropErr(e, ETrackedPropertyError_TrackedProp_Success);
    return need;
}
char* CT Sys_GetPropErrorNameFromEnum(ETrackedPropertyError e)
{
    static char names[][40] = {"TrackedProp_Success", "TrackedProp_WrongDataType", "TrackedProp_WrongDeviceClass",
                               "TrackedProp_BufferTooSmall", "TrackedProp_UnknownProperty", "TrackedProp_InvalidDevice"};
    static char other[] = "TrackedProp_Other";
    return (e >= 0 && e < 6) ? names[e] : other;
}
bool CT Sys_PollNextEvent(VREvent_t* ev, uint32_t n) { if (ev && n) std::memset(ev, 0, n); return false; }
bool CT Sys_PollNextEventWithPose(ETrackingUniverseOrigin, VREvent_t* ev, uint32_t n, TrackedDevicePose_t* p)
{
    if (ev && n) std::memset(ev, 0, n);
    if (p) std::memset(p, 0, sizeof(*p));
    return false;
}
bool CT Sys_PollNextEventWithPoseAndOverlays(ETrackingUniverseOrigin, VREvent_t* ev, uint32_t n, TrackedDevicePose_t* p, VROverlayHandle_t* o)
{
    if (ev && n) std::memset(ev, 0, n);
    if (p) std::memset(p, 0, sizeof(*p));
    if (o) *o = 0;
    return false;
}
HiddenAreaMesh_t CT Sys_GetHiddenAreaMesh(EVREye, EHiddenAreaMeshType) { return HiddenAreaMesh_t{}; }
bool CT Sys_GetControllerState(TrackedDeviceIndex_t, VRControllerState_t* s, uint32_t n) { if (s && n) std::memset(s, 0, n); return false; }
bool CT Sys_GetControllerStateWithPose(ETrackingUniverseOrigin, TrackedDeviceIndex_t, VRControllerState_t* s, uint32_t n, TrackedDevicePose_t* p)
{
    if (s && n) std::memset(s, 0, n);
    if (p) std::memset(p, 0, sizeof(*p));
    return false;
}
void CT Sys_TriggerHapticPulse(TrackedDeviceIndex_t, uint32_t, unsigned short) {}
bool CT Sys_IsInputAvailable() { return true; }
bool CT Sys_False() { return false; }
void CT Sys_AcknowledgeQuit_Exiting() {}
uint32_t CT Sys_GetAppContainerFilePaths(char* buf, uint32_t n) { if (buf && n) buf[0] = 0; return 0; }
char* CT Sys_GetRuntimeVersion() { static char v[] = FAKEVR_VERSION; return v; }
EVRInitError CT Sys_SetSDKVersion(uint32_t, uint32_t, uint32_t) { return EVRInitError_VRInitError_None; }

// ------------------------------------------------------------------------------------------------
// IVRCompositor implementation
// ------------------------------------------------------------------------------------------------
void CT Comp_SetTrackingSpace(ETrackingUniverseOrigin o)
{
    Lock lk;
    if (o != g_s.origin) Log("compositor: SetTrackingSpace(%d)", (int)o);
    g_s.origin = o;
}
ETrackingUniverseOrigin CT Comp_GetTrackingSpace() { return g_s.origin; }

EVRCompositorError CT Comp_WaitGetPoses(TrackedDevicePose_t* render, uint32_t nRender, TrackedDevicePose_t* game, uint32_t nGame)
{
    int64_t target;
    {
        Lock lk;
        if (!g_s.inited) return EVRCompositorError_VRCompositorError_RequestFailed;
        if (g_s.type != EVRApplicationType_VRApplication_Scene) return EVRCompositorError_VRCompositorError_IsNotSceneApplication;
        int64_t now = Qpc();
        if (g_s.lastReturn) {
            double ms = TicksToMs((double)(now - g_s.lastReturn));
            g_s.cpuSum += ms; ++g_s.cpuN; if (ms > g_s.cpuMax) g_s.cpuMax = ms; g_s.hist.add(ms);
            Window& w = g_s.win; w.cpuSum += ms; ++w.cpuN; if (ms > w.cpuMax) w.cpuMax = ms;
        }
        if (g_s.frames > 0 && g_s.mask != 3) { ++g_s.dropped; ++g_s.win.noSubmit; }
        int64_t slot = (int64_t)std::floor((double)(now - g_s.t0) / g_s.period) + 1;     // next display slot after now
        if (g_s.frames == 0) g_s.firstSlot = slot;
        else {
            if (slot <= g_s.lastSlot) slot = g_s.lastSlot + 1;                          // never two frames per slot
            int64_t miss = slot - g_s.lastSlot - 1;
            if (miss > 0) { g_s.missed += (uint64_t)miss; g_s.win.missed += (uint32_t)miss; }
        }
        g_s.lastSlot = slot;
        ++g_s.frames; ++g_s.win.frames;
        g_s.mask = 0;
        target = g_s.t0 + (int64_t)std::llround((double)slot * g_s.period);
        MaybeEmitSecond(now);
    }
    WaitUntil(target);
    // Render poses are predicted for the photon time of this frame: one slot after the one we woke on.
    double t = SecondsSinceInit(target) + 1.0 / g_cfg.refresh_hz;
    TrackedDevicePose_t head;
    HeadPose(t, &head);
    if (render && nRender) { std::memset(render, 0, sizeof(*render) * nRender); render[0] = head; }
    if (game && nGame) { std::memset(game, 0, sizeof(*game) * nGame); game[0] = head; }
    Lock lk;
    g_s.lastPose = head;
    g_s.lastReturn = Qpc();
    return EVRCompositorError_VRCompositorError_None;
}
EVRCompositorError CT Comp_GetLastPoses(TrackedDevicePose_t* render, uint32_t nRender, TrackedDevicePose_t* game, uint32_t nGame)
{
    Lock lk;
    if (render && nRender) { std::memset(render, 0, sizeof(*render) * nRender); render[0] = g_s.lastPose; }
    if (game && nGame) { std::memset(game, 0, sizeof(*game) * nGame); game[0] = g_s.lastPose; }
    return EVRCompositorError_VRCompositorError_None;
}
EVRCompositorError CT Comp_GetLastPoseForTrackedDeviceIndex(TrackedDeviceIndex_t d, TrackedDevicePose_t* p, TrackedDevicePose_t* g)
{
    Lock lk;
    if (p) std::memset(p, 0, sizeof(*p));
    if (g) std::memset(g, 0, sizeof(*g));
    if (d >= k_unMaxTrackedDeviceCount) return EVRCompositorError_VRCompositorError_IndexOutOfRange;
    if (d == 0) { if (p) *p = g_s.lastPose; if (g) *g = g_s.lastPose; }
    return EVRCompositorError_VRCompositorError_None;
}

// Shared by Submit / SubmitWithArrayIndex.
EVRCompositorError DoSubmit(EVREye eye, Texture_t* tex, UINT arraySlice, VRTextureBounds_t* pb, EVRSubmitFlags flags)
{
    const int e = (int)eye;
    EVRCompositorError err = EVRCompositorError_VRCompositorError_None;
    char why[256] = "";
    VRTextureBounds_t b = {0.f, 0.f, 1.f, 1.f};
    uint64_t frame = 0;
    bool dumpThis = false;
    {
        Lock lk;
        frame = g_s.frames;
        if (!g_s.inited) err = EVRCompositorError_VRCompositorError_RequestFailed, _snprintf(why, sizeof(why), "not initialised");
        else if (g_s.type != EVRApplicationType_VRApplication_Scene) err = EVRCompositorError_VRCompositorError_IsNotSceneApplication, _snprintf(why, sizeof(why), "not a scene app");
        else if (e != 0 && e != 1) err = EVRCompositorError_VRCompositorError_IndexOutOfRange, _snprintf(why, sizeof(why), "eye %d", e);
        else if (!tex || !tex->handle) err = EVRCompositorError_VRCompositorError_InvalidTexture, _snprintf(why, sizeof(why), "%s", tex ? "null handle" : "null Texture_t");
        else {
            if (pb) b = *pb;
            const float v[4] = {b.uMin, b.vMin, b.uMax, b.vMax};
            bool okb = true;
            for (float x : v) if (!(x >= 0.f && x <= 1.f)) okb = false;                  // also rejects NaN
            if (!okb || b.uMin == b.uMax || b.vMin == b.vMax) {
                err = EVRCompositorError_VRCompositorError_InvalidBounds;
                _snprintf(why, sizeof(why), "bounds u[%g,%g] v[%g,%g]", b.uMin, b.uMax, b.vMin, b.vMax);
            } else if (g_s.mask & (1u << e)) {
                err = EVRCompositorError_VRCompositorError_AlreadySubmitted;
                _snprintf(why, sizeof(why), "eye %s submitted twice in frame %llu", e ? "R" : "L", (unsigned long long)frame);
            }
        }
        if (!err) {
            if (g_s.bounds[e].add(b)) {
                Log("submit[%s]: bounds u[%.4f..%.4f] v[%.4f..%.4f] flags=0x%x (frame %llu)", e ? "R" : "L", b.uMin, b.uMax, b.vMin, b.vMax,
                    (unsigned)flags, (unsigned long long)frame);
            }
            dumpThis = g_cfg.dump_every && frame && (frame % g_cfg.dump_every) == 0 && tex->eType == ETextureType_TextureType_DirectX;
        }
    }
    bool dumped = false; char dumpWhy[256] = "", dumpPath[MAX_PATH] = "";
    if (!err) {
        if (tex->eType == ETextureType_TextureType_DirectX) {
            IUnknown* unk = (IUnknown*)tex->handle;
            ID3D11Texture2D* t2d = nullptr;
            HRESULT hr = unk->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t2d);
            if (FAILED(hr) || !t2d) {
                err = EVRCompositorError_VRCompositorError_InvalidTexture;
                _snprintf(why, sizeof(why), "handle is not an ID3D11Texture2D (QI hr=0x%08lx)", (unsigned long)hr);
            } else {
                D3D11_TEXTURE2D_DESC d{};
                t2d->GetDesc(&d);
                if (arraySlice >= d.ArraySize) {
                    err = EVRCompositorError_VRCompositorError_IndexOutOfRange;
                    _snprintf(why, sizeof(why), "array index %u >= ArraySize %u", arraySlice, d.ArraySize);
                } else {
                    Lock lk;
                    if (g_s.descs[e].add(d)) {
                        Log("submit[%s]: D3D11 texture %ux%u fmt=%d mips=%u array=%u samples=%u/%u usage=%d bind=0x%x cpu=0x%x misc=0x%x colorspace=%d (frame %llu)",
                            e ? "R" : "L", d.Width, d.Height, (int)d.Format, d.MipLevels, d.ArraySize, d.SampleDesc.Count, d.SampleDesc.Quality,
                            (int)d.Usage, d.BindFlags, d.CPUAccessFlags, d.MiscFlags, (int)tex->eColorSpace, (unsigned long long)frame);
                    }
                }
                if (!err && dumpThis) {
                    _snprintf(dumpPath, sizeof(dumpPath), "%s\\fakevr_dump_%06llu_%c.bmp", g_dir, (unsigned long long)frame, e ? 'R' : 'L');
                    dumpPath[MAX_PATH - 1] = 0;
                    dumped = DumpD3D11(t2d, d, b, arraySlice, dumpPath, dumpWhy, sizeof(dumpWhy));
                }
                t2d->Release();
            }
        } else if (tex->eType == ETextureType_TextureType_Vulkan) {
            const VRVulkanTextureData_t* vk = (const VRVulkanTextureData_t*)tex->handle;
            if (!vk->m_nImage || !vk->m_pDevice || !vk->m_pPhysicalDevice || !vk->m_pInstance || !vk->m_pQueue) {
                err = EVRCompositorError_VRCompositorError_InvalidTexture;
                _snprintf(why, sizeof(why), "Vulkan handle null: image=%s device=%s phys=%s instance=%s queue=%s",
                          vk->m_nImage ? "ok" : "NULL", vk->m_pDevice ? "ok" : "NULL", vk->m_pPhysicalDevice ? "ok" : "NULL",
                          vk->m_pInstance ? "ok" : "NULL", vk->m_pQueue ? "ok" : "NULL");
            } else {
                Lock lk;
                VkKey cur{vk->m_nWidth, vk->m_nHeight, vk->m_nFormat, vk->m_nSampleCount};
                if (g_s.vks[e].add(cur)) {
                    Log("submit[%s]: Vulkan image %ux%u VkFormat=%u samples=%u queueFamily=%u (validated, not dumped) (frame %llu)",
                        e ? "R" : "L", cur.w, cur.h, cur.fmt, cur.samples, vk->m_nQueueFamilyIndex, (unsigned long long)frame);
                }
            }
        } else {
            Lock lk;
            if (!(g_s.otherTypeLogged & (1u << (tex->eType & 31)))) {
                g_s.otherTypeLogged |= 1u << (tex->eType & 31);
                Log("submit[%s]: texture eType=%d accepted without validation", e ? "R" : "L", (int)tex->eType);
            }
        }
    }
    Lock lk;
    if (dumpThis) {
        if (dumped) { ++g_s.dumps; Log("dump: %s (%s)", dumpPath, dumpWhy); }
        else { ++g_s.dumpFails; Log("dump: frame %llu eye %s FAILED: %s", (unsigned long long)frame, e ? "R" : "L", dumpWhy); }
    }
    if (err) {
        ++g_s.submitErr; ++g_s.win.errors;
        if (err >= 100 && err < 116) ++g_s.errByCode[err - 100]; else ++g_s.errOther;
        if (g_s.errLogBudget) {
            --g_s.errLogBudget;
            Log("submit error %d (%s): %s%s", (int)err, CompErrName(err), why, g_s.errLogBudget ? "" : " [further submit errors only counted]");
        }
    } else {
        g_s.mask |= 1u << e;
        ++g_s.submitsOk; ++g_s.win.submits;
    }
    MaybeEmitSecond(Qpc());
    return err;
}
EVRCompositorError CT Comp_Submit(EVREye eye, Texture_t* tex, VRTextureBounds_t* b, EVRSubmitFlags f) { return DoSubmit(eye, tex, 0, b, f); }
EVRCompositorError CT Comp_SubmitWithArrayIndex(EVREye eye, Texture_t* tex, uint32_t idx, VRTextureBounds_t* b, EVRSubmitFlags f) { return DoSubmit(eye, tex, idx, b, f); }
void CT Comp_Void() {}
bool CT Comp_GetFrameTiming(Compositor_FrameTiming* t, uint32_t)
{
    if (t && t->m_nSize >= sizeof(uint32_t)) { uint32_t sz = t->m_nSize; std::memset(t, 0, sz < sizeof(*t) ? sz : sizeof(*t)); t->m_nSize = sz; }
    return false;
}
uint32_t CT Comp_GetFrameTimings(Compositor_FrameTiming*, uint32_t) { return 0; }
float CT Comp_GetFrameTimeRemaining()
{
    Lock lk;
    if (!g_s.inited || !g_s.frames) return 0.f;
    double end = (double)g_s.t0 + (double)(g_s.lastSlot + 1) * g_s.period;
    double rem = (end - (double)Qpc()) / (double)g_qpcFreq;
    return rem > 0 ? (float)rem : 0.f;
}
void CT Comp_GetCumulativeStats(Compositor_CumulativeStats* out, uint32_t size)
{
    if (!out || !size) return;
    Compositor_CumulativeStats st;
    { Lock lk; FillCumulative(&st); }
    memcpy(out, &st, size < sizeof(st) ? size : sizeof(st));   // respect the caller's struct size
}
bool CT Comp_True() { return true; }
bool CT Comp_False() { return false; }
uint32_t CT Comp_GetCurrentSceneFocusProcess() { return g_s.inited ? GetCurrentProcessId() : 0; }
uint32_t CT Comp_GetLastFrameRenderer() { return g_s.frames ? GetCurrentProcessId() : 0; }
EVRCompositorError CT Comp_GetMirrorTextureD3D11(EVREye, void*, void** srv) { if (srv) *srv = nullptr; return EVRCompositorError_VRCompositorError_RequestFailed; }
uint32_t CT Comp_GetVulkanInstanceExtensionsRequired(char* buf, uint32_t n) { if (buf && n) buf[0] = 0; return 1; }
uint32_t CT Comp_GetVulkanDeviceExtensionsRequired(VkPhysicalDevice_T*, char* buf, uint32_t n) { if (buf && n) buf[0] = 0; return 1; }
EVRCompositorError CT Comp_SubmitExplicitTimingData() { return EVRCompositorError_VRCompositorError_None; }
void CT Comp_SetExplicitTimingMode(EVRCompositorTimingMode m)
{
    Lock lk;
    if (!g_s.timingModeSet || g_s.timingMode != (int)m) Log("compositor: SetExplicitTimingMode(%d) (accepted; fakevr never touches the app's GPU queue)", (int)m);
    g_s.timingMode = (int)m; g_s.timingModeSet = true;
}

void BuildTables()
{
    if (g_tablesBuilt) return;
    std::memset(&g_sys, 0, sizeof(g_sys));
    std::memset(&g_comp, 0, sizeof(g_comp));
#define STUB_SYS(m) FAKEVR_STUB(g_sys, m);
#define STUB_COMP(m) FAKEVR_STUB(g_comp, m);
    SYS_MEMBERS(STUB_SYS)
    COMP_MEMBERS(STUB_COMP)
#undef STUB_SYS
#undef STUB_COMP
    g_sys.GetRecommendedRenderTargetSize = Sys_GetRecommendedRenderTargetSize;
    g_sys.GetProjectionMatrix = Sys_GetProjectionMatrix;
    g_sys.GetProjectionRaw = Sys_GetProjectionRaw;
    g_sys.ComputeDistortion = Sys_ComputeDistortion;
    g_sys.GetEyeToHeadTransform = Sys_GetEyeToHeadTransform;
    g_sys.GetTimeSinceLastVsync = Sys_GetTimeSinceLastVsync;
    g_sys.GetD3D9AdapterIndex = Sys_GetD3D9AdapterIndex;
    g_sys.GetDXGIOutputInfo = Sys_GetDXGIOutputInfo;
    g_sys.GetOutputDevice = Sys_GetOutputDevice;
    g_sys.IsDisplayOnDesktop = Sys_IsDisplayOnDesktop;
    g_sys.SetDisplayVisibility = Sys_SetDisplayVisibility;
    g_sys.GetDeviceToAbsoluteTrackingPose = Sys_GetDeviceToAbsoluteTrackingPose;
    g_sys.GetSeatedZeroPoseToStandingAbsoluteTrackingPose = Sys_GetSeatedZeroPose;
    g_sys.GetRawZeroPoseToStandingAbsoluteTrackingPose = Sys_GetRawZeroPose;
    g_sys.GetSortedTrackedDeviceIndicesOfClass = Sys_GetSortedTrackedDeviceIndicesOfClass;
    g_sys.GetTrackedDeviceActivityLevel = Sys_GetTrackedDeviceActivityLevel;
    g_sys.ApplyTransform = Sys_ApplyTransform;
    g_sys.GetTrackedDeviceIndexForControllerRole = Sys_GetTrackedDeviceIndexForControllerRole;
    g_sys.GetControllerRoleForTrackedDeviceIndex = Sys_GetControllerRoleForTrackedDeviceIndex;
    g_sys.GetTrackedDeviceClass = Sys_GetTrackedDeviceClass;
    g_sys.IsTrackedDeviceConnected = Sys_IsTrackedDeviceConnected;
    g_sys.GetBoolTrackedDeviceProperty = Sys_GetBoolProp;
    g_sys.GetFloatTrackedDeviceProperty = Sys_GetFloatProp;
    g_sys.GetInt32TrackedDeviceProperty = Sys_GetInt32Prop;
    g_sys.GetUint64TrackedDeviceProperty = Sys_GetUint64Prop;
    g_sys.GetMatrix34TrackedDeviceProperty = Sys_GetMatrix34Prop;
    g_sys.GetArrayTrackedDeviceProperty = Sys_GetArrayProp;
    g_sys.GetStringTrackedDeviceProperty = Sys_GetStringProp;
    g_sys.GetPropErrorNameFromEnum = Sys_GetPropErrorNameFromEnum;
    g_sys.PollNextEvent = Sys_PollNextEvent;
    g_sys.PollNextEventWithPose = Sys_PollNextEventWithPose;
    g_sys.PollNextEventWithPoseAndOverlays = Sys_PollNextEventWithPoseAndOverlays;
    g_sys.GetHiddenAreaMesh = Sys_GetHiddenAreaMesh;
    g_sys.GetControllerState = Sys_GetControllerState;
    g_sys.GetControllerStateWithPose = Sys_GetControllerStateWithPose;
    g_sys.TriggerHapticPulse = Sys_TriggerHapticPulse;
    g_sys.IsInputAvailable = Sys_IsInputAvailable;
    g_sys.IsSteamVRDrawingControllers = Sys_False;
    g_sys.ShouldApplicationPause = Sys_False;
    g_sys.ShouldApplicationReduceRenderingWork = Sys_False;
    g_sys.AcknowledgeQuit_Exiting = Sys_AcknowledgeQuit_Exiting;
    g_sys.GetAppContainerFilePaths = Sys_GetAppContainerFilePaths;
    g_sys.GetRuntimeVersion = Sys_GetRuntimeVersion;
    g_sys.SetSDKVersion = Sys_SetSDKVersion;

    g_comp.SetTrackingSpace = Comp_SetTrackingSpace;
    g_comp.GetTrackingSpace = Comp_GetTrackingSpace;
    g_comp.WaitGetPoses = Comp_WaitGetPoses;
    g_comp.GetLastPoses = Comp_GetLastPoses;
    g_comp.GetLastPoseForTrackedDeviceIndex = Comp_GetLastPoseForTrackedDeviceIndex;
    g_comp.Submit = Comp_Submit;
    g_comp.SubmitWithArrayIndex = Comp_SubmitWithArrayIndex;
    g_comp.ClearLastSubmittedFrame = Comp_Void;
    g_comp.PostPresentHandoff = Comp_Void;
    g_comp.GetFrameTiming = Comp_GetFrameTiming;
    g_comp.GetFrameTimings = Comp_GetFrameTimings;
    g_comp.GetFrameTimeRemaining = Comp_GetFrameTimeRemaining;
    g_comp.GetCumulativeStats = Comp_GetCumulativeStats;
    g_comp.IsFullscreen = Comp_True;
    g_comp.GetCurrentSceneFocusProcess = Comp_GetCurrentSceneFocusProcess;
    g_comp.GetLastFrameRenderer = Comp_GetLastFrameRenderer;
    g_comp.CanRenderScene = Comp_True;
    g_comp.IsMirrorWindowVisible = Comp_False;
    g_comp.ShouldAppRenderWithLowResources = Comp_False;
    g_comp.GetMirrorTextureD3D11 = Comp_GetMirrorTextureD3D11;
    g_comp.GetVulkanInstanceExtensionsRequired = Comp_GetVulkanInstanceExtensionsRequired;
    g_comp.GetVulkanDeviceExtensionsRequired = Comp_GetVulkanDeviceExtensionsRequired;
    g_comp.SubmitExplicitTimingData = Comp_SubmitExplicitTimingData;
    g_comp.SetExplicitTimingMode = Comp_SetExplicitTimingMode;
    g_comp.IsMotionSmoothingEnabled = Comp_False;
    g_comp.IsMotionSmoothingSupported = Comp_False;
    g_comp.IsCurrentSceneFocusAppLoading = Comp_False;

    // Belt and braces: every slot must be non-null.
    void* const* s = reinterpret_cast<void* const*>(&g_sys);
    for (size_t i = 0; i < sizeof(g_sys) / sizeof(void*); ++i) if (!s[i]) Log("BUG: IVRSystem slot %u is null", (unsigned)i);
    void* const* c = reinterpret_cast<void* const*>(&g_comp);
    for (size_t i = 0; i < sizeof(g_comp) / sizeof(void*); ++i) if (!c[i]) Log("BUG: IVRCompositor slot %u is null", (unsigned)i);
    g_tablesBuilt = true;
}

struct ErrName { int v; const char* n; };
const ErrName kInitErrors[] = {
#include "initerror_names.inc"
};

const char* InitErrSymbol(int e)
{
    for (const ErrName& x : kInitErrors) if (x.v == e) return x.n;
    return "VRInitError_Unknown";
}

void FullName(const char* ver, char* out, size_t n) { _snprintf(out, n, "FnTable:%s", ver); out[n - 1] = 0; }

intptr_t DoInit(EVRInitError* pe, EVRApplicationType type, const char* startupInfo)
{
    Lock lk;
    LoadConfig();
    BuildTables();
    const char* tn[] = {"Other", "Scene", "Overlay", "Background", "Utility", "VRMonitor", "SteamWatchdog", "Bootstrapper",
                        "WebHelper", "OpenXRInstance", "OpenXRScene", "OpenXROverlay", "Prism", "RoomView"};
    const char* tname = ((int)type >= 0 && (int)type < 14) ? tn[type] : "?";
    EVRInitError err = EVRInitError_VRInitError_None;
    if ((int)type < 0 || (int)type >= EVRApplicationType_VRApplication_Max) err = EVRInitError_VRInitError_Init_InvalidApplicationType;
    else if (g_cfg.fail_init) err = (EVRInitError)g_cfg.fail_init_error;
    if (err) {
        Log("VR_InitInternal(type=%d %s) -> FAIL %d %s%s", (int)type, tname, (int)err, InitErrSymbol(err), g_cfg.fail_init ? " (fail_init=1 in fakevr.ini)" : "");
        if (pe) *pe = err;
        return 0;
    }
    if (g_s.inited) {
        // Nested init (e.g. DXVK calls VR_InitInternal(Background) to query the compositor while the game already
        // runs a Scene session). Keep the running session and its counters; never downgrade Scene. Note: like
        // Valve's openvr_api.dll, fakevr does not reference-count: the next VR_ShutdownInternal ends the session.
        const char* had = ((int)g_s.type >= 0 && (int)g_s.type < 14) ? tn[g_s.type] : "?";
        if (type == EVRApplicationType_VRApplication_Scene) g_s.type = type;
        ++g_initToken;
        Log("WARNING: nested VR_InitInternal(type=%d %s) while a %s session is active -> OK, session kept (now %s), token %u. "
            "Any VR_ShutdownInternal ends the session for everyone (no refcount, same as Valve's openvr_api.dll).",
            (int)type, tname, had, ((int)g_s.type >= 0 && (int)g_s.type < 14) ? tn[g_s.type] : "?", g_initToken);
        if (pe) *pe = EVRInitError_VRInitError_None;
        return (intptr_t)g_initToken;
    }
    g_s = Session();
    g_s.inited = true;
    g_s.type = type;
    g_s.t0 = Qpc();
    g_s.period = (double)g_qpcFreq / g_cfg.refresh_hz;
    if (!g_timerPeriodSet) { timeBeginPeriod(1); g_timerPeriodSet = true; }
    if (!g_hiresTimer) {
        g_hiresTimer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /*CREATE_WAITABLE_TIMER_HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
        if (!g_hiresTimer) g_hiresTimer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }
    ++g_initToken;
    Log("VR_InitInternal(type=%d %s%s%s) -> OK, token %u, %s / %s, refresh %.3f Hz", (int)type, tname,
        startupInfo ? ", startup=" : "", startupInfo ? startupInfo : "", g_initToken, IVRSystem_Version, IVRCompositor_Version, g_cfg.refresh_hz);
    if (pe) *pe = EVRInitError_VRInitError_None;
    return (intptr_t)g_initToken;
}

void InitModulePath()
{
    char p[MAX_PATH] = "";
    if (g_module && GetModuleFileNameA(g_module, p, MAX_PATH)) {
        char* s = strrchr(p, '\\');
        if (!s) s = strrchr(p, '/');
        if (s) { *s = 0; _snprintf(g_dir, sizeof(g_dir), "%s", p); g_dir[MAX_PATH - 1] = 0; }
    }
}

}  // namespace

// ------------------------------------------------------------------------------------------------
// Exports (cdecl, undecorated via fakevr.def)
// ------------------------------------------------------------------------------------------------
extern "C" {

__declspec(dllexport) intptr_t VR_InitInternal2(EVRInitError* peError, EVRApplicationType eType, const char* pStartupInfo)
{
    return DoInit(peError, eType, pStartupInfo);
}

__declspec(dllexport) intptr_t VR_InitInternal(EVRInitError* peError, EVRApplicationType eType)
{
    return DoInit(peError, eType, nullptr);
}

__declspec(dllexport) void VR_ShutdownInternal()
{
    Lock lk;
    if (!g_s.inited) { Log("VR_ShutdownInternal (not initialised)"); return; }
    WriteSummary("VR_ShutdownInternal");
    g_s.inited = false;
    Log("VR_ShutdownInternal -> done");
}

__declspec(dllexport) bool VR_IsHmdPresent() { Lock lk; LoadConfig(); return !g_cfg.fail_init; }
__declspec(dllexport) bool VR_IsRuntimeInstalled() { return true; }

__declspec(dllexport) bool VR_GetRuntimePath(char* pchPathBuffer, uint32_t unBufferSize, uint32_t* punRequiredBufferSize)
{
    uint32_t need = (uint32_t)strlen(g_dir) + 1;
    if (punRequiredBufferSize) *punRequiredBufferSize = need;
    if (!pchPathBuffer || unBufferSize < need) return false;
    memcpy(pchPathBuffer, g_dir, need);
    return true;
}

__declspec(dllexport) intptr_t VR_GetGenericInterface(const char* pchInterfaceVersion, EVRInitError* peError)
{
    Lock lk;
    EnsureConfig();
    BuildTables();
    char sys[64], comp[64];
    FullName(IVRSystem_Version, sys, sizeof(sys));
    FullName(IVRCompositor_Version, comp, sizeof(comp));
    const char* name = pchInterfaceVersion ? pchInterfaceVersion : "(null)";
    void* r = nullptr;
    EVRInitError err = EVRInitError_VRInitError_None;
    if (!g_s.inited) err = EVRInitError_VRInitError_Init_NotInitialized;
    else if (pchInterfaceVersion && !strcmp(pchInterfaceVersion, sys)) r = &g_sys;
    else if (pchInterfaceVersion && !strcmp(pchInterfaceVersion, comp)) r = &g_comp;
    else err = EVRInitError_VRInitError_Init_InterfaceNotFound;
    Log("VR_GetGenericInterface(%s) -> %s%s", name, r ? "OK" : "nullptr, ", r ? "" : InitErrSymbol(err));
    if (peError) *peError = err;
    return (intptr_t)r;
}

__declspec(dllexport) bool VR_IsInterfaceVersionValid(const char* pchInterfaceVersion)
{
    if (!pchInterfaceVersion) return false;
    char sys[64], comp[64];
    FullName(IVRSystem_Version, sys, sizeof(sys));
    FullName(IVRCompositor_Version, comp, sizeof(comp));
    bool ok = !strcmp(pchInterfaceVersion, sys) || !strcmp(pchInterfaceVersion, comp);
    { Lock lk; Log("VR_IsInterfaceVersionValid(%s) -> %s", pchInterfaceVersion, ok ? "true" : "false (fakevr serves only the FnTable C API)"); }
    return ok;
}

__declspec(dllexport) uint32_t VR_GetInitToken() { return g_initToken; }

__declspec(dllexport) const char* VR_GetVRInitErrorAsSymbol(EVRInitError error) { return InitErrSymbol((int)error); }

__declspec(dllexport) const char* VR_GetVRInitErrorAsEnglishDescription(EVRInitError error)
{
    switch ((int)error) {
    case EVRInitError_VRInitError_None: return "No Error (fakevr)";
    case EVRInitError_VRInitError_Init_InterfaceNotFound: return "Interface not found (fakevr only provides FnTable:" "IVRSystem/IVRCompositor)";
    case EVRInitError_VRInitError_Init_HmdNotFound: return "Hmd Not Found (fakevr: fail_init=1 in fakevr.ini)";
    case EVRInitError_VRInitError_Init_NotInitialized: return "Not Initialized (fakevr: call VR_InitInternal first)";
    case EVRInitError_VRInitError_Init_InvalidApplicationType: return "Invalid application type (fakevr)";
    default: return InitErrSymbol((int)error);
    }
}

}  // extern "C"

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = h;
        DisableThreadLibraryCalls(h);
        InitializeCriticalSection(&g_lock);
        LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_qpcFreq = f.QuadPart ? f.QuadPart : 1;
        g_qpcStart = Qpc();
        InitModulePath();
    } else if (reason == DLL_PROCESS_DETACH) {
        g_exiting = reserved != nullptr;
        // reserved != NULL: the process is exiting; other threads are gone and may have died holding the
        // lock, so write the summary without taking it.
        if (g_s.inited) WriteSummary(reserved ? "process exit without VR_ShutdownInternal" : "DLL unloaded without VR_ShutdownInternal");
        if (g_timerPeriodSet) timeEndPeriod(1);
        if (g_hiresTimer) CloseHandle(g_hiresTimer);
        if (g_log != INVALID_HANDLE_VALUE) CloseHandle(g_log);
        if (!reserved) DeleteCriticalSection(&g_lock);
    }
    return TRUE;
}
