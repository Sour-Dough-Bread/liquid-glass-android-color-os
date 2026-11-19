#!/usr/bin/env python3
"""
Generates the SkSL glass shader for SurfaceFlinger's BlurFilter::drawBlurRegion.

Single source of truth. Two consumers:
  * the APK harness      — loads the .sksl from assets into a RuntimeShader
  * libglassengine.so    — reads it from /data/adb/glass/ at hook-install time,
                           so a shader edit is a push + `stop/start surfaceflinger`
                           rather than a rebuild of the .so

This is a port of expo-liquid-glass-view's `GlassShaderSource.kt` (branch
android-port), which is itself an AGSL translation of that project's
`ios/Shaders/LiquidGlass.metal:180-256`, eye-calibrated against real iOS 26
glass (see that repo's docs/android-port/research/04-ios26-edge-evidence.md).
The refraction/dispersion/tone/highlight *maths is unchanged* — this file only
re-plumbs the backdrop inputs. Portions derived from AndroidLiquidGlass
(Apache-2.0, (c) 2025 Kyant); see the upstream NOTICE.

=============================================================================
 THE CONTRACT  (frameworks/native/libs/renderengine/skia/filters/BlurFilter.h)
=============================================================================

    void BlurFilter::drawBlurRegion(SkCanvas* canvas, const SkRRect& effectRegion,
                                    const uint32_t blurRadius, const float blurAlpha,
                                    const SkRect& blurRect, sk_sp<SkImage> blurredImage,
                                    sk_sp<SkImage> input);

Identical on Android 14, 15 and 16 — verified against a real A14 device binary
(symbol recovered from .gnu_debugdata) and against AOSP main's header.

We replace the body. Instead of one `content` child fed by a RenderEffect
chain, there are TWO children, built by the caller:

    blurredContent = blurredImage->makeShader(kClamp, kClamp, linear, &Mblur)
                     Mblur  = getShaderTransform(canvas, blurRect, kInverseInputScale)
                     kInputScale = 0.25f, so this image is QUARTER resolution.

    sharpContent   = input->makeShader(kClamp, kClamp, linear, &Msharp)
                     Msharp = getShaderTransform(canvas, blurRect, 1.0f)
                     Full resolution. Stock AOSP only uses this below
                     kMaxCrossFadeRadius; we use it at every radius.

Because each child carries its own local matrix, BOTH ARE EVALUATED AT THE
SAME COORDINATE. The shader never does the scale math — do not "correct" for
the 0.25 anywhere in here.

Coordinate space: `main(float2 fragCoord)` receives canvas-LOCAL coordinates,
the same space `effectRegion` and `blurRect` are expressed in. Uniform contract:

    offset = -effectRegion.rect().topLeft()   so  pixels = fragCoord + offset
    size   =  effectRegion.rect().size()
    crop   =  blurRect, in that same local space

which is exactly the shape the app-side port uses (there `offset` was (-P,-P)
for the padded RenderNode). Keeping it identical is deliberate: the app-side
renderer stays a diffable reference for this one.

Two notes on what did NOT need to carry over:

  * Padding. The app-side port records a padded RenderNode because it owns its
    surface. We do not own ours — the canvas arrives clipped. That is affordable
    because refraction displaces INWARD (upstream's own note on
    `backdropPaddingPx`: "at every variant default the refraction term is zero"),
    so only `0.5 * |dispersionAmount|` reaches outward. Any effect that must
    draw OUTSIDE the region — the stretch/jiggle — needs the BlurRegion itself
    inflated by the requesting window, and is out of scope here.

  * `crop` is belt-and-braces now, not load-bearing. Upstream needed it because
    an out-of-bounds `content.eval` on a chain input returns transparent black
    (not the clamped edge texel), which painted a black rim. We build both
    children with SkTileMode::kClamp ourselves, so the clamp is already in the
    sampler. It is kept only so this shader stays numerically identical to the
    reference renderer.
"""

from pathlib import Path

# ---------------------------------------------------------------------------- tiers

TAPS = {"LOW": 0, "MEDIUM": 8, "HIGH": 16}

# The dispersion tap count cannot be a uniform: SkSL follows GLSL ES 1.00
# Appendix A, so a `for` bound must be a compile-time constant to be statically
# unrollable. One compiled program per tier, exactly as upstream does.

# ---------------------------------------------------------------------------- uniforms

BASE_UNIFORMS = [
    "size", "offset", "crop", "cornerExtents", "cornerShapes", "unitScale",
    "refractionScale", "refractionAmount", "refractionSwirl", "depthEffect",
    "profilePower", "profileBias",
    "blurMix", "blurFalloff", "lensBlur", "rimFrost",
    "tintColor", "frostColor",
    "highlightIntensity", "highlightDir", "highlightWidth", "highlightFalloff",
    "lightIntensity", "glassOpacity", "saturation", "hdrHeadroom",
    "touchPos", "touchGlow", "touchLens",
    "softenRadius", "softenFast", "deepBand",
    "fillLighten", "fillDarken", "lumaLimit", "toneScale", "toneBias", "filterColor",
]
DISPERSION_UNIFORMS = [
    "dispersionHeight", "dispersionAmount", "dispersionTapSpacing", "dispersionQuadrant",
]
GRAIN_UNIFORMS = ["noiseAmount"]

# ---------------------------------------------------------------------------- fragments

HEADER = """\
// =========================================================================
//  glass_%(tier)s.sksl - GENERATED by shader/gen_glass_sksl.py. Do not edit.
//
//  Drop-in replacement for the body of
//  android::renderengine::skia::BlurFilter::drawBlurRegion.
//
//  Coordinate space: canvas-LOCAL pixels, the space effectRegion/blurRect
//  live in. Every length-valued uniform is in those units.
//
//  Precision policy: there is NO `half` declaration anywhere below. The only
//  two half touchpoints are imposed by the language - main() must return
//  half4, and shader.eval() returns half4 - and both are converted explicitly
//  at the boundary. fp16 overflow was observed on Mali during the app-side
//  port; RenderEngine runs across every vendor's driver, so this matters more
//  here, not less.
//
//  Ported from expo-liquid-glass-view (android-port). Portions derived from
//  AndroidLiquidGlass, Apache-2.0, (c) 2025 Kyant.
// =========================================================================

const float kNormEps = 1e-6;

"""

CHILDREN = """\
// The two backdrops handed to us by drawBlurRegion. Each carries its own local
// matrix (built with getShaderTransform at kInverseInputScale and 1.0
// respectively), so they are evaluated at the SAME coordinate despite
// blurredContent being quarter-resolution. Do not scale coordinates here.
uniform shader blurredContent;   // <- blurredImage, Kawase output, 0.25x
uniform shader sharpContent;     // <- input, full-res backdrop snapshot

"""

