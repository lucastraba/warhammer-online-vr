// head_smoke.cpp - drives the mod's own OpenVRHead (src/openvr_head.h) against fakevr.
// Complements test_client.cpp (which re-implements the binding): this one uses WARVR's real code path.
// Exit code 0 only if every check passed. Build: tools/fakevr/build.sh.
#include "../../src/openvr_head.h"
#include <cstdarg>
#include <cstdio>

static int g_fail = 0;
static void Check(bool ok, const char* what) { printf("%s: %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++g_fail; }
static void LogFn(const char* fmt, ...) { va_list ap; va_start(ap, fmt); printf("  [head] "); vprintf(fmt, ap); va_end(ap); }

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    char exe[MAX_PATH]; GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char* s = strrchr(exe, '\\'); if (s) *s = 0;
    OpenVRHead head;
    Check(head.Init(exe, LogFn, EVRApplicationType_VRApplication_Other), "OpenVRHead::Init(Other)");
    OpenVRHead::Pose p{};
    Check(head.Poll(0.f, &p) && p.valid, "OpenVRHead::Poll -> valid pose");
    Check(head.Init(exe, LogFn, EVRApplicationType_VRApplication_Scene) && head.isScene(), "OpenVRHead::Init(Scene) (switch from Other)");
    uint32_t w = 0, h = 0; head.RecommendedSize(&w, &h);
    printf("  size %ux%u, display %.1f Hz, activity %d, adapter %d\n", w, h, head.DisplayFrequency(), head.HmdActivity(), head.AdapterIndex());
    Check(w > 0 && h > 0 && head.DisplayFrequency() > 0 && head.HmdActivity() == 1 && head.AdapterIndex() == 0, "RecommendedSize / DisplayFrequency / HmdActivity / AdapterIndex");
    for (int eye = 0; eye < 2; ++eye) {
        float l, r, t, b, e2h[3], rot[9];
        head.EyeParams(eye, &l, &r, &t, &b, e2h, rot);
        printf("  eye %d: l=%.3f r=%.3f t=%.3f b=%.3f eyeToHead=(%.4f %.4f %.4f)\n", eye, l, r, t, b, e2h[0], e2h[1], e2h[2]);
        Check(l < 0 && r > 0 && t < 0 && b > 0 && (eye ? e2h[0] > 0 : e2h[0] < 0), "EyeParams sane");
    }
    int okFrames = 0, nullErr = 0;
    for (int i = 0; i < 90; ++i) {
        if (head.WaitPoses(&p) == EVRCompositorError_VRCompositorError_None && p.valid) ++okFrames;
        // No D3D11 device here: the mod's Submit with a null texture must come back as InvalidTexture, not crash.
        if (head.Submit(0, nullptr) == EVRCompositorError_VRCompositorError_InvalidTexture) ++nullErr;
        head.PostPresentHandoff();
    }
    Check(okFrames == 90, "90 x WaitPoses -> None with a valid pose");
    Check(nullErr == 90, "90 x Submit(null) -> InvalidTexture");
    Compositor_CumulativeStats st;
    Check(head.Stats(&st) && st.m_nNumFramePresents >= 90 && st.m_nNumFramePresents == 90 + st.m_nNumReprojectedFrames && st.m_nNumDroppedFrames == 89,
          "Stats: presents == frames + reprojected; 89 completed frames without submits counted as dropped (the 90th is still open)");
    printf("  stats: presents=%u reprojected=%u dropped=%u submits=%u\n", st.m_nNumFramePresents, st.m_nNumReprojectedFrames, st.m_nNumDroppedFrames, st.m_nNumFrameSubmits);
    head.Shutdown();
    printf("RESULT: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
