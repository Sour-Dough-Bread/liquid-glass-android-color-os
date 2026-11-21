// StockGlass, built into RenderEngine: the Skia bridge of a native build.
//
// Each function below has the signature of a glass::skia::Bridge entry point
// (sk_bridge.h) and calls the Skia function of the same name. Where the
// module's convention passes an sk_sp by value as the address of a slot
// holding the pointer, these take the slot's reference as the by-value
// sk_sp would. compile() is this build's definition of sk_bridge.h's.
//
// Hand-written glue: rom/glue/glass/ in github.com/capthndsme/liquid-glass-android.

#include "skia_native.h"

#include <new>
#include <string>
#include <utility>

#include "include/core/SkString.h"

namespace glass {
namespace skia {

SkRuntimeEffect* compile(const Bridge&, const char* sksl, std::string* error) {
    if (sksl == nullptr) return nullptr;
    SkRuntimeEffect::Result result = SkRuntimeEffect::MakeForShader(SkString(sksl));
    if (result.effect == nullptr) {
        if (error != nullptr) error->assign(result.errorText.c_str(), result.errorText.size());
        return nullptr;
    }
    // One reference, kept for the life of the process, as the module's is.
    return result.effect.release();
}

namespace {

// Never called: compile() above calls Skia itself. Bridge::compileReady()
// asks for a compile entry point all the same, so it is given one.
Returned<kResultSize> makeForShaderUnused(void*, const SkRuntimeEffect::Options*) {
    return Returned<kResultSize>();
}

void stringCtor(void* self, const char* text) { new (self) SkString(text); }
void stringDtor(void* self) { static_cast<SkString*>(self)->~SkString(); }

const SkRuntimeEffect::Uniform* findUniform(const void* effect, std::string_view name) {
    return static_cast<const SkRuntimeEffect*>(effect)->findUniform(name);
}
const SkRuntimeEffect::Child* findChild(const void* effect, std::string_view name) {
    return static_cast<const SkRuntimeEffect*>(effect)->findChild(name);
}
size_t uniformSize(const void* effect) {
    return static_cast<const SkRuntimeEffect*>(effect)->uniformSize();
}

ReturnedSkSp imageMakeShader(const void* image, SkTileMode tx, SkTileMode ty,
                             const SkSamplingOptions* sampling, const SkMatrix* local_matrix) {
    return ReturnedSkSp(
        static_cast<const SkImage*>(image)->makeShader(tx, ty, *sampling, local_matrix).release());
}

ReturnedSkSp dataMakeZeroInit(size_t length) {
    return ReturnedSkSp(SkData::MakeZeroInitialized(length).release());
}

// sk_sp<SkData>'s destructor: the slot's reference, if it still holds one.
void dataSlotRelease(void* slot) { SkSafeUnref(*static_cast<SkData**>(slot)); }

ReturnedSkSp effectMakeShader(const void* effect, void* data_slot, const void* children,
                              size_t child_count, const SkMatrix* local_matrix) {
    // The uniform blob by value: its reference moves in, and the slot is left
    // empty, as the module's caller expects of the by-value sk_sp.
    auto** slot = static_cast<SkData**>(data_slot);
    sk_sp<const SkData> uniforms(*slot);
    *slot = nullptr;
    constexpr size_t kMaxChildren = 4;
    if (child_count > kMaxChildren) return ReturnedSkSp(nullptr);
    sk_sp<SkShader> kids[kMaxChildren];
    const auto* raw = static_cast<SkShader* const*>(children);
    for (size_t i = 0; i < child_count; ++i) kids[i] = sk_ref_sp(raw[i]);
    return ReturnedSkSp(static_cast<const SkRuntimeEffect*>(effect)
                            ->makeShader(std::move(uniforms), kids, child_count, local_matrix)
                            .release());
}

void matrixPostConcat(SkMatrix* self, const SkMatrix* other) { self->postConcat(*other); }
bool matrixInvert(const SkMatrix* self, SkMatrix* inverse) { return self->invert(inverse); }

// ScopedPaint zeroes its storage, then asks for an SkPaint in it.
void paintReset(void* paint) { new (paint) SkPaint(); }
void paintDtor(void* paint) { static_cast<SkPaint*>(paint)->~SkPaint(); }

void canvasDrawRect(void* canvas, const SkRect* rect, const void* paint) {
    static_cast<SkCanvas*>(canvas)->drawRect(*rect, *static_cast<const SkPaint*>(paint));
}

SkMatrix canvasGetTotalMatrix(const void* canvas) {
    return static_cast<const SkCanvas*>(canvas)->getTotalMatrix();
}

void paintSetShader(void* paint, void* shader_slot) {
    auto** slot = static_cast<SkShader**>(shader_slot);
    static_cast<SkPaint*>(paint)->setShader(sk_sp<SkShader>(*slot));
    *slot = nullptr;
}

ReturnedSkSp imageMakeSubset(const void* image, void* recorder, const SkIRect* subset,
                             Bridge::SubsetProperties properties) {
    SkImage::RequiredProperties required;
    required.fMipmapped = properties.mipmapped;
    return ReturnedSkSp(static_cast<const SkImage*>(image)
                            ->makeSubset(static_cast<SkRecorder*>(recorder), *subset, required)
                            .release());
}

void* canvasBaseRecorder(const void* canvas) {
    return static_cast<const SkCanvas*>(canvas)->baseRecorder();
}

void* canvasRecorder(const void* canvas) {
    return static_cast<const SkCanvas*>(canvas)->recorder();
}

}  // namespace

Bridge nativeBridge() {
    Bridge b;
    b.MakeForShaderOpts = &makeForShaderUnused;
    b.SkStringCtor = &stringCtor;
    b.SkStringDtor = &stringDtor;
    b.findUniform = &findUniform;
    b.findChild = &findChild;
    b.uniformSize = &uniformSize;
    b.ImageMakeShader = &imageMakeShader;
    b.DataMakeZeroInit = &dataMakeZeroInit;
    b.SkSpDataDtor = &dataSlotRelease;
    b.EffectMakeShader = &effectMakeShader;
    b.MatrixPostConcat = &matrixPostConcat;
    b.MatrixInvertNonIdentity = &matrixInvert;
    b.PaintReset = &paintReset;
    b.PaintDtor = &paintDtor;
    b.CanvasDrawRect = &canvasDrawRect;
    b.CanvasGetTotalMatrix = &canvasGetTotalMatrix;
    b.PaintSetShader = &paintSetShader;
    b.ImageMakeSubset = &imageMakeSubset;
    b.CanvasBaseRecorder = &canvasBaseRecorder;
    b.CanvasRecorder = &canvasRecorder;
    return b;
}

}  // namespace skia
}  // namespace glass