UNIFORM_BLOCK = """\
uniform float2 size;             // effectRegion.rect() size, px
uniform float2 offset;           // -rect.topLeft(): fragCoord -> view-local px
uniform float4 crop;             // blurRect in local space (minX,minY,maxX,maxY)
uniform float4 cornerExtents;    // px, packed (BL, BR, TR, TL). From the SkRRect
                                 // radii; corner-cell size E = r for circular.
uniform float4 cornerShapes;     // superellipse exponent per corner, same packing.
                                 // 2 = circular exactly. SkRRect has nowhere to
                                 // carry this, so it is global config, not
                                 // per-region: 2.0 unless you want Apple's
                                 // continuous corner (3.3418).

uniform float  unitScale;        // device px per dp (density). Used ONLY where a
                                 // point-valued constant is hard-coded: the
                                 // dispersion cutoff and the sheen band.

uniform float2 refractionScale;  // px
uniform float  refractionAmount; // px
uniform float  refractionSwirl;  // lean toward the light axis. 0 = iOS/Kyant.
uniform float  depthEffect;
uniform float  profilePower;
uniform float  profileBias;

// How the lens hands over to the compositor's own Kawase blur with depth.
// blurMix   1 = interior fully frosted (default, and what Liquid Glass reads as)
//           0 = clear glass: the sharp backdrop everywhere, bent at the rim
// blurFalloff shapes the handover curve; >1 keeps the lens sharp deeper in.
// Swept on an A16 AVD (docs/VERIFIED.md): the two are cleanly orthogonal --
// blurFalloff moves rim-band contrast 12.9->29.3 while deep-interior contrast
// stays at 4.18 for every value, so it is a rim control that cannot leak
// inward. Calibrated defaults: blurMix 1.0, blurFalloff 2.0.
uniform float  blurMix;
uniform float  blurFalloff;

// How much of the compositor's blur the RIM keeps.
//
// The lens reads the sharp child - that is what gives the edge its detail, and
// it is also why the edge ignores the blur radius the compositor picked for
// this region. Measured on a full-screen region at radius 80: the 52 px
// refraction band carries 9x the fine detail of the blurred content it sits in,
// and stops being sharp exactly at refractionScale (docs/VERIFIED.md).
//
// lensBlur folds the blurred child back into the lens at the SAME displaced
// position, so the rim honours that radius while keeping the refraction
// geometry. It also repairs a second-order defect: at 0 the frost handover
// cross-dissolves a DISPLACED sharp image against an UNDISPLACED blurred one,
// which ghosts. At 1 both sides of that mix are the blurred child and the
// handover becomes a pure geometry blend.
//
// The cost is one extra texture read, taken only inside the rim band and only
// when this is above zero. Chromatic dispersion fades as this rises, because
// dispersion of an already-blurred image is nearly invisible - which is
// physically right, but it is the reason not to simply pin this at 1.
uniform float  lensBlur;

// How frosted the RIM is, against the inside of the pane.
//
// The handover above fades the frost out toward the edge: frostT is blurMix
// deep inside and 0 at the rim. lensBlur was meant to make up for it, and the
// presets pin it to blurMix "so the rim is no sharper than the pane it
// borders". It does not: lensBlur is taken off the see-through view at the rim
// AND inside, and blurMix on top of that only inside, so the see-through view
// keeps (1 - lensBlur) of its strength at the rim against
// (1 - lensBlur)(1 - blurMix) inside. At 0.3 and 0.3 that is 70% against 49%:
// a rim band visibly clearer than the pane, with the colour fringe drawn at
// the strength of the clear part. Measured on the AVD as 1.4 times the
// contrast in the band.
//
// rimFrost 1 mixes the frost in at full blurMix across the band, so the rim
// is as frosted as the inside, and takes that frost from where the lens read
// (the same bend as the see-through view) rather than from straight behind
// the fragment, which would ghost a bent image against an unbent one. 0 is
// the handover as it was, exactly.
uniform float  rimFrost;

uniform float4 tintColor;        // straight (non-premultiplied) RGBA
uniform float4 frostColor;       // rgb = system background, a = frostAmount
uniform float  highlightIntensity;
uniform float2 highlightDir;     // (cos angle, sin angle), precomputed CPU-side
uniform float  highlightWidth;   // px, depth of the glass border light
uniform float  highlightFalloff; // angular falloff exponent, floored >= 0.01 CPU-side
uniform float  lightIntensity;
uniform float  glassOpacity;     // fold blurAlpha from drawBlurRegion in here
uniform float  saturation;

uniform float  hdrHeadroom;      // display HDR/SDR ratio, >= 1. Exactly 1.0 on
                                 // SDR, and every HDR line below then reduces to
                                 // the pre-HDR build bit-for-bit.

uniform float2 touchPos;         // view-local px. Phase 3 (shared-memory channel).
uniform float  touchGlow;        // 0..1 press progress; 0 turns the branch off
uniform float  touchLens;        // 0..1 gate on the press's optical half

// A little blur on what is seen THROUGH the glass, in px, as an Android blur
// radius (sigma = 0.57735 r + 0.5, what RenderEffect.createBlurEffect uses).
// iOS's clear glass is not perfectly sharp, and neither is the desktop
// reference it was calibrated against (radius 2 at half resolution). 0 is the
// sharp view, and sampleSoft() is then exactly one read. It softens the plain
// see-through reads only: the dispersion taps at the very edge already smear
// the image along the displacement, so there each tap steps sideways instead
// (see the dispersion loop): the same blur, still one read per tap.
uniform float  softenRadius;

// 1 when the see-through child is itself a blur: a pane over a backdrop reads
// the backdrop's quarter-resolution image there, which has no detail finer
// than four pixels for a nine-tap kernel to resolve. Four taps at the same
// variance then give the same picture for less than half the reads.
// 2 is for the sharp child: five taps on a rotated grid in the nine's place
// (sampleSoft), which is what a phone's GPU can afford per pixel.
uniform float  softenFast;

// How far in from the edge the edge still does anything, px: the widest of the
// refraction band, the rim light and the sheen (set CPU-side). A fragment
// deeper than this is plain pane: undisplaced, t == 1, no light. Most of a
// large pane is, and main() skips the whole lens for it (see `deep`).
uniform float  deepBand;

// iOS's regular glass, as Core Animation draws it (ported from the maths a
// WebGPU reimplementation of QuartzCore uses; uikit.even.how, datamined into
// projects/uikit-even-how-datamine/FINDINGS.md section 4). Two reads of what is
// behind, the refracted see-through one and the blurred one, and before they
// are mixed by blurMix the see-through one is pulled toward the blur where the
// blur is brighter (lighten, the light theme) or darker (darken, the dark one):
//
//   face = L * max(see, blur) + D * min(see, blur) + (1 - L - D) * see
//
// So in light mode sharp detail survives only where it is brighter than the
// blur, and the glass reads as frosted without losing its edges. 0 and 0 is
// the plain mix this shader always did.
uniform float  fillLighten;
uniform float  fillDarken;

// The reference's tone, after the blend: a luma limiter (k; 0 = off) that
// pulls highlights down and gives back a little chroma, then the colour matrix
// it applies as scale * saturation(c) + bias. Then filterColor multiplies, which
// is how a Material You tint colours the glass itself rather than laying a
// layer over it. 1, 0 and 1 are the identity.
uniform float  lumaLimit;
uniform float  toneScale;
uniform float3 toneBias;
uniform float3 filterColor;

"""

