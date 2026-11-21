// The drawing half of StockGlass: what draws a blur region as Liquid Glass,
// and makes the blurs it draws. Two hosts drive it. The module
// (glass_engine.cpp) hooks SurfaceFlinger's blur filter at runtime and calls
// in from its hooks; a ROM that builds the glass in (rom/) calls in from a
// GlassFilter that wraps RenderEngine's blur filter. The code behind this
// header (glass_draw.cpp) is the same in both: only the Skia entry points it
// calls (bridge()) and the filters it asks for blurs (setGenerator()) come
// from the host.
//
// Everything here runs on RenderEngine's thread, as BlurFilter does.
#pragma once

#include <cstddef>
#include <cstdint>

#include "sk_bridge.h"

namespace glass {
namespace draw {

// How many blur filters a host can hand generation to: the module one per
// row of glass::sf::kGenerateFilters (glass_engine.cpp asserts they fit), a
// ROM one, the filter it wraps. Room to spare, so a new row there needs
// nothing here.
inline constexpr size_t kGenerateSlots = 8;

// persist.sys.glass.enabled as the engine has always read it: unset, or
// anything that is not the number 0, is on. Read once, when the host starts;
// off means the host stays out of the way entirely.
bool enabledAtBoot();

// sys.glass.engine, which glassctl and the WebUI read: "hooked" once the host
// is in place (the drawing code itself moves it on to "glass" or "no-glass"
// at the first glass frame), "not-hooked" when the host declined.
void setStatus(const char* status);

// sys.glass.regen: whether a blur can be made to order on this build (a
// generator is set), which is what Blur Intensity needs.
void setRegen(bool available);

// How many output pixels one pixel of a blur this RenderEngine's filters make
// covers: 1 / BlurFilter::kInputScale, which is how drawBlurRegion maps
// Android's own blur onto blurRect. 4 on AOSP and nearly every ROM, 4.545 on
// Lunaris 3.13. With `per_axis`, drawBlurRegion maps each axis by blurRect
// over the image's own size instead (Infinity-X 4.x), and the glass does the
// same. The module reads it from drawBlurRegion's code before hooking it
// (blur_scale.h); a host that never calls this gets 4. Called before the
// first frame.
void setBlurScale(float inverse, bool per_axis);

// Android's blur is never put off from here on, as with
// persist.sys.glass.lazyBlur=0: for a host that knows this RenderEngine keeps
// blurs between frames (the module: sf_symbols.cpp, kBlurCacheValid). Called
// before the first frame.
void refuseLazyBlur();

// The Skia entry points the drawing code calls. The module fills it with
// symbols resolved from SurfaceFlinger; a native build with Skia's own
// functions (rom/glue/glass/skia_native.cpp). Filled once, before the first
// draw.
skia::Bridge& bridge();

// A blur filter's own generate call, in the module's calling convention: the
// image argument is the address of a slot holding the image (as an sk_sp by
// value is passed), and the result is an sk_sp's bytes. `filter` is what the
// host handed generate() below; `display` may be null where the filter takes
// no DisplaySettings.
using GenerateFn = skia::ReturnedSkSp (*)(const void* filter, void* context,
                                          const void* display, uint32_t radius,
                                          void* input_sp, const SkRect* blur_rect);
void setGenerator(size_t slot, GenerateFn fn);

// In the filter's place: RenderEngine asked it for a blur of `*input_sp`.
// Returns the blur (through the generator of `slot`), or, while glass is
// being drawn, the input itself with one more reference: the blur is made by
// blurRegion() at the radius it is drawn at ("blur made to order",
// glass_draw.cpp). A slot with no generator returns null; no host asks one.
skia::ReturnedSkSp generate(size_t slot, const void* filter, void* context, const void* display,
                            uint32_t radius, void* input_sp, const SkRect* blur_rect);

// A region with no material: every ROM's but those below, and on those a
// layer's own blur.
inline constexpr int kNoMaterial = -1;

// Android's own drawBlurRegion, for a region the glass hands back: called with
// a slot holding the blurred image to draw with (one reference, the caller's)
// and the radius to draw at. The host's cookie carries the rest of its call.
using OriginalFn = void (*)(void* cookie, void* blurred_slot, uint32_t radius);

// The whole of BlurFilter::drawBlurRegion: draws the region as glass, or hands
// it to `original` with Android's blur (made now if generate() put it off).
// The images come as slots, as generate()'s does.
//
// `material` is the region's material where drawBlurRegion takes one
// (Evolution X 12.x and crDroid 13.x: 0 for the notification shade, 1 and up
// for a material a client chose), kNoMaterial anywhere else. Which materials
// the glass draws is decided in glass_draw.cpp (glassTakesMaterial) and
// nowhere else; a region it does not take goes to `original` at the radius
// it asked for, for the ROM to draw its own way.
void blurRegion(SkCanvas* canvas, const SkRRect* region, uint32_t radius, float zoom,
                float alpha, const SkRect* blur_rect, void* blurred_sp, void* input_sp,
                int material, OriginalFn original, void* cookie);

// Lets go of every image the drawing code holds between calls, and forgets
// the last generate call. For a host whose filter is being destroyed while
// its GPU context still exists.
void releaseAll();

}  // namespace draw
}  // namespace glass
