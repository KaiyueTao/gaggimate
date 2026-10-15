#ifndef MUSE_SPRITE_FIT_H
#define MUSE_SPRITE_FIT_H

// Sprite fit + anchoring math for the Muse overlay — muse-1.3.2.
//
// The defect this pins down: in the compact layout the character rendered
// with its whole hood/head cut off, in every state, on the final tagged
// builds of both muse-1.3 and muse-1.3.1 — while the layout math on paper
// "fit". Root cause, in LVGL 8.4 semantics: an lv_img in the default
// LV_IMG_SIZE_MODE_VIRTUAL reports its self size as the *untransformed*
// source size, and the zoomed artwork is drawn around the image pivot,
// wherever the object rect happens to sit. The plugin positioned the img
// with lv_obj_align(LV_ALIGN_BOTTOM_MID) — aligning the oversized source
// rect (e.g. 151x169) inside the 96x96 root. The pivot then lands at
// root y = boxH - h/2 (e.g. -73 + 84 = 11), the drawn sprite spans
// pivot +/- drawnH/2 (e.g. -37..59), and the parent's clip eats the top
// ~37 px — the head. The standby box (168x158) is close to the source
// size, so the anchor error (h - drawnH)/2 is small there and the large
// layout looked fine, which is why this survived two review rounds that
// checked disc geometry but never cropped the rendered sprite.
//
// The fix has two halves, both in this header so the native suite can
// pin them (test_muse_sprite_fit):
//   1. fitZoom(): the zoom is capped so the pose's axis-aligned bounding
//      box (AABB) at the state's worst animation phase — max |angle|,
//      max upward travel (DONE hop -12, BREWING_LATE hop -8, bobs -2),
//      max sideways travel (WARNING shake +/-2), plus the SLEEP
//      breathing zoom headroom (+4) — fits the box with margin.
//   2. pivotTarget(): the img is positioned by its PIVOT, never by
//      aligning the object rect. Per render pass the pivot is placed so
//      the drawn AABB's bottom edge sits at boxH + dy (feet planted,
//      hops rise from there) and its horizontal centre at boxW/2 + dx.
//      Containment then holds at every phase by construction.
//
// Extents use cos = 1.0 and sin from the small-angle approximation, both
// of which *over*estimate the true AABB (LVGL's exact trig has
// cos(2 deg) = 0.99939 < 1), so the rendered sprite always lands inside
// the box computed here, never outside it. All values are plain ints so
// this header stays free of LVGL/Arduino includes.

namespace MuseSpriteFit {

// Slack (px) kept between the worst-phase AABB and the box on top of the
// travel budgets; also absorbs the ceil() slack in the extents below.
constexpr int kFitMarginPx = 2;

// sin(|angle|) * 1000 for an angle in 0.1-degree units, small-angle
// approximation rounded up (sin(x) ~ x for x <= 2 deg; error < 0.01).
// angle 20 (2.0 deg) -> 35; true sin(2 deg) * 1000 = 34.90.
constexpr int sinKFromAngle(int angleDeciDeg) {
    const int a = angleDeciDeg < 0 ? -angleDeciDeg : angleDeciDeg;
    return (a * 17453 + 9999) / 10000;
}

inline int ceilDiv(int num, int den) { return (num + den - 1) / den; }

// Pivot-relative AABB edges of the pose at a zoom, for |sin| = sinK/1000
// (cos = 1000/1000, the conservative overestimate described above).
// (px, py) is the LVGL pivot inside the w x h source. Edges are in drawn
// pixels relative to the pivot; left/top are negative.
struct Extents {
    int left;
    int top;
    int right;
    int bottom;
};

inline Extents extents(int w, int h, int px, int py, int zoom, int sinK) {
    const int dxMax = px > w - px ? px : w - px;
    const int dyMax = py > h - py ? py : h - py;
    Extents e;
    e.right = ceilDiv(((w - px) * 1000 + dyMax * sinK) * zoom, 256000);
    e.left = -ceilDiv((px * 1000 + dyMax * sinK) * zoom, 256000);
    e.bottom = ceilDiv(((h - py) * 1000 + dxMax * sinK) * zoom, 256000);
    e.top = -ceilDiv((py * 1000 + dxMax * sinK) * zoom, 256000);
    return e;
}

// Per-state animation envelope: the worst phase the fit must survive.
// angleMax: max |angle| (0.1-degree units). up: max upward travel (px,
// positive). side: max |sideways travel| (px). breath: extra zoom added
// on top of the fitted zoom at some phases (SLEEP breathing, +4).
struct Envelope {
    int angleMax;
    int up;
    int side;
    int breath;
};

// Zoom (256 = 100%) at which the pose's worst-phase AABB fits the box.
// The breathing headroom is subtracted here; the caller adds the
// phase-dependent breath (0..breath) back on top, so the total never
// exceeds the fitted cap.
inline int fitZoom(int w, int h, int boxW, int boxH, const Envelope &env) {
    const int px = w / 2;
    const int py = h / 2;
    const Extents e = extents(w, h, px, py, 256, sinKFromAngle(env.angleMax));
    const int aabbW = e.right - e.left;
    const int aabbH = e.bottom - e.top;
    const int budgetW = boxW - 2 * env.side - kFitMarginPx;
    const int budgetH = boxH - env.up - kFitMarginPx;
    const int zW = budgetW * 256 / aabbW;
    const int zH = budgetH * 256 / aabbH;
    const int z = (zW < zH ? zW : zH) - env.breath;
    return z > 0 ? z : 1;
}

// Where the pivot must sit (root/box coordinates) at a given phase:
// the drawn AABB's bottom edge lands at boxH + dy and its horizontal
// centre at boxW/2 + dx. (tx, ty) is the pivot position; the caller sets
// the img object pos to (tx - px, ty - py).
inline void pivotTarget(int w, int h, int px, int py, int zoom, int angleDeciDeg, int boxW, int boxH, int dx, int dy,
                        int &tx, int &ty) {
    const Extents e = extents(w, h, px, py, zoom, sinKFromAngle(angleDeciDeg));
    tx = boxW / 2 + dx - (e.left + e.right) / 2;
    ty = boxH + dy - e.bottom;
}

} // namespace MuseSpriteFit

#endif // MUSE_SPRITE_FIT_H
