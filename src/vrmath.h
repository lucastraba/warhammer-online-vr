// vrmath.h - pure math, no Windows/D3D/OpenVR dependencies (unit-testable natively).
//
// Conventions
//   OpenVR pose:  3x4, column-vector, right-handed, +Y up, -Z forward.  v_tracking = H * v_head.
//   D3D9 (WAR):   4x4, row-vector,    camera space is left-handed: +X right, +Y up, +Z forward.
//                 v_cam = v_world * View.   (WAR's own View has det = -1: its world is right-handed.)
//
// We never touch WAR's world convention. We rotate in *camera space*:
//        View' = View * Q4          (row-vector: applied after View, rotates about the eye)
// where Q is the head rotation relative to a yaw-only reference, converted RH->LH.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>

namespace vrm {

struct Mat3 { float m[3][3]; };

inline Mat3 identity() { Mat3 r{}; r.m[0][0] = r.m[1][1] = r.m[2][2] = 1.f; return r; }

inline Mat3 mul(const Mat3& a, const Mat3& b) {
    Mat3 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) r.m[i][j] += a.m[i][k] * b.m[k][j];
    return r;
}

inline Mat3 transpose(const Mat3& a) {
    Mat3 r{};
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) r.m[i][j] = a.m[j][i];
    return r;
}

// Column-vector rotation about +Y by theta (RH). Maps (0,0,-1) -> (-sin t, 0, -cos t).
inline Mat3 rotY(float t) {
    float c = std::cos(t), s = std::sin(t);
    Mat3 r = identity();
    r.m[0][0] = c;  r.m[0][2] = s;
    r.m[2][0] = -s; r.m[2][2] = c;
    return r;
}

// Upper-left 3x3 of an OpenVR HmdMatrix34_t (float[3][4]).
inline Mat3 fromOpenVR(const float h[3][4]) {
    Mat3 r{};
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) r.m[i][j] = h[i][j];
    return r;
}

// Heading of the head (rotation about +Y) in tracking space. Returns false when looking
// (almost) straight up/down and yaw is ill-defined.
inline bool headYaw(const Mat3& head, float* outYaw) {
    float fx = -head.m[0][2], fz = -head.m[2][2];          // forward = head * (0,0,-1)
    if (fx * fx + fz * fz < 1e-4f) return false;
    *outYaw = std::atan2(-fx, -fz);                         // inverse of rotY on (0,0,-1)
    return true;
}

// Head rotation relative to a yaw-only reference, expressed as a row-vector rotation in
// WAR's LH camera space (X right, Y up, Z forward).  Pitch/roll stay gravity-aligned.
inline Mat3 cameraDelta(const Mat3& head, float refYaw) {
    Mat3 rel = mul(transpose(rotY(refYaw)), head);          // head relative to reference frame
    const float s[3] = { 1.f, 1.f, -1.f };                  // RH(-Z fwd) -> LH(+Z fwd)
    Mat3 q{};
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) q.m[i][j] = s[i] * s[j] * rel.m[i][j];
    return q;                                               // used directly as row-vector matrix
}

// out = view * Q4  (Q4 = Q in the upper-left 3x3, identity elsewhere). out may not alias view.
inline void applyToView(const float view[4][4], const Mat3& q, float out[4][4]) {
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 3; ++c) {
            out[r][c] = view[r][0] * q.m[0][c] + view[r][1] * q.m[1][c] + view[r][2] * q.m[2][c];
        }
        out[r][3] = view[r][3];
    }
}

// Determinant of the upper-left 3x3 of a D3D matrix. WAR's normal world views are -1; a mirrored
// (water reflection) view flips the sign.
inline float det3(const float v[4][4]) {
    return v[0][0]*(v[1][1]*v[2][2]-v[1][2]*v[2][1]) - v[0][1]*(v[1][0]*v[2][2]-v[1][2]*v[2][0]) + v[0][2]*(v[1][0]*v[2][1]-v[1][1]*v[2][0]);
}

// D3D row-vector, left-handed perspective (D3DXMatrixPerspectiveFovLH style):
//   _34 == 1, _44 == 0.   Orthographic has _34 == 0, _44 == 1.
inline bool isPerspective(const float p[4][4]) {
    return std::fabs(p[2][3] - 1.f) < 1e-3f && std::fabs(p[3][3]) < 1e-3f;
}

// Decode near/far/vertical FOV/aspect for logging.
inline bool decodePerspective(const float p[4][4], float* zn, float* zf, float* fovyDeg, float* aspect) {
    if (!isPerspective(p) || p[2][2] == 0.f || p[1][1] == 0.f || p[0][0] == 0.f) return false;
    *zn = -p[3][2] / p[2][2];
    *zf = (p[2][2] != 1.f) ? (*zn) * p[2][2] / (p[2][2] - 1.f) : 0.f;
    *fovyDeg = 2.f * std::atan(1.f / p[1][1]) * 57.29578f;
    *aspect = p[1][1] / p[0][0];
    return true;
}


// ---- combined View*Projection in vertex-shader constants -------------------------------------
// out = a * b  (row-vector convention, 4x4). out may not alias a or b.
inline void mul4(const float a[4][4], const float b[4][4], float out[4][4]) {
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            out[i][j] = a[i][0]*b[0][j] + a[i][1]*b[1][j] + a[i][2]*b[2][j] + a[i][3]*b[3][j];
}

inline bool closeEnough(float a, float b) {
    float m = std::fabs(a) > std::fabs(b) ? std::fabs(a) : std::fabs(b);
    return std::fabs(a - b) <= 1e-3f + 2e-3f * m;
}

// 4 consecutive float4 registers (16 floats) versus View*Projection.
// Returns 1 if the registers hold transpose(VP) (register r = column r of VP, i.e. dp4 layout),
//         2 if they hold VP as-is (register r = row r), 0 if neither.
inline int detectVPLayout(const float vp[4][4], const float* regs) {
    bool t = true, n = true;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            if (!closeEnough(regs[r*4 + c], vp[c][r])) t = false;
            if (!closeEnough(regs[r*4 + c], vp[r][c])) n = false;
        }
    return t ? 1 : (n ? 2 : 0);
}

inline void writeVPRegs(const float vp[4][4], int layout, float* regs) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) regs[r*4 + c] = (layout == 1) ? vp[c][r] : vp[r][c];
}


// ---- steering at the engine's SetCamera ----------------------------------------------------------
// WAR builds its view matrix from basis vectors with  view[i][0]=right[i], view[i][1]=up[i],
// view[i][2]=forward[i]  and  view[3][c] = -dot(eye, column c)   (disassembled from RenderContext::SetCamera).
// The equivalent of  View' = View * Q  is therefore a Q-weighted mix of the INPUT vectors.
struct V3 { float x, y, z; };
inline void rotateBasis(const Mat3& q, const V3& right, const V3& up, const V3& fwd, V3* r2, V3* u2, V3* f2) {
    *r2 = { q.m[0][0]*right.x + q.m[1][0]*up.x + q.m[2][0]*fwd.x,
            q.m[0][0]*right.y + q.m[1][0]*up.y + q.m[2][0]*fwd.y,
            q.m[0][0]*right.z + q.m[1][0]*up.z + q.m[2][0]*fwd.z };
    *u2 = { q.m[0][1]*right.x + q.m[1][1]*up.x + q.m[2][1]*fwd.x,
            q.m[0][1]*right.y + q.m[1][1]*up.y + q.m[2][1]*fwd.y,
            q.m[0][1]*right.z + q.m[1][1]*up.z + q.m[2][1]*fwd.z };
    *f2 = { q.m[0][2]*right.x + q.m[1][2]*up.x + q.m[2][2]*fwd.x,
            q.m[0][2]*right.y + q.m[1][2]*up.y + q.m[2][2]*fwd.y,
            q.m[0][2]*right.z + q.m[1][2]*up.z + q.m[2][2]*fwd.z };
}
// What the engine does with those vectors (for tests).
inline void engineBuildView(const V3& pos, const V3& right, const V3& up, const V3& fwd, float v[4][4]) {
    v[0][0]=right.x; v[0][1]=up.x; v[0][2]=fwd.x; v[0][3]=0;
    v[1][0]=right.y; v[1][1]=up.y; v[1][2]=fwd.y; v[1][3]=0;
    v[2][0]=right.z; v[2][1]=up.z; v[2][2]=fwd.z; v[2][3]=0;
    v[3][0]=-(pos.x*right.x+pos.y*right.y+pos.z*right.z);
    v[3][1]=-(pos.x*up.x+pos.y*up.y+pos.z*up.z);
    v[3][2]=-(pos.x*fwd.x+pos.y*fwd.y+pos.z*fwd.z);
    v[3][3]=1;
}