DISPERSION_UNIFORM_BLOCK = """\
uniform float dispersionHeight;      // px
uniform float dispersionAmount;      // px
uniform float dispersionTapSpacing;  // px between taps. MUST be <= unitScale or
                                     // the spread >= 2 invariant breaks and every
                                     // glass edge gets a blue fringe.
uniform float dispersionQuadrant;    // 0 = fringe evenly all round (iOS).
                                     // 1 = Kyant's quadrant weighting.

const int kDispersionTaps = %(taps)d;

"""

GRAIN_UNIFORM_BLOCK = """\
uniform float noiseAmount;

"""

FUNCTIONS = """\
// NaN-free normalize. A ternary is NOT safe: if SkSL lowers `cond ? normalize(v)
// : fb` to an arithmetic select then normalize(0) = NaN is still multiplied by
// 0, and NaN * 0 = NaN. The division form cannot produce NaN for any finite v,
// because the denominator is floored.
float2 safeNormalize(float2 v, float2 fallback) {
    float len = length(v);
    float ok = step(kNormEps, len);
    return mix(fallback, v / max(len, kNormEps), ok);
}

// Quadrant-select any per-corner packed float4 (extents, shapes). The argument
// MUST be the CENTERED coordinate, y-down. NB: `packed` is a reserved word in
// SkSL - hence `corners`.
float cornerParam(float2 c, float4 corners) {
    if (c.x >= 0.0) {
        return (c.y <= 0.0) ? corners.z : corners.y;   // right-top : right-bottom
    }
    return (c.y <= 0.0) ? corners.w : corners.x;       // left-top  : left-bottom
}

// Rounded-rect SDF generalized to the continuous-corner family: a corner is a
// superellipse quadrant in an E x E cell, and (E = r, n = 2) IS the circular
// corner - the else-branch is the plain Euclidean SDF with E in place of r. The
// edge and interior algebra cancels E exactly as it cancelled r, so straight
// edges stay exact for any extent.
//
// The corner branch is the n-norm field with a first-order Euclidean correction,
// sd ~ (g - E) / |grad g|. pow() is only ever called with a strictly positive
// base (the qp > 0 guards), so GLSL-undefined pow(x<0) and pow(0,0) are
// unreachable.
float sdGlassRect(float2 c, float2 halfSize, float4 extents, float4 shapes) {
    float E = cornerParam(c, extents);
    float n = cornerParam(c, shapes);
    float2 innerHalf = halfSize - float2(E);
    float2 q = abs(c) - innerHalf;
    float2 qp = max(q, float2(0.0));
    float outside;
    if (n > 2.001 && qp.x > 0.0 && qp.y > 0.0) {
        float g = max(pow(pow(qp.x, n) + pow(qp.y, n), 1.0 / n), kNormEps);
        float2 dg = pow(qp / g, float2(n - 1.0));
        outside = (g - E) / max(length(dg), kNormEps);
    } else {
        outside = length(qp) - E;
    }
    float inside = min(max(q.x, q.y), 0.0);
    return outside + inside;
}

// In the corner cell the outward direction is the n-norm field's gradient
// (qp/g)^(n-1), which at n = 2 is qp itself. pow(0, n-1) with n > 2 is defined
// (0), so a cell-boundary qp component is safe.
float2 gradGlassRect(float2 c, float2 halfSize, float4 extents, float4 shapes) {
    float E = cornerParam(c, extents);
    float n = cornerParam(c, shapes);
    float2 innerHalf = halfSize - float2(E);
    float2 q = abs(c) - innerHalf;

    float insideCorner = step(0.0, min(q.x, q.y));
    float xMajor = step(q.y, q.x);
    float2 gradEdge = float2(xMajor, 1.0 - xMajor);
    float2 qp = max(q, float2(0.0));
    float2 dir = qp;
    if (n > 2.001) {
        float g = max(pow(pow(qp.x, n) + pow(qp.y, n), 1.0 / n), kNormEps);
        dir = pow(qp / g, float2(n - 1.0));
    }
    float2 gradCorner = safeNormalize(dir, float2(0.0));
    return sign(c) * mix(gradEdge, gradCorner, insideCorner);
}

// KEEP the max(0.0, ...): without it this returns NaN for |x| > 1.
float circleMap(float x) {
    return 1.0 - sqrt(max(0.0, 1.0 - x * x));
}

"""

HASH_NOISE = """\
// MUST stay float: 43758.5453 has no useful fp16 mantissa left after the
// multiply, and sin() of a large half-precision argument is pure noise on Mali.
float hashNoise(float2 co) {
    return fract(sin(dot(co, float2(12.9898, 78.233))) * 43758.5453);
}

"""

