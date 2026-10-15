// Unit tests: Muse sprite fit + pivot anchoring (MuseSpriteFit.h).
//
// Regression suite for the muse-1.3 / muse-1.3.1 hardware-class defect:
// in the compact layout the character rendered with its whole hood/head
// cut off in every state, although every "fit" check on paper passed.
// The layout work verified that the *box* sits inside the round panel's
// disc, but the sprite inside the box was positioned by aligning the
// lv_img object rect — which in LVGL 8.4 (LV_IMG_SIZE_MODE_VIRTUAL) is
// the untransformed source size (e.g. 151x169) — inside the 96x96 box.
// The zoomed artwork is drawn around the image pivot, so it landed
// shifted up by (h - drawnH)/2 and the parent clip ate the head.
// Standby's box is close to the source size, so the error was invisible
// there and the defect shipped twice.
//
// The policy pinned here (see MuseSpriteFit.h): the zoom is capped so
// the pose's AABB at the worst animation phase (max |angle|, max upward
// travel incl. the DONE hop, WARNING shake, SLEEP breathing headroom)
// fits the box, and the img is anchored by its pivot so the AABB bottom
// sits at boxH + dy and its centre at boxW/2 + dx — containment at every
// phase, by construction. These tests sweep every pose asset x every
// state envelope x both layouts x the phase corners and assert the
// anchored AABB stays inside the box. The rendering itself is pinned by
// the pixel-level crop pass in the simulator (MUSE_1_3_2_FIX_NOTES.md).
//
// Pose dimensions mirror MuseSprites.c (all poses share one scale).
// Envelopes mirror the animation switch in MuseAvatarPlugin.cpp.
//
// Host-side — pio test -e native -f test_muse_sprite_fit.

#include <unity.h>

#include <display/plugins/MuseSpriteFit.h>

namespace {

using MuseSpriteFit::Envelope;
using MuseSpriteFit::Extents;
using MuseSpriteFit::extents;
using MuseSpriteFit::fitZoom;
using MuseSpriteFit::pivotTarget;
using MuseSpriteFit::sinKFromAngle;

struct Pose {
    const char *name;
    int w;
    int h;
};

// MuseSprites.c order: sleep, heating, ready, brewing, steaming,
// celebrate, warning, done.
constexpr Pose kPoses[] = {
    {"sleep", 147, 156}, {"heating", 172, 168}, {"ready", 151, 169}, {"brewing", 143, 169},
    {"steaming", 168, 173}, {"celebrate", 176, 169}, {"warning", 140, 169}, {"done", 134, 169},
};

// Animation envelopes, mirroring MuseAvatarPlugin.cpp's switch:
// angleMax in 0.1-degree units, up = max -dy, side = max |dx|,
// breath = zoom headroom (SLEEP breathing +4).
constexpr Envelope kSleep = {0, 0, 0, 4};
constexpr Envelope kHeating = {12, 2, 0, 0};
constexpr Envelope kReady = {14, 2, 0, 0};
constexpr Envelope kBrewing = {10, 0, 0, 0};
constexpr Envelope kBrewingLate = {16, 8, 0, 0};
constexpr Envelope kSteaming = {20, 2, 0, 0};
constexpr Envelope kDone = {0, 12, 0, 0};
constexpr Envelope kWarning = {0, 0, 2, 0};
constexpr Envelope kEnvelopes[] = {kSleep, kHeating, kReady, kBrewing, kBrewingLate, kSteaming, kDone, kWarning};

// Layouts (MuseAvatarPlugin.cpp): large standby box, compact box.
constexpr int kBoxWl = 168;
constexpr int kBoxHl = 158;
constexpr int kBoxWc = 96;
constexpr int kBoxHc = 96;

// Assert the anchored AABB is inside the box for one phase.
void assertPhaseInside(const Pose &pose, int boxW, int boxH, const Envelope &env, int zoom, int angle, int dx,
                       int dy) {
    const int px = pose.w / 2;
    const int py = pose.h / 2;
    int tx = 0;
    int ty = 0;
    pivotTarget(pose.w, pose.h, px, py, zoom, angle, boxW, boxH, dx, dy, tx, ty);
    const Extents e = extents(pose.w, pose.h, px, py, zoom, sinKFromAngle(angle));
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(0, tx + e.left, pose.name);
    TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(boxW, tx + e.right, pose.name);
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(0, ty + e.top, pose.name);
    TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(boxH, ty + e.bottom, pose.name);
    // Feet planted: the AABB bottom is exactly the requested boxH + dy.
    TEST_ASSERT_EQUAL_INT_MESSAGE(boxH + dy, ty + e.bottom, pose.name);
}

void sweepPoseLayout(const Pose &pose, int boxW, int boxH) {
    for (const Envelope &env : kEnvelopes) {
        const int base = fitZoom(pose.w, pose.h, boxW, boxH, env);
        TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(1, base, pose.name);
        const int angles[] = {-env.angleMax, 0, env.angleMax};
        const int dys[] = {0, -env.up};
        const int dxs[] = {-env.side, 0, env.side};
        const int zooms[] = {base, base + env.breath};
        for (int zoom : zooms)
            for (int angle : angles)
                for (int dy : dys)
                    for (int dx : dxs)
                        assertPhaseInside(pose, boxW, boxH, env, zoom, angle, dx, dy);
    }
}

} // namespace

