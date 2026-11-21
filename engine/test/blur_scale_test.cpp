// Host test for the blurred-image scale probe (blur_scale.h).
//
//   blur_scale_test                    hand-assembled cases; no ROM binaries
//   blur_scale_test --corpus [dir]     every surfaceflinger under dir
//                                      (default captures/sf-corpus): k,
//                                      per_axis and how, checked against what
//                                      each family is known to do
//
// Each unit case is a pattern a real build showed (the firmware survey's
// reports, captures/sf-corpus/reports/), reduced to the instructions that
// decide it, plus the hostile inputs the probe must survive inside
// SurfaceFlinger: garbage, truncated code, loops, addresses it may not read.
// A wrong scale draws the glass misplaced; a crash there is a soft reboot.
//
// Build from the repository root, as one command:
//   g++ -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -include cstdarg
//       engine/test/blur_scale_test.cpp engine/src/blur_scale.cpp
//       engine/src/elf_syms.cpp engine/src/sf_symbols.cpp engine/src/sf_resolve.cpp
//       engine/src/sf_cache.cpp engine/src/abi_check.cpp -o blur_scale_test

#include "../src/blur_scale.h"
#include "../src/elf_syms.h"
#include "../src/sf_resolve.h"
#include "../src/sf_symbols.h"

#include <cxxabi.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

namespace {

// ---- a sparse image: code words and read-only bytes, nothing else ----------

struct Image {
    std::map<uint64_t, uint32_t> code;
    std::map<uint64_t, uint8_t> data;
    // Code the probe must never ask for: [fence_lo, fence_hi). A request that
    // touches it is refused and counted.
    uint64_t fence_lo = 0, fence_hi = 0;
    int fence_hits = 0;
    int reads = 0;