SAMPLERS = """\
// view-local px -> local/canvas px, with the press dent applied.
//
// The dent: samples near the finger are pulled toward it, which reads as a
// magnifying bulge travelling with the touch - the glass flexing, not just
// lighting up. Living here means every read warps coherently (interior, rim
// band, all dispersion taps) for the cost of a length() per sample, and
// geometry (sd, normals, t) stays un-warped. d == 0 at the touch point, so
// there is no normalize() and no NaN to guard.
float2 warpToLocal(float2 viewPx) {
    if (touchGlow * touchLens > 0.0) {
        float2 d = viewPx - touchPos;
        float f = 1.0 - smoothstep(0.0, 0.6 * min(size.x, size.y), length(d));
        viewPx -= d * (f * f * 0.22 * touchGlow * touchLens);
    }
    return clamp(viewPx - offset, crop.xy, crop.zw);
}

// Both children are premultiplied; un-premultiply and carry alpha out so a
// partially-covered texel keeps its true hue instead of reading as black. On
// the SurfaceFlinger path both snapshots are opaque, so alpha is 1 and every
// line of the coverage machinery below collapses to the identity - it is kept
// so this stays numerically identical to the app-side reference renderer, and
// can be stripped once calibration is signed off.
// NOT named `unpremul`: SkSL already declares `$pure half4 unpremul(half4)` as
// a builtin, and overloads differing only in return type are rejected. The
// collision exists in RenderEngine's Skia too, where the compile failure would
// have been a soft reboot rather than a logcat line.
float4 toStraight(half4 h) {
    float4 c = float4(h);
    return float4(c.rgb / max(c.a, 1e-4), c.a);
}

float4 sampleSharp(float2 viewPx)   { return toStraight(sharpContent.eval(warpToLocal(viewPx))); }
float4 sampleBlurred(float2 viewPx) { return toStraight(blurredContent.eval(warpToLocal(viewPx))); }

// The sharp view with softenRadius of blur: a 3x3 Gaussian at one-sigma
// spacing (weights e^0, e^-1/2, e^-1, normalised), the linear filter filling in
// between taps. Good to a few dp; past that the grid would start to show.
// Averaged premultiplied, then un-premultiplied once, like any blur.
float4 sampleSoft(float2 viewPx) {
    if (softenRadius <= 0.0) return sampleSharp(viewPx);
    float s = 0.57735 * softenRadius + 0.5;
    if (softenFast > 1.5) {
        // The centre and (2,1), (-1,2), (-2,-1), (1,-2) times k: no two taps
        // share a row or a column, so each axis sees five positions, -2k to
        // 2k, from five reads. Weights 0.36 and 0.16 with k = 0.585 s give
        // the nine taps' variance, 0.548 s^2 on each axis, and no direction
        // is left without a tap near it the way a plain X or + would leave.
        float k = 0.585 * s;
        float4 c = float4(sharpContent.eval(warpToLocal(viewPx)));
        float4 r = float4(sharpContent.eval(warpToLocal(viewPx + float2( 2.0 * k,  k))))
                 + float4(sharpContent.eval(warpToLocal(viewPx + float2(-k,  2.0 * k))))
                 + float4(sharpContent.eval(warpToLocal(viewPx + float2(-2.0 * k, -k))))
                 + float4(sharpContent.eval(warpToLocal(viewPx + float2( k, -2.0 * k))));
        return toStraight(half4(c * 0.36 + r * 0.16));
    }
    if (softenFast > 0.5) {
        // The nine taps below have a variance of 0.548 s^2 on each axis; four
        // at (+-d, +-d) have d^2.
        float d = 0.7403 * s;
        float4 q = float4(sharpContent.eval(warpToLocal(viewPx + float2( d,  d))))
                 + float4(sharpContent.eval(warpToLocal(viewPx + float2(-d,  d))))
                 + float4(sharpContent.eval(warpToLocal(viewPx + float2( d, -d))))
                 + float4(sharpContent.eval(warpToLocal(viewPx + float2(-d, -d))));
        return toStraight(half4(q * 0.25));
    }
    float4 c = float4(sharpContent.eval(warpToLocal(viewPx)));
    float4 a = float4(sharpContent.eval(warpToLocal(viewPx + float2( s, 0.0))))
             + float4(sharpContent.eval(warpToLocal(viewPx + float2(-s, 0.0))))
             + float4(sharpContent.eval(warpToLocal(viewPx + float2(0.0,  s))))
             + float4(sharpContent.eval(warpToLocal(viewPx + float2(0.0, -s))));
    float4 d = float4(sharpContent.eval(warpToLocal(viewPx + float2( s,  s))))
             + float4(sharpContent.eval(warpToLocal(viewPx + float2(-s,  s))))
             + float4(sharpContent.eval(warpToLocal(viewPx + float2( s, -s))))
             + float4(sharpContent.eval(warpToLocal(viewPx + float2(-s, -s))));
    return toStraight(half4(c * 0.20418 + a * 0.12384 + d * 0.07511));
}

"""

MAIN_PROLOGUE = """\
half4 main(float2 fragCoord) {
    float2 pixels = fragCoord + offset;         // view-local px, top-left origin, y-down
    float2 halfSize = size * 0.5;
    float2 centered = pixels - halfSize;

    // The SDF alone decides the early-out, so the gradient / direction / scale
    // work is skipped for every pixel outside the shape.
    float sd = sdGlassRect(centered, halfSize, cornerExtents, cornerShapes);

    // fwidth() does not exist in AGSL/SkSL runtime effects. |grad(sd)| == 1 for
    // an exact SDF and we are in device pixels, so fwidth(sd) == 1.0 - a 2 px
    // feather, which is what iOS produces.
    //
    // NB: this shader owns its own antialiasing, which is why the caller must
    // drawRect(effectRegion.rect()) and NOT drawRRect(effectRegion). Drawing the
    // rrect would clip with Skia's own AA on top of this and double-feather the
    // edge - and would also foreclose ever drawing outside the region.
    float aa = 1.0;
    float shapeAlpha = 1.0 - smoothstep(-aa, aa, sd);
    if (shapeAlpha <= 0.001) {
        return half4(0.0);
    }

    float inside = -min(sd, 0.0);

    // ---- the frost handover -------------------------------------------------
    // One blurred read, always, at the UNDISPLACED position: the compositor
    // already chose this region's blur radius, and the deep interior is simply
    // that. `frostT` reaches blurMix at t == 1, and the lens path is skipped
    // entirely when frost has fully taken over - which is the whole interior at
    // the default blurMix == 1. The branch is uniform-coherent across that
    // region, so it is close to free.
    float4 fr = sampleBlurred(pixels);
    float3 color = fr.rgb;
    float bdAlpha = fr.a;

    // Past deepBand the edge is out of reach: t is exactly 1, so frostT is
    // blurMix, the lens reads straight through, and the rim and the sheen have
    // both faded to nothing. Those fragments take the short way: none of the
    // gradient, the three normalizes, the two pow() or the dispersion below.
    // Every line of it is the line of the long way with t == 1 put in, so the
    // two agree exactly where they meet.
    bool deep = inside >= deepBand;

    float2 normal = float2(0.0, 1.0);
    float2 lobeDir = float2(-highlightDir.y, highlightDir.x);

    if (deep) {
        float frostT = blurMix;
        if (frostT < 0.999) {
            float4 sIn = sampleSoft(pixels);
            float3 lens = sIn.rgb;
            float lensA = sIn.a;
            if (lensBlur > 0.0) {
                lens = mix(lens, fr.rgb, lensBlur);
                lensA = mix(lensA, fr.a, lensBlur);
            }
            float3 face = lens;
            if (fillLighten + fillDarken > 0.0) {
                face = fillLighten * max(lens, fr.rgb) + fillDarken * min(lens, fr.rgb)
                     + (1.0 - fillLighten - fillDarken) * lens;
            }
            color = mix(face, fr.rgb, frostT);
            bdAlpha = mix(lensA, fr.a, frostT);
        }
    } else {
    float4 maxGradExtent = float4(min(halfSize.x, halfSize.y));
    float4 gradExtents = min(cornerExtents * 1.5, maxGradExtent);
    normal = safeNormalize(
        gradGlassRect(centered, halfSize, gradExtents, cornerShapes),
        float2(0.0, 1.0));

    float2 radial = safeNormalize(centered, float2(0.0));

    // The light axis: highlightDir rotated +90 degrees - the same axis the
    // HIGHLIGHT lobes sit on, so `highlight.angle` steers both. At swirl 0 the
    // sum is exactly the iOS/Kyant gradient.
    float2 direction = safeNormalize(
        normal + depthEffect * radial + refractionSwirl * lobeDir, normal);

    // Anisotropic refraction height: n^2 . refractionScale yields
    // refractionScale.x on a vertical edge and .y on a horizontal one.
    float2 limit = max(halfSize, float2(1e-3));
    float2 scale2 = clamp(refractionScale, float2(1e-3), limit);
    float scale = max(dot(normal * normal, scale2), 1e-3);

    float t = clamp(inside / scale, 0.0, 1.0);

    float touchBoost = 1.0 + 0.35 * touchGlow * touchLens;

    float handover = pow(smoothstep(0.0, 1.0, t), max(blurFalloff, 1e-3));
    float frostT = mix(handover, 1.0, rimFrost) * blurMix;

    // With rimFrost the lens runs even under full frost: the rim then shows
    // the blur bent, where it used to show the screen.
    if (frostT < 0.999 || rimFrost > 0.0) {
        float3 lens = fr.rgb;
        float lensA = fr.a;
        // Where the lens ended up reading. Needed so lensBlur can re-read the
        // blurred child at the same place rather than at fragCoord.
        float2 lensPos = pixels;

"""

