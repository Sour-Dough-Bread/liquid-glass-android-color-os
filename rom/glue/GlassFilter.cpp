/*
 * StockGlass: Liquid Glass for SurfaceFlinger's blur regions, built in.
 * See GlassFilter.h. Hand-written glue from rom/glue/ in
 * github.com/capthndsme/liquid-glass-android.
 */

#undef LOG_TAG
#define LOG_TAG "StockGlass"

#include "GlassFilter.h"

#include <log/log.h>
#include <sys/system_properties.h>

#include <utility>

#include "glass/crash_guard.h"
#include "glass/glass_draw.h"
#include "glass/glass_params.h"
#include "glass/skia_native.h"

namespace android {
namespace renderengine {
namespace skia {

namespace {

#if STOCKGLASS_GENERATE_TEMPORARY_IMAGE
// The wrapped filter's own blur, in the drawing code's generate convention
// (glass_draw.h): `filter` is the wrapped filter, `display` its DisplaySettings.
glass::skia::ReturnedSkSp innerGenerate(const void* filter, void* context, const void* display,
                                        uint32_t radius, void* input_sp,
                                        const SkRect* blur_rect) {
    sk_sp<SkImage> input = sk_ref_sp(*static_cast<SkImage* const*>(input_sp));
    sk_sp<SkImage> blurred = static_cast<const BlurFilter*>(filter)->generateTemporaryImage(
            static_cast<SkiaGpuContext*>(context), *static_cast<const DisplaySettings*>(display),
            radius, std::move(input), *blur_rect);
    return glass::skia::ReturnedSkSp(blurred.release());
}
#endif

// The wrapped filter's drawBlurRegion, less the image and the radius, which
// the drawing code picks.
struct StockDraw {
    BlurFilter* filter;
    SkCanvas* canvas;
    const SkRRect* region;
    float zoom;
    float alpha;
    const SkRect* blurRect;
    const sk_sp<SkImage>* input;
    int material;
};

void drawStock(void* cookie, void* blurred_slot, uint32_t radius) {
    const auto* d = static_cast<const StockDraw*>(cookie);
    sk_sp<SkImage> blurred = sk_ref_sp(*static_cast<SkImage**>(blurred_slot));
#if STOCKGLASS_DRAW_MATERIAL
    d->filter->drawBlurRegion(d->canvas, *d->region, radius, d->zoom, d->alpha, *d->blurRect,
                              std::move(blurred), *d->input, d->material);
#else
    d->filter->drawBlurRegion(d->canvas, *d->region, radius, d->zoom, d->alpha, *d->blurRect,
                              std::move(blurred), *d->input);
#endif
}

} // namespace

BlurFilter* GlassFilter::wrap(BlurFilter* inner, RuntimeEffectManager& effectManager) {
    if (inner == nullptr) return nullptr; // blur is off on this device: nothing to restyle
    if (!glass::draw::enabledAtBoot()) {
        ALOGI("disabled by persist.sys.glass.enabled=0");
        return inner;
    }
    static const bool started = [] {
        // Before anything reads the mode: if the last SurfaceFlinger died
        // drawing glass, this one starts stock (crash_guard.h).
        glass::crashGuardStart();
        glass::paramsInit();
        glass::draw::bridge() = glass::skia::nativeBridge();
#if STOCKGLASS_GENERATE_TEMPORARY_IMAGE
        glass::draw::setGenerator(0, &innerGenerate);
#endif
        return true;
    }();
    (void)started;
    glass::draw::setRegen(STOCKGLASS_GENERATE_TEMPORARY_IMAGE != 0);
    glass::draw::setStatus("hooked");
    // Not the module's: says the engine is built into this ROM, for tools
    // that would otherwise look for the module's library in SurfaceFlinger.
    __system_property_set("sys.glass.builtin", "1");
    ALOGI("built in; persist.sys.glass.mode=glass to draw Liquid Glass, anything else keeps "
          "Android's blur");
    return new GlassFilter(inner, effectManager);
}

GlassFilter::GlassFilter(BlurFilter* inner, RuntimeEffectManager& effectManager)
      : BlurFilter(effectManager, inner->getMaxCrossFadeRadius()), mInner(inner) {}

GlassFilter::~GlassFilter() {
    glass::draw::releaseAll();
    delete mInner;
}

#if STOCKGLASS_GENERATE_TEMPORARY_IMAGE
sk_sp<SkImage> GlassFilter::generateTemporaryImage(SkiaGpuContext* context,
                                                   const DisplaySettings& display,
                                                   const uint32_t radius,
                                                   const sk_sp<SkImage> blurInput,
                                                   const SkRect& blurRect) const {
    // While glass is drawn this hands back the input itself, and the region's
    // blur is made when it is drawn, at the radius it is drawn at.
    SkImage* input = blurInput.get();
    glass::skia::ReturnedSkSp blurred =
            glass::draw::generate(0, mInner, context, &display, radius, &input, &blurRect);
    return sk_sp<SkImage>(static_cast<SkImage*>(blurred.pointer()));
}

void GlassFilter::preallocateBuffers(SkiaGpuContext* context, ui::Size size) {
    mInner->preallocateBuffers(context, size);
}

bool GlassFilter::areBuffersPreallocated(const SkiaGpuContext* context,
                                         ui::Size displaySize) const {
    return mInner->areBuffersPreallocated(context, displaySize);
}
#else
sk_sp<SkImage> GlassFilter::generate(SkiaGpuContext* context, const uint32_t radius,
                                     const sk_sp<SkImage> blurInput,
                                     const SkRect& blurRect) const {
    // Before Android 17 the module leaves generate() alone too: every region
    // is drawn from the blur Android made, at the radius it asked for.
    return mInner->generate(context, radius, blurInput, blurRect);
}
#endif

#if STOCKGLASS_DRAW_MATERIAL
void GlassFilter::drawBlurRegion(SkCanvas* canvas, const SkRRect& effectRegion,
                                 const uint32_t blurRadius, const float zoomScale,
                                 const float blurAlpha, const SkRect& blurRect,
                                 sk_sp<SkImage> blurredImage, sk_sp<SkImage> input,
                                 int32_t material) {
    draw(canvas, effectRegion, blurRadius, zoomScale, blurAlpha, blurRect, blurredImage, input,
         material);
}
#else
void GlassFilter::drawBlurRegion(SkCanvas* canvas, const SkRRect& effectRegion,
                                 const uint32_t blurRadius, const float zoomScale,
                                 const float blurAlpha, const SkRect& blurRect,
                                 sk_sp<SkImage> blurredImage, sk_sp<SkImage> input) {
    draw(canvas, effectRegion, blurRadius, zoomScale, blurAlpha, blurRect, blurredImage, input,
         glass::draw::kNoMaterial);
}
#endif

void GlassFilter::draw(SkCanvas* canvas, const SkRRect& effectRegion, uint32_t blurRadius,
                       float zoomScale, float blurAlpha, const SkRect& blurRect,
                       const sk_sp<SkImage>& blurredImage, const sk_sp<SkImage>& input,
                       int material) {
    // The drawing code reads each image through a slot holding it, as the
    // module's hook is handed them; these borrow the references the caller's
    // sk_sp arguments hold.
    SkImage* blurred = blurredImage.get();
    SkImage* in = input.get();
    StockDraw stock{mInner, canvas, &effectRegion, zoomScale, blurAlpha, &blurRect, &input,
                    material};
    glass::draw::blurRegion(canvas, &effectRegion, blurRadius, zoomScale, blurAlpha, &blurRect,
                            &blurred, &in, material, &drawStock, &stock);
}

#if STOCKGLASS_SET_DISPLAY
void GlassFilter::setDisplay(const std::string& namePlusId) {
    mInner->setDisplay(namePlusId);
}
#endif

#if STOCKGLASS_LENS_REACH
float GlassFilter::lensReach(const SkRRect& region, int32_t material) const {
    return mInner->lensReach(region, material);
}
#endif

} // namespace skia
} // namespace renderengine
} // namespace android