// Recently produced camera bases. WAR reads the stored camera back (GetCamera) and feeds it to SetCamera
// again for derived passes (sky etc.). Those inputs equal one of OUR outputs and must pass through
// unchanged, otherwise the rotation compounds once per nested pass.
struct OutputRing {
    struct K { V3 r, u, f; };
    K k[8]; int n = 0, next = 0;
    static bool same(const V3& a, const V3& b) {
        return std::fabs(a.x-b.x) < 1e-6f && std::fabs(a.y-b.y) < 1e-6f && std::fabs(a.z-b.z) < 1e-6f;
    }
    bool contains(const V3& r, const V3& u, const V3& f) const {
        for (int i = 0; i < n; ++i) if (same(k[i].r, r) && same(k[i].u, u) && same(k[i].f, f)) return true;
        return false;
    }
    void add(const V3& r, const V3& u, const V3& f) {
        k[next] = { r, u, f }; next = (next + 1) % 8; if (n < 8) ++n;
    }
};


// ---- positional tracking and per-eye setup ---------------------------------------------------------
// Head translation since recentering, expressed in the recentered camera frame (x right, y up, z forward,
// left-handed like WAR's camera space), in metres. OpenVR tracking space is right-handed with +Y up and
// -Z forward; `refYaw` is the heading captured at recentering (see headYaw), refPos the position then.
inline V3 headDeltaCamera(float refYaw, const float refPos[3], const float headPos[3]) {
    const float d[3] = { headPos[0]-refPos[0], headPos[1]-refPos[1], headPos[2]-refPos[2] };
    const Mat3 rt = transpose(rotY(refYaw));                 // tracking space -> recentered frame
    float rel[3];
    for (int i = 0; i < 3; ++i) rel[i] = rt.m[i][0]*d[0] + rt.m[i][1]*d[1] + rt.m[i][2]*d[2];
    return { rel[0], rel[1], -rel[2] };                      // RH(-Z forward) -> LH(+Z forward)
}

// World-space offset = offset.x*right + offset.y*up + offset.z*fwd, scaled (metres -> WAR world units).
inline V3 worldOffset(const V3& offsetCam, const V3& right, const V3& up, const V3& fwd, float unitsPerMetre) {
    return { (offsetCam.x*right.x + offsetCam.y*up.x + offsetCam.z*fwd.x) * unitsPerMetre,
             (offsetCam.x*right.y + offsetCam.y*up.y + offsetCam.z*fwd.y) * unitsPerMetre,
             (offsetCam.x*right.z + offsetCam.y*up.z + offsetCam.z*fwd.z) * unitsPerMetre };
}

// OpenVR eye-to-head translation (head space: +x right, +y up, -z forward) -> camera-space (LH) metres.
inline V3 eyeOffsetCamera(const float eyeToHead[3]) { return { eyeToHead[0], eyeToHead[1], -eyeToHead[2] }; }

// OpenVR GetProjectionRaw returns tangents with left < 0 < right, and -- documented by Valve and easy to get wrong --
// "top" and "bottom" are BACKWARDS: pfBottom is the (positive) tangent from the view axis to the TOP (+Y) edge of the
// display, and pfTop is the (negative) tangent to the BOTTOM (-Y) edge. (Valve driver docs: "Left = -2.747,
// Right = 1.732, Top = -1.42, Bottom = 1.42".) WAR's frustum wants y0 = upper edge (positive), y1 = lower edge
// (negative), so  y0 = pfBottom  and  y1 = pfTop  -- no negation. (Round 6f got this backwards: the vertical field of
// view was mirrored, i.e. wrong by up to ~0.37 tangent at the edges, the "wrong lens" distortion.)
// Ordering is enforced so an unconventional driver cannot invert the image.
inline void openvrRawToFrustum(float left, float right, float top, float bottom, float out[4]) {
    out[0] = left; out[1] = right;
    out[2] = bottom > top ? bottom : top;     // y0: upper edge (the positive value)
    out[3] = bottom > top ? top : bottom;     // y1: lower edge (the negative value)
}


// ---- simultaneous stereo (both eyes in one frame) ---------------------------------------------------
// WAR's perspective projection from a Gamebryo-style frustum {x0=left, x1=right, y0=top, y1=bottom} at unit
// distance, exactly as RenderContext::SetCamera builds it (disassembled from the supplied WAR.exe):
//   m00 = 2/(x1-x0)   m20 = -(x0+x1)/(x1-x0)   m11 = 2/(y0-y1)   m21 = -(y0+y1)/(y0-y1)
//   m22 = zf/(zf-zn)  m23 = 1                  m32 = -zn*zf/(zf-zn)   (row-vector, left-handed)
inline void perspFromFrustum(float x0, float x1, float y0, float y1, float zn, float zf, float out[4][4]) {
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) out[i][j] = 0.f;
    out[0][0] = 2.f / (x1 - x0);       out[2][0] = -(x0 + x1) / (x1 - x0);
    out[1][1] = 2.f / (y0 - y1);       out[2][1] = -(y0 + y1) / (y0 - y1);
    out[2][2] = zf / (zf - zn);        out[2][3] = 1.f;
    out[3][2] = -zn * zf / (zf - zn);
}

// View matrix of a camera that has moved by (dx,dy,dz) in ITS OWN axes (row-vector): V' = V * shift.
inline void cameraShiftMatrix(float dx, float dy, float dz, float out[4][4]) {
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) out[i][j] = (i == j) ? 1.f : 0.f;
    out[3][0] = -dx; out[3][1] = -dy; out[3][2] = -dz;
}

// Where to draw the 2D HUD inside one eye's region so it floats `distance` metres ahead of the head (head-locked),
// `widthM` metres wide, with the UI's own aspect ratio, `vertM` metres above the centre line.
//   frustum = this eye's {x0,x1,y0,y1} tangents, eyeX/eyeY = eye offset from head centre (metres, camera space),
//   region  = pixel size of this eye's image. Result is shrunk (never moved off-centre) to stay inside the region.
struct PixelRect { int x, y, w, h; };
inline PixelRect hudRectPixels(const float frustum[4], float eyeX, float eyeY, float distance, float widthM,
                               float uiAspect, float vertM, int regionW, int regionH) {
    const float hw = (widthM * 0.5f) / distance;                 // half-size in tangent space
    const float hh = (widthM / uiAspect * 0.5f) / distance;
    const float cx = (0.f - eyeX) / distance;                    // direction to the HUD centre seen from this eye
    const float cy = (vertM - eyeY) / distance;
    float left = cx - hw, right = cx + hw, top = cy + hh, bottom = cy - hh;
    const float fw = frustum[1] - frustum[0], fh = frustum[2] - frustum[3];
    float px0 = (left  - frustum[0]) / fw * regionW, px1 = (right  - frustum[0]) / fw * regionW;
    float py0 = (frustum[2] - top)   / fh * regionH, py1 = (frustum[2] - bottom) / fh * regionH;
    // shrink about the rectangle centre until it fits entirely inside the region
    float cxp = 0.5f * (px0 + px1), cyp = 0.5f * (py0 + py1), hwp = 0.5f * (px1 - px0), hhp = 0.5f * (py1 - py0);
    float k = 1.f;
    if (cxp - hwp < 0.f)        k = std::fmin(k, cxp / hwp);
    if (cxp + hwp > regionW)    k = std::fmin(k, (regionW - cxp) / hwp);
    if (cyp - hhp < 0.f)        k = std::fmin(k, cyp / hhp);
    if (cyp + hhp > regionH)    k = std::fmin(k, (regionH - cyp) / hhp);
    if (k < 0.05f) k = 0.05f;
    hwp *= k; hhp *= k;
    PixelRect r;
    r.x = (int)std::lround(cxp - hwp); r.y = (int)std::lround(cyp - hhp);
    r.w = (int)std::lround(2.f * hwp); r.h = (int)std::lround(2.f * hhp);
    if (r.w < 2) r.w = 2;
    if (r.h < 2) r.h = 2;
    return r;
}


