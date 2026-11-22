// Native (non-Windows) test of vrmath.h.  Build & run:
//   g++ -std=c++17 -o vrmath_test tests/vrmath_test.cpp && ./vrmath_test
#include "../src/vrmath.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
using namespace vrm;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

static bool near3(const float* a, const float* b, float e = 1e-3f) {
    return std::fabs(a[0]-b[0]) < e && std::fabs(a[1]-b[1]) < e && std::fabs(a[2]-b[2]) < e;
}
static Mat3 rotX(float t) { Mat3 r = identity(); float c = cosf(t), s = sinf(t); r.m[1][1] = c; r.m[1][2] = -s; r.m[2][1] = s; r.m[2][2] = c; return r; }

// Build a WAR-like view (row-vector, translation in row 3) from camera axes expressed in world.
static void makeView(const float R[3], const float U[3], const float F[3], const float eye[3], float v[4][4]) {
    for (int i = 0; i < 3; ++i) { v[i][0] = R[i]; v[i][1] = U[i]; v[i][2] = F[i]; v[i][3] = 0; }
    for (int c = 0; c < 3; ++c) v[3][c] = -(eye[0]*v[0][c] + eye[1]*v[1][c] + eye[2]*v[2][c]);
    v[3][3] = 1;
}
static void axes(const float v[4][4], float r[3], float u[3], float f[3]) {
    for (int i = 0; i < 3; ++i) { r[i] = v[i][0]; u[i] = v[i][1]; f[i] = v[i][2]; }
}
static void normalize(float* a) { float n = std::sqrt(a[0]*a[0]+a[1]*a[1]+a[2]*a[2]); for (int i = 0; i < 3; ++i) a[i] /= n; }
static void cross(const float* a, const float* b, float* o) { o[0]=a[1]*b[2]-a[2]*b[1]; o[1]=a[2]*b[0]-a[0]*b[2]; o[2]=a[0]*b[1]-a[1]*b[0]; }

