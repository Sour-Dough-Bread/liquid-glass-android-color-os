#include "sf_symbols.h"

namespace glass {
namespace sf {
namespace {

// ---- SurfaceFlinger ------------------------------------------------------
// The hook target. Byte-identical on Android 14 and Android 16 QPR0.
const char* kDrawBlurRegionM[] = {
    "_ZN7android12renderengine4skia10BlurFilter14drawBlurRegionEP8SkCanvasRK7SkRRectjfRK6SkRect"
    "5sk_spI7SkImageESD_",
    nullptr,
};
// The same function with an extra leading float. This is the Android 16 QPR2
// shape: byte-identical on Project Infinity-X 3.12 (beryllium) and LineageOS
// 23.2 (alioth), and absent from Android 16 QPR0 and Android 14.
// Reading its compiled code: the SECOND float is
// blurAlpha — it is clamped to [0,1] and stored into the paint — while the
// first builds SkMatrix::Scale(s,s), postConcats it into the shader transform
// and offsets by (1-s)*size*0.5, i.e. a scale about the region's centre, with a
// `s == 1.0` early-out confirming 1.0 as the default.
const char* kDrawBlurRegionScaledM[] = {
    "_ZN7android12renderengine4skia10BlurFilter14drawBlurRegionEP8SkCanvasRK7SkRRectjffRK6SkRect"
    "5sk_spI7SkImageESD_",
    nullptr,
};
// Evolution X 12.x (Android 17): the shape above and `int kind`.
const char* kDrawBlurRegionScaledKindM[] = {
    "_ZN7android12renderengine4skia10BlurFilter14drawBlurRegionEP8SkCanvasRK7SkRRectjffRK6SkRect"
    "5sk_spI7SkImageESD_i",
    nullptr,
};

// ---- SkRuntimeEffect -----------------------------------------------------
const char* kMakeForShaderM[] = {
    "_ZN15SkRuntimeEffect13MakeForShaderE8SkString",
    nullptr,
};
// The real definition, which the one-argument form is a header-inline wrapper
// around. Preferred: being inline, the wrapper's out-of-line copy is exactly
// the kind of function a compiler may re-convention, and on alioth's crDroid
// A17 it did (result address in x0, not x8). The engine uses this one when it
// resolves and checks out, and the wrapper only otherwise.
const char* kMakeForShaderOptsM[] = {
    "_ZN15SkRuntimeEffect13MakeForShaderE8SkStringRKNS_7OptionsE",
    nullptr,
};

// A16 takes SkSpan<ChildPtr const>, A14 takes SkSpan<ChildPtr>. Same ABI.
const char* kEffectMakeShaderM[] = {
    "_ZNK15SkRuntimeEffect10makeShaderE5sk_spIK6SkDataE6SkSpanIKNS_8ChildPtrEEPK8SkMatrix",
    "_ZNK15SkRuntimeEffect10makeShaderE5sk_spIK6SkDataE6SkSpanINS_8ChildPtrEEPK8SkMatrix",
    nullptr,
};

// SkRuntimeEffect::uniforms() is header-inline and has no symbol, so the total
// blob size cannot be summed from the reflection. This returns it directly.
const char* kEffectUniformSizeM[] = {
    "_ZNK15SkRuntimeEffect11uniformSizeEv",
    nullptr,
};

// SkRuntimeEffect::uniform(name) on the builder is header-inline, but the
// findUniform/findChild it delegates to are not. Calling those directly means
// the engine never has to reproduce Skia's uniform packing rules: each Uniform
// carries its own offset.
const char* kFindUniformM[] = {
    "_ZNK15SkRuntimeEffect11findUniformENSt3__117basic_string_viewIcNS0_11char_traitsIcEEEE",
    nullptr,
};
const char* kFindChildM[] = {
    "_ZNK15SkRuntimeEffect9findChildENSt3__117basic_string_viewIcNS0_11char_traitsIcEEEE",
    nullptr,
};

// ---- SkString ------------------------------------------------------------
// MakeForShader takes SkString BY VALUE, so one must be constructed and
// destroyed around the call. C1/C2 and D1/D2 are the complete/base variants;
// for a class with no virtual bases they are the same code, but which one the
// compiler emitted varies, so accept either.
const char* kSkStringCtorM[] = {
    "_ZN8SkStringC1EPKc",
    "_ZN8SkStringC2EPKc",
    nullptr,
};
const char* kSkStringDtorM[] = {
    "_ZN8SkStringD1Ev",
    "_ZN8SkStringD2Ev",
    nullptr,
};

// ---- SkData --------------------------------------------------------------
// The uniform blob. Zero-initialized rather than uninitialized because a
// uniform the engine forgets to write then reads as 0 instead of as stack
// garbage, which is the difference between a wrong-looking pane and a
// nondeterministic one.
const char* kDataMakeZeroInitM[] = {
    "_ZN6SkData19MakeZeroInitializedEm",
    nullptr,
};
// SkData is allocated in-place and declares a private operator delete, so
// sk_sp<SkData>'s inline destructor cannot be instantiated outside Skia. The
// engine therefore holds a raw SkData* and releases it through one of these.
//
// They are alternatives, not a candidate list: the argument differs. The sk_sp
// destructor takes the SLOT holding the pointer (and tolerates a null one);
// unref takes the OBJECT. A candidate list is for one ABI spelled several ways,
// so mixing these two would break the very invariant that makes it safe.
const char* kSkSpDataDtorM[] = {
    "_ZN5sk_spI6SkDataED2Ev",
    "_ZN5sk_spI6SkDataED1Ev",
    nullptr,
};
const char* kDataUnrefM[] = {
    "_ZNK10SkNVRefCntI6SkDataE5unrefEv",
    nullptr,
};

// ---- Skia drawing --------------------------------------------------------
const char* kImageMakeShaderM[] = {
    "_ZNK7SkImage10makeShaderE10SkTileModeS0_RK17SkSamplingOptionsPK8SkMatrix",
    nullptr,
};
const char* kCanvasDrawRectM[] = {
    "_ZN8SkCanvas8drawRectERK6SkRectRK7SkPaint",
    nullptr,
};

// ---- SkMatrix ------------------------------------------------------------
// getShaderTransform() composes Scale, Translate and the inverse canvas matrix.
// SkMatrix::Scale/Translate are header-inline (they set fMat and the type mask
// directly); these two are not.
const char* kMatrixPostConcatM[] = {
    "_ZN8SkMatrix10postConcatERKS_",
    nullptr,
};
// SkMatrix::invert() is header-inline and forwards to this for anything that is
// not the identity. Calling it directly also skips getType(), which would
// otherwise drag in the out-of-line computeTypeMask().
const char* kMatrixInvertNonIdentityM[] = {
    "_ZNK8SkMatrix17invertNonIdentityEPS_",
    nullptr,
};

// ---- SkPaint -------------------------------------------------------------
// SkPaint's default constructor and every setter except setBlendMode were
// inlined away in both the A14 and A16 binaries — measured, see VERIFIED.md.
// reset() survived, and its contract is "equivalent to replacing SkPaint with
// the result of SkPaint()", so zeroed storage plus reset() gives a paint built
// by SurfaceFlinger's own code with no layout assumptions on our side.
const char* kPaintResetM[] = {
    "_ZN7SkPaint5resetEv",
    nullptr,
};
const char* kPaintDtorM[] = {
    "_ZN7SkPaintD1Ev",
    "_ZN7SkPaintD2Ev",
    nullptr,
};

// ---- kOptional -----------------------------------------------------------
// Present on Android 14, inlined away on Android 16. When present the engine
// calls them and depends on nothing but the signature; when absent it falls
// back to its own definition, which depends on a private object layout.
const char* kCanvasGetTotalMatrixM[] = {
    "_ZNK8SkCanvas14getTotalMatrixEv",
    nullptr,
};
const char* kPaintSetShaderM[] = {
    "_ZN7SkPaint9setShaderE5sk_spI8SkShaderE",
    nullptr,
};

// ---- a copy of part of the snapshot ---------------------------------------
// Android 17's Skia takes the context through an SkRecorder, which the canvas
// hands out. Earlier releases spell makeSubset with a GrDirectContext* and are
// not matched: the copy is simply not made there.
const char* kImageMakeSubsetM[] = {
    "_ZNK12SkImage_Base10makeSubsetEP10SkRecorderRK7SkIRectN7SkImage18RequiredPropertiesE",
    nullptr,
};
const char* kCanvasBaseRecorderM[] = {
    "_ZNK8SkCanvas12baseRecorderEv",
    nullptr,
};
// skgpu::graphite::Recorder* SkCanvas::recorder() const: the top device's
// recorder, which only a Graphite device has. Out of line from Android 15 to
// 17 on every build surveyed.
const char* kCanvasRecorderM[] = {
    "_ZNK8SkCanvas8recorderEv",
    nullptr,
};
// A RenderEngine that can keep a layer's blur from one frame to the next and
// skip generateTemporaryImage on the frames after. Never called: a snapshot
// handed back in a blur's place would be kept as that layer's blur, so where
// one of these exists nothing is put off. Any one of them marks it:
// - bool SkiaRenderEngine::isBlurCacheValid(BlurCacheKey const&, SkRect const&,
//   long, long) const: Pixel CP3A on, switched on by lower_blur_fps from CP4A.
// - Infinity-X 4.x (Android 17; AxionOS's ax-dragonite-dev) keeps the last four
//   generateTemporaryImage results in a std::deque<BlurCacheEntry>, looked up
//   and stored by lambdas inside drawLayersInternal: no method of its own, but
//   the deque's members are out of line on tokay and a53x.
// The draw hook also notices a handed-back snapshot coming back (blurSlotFor),
// for a cache none of these shows.
const char* kBlurCacheValidM[] = {
    "_ZNK7android12renderengine4skia16SkiaRenderEngine16isBlurCacheValidERKNS1_12BlurCacheKeyE"
    "RK6SkRectll",
    "_ZNSt3__15dequeIN7android12renderengine4skia16SkiaRenderEngine14BlurCacheEntryE"
    "NS_9allocatorIS5_EEE10push_frontEOS5_",
    "_ZNSt3__15dequeIN7android12renderengine4skia16SkiaRenderEngine14BlurCacheEntryE"
    "NS_9allocatorIS5_EEE5clearEv",
    "_ZNSt3__15dequeIN7android12renderengine4skia16SkiaRenderEngine14BlurCacheEntryE"
    "NS_9allocatorIS5_EEE8pop_backEv",
    nullptr,
};

// ---- kBlurGenerate -------------------------------------------------------
// The virtual that produces a blur, one implementation per filter. Hooked only
// to learn the arguments it was called with (the GPU context and the display),
// which drawBlurRegion is never given, so that a toned backdrop can ask the
// same filter for the same blur at a lower radius. Android 17 named it
// generateTemporaryImage and added the DisplaySettings; earlier releases spell
// it generate() with a different argument list, and are simply not matched —
// backdropBlur then keeps the requested radius.
const char* kGenerateKawaseM[] = {
    "_ZNK7android12renderengine4skia16KawaseBlurFilter22generateTemporaryImageEPNS1_"
    "14SkiaGpuContextERKNS0_15DisplaySettingsEj5sk_spI7SkImageERK6SkRect",
    nullptr,
};
const char* kGenerateGaussianM[] = {
    "_ZNK7android12renderengine4skia18GaussianBlurFilter22generateTemporaryImageEPNS1_"
    "14SkiaGpuContextERKNS0_15DisplaySettingsEj5sk_spI7SkImageERK6SkRect",
    nullptr,
};
const char* kGenerateKawaseDualV2M[] = {
    "_ZNK7android12renderengine4skia22KawaseBlurDualFilterV222generateTemporaryImageEPNS1_"
    "14SkiaGpuContextERKNS0_15DisplaySettingsEj5sk_spI7SkImageERK6SkRect",
    nullptr,
};
// Project Infinity-X 4.x (Android 17) removed AOSP's Kawase filters for this
// one ("adding glass blur filter v2"; its header credits AxionOS). Same
// signature as AOSP's.
const char* kGenerateGlassM[] = {
    "_ZNK7android12renderengine4skia15GlassBlurFilter22generateTemporaryImageEPNS1_"
    "14SkiaGpuContextERKNS0_15DisplaySettingsEj5sk_spI7SkImageERK6SkRect",
    nullptr,
};
// Evolution X 12.x (Android 17) and Lunaris 3.13 (Android 16 QPR2) ship a blur
// algorithm beside AOSP's ("Kawase Darkmoon",
// debug.renderengine.blur_algorithm=kawase_darkmoon; EvoX picks it by default
// on low-RAM devices). Same signature as AOSP's.
const char* kGenerateDarkmoonM[] = {
    "_ZNK7android12renderengine4skia14KawaseDarkmoon22generateTemporaryImageEPNS1_"
    "14SkiaGpuContextERKNS0_15DisplaySettingsEj5sk_spI7SkImageERK6SkRect",
    nullptr,
};

// ---- SkRuntimeEffectBuilder ---------------------------------------------
// Resolved but never called; see the header for why. Android 14 spells the
// shader-specific builder SkRuntimeShaderBuilder, which Android 16 folded into
// SkRuntimeEffectBuilder — same role, same ABI, different name, which is exactly
// what a candidate list is for. Without the A14 spellings this table would
// report "partly inlined" on a build that in fact has the whole surface.
const char* kBuilderCtorM[] = {
    "_ZN22SkRuntimeEffectBuilderC1E5sk_spI15SkRuntimeEffectE",
    "_ZN22SkRuntimeEffectBuilderC2E5sk_spI15SkRuntimeEffectE",
    "_ZN22SkRuntimeShaderBuilderC1E5sk_spI15SkRuntimeEffectE",
    "_ZN22SkRuntimeShaderBuilderC2E5sk_spI15SkRuntimeEffectE",
    nullptr,
};
const char* kBuilderDtorM[] = {
    "_ZN22SkRuntimeEffectBuilderD1Ev",
    "_ZN22SkRuntimeEffectBuilderD2Ev",
    "_ZN22SkRuntimeShaderBuilderD1Ev",
    "_ZN22SkRuntimeShaderBuilderD2Ev",
    nullptr,
};
const char* kBuilderWritableUniformDataM[] = {
    "_ZN22SkRuntimeEffectBuilder19writableUniformDataEv",
    nullptr,
};
const char* kBuilderMakeShaderM[] = {
    "_ZNK22SkRuntimeEffectBuilder10makeShaderEPK8SkMatrix",
    "_ZN22SkRuntimeShaderBuilder10makeShaderEPK8SkMatrix",
    nullptr,
};
const char* kBuilderUniformSetFloatM[] = {
    "_ZN22SkRuntimeEffectBuilder14BuilderUniform3setIfEEbPKT_i",
    nullptr,
};

}  // namespace

const EntryPoint kEntryPoints[] = {
    {"BlurFilter::drawBlurRegion", kDrawBlurRegionM,
     "_ZN7android12renderengine4skia10BlurFilter14drawBlurRegion", kHookTarget},
    {"BlurFilter::drawBlurRegion (scaled)", kDrawBlurRegionScaledM, nullptr, kHookTarget},
    {"BlurFilter::drawBlurRegion (scaled, material)", kDrawBlurRegionScaledKindM, nullptr,
     kHookTarget},

    // ---- compiling the shader --------------------------------------------
    {"SkRuntimeEffect::MakeForShader", kMakeForShaderM,
     "_ZN15SkRuntimeEffect13MakeForShader", kCore, true},
    {"SkRuntimeEffect::MakeForShader(Options)", kMakeForShaderOptsM, nullptr, kOptional, true},
    {"SkString::SkString(const char*)", kSkStringCtorM, "_ZN8SkStringC", kCore},
    {"SkString::~SkString", kSkStringDtorM, "_ZN8SkStringD", kCore},
    {"SkRuntimeEffect::findUniform", kFindUniformM, "_ZNK15SkRuntimeEffect11findUniform",
     kOptional},
    {"SkRuntimeEffect::findChild", kFindChildM, "_ZNK15SkRuntimeEffect9findChild", kOptional},
    {"SkRuntimeEffect::uniformSize", kEffectUniformSizeM,
     "_ZNK15SkRuntimeEffect11uniformSize", kCore},

    // ---- building the shader ---------------------------------------------
    {"SkImage::makeShader", kImageMakeShaderM, "_ZNK7SkImage10makeShaderE10SkTileMode", kCore,
     true},
    {"SkData::MakeZeroInitialized", kDataMakeZeroInitM, "_ZN6SkData19MakeZeroInitialized", kCore,
     true},
    {"SkRuntimeEffect::makeShader", kEffectMakeShaderM,
     "_ZNK15SkRuntimeEffect10makeShader", kCore, true},

    // ---- kDataRelease (alternatives; at least one) ------------------------
    {"sk_sp<SkData>::~sk_sp", kSkSpDataDtorM, "_ZN5sk_spI6SkDataED", kDataRelease},
    {"SkNVRefCnt<SkData>::unref", kDataUnrefM, "_ZNK10SkNVRefCntI6SkDataE5unref", kDataRelease},

    // ---- drawing it ------------------------------------------------------
    {"SkMatrix::postConcat", kMatrixPostConcatM, "_ZN8SkMatrix10postConcat", kCore},
    {"SkMatrix::invertNonIdentity", kMatrixInvertNonIdentityM,
     "_ZNK8SkMatrix17invertNonIdentity", kOptional},
    {"SkPaint::reset", kPaintResetM, "_ZN7SkPaint5reset", kCore},
    {"SkPaint::~SkPaint", kPaintDtorM, "_ZN7SkPaintD", kCore},
    {"SkCanvas::drawRect", kCanvasDrawRectM, "_ZN8SkCanvas8drawRectERK6SkRect", kCore},

    // ---- kOptional (engine has its own implementation) --------------------
    {"SkCanvas::getTotalMatrix", kCanvasGetTotalMatrixM,
     "_ZNK8SkCanvas14getTotalMatrix", kOptional, true},
    {"SkPaint::setShader", kPaintSetShaderM, "_ZN7SkPaint9setShader", kOptional},
    {"SkImage_Base::makeSubset(SkRecorder*)", kImageMakeSubsetM, nullptr, kOptional, true},
    {"SkCanvas::baseRecorder", kCanvasBaseRecorderM, nullptr, kOptional},
    {"SkCanvas::recorder", kCanvasRecorderM, nullptr, kOptional},
    {"RenderEngine blur cache (marker)", kBlurCacheValidM, nullptr, kOptional},

    // ---- kBlurGenerate (any one enables backdropBlur) ---------------------
    {"KawaseBlurFilter::generateTemporaryImage", kGenerateKawaseM, nullptr, kBlurGenerate, true},
    {"GaussianBlurFilter::generateTemporaryImage", kGenerateGaussianM, nullptr, kBlurGenerate,
     true},
    {"KawaseBlurDualFilterV2::generateTemporaryImage", kGenerateKawaseDualV2M, nullptr,
     kBlurGenerate, true},
    {"GlassBlurFilter::generateTemporaryImage", kGenerateGlassM, nullptr, kBlurGenerate, true},
    {"KawaseDarkmoon::generateTemporaryImage", kGenerateDarkmoonM, nullptr, kBlurGenerate, true},

    // ---- kBuilder (informational) ----------------------------------------
    {"SkRuntimeEffectBuilder::ctor", kBuilderCtorM, "_ZN22SkRuntimeEffectBuilderC", kBuilder},
    {"SkRuntimeEffectBuilder::dtor", kBuilderDtorM, "_ZN22SkRuntimeEffectBuilderD", kBuilder},
    {"SkRuntimeEffectBuilder::writableUniformData", kBuilderWritableUniformDataM,
     "_ZN22SkRuntimeEffectBuilder19writableUniformData", kBuilder},
    {"SkRuntimeEffectBuilder::makeShader", kBuilderMakeShaderM,
     "_ZNK22SkRuntimeEffectBuilder10makeShader", kBuilder},
    {"BuilderUniform::set<float>", kBuilderUniformSetFloatM,
     "_ZN22SkRuntimeEffectBuilder14BuilderUniform3setIfE", kBuilder},
};

const char* strategyName(Strategy s) {
    switch (s) {
        case kCore: return "core";
        case kHookTarget: return "hook";
        case kDataRelease: return "release";
        case kOptional: return "opt";
        case kBuilder: return "builder";
        case kBlurGenerate: return "generate";
        default: return "?";
    }
}

const size_t kEntryPointCount = sizeof(kEntryPoints) / sizeof(kEntryPoints[0]);

static_assert(sizeof(kEntryPoints) / sizeof(kEntryPoints[0]) == kCount,
              "kEntryPoints and the Index enum have drifted apart");

}  // namespace sf
}  // namespace glass
