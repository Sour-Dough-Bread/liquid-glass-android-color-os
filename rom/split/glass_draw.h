// The drawing half of StockGlass, which two hosts drive: the module
// (engine/src/glass_engine.cpp), which hooks SurfaceFlinger's blur filter at
// runtime, and a ROM that builds the glass in (rom/, which adds a GlassFilter
// to frameworks/native). The code behind this header is the same in both;
// only the Skia entry points it calls (bridge()) and the blur filter it asks
// for blurs (setGenerator()) come from the host.
//
// Everything here runs on RenderEngine's thread, as BlurFilter does.
//
// PROPOSED (rom_patch.md, "The engine/src split"): for now rom/split/
// split_engine.py builds this API from a copy of glass_engine.cpp. Once
// engine/src has glass_draw.h and glass_draw.cpp of its own, rom/ uses them
// as they are.
#pragma once

#include <cstddef>
#include <cstdint>

#include "sk_bridge.h"

namespace glass {
namespace draw {

// How many blur filters a host can hand generation to: the module one per
// row of glass::sf::kGenerateFilters, a ROM one (the filter it wraps).
inline constexpr size_t kGenerateSlots = 4;

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

// The Skia entry points the drawing code calls. The module fills it with
// symbols resolved from SurfaceFlinger; a native build with Skia's own
// functions (rom/glue/skia_native.cpp). Filled once, before the first draw.
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
// blurRegion() at the radius it is drawn at ("blur made to order").
skia::ReturnedSkSp generate(size_t slot, const void* filter, void* context, const void* display,
                            uint32_t radius, void* input_sp, const SkRect* blur_rect);

// Android's own drawBlurRegion, for a region the glass hands back: called with
// a slot holding the blurred image to draw with (one reference, the caller's)
// and the radius to draw at.
using OriginalFn = void (*)(void* cookie, void* blurred_slot, uint32_t radius);

// The whole of BlurFilter::drawBlurRegion: draws the region as glass, or hands
// it to `original` with Android's blur (made now if generate() put it off).
// The images come as slots, as generate()'s does.
void blurRegion(SkCanvas* canvas, const SkRRect* region, uint32_t radius, float zoom,
                float alpha, const SkRect* blur_rect, void* blurred_sp, void* input_sp,
                OriginalFn original, void* cookie);

// A region the host leaves to Android without asking the glass (the module:
// Evolution X's own glass, kind > 0): `original` draws it with Android's blur,
// made now if generate() put it off, at `radius` as given.
void androidDraws(const SkRect* blur_rect, uint32_t radius, void* blurred_sp, void* input_sp,
                  OriginalFn original, void* cookie);

// Lets go of every image the drawing code holds between calls, and forgets
// the last generate call. For a host whose filter is being destroyed while
// its GPU context still exists.
void releaseAll();

}  // namespace draw
}  // namespace glass