# Note the indentation: the two refraction bodies live inside `if (frostT < 0.999)`.

REFRACTION_WITH_DISPERSION = """\
        if (inside >= scale) {
            float4 sIn = sampleSoft(pixels);
            lens = sIn.rgb;
            lensA = sIn.a;
        } else {
            // The pow() base is 1-t with t clamped to [0,1] so it is never
            // negative, and the exponent is floored so pow(0,0) is unreachable.
            float profile = circleMap(pow(1.0 - t, max(profilePower, 1e-3)));
            float amount = (profile + profileBias * (1.0 - t)) * refractionAmount * touchBoost;
            float2 base = pixels - amount * direction;   // NOTE the minus
            lensPos = base;

            float dispersionT = clamp(inside / max(dispersionHeight, 1e-3), 0.0, 1.0);
            float spread = circleMap(1.0 - dispersionT) * dispersionAmount * touchBoost;

            // Kyant's quadrant weighting. The product of the normalized centered
            // coordinates is 0 along BOTH centre lines and +/-1 at the corners,
            // so the fringe lives where the eye already reads the shape bending.
            // Left signed on purpose: the sign flips between adjacent quadrants,
            // which walks the taps the other way and so reverses the hue order.
            // That alternation is the effect's whole signature, and abs() would
            // quietly delete it. |weight| <= 1 always, so this only ever SHRINKS
            // the spread.
            float2 quadrant = centered / max(halfSize, float2(1e-3));
            spread *= mix(1.0, quadrant.x * quadrant.y, dispersionQuadrant);

            // Magnitude from here down: a negative spread is a direction, not a
            // smaller one. The `< 2 * unitScale` guard is what keeps maxI >= 2,
            // so `u` takes at least 0, 0.5 and 1.0 and every channel mask fires.
            // If only u = 0 were active the mask would be (0,0,1) and the divide
            // would force red and green to black - a solid blue rim on every edge.
            float spreadMag = abs(spread);

            if (spreadMag < 2.0 * unitScale) {
                float4 sBase = sampleSoft(base);
                lens = sBase.rgb;
                lensA = sBase.a;
            } else {
                // LONGITUDINAL aberration: taps walk along the displacement axis
                // itself - R sampled deepest, B shallowest, G centred.
                // `base - direction * s` deepens the sample for positive s, so
                // the R half of the mask (u > 0.5) lands deepest.
                float3 accumulated = float3(0.0);
                float3 weight = float3(0.0);
                float aAccum = 0.0;
                float aTaps = 0.0;

                float maxI = min(spreadMag / max(dispersionTapSpacing, 1e-3),
                                 float(kDispersionTaps));

                // softenRadius, here without a single extra read. The taps
                // already walk ALONG the displacement, so each channel's
                // average is blurred in that direction by its share of the
                // spread. What is missing is ACROSS it: each tap also steps
                // sideways, cycling through four offsets (-1.2, -0.4, 0.4,
                // 1.2) x 1.118 sigma, whose spread is the soften sigma. Every
                // channel spans at least one full cycle on the MEDIUM and HIGH
                // tiers, so the fringe is blurred in 2D like the rest of the
                // pane. Zero when softenRadius is, which leaves this loop
                // exactly as it was.
                float2 across = float2(-direction.y, direction.x);
                float acrossStep = softenRadius > 0.0
                    ? (0.57735 * softenRadius + 0.5) * 1.118 : 0.0;

                for (int i = 0; i < kDispersionTaps; ++i) {
                    float u = float(i) / maxI;
                    // A dynamic `if` rather than `break`: it stays statically
                    // unrollable across the whole Skia range, and the GPU still
                    // skips the eval when the wave agrees. The accumulator and
                    // weight are untouched when the test fails, so the result is
                    // identical.
                    if (u <= 1.0) {
                        float side = (mod(float(i), 4.0) * 0.8 - 1.2) * acrossStep;
                        float4 tap = sampleSharp(base - direction * (u - 0.5) * spread
                                                 + across * side);
                        float3 mask = float3(step(0.5, u),                   // R: upper half
                                             step(0.25, u) * step(u, 0.75),  // G: middle band
                                             step(u, 0.5));                  // B: lower half
                        accumulated += tap.rgb * mask;
                        weight += mask;
                        aAccum += tap.a;
                        aTaps += 1.0;
                    }
                }
                lens = accumulated / max(weight, float3(1e-6));
                lensA = aAccum / max(aTaps, 1.0);
            }
        }

"""

