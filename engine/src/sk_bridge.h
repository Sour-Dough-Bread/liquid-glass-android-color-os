// Calling into SurfaceFlinger's statically-linked Skia.
//
// Nothing is linked against Skia. Every entry point is a pointer resolved at
// runtime from the target's own MiniDebugInfo, and every signature is modelled
// in terms of POINTERS ONLY.
//
// That last part is the load-bearing rule, learned the expensive way. Declaring
// a pointer as `Result (*)(SkString)` and calling it with an SkString looks
// natural and is ABI-correct — but it makes clang emit a call to
// SkString::SkString(const SkString&) to build the by-value argument, and that
// symbol exists only inside surfaceflinger's non-exported code. The result is
// an undefined symbol in this .so, which the runtime linker cannot resolve, so
// surfaceflinger becomes unlinkable and the device bootloops before any of our
// code runs:
//
//   CANNOT LINK EXECUTABLE "/system/bin/surfaceflinger": cannot locate symbol
//   "_ZN8SkStringC1ERKS_" referenced by "/system/lib64/libglassengine.so"
//
// So: under the Itanium C++ ABI a type that is "non-trivial for the purposes of
// calls" (non-trivial copy ctor, move ctor or destructor — sk_sp, SkString and
// anything containing them) is passed as a POINTER to a caller-owned temporary.
// Writing that pointer out explicitly is exactly the same machine code with none
// of the emitted calls. `build.sh` links with -Wl,--no-undefined so any
// regression is a build error rather than a bootloop.
//
// RETURNING such a type is different, and differs by architecture. The caller
// supplies the address to construct the result at, but:
//
//   x86-64 SysV   as a hidden FIRST argument, in rdi; `this` and the rest shift
//   AArch64       in x8, a register of its own; `this` stays in x0
//
// So `void f(void* sret, ...)` is only right on x86-64. On arm64 it moves every
// argument one register along and leaves the callee writing its result through
// whatever x8 happened to hold — measured in this file's own compiled output,
// where x8 held the address of the function being called. Every by-value return
// is therefore declared as Returned<N> below, and the compiler places the
// pointer wherever the target ABI wants it.
//
// Trivially-copyable types are exempt and are declared naturally: SkRect,
// SkMatrix, SkTileMode, std::string_view and SkSpan all pass and return in
// registers with no compiler-generated helpers.
//
// Two members are declared by the pinned headers but have NO symbol in the
// target because the optimizer inlined every call site — they are defined by
// this module instead; see sk_bridge.cpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "include/core/SkCanvas.h"
#include "include/core/SkData.h"
#include "include/core/SkImage.h"
#include "include/core/SkMatrix.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRRect.h"
#include "include/core/SkSamplingOptions.h"
#include "include/effects/SkRuntimeEffect.h"

namespace glass {
namespace skia {

// The return slot for a function that returns sk_sp<T>, SkString or
// SkRuntimeEffect::Result by value: same size, and non-trivial for the purposes
// of calls (user-provided destructor, no copy), so it is returned indirectly by
// exactly the mechanism the real type is. The bytes are whatever the callee
// constructed there; reading them is the caller's job. Nothing here is ever
// copied or destroyed in a way that emits a call: the destructor is empty and
// inline, and C++17 guarantees the result is constructed in place.
template <size_t N>
struct Returned {
    alignas(alignof(void*)) unsigned char bytes[N];
    Returned() {}
    // A returned sk_sp<T> holding `p`, whose one reference the caller of the
    // function this is returned from takes over. For a hook that answers in
    // the original's place; a prvalue of it is built straight into the
    // caller's return slot, like the original's own result.
    explicit Returned(void* p) {
        static_assert(N == sizeof(void*), "only an sk_sp is one pointer");
        memcpy(bytes, &p, sizeof(p));
    }
    ~Returned() {}
    Returned(const Returned&) = delete;
    Returned& operator=(const Returned&) = delete;

    // For a returned sk_sp<T>: the pointer it holds, whose reference is now the
    // caller's.
    void* pointer() const {
        void* p;
        memcpy(&p, bytes, sizeof(p));
        return p;
    }
};
using ReturnedSkSp = Returned<sizeof(void*)>;
static_assert(sizeof(ReturnedSkSp) == sizeof(sk_sp<SkShader>), "sk_sp is one pointer");

// Layout facts taken from the vendored headers, used only for sizing raw
// storage and reading plain data members — never to call anything.
//
//   SkRuntimeEffect::Result { sk_sp<SkRuntimeEffect> effect; SkString errorText; }
//   SkString                { sk_sp<Rec> fRec; }
//   SkString::Rec           { uint32_t fLength; atomic<int32_t> fRefCnt; char data[]; }
constexpr size_t kResultSize = sizeof(SkRuntimeEffect::Result);
constexpr size_t kSkStringSize = sizeof(SkString);
constexpr size_t kResultErrorTextOffset = sizeof(void*);  // after sk_sp<SkRuntimeEffect>
constexpr size_t kRecDataOffset = 8;                      // fLength(4) + fRefCnt(4)

struct Bridge {
    // ---- compiling ------------------------------------------------------
    // Result MakeForShader(SkString)        (SkString by value -> pointer to temporary)
    Returned<kResultSize> (*MakeForShader)(void* skstring_byval) = nullptr;
    // Result MakeForShader(SkString, const Options&): the real definition, and
    // the one used when it resolves. The one above is a header-inline wrapper
    // around it, and on the arm64 A17 builds measured its out-of-line copy takes
    // the result address in x0 instead of x8 (abi_check.h).
    Returned<kResultSize> (*MakeForShaderOpts)(void* skstring_byval,
                                               const SkRuntimeEffect::Options* opts) = nullptr;
    void (*SkStringCtor)(void* self, const char* text) = nullptr;
    void (*SkStringDtor)(void* self) = nullptr;
    const SkRuntimeEffect::Uniform* (*findUniform)(const void* effect,
                                                   std::string_view name) = nullptr;
    const SkRuntimeEffect::Child* (*findChild)(const void* effect,
                                               std::string_view name) = nullptr;
    size_t (*uniformSize)(const void* effect) = nullptr;

