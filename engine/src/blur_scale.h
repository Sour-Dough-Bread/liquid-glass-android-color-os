// How big Android's blurred image is, relative to the blur rect it covers.
//
// BlurFilter::drawBlurRegion maps the blurred image back over blurRect by a
// scale: BlurFilter::kInverseInputScale, 4 in AOSP, because every filter
// draws its blur into a surface a quarter of blurRect's size. The engine reads
// the same image and has to map it the same way. ROMs change it:
//
//   - Lunaris 3.13 (A16 QPR2) cut kInputScale to 0.22 for every filter, so its
//     drawBlurRegion loads 4.54545 where stock loads 4.
//   - Project Infinity-X 4.x (A17, AxionOS's GlassBlurFilter) loads no
//     constant at all: drawBlurRegion divides blurRect by the image's own
//     width and height, per axis (about 1/6 with its default gb_scale).
//
// The probe reads the answer out of the target's own drawBlurRegion:
//
//   1. In its first 120 instructions, the first `adrp xN` + `ldr qM, [xN,
//      #off]` whose 16 bytes are {k, 0, 0, 0} with 1 < k < 10 (A16 QPR2 and
//      A17 keep the scale row of the matrix in a literal), or `fmov sN, #k`
//      with 2 < k < 10 (Android 14 and 15 build it from an immediate).
//   2. No constant, and three or more fdiv in the whole function: the scale is
//      the image's, per axis (Infinity-X: two for the axes, one stock).
//   3. Otherwise the direct calls in that window, in call order, two deep,
//      where only the literal form counts. Pixel CP4A moved the body into
//      setupBlurPaint(), which calls an out-of-line getShaderTransform()
//      holding the literal; Evolution X 12.4 calls getShaderTransform() from
//      drawBlurRegion itself.
//   4. Nothing: not found, and 4, AOSP's value.
//
// It runs inside SurfaceFlinger on code it did not build. Every byte comes
// through the reader, which refuses anything outside the binary's segments:
// instructions only from an executable one, literals only from a read-only
// one. drawBlurRegion is never read past its symbol's size; a followed
// function, whose size is unknown, is read in short blocks, up to its first
// return or 120 instructions, within a fixed budget. A decoded address is
// checked for wrap-around before the reader is asked for it. Run it before
// drawBlurRegion is hooked: it reads the prologue the hook replaces.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "elf_syms.h"

namespace glass {

struct BlurScale {
    // Output pixels per pixel of Android's blurred image: 1 / kInputScale.
    // 4 (AOSP's) when not found. When per_axis is set, only a fallback for an
    // image with no size: the real values are blurRect.width() /
    // image->width() and blurRect.height() / image->height(), at draw time.
    float inverse = 4.0f;
    bool per_axis = false;
    bool found = false;  // a constant, or the per-axis division, was seen
    std::string how;     // one line for the log
};

// What the probe reads with: `n` bytes at link-time address `vaddr`, copied to
// `out`. kCode asks for instructions and must be refused unless the whole
// range lies in an executable segment; kConstant asks for a literal an
// instruction loads and must be refused unless it lies in a segment that is
// not writable. False means the bytes are not available; the probe treats
// that as "nothing here", never as an error to retry.
enum class BlurScaleRead : uint8_t { kCode, kConstant };
using BlurScaleReader = bool (*)(uint64_t vaddr, uint8_t* out, size_t n, BlurScaleRead what,
                                 void* user);

// The probe itself, on any code a reader can supply: drawBlurRegion's
// link-time address and size (from its symbol). Decodes A64 whatever the host,
// so the host tests run it on hand-assembled code.
BlurScale probeBlurScale(uint64_t vaddr, uint64_t size, BlurScaleReader read, void* user);

// The binary on disk, by its PT_LOAD headers (glasscheck, the host tests).
// Not found, with 4, unless it is an AArch64 ELF64.
BlurScale probeBlurScaleInFile(const std::string& path, const Symbol& draw_blur_region);

// This process: the binary at `exe_path`, mapped at load bias `bias`. Which
// ranges may be read comes from the file's headers (as readExecSegments does
// for abi_check); the bytes come from memory. On anything but AArch64, not
// found, with 4.
BlurScale probeBlurScaleInProcess(const std::string& exe_path, uintptr_t bias,
                                  const Symbol& draw_blur_region);

}  // namespace glass