REFRACTION_ONLY = """\
        if (inside >= scale) {
            float4 s = sampleSoft(pixels);
            lens = s.rgb;
            lensA = s.a;
        } else {
            // The LOW tier stops here: no chromatic dispersion.
            float profile = circleMap(pow(1.0 - t, max(profilePower, 1e-3)));
            float amount = (profile + profileBias * (1.0 - t)) * refractionAmount * touchBoost;
            lensPos = pixels - amount * direction;   // NOTE the minus
            float4 s = sampleSoft(lensPos);
            lens = s.rgb;
            lensA = s.a;
        }

"""

FROST_MERGE = """\
        // Fold the compositor's own blur back into the lens, at the position the
        // lens actually read from. Guarded so the default costs nothing. Past
        // the rim the lens read at this very fragment, where the blurred child
        // was already read (`fr`), so only the rim reads it again.
        float4 lb = fr;
        if (inside < scale && (lensBlur > 0.0 || rimFrost > 0.0)) {
            lb = sampleBlurred(lensPos);
        }
        if (lensBlur > 0.0) {
            lens = mix(lens, lb.rgb, lensBlur);
            lensA = mix(lensA, lb.a, lensBlur);
        }

        // The frost this band is mixed with (see rimFrost): the blur behind
        // the fragment, or the blur where the lens read. `fr` exactly at 0.
        float4 frost = mix(fr, lb, rimFrost);

        // The blur fill (see fillLighten). Exactly `lens` when both are 0.
        float3 face = lens;
        if (fillLighten + fillDarken > 0.0) {
            face = fillLighten * max(lens, frost.rgb) + fillDarken * min(lens, frost.rgb)
                 + (1.0 - fillLighten - fillDarken) * lens;
        }
        color = mix(face, frost.rgb, frostT);
        bdAlpha = mix(lensA, frost.a, frostT);
    }
    }   // !deep

"""

TONE_CHAIN = """\
    color = clamp(color, 0.0, 1.0);

    if (lumaLimit > 0.0) {
        float l = dot(color, float3(0.2126, 0.7152, 0.0722));
        float s = clamp(1.0 - lumaLimit * l, 0.0, 1.0);
        color = mix(float3(s * l), s * color, 1.0 + 0.3 * (1.0 - s));
    }

    float luma = dot(color, float3(0.2126, 0.7152, 0.0722));
    color = mix(float3(luma), color, saturation);
    color = color * toneScale + toneBias;
    color *= filterColor;

    // Frost and tint are composited by real coverage rather than mixed straight
    // onto the transmitted colour: glass over nothing recorded is still glass -
    // you lose what it transmits, not the pane. At bdAlpha == 1 (always true on
    // the SurfaceFlinger path) every line collapses to the original two mixes.
    float surfaceAlpha = frostColor.a + tintColor.a * (1.0 - frostColor.a);
    float3 surfacePremul =
        frostColor.rgb * (frostColor.a * (1.0 - tintColor.a)) + tintColor.rgb * tintColor.a;
    float transmitWeight = (1.0 - surfaceAlpha) * bdAlpha;
    float cover = surfaceAlpha + transmitWeight;
    color = (color * transmitWeight + surfacePremul) / max(cover, 1e-4);

"""

GRAIN = """\
    if (noiseAmount > 0.0) {
        // `pixels`, not fragCoord: view-local and stable, so the grain field
        // does not jump when the region moves.
        color += (hashNoise(pixels * 1e-3) - 0.5) * noiseAmount;
    }

"""

HIGHLIGHT = """\
    // The glass border light, and nothing else. Falloff model per Kyant's
    // DefaultHighlightShaderString: `normal` - not the position angle - is what
    // holds a long edge at constant intensity, and abs() is what lights BOTH
    // light-axis lobes. The rim is ADDED, not multiplied, which is what makes it
    // read over a black backdrop, and it fades over its own highlightWidth.
    //
    // There is deliberately NO drawn dark line. The dark edge real iOS glass
    // shows is the refraction FOLD imaging a dark stripe, not ink - our profile
    // folds the same way, so inking it on top double-darkens every strong edge.
    float glint = 1.0 + (hdrHeadroom - 1.0) * 0.75;

    // The interior is min()'d to the headroom first - glass does not amplify
    // what it merely transmits - and only its own light scales into HDR.
    color = min(color * (1.0 + lightIntensity * glint), hdrHeadroom);

    // Past deepBand both bands below are exactly zero, so there is no light to
    // add (and no pow to take).
    if (!deep) {
        float rim = pow(abs(dot(normal, lobeDir)), highlightFalloff);
        float rimT = clamp(inside / max(highlightWidth, 1e-3), 0.0, 1.0);
        float rimBand = 1.0 - smoothstep(0.0, 1.0, rimT);

        // The faint broad glow under the lit edges, ~7 pt deep at ~0.18x the
        // rim's weight. This is what lets the crisp line drop to hairline
        // width without the edge going dead.
        float sheenT = clamp(inside / (7.0 * unitScale), 0.0, 1.0);
        float sheen = 1.0 - smoothstep(0.0, 1.0, sheenT);

        color += rim * rimBand * highlightIntensity * glint;
        color += rim * sheen * highlightIntensity * 0.18;
    }

"""

TOUCH_GLOW_FRAGMENT = """\
    if (touchGlow > 0.0) {
        float touchRadius = 1.5 * min(size.x, size.y);
        float touchDist = distance(pixels, touchPos);
        // Written `1.0 - smoothstep(lo, hi, d)`, NOT `smoothstep(hi, lo, d)` -
        // reversed edges are undefined in GLSL ES, and Mali is where "undefined"
        // stops meaning "works anyway".
        float touchFalloff = 1.0 - smoothstep(touchRadius * 0.5, touchRadius, touchDist);
        color += (0.08 + 0.15 * touchFalloff) * touchGlow;

        float hotFalloff = 1.0 - smoothstep(0.0, 90.0 * unitScale, touchDist);
        color += 0.35 * (hdrHeadroom - 1.0) * hotFalloff * hotFalloff * touchGlow;
    }

"""

MAIN_EPILOGUE = """\
    color = clamp(color, 0.0, hdrHeadroom);

    // Fold drawBlurRegion's blurAlpha into glassOpacity on the CPU side.
    float alpha = shapeAlpha * glassOpacity * cover;
    // float -> half is not an implicit conversion in SkSL; construct explicitly.
    return half4(color * alpha, alpha);
}
"""


# ---------------------------------------------------------------------------- tone
#
# A toned backdrop (the shade, a dialog's blur-behind) is the glass with the
# lens off: the blurred child everywhere, a saturation and a veil. Run through
# the shader above it costs the whole lens per pixel of a full-screen region
# (the SDF gradient, three normalizes, the rim's pow) to add nothing, and it
# names the sharp child, which makes Skia copy the framebuffer (the snapshot
# cannot be sampled while it is being drawn into). This is the same picture
# with what is left once every lens term is zero: each line below is the line
# of the main shader it stands for, in the same order, so the two agree to the
# last bit. Only for a backdrop past its fade-in; while the blur ramps up the
# main shader still cross-fades the sharp child in.