    void at(uint64_t addr, std::initializer_list<uint32_t> insns) {
        for (uint32_t i : insns) {
            code[addr] = i;
            addr += 4;
        }
    }
    void floats(uint64_t addr, std::initializer_list<float> fs) {
        for (float f : fs) {
            uint8_t b[4];
            memcpy(b, &f, 4);
            for (int k = 0; k < 4; ++k) data[addr++] = b[k];
        }
    }
};

bool readImage(uint64_t vaddr, uint8_t* out, size_t n, glass::BlurScaleRead what, void* user) {
    auto* img = static_cast<Image*>(user);
    ++img->reads;
    if (what == glass::BlurScaleRead::kCode) {
        if (n % 4 != 0) return false;
        if (img->fence_hi > img->fence_lo && vaddr < img->fence_hi && vaddr + n > img->fence_lo) {
            ++img->fence_hits;
            return false;
        }
        for (size_t k = 0; k < n / 4; ++k) {
            const auto it = img->code.find(vaddr + 4 * k);
            if (it == img->code.end()) return false;
            memcpy(out + 4 * k, &it->second, 4);
        }
        return true;
    }
    for (size_t k = 0; k < n; ++k) {
        const auto it = img->data.find(vaddr + k);
        if (it == img->data.end()) return false;
        out[k] = it->second;
    }
    return true;
}

bool refuseAll(uint64_t, uint8_t*, size_t, glass::BlurScaleRead, void*) { return false; }

// ---- encoders (checked against clang's assembler) ---------------------------

uint32_t adrp(uint32_t reg, uint64_t pc, uint64_t target) {
    const int64_t pages = (static_cast<int64_t>(target & ~uint64_t{0xFFF}) -
                           static_cast<int64_t>(pc & ~uint64_t{0xFFF})) >> 12;
    const uint32_t imm = static_cast<uint32_t>(pages) & 0x1FFFFF;
    return 0x90000000u | ((imm & 3) << 29) | ((imm >> 2) << 5) | reg;
}
uint32_t ldrQ(uint32_t rt, uint32_t rn, uint32_t off) {  // ldr qT, [xN, #off]
    return 0x3DC00000u | ((off / 16) << 10) | (rn << 5) | rt;
}
uint32_t addX(uint32_t rd, uint32_t rn, uint32_t imm) {  // add xD, xN, #imm
    return 0x91000000u | (imm << 10) | (rn << 5) | rd;
}
uint32_t fdivS(uint32_t d, uint32_t n, uint32_t m) {
    return 0x1E201800u | (m << 16) | (n << 5) | d;
}
uint32_t fmovS(uint32_t rd, uint32_t imm8) { return 0x1E201000u | (imm8 << 13) | rd; }
uint32_t bl(uint64_t from, uint64_t to) {
    return 0x94000000u | (static_cast<uint32_t>((to - from) / 4) & 0x03FFFFFFu);
}

// imm8 of fmov s, #k (clang: fmov s0, #4.0 is 1e221000).
constexpr uint32_t kImm4 = 0x10, kImm2 = 0x00, kImm6 = 0x18, kImm1 = 0x70, kImmMinus4 = 0x90;

constexpr uint32_t kPaciasp = 0xD503233F;
constexpr uint32_t kBtiC = 0xD503245F;
constexpr uint32_t kNop = 0xD503201F;
constexpr uint32_t kStpFpLr = 0xA9BF7BFD;     // stp x29, x30, [sp, #-16]!
constexpr uint32_t kMovFpSp = 0x910003FD;     // mov x29, sp
constexpr uint32_t kRet = 0xD65F03C0;
constexpr uint32_t kRetaa = 0xD65F0BFF;
constexpr uint32_t kMovX8X0 = 0xAA0003E8;     // mov x8, x0
constexpr uint32_t kMovW8Miter = 0x52A81008;  // mov w8, #0x40800000 (4.0f: SkPaint's miter)
constexpr uint32_t kStrW8X19 = 0xB9004668;    // str w8, [x19, #0x44]
constexpr uint32_t kLdrX17 = 0xF9400E11;      // ldr x17, [x16, #0x18]
constexpr uint32_t kAddX16 = 0x91006210;      // add x16, x16, #0x18
constexpr uint32_t kBrX17 = 0xD61F0220;       // br x17
constexpr uint32_t kFmulS = 0x1E220820;       // fmul s0, s1, s2
constexpr uint32_t kFmovS8S0 = 0x1E204008;    // fmov s8, s0
constexpr uint32_t kMovX19X8 = 0xAA0803F3;    // mov x19, x8

int failures = 0;

void report(bool ok, const char* name, const glass::BlurScale& b) {
    if (!ok) ++failures;
    printf("  [%s] %-58s k=%-8g %s%s | %s\n", ok ? " ok " : "FAIL", name, b.inverse,
           b.found ? "found" : "not found", b.per_axis ? " per-axis" : "", b.how.c_str());
}

void expectK(const char* name, Image& img, uint64_t vaddr, uint64_t size, float k) {
    const glass::BlurScale b = glass::probeBlurScale(vaddr, size, readImage, &img);
    report(b.found && !b.per_axis && std::fabs(b.inverse - k) < 1e-4f && img.fence_hits == 0, name,
           b);
}

void expectPerAxis(const char* name, Image& img, uint64_t vaddr, uint64_t size) {
    const glass::BlurScale b = glass::probeBlurScale(vaddr, size, readImage, &img);
    report(b.found && b.per_axis && img.fence_hits == 0, name, b);
}

void expectNotFound(const char* name, Image& img, uint64_t vaddr, uint64_t size) {
    const glass::BlurScale b = glass::probeBlurScale(vaddr, size, readImage, &img);
    report(!b.found && !b.per_axis && b.inverse == 4.0f && img.fence_hits == 0, name, b);
}

// drawBlurRegion's usual opening, then `body`, padded to `pad` instructions.
void function(Image& img, uint64_t at, std::initializer_list<uint32_t> body, size_t pad = 0) {
    img.at(at, {kPaciasp, kStpFpLr, kMovFpSp});
    img.at(at + 12, body);
    for (size_t k = 3 + body.size(); k < pad; ++k) img.code[at + 4 * k] = kNop;
}

int unitTests() {
    const uint64_t F = 0x259000;   // drawBlurRegion
    const uint64_t P = 0x32000;    // its literal page
    {
        // A17 stock (PixelOS cancunf 0x2594e0): {4,0,0,0} right after traceBegin.
        Image img;
        function(img, F, {bl(F + 12, 0x900000), adrp(8, F + 16, P), ldrQ(2, 8, 0x2c0), kRet});
        img.floats(P + 0x2c0, {4.0f, 0.0f, 0.0f, 0.0f});
        expectK("A17 stock: literal {4,0,0,0}", img, F, 64, 4.0f);
    }
    {
        // Lunaris 3.13 duchamp (0x251c8c): kInputScale 0.22.
        Image img;
        function(img, F, {adrp(8, F + 12, P), ldrQ(2, 8, 0xc60), kRet});
        img.floats(P + 0xc60, {4.545455f, 0.0f, 0.0f, 0.0f});
        expectK("Lunaris 3.13: literal {4.54545,0,0,0}", img, F, 64, 4.545455f);
    }
    {
        // Android 14/15 (Motorola, Nothing OS 3.x, Pixel AP4A): two fmov #4.0.
        Image img;
        function(img, F, {fmovS(0, kImm4), fmovS(1, kImm4), kRet});
        expectK("Android 15: fmov s0, #4.0", img, F, 64, 4.0f);
    }
    {
        // A {1,0,0,0} row (an identity matrix) is no scale; the next literal is.
        Image img;
        function(img, F, {adrp(8, F + 12, P), ldrQ(0, 8, 0x100), ldrQ(1, 8, 0x200), kRet});
        img.floats(P + 0x100, {1.0f, 0.0f, 0.0f, 0.0f});
        img.floats(P + 0x200, {4.0f, 0.0f, 0.0f, 0.0f});
        expectK("{1,0,0,0} skipped, the next literal counts", img, F, 64, 4.0f);
    }
    {
        Image img;
        function(img, F, {adrp(8, F + 12, P), ldrQ(0, 8, 0x100), kRet});
        img.floats(P + 0x100, {4.0f, 1.0f, 0.0f, 0.0f});
        expectNotFound("{4,1,0,0} is not a scale row", img, F, 64);
    }
    {
        Image img;
        function(img, F, {adrp(8, F + 12, P), ldrQ(0, 8, 0x100), kRet});
        img.floats(P + 0x100, {10.0f, 0.0f, 0.0f, 0.0f});
        expectNotFound("k = 10 is out of range", img, F, 64);
    }
    {
        Image img;
        function(img, F, {fmovS(0, kImm2), fmovS(1, kImm1), fmovS(2, kImmMinus4), kRet});
        expectNotFound("fmov #2.0, #1.0, #-4.0 are no scale", img, F, 64);
    }
    {
        Image img;
        function(img, F, {fmovS(3, kImm6), kRet});
        expectK("fmov #6.0 is inside 2 < k < 10", img, F, 64, 6.0f);
    }
    {
        // The register an adrp filled is overwritten before the load.
        Image img;
        function(img, F, {adrp(8, F + 12, P), kMovX8X0, ldrQ(2, 8, 0x2c0), kRet});
        img.floats(P + 0x2c0, {4.0f, 0.0f, 0.0f, 0.0f});
        expectNotFound("adrp register overwritten before ldr q", img, F, 64);
    }
    {
        // adrp + add + ldr q [x8]: the page offset in the add.
        Image img;
        function(img, F, {adrp(8, F + 12, P), addX(8, 8, 0x2c0), ldrQ(2, 8, 0), kRet});
        img.floats(P + 0x2c0, {4.0f, 0.0f, 0.0f, 0.0f});
        expectK("adrp + add + ldr q [x8]", img, F, 64, 4.0f);
    }
    {
        // The literal the code points at is not readable data.
        Image img;
        function(img, F, {adrp(8, F + 12, 0x7000000), ldrQ(2, 8, 0x2c0), kRet});
        expectNotFound("literal outside the read-only data", img, F, 64);
    }
    {
        // Infinity-X 4.2 (0x2abebc, 0x2abec4): the image's size, per axis.
        Image img;
        function(img, F, {fdivS(0, 1, 0), fdivS(1, 1, 3), kFmulS, fdivS(9, 10, 0), kRet});
        expectPerAxis("Infinity-X 4.2: three fdiv, no constant", img, F, 64);
    }
    {
        Image img;
        function(img, F, {fdivS(0, 1, 0), fdivS(9, 10, 0), kRet});
        expectNotFound("stock's one or two fdiv are no division", img, F, 64);
    }
    {
        // Per axis wins over a constant in a callee.
        const uint64_t G = 0x300000;
        Image img;
        function(img, F, {fdivS(0, 1, 0), fdivS(1, 1, 3), fdivS(9, 10, 0), bl(F + 24, G), kRet});
        function(img, G, {adrp(8, G + 12, P), ldrQ(0, 8, 0x100), kRet});
        img.floats(P + 0x100, {5.0f, 0.0f, 0.0f, 0.0f});
        expectPerAxis("per axis is decided before any call is followed", img, F, 64);
    }
    {
        // Pixel CP4A (shiba 0x361b70, 444 B): drawBlurRegion calls traceBegin
        // through the PLT, then setupBlurPaint (0x361d80). That one sets the
        // paint's miter limit (4.0f, from a mov), loads an identity row and
        // calls getShaderTransform (0x646e90), whose literal is the scale.
        const uint64_t plt = 0x91b1f0, setup = 0x361d80, shader = 0x646e90;
        Image img;
        function(img, F, {bl(F + 12, plt), bl(F + 16, setup), kRet}, 111);
        img.at(plt, {adrp(16, plt, 0x924000), kLdrX17, kAddX16, kBrX17});
        function(img, setup,
                 {kMovW8Miter, kStrW8X19, adrp(8, setup + 20, 0x94000), ldrQ(0, 8, 0xb0),
                  bl(setup + 28, shader), kRet});
        img.floats(0x940b0, {1.0f, 0.0f, 0.0f, 0.0f});
        function(img, shader, {adrp(8, shader + 12, 0x93000), ldrQ(0, 8, 0xdc0), kRet});
        img.floats(0x93dc0, {4.0f, 0.0f, 0.0f, 0.0f});
        img.fence_lo = F + 444;  // nothing of drawBlurRegion past its size
        img.fence_hi = F + 0x1000;
        expectK("Pixel CP4A: drawBlurRegion -> setupBlurPaint -> getShaderTransform", img, F, 444,
                4.0f);
    }
    {
        // Evolution X 12.4 (garnet 0x5607b8): getShaderTransform out of line,
        // called from drawBlurRegion itself, second after traceBegin's PLT
        // stub; at 0x561750 it keeps its float argument in s8 between the
        // adrp x8 and the load.
        const uint64_t plt = 0x838020, shader = 0x561730;
        Image img;
        function(img, F, {kMovW8Miter, bl(F + 16, plt), bl(F + 20, shader), kRet}, 120);
        img.at(plt, {adrp(16, plt, 0x924000), kLdrX17, kAddX16, kBrX17});
        function(img, shader, {kMovX19X8, adrp(8, shader + 16, P), kFmovS8S0, ldrQ(0, 8, 0x6e0),
                               kRetaa});
        img.floats(P + 0x6e0, {4.0f, 0.0f, 0.0f, 0.0f});
        expectK("Evolution X 12.4: drawBlurRegion -> getShaderTransform", img, F, 480, 4.0f);
    }
    {
        // SIMD&FP instructions write V registers, except these.
        const struct {
            const char* name;
            uint32_t insn;
            bool kills_x8;
        } cases[] = {
            {"fmov s8, s0 keeps the x8 page", kFmovS8S0, false},
            {"fcmp s8, s0 keeps the x8 page", 0x1E202100, false},
            {"fmadd s8, s0, s1, s2 keeps the x8 page", 0x1F010808, false},
            {"movi v8.4s, #0 keeps the x8 page", 0x4F000408, false},
            {"mov v8.16b, v0.16b keeps the x8 page", 0x4EA01C08, false},
            {"fcvt d8, s0 keeps the x8 page", 0x1E22C008, false},
            {"fmov x8, d0 drops the x8 page", 0x9E660008, true},
            {"fcvtzs w8, s0 drops the x8 page", 0x1E380008, true},
            {"fcvtzs x8, d0, #16 drops the x8 page", 0x9E58C008, true},
            {"fjcvtzs w8, d0 drops the x8 page", 0x1E7E0008, true},
            {"umov w8, v0.s[0] drops the x8 page", 0x0E043C08, true},
            {"smov x8, v1.h[2] drops the x8 page", 0x4E0A2C28, true},
        };
        for (const auto& c : cases) {
            Image img;
            function(img, F, {adrp(8, F + 12, P), c.insn, ldrQ(2, 8, 0x2c0), kRet});
            img.floats(P + 0x2c0, {4.0f, 0.0f, 0.0f, 0.0f});
            if (c.kills_x8) {
                expectNotFound(c.name, img, F, 64);
            } else {
                expectK(c.name, img, F, 64, 4.0f);
            }
        }
    }
    {
        // A call may change x0-x18; x19-x28 survive it. The call goes to code
        // the reader refuses, so the follow finds nothing either.
        const uint64_t nowhere = 0x7FFFFFC;
        Image img;
        function(img, F, {adrp(0, F + 12, P), bl(F + 16, nowhere), ldrQ(2, 0, 0x2c0), kRet});
        img.floats(P + 0x2c0, {4.0f, 0.0f, 0.0f, 0.0f});
        expectNotFound("a page in x0 is gone after a call", img, F, 64);
    }
    {
        Image img;
        function(img, F, {adrp(0, F + 12, P), 0xD63F0120 /* blr x9 */, ldrQ(2, 0, 0x2c0), kRet});
        img.floats(P + 0x2c0, {4.0f, 0.0f, 0.0f, 0.0f});
        expectNotFound("a page in x0 is gone after blr", img, F, 64);
    }
    {
        const uint64_t nowhere = 0x7FFFFFC;
        Image img;
        function(img, F, {adrp(19, F + 12, P), bl(F + 16, nowhere), ldrQ(2, 19, 0x2c0), kRet});
        img.floats(P + 0x2c0, {4.0f, 0.0f, 0.0f, 0.0f});
        expectK("a page in x19 survives a call", img, F, 64, 4.0f);
    }
    {
        // ld1 post-indexed moves x8 on: the literal it then points at is not
        // the one the adrp + add named.
        const struct {
            const char* name;
            uint32_t insn;
            bool kills_x8;
        } cases[] = {
            {"ld1 {v0.4s}, [x8], #16 moves the x8 page", 0x4CDF7900, true},
            {"ld1 {v0.s}[1], [x8], #4 moves the x8 page", 0x0DDF9100, true},
            {"ld1 {v0.4s}, [x8] keeps the x8 page", 0x4C407900, false},
        };
        for (const auto& c : cases) {
            Image img;
            function(img, F, {adrp(8, F + 12, P), addX(8, 8, 0x2c0), c.insn, ldrQ(2, 8, 0), kRet});
            img.floats(P + 0x2c0, {4.0f, 0.0f, 0.0f, 0.0f});
            img.floats(P + 0x2d0, {7.0f, 1.0f, 0.0f, 0.0f});
            if (c.kills_x8) {
                expectNotFound(c.name, img, F, 64);
            } else {
                expectK(c.name, img, F, 64, 4.0f);
            }
        }
    }
    {
        // blraa is a call, not the end of a callee's straight-line code.
        const uint64_t a = 0x400000;
        Image img;
        function(img, F, {bl(F + 12, a), kRet});
        function(img, a, {0xD73F0909 /* blraa x8, x9 */, adrp(8, a + 16, P), ldrQ(0, 8, 0x100),
                          kRet});
        img.floats(P + 0x100, {4.0f, 0.0f, 0.0f, 0.0f});
        expectK("a callee is read past blraa", img, F, 64, 4.0f);
    }
    {
        // Three calls deep is past the follow.
        const uint64_t a = 0x400000, b = 0x401000, c = 0x402000;
        Image img;
        function(img, F, {bl(F + 12, a), kRet});
        function(img, a, {bl(a + 12, b), kRet});
        function(img, b, {bl(b + 12, c), kRet});
        function(img, c, {adrp(8, c + 12, P), ldrQ(0, 8, 0x100), kRet});
        img.floats(P + 0x100, {4.0f, 0.0f, 0.0f, 0.0f});
        expectNotFound("a constant three calls deep is not looked for", img, F, 64);
    }
    {
        // An fmov immediate in a callee means nothing.
        const uint64_t a = 0x400000;
        Image img;
        function(img, F, {bl(F + 12, a), kRet});
        function(img, a, {fmovS(0, kImm4), kRet});
        expectNotFound("fmov #4.0 in a callee does not count", img, F, 64);
    }
    {
        // A callee is read up to its first return, not into the next function.
        const uint64_t a = 0x400000;
        Image img;
        function(img, F, {bl(F + 12, a), kRet});
        function(img, a, {kRet, adrp(8, a + 16, P), ldrQ(0, 8, 0x100), kRet});
        img.floats(P + 0x100, {4.0f, 0.0f, 0.0f, 0.0f});
        expectNotFound("a callee ends at its first return", img, F, 64);
    }
    {
        // The window: 120 instructions, even inside the symbol.
        Image img;
        function(img, F, {}, 125);
        img.at(F + 125 * 4, {adrp(8, F + 500, P), ldrQ(2, 8, 0x2c0), kRet});
        img.floats(P + 0x2c0, {4.0f, 0.0f, 0.0f, 0.0f});
        expectNotFound("a literal past the first 120 instructions", img, F, 128 * 4);
    }
    {
        // Never past the symbol: the constant right after it is fenced off.
        Image img;
        function(img, F, {kNop, kRet});
        img.at(F + 20, {adrp(8, F + 20, P), ldrQ(2, 8, 0x2c0), kRet});
        img.floats(P + 0x2c0, {4.0f, 0.0f, 0.0f, 0.0f});
        img.fence_lo = F + 20;
        img.fence_hi = F + 0x1000;
        expectNotFound("nothing read past the symbol's size", img, F, 20);
    }
    {
        // The symbol says 400 bytes; there are 40.
        Image img;
        function(img, F, {}, 10);
        expectNotFound("truncated: symbol of 400 B, 40 B of code", img, F, 400);
    }
    {
        // Calls in circles end, and a call to the function itself is skipped.
        const uint64_t a = 0x400000, b = 0x401000;
        Image img;
        function(img, F, {bl(F + 12, F), bl(F + 16, a), kRet});
        function(img, a, {bl(a + 12, b), kRet});
        function(img, b, {bl(b + 12, a), kRet});
        expectNotFound("a call loop and a self call end", img, F, 64);
    }
    {
        Image img;
        function(img, F, {bl(F + 12, 0x7FFFFFC), kBtiC, kRet});
        expectNotFound("a call into unreadable code is skipped", img, F, 64);
    }
    {
        // Garbage, deterministic: whatever it decodes as, the probe returns,
        // with a scale in range or none, and never reads drawBlurRegion
        // beyond its size.
        int bad = 0;
        uint32_t x = 0x12345678;
        for (int run = 0; run < 200; ++run) {
            Image img;
            for (int k = 0; k < 160; ++k) {
                x = x * 1664525u + 1013904223u;
                img.code[F + 4 * static_cast<uint64_t>(k)] = x;
            }
            for (int k = 0; k < 64; ++k) {
                x = x * 1664525u + 1013904223u;
                img.data[P + static_cast<uint64_t>(k)] = static_cast<uint8_t>(x >> 24);
            }
            const glass::BlurScale b = glass::probeBlurScale(F, 480, readImage, &img);
            const bool sane = b.per_axis || !b.found || (b.inverse > 1.0f && b.inverse < 10.0f);
            if (!sane || b.how.empty()) ++bad;
        }
        glass::BlurScale summary;
        summary.how = std::to_string(200 - bad) + " of 200 random functions handled";
        report(bad == 0, "garbage: 200 random 160-word functions", summary);
    }
    {
        glass::BlurScale b = glass::probeBlurScale(F, 64, refuseAll, nullptr);
        report(!b.found && b.inverse == 4.0f, "a reader that refuses everything", b);
        b = glass::probeBlurScale(F, 0, refuseAll, nullptr);
        report(!b.found && b.inverse == 4.0f, "a symbol without a size", b);
        b = glass::probeBlurScale(F + 2, 64, refuseAll, nullptr);
        report(!b.found && b.inverse == 4.0f, "a misaligned address", b);
        b = glass::probeBlurScale(~uint64_t{0} - 8, 64, refuseAll, nullptr);
        report(!b.found && b.inverse == 4.0f, "an address that wraps", b);
    }
    {
        glass::Symbol s;
        s.value = 0x1000;
        s.size = 64;
        glass::BlurScale b = glass::probeBlurScaleInFile("/proc/self/exe", s);
#if defined(__aarch64__)
        report(true, "this test's own binary on disk (AArch64 host: not checked)", b);
#else
        report(!b.found && b.inverse == 4.0f, "an x86-64 binary on disk", b);
        b = glass::probeBlurScaleInProcess("/proc/self/exe", 0, s);
        report(!b.found && b.inverse == 4.0f, "in process on an x86-64 host", b);
#endif
        b = glass::probeBlurScaleInFile("/nonexistent/surfaceflinger", s);
        report(!b.found && b.inverse == 4.0f, "a file that does not exist", b);
    }
    printf("%s\n", failures == 0 ? "blur_scale_test: all passed" : "blur_scale_test: FAILURES");
    return failures == 0 ? 0 : 1;
}

// ---- corpus -----------------------------------------------------------------

// Host-side xz, as resolve_test does.
bool inflateViaXz(const uint8_t* in, size_t in_size, std::vector<uint8_t>* out, void*) {
    char in_path[] = "/tmp/glass_mdi_XXXXXX";
    const int fd = mkstemp(in_path);
    if (fd < 0) return false;
    const bool wrote = write(fd, in, in_size) == static_cast<ssize_t>(in_size);
    close(fd);
    if (!wrote) {
        unlink(in_path);
        return false;
    }
    const std::string cmd = std::string("xz -d -c ") + in_path;
    FILE* p = popen(cmd.c_str(), "r");
    if (p == nullptr) {
        unlink(in_path);
        return false;
    }
    out->clear();
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), p)) > 0) out->insert(out->end(), buf, buf + n);
    const int rc = pclose(p);
    unlink(in_path);
    return rc == 0 && !out->empty();
}