// Find transpose(VP) (4 consecutive float4 registers) anywhere in a shadow register file.
inline bool findVPInRegs(const float (*regs)[4], int nRegs, const float vp[4][4], int* offset) {
    for (int o = 0; o + 4 <= nRegs; ++o) {
        if (!closeEnough(regs[o][0], vp[0][0])) continue;
        bool ok = true;
        for (int r = 0; r < 4 && ok; ++r)
            for (int c = 0; c < 4; ++c)
                if (!closeEnough(regs[o + r][c], vp[c][r])) { ok = false; break; }
        if (ok) { *offset = o; return true; }
    }
    return false;
}

// Right-eye view and projection from the game's left-eye ones.
//   - projection: the engine's own matrix with only its x/y terms (m00 m20 m11 m21) replaced by the right eye's,
//     so near/far terms stay bit-identical to the engine's;
//   - view: shifted along the camera's own axes by (rightEyeOffset - leftEyeOffset) * unitsPerMetre, except for
//     camera-relative passes (translation row == 0, e.g. the sky) which only change projection.
// Returns false (and leaves outputs untouched) if the left-eye frustum does not reproduce the engine's projection.
inline bool rightEyeMatrices(const float viewL[4][4], const float projL[4][4], const float frL[4], const float frR[4],
                             const V3& offL, const V3& offR, float unitsPerMetre,
                             float viewR[4][4], float projR[4][4], float* worstRel) {
    float PL[4][4], PR[4][4];
    perspFromFrustum(frL[0], frL[1], frL[2], frL[3], 10.f, 1000.f, PL);
    perspFromFrustum(frR[0], frR[1], frR[2], frR[3], 10.f, 1000.f, PR);
    const int xy[4][2] = { {0,0}, {2,0}, {1,1}, {2,1} };
    float worst = 0.f;
    for (auto& e : xy) {
        float d = std::fabs(PL[e[0]][e[1]] - projL[e[0]][e[1]]);
        float m = std::fmax(1.f, std::fabs(projL[e[0]][e[1]]));
        worst = std::fmax(worst, d / m);
    }
    if (worstRel) *worstRel = worst;
    if (worst > 2e-3f) return false;
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) projR[i][j] = projL[i][j];
    for (auto& e : xy) projR[e[0]][e[1]] = PR[e[0]][e[1]];
    const float t = std::fabs(viewL[3][0]) + std::fabs(viewL[3][1]) + std::fabs(viewL[3][2]);
    if (t < 1e-3f) { for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) viewR[i][j] = viewL[i][j]; }
    else {
        float sh[4][4];
        cameraShiftMatrix((offR.x - offL.x) * unitsPerMetre, (offR.y - offL.y) * unitsPerMetre, (offR.z - offL.z) * unitsPerMetre, sh);
        mul4(viewL, sh, viewR);
    }
    return true;
}


// ---- everything else in the shader constants that depends on the camera (phase 7d) --------------------------------
// Phase 6b swapped only the plain View*Projection block for the right eye. Shaders also carry other camera-derived
// constants: a second copy of VP, the view or projection alone, VP combined with a clip-space scale/bias (projective
// texture / reflection / refraction lookup matrices) and the camera position. All of them are scanned for in the shadow
// register file and replaced by their right-eye equivalents for the right-eye draw.
struct EyeMats { float V[4][4], P[4][4], VP[4][4]; float cam[3]; };

// World position of the camera from a (rigid) D3D row-vector view matrix:  [C 1] * V = [0 0 0 1]  =>  C_i = -sum_j t_j * V[i][j]
inline void camPosFromView(const float v[4][4], float out[3]) {
    for (int i = 0; i < 3; ++i) out[i] = -(v[3][0]*v[i][0] + v[3][1]*v[i][1] + v[3][2]*v[i][2]);
}

inline void makeEyeMats(const float v[4][4], const float p[4][4], EyeMats* e) {
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) { e->V[i][j] = v[i][j]; e->P[i][j] = p[i][j]; }
    mul4(v, p, e->VP);
    camPosFromView(v, e->cam);
}

enum PatchKind { PK_VP = 0, PK_VIEW = 1, PK_PROJ = 2, PK_VPBIAS = 3, PK_CAMPOS = 4 };
inline const char* patchKindName(int k) {
    static const char* n[] = { "VP", "VIEW", "PROJ", "VP*bias", "CAMPOS" };
    return (k >= 0 && k < 5) ? n[k] : "?";
}
struct RegPatch { int reg; int count; int kind; bool transposed; float v[16]; };    // count float4 registers starting at reg

// Register image of a 4x4 matrix: transposed (register r = column r, the dp4 layout WAR uses) or straight (register r = row r).
inline void matToRegs(const float m[4][4], bool transposed, float out[4][4]) {
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) out[r][c] = transposed ? m[c][r] : m[r][c];
}

inline bool regsMatch(const float (*regs)[4], int at, const float want[4][4], int count) {
    for (int r = 0; r < count; ++r)
        for (int c = 0; c < 4; ++c)
            if (!closeEnough(regs[at + r][c], want[r][c])) return false;
    return true;
}

// Is regs[o..o+3] (transposed layout) a VP matrix combined with a clip-space scale/bias?
//   register r = a_r * VPT[r] + d_r * VPT[3]   (r = 0..2),  register 3 = VPT[3]        (VPT[r] = column r of VP)
// Solves a_r, d_r by least squares in double precision and checks the residual.
inline bool matchVPBias(const float (*regs)[4], int o, const float vpT[4][4], double a[3], double d[3]) {
    for (int c = 0; c < 4; ++c) if (!closeEnough(regs[o + 3][c], vpT[3][c])) return false;
    for (int r = 0; r < 3; ++r) {
        // The 4th component of a VP column is dominated by the huge translation (1e4..1e5), so it carries float noise
        // far above the rotation terms. Solve a, d from the three accurate components, then check the 4th loosely.
        double u[4], w[4], t[4];
        for (int c = 0; c < 4; ++c) { u[c] = vpT[r][c]; w[c] = vpT[3][c]; t[c] = regs[o + r][c]; }
        double uu = 0, uw = 0, ww = 0, ut = 0, wt = 0;
        for (int c = 0; c < 3; ++c) { uu += u[c]*u[c]; uw += u[c]*w[c]; ww += w[c]*w[c]; ut += u[c]*t[c]; wt += w[c]*t[c]; }
        const double det = uu * ww - uw * uw;
        if (std::fabs(det) < 1e-4 * uu * ww) {                           // columns (nearly) parallel: the depth column is almost the w column.
            bool same = true;                                            // Ambiguous, so accept only "unchanged" (a = 1, d = 0).
            for (int c = 0; c < 4; ++c) if (!closeEnough((float)t[c], (float)u[c])) same = false;
            if (!same) return false;
            a[r] = 1.0; d[r] = 0.0;
            continue;
        }
        a[r] = (ut * ww - wt * uw) / det;
        d[r] = (uu * wt - uw * ut) / det;
        double res = 0, tn = 0;
        for (int c = 0; c < 3; ++c) { const double e = t[c] - a[r] * u[c] - d[r] * w[c]; res += e * e; tn += t[c] * t[c]; }
        if (std::sqrt(res) > 2e-3 * std::sqrt(tn) + 2e-3) return false;
        const double e3 = t[3] - a[r] * u[3] - d[r] * w[3];
        if (std::fabs(e3) > 2e-4 * (std::fabs(t[3]) + std::fabs(a[r] * u[3]) + std::fabs(d[r] * w[3])) + 0.05) return false;
    }
    return true;
}