TONE_HEADER = """\
// =========================================================================
//  glass_tone.sksl - GENERATED by shader/gen_glass_sksl.py. Do not edit.
//
//  A toned backdrop: Android's blur, a saturation and a veil in the frost
//  colour. What glass_high.sksl computes when every lens term is zero, with
//  one child and one read.
// =========================================================================

const float kNormEps = 1e-6;

uniform shader blurredContent;   // <- blurredImage, Kawase output, 0.25x

uniform float2 size;             // effectRegion.rect() size, px
uniform float2 offset;           // -rect.topLeft(): fragCoord -> view-local px
uniform float4 crop;             // blurRect in local space (minX,minY,maxX,maxY)
uniform float4 cornerExtents;    // px, packed (BL, BR, TR, TL)
uniform float4 cornerShapes;     // superellipse exponent per corner
uniform float4 frostColor;       // rgb = the veil's colour, a = its strength
uniform float  glassOpacity;     // blurAlpha from drawBlurRegion, folded in
uniform float  saturation;
uniform float  hdrHeadroom;      // display HDR/SDR ratio, >= 1

"""

TONE_MAIN = """\
float4 toStraight(half4 h) {
    float4 c = float4(h);
    return float4(c.rgb / max(c.a, 1e-4), c.a);
}

half4 main(float2 fragCoord) {
    float2 pixels = fragCoord + offset;
    float2 halfSize = size * 0.5;
    float2 centered = pixels - halfSize;

    float sd = sdGlassRect(centered, halfSize, cornerExtents, cornerShapes);
    float aa = 1.0;
    float shapeAlpha = 1.0 - smoothstep(-aa, aa, sd);
    if (shapeAlpha <= 0.001) {
        return half4(0.0);
    }

    float4 fr = toStraight(blurredContent.eval(clamp(pixels - offset, crop.xy, crop.zw)));
    float3 color = clamp(fr.rgb, 0.0, 1.0);
    float bdAlpha = fr.a;

    float luma = dot(color, float3(0.2126, 0.7152, 0.0722));
    color = mix(float3(luma), color, saturation);

    float surfaceAlpha = frostColor.a;
    float3 surfacePremul = frostColor.rgb * frostColor.a;
    float transmitWeight = (1.0 - surfaceAlpha) * bdAlpha;
    float cover = surfaceAlpha + transmitWeight;
    color = (color * transmitWeight + surfacePremul) / max(cover, 1e-4);

    color = min(color, hdrHeadroom);
    color = clamp(color, 0.0, hdrHeadroom);

    float alpha = shapeAlpha * glassOpacity * cover;
    return half4(color * alpha, alpha);
}
"""

TONE_UNIFORMS = [
    "size", "offset", "crop", "cornerExtents", "cornerShapes",
    "frostColor", "glassOpacity", "saturation", "hdrHeadroom",
]


def build_tone(lean: bool = False) -> str:
    # The SDF is the main shader's own text, so the two outline one shape.
    start = FUNCTIONS.index("// Quadrant-select any per-corner packed float4")
    end = FUNCTIONS.index("// In the corner cell the outward direction")
    if not lean:
        return TONE_HEADER + FUNCTIONS[start:end] + TONE_MAIN
    assert TONE_HEADER.count("glass_tone.sksl") == 1
    return (TONE_HEADER.replace("glass_tone.sksl", "glass_tone_lean.sksl")
            + round_only(FUNCTIONS[start:end], grad=False) + TONE_MAIN)


# ---------------------------------------------------------------------------- lean
#
# The shader above decides what it does from its uniforms: no fringe when
# dispersionAmount is 0, no grain when noiseAmount is, no press dent or glow
# when touchGlow is, one read when softenRadius is. A phone's GPU does not
# reliably skip what such a test rules out. On a Poco F1 (Adreno 630) a pane
# with the lens band a third as wide, or with the lens off and one read, cost
# what a whole pane does (docs/VERIFIED.md, v0.4.20), where the arithmetic
# written for those cases is a fraction of it: code that a branch should have
# skipped was being paid for.
#
# So the same shader is built again with those parts left out of the text, and
# the engine draws a pane with the leanest build whose missing parts are all
# zero for that pane. Every line that is left is a line of the shader above
# with the zero put in, so the picture is the same:
#
#   lean       no fringe, grain or touch; sampleSoft is its single read
#   lean_soft  the same with sampleSoft as it is (softenRadius above zero)
#
# Both also leave out the superellipse corner (round_only): they are for panes
# whose corners are all circular, which is every pane unless cornerShape is
# set. glass_tone_lean.sksl is the tone shader with the same done to it.
#
# The uniforms are declared exactly as in HIGH, used or not, so that one
# uniform block serves all three; the engine checks that the offsets agree.

SD_SUPER = """\
    float E = cornerParam(c, extents);
    float n = cornerParam(c, shapes);
    float2 innerHalf = halfSize - float2(E);
    float2 q = abs(c) - innerHalf;
    float2 qp = max(q, float2(0.0));
    float outside;
    if (n > 2.001 && qp.x > 0.0 && qp.y > 0.0) {
        float g = max(pow(pow(qp.x, n) + pow(qp.y, n), 1.0 / n), kNormEps);
        float2 dg = pow(qp / g, float2(n - 1.0));
        outside = (g - E) / max(length(dg), kNormEps);
    } else {
        outside = length(qp) - E;
    }
"""
SD_ROUND = """\
    float E = cornerParam(c, extents);
    float2 innerHalf = halfSize - float2(E);
    float2 q = abs(c) - innerHalf;
    float2 qp = max(q, float2(0.0));
    float outside = length(qp) - E;
"""
GRAD_SUPER = """\
    float E = cornerParam(c, extents);
    float n = cornerParam(c, shapes);
    float2 innerHalf = halfSize - float2(E);
    float2 q = abs(c) - innerHalf;

    float insideCorner = step(0.0, min(q.x, q.y));
    float xMajor = step(q.y, q.x);
    float2 gradEdge = float2(xMajor, 1.0 - xMajor);
    float2 qp = max(q, float2(0.0));
    float2 dir = qp;
    if (n > 2.001) {
        float g = max(pow(pow(qp.x, n) + pow(qp.y, n), 1.0 / n), kNormEps);
        dir = pow(qp / g, float2(n - 1.0));
    }
    float2 gradCorner = safeNormalize(dir, float2(0.0));
"""
GRAD_ROUND = """\
    float E = cornerParam(c, extents);
    float2 innerHalf = halfSize - float2(E);
    float2 q = abs(c) - innerHalf;

    float insideCorner = step(0.0, min(q.x, q.y));
    float xMajor = step(q.y, q.x);
    float2 gradEdge = float2(xMajor, 1.0 - xMajor);
    float2 qp = max(q, float2(0.0));
    float2 gradCorner = safeNormalize(qp, float2(0.0));
"""