void test_sinK_small_angle_values() {
    TEST_ASSERT_EQUAL_INT(0, sinKFromAngle(0));
    TEST_ASSERT_EQUAL_INT(18, sinKFromAngle(10));  // 1.0 deg
    TEST_ASSERT_EQUAL_INT(35, sinKFromAngle(20));  // 2.0 deg, true 34.90
    TEST_ASSERT_EQUAL_INT(35, sinKFromAngle(-20)); // sign-independent
}

void test_all_poses_compact_box_contained() {
    for (const Pose &pose : kPoses)
        sweepPoseLayout(pose, kBoxWc, kBoxHc);
}

void test_all_poses_large_box_contained() {
    for (const Pose &pose : kPoses)
        sweepPoseLayout(pose, kBoxWl, kBoxHl);
}

// The incident regression in numbers: the brewing pose at the zoom the
// old fitZoom produced (145) had its drawn AABB top ~37 px above the
// compact box. The new fit must be smaller, and its AABB at rest must
// sit fully inside the box with the feet on the bottom edge.
void test_brewing_pose_compact_regression() {
    const Pose brewing = {"brewing", 143, 169};
    const int z = fitZoom(brewing.w, brewing.h, kBoxWc, kBoxHc, kBrewing);
    TEST_ASSERT_LESS_THAN_INT(145, z);
    const Extents e = extents(brewing.w, brewing.h, brewing.w / 2, brewing.h / 2, z, sinKFromAngle(10));
    TEST_ASSERT_LESS_OR_EQUAL_INT(kBoxHc, e.bottom - e.top);
    TEST_ASSERT_LESS_OR_EQUAL_INT(kBoxWc, e.right - e.left);
}

// Readability floor: the fit must not "solve" containment by shrinking
// the character to a stamp. Every pose keeps a usable size in both
// layouts (worst envelope per pose is what renderOverlay computes).
void test_fit_keeps_character_readable() {
    for (const Pose &pose : kPoses) {
        int worstCompact = 256;
        int worstLarge = 256;
        for (const Envelope &env : kEnvelopes) {
            const int zc = fitZoom(pose.w, pose.h, kBoxWc, kBoxHc, env);
            const int zl = fitZoom(pose.w, pose.h, kBoxWl, kBoxHl, env);
            if (zc < worstCompact)
                worstCompact = zc;
            if (zl < worstLarge)
                worstLarge = zl;
        }
        // >= ~45% scale compact, >= ~82% large, per pose. (The binding
        // combos are celebrate x DONE and steaming x DONE at 118 / 213;
        // the pairs that actually render — pose and state are linked —
        // bottom out at 124 compact (done) and 218 large.)
        TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(115, worstCompact, pose.name);
        TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(210, worstLarge, pose.name);
    }
}

// DONE is the tightest vertical case (12 px hop in a 96 px box): the
// fit must leave the hop headroom, i.e. at dy=-12 the top stays >= 0.
void test_done_hop_headroom() {
    const Pose done = {"done", 134, 169};
    const int z = fitZoom(done.w, done.h, kBoxWc, kBoxHc, kDone);
    int tx = 0;
    int ty = 0;
    pivotTarget(done.w, done.h, done.w / 2, done.h / 2, z, 0, kBoxWc, kBoxHc, 0, -12, tx, ty);
    const Extents e = extents(done.w, done.h, done.w / 2, done.h / 2, z, 0);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, ty + e.top);
    TEST_ASSERT_EQUAL_INT(kBoxHc - 12, ty + e.bottom);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_sinK_small_angle_values);
    RUN_TEST(test_all_poses_compact_box_contained);
    RUN_TEST(test_all_poses_large_box_contained);
    RUN_TEST(test_brewing_pose_compact_regression);
    RUN_TEST(test_fit_keeps_character_readable);
    RUN_TEST(test_done_hop_headroom);
    return UNITY_END();
}