// Scan `regs` (n float4 registers) for left-eye camera constants and produce the right-eye replacements.
// Never touches a register twice. Returns the number of patches written (<= maxOut).
inline int buildEyePatches(const float (*regs)[4], int n, const EyeMats& L, const EyeMats& R, RegPatch* out, int maxOut) {
    int np = 0;
    bool claimed[512] = {};
    if (n > 512) n = 512;
    auto free_ = [&](int o, int cnt) { for (int i = 0; i < cnt; ++i) if (claimed[o + i]) return false; return true; };
    auto claim = [&](int o, int cnt) { for (int i = 0; i < cnt; ++i) claimed[o + i] = true; };
    auto nontrivial = [](const float w[4][4], int cnt) { float s = 0; for (int r = 0; r < cnt; ++r) for (int c = 0; c < 4; ++c) s += std::fabs(w[r][c]); return s > 0.05f; };

    struct Pat { const float (*ml)[4]; const float (*mr)[4]; int count; int kind; bool needsTranslation; };
    const Pat pats[] = {
        { L.VP, R.VP, 4, PK_VP,   false },
        { L.P,  R.P,  4, PK_PROJ, false },
        { L.V,  R.V,  3, PK_VIEW, true  },      // rotation rows + translation (the 4th register is always 0,0,0,1)
    };
    for (const Pat& p : pats) {
        for (int tr = 1; tr >= 0; --tr) {
            float wl[4][4], wr[4][4];
            matToRegs(p.ml, tr != 0, wl); matToRegs(p.mr, tr != 0, wr);
            if (p.kind == PK_VIEW && tr == 0) { /* straight 4x4 view: all four registers */ }
            const int cnt = (p.kind == PK_VIEW && tr == 0) ? 4 : p.count;
            if (!nontrivial(wl, cnt)) continue;
            if (p.needsTranslation) {                                      // a view matrix has a real translation; a bare rotation is too easy to hit by accident
                float t = 0; for (int i = 0; i < 3; ++i) t += std::fabs(p.ml[3][i]);
                if (t < 1.f) continue;
            }
            for (int o = 0; o + cnt <= n; ++o) {
                if (!free_(o, cnt) || !regsMatch(regs, o, wl, cnt)) continue;
                if (np >= maxOut) return np;
                RegPatch& rp = out[np++];
                rp.reg = o; rp.count = cnt; rp.kind = p.kind; rp.transposed = tr != 0;
                for (int r = 0; r < cnt; ++r) for (int c = 0; c < 4; ++c) rp.v[r*4 + c] = wr[r][c];
                claim(o, cnt);
            }
        }
    }
    // VP combined with a clip-space scale/bias (transposed layout)
    {
        float vpTL[4][4], vpTR[4][4];
        matToRegs(L.VP, true, vpTL); matToRegs(R.VP, true, vpTR);
        for (int o = 0; o + 4 <= n; ++o) {
            if (!free_(o, 4)) continue;
            double a[3], d[3];
            if (!matchVPBias(regs, o, vpTL, a, d)) continue;
            if (std::fabs(a[0]) < 0.02 || std::fabs(a[1]) < 0.02) continue;                    // x/y must be scaled by something real
            const bool isPlainVP = std::fabs(a[0]-1) < 1e-3 && std::fabs(a[1]-1) < 1e-3 && std::fabs(a[2]-1) < 1e-3 &&
                                   std::fabs(d[0]) < 1e-3 && std::fabs(d[1]) < 1e-3 && std::fabs(d[2]) < 1e-3;
            if (isPlainVP) continue;                                                           // that is VP itself (handled above)
            if (np >= maxOut) return np;
            RegPatch& rp = out[np++];
            rp.reg = o; rp.count = 4; rp.kind = PK_VPBIAS; rp.transposed = true;
            for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) rp.v[r*4 + c] = (float)(a[r] * vpTR[r][c] + d[r] * vpTR[3][c]);
            for (int c = 0; c < 4; ++c) rp.v[12 + c] = vpTR[3][c];
            claim(o, 4);
        }
    }
    // camera position (x y z in the first three components; w is kept)
    for (int o = 0; o < n; ++o) {
        if (claimed[o]) continue;
        if (std::fabs(regs[o][0] - L.cam[0]) > 0.02f || std::fabs(regs[o][1] - L.cam[1]) > 0.02f || std::fabs(regs[o][2] - L.cam[2]) > 0.02f) continue;
        if (std::fabs(L.cam[0]) + std::fabs(L.cam[1]) + std::fabs(L.cam[2]) < 1.f) continue;
        if (std::fabs(L.cam[0] - R.cam[0]) + std::fabs(L.cam[1] - R.cam[1]) + std::fabs(L.cam[2] - R.cam[2]) < 1e-4f) continue;     // nothing to change
        if (np >= maxOut) return np;
        RegPatch& rp = out[np++];
        rp.reg = o; rp.count = 1; rp.kind = PK_CAMPOS; rp.transposed = false;
        rp.v[0] = R.cam[0]; rp.v[1] = R.cam[1]; rp.v[2] = R.cam[2]; rp.v[3] = regs[o][3];
        claimed[o] = true;
    }
    return np;
}

// ---- fast, incremental scanning (phase 7i) ------------------------------------------------------------------------
// buildEyePatches() above looks at all 256 registers for every duplicated draw: about 9 us per call, twice per draw (vertex and
// pixel constants), ~1300 draws a frame = 20+ ms of CPU. That is what held VR below 90 fps while the GPU sat half idle.
// RegScan remembers, per register, which of the things we look for it could possibly be the start of (a cheap test on its first
// float). The game rewrites only a few registers between draws, so only that window is re-examined (touch() records it), and the
// full match is run on the handful of candidates. The result is identical to buildEyePatches (see the equivalence test).
struct ScanKeys { float vp00, p00, v00, vp03, camx; };      // derived from the LEFT eye only; what we look for
inline void makeScanKeys(const EyeMats& L, ScanKeys* k) { k->vp00 = L.VP[0][0]; k->p00 = L.P[0][0]; k->v00 = L.V[0][0]; k->vp03 = L.VP[0][3]; k->camx = L.cam[0]; }

enum { CB_VP = 1, CB_P = 2, CB_V = 4, CB_BIAS = 8, CB_CAM = 16 };
struct RegScan {
    uint8_t cand[512];
    int n = 0, lo = 0, hi = 0;
    unsigned ver = 0;
    int count[5] = {};
    int lastLo = 0, lastHi = 0; bool lastAll = false;       // the window re-classified by the latest refresh (for MatchCache)
    void init(int nRegs) { n = nRegs > 512 ? 512 : nRegs; for (int i = 0; i < 512; ++i) cand[i] = 0; lo = 0; hi = n; ver = 0; for (int i = 0; i < 5; ++i) count[i] = 0; }
    void touch(int start, int cnt) {                         // the game wrote registers [start, start+cnt)
        int a = start - 3; if (a < 0) a = 0;                 // (a 4-register candidate starting up to 3 before can include them)
        int b = start + cnt; if (b > n) b = n;
        if (lo >= hi) { lo = a; hi = b; } else { if (a < lo) lo = a; if (b > hi) hi = b; }
    }
    static uint8_t classify(const float (*regs)[4], int o, int n, const ScanKeys& k) {
        uint8_t b = 0; const float x = regs[o][0];
        if (closeEnough(x, k.vp00)) b |= CB_VP;
        if (closeEnough(x, k.p00)) b |= CB_P;
        if (closeEnough(x, k.v00)) b |= CB_V;
        if (o + 3 < n && closeEnough(regs[o + 3][0], k.vp03)) b |= CB_BIAS;
        if (std::fabs(x - k.camx) <= 0.02f) b |= CB_CAM;
        return b;
    }
    void refresh(const float (*regs)[4], const ScanKeys& k, unsigned keyVer) {
        lastAll = false;
        if (ver != keyVer) { ver = keyVer; lo = 0; hi = n; lastAll = true; }
        lastLo = lo; lastHi = hi;
        for (int o = lo; o < hi; ++o) {
            const uint8_t nb = classify(regs, o, n, k), ob = cand[o];
            if (nb != ob) {
                for (int b = 0; b < 5; ++b) { if ((ob >> b) & 1) --count[b]; if ((nb >> b) & 1) ++count[b]; }
                cand[o] = nb;
            }
        }
        lo = hi = 0;
    }
};