    // ---- building -------------------------------------------------------
    // sk_sp<SkShader> SkImage::makeShader(SkTileMode, SkTileMode,
    //                                     const SkSamplingOptions&, const SkMatrix*) const
    ReturnedSkSp (*ImageMakeShader)(const void* image, SkTileMode tx, SkTileMode ty,
                                    const SkSamplingOptions* sampling,
                                    const SkMatrix* local_matrix) = nullptr;

    // sk_sp<SkData> SkData::MakeZeroInitialized(size_t)
    ReturnedSkSp (*DataMakeZeroInit)(size_t length) = nullptr;

    // Two ways to drop a reference to the uniform blob; at least one resolves.
    // The destructor takes the SLOT holding the pointer, unref takes the OBJECT
    // — hence two fields rather than one. See releaseData().
    void (*SkSpDataDtor)(void* sksp_slot) = nullptr;   // sk_sp<SkData>::~sk_sp()
    void (*DataUnref)(const void* data) = nullptr;     // SkNVRefCnt<SkData>::unref()

    // sk_sp<SkShader> SkRuntimeEffect::makeShader(sk_sp<const SkData>,
    //                                             SkSpan<const ChildPtr>,
    //                                             const SkMatrix*) const
    //
    // SkSpan is {pointer, length} and trivially copyable, so it occupies two
    // ordinary argument slots rather than one — spelling them out separately is
    // what makes this correct on both AArch64 and x86-64.
    ReturnedSkSp (*EffectMakeShader)(const void* effect, void* sksp_data_byval,
                                     const void* children, size_t child_count,
                                     const SkMatrix* local_matrix) = nullptr;

    // ---- drawing --------------------------------------------------------
    // Returns SkMatrix&, i.e. a pointer we have no use for.
    void (*MatrixPostConcat)(SkMatrix* self, const SkMatrix* other) = nullptr;
    bool (*MatrixInvertNonIdentity)(const SkMatrix* self, SkMatrix* inverse) = nullptr;
    void (*PaintReset)(void* paint) = nullptr;
    void (*PaintDtor)(void* paint) = nullptr;
    void (*CanvasDrawRect)(void* canvas, const SkRect* rect, const void* paint) = nullptr;

    // ---- optional: present on A14, inlined away on A16 -------------------
    // When these resolve the engine calls them and depends on nothing but the
    // signature. When they do not, it uses its own definitions, which depend on
    // SkCanvas::MCRec's and SkPaint's private layouts. Prefer the symbol.
    // SkMatrix is trivially copyable but 40 bytes, so it too comes back through
    // memory (x8 / rdi); declaring the real type lets the compiler do that.
    SkMatrix (*CanvasGetTotalMatrix)(const void* canvas) = nullptr;
    void (*PaintSetShader)(void* paint, void* sksp_shader_byval) = nullptr;

    // ---- optional: a copy of part of an image (Android 17) -----------------
    // sk_sp<SkImage> SkImage_Base::makeSubset(SkRecorder*, const SkIRect&,
    //                                         SkImage::RequiredProperties) const
    // RequiredProperties is one bool in a struct, trivially copyable, so it
    // travels in one integer register on both ABIs, as any struct of up to
    // eight bytes does. This twin fills that register with zeroes: the bool
    // the callee reads is false, and so would be any flag a later Skia adds
    // beside it.
    struct SubsetProperties {
        bool mipmapped = false;
        unsigned char reserved[7] = {0, 0, 0, 0, 0, 0, 0};
    };
    static_assert(sizeof(SubsetProperties) == 8, "one register, all of it");
    ReturnedSkSp (*ImageMakeSubset)(const void* image, void* recorder, const SkIRect* subset,
                                    SubsetProperties properties) = nullptr;
    // SkRecorder* SkCanvas::baseRecorder() const
    void* (*CanvasBaseRecorder)(const void* canvas) = nullptr;