def round_only(functions: str, grad: bool = True) -> str:
    """The SDF and its gradient for corners that are all circular (every
    exponent 2, the default and all an SkRRect can mean): the superellipse
    branches left out, which is the `else` of each. Five `pow` in the SDF and
    four in the gradient that a phone's GPU may otherwise run to throw away."""
    assert functions.count(SD_SUPER) == 1
    functions = functions.replace(SD_SUPER, SD_ROUND)
    if grad:
        assert functions.count(GRAD_SUPER) == 1
        functions = functions.replace(GRAD_SUPER, GRAD_ROUND)
    return functions


def lean_samplers(soft: bool) -> str:
    dent = """\
    if (touchGlow * touchLens > 0.0) {
        float2 d = viewPx - touchPos;
        float f = 1.0 - smoothstep(0.0, 0.6 * min(size.x, size.y), length(d));
        viewPx -= d * (f * f * 0.22 * touchGlow * touchLens);
    }
"""
    assert SAMPLERS.count(dent) == 1
    samplers = SAMPLERS.replace(dent, "")
    if not soft:
        start = samplers.index("// The sharp view with softenRadius of blur")
        samplers = (samplers[:start]
                    + "// softenRadius is 0 for every pane drawn with this build.\n"
                    + "float4 sampleSoft(float2 viewPx) { return sampleSharp(viewPx); }\n\n")
    return samplers


def lean_preamble(name: str) -> list[str]:
    return [
        HEADER % {"tier": name},
        CHILDREN,
        UNIFORM_BLOCK,
        DISPERSION_UNIFORM_BLOCK % {"taps": TAPS["HIGH"]},
        GRAIN_UNIFORM_BLOCK,
        round_only(FUNCTIONS),
    ]


def build_lean(soft: bool) -> str:
    # touchBoost is 1.0 + 0.35 * 0: a factor of exactly one.
    boost = "    float touchBoost = 1.0 + 0.35 * touchGlow * touchLens;\n\n"
    assert MAIN_PROLOGUE.count(boost) == 1
    assert REFRACTION_ONLY.count(" * touchBoost") == 1
    return "".join(lean_preamble("lean_soft" if soft else "lean") + [
        lean_samplers(soft),
        MAIN_PROLOGUE.replace(boost, ""),
        REFRACTION_ONLY.replace(" * touchBoost", ""),
        FROST_MERGE,
        TONE_CHAIN,
        HIGHLIGHT,
        MAIN_EPILOGUE,
    ])


# For measuring only (debugPane=3): the lean build with no lens at all. Every
# fragment takes what the shader calls its deep path: the blur, the see-through
# read, the tone chain, the pane's own light; no gradient, no bend, no rim. It
# is not glass (a pane drawn with it has no edge), and what it costs next to
# `lean` is what the lens costs when it is really gone, which a narrower lens
# band did not show on a phone.
def build_deep() -> str:
    head = MAIN_PROLOGUE[:MAIN_PROLOGUE.index("    // Past deepBand the edge is out of reach")]
    a = MAIN_PROLOGUE.index("    if (deep) {\n") + len("    if (deep) {\n")
    b = MAIN_PROLOGUE.index("    } else {\n", a)
    light = HIGHLIGHT[:HIGHLIGHT.index("    // Past deepBand both bands below")]
    assert "float glint" in light and "lightIntensity" in light
    return "".join(lean_preamble("deep") + [
        lean_samplers(False),
        head,
        "    {\n",
        MAIN_PROLOGUE[a:b],
        "    }\n\n",
        TONE_CHAIN,
        light,
        MAIN_EPILOGUE,
    ])


def build(tier: str) -> tuple[str, list[str]]:
    taps = TAPS[tier]
    disperses = taps > 0
    grain = tier != "LOW"

    live = list(BASE_UNIFORMS)
    if disperses:
        live += DISPERSION_UNIFORMS
    if grain:
        live += GRAIN_UNIFORMS

    parts = [
        HEADER % {"tier": tier.lower()},
        CHILDREN,
        UNIFORM_BLOCK,
    ]
    if disperses:
        parts.append(DISPERSION_UNIFORM_BLOCK % {"taps": taps})
    if grain:
        parts.append(GRAIN_UNIFORM_BLOCK)

    parts.append(FUNCTIONS)
    if grain:
        parts.append(HASH_NOISE)
    parts.append(SAMPLERS)

    parts.append(MAIN_PROLOGUE)
    parts.append(REFRACTION_WITH_DISPERSION if disperses else REFRACTION_ONLY)
    parts.append(FROST_MERGE)
    parts.append(TONE_CHAIN)
    if grain:
        parts.append(GRAIN)
    parts.append(HIGHLIGHT)
    parts.append(TOUCH_GLOW_FRAGMENT)
    parts.append(MAIN_EPILOGUE)

    return "".join(parts), live


def main() -> None:
    out_dir = Path(__file__).parent / "build"
    out_dir.mkdir(exist_ok=True)

    manifest = []
    for tier in ("LOW", "MEDIUM", "HIGH"):
        src, live = build(tier)
        path = out_dir / f"glass_{tier.lower()}.sksl"
        path.write_text(src)
        manifest.append(f"{tier.lower()}\t{len(src)}\t{len(live)}\t" + ",".join(live))
        print(f"{path.name:24} {len(src):6d} bytes  {len(live):2d} live uniforms")

    for name, text in (("glass_lean.sksl", build_lean(False)),
                       ("glass_lean_soft.sksl", build_lean(True)),
                       ("glass_deep.sksl", build_deep()),
                       ("glass_tone_lean.sksl", build_tone(lean=True))):
        (out_dir / name).write_text(text)
        print(f"{name:24} {len(text):6d} bytes")

    tone = build_tone()
    (out_dir / "glass_tone.sksl").write_text(tone)
    manifest.append(f"tone\t{len(tone)}\t{len(TONE_UNIFORMS)}\t" + ",".join(TONE_UNIFORMS))
    print(f"{'glass_tone.sksl':24} {len(tone):6d} bytes  {len(TONE_UNIFORMS):2d} live uniforms")

    # The uploader consults this: Skia throws at draw time for a declared
    # uniform that was never set, and the LOW tier drops five of them.
    (out_dir / "uniforms.tsv").write_text("\n".join(manifest) + "\n")


if __name__ == "__main__":
    main()
