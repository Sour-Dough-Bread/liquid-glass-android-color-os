// The bridge's helpers that hold in any build: what the drawing code
// (glass_draw.cpp) needs of Skia beyond the bridge's entry points, made from
// them. The module and a ROM that builds the glass in (rom/) both compile
// this file. sk_bridge.cpp is the module's alone; a ROM has its own compile()
// and links Skia's members.
#include "sk_bridge.h"

#include <cmath>

namespace glass {
namespace skia {

// Inverts a 3x3 matrix, through the target's own invertNonIdentity when it has
// one and by hand when it does not.
//
// Android 16 QPR2 inlines invertNonIdentity away entirely — seen on both QPR2
// ROMs tested — which would have made every current AOSP build unusable over one
// arithmetic helper. The general adjugate/determinant inverse below is a dozen lines and
// handles perspective too, so nothing is given up by not calling Skia's.
bool invertMatrix(const Bridge& b, const SkMatrix& m, SkMatrix* out) {
    if (b.MatrixInvertNonIdentity != nullptr) return b.MatrixInvertNonIdentity(&m, out);

    const float a0 = m.get(0), a1 = m.get(1), a2 = m.get(2);
    const float a3 = m.get(3), a4 = m.get(4), a5 = m.get(5);
    const float a6 = m.get(6), a7 = m.get(7), a8 = m.get(8);

    const float c0 = a4 * a8 - a5 * a7;
    const float c1 = a5 * a6 - a3 * a8;
    const float c2 = a3 * a7 - a4 * a6;
    const float det = a0 * c0 + a1 * c1 + a2 * c2;
    if (det == 0.0f || !std::isfinite(det)) return false;
    const float inv = 1.0f / det;

    *out = SkMatrix::MakeAll(c0 * inv, (a2 * a7 - a1 * a8) * inv, (a1 * a5 - a2 * a4) * inv,
                             c1 * inv, (a0 * a8 - a2 * a6) * inv, (a2 * a3 - a0 * a5) * inv,
                             c2 * inv, (a1 * a6 - a0 * a7) * inv, (a0 * a4 - a1 * a3) * inv);
    return true;
}

SkMatrix canvasTotalMatrix(const Bridge& b, const SkCanvas* canvas) {
    if (b.CanvasGetTotalMatrix != nullptr) return b.CanvasGetTotalMatrix(canvas);
    // The module's own definition (sk_bridge.cpp), which depends on MCRec's
    // layout; a ROM build links Skia's.
    return canvas->getTotalMatrix();
}

void paintSetShader(const Bridge& b, SkPaint* paint, SkShader* shader) {
    if (b.PaintSetShader != nullptr) {
        // Passed by value, so the callee receives a pointer to our temporary and
        // takes the reference we are holding.
        void* slot = shader;
        b.PaintSetShader(paint, &slot);
        return;
    }
    paint->setShader(sk_sp<SkShader>(shader));  // adopts; the module's own definition
}

namespace {

bool isIdentity(const SkMatrix& m) {
    // Testing the nine floats rather than calling isIdentity() keeps the
    // out-of-line computeTypeMask() out of this .so entirely; asM33 leaves the
    // type mask unknown, so isIdentity() would have to compute it.
    for (int i = 0; i < 9; ++i) {
        if (m.get(i) != (i % 4 == 0 ? 1.0f : 0.0f)) return false;
    }
    return true;
}

// Post-concatenates the inverse canvas matrix, which takes the device-space
// snapshots into the canvas-local space the shader is evaluated in.
void undoCanvas(const Bridge& b, const SkCanvas* canvas, SkMatrix* m) {
    const SkMatrix ctm = canvasTotalMatrix(b, canvas);
    if (isIdentity(ctm)) return;
    SkMatrix inverse;
    if (invertMatrix(b, ctm, &inverse)) {
        b.MatrixPostConcat(m, &inverse);
    }
    // A singular canvas matrix means nothing is visible anyway; leaving the
    // transform un-inverted matches what stock does with the same failure.
}

// Stock's zoom: scale by z, then shift by half of what the scale removed from
// blurRect's size. It is written in terms of the rect's WIDTH and HEIGHT, not
// its centre, in both of stock's matrices, so it is reproduced that way rather
// than "corrected" — both children have to agree with each other and with
// stock, not with geometry.
SkMatrix zoomMatrix(const SkRect& blur_rect, float zoom) {
    return SkMatrix::MakeAll(zoom, 0, blur_rect.width() * (1.0f - zoom) * 0.5f,
                             0, zoom, blur_rect.height() * (1.0f - zoom) * 0.5f,
                             0, 0, 1);
}

}  // namespace

SkMatrix blurredTransform(const Bridge& b, const SkCanvas* canvas, const SkRect& blur_rect,
                          float zoom, float texel_x, float texel_y) {
    // MakeAll rather than SkMatrix::Scale/Translate: those are header-inline but
    // delegate to setScale/setTranslate, which are not, and resolving two more
    // symbols to write two constant matrices is a poor trade. MakeAll is inline
    // all the way down; it leaves the type mask unknown, and SurfaceFlinger's
    // own postConcat computes it — the mask is a cache, not state.
    // The texel is BlurFilter::kInverseInputScale (4 on AOSP) for the
    // compositor's own blur; see the header for the other values.
    SkMatrix m = SkMatrix::MakeAll(texel_x, 0, 0, 0, texel_y, 0, 0, 0, 1);
    const SkMatrix translate =
        SkMatrix::MakeAll(1, 0, blur_rect.fLeft, 0, 1, blur_rect.fTop, 0, 0, 1);
    b.MatrixPostConcat(&m, &translate);
    if (zoom != 1.0f) {
        // stock: matrix.postScale(z, z); matrix.postTranslate(w(1-z)/2, h(1-z)/2)
        const SkMatrix z = zoomMatrix(blur_rect, zoom);
        b.MatrixPostConcat(&m, &z);
    }
    undoCanvas(b, canvas, &m);
    return m;
}

SkMatrix sharpTransform(const Bridge& b, const SkCanvas* canvas, const SkRect& blur_rect,
                        float zoom) {
    // stock: inputMatrix = inverse(ctm); inputMatrix.preTranslate(w(1-z)/2,
    // h(1-z)/2); inputMatrix.preScale(z, z). Pre-concatenating T*S onto the
    // inverse is the same as starting from T*S and post-concatenating the
    // inverse, which needs only the postConcat this bridge already has.
    SkMatrix m = zoom != 1.0f ? zoomMatrix(blur_rect, zoom) : SkMatrix::MakeAll(1, 0, 0, 0, 1, 0, 0, 0, 1);
    undoCanvas(b, canvas, &m);
    return m;
}

}  // namespace skia
}  // namespace glass