// Same algorithm and output as buildEyePatches, restricted to the candidates of an up-to-date RegScan (call scan.refresh first).
inline int buildEyePatchesScan(const float (*regs)[4], int n, const EyeMats& L, const EyeMats& R, const RegScan& scan, RegPatch* out, int maxOut) {
    int np = 0;
    bool claimed[512] = {};
    if (n > 512) n = 512;
    auto free_ = [&](int o, int cnt) { for (int i = 0; i < cnt; ++i) if (claimed[o + i]) return false; return true; };
    auto claim = [&](int o, int cnt) { for (int i = 0; i < cnt; ++i) claimed[o + i] = true; };
    auto nontrivial = [](const float w[4][4], int cnt) { float s = 0; for (int r = 0; r < cnt; ++r) for (int c = 0; c < 4; ++c) s += std::fabs(w[r][c]); return s > 0.05f; };

    struct Pat { const float (*ml)[4]; const float (*mr)[4]; int count; int kind; bool needsTranslation; uint8_t bit; int bitIdx; };
    const Pat pats[] = {
        { L.VP, R.VP, 4, PK_VP,   false, CB_VP, 0 },
        { L.P,  R.P,  4, PK_PROJ, false, CB_P,  1 },
        { L.V,  R.V,  3, PK_VIEW, true,  CB_V,  2 },
    };
    for (const Pat& p : pats) {
        if (scan.count[p.bitIdx] == 0) continue;
        for (int tr = 1; tr >= 0; --tr) {
            float wl[4][4], wr[4][4];
            matToRegs(p.ml, tr != 0, wl); matToRegs(p.mr, tr != 0, wr);
            const int cnt = (p.kind == PK_VIEW && tr == 0) ? 4 : p.count;
            if (!nontrivial(wl, cnt)) continue;
            if (p.needsTranslation) {
                float t = 0; for (int i = 0; i < 3; ++i) t += std::fabs(p.ml[3][i]);
                if (t < 1.f) continue;
            }
            for (int o = 0; o + cnt <= n; ++o) {
                if (!(scan.cand[o] & p.bit)) continue;
                if (!free_(o, cnt) || !regsMatch(regs, o, wl, cnt)) continue;
                if (np >= maxOut) return np;
                RegPatch& rp = out[np++];
                rp.reg = o; rp.count = cnt; rp.kind = p.kind; rp.transposed = tr != 0;
                for (int r = 0; r < cnt; ++r) for (int c = 0; c < 4; ++c) rp.v[r*4 + c] = wr[r][c];
                claim(o, cnt);
            }
        }
    }
    if (scan.count[3] > 0) {
        float vpTL[4][4], vpTR[4][4];
        matToRegs(L.VP, true, vpTL); matToRegs(R.VP, true, vpTR);
        for (int o = 0; o + 4 <= n; ++o) {
            if (!(scan.cand[o] & CB_BIAS) || !free_(o, 4)) continue;
            double a[3], d[3];
            if (!matchVPBias(regs, o, vpTL, a, d)) continue;
            if (std::fabs(a[0]) < 0.02 || std::fabs(a[1]) < 0.02) continue;
            const bool isPlainVP = std::fabs(a[0]-1) < 1e-3 && std::fabs(a[1]-1) < 1e-3 && std::fabs(a[2]-1) < 1e-3 &&
                                   std::fabs(d[0]) < 1e-3 && std::fabs(d[1]) < 1e-3 && std::fabs(d[2]) < 1e-3;
            if (isPlainVP) continue;
            if (np >= maxOut) return np;
            RegPatch& rp = out[np++];
            rp.reg = o; rp.count = 4; rp.kind = PK_VPBIAS; rp.transposed = true;
            for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) rp.v[r*4 + c] = (float)(a[r] * vpTR[r][c] + d[r] * vpTR[3][c]);
            for (int c = 0; c < 4; ++c) rp.v[12 + c] = vpTR[3][c];
            claim(o, 4);
        }
    }
    if (scan.count[4] > 0 && std::fabs(L.cam[0]) + std::fabs(L.cam[1]) + std::fabs(L.cam[2]) >= 1.f &&
        std::fabs(L.cam[0] - R.cam[0]) + std::fabs(L.cam[1] - R.cam[1]) + std::fabs(L.cam[2] - R.cam[2]) >= 1e-4f) {
        for (int o = 0; o < n; ++o) {
            if (claimed[o] || !(scan.cand[o] & CB_CAM)) continue;
            if (std::fabs(regs[o][0] - L.cam[0]) > 0.02f || std::fabs(regs[o][1] - L.cam[1]) > 0.02f || std::fabs(regs[o][2] - L.cam[2]) > 0.02f) continue;
            if (np >= maxOut) return np;
            RegPatch& rp = out[np++];
            rp.reg = o; rp.count = 1; rp.kind = PK_CAMPOS; rp.transposed = false;
            rp.v[0] = R.cam[0]; rp.v[1] = R.cam[1]; rp.v[2] = R.cam[2]; rp.v[3] = regs[o][3];
            claimed[o] = true;
        }
    }
    return np;
}

// ---- persistent match cache (phase 7j) ---------------------------------------------------------------------------
// The scanner above still walked all candidate bytes and rebuilt the matrix register images on every call. Almost nothing changes
// between two draws (the game rewrites c0..c17), so the VERIFIED matches (which register holds which of the things we look for) are kept
// between calls: a touched window drops the matches that overlap it and re-verifies the window's candidates, everything else stays.
// A call then only assembles the patches from the match list. Output is identical to buildEyePatches (equivalence test).
struct CachedMatch { int16_t reg; uint8_t kind; uint8_t tr; uint8_t cnt; double a[3], d[3]; };      // L-only facts; right-eye data is made when assembling
struct MatchCache {
    static const int kCap = 96;
    CachedMatch m[kCap]; int n = 0; bool overflow = false;
    // per key version: the left-eye register images and which (pattern, layout) combinations can match at all
    unsigned ver = 0xFFFFFFFFu;
    float wl[3][2][4][4]; bool ok[3][2]; int cnt[3][2]; float vpTL[4][4]; bool camOk = false;
    void reset() { n = 0; overflow = false; ver = 0xFFFFFFFFu; }
};

inline void cacheSetup(MatchCache& c, const EyeMats& L) {
    const float (*ml[3])[4] = { L.VP, L.P, L.V };
    for (int p = 0; p < 3; ++p) for (int t = 0; t < 2; ++t) {          // t = 0: transposed (tr = 1), t = 1: straight (tr = 0)
        const int tr = 1 - t;
        matToRegs(ml[p], tr != 0, c.wl[p][t]);
        c.cnt[p][t] = (p == 2) ? (tr ? 3 : 4) : 4;
        bool good = false;
        { float sum = 0; for (int r = 0; r < c.cnt[p][t]; ++r) for (int k = 0; k < 4; ++k) sum += std::fabs(c.wl[p][t][r][k]); good = sum > 0.05f; }
        if (good && p == 2) { float tt = 0; for (int i = 0; i < 3; ++i) tt += std::fabs(L.V[3][i]); good = tt >= 1.f; }
        c.ok[p][t] = good;
    }
    matToRegs(L.VP, true, c.vpTL);
    c.camOk = std::fabs(L.cam[0]) + std::fabs(L.cam[1]) + std::fabs(L.cam[2]) >= 1.f;
}

// Verify the candidates of one register and append the matches that start there.
inline void cacheVerifyAt(MatchCache& c, const float (*regs)[4], int n, int o, uint8_t bits, const EyeMats& L) {
    auto add = [&](int kind, int tr, int cnt) {
        if (c.n >= MatchCache::kCap) { c.overflow = true; return; }
        CachedMatch& x = c.m[c.n++]; x.reg = (int16_t)o; x.kind = (uint8_t)kind; x.tr = (uint8_t)tr; x.cnt = (uint8_t)cnt;
        x.a[0] = x.a[1] = x.a[2] = x.d[0] = x.d[1] = x.d[2] = 0;
        return;
    };
    static const uint8_t patBit[3] = { CB_VP, CB_P, CB_V };
    static const int patKind[3] = { PK_VP, PK_PROJ, PK_VIEW };
    for (int p = 0; p < 3; ++p) {
        if (!(bits & patBit[p])) continue;
        for (int t = 0; t < 2; ++t) {
            if (!c.ok[p][t] || o + c.cnt[p][t] > n) continue;
            if (regsMatch(regs, o, c.wl[p][t], c.cnt[p][t])) add(patKind[p], 1 - t, c.cnt[p][t]);
        }
    }
    if ((bits & CB_BIAS) && o + 4 <= n) {
        double a[3], d[3];
        if (matchVPBias(regs, o, c.vpTL, a, d) && std::fabs(a[0]) >= 0.02 && std::fabs(a[1]) >= 0.02) {
            const bool isPlainVP = std::fabs(a[0]-1) < 1e-3 && std::fabs(a[1]-1) < 1e-3 && std::fabs(a[2]-1) < 1e-3 &&
                                   std::fabs(d[0]) < 1e-3 && std::fabs(d[1]) < 1e-3 && std::fabs(d[2]) < 1e-3;
            if (!isPlainVP && c.n < MatchCache::kCap) {
                CachedMatch& x = c.m[c.n++]; x.reg = (int16_t)o; x.kind = PK_VPBIAS; x.tr = 1; x.cnt = 4;
                for (int i = 0; i < 3; ++i) { x.a[i] = a[i]; x.d[i] = d[i]; }
            } else if (!isPlainVP) c.overflow = true;
        }
    }
    if ((bits & CB_CAM) && c.camOk && std::fabs(regs[o][0] - L.cam[0]) <= 0.02f && std::fabs(regs[o][1] - L.cam[1]) <= 0.02f && std::fabs(regs[o][2] - L.cam[2]) <= 0.02f) add(PK_CAMPOS, 0, 1);
}

