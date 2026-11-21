// The module's half of the bridge (sk_bridge.h): two Skia members its target
// has no symbol for, and compile() through the resolved entry points. A ROM
// that builds the glass in links Skia's own members and compiles with
// SkRuntimeEffect::MakeForShader (rom/glue/glass/skia_native.cpp), so it
// never builds this file. The helpers both use are in sk_shared.cpp.
#include "sk_bridge.h"

#include <string>
#include <type_traits>
#include <utility>

// ---------------------------------------------------------------------------
// Members that the target has no symbol for.
//
// SkCanvas::getTotalMatrix and SkPaint::setShader are declared by the pinned
// headers, but the optimizer inlined every call site in the SurfaceFlinger
// binary and dropped the out-of-line copy — measured on both the Android 14 and
// Android 16 images, see docs/VERIFIED.md. There is nothing to resolve, so they
// are defined here instead.
//
// This is an out-of-class definition of a member the header declares, which is
// ordinary C++ and comes with full access to the private members involved. It
// is emphatically NOT a layout guess: `MCRec` is defined in SkCanvas.h and
// `fShader` in SkPaint.h, so both bodies and every offset they touch come from
// the same pinned tree the ROM's Skia was built from.
//
// Both were cross-checked against the target's own compiled drawBlurRegion:
// its inlined getTotalMatrix() loads fMCRec at +0xc40, reads nine floats from
// fMatrix at +0x18 in the order SkM44::asM33 specifies, and stores them with a
// kUnknown type mask — which is exactly what this expression compiles to.
SkMatrix SkCanvas::getTotalMatrix() const {
    return fMCRec->fMatrix.asM33();
}

void SkPaint::setShader(sk_sp<SkShader> shader) {
    fShader = std::move(shader);
}
// ---------------------------------------------------------------------------

namespace glass {
namespace skia {

SkRuntimeEffect* compile(const Bridge& b, const char* sksl, std::string* error) {
    if (!b.compileReady() || sksl == nullptr) return nullptr;

    alignas(alignof(void*)) unsigned char src[kSkStringSize];
    b.SkStringCtor(src, sksl);

    // Options{} is what the header's own wrapper passes: every field at its
    // default. Built from the pinned header, so it is the target's layout
    // (alioth's wrapper zeroes 40 bytes and reads allowPrivateAccess at +24).
    //
    // Every byte has to be zero, padding included. Android 15's Skia reads its
    // own 12-byte Options out of these bytes, with allowPrivateAccess at +1 and
    // fStableKey at +4, both in what is padding here (Nothing OS 3.2 and Pixel
    // A15, read from their MakeForShader), and zero is every field's default
    // in every layout. Options is not an aggregate (it has private members),
    // so Options{} value-initializes: the object is zero-initialized, padding
    // and all, before the defaults go in. The assert keeps it that way.
    static_assert(sizeof(SkRuntimeEffect::Options) == 40, "Options layout moved; re-check");
    static_assert(!std::is_aggregate_v<SkRuntimeEffect::Options>,
                  "Options{} must value-initialize, so its padding is zeroed");
    const SkRuntimeEffect::Options options{};
    // Each return is a prvalue, so the result is built straight into `result`
    // by guaranteed elision; Returned cannot be copied.
    auto make = [&]() -> Returned<kResultSize> {
        if (b.MakeForShaderOpts != nullptr) return b.MakeForShaderOpts(src, &options);
        return b.MakeForShader(src);
    };
    Returned<kResultSize> result = make();
    unsigned char* res = result.bytes;

    // The by-value argument is caller-owned under this ABI, so it is ours to
    // destroy regardless of what happened inside.
    b.SkStringDtor(src);

    auto* effect = *reinterpret_cast<SkRuntimeEffect**>(res);
    void* err_slot = res + kResultErrorTextOffset;

    if (effect == nullptr && error != nullptr) {
        void* rec = *reinterpret_cast<void**>(err_slot);
        if (rec != nullptr) {
            const auto len = *reinterpret_cast<const uint32_t*>(rec);
            const char* data = reinterpret_cast<const char*>(rec) + kRecDataOffset;
            error->assign(data, len);
        } else {
            error->assign("(no diagnostics)");
        }
    }

    b.SkStringDtor(err_slot);
    return effect;
}

}  // namespace skia
}  // namespace glass
