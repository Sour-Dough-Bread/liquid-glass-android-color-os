// The SurfaceFlinger / Skia entry points the engine resolves at load time.
//
// Each logical entry point carries a *list* of accepted manglings, newest
// first. Skia's templated signatures drift between Android releases in ways
// that change the mangled name without changing the ABI — Android 14 has
// `SkSpan<ChildPtr>` where Android 16 has `SkSpan<ChildPtr const>`, one `K`
// apart, and a span is {pointer, length} either way. A candidate list absorbs
// that; a single hard-coded string would make the engine silently
// version-locked.
//
// `prefix` exists to tell two different failures apart. No prefix hit means the
// method is gone or was inlined away. A prefix hit with no exact hit means the
// signature drifted into a form we have not vetted — the ABI may genuinely
// differ, so the hook must NOT be installed and the engine must fall through to
// stock behaviour.
//
// Two strategies remain, and only one of them is load-bearing:
//
//   kCore     everything the draw actually calls. Missing any of it means the
//             engine declines to install.
//   kBuilder  SkRuntimeEffectBuilder. Resolved and reported but NOT used: the
//             builder's uniform packing lives in its own object layout, while
//             SkRuntimeEffect::makeShader takes a plain SkData blob whose
//             offsets come from findUniform. Same result, far less ABI surface.
//             Kept resolved because Android 14 inlines most of the builder
//             away, and knowing that on a new target is worth one lookup.
//   kDataRelease
//             Two ways to drop a reference to the uniform blob. Unlike the
//             other strategies these are ALTERNATIVES — at least one has to
//             resolve, not all of them. Both are implicit template
//             instantiations, so whether either survives out-of-line is a
//             per-build accident: A16 has both, A14 has only the sk_sp
//             destructor. Requiring one specific instantiation would decline to
//             install on a build that inlined it, which is exactly what
//             happened on A14.
//   kHookTarget
//             The hook target comes in two shapes, split by QPR level rather
//             than by vendor. Android 14 and Android 16 QPR0 take
//             (uint32_t blurRadius, float blurAlpha); Android 16 QPR2 takes
//             (uint32_t, float blurScale, float blurAlpha), where blurScale
//             drives a scale-about-centre on the shader transform and 1.0 is
//             the no-op. Confirmed identical on two independent QPR2 ROMs for
//             two different devices, and absent from both QPR0 and A14, so this
//             is AOSP evolution and not a vendor patch. These are NOT candidates
//             for one entry: the argument lists genuinely differ, so each needs
//             its own replacement function. Exactly one is expected to resolve.
//
//             Worth knowing: on AArch64 and x86-64 SysV the extra float does
//             not shift the integer arguments, because floats live in their own
//             register file. The canvas, rect and image pointers stay put. What
//             would break is subtler — reading blurAlpha from the wrong float,
//             and calling the original with one float where it expects two.
//   kOptional
//             Members the engine can implement itself from the pinned headers,
//             but would much rather call. Measured: Android 16 inlined
//             SkCanvas::getTotalMatrix and SkPaint::setShader away entirely,
//             while Android 14 kept both as real symbols. Calling the target's
//             own copy means the engine depends on nothing but the signature;
//             falling back to its own means depending on a private object
//             layout (SkCanvas::MCRec, SkPaint::fShader). Prefer the symbol,
//             and treat the fallback as what it is — the riskier path, taken
//             only when there is no alternative.
//             SkRuntimeEffect::MakeForShader(SkString, const Options&) is
//             listed here too, the other way round: the core one-argument form
//             always works as a fallback, but this one is preferred, because
//             the one-argument form is a header-inline wrapper whose calling
//             convention a compiler may change (it did, on alioth).
//
// Separately from the strategy, an entry is marked `result_in_memory` when it
// returns a C++ object by value. On AArch64 the engine reads such a function's
// first instructions before calling it, and does not call it unless it takes
// the result address in x8 the standard way. See abi_check.h.
#pragma once

#include <cstddef>

namespace glass {
namespace sf {

enum Strategy {
    kCore = 0,      // ALL required; the engine refuses to install without them
    kHookTarget,    // ALTERNATIVES: exactly one shape of drawBlurRegion
    kDataRelease,   // ALTERNATIVES: at least one must resolve, not all
    kOptional,      // NONE required: the engine carries its own implementation
    kBuilder,       // resolved for information only, never called
    kBlurGenerate,  // NONE required: one feature (backdropBlur) needs any one
    kStrategyCount,
};

struct EntryPoint {
    const char* label;             // human-readable, for logs
    const char* const* manglings;  // nullptr-terminated, preference order
    const char* prefix;            // drift detection; may be nullptr
    Strategy strategy;
    // Returns a C++ object by value, so its caller passes the address to build
    // it at — in x8 on AArch64, if the compiler kept the standard convention.
    // The engine checks that it did before calling it (abi_check.h).
    bool result_in_memory = false;
};

extern const EntryPoint kEntryPoints[];
extern const size_t kEntryPointCount;

// Indices into kEntryPoints, so callers do not string-match at runtime.
enum Index : size_t {
    // ---- kHookTarget: which shape of drawBlurRegion this build has
    kDrawBlurRegion = 0,
    kDrawBlurRegionScaled,

    // ---- compiling the shader
    kMakeForShader,
    kMakeForShaderOpts,
    kSkStringCtor,
    kSkStringDtor,
    kFindUniform,
    kFindChild,
    kEffectUniformSize,

    // ---- building the shader
    kImageMakeShader,
    kDataMakeZeroInit,
    kEffectMakeShader,

    // ---- releasing the uniform blob (alternatives, see kDataRelease)
    kSkSpDataDtor,
    kDataUnref,

    // ---- drawing it
    kMatrixPostConcat,
    kMatrixInvertNonIdentity,
    kPaintReset,
    kPaintDtor,
    kCanvasDrawRect,

    // ---- kOptional (the engine defines these itself if absent)
    kCanvasGetTotalMatrix,
    kPaintSetShader,

    // ---- kBlurGenerate: BlurFilter::generateTemporaryImage per filter (A17)
    kGenerateKawase,
    kGenerateGaussian,
    kGenerateKawaseDualV2,

    // ---- kBuilder
    kBuilderCtor,
    kBuilderDtor,
    kBuilderWritableUniformData,
    kBuilderMakeShader,
    kBuilderUniformSetFloat,

    kCount,
};

const char* strategyName(Strategy s);

}  // namespace sf
}  // namespace glass