// Bring the cache up to date: `scan` (candidate bits) must already be refreshed; `dirtyLo/Hi` is the window that was re-classified.
inline void cacheUpdate(MatchCache& c, const float (*regs)[4], int n, const RegScan& scan, int dirtyLo, int dirtyHi, bool all, unsigned keyVer, const EyeMats& L) {
    if (all || c.ver != keyVer) { cacheSetup(c, L); c.ver = keyVer; c.n = 0; c.overflow = false; dirtyLo = 0; dirtyHi = n; }
    else if (dirtyLo >= dirtyHi) return;
    int w = 0;                                                         // drop the matches that start inside the window
    for (int i = 0; i < c.n; ++i) if (!(c.m[i].reg >= dirtyLo && c.m[i].reg < dirtyHi)) c.m[w++] = c.m[i];
    c.n = w;
    for (int o = dirtyLo; o < dirtyHi; ++o) { const uint8_t b = scan.cand[o]; if (b) cacheVerifyAt(c, regs, n, o, b, L); }
}

// Assemble the patches for the right eye (R) from the cached left-eye matches. Same output as buildEyePatches.
inline int buildEyePatchesCached(const float (*regs)[4], int n, const EyeMats& L, const EyeMats& R, const MatchCache& c, RegPatch* out, int maxOut) {
    if (c.overflow) return buildEyePatches(regs, n, L, R, out, maxOut);
    int np = 0;
    bool claimed[512] = {};
    if (n > 512) n = 512;
    auto freeAt = [&](int o, int cnt) { for (int i = 0; i < cnt; ++i) if (claimed[o + i]) return false; return true; };
    auto claim = [&](int o, int cnt) { for (int i = 0; i < cnt; ++i) claimed[o + i] = true; };
    const float (*mr[3])[4] = { R.VP, R.P, R.V };
    static const int patKind[3] = { PK_VP, PK_PROJ, PK_VIEW };
    // matches of one (kind, layout), ascending by register
    auto collect = [&](int kind, int tr, int* idx) {
        int k = 0;
        for (int i = 0; i < c.n; ++i) if (c.m[i].kind == kind && c.m[i].tr == tr) {
            int j = k++; while (j > 0 && c.m[idx[j - 1]].reg > c.m[i].reg) { idx[j] = idx[j - 1]; --j; } idx[j] = i;
        }
        return k;
    };
    int idx[MatchCache::kCap];
    for (int p = 0; p < 3; ++p) {
        for (int t = 0; t < 2; ++t) {
            if (!c.ok[p][t]) continue;
            const int tr = 1 - t, cnt = c.cnt[p][t];
            const int k = collect(patKind[p], tr, idx);
            if (!k) continue;
            float wr[4][4]; bool have = false;
            for (int q = 0; q < k; ++q) {
                const CachedMatch& x = c.m[idx[q]];
                if (!freeAt(x.reg, cnt)) continue;
                if (np >= maxOut) return np;
                if (!have) { matToRegs(mr[p], tr != 0, wr); have = true; }
                RegPatch& rp = out[np++];
                rp.reg = x.reg; rp.count = cnt; rp.kind = patKind[p]; rp.transposed = tr != 0;
                for (int r = 0; r < cnt; ++r) for (int cc = 0; cc < 4; ++cc) rp.v[r*4 + cc] = wr[r][cc];
                claim(x.reg, cnt);
            }
        }
    }
    {
        const int k = collect(PK_VPBIAS, 1, idx);
        if (k) {
            float vpTR[4][4]; matToRegs(R.VP, true, vpTR);
            for (int q = 0; q < k; ++q) {
                const CachedMatch& x = c.m[idx[q]];
                if (!freeAt(x.reg, 4)) continue;
                if (np >= maxOut) return np;
                RegPatch& rp = out[np++];
                rp.reg = x.reg; rp.count = 4; rp.kind = PK_VPBIAS; rp.transposed = true;
                for (int r = 0; r < 3; ++r) for (int cc = 0; cc < 4; ++cc) rp.v[r*4 + cc] = (float)(x.a[r] * vpTR[r][cc] + x.d[r] * vpTR[3][cc]);
                for (int cc = 0; cc < 4; ++cc) rp.v[12 + cc] = vpTR[3][cc];
                claim(x.reg, 4);
            }
        }
    }
    if (c.camOk && std::fabs(L.cam[0] - R.cam[0]) + std::fabs(L.cam[1] - R.cam[1]) + std::fabs(L.cam[2] - R.cam[2]) >= 1e-4f) {
        const int k = collect(PK_CAMPOS, 0, idx);
        for (int q = 0; q < k; ++q) {
            const CachedMatch& x = c.m[idx[q]];
            if (claimed[x.reg]) continue;
            if (np >= maxOut) return np;
            RegPatch& rp = out[np++];
            rp.reg = x.reg; rp.count = 1; rp.kind = PK_CAMPOS; rp.transposed = false;
            rp.v[0] = R.cam[0]; rp.v[1] = R.cam[1]; rp.v[2] = R.cam[2]; rp.v[3] = regs[x.reg][3];
            claimed[x.reg] = true;
        }
    }
    return np;
}

// ---- HUD anchored in the game's frame (phase 7d) ----------------------------------------------------------
// The HUD hangs `distance` metres in front of the GAME camera (and vertM above its centre line), not in front of the
// headset. q = head rotation relative to the game camera (the same row-vector matrix that steers the camera),
// headPos = head position relative to the reference pose in camera space (zero when positional tracking is off).
// Returns the rectangle (pixels inside this eye's region, NOT clamped to it) where a billboard of that size ends up,
// or false when it is behind the eye. With q = identity and headPos = 0 this is exactly hudRectPixels without the
// shrink-to-fit.
inline bool hudAnchoredRect(const float frustum[4], const V3& eyeOff, const Mat3& q, const V3& headPos, float distance, float widthM,
                            float uiAspect, float vertM, int regionW, int regionH, int rect[4]) {
    const float rel[3] = { 0.f - headPos.x, vertM - headPos.y, distance - headPos.z };
    float p[3];
    for (int c = 0; c < 3; ++c) p[c] = rel[0]*q.m[0][c] + rel[1]*q.m[1][c] + rel[2]*q.m[2][c];      // row vector times Q
    p[0] -= eyeOff.x; p[1] -= eyeOff.y; p[2] -= eyeOff.z;
    if (p[2] < 0.1f) return false;
    const float hw = (widthM * 0.5f) / p[2], hh = (widthM / uiAspect * 0.5f) / p[2];
    const float cx = p[0] / p[2], cy = p[1] / p[2];
    const float fw = frustum[1] - frustum[0], fh = frustum[2] - frustum[3];
    const float px0 = ((cx - hw) - frustum[0]) / fw * regionW, px1 = ((cx + hw) - frustum[0]) / fw * regionW;
    const float py0 = (frustum[2] - (cy + hh)) / fh * regionH, py1 = (frustum[2] - (cy - hh)) / fh * regionH;
    rect[0] = (int)std::lround(px0); rect[1] = (int)std::lround(py0);
    rect[2] = (int)std::lround(px1 - px0); rect[3] = (int)std::lround(py1 - py0);
    if (rect[2] < 2) rect[2] = 2;
    if (rect[3] < 2) rect[3] = 2;
    return true;
}


// Corners (top-left, top-right, bottom-left, bottom-right) of the HUD panel in EYE space, for drawing it as a real quad.
// The panel is a flat rectangle fixed in the game's frame: widthM wide, widthM/uiAspect tall, centred distance metres ahead
// of the game camera and vertM above its centre line. It does NOT turn to face the viewer.
// Returns false when the whole panel is closer than nearZ (behind the viewer).
inline bool hudQuadEyeSpace(const V3& eyeOff, const Mat3& q, const V3& headPos, float distance, float widthM, float uiAspect,
                            float vertM, float nearZ, float out[4][3]) {
    const float hw = widthM * 0.5f, hh = widthM / uiAspect * 0.5f;
    const float cx[4] = { -hw, hw, -hw, hw }, cy[4] = { vertM + hh, vertM + hh, vertM - hh, vertM - hh };
    bool any = false;
    for (int i = 0; i < 4; ++i) {
        const float rel[3] = { cx[i] - headPos.x, cy[i] - headPos.y, distance - headPos.z };
        for (int c = 0; c < 3; ++c) out[i][c] = rel[0]*q.m[0][c] + rel[1]*q.m[1][c] + rel[2]*q.m[2][c];
        out[i][0] -= eyeOff.x; out[i][1] -= eyeOff.y; out[i][2] -= eyeOff.z;
        if (out[i][2] > nearZ) any = true;
    }
    return any;
}