    bool compileReady() const {
        return (MakeForShader || MakeForShaderOpts) && SkStringCtor && SkStringDtor &&
               findUniform && findChild &&
               uniformSize;
    }
    bool drawReady() const {
        // MatrixInvertNonIdentity is deliberately absent: some builds inline it
        // away and the engine inverts the matrix itself there.
        return compileReady() && ImageMakeShader && DataMakeZeroInit &&
               (SkSpDataDtor || DataUnref) && EffectMakeShader && MatrixPostConcat &&
               PaintReset && PaintDtor && CanvasDrawRect;
    }
};

// Compiles SkSL using SurfaceFlinger's own Skia.
//
// Returns the effect with one owned reference, deliberately never released: it
// is cached for the process lifetime, so there is nothing to free and no need
// to reach for sk_sp's destructor. Returns nullptr on failure, with the SkSL
// compiler's diagnostics in `error` — losing those would make a shader that
// fails only inside the compositor almost undebuggable.
SkRuntimeEffect* compile(const Bridge& b, const char* sksl, std::string* error);

// canvas->getTotalMatrix() and paint->setShader(), through the target's own copy
// when it has one. Written as free functions rather than called as members
// because the engine *also* defines those members (see sk_bridge.cpp), and a
// member call would statically bind to its own definition and silently never
// use the resolved symbol.
SkMatrix canvasTotalMatrix(const Bridge& b, const SkCanvas* canvas);
bool invertMatrix(const Bridge& b, const SkMatrix& m, SkMatrix* out);
void paintSetShader(const Bridge& b, SkPaint* paint, SkShader* shader);

// Drops the engine's reference to a uniform blob and clears the slot. Safe on a
// slot that SkRuntimeEffect::makeShader already moved out of, which is the
// normal case — the destructor tolerates null and unref is guarded here.
inline void releaseData(const Bridge& b, void** slot) {
    if (b.SkSpDataDtor != nullptr) {
        b.SkSpDataDtor(slot);       // handles a null slot itself
    } else if (*slot != nullptr) {
        b.DataUnref(*slot);
    }
    *slot = nullptr;
}

// Local matrix for the BLURRED child. BlurFilter::kInputScale is 0.25, so it
// needs a x4 scale, then the blurRect offset (the blurred image covers only
// blurRect), then the zoom, then the inverse canvas matrix. Reproduces
// BlurFilter.cpp's file-static getShaderTransform() exactly, read out of the
// target's own compiled code and, for the zoom, AOSP android17-release.
//
// `zoom` is drawBlurRegion's zoomScale (A16 QPR2 onward); 1 everywhere else.
//
// `texel` is how many output pixels one pixel of the image covers: 4 for a blur
// of the snapshot, 16 for a blur made from such a blur (a pane's, derived from
// the backdrop's). Every blur filter maps by that scale from blurRect's top
// left, whatever size its image came out at.
SkMatrix blurredTransform(const Bridge& b, const SkCanvas* canvas, const SkRect& blur_rect,
                          float zoom, float texel = 4.0f);

// Local matrix for the SHARP child. Not the same recipe with scale 1: the sharp
// input is a snapshot of the whole surface, already in device space at the
// origin, so it takes the inverse canvas matrix and the zoom but NOT the
// blurRect offset. Stock's crossfade builds exactly this (`inputMatrix`).
SkMatrix sharpTransform(const Bridge& b, const SkCanvas* canvas, const SkRect& blur_rect,
                        float zoom);

// An SkPaint built by SurfaceFlinger's own reset(), on our stack.
//
// SkPaint's default constructor was inlined away in the target, so there is no
// symbol to call. reset() survived, and it is documented as "equivalent to
// replacing SkPaint with the result of SkPaint()". Zeroing first is what makes
// that safe: reset() assigns over six sk_sp members, and assigning over a null
// one is a no-op where assigning over garbage would free a wild pointer.
class ScopedPaint {
public:
    explicit ScopedPaint(const Bridge& b) : b_(b) {
        memset(storage_, 0, sizeof(storage_));
        b_.PaintReset(storage_);
    }
    ~ScopedPaint() { b_.PaintDtor(storage_); }
    ScopedPaint(const ScopedPaint&) = delete;
    ScopedPaint& operator=(const ScopedPaint&) = delete;

    SkPaint* get() { return reinterpret_cast<SkPaint*>(storage_); }

private:
    const Bridge& b_;

    // The engine never reads or writes an SkPaint field — the target constructs
    // it (reset), configures it (setShader), consumes it (drawRect) and destroys
    // it (~SkPaint). So the ONLY layout fact required is that this buffer is at
    // least as large as the target's SkPaint, and the headroom buys tolerance
    // for a build whose SkPaint grew a field or two. Undersizing it would let
    // reset() write past the end of a stack frame inside SurfaceFlinger, which
    // is about the worst failure mode available here; 48 spare bytes of stack is
    // not a price worth haggling over.
    //
    // Measured 80 bytes on both A14 and A16, with identical offsets
    // (fColor4f@0x30, fWidth@0x40, fMiterLimit@0x44) — see docs/VERIFIED.md.
    static constexpr size_t kPaintHeadroom = 48;
    alignas(16) unsigned char storage_[sizeof(SkPaint) + kPaintHeadroom];
};

}  // namespace skia
}  // namespace glass
