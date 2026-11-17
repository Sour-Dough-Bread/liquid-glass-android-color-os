// Tinted glass, asked for by the app that owns the blur region.
//
// A blur region (android.view.SurfaceControl's BlurRegion, which
// BackgroundBlurDrawable and Window.setBackgroundBlurRadius produce) carries a
// radius, an alpha, a rect and four corner radii, and nothing else: there is no
// field for a colour. But the corner radii are floats that reach
// drawBlurRegion's SkRRect exactly as the app set them (the layer's transform
// is applied through the canvas, not to the radii), and nobody can see a
// corner move by a fraction of a pixel. So a region asks for a tint by putting
// it in the sub-pixel part of its corner radii:
//
//   corner radius = whole pixels + (2 * q + 1) / 512,   q = 0..255
//
//   top-left q = red, top-right q = green, bottom-right q = blue,
//   bottom-left q = how strongly (0 = untinted, 255 = fully)
//
// Every corner must carry it. The fractions are odd multiples of 1/512, which
// a radius computed the ordinary way (dp times a density such as 2.75 or
// 2.625) never is: those land on multiples of 1/16 or coarser. A float holds a
// radius below 16384 px plus a 1/512 step exactly, so nothing is lost on the
// way. Keep each radius at least a pixel below half the shorter side: a round
// rect whose corners do not fit is scaled down by Skia, and the scaling would
// destroy the fractions.
//
// In Android's own blur (stock mode, or no module at all) the corners are off
// by under a pixel, so the encoding is invisible there. With the module the
// region is drawn as glass in that colour: what it transmits is filtered by
// the colour and the colour itself is laid over it at that strength. That is
// what Quick Settings would use for a tile that is on.
#pragma once

#include <cmath>

namespace glass {

struct RegionTint {
    bool on = false;
    float rgb[3] = {0.0f, 0.0f, 0.0f};
    float strength = 0.0f;
};

// Corner radii in the order top-left, top-right, bottom-right, bottom-left
// (SkRRect's corner order). Each corner's x and y radius must agree.
inline RegionTint decodeRegionTint(const float rx[4], const float ry[4]) {
    RegionTint t;
    float q[4];
    for (int i = 0; i < 4; ++i) {
        if (!(rx[i] > 0.0f) || rx[i] != ry[i] || !(rx[i] < 16384.0f)) return t;
        const double steps = (static_cast<double>(rx[i]) - std::floor(rx[i])) * 512.0;
        const double n = std::round(steps);
        if (std::fabs(steps - n) > 1e-3) return t;
        const long k = static_cast<long>(n);
        if ((k & 1) == 0) return t;
        q[i] = static_cast<float>((k - 1) / 2) / 255.0f;
    }
    t.rgb[0] = q[0];
    t.rgb[1] = q[1];
    t.rgb[2] = q[2];
    t.strength = q[3];
    t.on = t.strength > 0.0f;
    return t;
}

// The other direction, for tests and as the reference an app copies:
// `radius` is the corner radius the app wants (rounded down to whole pixels
// here), `q` the byte for that corner.
inline float encodeRegionTintCorner(float radius, int q) {
    if (q < 0) q = 0;
    if (q > 255) q = 255;
    return std::floor(radius) + static_cast<float>(2 * q + 1) / 512.0f;
}

}  // namespace glass