// ---- HUD panel shape and lazy follow ------------------------------------------------------------------------------
// One point of the HUD panel in the game camera's frame (x right, y up, z forward, metres from the default head
// position), for texture coordinates u, v in [0,1] (v = 0 at the top).
//   curved = false: a flat rectangle widthM wide at `distance` (the original panel).
//   curved = true : a strip of a cylinder around the head with radius `distance`, spanning the same angle as the flat
//                   panel would (so switching it on keeps the HUD's size and only bends it). Every column of the HUD is
//                   then the same distance away and faces you, so the edges are as readable as the middle. Its height
//                   is scaled to the arc's length, so the UI keeps its proportions (bending a strip of the full flat
//                   height would make everything look stretched upwards, by ~19% with the default 2.4 m at 1.5 m).
//   tiltDeg       : leans the panel back about its horizontal centre line (top farther away), so a panel placed below
//                   eye level (vertM < 0) faces a downward glance. 0 = upright.
//   yaw           : turns the whole panel about the vertical axis through the head (radians, + = to the right).
inline V3 hudPanelPoint(float u, float v, float distance, float widthM, float uiAspect, float vertM, bool curved,
                        float tiltDeg, float yaw) {
    float hh = widthM / uiAspect * 0.5f;
    const float half = std::atan(widthM * 0.5f / distance);
    if (curved && half > 1e-4f) hh *= 2.f * half * distance / widthM;     // arc length / flat width
    const float yl = hh - v * 2.f * hh;
    const float t = tiltDeg * 3.14159265f / 180.f;
    const float dy = yl * std::cos(t), dz = yl * std::sin(t);
    float x, z;
    if (curved) {
        const float th = (u - 0.5f) * 2.f * half, r = distance + dz;
        x = r * std::sin(th); z = r * std::cos(th);
    }
    else { x = (u - 0.5f) * widthM; z = distance + dz; }
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    return { x * cy + z * sy, vertM + dy, -x * sy + z * cy };
}
// The same transform hudQuadEyeSpace applies to its corners: game frame -> this eye's space.
inline void hudPointToEye(const V3& pt, const V3& eyeOff, const Mat3& q, const V3& headPos, float out[3]) {
    const float rel[3] = { pt.x - headPos.x, pt.y - headPos.y, pt.z - headPos.z };
    for (int c = 0; c < 3; ++c) out[c] = rel[0]*q.m[0][c] + rel[1]*q.m[1][c] + rel[2]*q.m[2][c];
    out[0] -= eyeOff.x; out[1] -= eyeOff.y; out[2] -= eyeOff.z;
}
// Heading of the head in the game frame (radians, + = right), for the head rotation q as used above.
inline float headYawOf(const Mat3& q) { return std::atan2(q.m[0][2], q.m[2][2]); }
inline float wrapPi(float a) {
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    return a;
}
// Lazy follow: the panel stays where it is while you look around it, and once your heading is more than startDeg away
// it glides after you (time constant tau seconds) until it is back within 2 degrees. *following carries that state.
inline float lazyFollowYaw(float panelYaw, float headYaw, float dt, float startDeg, float tau, bool* following) {
    const float d = wrapPi(headYaw - panelYaw), ad = std::fabs(d) * 180.f / 3.14159265f;
    if (!*following && ad > startDeg) *following = true;
    if (*following) {
        if (ad < 2.f) { *following = false; return panelYaw; }
        const float k = (dt <= 0.f) ? 0.f : (dt >= tau * 4.f ? 1.f : 1.f - std::exp(-dt / tau));
        return wrapPi(panelYaw + d * k);
    }
    return panelYaw;
}

// ---- laser pointer ---------------------------------------------------------------------------------------------------
// Direction of the game's mouse-pick ray in its camera's axes (x right, y up, z forward), for the cursor at texture
// coordinates u, v (v = 0 at the top) and the camera's Gamebryo frustum {left, right, top, bottom} at unit distance.
// Normalised. This is the ray the game itself uses to decide what is under the mouse.
inline V3 pickRayCamera(float u, float v, const float fr[4]) {
    const float x = fr[0] + u * (fr[1] - fr[0]), y = fr[2] + v * (fr[3] - fr[2]);
    const float n = std::sqrt(x * x + y * y + 1.f);
    return { x / n, y / n, 1.f / n };
}
// Row vector [x y z 1] times a 4x4 matrix (D3D convention): clip-space position.
inline void xformPoint(const float p[3], const float m[4][4], float out[4]) {
    for (int c = 0; c < 4; ++c) out[c] = p[0] * m[0][c] + p[1] * m[1][c] + p[2] * m[2][c] + m[3][c];
}
// Clip the segment a-b (clip space) to w >= eps, in place. False if nothing is left in front of the eye.
inline bool clipSegmentW(float a[4], float b[4], float eps) {
    if (a[3] < eps && b[3] < eps) return false;
    if (a[3] < eps || b[3] < eps) {
        float* in = a[3] >= eps ? a : b; float* out = a[3] >= eps ? b : a;
        const float t = (in[3] - eps) / (in[3] - out[3]);
        for (int c = 0; c < 4; ++c) out[c] = in[c] + (out[c] - in[c]) * t;
    }
    return true;
}
// Probe i of n along the ray, log-spaced from dmin to dmax (any units).
inline float laserProbeDistance(int i, int n, float dmin, float dmax) {
    if (n < 2) return dmin;
    return dmin * std::pow(dmax / dmin, (float)i / (float)(n - 1));
}
// Where the ray hits, from the probes' visibility (1 visible, 0 hidden behind something, -1 not tested / off screen):
// the first hidden probe that follows a visible one. Returns the geometric mean of the two distances, or -1 (sky,
// nothing hit, or no visible probe to anchor on).
inline float laserHitDistance(const int* state, const float* dist, int n) {
    int lastVisible = -1;
    for (int i = 0; i < n; ++i) {
        if (state[i] == 1) lastVisible = i;
        else if (state[i] == 0 && lastVisible >= 0) return std::sqrt(dist[lastVisible] * dist[i]);
    }
    return -1.f;
}

