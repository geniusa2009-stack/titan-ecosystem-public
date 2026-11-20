// TITAN <-> Unreal conversion tests, run without Unreal. Original TITAN code.
// Compiles the bridge's own headers (unreal/Plugins/TitanStudioBridge/.../Public) against TITAN's math,
// so the plugin and the editor agree on transforms and on the shared frame layout.
#include "TitanCoordinates.h"
#include "TitanFrameChannel.h"
#include "titan/math.h"
#include "titan_test.h"

#include <cstddef>
#include <random>

using namespace TitanCoords;

namespace {
V3 mul(const M3& m, const V3& v) { return m * v; }
double maxDiff(const V3& a, const V3& b) {
    return std::max({std::fabs(a.X - b.X), std::fabs(a.Y - b.Y), std::fabs(a.Z - b.Z)});
}
}  // namespace

TEST("Euler matrix matches titan::Transform::matrix (R = Ry*Rx*Rz)") {
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> ang(-180.f, 180.f);
    for (int i = 0; i < 500; ++i) {
        titan::Transform t;
        t.rotation = {ang(rng), ang(rng), ang(rng)};
        const titan::Mat4 m = t.matrix();
        const M3 r = TitanEulerToMatrix(t.rotation.x, t.rotation.y, t.rotation.z);
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 3; ++col) CHECK_NEAR(r.M[row][col], m.at(row, col), 2e-6);
    }
}

TEST("Euler -> matrix -> Euler reproduces the rotation") {
    std::mt19937 rng(11);
    std::uniform_real_distribution<double> ang(-179.0, 179.0), pitch(-89.0, 89.0);
    for (int i = 0; i < 2000; ++i) {
        const V3 e{pitch(rng), ang(rng), ang(rng)};
        const M3 a = TitanEulerToMatrix(e.X, e.Y, e.Z);
        V3 back;
        TitanMatrixToEuler(a, back.X, back.Y, back.Z);
        const M3 b = TitanEulerToMatrix(back.X, back.Y, back.Z);
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) CHECK_NEAR(a.M[r][c], b.M[r][c], 1e-9);
    }
    // Gimbal lock (X = +-90) still yields the same matrix.
    for (double x : {90.0, -90.0}) {
        const M3 a = TitanEulerToMatrix(x, 30.0, 40.0);
        V3 back;
        TitanMatrixToEuler(a, back.X, back.Y, back.Z);
        const M3 b = TitanEulerToMatrix(back.X, back.Y, back.Z);
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) CHECK_NEAR(a.M[r][c], b.M[r][c], 1e-7);
    }
}

TEST("Unreal rotation and TITAN rotation move the same physical vector") {
    // For any UE rotation R_ue and UE vector v:  map(R_ue v) == R_titan map(v).
    std::mt19937 rng(3);
    std::uniform_real_distribution<double> ang(-180.0, 180.0), pitch(-89.0, 89.0), comp(-5.0, 5.0);
    for (int i = 0; i < 2000; ++i) {
        const double p = pitch(rng), y = ang(rng), r = ang(rng);
        const V3 v{comp(rng), comp(rng), comp(rng)};
        const V3 lhs = UeToTitanDirection(mul(UeRotatorToMatrix(p, y, r), v));
        const V3 e = UeRotatorToTitanEuler(p, y, r);
        const V3 rhs = mul(TitanEulerToMatrix(e.X, e.Y, e.Z), UeToTitanDirection(v));
        CHECK(maxDiff(lhs, rhs) < 1e-9);
        // and back to a UE rotator describing the same orientation
        double p2 = 0, y2 = 0, r2 = 0;
        TitanEulerToUeRotator(e, p2, y2, r2);
        const V3 again = mul(UeRotatorToMatrix(p2, y2, r2), v);
        CHECK(maxDiff(again, mul(UeRotatorToMatrix(p, y, r), v)) < 1e-9);
    }
}

TEST("Unreal yaw is clockwise about +Z seen from above (left-handed), TITAN yaw is about +Y") {
    // UE yaw +90 turns forward (+X) to right (+Y). In TITAN, forward is -Z and right is +X, which is a
    // -90 degree turn about +Y in a right-handed frame.
    const V3 e = UeRotatorToTitanEuler(0, 90, 0);
    CHECK_NEAR(e.X, 0.0, 1e-9);
    CHECK_NEAR(e.Y, -90.0, 1e-9);
    CHECK_NEAR(e.Z, 0.0, 1e-9);
}

TEST("positions and scale map with the documented axis table") {
    const V3 u = TitanToUePosition({1.5, 2.5, 3.5});
    CHECK_NEAR(u.X, -350.0, 1e-12);
    CHECK_NEAR(u.Y, 150.0, 1e-12);
    CHECK_NEAR(u.Z, 250.0, 1e-12);
    const V3 t = UeToTitanPosition(u);
    CHECK(maxDiff(t, {1.5, 2.5, 3.5}) < 1e-12);
    const V3 s = TitanToUeScale({0.6, 2.0, 0.6});
    CHECK(maxDiff(UeToTitanScale(s), {0.6, 2.0, 0.6}) < 1e-12);
    CHECK_NEAR(s.Z, 2.0, 1e-12);  // TITAN height (Y) is Unreal Z
}

TEST("camera: forward -Z in TITAN looks along +X in Unreal; vertical FOV converts to horizontal") {
    V3 loc;
    double pitch = 0, yaw = 0;
    TitanCameraToUe({0, 1.6, 6}, {0, 0, -1}, loc, pitch, yaw);
    CHECK_NEAR(yaw, 0.0, 1e-9);
    CHECK_NEAR(pitch, 0.0, 1e-9);
    CHECK_NEAR(loc.X, -600.0, 1e-9);
    CHECK_NEAR(loc.Z, 160.0, 1e-9);
    TitanCameraToUe({0, 0, 0}, {0, -1, 0}, loc, pitch, yaw);
    CHECK_NEAR(pitch, -90.0, 1e-9);
    CHECK_NEAR(HorizontalFovDeg(90.0, 1.0), 90.0, 1e-9);
    CHECK(HorizontalFovDeg(50.0, 16.0 / 9.0) > 50.0);
}

TEST("frame channel layout is stable") {
    CHECK(sizeof(TitanFrameSlot) == 160);
    CHECK(sizeof(TitanFrameChannelHeader) <= TITAN_FRAME_HEADER_BYTES);
    CHECK(offsetof(TitanFrameChannelHeader, ring) % 8 == 0);
    CHECK(TITAN_FRAME_HEADER_BYTES % 4096 == 0);
}

TITAN_TEST_MAIN()