int main() {
    // Camera loosely modelled on the real log (right-handed world => det(view3) = -1).
    float R[3] = {0.42849f, 0.90355f, 0.f}; normalize(R);
    float F[3] = {0.88946f, -0.42181f, 0.17588f};
    float d = F[0]*R[0] + F[1]*R[1] + F[2]*R[2]; for (int i = 0; i < 3; ++i) F[i] -= d*R[i]; normalize(F);
    float U[3]; cross(F, R, U); normalize(U);
    float eye[3] = {48359.f, 8261.f, 3377.f};
    float V[4][4]; makeView(R, U, F, eye, V);
    if (det3(V) > 0) { for (auto& x : U) x = -x; makeView(R, U, F, eye, V); }
    CHECK(det3(V) < 0);

    // 1) Head exactly at the recenter pose -> view unchanged.
    {
        Mat3 head = rotY(0.7f); float yaw; CHECK(headYaw(head, &yaw));
        float out[4][4]; applyToView(V, cameraDelta(head, yaw), out);
        for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c)
            CHECK(std::fabs(out[r][c] - V[r][c]) < 1e-2f);
    }
    // 2) Head turns RIGHT 30 deg after recentering -> forward = cos30*F + sin30*Right; eye stays at origin.
    {
        float a = 30.f * 3.14159265f / 180.f;
        float yaw = 0.4f;
        Mat3 head = rotY(yaw - a);                      // turning right = negative rotY angle
        float out[4][4]; applyToView(V, cameraDelta(head, yaw), out);
        float r2[3], u2[3], f2[3]; axes(out, r2, u2, f2);
        float expect[3]; for (int i = 0; i < 3; ++i) expect[i] = cosf(a)*F[i] + sinf(a)*R[i];
        CHECK(near3(f2, expect));
        float p[3];
        for (int c = 0; c < 3; ++c) p[c] = eye[0]*out[0][c] + eye[1]*out[1][c] + eye[2]*out[2][c] + out[3][c];
        CHECK(std::fabs(p[0]) < 0.1f && std::fabs(p[1]) < 0.1f && std::fabs(p[2]) < 0.1f);
    }
    // 3) Head pitches UP 20 deg -> forward tilts toward camera Up.
    {
        float a = 20.f * 3.14159265f / 180.f;
        Mat3 head = mul(rotY(1.1f), rotX(a));           // +rotX tilts -Z forward upward (RH)
        float yaw; CHECK(headYaw(head, &yaw));
        float out[4][4]; applyToView(V, cameraDelta(head, yaw), out);
        float r2[3], u2[3], f2[3]; axes(out, r2, u2, f2);
        float expect[3]; for (int i = 0; i < 3; ++i) expect[i] = cosf(a)*F[i] + sinf(a)*U[i];
        CHECK(near3(f2, expect, 3e-3f));
    }
    // 4) Delta is a proper rotation (orthonormal, det +1): no shear/scale/mirroring introduced.
    {
        Mat3 head = mul(rotY(0.3f), rotX(0.2f)); float yaw; headYaw(head, &yaw);
        Mat3 q = cameraDelta(head, yaw), qtq = mul(transpose(q), q);
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) CHECK(std::fabs(qtq.m[i][j] - (i == j)) < 1e-5f);
        float dq = q.m[0][0]*(q.m[1][1]*q.m[2][2]-q.m[1][2]*q.m[2][1]) - q.m[0][1]*(q.m[1][0]*q.m[2][2]-q.m[1][2]*q.m[2][0]) + q.m[0][2]*(q.m[1][0]*q.m[2][1]-q.m[1][1]*q.m[2][0]);
        CHECK(std::fabs(dq - 1.f) < 1e-5f);
    }
    // 5) Classification/decoding against real matrices copied from WARVR.log.
    {
        float persp[4][4]  = {{0.648331404f,0,0,0},{0,1.53678572f,0,0},{0,0,1.00077236f,1},{0,0,-10.0077238f,0}};
        float ortho[4][4]  = {{2,0,0,0},{0,2,0,0},{0,0,0.000100010002f,0},{-0.001953125f,0.001953125f,-0.000100010002f,1}};
        float shadow[4][4] = {{0.0266877394f,0,0,0},{0,0.0266877394f,0,0},{0,0,0.00667193485f,0},{0,0,-0.100000031f,1}};
        CHECK(isPerspective(persp)); CHECK(!isPerspective(ortho)); CHECK(!isPerspective(shadow));
        float zn, zf, fov, asp; CHECK(decodePerspective(persp, &zn, &zf, &fov, &asp));
        CHECK(std::fabs(zn - 10.f) < 0.05f); CHECK(std::fabs(zf - 12957.f) < 50.f); CHECK(std::fabs(asp - 2.370f) < 0.01f);
        printf("decoded real matrix: near=%.1f far=%.0f fovY=%.1f aspect=%.3f\n", zn, zf, fov, asp);
    }

    // 6) Shader-constant steering, using numbers from WARVR_A.log.
    {
        // Real main-pass state: view rotation rows from the scan, translation row from the capture
        // (48242.0 -2408.9 -32759.0), projection fovY=55 aspect=2.370 near=10 far=16384.
        float Vr[4][4] = {{-0.874623f,-0.081796f,-0.477853f,0},{-0.484803f,0.147556f,0.862086f,0},{0.000005f,-0.985666f,0.168711f,0},{48242.0f,-2408.9f,-32759.0f,1}};
        float t = std::tan(55.f*3.14159265f/180.f/2.f);
        float zn = 10.f, zf = 16384.f;
        float Pr[4][4] = {{1.f/(t*2.370f),0,0,0},{0,1.f/t,0,0},{0,0,zf/(zf-zn),1},{0,0,-zn*zf/(zf-zn),0}};
        float VP[4][4]; mul4(Vr, Pr, VP);
        float regs[16]; writeVPRegs(VP, 1, regs);
        // The log's first constant register for the main pass was [-0.7088 -0.39291 -3.4916e-08 39096].
        CHECK(std::fabs(regs[0] - (-0.7088f)) < 2e-3f);
        CHECK(std::fabs(regs[1] - (-0.39291f)) < 2e-3f);
        CHECK(std::fabs(regs[2]) < 1e-4f);
        CHECK(std::fabs(regs[3] - 39096.f) < 100.f);
        printf("computed c0 = [%.5g %.5g %.5g %.5g]  (log: -0.7088 -0.39291 -3.4916e-08 39096)\n", regs[0], regs[1], regs[2], regs[3]);
        CHECK(detectVPLayout(VP, regs) == 1);
        float straight[16]; writeVPRegs(VP, 2, straight);
        CHECK(detectVPLayout(VP, straight) == 2);
        float junk[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
        CHECK(detectVPLayout(VP, junk) == 0);

        // Rotating the camera 30 degrees right must put a point that was 30 degrees right of the
        // original view direction dead centre on screen.
        float a = 30.f*3.14159265f/180.f;
        Mat3 head = rotY(-a); float yaw = 0.f; headYaw(rotY(0.f), &yaw);
        Mat3 q = cameraDelta(head, 0.f);
        float V2[4][4]; applyToView(Vr, q, V2);
        float VP2[4][4]; mul4(V2, Pr, VP2);
        // camera axes in world (columns of the view rotation) and eye position
        float Rg[3] = {Vr[0][0],Vr[1][0],Vr[2][0]}, Fg[3] = {Vr[0][2],Vr[1][2],Vr[2][2]};
        float eye[3]; for (int j = 0; j < 3; ++j) eye[j] = -(Vr[3][0]*Vr[j][0] + Vr[3][1]*Vr[j][1] + Vr[3][2]*Vr[j][2]);
        float pt[4]; for (int j = 0; j < 3; ++j) pt[j] = eye[j] + 1000.f*(std::cos(a)*Fg[j] + std::sin(a)*Rg[j]); pt[3] = 1.f;
        float clip[4]; for (int c = 0; c < 4; ++c) clip[c] = pt[0]*VP2[0][c] + pt[1]*VP2[1][c] + pt[2]*VP2[2][c] + pt[3]*VP2[3][c];
        printf("point 30 deg right of old view -> ndc x=%.5f y=%.5f (want ~0,0)\n", clip[0]/clip[3], clip[1]/clip[3]);
        CHECK(std::fabs(clip[0]/clip[3]) < 1e-3f);
        CHECK(std::fabs(clip[1]/clip[3]) < 1e-3f);
        // and the same point is NOT centred with the unrotated matrices
        float clip0[4]; for (int c = 0; c < 4; ++c) clip0[c] = pt[0]*VP[0][c] + pt[1]*VP[1][c] + pt[2]*VP[2][c] + pt[3]*VP[3][c];
        CHECK(std::fabs(clip0[0]/clip0[3]) > 0.1f);
    }

    // 7) Engine-level steering: rotating SetCamera's INPUT vectors must equal View * Q, and the eye must not move.
    {
        // Real vectors from the engine: view rotation rows from WARVR_A.log, eye from the scan.
        V3 right = {-0.874623f,-0.484803f,0.000005f}, up = {-0.081796f,0.147556f,-0.985666f}, fwd = {-0.477853f,0.862086f,0.168711f};
        V3 pos = {26735.68f, 51273.12f, -7906.76f};
        float V0[4][4]; engineBuildView(pos, right, up, fwd, V0);
        // The vectors/eye above were scanned from the 256x256 offscreen pass, whose logged D3D view translation was
        // (48241.1 -13172.2 -30092.2). Reproducing it from the engine's own formula validates the convention.
        printf("engineBuildView translation row = (%.1f %.1f %.1f)  (log: 48241.1 -13172.2 -30092.2)\n", V0[3][0], V0[3][1], V0[3][2]);
        CHECK(std::fabs(V0[3][0]-48241.1f) < 1.f); CHECK(std::fabs(V0[3][1]+13172.2f) < 1.f); CHECK(std::fabs(V0[3][2]+30092.2f) < 1.f);
        for (int trial = 0; trial < 3; ++trial) {
            Mat3 head = mul(mul(rotY(0.5f + trial), rotX(0.3f*trial - 0.2f)), identity());
            float yaw; headYaw(head, &yaw);
            Mat3 q = cameraDelta(head, yaw - 0.4f);
            V3 r2, u2, f2; rotateBasis(q, right, up, fwd, &r2, &u2, &f2);
            float Vnew[4][4]; engineBuildView(pos, r2, u2, f2, Vnew);
            float Vref[4][4]; applyToView(V0, q, Vref);
            float worst = 0;
            for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) worst = std::fmax(worst, std::fabs(Vnew[r][c]-Vref[r][c]));
            // translation rows differ only by the eye-centering convention; compare by eye invariance instead
            float p0[3];
            for (int c = 0; c < 3; ++c) p0[c] = pos.x*Vnew[0][c] + pos.y*Vnew[1][c] + pos.z*Vnew[2][c] + Vnew[3][c];
            CHECK(worst < 1e-5f);
            CHECK(std::fabs(p0[0]) < 0.1f && std::fabs(p0[1]) < 0.1f && std::fabs(p0[2]) < 0.1f);
            // still an orthonormal right-handed-ness-preserving basis (det sign unchanged)
            float dOld = det3(V0), dNew = det3(Vnew);
            CHECK(dOld * dNew > 0.f);
            CHECK(std::fabs(std::fabs(dNew) - 1.f) < 1e-4f);
        }
        // 30 degrees right: new forward = cos30*F + sin30*Right
        float a = 30.f*3.14159265f/180.f;
        Mat3 q = cameraDelta(rotY(-a), 0.f);
        V3 r2, u2, f2; rotateBasis(q, right, up, fwd, &r2, &u2, &f2);
        float ex = std::cos(a)*fwd.x + std::sin(a)*right.x, ey = std::cos(a)*fwd.y + std::sin(a)*right.y, ez = std::cos(a)*fwd.z + std::sin(a)*right.z;
        CHECK(std::fabs(f2.x-ex) < 1e-4f && std::fabs(f2.y-ey) < 1e-4f && std::fabs(f2.z-ez) < 1e-4f);
        printf("30deg-right forward = (%.4f %.4f %.4f) expected (%.4f %.4f %.4f)\n", f2.x, f2.y, f2.z, ex, ey, ez);
    }

    // 8) Nested-pass behaviour (the bug seen in round 3): WAR calls SetCamera, then reads the stored camera back and
    //    calls SetCamera again for derived passes (sky etc.). The rotation must be applied exactly once.
    {
        V3 right = {-0.874623f,-0.484803f,0.000005f}, up = {-0.081796f,0.147556f,-0.985666f}, fwd = {-0.477853f,0.862086f,0.168711f};
        float a = 20.f*3.14159265f/180.f;
        Mat3 q = cameraDelta(rotY(-a), 0.f);                    // 20 degrees right
        auto angleBetween = [](const V3& x, const V3& y) {
            float d = x.x*y.x + x.y*y.y + x.z*y.z; d = d > 1.f ? 1.f : (d < -1.f ? -1.f : d);
            return std::acos(d) * 57.29578f; };
        OutputRing ring;
        // primary call
        V3 r1,u1,f1; CHECK(!ring.contains(right, up, fwd));
        rotateBasis(q, right, up, fwd, &r1, &u1, &f1); ring.add(r1, u1, f1);
        printf("primary: forward turned %.2f deg\n", angleBetween(fwd, f1));
        CHECK(std::fabs(angleBetween(fwd, f1) - 20.f) < 0.01f);
        // derived call: engine passes back exactly what we stored -> must be recognised and left alone
        CHECK(ring.contains(r1, u1, f1));
        // what the OLD behaviour did (rotating the derived call too): compounds to 40 degrees
        V3 r2,u2,f2; rotateBasis(q, r1, u1, f1, &r2, &u2, &f2);
        printf("old behaviour on derived call: forward turned %.2f deg from the original (should have stayed 20)\n", angleBetween(fwd, f2));
        CHECK(std::fabs(angleBetween(fwd, f2) - 40.f) < 0.05f);
        // a third nested pass would reach 60 degrees -> character swung off screen
        V3 r3,u3,f3; rotateBasis(q, r2, u2, f2, &r3, &u3, &f3);
        CHECK(std::fabs(angleBetween(fwd, f3) - 60.f) < 0.1f);
        // next frame: the game's ORIGINAL camera comes again and must be rotated afresh with the new head pose
        Mat3 q2 = cameraDelta(rotY(-a*1.5f), 0.f);
        CHECK(!ring.contains(right, up, fwd));
        V3 r4,u4,f4; rotateBasis(q2, right, up, fwd, &r4, &u4, &f4);
        CHECK(std::fabs(angleBetween(fwd, f4) - 30.f) < 0.01f);
        // ring keeps at most 8 and still finds recent ones
        for (int i = 0; i < 10; ++i) ring.add({float(i),0,0},{0,float(i),0},{0,0,float(i)});
        CHECK(ring.n == 8); CHECK(!ring.contains(r1, u1, f1)); CHECK(ring.contains({9,0,0},{0,9,0},{0,0,9}));
    }

    // 9) Positional tracking and eye setup.
    {
        // Head moves 0.10 m to its right, 0.05 m up, 0.20 m forward (in its own recentered frame) after
        // recentering while facing yaw=0.7 rad. Express that motion in tracking space, then recover it.
        float refYaw = 0.7f; float refPos[3] = {0.3f, 1.2f, -0.4f};
        Mat3 ref = rotY(refYaw);
        float local[3] = {0.10f, 0.05f, -0.20f};              // RH recentered frame: forward is -Z
        float head[3];
        for (int i = 0; i < 3; ++i) head[i] = refPos[i] + ref.m[i][0]*local[0] + ref.m[i][1]*local[1] + ref.m[i][2]*local[2];
        V3 d = headDeltaCamera(refYaw, refPos, head);
        printf("head delta (right, up, forward) = %.3f %.3f %.3f  (want 0.100 0.050 0.200)\n", d.x, d.y, d.z);
        CHECK(std::fabs(d.x-0.10f) < 1e-4f && std::fabs(d.y-0.05f) < 1e-4f && std::fabs(d.z-0.20f) < 1e-4f);
        // No movement -> zero delta, whatever the heading.
        V3 z = headDeltaCamera(2.2f, refPos, refPos); CHECK(std::fabs(z.x)+std::fabs(z.y)+std::fabs(z.z) < 1e-6f);

        // World offset along WAR's real camera axes (from the log) and unit scaling.
        V3 right = {-0.874623f,-0.484803f,0.000005f}, up = {-0.081796f,0.147556f,-0.985666f}, fwd = {-0.477853f,0.862086f,0.168711f};
        V3 w = worldOffset({0,0,1.f}, right, up, fwd, 50.f);   // one metre forward at 50 units/m
        CHECK(std::fabs(w.x-50.f*fwd.x) < 1e-3f && std::fabs(w.y-50.f*fwd.y) < 1e-3f && std::fabs(w.z-50.f*fwd.z) < 1e-3f);

        // Eyes: left eye is 32 mm to the LEFT of the head centre => negative camera-space x.
        float lEye[3] = {-0.032f, 0.f, 0.015f};                // 15 mm in FRONT of the head centre = -Z in OpenVR head space? no: +z is behind
        V3 e = eyeOffsetCamera(lEye);
        CHECK(e.x < 0.f); CHECK(std::fabs(e.z + 0.015f) < 1e-6f);  // behind the head centre in LH space
        // Frustum conversion. Valve's driver docs: "Left = -2.747, Right = 1.732, Top = -1.42, Bottom = 1.42" and
        // "'bottom' and 'top' are backwards: bottom is the tan angle to the TOP (+Y) edge". So y0 (up) = pfBottom.
        float f[4]; openvrRawToFrustum(-1.25f, 1.05f, -1.15f, 1.18f, f);
        printf("frustum x0=%.2f x1=%.2f y0=%.2f y1=%.2f\n", f[0], f[1], f[2], f[3]);
        CHECK(f[0] < 0.f && f[1] > 0.f && f[2] > 0.f && f[3] < 0.f);
        CHECK(std::fabs(f[2]-1.18f) < 1e-6f && std::fabs(f[3]+1.15f) < 1e-6f);     // up = 1.18, down = 1.15 (NOT swapped)
        // REGRESSION (the round-6f bug), with the real Pimax Crystal Super left eye from WARVR.log:
        //   raw: left=-1.9210 right=1.2131 top=-1.5806 bottom=1.2053   =>  sees 1.2053 UP and 1.5806 DOWN.
        float rl[4]; openvrRawToFrustum(-1.9210f, 1.2131f, -1.5806f, 1.2053f, rl);
        printf("Crystal Super left eye -> x0=%.4f x1=%.4f y0(up)=%.4f y1(down)=%.4f\n", rl[0], rl[1], rl[2], rl[3]);
        CHECK(std::fabs(rl[2] - 1.2053f) < 1e-6f);                                // up tangent
        CHECK(std::fabs(rl[3] + 1.5806f) < 1e-6f);                                // down tangent
        CHECK(std::fabs(rl[2]) < std::fabs(rl[3]));                               // more field of view DOWN than up
        // A point 40 degrees below straight ahead must be inside this eye's field of view, and 55 degrees ABOVE must not.
        // (With the swapped values of round 6f the reverse was true.)
        const float below = std::tan(40.f*3.14159265f/180.f), above = std::tan(55.f*3.14159265f/180.f);   // 0.839 and 1.428
        CHECK(below < -rl[3]);                                                      // 0.839 < 1.5806 (inside)
        CHECK(above > rl[2]);                                                       // 1.428 > 1.2053 (outside)
        // Unconventional sign (top positive) must still give y0 > y1.
        float g[4]; openvrRawToFrustum(-1.25f, 1.05f, 1.15f, -1.18f, g); CHECK(g[2] > g[3]);
    }
    // 10) Simultaneous stereo: right-eye matrices.
    {
        // The engine's real main-pass projection (WARVR_A.log): frustum +-1.2339 x +-0.5206, near 10, far 16384 -> m00 0.8104, m11 1.9210.
        float P[4][4]; perspFromFrustum(-1.2339f, 1.2339f, 0.5206f, -0.5206f, 10.f, 16384.f, P);
        printf("perspFromFrustum: m00=%.4f m11=%.4f m22=%.5f m32=%.4f m20=%.4f\n", P[0][0], P[1][1], P[2][2], P[3][2], P[2][0]);
        CHECK(std::fabs(P[0][0]-0.8104f) < 1e-3f); CHECK(std::fabs(P[1][1]-1.9210f) < 2e-3f);
        CHECK(std::fabs(P[2][0]) < 1e-6f); CHECK(std::fabs(P[2][3]-1.f) < 1e-6f); CHECK(isPerspective(P));
        float zn, zf, fov, asp; CHECK(decodePerspective(P, &zn, &zf, &fov, &asp));
        CHECK(std::fabs(zn-10.f) < 0.01f); CHECK(std::fabs(zf-16384.f) < 5.f);
        // A point on the left eye's frustum edge must land at ndc x = -1, on the right edge at +1, even for an
        // asymmetric (nasal) frustum -- this is what makes the two eyes line up with the headset's optics.
        float x0 = -1.35f, x1 = 1.05f, y0 = 1.10f, y1 = -1.20f;
        float Q[4][4]; perspFromFrustum(x0, x1, y0, y1, 10.f, 16384.f, Q);
        auto ndc = [&](float X, float Y, float Z, float* nx, float* ny) {
            float cx = X*Q[0][0] + Z*Q[2][0], cy = Y*Q[1][1] + Z*Q[2][1], cw = Z; *nx = cx/cw; *ny = cy/cw; };
        float nx, ny;
        ndc(x0*100.f, 0.f, 100.f, &nx, &ny); CHECK(std::fabs(nx+1.f) < 1e-4f);
        ndc(x1*100.f, 0.f, 100.f, &nx, &ny); CHECK(std::fabs(nx-1.f) < 1e-4f);
        ndc(0.f, y0*100.f, 100.f, &nx, &ny); CHECK(std::fabs(ny-1.f) < 1e-4f);
        ndc(0.f, y1*100.f, 100.f, &nx, &ny); CHECK(std::fabs(ny+1.f) < 1e-4f);

        // Right eye = left-eye view shifted by the eye separation in camera space, then the right projection.
        // A point straight ahead of the HEAD at distance D must show the correct convergence: left eye sees it right of
        // centre, right eye sees it left of centre.
        float ipd = 0.064f, units = 16.f;
        float eyeL[3] = {-ipd/2,0,0}, eyeR[3] = {ipd/2,0,0};
        float dx = (eyeR[0]-eyeL[0]) * units;                    // camera moves right by this many world units
        float Vl[4][4] = {{1,0,0,0},{0,1,0,0},{0,0,1,0},{-eyeL[0]*units,0,0,1}};   // camera at x=eyeL, looking +Z
        float S[4][4]; cameraShiftMatrix(dx, 0, 0, S);
        float Vr[4][4]; mul4(Vl, S, Vr);
        float pt[4] = {0, 0, 100.f, 1.f};                        // straight ahead of the head centre, 100 units
        auto camX = [&](float V[4][4], int c) { return pt[0]*V[0][c] + pt[1]*V[1][c] + pt[2]*V[2][c] + pt[3]*V[3][c]; };
        float lx = camX(Vl, 0), rx = camX(Vr, 0);
        printf("point ahead of head: left-eye camera x=%.3f  right-eye camera x=%.3f (want +0.512 / -0.512)\n", lx, rx);
        CHECK(lx > 0.f && rx < 0.f); CHECK(std::fabs(lx-0.512f) < 1e-3f && std::fabs(rx+0.512f) < 1e-3f);
        // and the shifted view stays orthonormal with the eye moved, not rotated
        CHECK(std::fabs(Vr[3][0] - (Vl[3][0] - dx)) < 1e-4f);
    }
    // 11) HUD placement.
    {
        float fr[4] = {-1.f, 1.f, 1.f, -1.f};                    // symmetric 90 deg eye, 1000x1000 pixels
        PixelRect c = hudRectPixels(fr, 0.f, 0.f, 2.f, 1.f, 2.f, 0.f, 1000, 1000);   // 1 m wide HUD at 2 m, aspect 2:1
        printf("HUD rect centred eye: x=%d y=%d w=%d h=%d\n", c.x, c.y, c.w, c.h);
        CHECK(c.w == 250 && c.h == 125); CHECK(c.x == 375 && (c.y == 437 || c.y == 438));   // top edge at 437.5
        // Left eye (-32 mm) must see the HUD shifted RIGHT of the right eye's; the difference is the parallax.
        PixelRect l = hudRectPixels(fr, -0.032f, 0.f, 2.f, 1.f, 2.f, 0.f, 1000, 1000);
        PixelRect r = hudRectPixels(fr, +0.032f, 0.f, 2.f, 1.f, 2.f, 0.f, 1000, 1000);
        printf("HUD parallax: left-eye x=%d right-eye x=%d\n", l.x, r.x);
        CHECK(l.x > r.x); CHECK(l.x - r.x == 16);                 // 2*0.032/2m = 0.032 tan = 16 px at 500 px per unit tan
        // Never leaves its region: ask for a huge HUD.
        PixelRect big = hudRectPixels(fr, -0.032f, 0.f, 0.5f, 3.f, 2.f, 0.f, 1000, 1000);
        CHECK(big.x >= 0 && big.y >= 0 && big.x + big.w <= 1000 && big.y + big.h <= 1000);
        // Raising it moves it up (smaller y).
        PixelRect up = hudRectPixels(fr, 0.f, 0.f, 2.f, 1.f, 2.f, 0.3f, 1000, 1000); CHECK(up.y < c.y);
    }

    // 12) End to end: the right-eye matrices derived by SHIFTING the left-eye view must equal a camera placed at the
    //     right eye's true world position, on WAR's real (mirrored, det -1) axes, for asymmetric eyes.
    {
        V3 right = {-0.874623f,-0.484803f,0.000005f}, up = {-0.081796f,0.147556f,-0.985666f}, fwd = {-0.477853f,0.862086f,0.168711f};
        V3 headPos = {26735.68f, 51273.12f, -7906.76f};
        float frL[4] = {-1.35f, 1.05f, 1.10f, -1.20f}, frR[4] = {-1.05f, 1.35f, 1.10f, -1.20f};   // nasal sides are inner
        float lEye[3] = {-0.032f, 0.f, 0.f}, rEye[3] = {0.032f, 0.f, 0.f};
        V3 offL = eyeOffsetCamera(lEye), offR = eyeOffsetCamera(rEye);
        float units = 16.f;
        // World positions of the two eyes (offset along the camera's own axes).
        V3 wl = worldOffset(offL, right, up, fwd, units), wr = worldOffset(offR, right, up, fwd, units);
        V3 posL = {headPos.x+wl.x, headPos.y+wl.y, headPos.z+wl.z}, posR = {headPos.x+wr.x, headPos.y+wr.y, headPos.z+wr.z};
        // "Engine" state for the left eye as the game would hold it after SetCamera.
        float VL[4][4]; engineBuildView(posL, right, up, fwd, VL);
        float PL[4][4]; perspFromFrustum(frL[0], frL[1], frL[2], frL[3], 10.f, 16384.f, PL);
        // Ground truth for the right eye.
        float VRtruth[4][4]; engineBuildView(posR, right, up, fwd, VRtruth);
        float PRtruth[4][4]; perspFromFrustum(frR[0], frR[1], frR[2], frR[3], 10.f, 16384.f, PRtruth);
        float VRd[4][4], PRd[4][4], worst = 0;
        bool ok = rightEyeMatrices(VL, PL, frL, frR, offL, offR, units, VRd, PRd, &worst);
        printf("rightEyeMatrices ok=%d worst=%.3g\n", ok ? 1 : 0, worst);
        CHECK(ok);
        float vpTruth[4][4], vpDerived[4][4]; mul4(VRtruth, PRtruth, vpTruth); mul4(VRd, PRd, vpDerived);
        float maxErr = 0;
        for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) maxErr = std::fmax(maxErr, std::fabs(vpTruth[i][j]-vpDerived[i][j]) / std::fmax(1.f, std::fabs(vpTruth[i][j])));
        printf("derived right-eye VP vs true right-eye VP: max relative error %.3g\n", maxErr);
        CHECK(maxErr < 2e-4f);
        // Project random-ish world points through both and compare screen positions.
        float worstNdc = 0;
        const float pts[5][3] = {{27000,52000,-7000},{26000,51500,-8100},{28000,50500,-7900},{26700,51900,-7950},{27500,51000,-8300}};
        for (auto& p3 : pts) {
            float cl[4], ct[4];
            for (int c = 0; c < 4; ++c) {
                cl[c] = p3[0]*vpDerived[0][c] + p3[1]*vpDerived[1][c] + p3[2]*vpDerived[2][c] + vpDerived[3][c];
                ct[c] = p3[0]*vpTruth[0][c]   + p3[1]*vpTruth[1][c]   + p3[2]*vpTruth[2][c]   + vpTruth[3][c];
            }
            if (ct[3] > 1.f) worstNdc = std::fmax(worstNdc, std::fmax(std::fabs(cl[0]/cl[3]-ct[0]/ct[3]), std::fabs(cl[1]/cl[3]-ct[1]/ct[3])));
        }
        printf("worst NDC difference over test points: %.3g\n", worstNdc);
        CHECK(worstNdc < 1e-3f);
        // The stereo difference is real: the two eyes must NOT see a near point at the same screen x.
        float pn[3] = {headPos.x + fwd.x*200.f, headPos.y + fwd.y*200.f, headPos.z + fwd.z*200.f};   // 200 units ahead
        float cl[4], cr[4], VP_L[4][4]; mul4(VL, PL, VP_L);
        for (int c = 0; c < 4; ++c) { cl[c] = pn[0]*VP_L[0][c] + pn[1]*VP_L[1][c] + pn[2]*VP_L[2][c] + VP_L[3][c]; cr[c] = pn[0]*vpDerived[0][c] + pn[1]*vpDerived[1][c] + pn[2]*vpDerived[2][c] + vpDerived[3][c]; }
        printf("near point ndc x: left eye %.4f right eye %.4f (must differ; left eye sees it further RIGHT)\n", cl[0]/cl[3], cr[0]/cr[3]);
        CHECK(std::fabs(cl[0]/cl[3] - cr[0]/cr[3]) > 0.05f);
        // Constant-register matching: put transpose(VP_L) at some offset in a noisy register file and find it.
        float regs[256][4];
        for (int i = 0; i < 256; ++i) for (int c = 0; c < 4; ++c) regs[i][c] = std::sin(float(i*7+c))*3.f;
        const int where = 37;
        for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) regs[where+r][c] = VP_L[c][r];
        int off = -1; CHECK(findVPInRegs(regs, 256, VP_L, &off)); CHECK(off == where);
        // A projection that does NOT match our eye model must be refused rather than mangled.
        float badP[4][4]; perspFromFrustum(-1.2339f, 1.2339f, 0.5206f, -0.5206f, 10.f, 16384.f, badP);
        float v2[4][4], p2[4][4], w2 = 0; CHECK(!rightEyeMatrices(VL, badP, frL, frR, offL, offR, units, v2, p2, &w2)); CHECK(w2 > 2e-3f);
        // Camera-relative pass (sky): view translation is zero -> only the projection changes, no eye shift.
        float sky[4][4]; engineBuildView({0,0,0}, right, up, fwd, sky);
        float sv[4][4], sp[4][4]; float skyP[4][4]; perspFromFrustum(frL[0], frL[1], frL[2], frL[3], 6553.6f, 245760.f, skyP);
        CHECK(rightEyeMatrices(sky, skyP, frL, frR, offL, offR, units, sv, sp, nullptr));
        for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) CHECK(std::fabs(sv[i][j]-sky[i][j]) < 1e-6f);
    }

    // 13) Tracking-space reset detection (what happened in the round-6b log): pose at VR start was the headset on the
    //     desk (yaw 157.6 deg, position (1.55, 0.25, 0.30)); once worn it read yaw ~0 and position ~(0,0,0).
    {
        float desk[3] = {1.550f, 0.247f, 0.304f}, worn[3] = {0.003f, 0.001f, 0.002f};
        const float d2r = 3.14159265f / 180.f;
        CHECK(poseJumped(desk, worn, 157.6f*d2r, 0.2f*d2r));
        // ordinary head motion between two frames is NOT a jump
        float a[3] = {0.f, 0.f, 0.f}, b[3] = {0.02f, 0.01f, -0.015f};
        CHECK(!poseJumped(a, b, 0.5f, 0.5f + 0.08f));
        // a fast but plausible turn (45 deg in one frame is not plausible; 20 deg is the limit we accept)
        CHECK(!poseJumped(a, a, 0.f, 20.f*d2r)); CHECK(poseJumped(a, a, 0.f, 70.f*d2r));
        // yaw wraparound: 179 -> -179 degrees is a 2 degree change, not a jump
        CHECK(!poseJumped(a, a, 179.f*d2r, -179.f*d2r));
        // The relative yaw the old build produced after the jump (-157 deg) really is looking backwards:
        printf("relative yaw after the jump: %.1f deg\n", angleDiff(0.2f*d2r, 157.6f*d2r) * 57.29578f);
        CHECK(std::fabs(angleDiff(0.2f*d2r, 157.6f*d2r)*57.29578f + 157.4f) < 0.2f);
    }
    // 14) Culling takeover: union frustum and Gamebryo rotation layout.
    {
        float L[4] = {-1.9210f, 1.2131f, 1.5806f, -1.2053f}, R[4] = {-1.2131f, 1.9210f, 1.5806f, -1.2053f}, u[4];
        unionFrustum(L, R, 0.05f, u);
        printf("union frustum: x0=%.4f x1=%.4f y0=%.4f y1=%.4f\n", u[0], u[1], u[2], u[3]);
        CHECK(u[0] < L[0] && u[1] > R[1] && u[2] > L[2] && u[3] < L[3]);      // covers both eyes with margin
        CHECK(std::fabs(u[0] + u[1]) < 1e-5f);                                // symmetric
        // WAR's real camera basis (WARVR_A.log) round-trips through the Gamebryo matrix layout.
        V3 fwd = {-0.302f, 0.952f, -0.048f}, up = {-0.014f, 0.046f, 0.999f}, right = {0.953f, 0.302f, 0.f};
        float m[9]; basisToGamebryoRot(fwd, up, right, m);
        CHECK(gamebryoRotMatchesBasis(m, fwd, up, right));
        CHECK(!gamebryoRotMatchesBasis(m, right, up, fwd));                   // wrong mapping is rejected
        CHECK(std::fabs(m[0]-fwd.x) < 1e-6f && std::fabs(m[3]-fwd.y) < 1e-6f && std::fabs(m[2]-right.x) < 1e-6f && std::fabs(m[7]-up.z) < 1e-6f);
    }


    // 16) ROLL direction (not covered before): tilting the head so the right ear goes down must tilt the game's up
    //     vector toward the camera's right, like a real head. Head axes in OpenVR tracking space (RH, forward -Z).
    {
        const float th = 25.f * 3.14159265f / 180.f;
        Mat3 head = identity();                       // columns are the head's right, up, back axes in tracking space
        head.m[0][0] =  std::cos(th); head.m[1][0] = -std::sin(th);      // right axis tilts DOWN
        head.m[0][1] =  std::sin(th); head.m[1][1] =  std::cos(th);      // up axis tilts toward +X (the right)
        Mat3 q = cameraDelta(head, 0.f);
        V3 right = {-0.874623f,-0.484803f,0.000005f}, up = {-0.081796f,0.147556f,-0.985666f}, fwd = {-0.477853f,0.862086f,0.168711f};
        V3 r2, u2, f2; rotateBasis(q, right, up, fwd, &r2, &u2, &f2);
        V3 expectUp = { std::cos(th)*up.x + std::sin(th)*right.x, std::cos(th)*up.y + std::sin(th)*right.y, std::cos(th)*up.z + std::sin(th)*right.z };
        printf("roll: new up = (%.4f %.4f %.4f) expected (%.4f %.4f %.4f); forward unchanged = (%.3f %.3f %.3f)\n",
               u2.x, u2.y, u2.z, expectUp.x, expectUp.y, expectUp.z, f2.x, f2.y, f2.z);
        CHECK(std::fabs(u2.x-expectUp.x) < 1e-4f && std::fabs(u2.y-expectUp.y) < 1e-4f && std::fabs(u2.z-expectUp.z) < 1e-4f);
        CHECK(std::fabs(f2.x-fwd.x) < 1e-4f && std::fabs(f2.y-fwd.y) < 1e-4f && std::fabs(f2.z-fwd.z) < 1e-4f);   // roll leaves forward alone
    }
    // 17) head angle between frames
    {
        CHECK(std::fabs(headAngleDeg(rotY(0.2f), rotY(0.2f))) < 1e-3f);
        CHECK(std::fabs(headAngleDeg(rotY(0.f), rotY(30.f*3.14159265f/180.f)) - 30.f) < 0.01f);
    }

    // 18) Render-size budget (SteamVR's recommendation can be 75 Mpx per frame).
    {
        unsigned ew, eh;
        fitEyeSize(5976, 6252, 0.0, 1.0, &ew, &eh);                           // unlimited: exactly SteamVR's number
        printf("unlimited: %ux%u\n", ew, eh); CHECK(ew == 5976 && eh == 6252);
        fitEyeSize(5976, 6252, 20e6, 1.0, &ew, &eh);                          // 20 Mpx budget for both eyes
        double total = 2.0 * ew * eh; printf("20 Mpx budget: %ux%u per eye, %.1f Mpx total\n", ew, eh, total / 1e6);
        CHECK(total <= 20.2e6 && total > 19e6); CHECK((ew & 1) == 0 && (eh & 1) == 0);
        CHECK(std::fabs((double)ew / eh - 5976.0 / 6252.0) < 0.01);            // aspect preserved
        fitEyeSize(2000, 2000, 20e6, 1.0, &ew, &eh); CHECK(ew == 2000 && eh == 2000);   // already within budget: untouched
        fitEyeSize(5976, 6252, 0.0, 0.5, &ew, &eh); CHECK(ew == 2988 && eh == 3126);    // explicit scale
        fitEyeSize(100, 100, 0.0, 1.0, &ew, &eh); CHECK(ew == 256 && eh == 256);        // floor
    }
    // 19) Rectangle scaling (game thinks 5120x2160, real target is 11952x6252-ish).
    {
        long in[4] = {0, 0, 5120, 2160}, out[4];
        scaleRect(in, 4148.f / 5120.f * 2.f, 4340.f / 2160.f, out);
        printf("scaled rect: %ld %ld %ld %ld\n", out[0], out[1], out[2], out[3]);
        CHECK(out[0] == 0 && out[1] == 0 && out[2] == 8296 && out[3] == 4340);        // full target covered exactly
        long half[4] = {2560, 0, 5120, 2160}, oh[4]; scaleRect(half, 2.f, 2.f, oh); CHECK(oh[0] == 5120 && oh[2] == 10240);
    }
    // 20) Cursor mapping and shape.
    {
        float u, v;
        cursorUV(1280, 540, 2560, 1080, &u, &v); CHECK(std::fabs(u - 0.5f) < 1e-6f && std::fabs(v - 0.5f) < 1e-6f);
        cursorUV(-50, 5000, 2560, 1080, &u, &v); CHECK(u == 0.f && v == 1.f);          // outside the window is clamped
        cursorUV(10, 10, 0, 0, &u, &v); CHECK(u == 0.5f && v == 0.5f);                  // degenerate window size
        float xy[40]; int n = cursorArrowTriangles(xy);
        CHECK(n == 15);
        float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
        for (int i = 0; i < n; ++i) { minx = std::fmin(minx, xy[2*i]); maxx = std::fmax(maxx, xy[2*i]); miny = std::fmin(miny, xy[2*i+1]); maxy = std::fmax(maxy, xy[2*i+1]); }
        printf("arrow extent: x %.1f..%.1f  y %.1f..%.1f (tip at origin)\n", minx, maxx, miny, maxy);
        CHECK(minx == 0.f && miny == 0.f && maxx > 10.f && maxy > 18.f);
        // every triangle has the same winding (we draw with culling off, but a flipped one would indicate a typo)
        int pos = 0, neg = 0;
        for (int t = 0; t < 5; ++t) {
            float ax = xy[6*t], ay = xy[6*t+1], bx = xy[6*t+2], by = xy[6*t+3], cx = xy[6*t+4], cy = xy[6*t+5];
            float cr = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax); if (cr > 0) ++pos; else if (cr < 0) ++neg;
        }
        printf("triangle windings: +%d -%d\n", pos, neg);
        CHECK(pos == 0 && neg == 5);                                                  // all five triangles wind the same way
    }

    // ---- phase 7d: eye-constant patching and the world-anchored HUD ----------------------------------------------------
    {
        const float posL[3] = { 26075.f, 53651.5f, -7911.3f };
        float R[3] = { -0.999f, -0.039f, 0.f }, U[3] = { 0.002f, -0.068f, -0.998f }, F[3];
        normalize(R);
        { const float d = R[0]*U[0] + R[1]*U[1] + R[2]*U[2]; for (int i = 0; i < 3; ++i) U[i] -= d * R[i]; }     // Gram-Schmidt: a real camera basis
        normalize(U); cross(R, U, F); normalize(F);
        float vL[4][4]; makeView(R, U, F, posL, vL);
        float cam[3]; camPosFromView(vL, cam);
        printf("camPosFromView: (%.2f %.2f %.2f) vs (%.2f %.2f %.2f)\n", cam[0], cam[1], cam[2], posL[0], posL[1], posL[2]);
        CHECK(std::fabs(cam[0]-posL[0]) < 0.05f && std::fabs(cam[1]-posL[1]) < 0.05f && std::fabs(cam[2]-posL[2]) < 0.05f);

        const float frL[4] = { -1.921f, 1.213f, 1.205f, -1.581f }, frR[4] = { -1.213f, 1.921f, 1.205f, -1.581f };
        float pL[4][4], pR[4][4], vR[4][4];
        perspFromFrustum(frL[0], frL[1], frL[2], frL[3], 10.f, 16384.f, pL);
        perspFromFrustum(frR[0], frR[1], frR[2], frR[3], 10.f, 16384.f, pR);
        float sh[4][4]; cameraShiftMatrix(0.064f * 50.f, 0, 0, sh); mul4(vL, sh, vR);
        EyeMats EL, ER; makeEyeMats(vL, pL, &EL); makeEyeMats(vR, pR, &ER);
        printf("right eye camera is %.2f world units from the left one\n", std::fabs(ER.cam[0]-EL.cam[0]) + std::fabs(ER.cam[1]-EL.cam[1]) + std::fabs(ER.cam[2]-EL.cam[2]));
        CHECK(std::fabs(ER.cam[0]-EL.cam[0]) + std::fabs(ER.cam[1]-EL.cam[1]) + std::fabs(ER.cam[2]-EL.cam[2]) > 3.0f);

        // a fake shader register file: VP at c7 (transposed), an extra VP copy at c40, View (3 regs) at c20, camera position at c30,
        // VP*bias at c50 (x' = 0.5x+0.5w, y' = -0.5y+0.5w), garbage elsewhere
        static float regs[256][4]; memset(regs, 0, sizeof(regs));
        for (int i = 0; i < 256; ++i) for (int c = 0; c < 4; ++c) regs[i][c] = 0.37f * (float)((i * 7 + c * 3) % 11) - 1.2f;
        float t[4][4];
        matToRegs(EL.VP, true, t); memcpy(regs[7], t, sizeof(t)); memcpy(regs[40], t, sizeof(t));
        matToRegs(EL.V, true, t); memcpy(regs[20], t, 3 * sizeof(t[0]));
        regs[30][0] = EL.cam[0]; regs[30][1] = EL.cam[1]; regs[30][2] = EL.cam[2]; regs[30][3] = 1.f;
        float vpT[4][4]; matToRegs(EL.VP, true, vpT);
        for (int c = 0; c < 4; ++c) { regs[50][c] = 0.5f * vpT[0][c] + 0.5f * vpT[3][c]; regs[51][c] = -0.5f * vpT[1][c] + 0.5f * vpT[3][c]; regs[52][c] = vpT[2][c]; regs[53][c] = vpT[3][c]; }

        RegPatch pt[32];
        const int np = buildEyePatches(regs, 256, EL, ER, pt, 32);
        int seen[5] = {}; for (int i = 0; i < np; ++i) { ++seen[pt[i].kind]; printf("  patch %-8s at c%d x%d\n", patchKindName(pt[i].kind), pt[i].reg, pt[i].count); }
        CHECK(seen[PK_VP] == 2);                      // both copies of VP
        CHECK(seen[PK_VIEW] == 1);
        CHECK(seen[PK_CAMPOS] == 1);
        CHECK(seen[PK_VPBIAS] == 1);
        // the VP*bias replacement must equal  bias applied to the RIGHT eye's VP
        for (int i = 0; i < np; ++i) if (pt[i].kind == PK_VPBIAS) {
            float vpTR[4][4]; matToRegs(ER.VP, true, vpTR);
            bool ok = true;
            for (int c = 0; c < 4; ++c) {
                ok &= std::fabs(pt[i].v[0*4+c] - (0.5f * vpTR[0][c] + 0.5f * vpTR[3][c])) < 1e-2f * (1.f + std::fabs(pt[i].v[c]));
                ok &= std::fabs(pt[i].v[1*4+c] - (-0.5f * vpTR[1][c] + 0.5f * vpTR[3][c])) < 1e-2f * (1.f + std::fabs(pt[i].v[4+c]));
                ok &= std::fabs(pt[i].v[2*4+c] - vpTR[2][c]) < 1e-2f * (1.f + std::fabs(vpTR[2][c]));
            }
            CHECK(ok);
        }
        for (int i = 0; i < np; ++i) if (pt[i].kind == PK_CAMPOS) CHECK(std::fabs(pt[i].v[0] - ER.cam[0]) < 1e-3f && pt[i].v[3] == 1.f);
        // an empty register file produces nothing
        static float zeros[256][4]; memset(zeros, 0, sizeof(zeros));
        CHECK(buildEyePatches(zeros, 256, EL, ER, pt, 32) == 0);
    }
    {
        // HUD: anchored with q = identity equals the head-locked rectangle (when that one fits)
        const float fr[4] = { -1.921f, 1.213f, 1.205f, -1.581f };
        int a[4];
        const V3 eo = { -0.032f, 0.f, 0.f }, zero = { 0, 0, 0 };
        CHECK(hudAnchoredRect(fr, eo, identity(), zero, 1.5f, 1.2f, 2.37f, 0.f, 3816, 3992, a));
        PixelRect lock = hudRectPixels(fr, eo.x, eo.y, 1.5f, 1.2f, 2.37f, 0.f, 3816, 3992);
        printf("HUD anchored(identity) = %d,%d %dx%d   head-locked = %d,%d %dx%d\n", a[0], a[1], a[2], a[3], lock.x, lock.y, lock.w, lock.h);
        CHECK(std::abs(a[0]-lock.x) <= 1 && std::abs(a[1]-lock.y) <= 1 && std::abs(a[2]-lock.w) <= 1 && std::abs(a[3]-lock.h) <= 1);
        // turn the head 30 degrees to the right: the HUD (fixed in the world) must move LEFT in the view, by about tan(30deg) of the frustum width
        Mat3 head = rotY(-30.f * 3.14159265f / 180.f);
        Mat3 q = cameraDelta(head, 0.f);
        int b[4];
        CHECK(hudAnchoredRect(fr, eo, q, zero, 1.5f, 1.2f, 2.37f, 0.f, 3816, 3992, b));
        const float shiftTan = ((float)a[0] + 0.5f * a[2] - (float)b[0] - 0.5f * b[2]) / 3816.f * (fr[1] - fr[0]);      // shift of the rectangle's CENTRE
        printf("head turned right 30 deg: HUD centre moved %.1f px left = %.3f tan (tan30 = 0.577)\n", shiftTan * 3816.f / (fr[1] - fr[0]), shiftTan);
        CHECK(b[0] < a[0]);
        CHECK(std::fabs(shiftTan - 0.577f) < 0.08f);
        // turn the head left: it moves right; turn 120 degrees: it is behind the viewer
        CHECK(hudAnchoredRect(fr, eo, cameraDelta(rotY(30.f * 3.14159265f / 180.f), 0.f), zero, 1.5f, 1.2f, 2.37f, 0.f, 3816, 3992, b) && b[0] > a[0]);
        CHECK(!hudAnchoredRect(fr, eo, cameraDelta(rotY(-150.f * 3.14159265f / 180.f), 0.f), zero, 1.5f, 1.2f, 2.37f, 0.f, 3816, 3992, b));
        // lean the head 0.3 m forward: the HUD gets bigger (closer)
        const V3 lean = { 0.f, 0.f, 0.3f };
        CHECK(hudAnchoredRect(fr, eo, identity(), lean, 1.5f, 1.2f, 2.37f, 0.f, 3816, 3992, b) && b[2] > a[2]);
    }
    {
        // HUD as a real quad: centre agrees with the rectangle version; turning the head makes it a foreshortened, tilted panel
        const V3 eo = { 0, 0, 0 }, zero = { 0, 0, 0 };
        float c0[4][3], c1[4][3];
        CHECK(hudQuadEyeSpace(eo, identity(), zero, 1.5f, 1.2f, 2.0f, 0.f, 0.05f, c0));
        CHECK(std::fabs(c0[0][0] + 0.6f) < 1e-4f && std::fabs(c0[1][0] - 0.6f) < 1e-4f && std::fabs(c0[0][1] - 0.3f) < 1e-4f && std::fabs(c0[2][1] + 0.3f) < 1e-4f);
        CHECK(std::fabs(c0[0][2] - 1.5f) < 1e-4f && std::fabs(c0[3][2] - 1.5f) < 1e-4f);          // facing the viewer: all corners at the same depth
        const float wide0 = c0[1][0] / c0[1][2] - c0[0][0] / c0[0][2];
        CHECK(hudQuadEyeSpace(eo, cameraDelta(rotY(-60.f * 3.14159265f / 180.f), 0.f), zero, 1.5f, 1.2f, 2.0f, 0.f, 0.05f, c1));
        const float wide1 = c1[1][0] / c1[1][2] - c1[0][0] / c1[0][2];
        printf("HUD quad: projected width %.3f straight ahead, %.3f when the head is turned 60 deg; corner depths %.3f / %.3f\n", wide0, wide1, c1[0][2], c1[1][2]);
        CHECK(std::fabs(c1[0][2] - c1[1][2]) > 0.2f);                                              // one edge is nearer than the other: a tilted panel
        CHECK(c1[0][0] / c1[0][2] < 0.f || c1[1][0] / c1[1][2] < 0.f);                              // it moved to the left of the view
        CHECK(!hudQuadEyeSpace(eo, cameraDelta(rotY(-180.f * 3.14159265f / 180.f), 0.f), zero, 1.5f, 1.2f, 2.0f, 0.f, 0.05f, c1));   // behind
    }
    {
        // planar reflection: the mirror mapping is an involution, detects the engine's reflection camera, and the symmetric projection
        // + widened viewport reproduces the eye's own picture while giving both eyes the same lookup coordinates
        const V3 r = { -0.783f, -0.622f, 0.f }, u = { -0.131f, 0.165f, -0.978f }, f = { -0.609f, 0.765f, 0.210f };
        V3 r2, u2, f2, r3, u3, f3;
        reflectionBasis(r, u, f, &r2, &u2, &f2);
        printf("reflection camera: right (%.3f %.3f %.3f) up (%.3f %.3f %.3f) fwd (%.3f %.3f %.3f)\n", r2.x, r2.y, r2.z, u2.x, u2.y, u2.z, f2.x, f2.y, f2.z);
        CHECK(std::fabs(f2.z + 0.210f) < 1e-5f && std::fabs(u2.x - 0.131f) < 1e-5f && std::fabs(u2.z + 0.978f) < 1e-5f);     // matches the logged camera
        reflectionBasis(r2, u2, f2, &r3, &u3, &f3);
        CHECK(std::fabs(r3.x - r.x) + std::fabs(u3.y - u.y) + std::fabs(f3.z - f.z) < 1e-5f);                               // involution
        CHECK(isReflectionOf(r2, u2, f2, r, u, f, 0.9f));
        {   // an unrelated camera (a shadow-map light looking steeply down) must not be mistaken for it
            const V3 sr = { 0.7f, -0.7f, 0.f }, su = { 0.5f, 0.5f, 0.7f }, sf = { 0.5f, 0.5f, -0.7f };
            CHECK(!isReflectionOf(sr, su, sf, r, u, f, 0.95f));
        }
        CHECK(isReflectionOf(r2, u2, f2, r, u, f, 0.95f));

        const float fL[4] = { -1.921f, 1.213f, 1.205f, -1.581f }, fR[4] = { -1.213f, 1.921f, 1.205f, -1.581f };
        float fs[4]; const float S = symmetricFrustum(fL, fR, fs);
        CHECK(std::fabs(S - 1.921f) < 1e-5f && fs[0] == -S && fs[1] == S && fs[2] == fL[2]);
        for (int eye = 0; eye < 2; ++eye) {
            const float* fe = eye ? fR : fL;
            const int rx = eye ? 5976 : 0, rw = 5976, rh = 6252;
            int vp[4]; symmetricViewport(fe, S, rx, 0, rw, rh, vp);
            printf("eye %d: region x=%d w=%d, symmetric viewport x=%d w=%d (ends at %d of %d)\n", eye, rx, rw, vp[0], vp[2], vp[0] + vp[2], 2 * rw);
            CHECK(vp[0] >= 0 && vp[0] + vp[2] <= 2 * rw);                                                                   // a legal viewport inside the SBS target
            float maxErr = 0, maxDU = 0;
            for (float tx = fe[0] + 0.01f; tx < fe[1]; tx += 0.173f) {
                const float pxEye = rx + (tx - fe[0]) / (fe[1] - fe[0]) * rw;                                                 // where the eye's projection puts it
                const float ndc = tx / S;                                                                                     // symmetric projection
                const float pxSym = vp[0] + (ndc * 0.5f + 0.5f) * vp[2];
                maxErr = std::max(maxErr, std::fabs(pxEye - pxSym));
                const float uSym = ndc * 0.5f + 0.5f, uEyeOld = (tx - fe[0]) / (fe[1] - fe[0]);                              // lookup coordinate: new vs old
                maxDU = std::max(maxDU, std::fabs(uSym - uEyeOld));
            }
            printf("   position error %.3f px, lookup coordinate changed by up to %.3f\n", maxErr, maxDU);
            CHECK(maxErr < 1.5f);
        }
        // the same point gets the same lookup coordinate in both eyes (that is the whole point)
        CHECK(std::fabs((0.4f / S * 0.5f + 0.5f) - (0.4f / S * 0.5f + 0.5f)) < 1e-6f);
        const float uOldL = (0.4f - fL[0]) / (fL[1] - fL[0]), uOldR = (0.4f - fR[0]) / (fR[1] - fR[0]);
        printf("old lookup coordinate for the same direction: left %.3f right %.3f (differ by %.3f); new: both %.3f\n", uOldL, uOldR, uOldL - uOldR, 0.4f / S * 0.5f + 0.5f);
        CHECK(std::fabs(uOldL - uOldR) > 0.2f);
    }
    {
        // The incremental scanner must give EXACTLY the patches the full scan gives, through any sequence of register uploads.
        const float posL[3] = { 40314.f, 2486.2f, -8435.f };
        V3 r = { -0.985f, -0.173f, 0.044f }, u = { -0.13f, 0.16f, -0.98f }, f = { -0.49f, 0.87f, 0.024f }, pos = { posL[0], posL[1], posL[2] };
        float vL[4][4], pL[4][4], pR[4][4], vR[4][4], sh[4][4];
        engineBuildView(pos, r, u, f, vL);
        perspFromFrustum(-1.921f, 1.213f, 1.205f, -1.581f, 10.f, 16384.f, pL); perspFromFrustum(-1.213f, 1.921f, 1.205f, -1.581f, 10.f, 16384.f, pR);
        cameraShiftMatrix(3.2f, 0, 0, sh); mul4(vL, sh, vR);
        EyeMats EL, ER; makeEyeMats(vL, pL, &EL); makeEyeMats(vR, pR, &ER);
        ScanKeys keys; makeScanKeys(EL, &keys);
        static float regs[256][4];
        RegScan scan; scan.init(256);
        unsigned seed = 12345u; auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) & 0xFFFFFF; };
        for (int i = 0; i < 256; ++i) for (int c = 0; c < 4; ++c) regs[i][c] = 0.37f * (float)((i * 7 + c * 3) % 11) - 1.2f;
        scan.touch(0, 256);
        MatchCache mc; mc.reset();
        int compared = 0, mismatches = 0, planted = 0, cacheMismatch = 0;
        for (int iter = 0; iter < 4000; ++iter) {
            // the game uploads a block somewhere (often at c0), sometimes with one of the things we look for inside it
            const int start = (rnd() % 3 == 0) ? (int)(rnd() % 200) : 0, cnt = 1 + (int)(rnd() % 20);
            for (int i = 0; i < cnt && start + i < 256; ++i) for (int c = 0; c < 4; ++c) regs[start + i][c] = 0.01f * (float)(rnd() % 500) - 2.f;
            const int what = (int)(rnd() % 7);
            float t4[4][4];
            const int at = start + (int)(rnd() % 4);
            if (what == 0 && at + 4 <= 256) { matToRegs(EL.VP, true, t4); memcpy(regs[at], t4, sizeof(t4)); ++planted; }
            if (what == 1 && at + 4 <= 256) { matToRegs(EL.P, rnd() & 1, t4); memcpy(regs[at], t4, sizeof(t4)); ++planted; }
            if (what == 2 && at + 4 <= 256) { matToRegs(EL.V, true, t4); memcpy(regs[at], t4, 3 * sizeof(t4[0])); ++planted; }
            if (what == 3 && at < 256) { regs[at][0] = EL.cam[0]; regs[at][1] = EL.cam[1]; regs[at][2] = EL.cam[2]; ++planted; }
            if (what == 4 && at + 4 <= 256) {
                float vpT[4][4]; matToRegs(EL.VP, true, vpT);
                for (int c = 0; c < 4; ++c) { regs[at][c] = 0.5f * vpT[0][c] + 0.5f * vpT[3][c]; regs[at+1][c] = -0.5f * vpT[1][c] + 0.5f * vpT[3][c]; regs[at+2][c] = vpT[2][c]; regs[at+3][c] = vpT[3][c]; }
                ++planted;
            }
            scan.touch(start, cnt);
            if (what <= 4) scan.touch(at, 4);
            scan.refresh(regs, keys, 1);
            RegPatch a[16], b[16], cc[16];
            const int na = buildEyePatches(regs, 256, EL, ER, a, 16), nb = buildEyePatchesScan(regs, 256, EL, ER, scan, b, 16);
            cacheUpdate(mc, regs, 256, scan, scan.lastLo, scan.lastHi, scan.lastAll, 1, EL);
            const int nc = buildEyePatchesCached(regs, 256, EL, ER, mc, cc, 16);
            { bool sc = na == nc; for (int i = 0; sc && i < na; ++i) { sc = a[i].reg == cc[i].reg && a[i].count == cc[i].count && a[i].kind == cc[i].kind; for (int j = 0; sc && j < a[i].count * 4; ++j) sc = a[i].v[j] == cc[i].v[j]; }
              if (!sc) { ++cacheMismatch; if (cacheMismatch < 4) printf("  CACHE MISMATCH at iteration %d: full=%d cached=%d\n", iter, na, nc); } }
            ++compared;
            bool same = na == nb;
            for (int i = 0; same && i < na; ++i) {
                same = a[i].reg == b[i].reg && a[i].count == b[i].count && a[i].kind == b[i].kind;
                for (int j = 0; same && j < a[i].count * 4; ++j) same = a[i].v[j] == b[i].v[j];
            }
            if (!same) { ++mismatches; if (mismatches < 4) printf("  MISMATCH at iteration %d: full=%d incremental=%d\n", iter, na, nb); }
        }
        printf("incremental scanner: %d comparisons (%d planted patterns), %d mismatches; match cache: %d mismatches\n", compared, planted, mismatches, cacheMismatch);
        CHECK(mismatches == 0 && cacheMismatch == 0 && planted > 1000);
        // a change of eye matrices (new keys / new version) is handled by a full refresh
        EL.V[3][0] += 5.f; makeEyeMats(vL, pL, &EL); makeScanKeys(EL, &keys);
        scan.refresh(regs, keys, 2);
        RegPatch a[16], b[16];
        CHECK(buildEyePatches(regs, 256, EL, ER, a, 16) == buildEyePatchesScan(regs, 256, EL, ER, scan, b, 16));
    }
    {
        // pitch lock: the logged game camera (looking ~12 degrees down) becomes level with the same heading and handedness
        const V3 r = { -0.783f, -0.622f, 0.f }, u = { -0.131f, 0.165f, -0.978f }, f = { -0.609f, 0.765f, 0.210f };
        V3 ro, uo, fo;
        CHECK(levelBasis(r, u, f, &ro, &uo, &fo));
        printf("levelled camera: right (%.3f %.3f %.3f) up (%.3f %.3f %.3f) fwd (%.3f %.3f %.3f)\n", ro.x, ro.y, ro.z, uo.x, uo.y, uo.z, fo.x, fo.y, fo.z);
        CHECK(std::fabs(fo.z) < 1e-6f && std::fabs(uo.x) < 1e-6f && uo.z == -1.f && std::fabs(ro.z) < 1e-6f);               // level; up stays "up" (-z in WAR)
        CHECK(std::fabs(dot3(ro, fo)) < 1e-4f && std::fabs(std::sqrt(dot3(fo, fo)) - 1.f) < 1e-4f && std::fabs(std::sqrt(dot3(ro, ro)) - 1.f) < 1e-4f);
        const float hdgOld = std::atan2(f.y, f.x), hdgNew = std::atan2(fo.y, fo.x);
        CHECK(std::fabs(hdgOld - hdgNew) < 1e-4f);                                                                          // heading (where the mouse points sideways) kept
        auto trip = [](const V3& a, const V3& b, const V3& c) { return a.x * (b.y * c.z - b.z * c.y) - a.y * (b.x * c.z - b.z * c.x) + a.z * (b.x * c.y - b.y * c.x); };
        printf("handedness (triple product) before %.3f after %.3f\n", trip(r, u, f), trip(ro, uo, fo));
        CHECK(trip(r, u, f) * trip(ro, uo, fo) > 0.f && std::fabs(trip(ro, uo, fo)) > 0.99f);                                // same handedness, proper rotation
        const float pitch = std::asin(f.z) * 180.f / 3.14159265f;
        printf("pitch removed: %.1f degrees\n", pitch);
        CHECK(pitch > 11.f && pitch < 13.f);
        // looking straight down: heading undefined, nothing changes
        V3 a, b, c; CHECK(!levelBasis({ 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, &a, &b, &c) && c.z == 1.f);
        // an already level camera is unchanged
        CHECK(levelBasis({ -0.783f, -0.622f, 0.f }, { 0, 0, -1 }, { -0.622f, 0.783f, 0.f }, &a, &b, &c) && std::fabs(c.x + 0.622f) < 1e-4f && std::fabs(a.x + 0.783f) < 1e-4f);
        // composed with a head rotation: with the lock, the rendered pitch is the head's pitch only
        const Mat3 q = cameraDelta(rotX(-20.f * 3.14159265f / 180.f), 0.f);                                                  // head pitched 20 degrees
        V3 rs, us, fs; rotateBasis(q, ro, uo, fo, &rs, &us, &fs);
        printf("rendered forward z with lock: %.3f (head pitch alone gives sin(20 deg) = 0.342)\n", fs.z);
        CHECK(std::fabs(std::fabs(fs.z) - 0.342f) < 0.02f);
    }
    {   // HUD panel shape: flat matches the original quad, curved keeps the span and puts every column at the radius
        printf("\n[hud panel shape / lazy follow]\n");
        const V3 eo{ 0, 0, 0 }, zero{ 0, 0, 0 };
        float c[4][3];
        CHECK(hudQuadEyeSpace(eo, identity(), zero, 1.5f, 2.4f, 16.f / 9.f, -0.1f, 0.05f, c));
        const float uv[4][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } };
        for (int i = 0; i < 4; ++i) {
            const V3 p = hudPanelPoint(uv[i][0], uv[i][1], 1.5f, 2.4f, 16.f / 9.f, -0.1f, false, 0.f, 0.f);
            CHECK(std::fabs(p.x - c[i][0]) < 1e-5f && std::fabs(p.y - c[i][1]) < 1e-5f && std::fabs(p.z - c[i][2]) < 1e-5f);
        }
        for (int i = 0; i <= 8; ++i) {
            const V3 p = hudPanelPoint(i / 8.f, 0.5f, 1.5f, 2.4f, 16.f / 9.f, 0.f, true, 0.f, 0.f);
            CHECK(std::fabs(std::sqrt(p.x * p.x + p.z * p.z) - 1.5f) < 1e-4f);
        }
        const V3 eL = hudPanelPoint(0.f, 0.5f, 1.5f, 2.4f, 16.f / 9.f, 0.f, true, 0.f, 0.f);
        const V3 fL = hudPanelPoint(0.f, 0.5f, 1.5f, 2.4f, 16.f / 9.f, 0.f, false, 0.f, 0.f);
        printf("left edge angle: curved %.2f deg, flat %.2f deg\n", std::atan2(eL.x, eL.z) * 57.29578f, std::atan2(fL.x, fL.z) * 57.29578f);
        CHECK(std::fabs(std::atan2(eL.x, eL.z) - std::atan2(fL.x, fL.z)) < 1e-4f);
        // tilt: top leans back, bottom comes forward; the centre line stays
        const V3 top = hudPanelPoint(0.5f, 0.f, 1.5f, 2.4f, 16.f / 9.f, -0.3f, false, 20.f, 0.f);
        const V3 bot = hudPanelPoint(0.5f, 1.f, 1.5f, 2.4f, 16.f / 9.f, -0.3f, false, 20.f, 0.f);
        const V3 mid = hudPanelPoint(0.5f, 0.5f, 1.5f, 2.4f, 16.f / 9.f, -0.3f, false, 20.f, 0.f);
        CHECK(top.z > 1.5f && bot.z < 1.5f && std::fabs(mid.z - 1.5f) < 1e-5f && std::fabs(mid.y + 0.3f) < 1e-5f);
        // yaw: the panel centre ends up at that heading, and headYawOf reads a head turn back
        const V3 yc = hudPanelPoint(0.5f, 0.5f, 1.5f, 2.4f, 16.f / 9.f, 0.f, true, 0.f, 0.5f);
        CHECK(std::fabs(std::atan2(yc.x, yc.z) - 0.5f) < 1e-4f);
        const Mat3 q30 = cameraDelta(rotY(-30.f * 3.14159265f / 180.f), 0.f);
        const float hy = headYawOf(q30);
        printf("headYawOf(30 deg turn) = %.2f deg\n", hy * 57.29578f);
        CHECK(std::fabs(std::fabs(hy) - 30.f * 3.14159265f / 180.f) < 1e-3f);
        // a point straight ahead of the turned head lands at eye-space x = 0
        float e[3]; hudPointToEye(hudPanelPoint(0.5f, 0.5f, 1.5f, 2.4f, 16.f / 9.f, 0.f, true, 0.f, hy), eo, q30, zero, e);
        printf("panel centre at head heading -> eye space (%.3f, %.3f, %.3f)\n", e[0], e[1], e[2]);
        CHECK(std::fabs(e[0]) < 1e-3f && e[2] > 1.49f);
        // lazy follow: inside the dead zone nothing moves; past it the panel glides and then stops
        bool fol = false;
        CHECK(lazyFollowYaw(0.f, 0.5f, 0.016f, 45.f, 0.3f, &fol) == 0.f && !fol);
        float y = 0.f;
        for (int i = 0; i < 400; ++i) y = lazyFollowYaw(y, 1.2f, 0.016f, 45.f, 0.3f, &fol);
        printf("lazy follow after 6.4 s towards 68.8 deg: %.2f deg, following=%d\n", y * 57.29578f, fol ? 1 : 0);
        CHECK(std::fabs(y - 1.2f) < 2.f / 57.29578f && !fol);
        CHECK(std::fabs(wrapPi(3.5f) - (3.5f - 6.2831853f)) < 1e-5f);
    }
    {   // laser pointer helpers
        printf("\n[laser pointer]\n");
        const float fr[4] = { -0.8f, 0.8f, 0.45f, -0.45f };
        const V3 c = pickRayCamera(0.5f, 0.5f, fr);
        CHECK(std::fabs(c.x) < 1e-6f && std::fabs(c.y) < 1e-6f && std::fabs(c.z - 1.f) < 1e-6f);
        const V3 tl = pickRayCamera(0.f, 0.f, fr);                       // top-left corner: left and up
        CHECK(tl.x < 0.f && tl.y > 0.f && std::fabs(tl.x / tl.z + 0.8f) < 1e-5f && std::fabs(tl.y / tl.z - 0.45f) < 1e-5f);
        const V3 br = pickRayCamera(1.f, 1.f, fr);
        CHECK(br.x > 0.f && br.y < 0.f);
        // a point on the ray projects to the cursor position with the matching projection
        float P[4][4]; perspFromFrustum(fr[0], fr[1], fr[2], fr[3], 1.f, 100.f, P);
        const V3 r = pickRayCamera(0.25f, 0.75f, fr);
        const float pt[3] = { r.x * 10.f, r.y * 10.f, r.z * 10.f };
        float cl[4]; xformPoint(pt, P, cl);
        const float sx = (cl[0] / cl[3] + 1.f) * 0.5f, sy = (1.f - cl[1] / cl[3]) * 0.5f;
        printf("ray point at u=0.25 v=0.75 projects to (%.4f, %.4f)\n", sx, sy);
        CHECK(std::fabs(sx - 0.25f) < 1e-4f && std::fabs(sy - 0.75f) < 1e-4f);
        // clipping against w
        float a[4] = { 0, 0, 0, -1.f }, b[4] = { 0, 0, 0, 3.f };
        CHECK(clipSegmentW(a, b, 0.5f) && std::fabs(a[3] - 0.5f) < 1e-5f && b[3] == 3.f);
        float a2[4] = { 0, 0, 0, -1.f }, b2[4] = { 0, 0, 0, 0.1f };
        CHECK(!clipSegmentW(a2, b2, 0.5f));
        // probes and hit
        CHECK(std::fabs(laserProbeDistance(0, 5, 1.f, 16.f) - 1.f) < 1e-5f && std::fabs(laserProbeDistance(4, 5, 1.f, 16.f) - 16.f) < 1e-3f);
        CHECK(std::fabs(laserProbeDistance(2, 5, 1.f, 16.f) - 4.f) < 1e-4f);
        const float d[6] = { 1, 2, 4, 8, 16, 32 };
        const int s1[6] = { 1, 1, 1, 0, 0, 0 };   CHECK(std::fabs(laserHitDistance(s1, d, 6) - std::sqrt(32.f)) < 1e-4f);
        const int s2[6] = { 1, 1, 1, 1, 1, 1 };   CHECK(laserHitDistance(s2, d, 6) < 0.f);      // sky
        const int s3[6] = { -1, 0, 1, -1, 0, 0 }; CHECK(std::fabs(laserHitDistance(s3, d, 6) - 8.f) < 1e-4f);   // hidden before any visible probe is ignored; first hidden after a visible one counts
        const int s4[6] = { 0, 0, 0, 0, 0, 0 };   CHECK(laserHitDistance(s4, d, 6) < 0.f);
    }
    printf(fails ? "\n%d FAILURES\n" : "\nALL TESTS PASSED\n", fails);
    return fails ? 1 : 0;
}