// ---- planar water reflection (phase 7f) --------------------------------------------------------------------------
// WAR renders the reflection map once per frame from a "reflection camera" that is the game camera mirrored about z:
//   right' = M right, fwd' = M fwd, up' = -M up   (M flips z), same position. The shader then looks the map up with the
// SCREEN position of each water pixel, so the map is only right if its camera has the same orientation and frustum as the
// view that draws the water. In VR each eye has its own asymmetric frustum, so the two eyes would sample the single map
// at places ~20% of its width apart. Fix: render the map with a symmetric frustum covering both eyes and the head-steered
// orientation, and draw the water in each eye with that same symmetric projection (viewport widened to keep the geometry
// where it was, scissored to the eye) so both eyes look the map up identically.
inline V3 mirrorZ(const V3& v) { return { v.x, v.y, -v.z }; }
inline float dot3(const V3& a, const V3& b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
// Maps a camera basis to the engine's reflection-camera basis. It is its own inverse.
inline void reflectionBasis(const V3& r, const V3& u, const V3& f, V3* ro, V3* uo, V3* fo) {
    *ro = mirrorZ(r); *fo = mirrorZ(f);
    const V3 m = mirrorZ(u); *uo = { -m.x, -m.y, -m.z };
}
inline bool isReflectionOf(const V3& r, const V3& u, const V3& f, const V3& gr, const V3& gu, const V3& gf, float minDot) {
    V3 a, b, c; reflectionBasis(gr, gu, gf, &a, &b, &c);
    return dot3(r, a) > minDot && dot3(u, b) > minDot && dot3(f, c) > minDot;
}
// Frustum symmetric in x that covers both eyes (the vertical range is shared by the eyes). Returns S, the half width in tan.
inline float symmetricFrustum(const float f0[4], const float f1[4], float out[4]) {
    float S = std::fabs(f0[0]);
    S = std::max(S, std::fabs(f0[1])); S = std::max(S, std::fabs(f1[0])); S = std::max(S, std::fabs(f1[1]));
    out[0] = -S; out[1] = S; out[2] = f0[2]; out[3] = f0[3];
    return S;
}
// The viewport (pixels) that makes a symmetric-frustum projection land every point exactly where the eye's own projection
// puts it inside the eye's region (rx, ry, rw, rh). It extends beyond the region for the part of the symmetric frustum the
// eye does not see, so the caller must scissor to the region.
inline void symmetricViewport(const float eyeFr[4], float S, int rx, int ry, int rw, int rh, int out[4]) {
    const float fw = eyeFr[1] - eyeFr[0];
    out[0] = rx + (int)std::lround((-S - eyeFr[0]) / fw * rw);
    out[1] = ry;
    out[2] = (int)std::lround(2.f * S / fw * rw);
    out[3] = rh;
}

// ---- mouse pitch lock (phase 7l) ----------------------------------------------------------------------------------
// Remove the pitch of the GAME camera (the one the mouse tilts) and keep its heading, so the headset's own pitch is the only up/down
// rotation. The world's up axis is the one the camera's up vector mostly points along (WAR: z, with up = -z). Roll is removed too (WAR has
// none). Returns false, leaving the basis as it was, when the camera looks almost straight along the up axis (heading undefined).
inline bool levelBasis(const V3& r, const V3& u, const V3& f, V3* ro, V3* uo, V3* fo) {
    const float s = (u.z < 0.f) ? -1.f : 1.f;
    const float hl = std::sqrt(f.x * f.x + f.y * f.y);
    if (hl < 0.05f) { *ro = r; *uo = u; *fo = f; return false; }
    const V3 nf = { f.x / hl, f.y / hl, 0.f };
    V3 rh = { r.x, r.y, 0.f };                                   // the original right vector, flattened and made perpendicular to the new forward
    const float d = rh.x * nf.x + rh.y * nf.y;
    rh.x -= d * nf.x; rh.y -= d * nf.y;
    float rl = std::sqrt(rh.x * rh.x + rh.y * rh.y);
    if (rl < 1e-4f) { *ro = r; *uo = u; *fo = f; return false; }
    *ro = { rh.x / rl, rh.y / rl, 0.f };
    *uo = { 0.f, 0.f, s };
    *fo = nf;
    return true;
}

// ---- tracking-space resets and culling frustum ---------------------------------------------------------
// A SteamVR "reset seated zero pose" (or putting the headset on after VR started with it on the desk) makes the
// reported pose jump. Detect a jump between two consecutive frames: more than maxDist metres or maxYaw radians.
inline float angleDiff(float a, float b) {
    float d = a - b;
    const float PI = 3.14159265f;
    while (d > PI) d -= 2.f * PI;
    while (d < -PI) d += 2.f * PI;
    return d;
}
inline bool poseJumped(const float p0[3], const float p1[3], float yaw0, float yaw1, float maxDist = 0.5f, float maxYaw = 1.0f) {
    const float dx = p1[0]-p0[0], dy = p1[1]-p0[1], dz = p1[2]-p0[2];
    if (dx*dx + dy*dy + dz*dz > maxDist*maxDist) return true;
    return std::fabs(angleDiff(yaw1, yaw0)) > maxYaw;
}

// Frustum (x0 x1 y0 y1) covering both eyes, padded by `pad` (fraction), for the engine's culling.
inline void unionFrustum(const float a[4], const float b[4], float pad, float out[4]) {
    out[0] = std::fmin(a[0], b[0]); out[1] = std::fmax(a[1], b[1]);
    out[2] = std::fmax(a[2], b[2]); out[3] = std::fmin(a[3], b[3]);
    const float w = out[1] - out[0], h = out[2] - out[3];
    out[0] -= w * pad; out[1] += w * pad; out[2] += h * pad; out[3] -= h * pad;
}

// Gamebryo camera rotation (row-major 3x3 at camera+0x60). WAR passes direction = column 0, up = column 1,
// right = column 2 to SetCamera. Write a basis into / read a basis from such a matrix.
inline void basisToGamebryoRot(const V3& fwd, const V3& up, const V3& right, float m[9]) {
    m[0] = fwd.x; m[1] = up.x; m[2] = right.x;
    m[3] = fwd.y; m[4] = up.y; m[5] = right.y;
    m[6] = fwd.z; m[7] = up.z; m[8] = right.z;
}
inline bool gamebryoRotMatchesBasis(const float m[9], const V3& fwd, const V3& up, const V3& right, float tol = 1e-4f) {
    auto close = [&](float a, float b) { return std::fabs(a - b) <= tol; };
    return close(m[0],fwd.x) && close(m[3],fwd.y) && close(m[6],fwd.z) &&
           close(m[1],up.x)  && close(m[4],up.y)  && close(m[7],up.z)  &&
           close(m[2],right.x) && close(m[5],right.y) && close(m[8],right.z);
}


// Angle in degrees between the viewing directions of two head orientations (OpenVR head: forward = -Z column).
inline float headAngleDeg(const Mat3& a, const Mat3& b) {
    const float fa[3] = { -a.m[0][2], -a.m[1][2], -a.m[2][2] }, fb[3] = { -b.m[0][2], -b.m[1][2], -b.m[2][2] };
    float d = fa[0]*fb[0] + fa[1]*fb[1] + fa[2]*fb[2];
    d = d > 1.f ? 1.f : (d < -1.f ? -1.f : d);
    return std::acos(d) * 57.29578f;
}


// ---- render-size budget, rectangle scaling, cursor mapping and shape ----------------------------------
// SteamVR's recommended per-eye size can be enormous (5976x6252 = 37 Mpx per eye, 75 Mpx per frame for side-by-side).
// Scale it down (keeping its aspect) so that BOTH eyes together stay within maxPixels (0 = unlimited), then apply
// `scale`. Results are even and at least 256.
inline void fitEyeSize(unsigned recW, unsigned recH, double maxPixels, double scale, unsigned* ew, unsigned* eh) {
    double k = scale > 0.0 ? scale : 1.0;
    if (maxPixels > 0.0) {
        const double total = 2.0 * (double)recW * (double)recH * k * k;
        if (total > maxPixels) k *= std::sqrt(maxPixels / total);
    }
    unsigned w = (unsigned)((double)recW * k + 0.5), h = (unsigned)((double)recH * k + 0.5);
    w &= ~1u; h &= ~1u;
    if (w < 256) w = 256;
    if (h < 256) h = 256;
    *ew = w; *eh = h;
}

// Scale an integer rectangle by sx/sy (used when the game believes its screen is smaller than the real target).
inline void scaleRect(long in[4] /* x1 y1 x2 y2 */, float sx, float sy, long out[4]) {
    out[0] = (long)std::lround(in[0] * sx); out[1] = (long)std::lround(in[1] * sy);
    out[2] = (long)std::lround(in[2] * sx); out[3] = (long)std::lround(in[3] * sy);
}

// Mouse position inside the game window as 0..1 coordinates (clamped), from client-area pixels.
inline void cursorUV(long px, long py, long clientW, long clientH, float* u, float* v) {
    float fu = clientW > 0 ? (float)px / (float)clientW : 0.5f, fv = clientH > 0 ? (float)py / (float)clientH : 0.5f;
    *u = fu < 0.f ? 0.f : (fu > 1.f ? 1.f : fu);
    *v = fv < 0.f ? 0.f : (fv > 1.f ? 1.f : fv);
}

// Arrow cursor as a triangle list in "cursor units" (tip at 0,0; x right, y DOWN; about 12.5 wide, 20 tall).
// Returns the number of vertices written (3 per triangle) into xy[2*n].
inline int cursorArrowTriangles(float* xy) {
    const float A[2] = {0.f, 0.f}, B[2] = {0.f, 17.f}, C[2] = {4.2f, 13.2f}, D[2] = {7.2f, 20.f}, E[2] = {10.f, 18.8f},
                F[2] = {7.0f, 12.2f}, G[2] = {12.5f, 12.2f};
    const float* tri[5][3] = { {A,B,C}, {A,C,G}, {C,F,G}, {C,D,E}, {C,E,F} };
    int n = 0;
    for (auto& t : tri) {
        // consistent winding (negative cross product in this y-down space) regardless of how the polygon was split
        const float cr = (t[1][0] - t[0][0]) * (t[2][1] - t[0][1]) - (t[1][1] - t[0][1]) * (t[2][0] - t[0][0]);
        const float* v0 = t[0]; const float* v1 = cr > 0.f ? t[2] : t[1]; const float* v2 = cr > 0.f ? t[1] : t[2];
        for (const float* p : { v0, v1, v2 }) { xy[2*n] = p[0]; xy[2*n+1] = p[1]; ++n; }
    }
    return n;
}

} // namespace vrm
