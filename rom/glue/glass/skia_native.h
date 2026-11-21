// StockGlass, built into RenderEngine: the Skia bridge of a native build.
//
// The drawing code (glass_draw.cpp) calls Skia through glass::skia::Bridge,
// a table of entry points. The module fills it with functions resolved from
// SurfaceFlinger's own symbols; a native build fills it with Skia's own
// functions, linked, which is all this is.
//
// Hand-written glue: rom/glue/glass/ in github.com/capthndsme/liquid-glass-android.
#pragma once

#include "sk_bridge.h"

namespace glass {
namespace skia {

// Every entry point the drawing code needs, each Skia's own, plus the
// optional ones (snapshotCopy, the Graphite check).
Bridge nativeBridge();

}  // namespace skia
}  // namespace glass