bool contains(const std::string& s, const char* what) { return s.find(what) != std::string::npos; }

// android::renderengine::skia::BlurFilter::setupBlurPaint(...) as
// "BlurFilter::setupBlurPaint".
std::string shortName(const std::string& mangled) {
    int status = 0;
    char* d = abi::__cxa_demangle(mangled.c_str(), nullptr, nullptr, &status);
    std::string s = status == 0 && d != nullptr ? d : mangled;
    free(d);
    const std::string ns = "android::renderengine::skia::";
    if (s.compare(0, ns.size(), ns) == 0) s.erase(0, ns.size());
    const size_t paren = s.find('(');
    if (paren != std::string::npos) s.erase(paren);
    return s;
}

// The names of the functions a "how" line went through (the addresses after
// "->"), from RenderEngine's symbols; "?" for one outside them.
std::string pathNames(const glass::SymbolResolver& syms, const std::string& how) {
    if (!contains(how, " -> ")) return "";
    std::map<uint64_t, std::string> by_addr;
    for (const char* prefix :
         {"_ZN7android12renderengine4skia", "_ZNK7android12renderengine4skia"}) {
        for (const std::string& n : syms.findByPrefix(prefix)) {
            const glass::Symbol s = syms.find(n);
            if (s.valid()) by_addr.emplace(s.value, n);
        }
    }
    std::string out;
    for (size_t pos = how.find(" -> 0x"); pos != std::string::npos; pos = how.find(" -> 0x", pos)) {
        pos += 4;
        const auto it = by_addr.find(strtoull(how.c_str() + pos, nullptr, 16));
        out += out.empty() ? "" : " -> ";
        out += it == by_addr.end() ? "?" : shortName(it->second);
    }
    return out;
}

