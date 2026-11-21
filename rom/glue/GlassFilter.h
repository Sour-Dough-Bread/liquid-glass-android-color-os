/*
 * StockGlass: Liquid Glass for SurfaceFlinger's blur regions, built in.
 *
 * GlassFilter wraps the blur filter SkiaRenderEngine chose (Kawase, Gaussian,
 * Kawase dual-filter) and takes its drawBlurRegion: a region is drawn as
 * Liquid Glass while persist.sys.glass.mode=glass, and by the wrapped filter,
 * untouched, otherwise. Glass is opt-in and Android's blur is the default.
 * With persist.sys.glass.enabled=0 at boot there is no wrapper at all.
 *
 * The drawing is the StockGlass engine's own code (glass/glass_draw.cpp),
 * the same that the StockGlass module hooks into SurfaceFlinger, driven
 * through glass/glass_draw.h; the shader, its presets and its properties are
 * the module's. Hand-written glue from rom/glue/ in
 * github.com/capthndsme/liquid-glass-android; rom/make_patch.sh generates
 * the patch that adds it.
 */

#pragma once

#include "BlurFilter.h"
#include "GlassFilterApi.h"

namespace android {
namespace renderengine {
namespace skia {

class GlassFilter final : public BlurFilter {
public:
    // `inner` wrapped (and owned), or `inner` itself when it is null or when
    // persist.sys.glass.enabled=0. Call once, where the filter is chosen.
    static BlurFilter* wrap(BlurFilter* inner, RuntimeEffectManager& effectManager);

    ~GlassFilter() override;

#if STOCKGLASS_GENERATE_TEMPORARY_IMAGE
    sk_sp<SkImage> generateTemporaryImage(SkiaGpuContext* context, const DisplaySettings& display,
                                          const uint32_t radius, const sk_sp<SkImage> blurInput,
                                          const SkRect& blurRect) const override;
    void preallocateBuffers(SkiaGpuContext* context, ui::Size size) override;
    bool areBuffersPreallocated(const SkiaGpuContext* context,
                                ui::Size displaySize) const override;
#else
    sk_sp<SkImage> generate(SkiaGpuContext* context, const uint32_t radius,
                            const sk_sp<SkImage> blurInput, const SkRect& blurRect) const override;
#endif

    void drawBlurRegion(SkCanvas* canvas, const SkRRect& effectRegion, const uint32_t blurRadius,
                        const float zoomScale, const float blurAlpha, const SkRect& blurRect,
                        sk_sp<SkImage> blurredImage, sk_sp<SkImage> input) override;

private:
    GlassFilter(BlurFilter* inner, RuntimeEffectManager& effectManager);

    BlurFilter* const mInner;
};

} // namespace skia
} // namespace renderengine
} // namespace android