// What each family is known to do (captures/sf-corpus/reports/top5.md,
// finding 1, and the follow: CP4A and Evolution X 12.4 moved the literal out
// of drawBlurRegion). `via` is pathNames() of the result.
std::string expectation(const std::string& build, const glass::BlurScale& b,
                        const std::string& via) {
    const bool four = b.found && !b.per_axis && b.inverse == 4.0f;
    if (contains(build, "lunaris")) {
        return b.found && !b.per_axis && std::fabs(b.inverse - 4.545455f) < 1e-4f && via.empty()
                   ? ""
                   : "want 4.54545 in drawBlurRegion";
    }
    if (contains(build, "infinityx")) return b.per_axis ? "" : "want per axis";
    if (contains(build, "CP4A")) {
        return four && via == "BlurFilter::setupBlurPaint -> getShaderTransform"
                   ? ""
                   : "want 4 through setupBlurPaint -> getShaderTransform";
    }
    if (contains(build, "evox") && contains(build, "-12.")) {
        return four && via == "getShaderTransform" ? "" : "want 4 through getShaderTransform";
    }
    return four && via.empty() ? "" : "want 4 in drawBlurRegion";
}

int corpus(const std::string& dir) {
    std::vector<std::string> paths;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(dir, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (it->is_regular_file() && it->path().filename() == "surfaceflinger") {
            paths.push_back(it->path().string());
        }
    }
    std::sort(paths.begin(), paths.end());
    int bad = 0, n_const = 0, n_axis = 0, n_follow = 0, n_none = 0, unjudged = 0;
    for (const std::string& path : paths) {
        std::string build = path.substr(dir.size());
        while (!build.empty() && build[0] == '/') build.erase(0, 1);
        build = build.substr(0, build.size() - std::string("/surfaceflinger").size());

        glass::SymbolResolver syms;
        if (!syms.loadFromFile(path, inflateViaXz, nullptr)) {
            printf("%-52s no symbols (%s)\n", build.c_str(), syms.error().c_str());
            ++bad;
            continue;
        }
        // As glasscheck and resolve_test resolve it: the ABI asked of the file.
        glass::sf::Resolution res;
        glass::sf::resolve(syms, glass::sf::fileAbiProbe, const_cast<char*>(path.c_str()), &res);
        glass::Symbol dbr;
        if (res.shape >= 0) {
            dbr = res.sym(glass::sf::kHookShapes[res.shape].entry);
        } else {
            // judge() refused the build (the engine would not run there); the
            // first hook entry that resolved, so the row still says something.
            build += " *";
            ++unjudged;
            for (const glass::sf::HookShape& h : glass::sf::kHookShapes) {
                if (res.has(h.entry)) {
                    dbr = res.sym(h.entry);
                    break;
                }
            }
        }
        const glass::BlurScale b = glass::probeBlurScaleInFile(path, dbr);
        const std::string via = pathNames(syms, b.how);
        const std::string wrong = expectation(build, b, via);
        if (!wrong.empty()) ++bad;
        if (b.per_axis) {
            ++n_axis;
        } else if (!b.found) {
            ++n_none;
        } else if (contains(b.how, " -> ")) {
            ++n_follow;
        } else {
            ++n_const;
        }
        printf("%-52s k=%-8g %-8s %-9s %s%s%s%s%s\n", build.c_str(), b.inverse,
               b.per_axis ? "per-axis" : "-", b.found ? "found" : "not-found", b.how.c_str(),
               via.empty() ? "" : " = ", via.c_str(), wrong.empty() ? "" : "   <-- ",
               wrong.c_str());
    }
    printf("\n%zu builds: %d constant in drawBlurRegion, %d through a call, %d per axis, "
           "%d not found; %d not as expected\n",
           paths.size(), n_const, n_follow, n_axis, n_none, bad);
    if (unjudged > 0) {
        printf("* %d builds judge() refuses: probed at the first hook entry that resolved\n",
               unjudged);
    }
    return bad == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && strcmp(argv[1], "--corpus") == 0) {
        return corpus(argc >= 3 ? argv[2] : "captures/sf-corpus");
    }
    if (argc != 1) {
        fprintf(stderr, "usage: %s [--corpus [dir]]\n", argv[0]);
        return 2;
    }
    return unitTests();
}
