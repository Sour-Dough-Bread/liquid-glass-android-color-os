#include "blur_scale.h"

#include <elf.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace glass {
namespace {

// ---- limits ---------------------------------------------------------------

constexpr size_t kWindow = 120;            // instructions looked at in each function
constexpr uint64_t kMaxBody = 4096;        // drawBlurRegion instructions counted for fdiv
constexpr int kPerAxisFdiv = 3;            // Infinity-X: one per axis, plus stock's one
constexpr int kFollowDepth = 2;            // CP4A: setupBlurPaint -> getShaderTransform
constexpr size_t kMaxCallsPerFunction = 16;
constexpr int kMaxFollowed = 24;           // functions followed, in all
constexpr size_t kFollowBudget = 1536;     // instructions read in followed functions, in all
constexpr size_t kChunk = 16;              // instructions per read

constexpr uint64_t kMaxAddress = std::numeric_limits<uint64_t>::max();

// ---- A64, only what the probe needs ----------------------------------------

uint32_t rd(uint32_t i) { return i & 31; }
uint32_t rn(uint32_t i) { return (i >> 5) & 31; }
uint32_t rt2(uint32_t i) { return (i >> 10) & 31; }

bool isAdrp(uint32_t i) { return (i & 0x9F000000u) == 0x90000000u; }

int64_t adrpOffset(uint32_t i) {
    int64_t imm = (static_cast<int64_t>((i >> 5) & 0x7FFFF) << 2) | ((i >> 29) & 3);
    if (imm & (int64_t{1} << 20)) imm -= int64_t{1} << 21;
    return imm * 4096;
}

// ADD Xd, Xn, #imm{, lsl #12}: 64-bit, flags untouched.
bool isAddImm64(uint32_t i) { return (i & 0xFF800000u) == 0x91000000u; }

uint64_t addImm(uint32_t i) {
    const uint64_t imm = (i >> 10) & 0xFFF;
    return (i & (1u << 22)) != 0 ? imm << 12 : imm;
}

// LDR Qt, [Xn, #imm]: SIMD&FP, 128-bit, unsigned offset, imm12 scaled by 16.
bool isLdrQ(uint32_t i) { return (i & 0xFFC00000u) == 0x3DC00000u; }
uint64_t ldrQOffset(uint32_t i) { return static_cast<uint64_t>((i >> 10) & 0xFFF) * 16; }

// FMOV Sd, #imm. imm8 = a:b:cd:efgh is +-(16 + efgh) / 16 * 2^r, with r = cd + 1
// when b is clear and cd - 3 when it is set (VFPExpandImm).
bool isFmovS(uint32_t i) { return (i & 0xFFE01FE0u) == 0x1E201000u; }

float fmovValue(uint32_t i) {
    const uint32_t imm8 = (i >> 13) & 0xFF;
    const int cd = static_cast<int>((imm8 >> 4) & 3);
    const int r = (imm8 & 0x40) != 0 ? cd - 3 : cd + 1;
    const float v = std::ldexp(static_cast<float>(16 + (imm8 & 15)) / 16.0f, r);
    return (imm8 & 0x80) != 0 ? -v : v;
}

// FDIV in every form: scalar half, single and double; vector.
bool isFdiv(uint32_t i) {
    return (i & 0xFF20FC00u) == 0x1E201800u ||  // scalar
           (i & 0xBFA0FC00u) == 0x2E20FC00u ||  // vector, single or double
           (i & 0xBFE0FC00u) == 0x2E403C00u;    // vector, half
}

bool isBl(uint32_t i) { return (i & 0xFC000000u) == 0x94000000u; }

int64_t branchOffset(uint32_t i) {
    int64_t imm = i & 0x03FFFFFF;
    if (imm & 0x02000000) imm -= 0x04000000;
    return imm * 4;
}

// BLR and its PAC forms: opc (bits 24:21) 0001 for BLR, BLRAAZ and BLRABZ, 1001
// for BLRAA and BLRAB.
bool isBlr(uint32_t i) { return (i & 0xFE000000u) == 0xD6000000u && ((i >> 21) & 7) == 1; }

// Where a function's straight-line code ends: B, a register branch that is not
// a call (BR, RET and their PAC forms, ERET), an exception, UDF.
bool endsStraightLine(uint32_t i) {
    if (i == 0) return true;                                   // udf #0
    if ((i & 0xFC000000u) == 0x14000000u) return true;         // B
    if ((i & 0xFE000000u) == 0xD6000000u) return !isBlr(i);
    if ((i & 0xFF000000u) == 0xD4000000u) return true;         // SVC, BRK, HLT, ...
    return false;
}

// Whether `i` may write general register `r` (0-30). False only for forms known
// not to: keeping a stale address by mistake would send a literal read to the
// wrong place (which the reader bounds and the {k,0,0,0} test filters), while
// dropping one too many only loses a match.
bool mayWrite(uint32_t i, uint32_t r) {
    if ((i & 0xFFFFF01Fu) == 0xD503201Fu) return false;                    // hints
    if ((i & 0x7C000000u) == 0x14000000u) return (i >> 31) != 0 && r == 30;  // B, BL
    if ((i & 0xFF000010u) == 0x54000000u) return false;                    // B.cond
    if ((i & 0x7E000000u) == 0x34000000u) return false;                    // CBZ, CBNZ
    if ((i & 0x7E000000u) == 0x36000000u) return false;                    // TBZ, TBNZ
    if ((i & 0xFE000000u) == 0xD6000000u) return isBlr(i) && r == 30;
    if ((i & 0xFF000000u) == 0xD4000000u) return false;                    // exceptions
    if ((i & 0xFFC00000u) == 0xD5000000u) return (i & (1u << 21)) != 0 && rd(i) == r;  // MRS
    if ((i & 0x0A000000u) == 0x08000000u) {                                // loads, stores
        const bool vector = (i & 0x04000000u) != 0;
        if ((i & 0x3B000000u) == 0x18000000u) return !vector && rd(i) == r;  // literal
        const bool pair = (i & 0x3A000000u) == 0x28000000u;
        uint32_t index = 0;  // 01 post-indexed, 11 pre-indexed: the base is written
        if (pair) {
            index = (i >> 23) & 3;
        } else if ((i & 0x3B200000u) == 0x38000000u) {
            index = (i >> 10) & 3;
        }
        if ((index == 1 || index == 3) && rn(i) == r) return true;
        // LD1-LD4 and ST1-ST4, both structure forms, post-indexed: Rn is written back.
        if ((i & 0xBE800000u) == 0x0C800000u && rn(i) == r) return true;
        if ((i & 0x3F000000u) == 0x08000000u) {  // exclusive: loaded registers, store status
            return rd(i) == r || rt2(i) == r || ((i >> 16) & 31) == r;
        }
        if (vector) return false;  // the data registers are SIMD&FP
        if (pair) return (i & (1u << 22)) != 0 && (rd(i) == r || rt2(i) == r);
        if ((i & 0x3B200C00u) == 0x38200000u) {  // atomics write Rt, and Rs for the swaps
            return rd(i) == r || ((i >> 16) & 31) == r;
        }
        return ((i >> 22) & 3) != 0 && rd(i) == r;  // opc 00 is a store
    }
    if ((i & 0x0E000000u) == 0x0E000000u) {  // SIMD&FP data processing: bits 4:0 name a V
        // register, except in the conversions between floating point and a
        // general register (FCVTZS w8, FMOV x8, d0, FJCVTZS; SCVTF and FMOV s0,
        // w8 are in the same groups and counted too) and in SMOV/UMOV (with
        // DUP and INS, from the same group). EvoX 12.4's getShaderTransform has
        // `fmov s8, s0` between `adrp x8` and the `ldr q0, [x8]`.
        if ((i & 0x5F000000u) == 0x1E000000u) {             // 0 S 11110: scalar floating point
            const bool fixed_point = (i & 0x00200000u) == 0;  // FP <-> fixed point
            const bool integer = (i & 0x0000FC00u) == 0;      // FP <-> integer, with bit 21
            return (fixed_point || integer) && rd(i) == r;
        }
        if ((i & 0x9FE08400u) == 0x0E000400u) return rd(i) == r;  // Advanced SIMD copy
        return false;
    }
    return rd(i) == r;  // data processing: the destination is bits 4:0
}

// ---- reading ---------------------------------------------------------------

struct Reader {
    BlurScaleReader fn;
    void* user;

    bool code(uint64_t vaddr, uint32_t* out, size_t count) const {
        if (count == 0 || (vaddr & 3) != 0 || vaddr > kMaxAddress - count * 4) return false;
        return fn(vaddr, reinterpret_cast<uint8_t*>(out), count * 4, BlurScaleRead::kCode, user);
    }
    bool constant(uint64_t vaddr, uint8_t* out, size_t n) const {
        if (vaddr > kMaxAddress - n) return false;
        return fn(vaddr, out, n, BlurScaleRead::kConstant, user);
    }
};

std::string hex(uint64_t v) {
    char b[24];
    snprintf(b, sizeof(b), "0x%llx", static_cast<unsigned long long>(v));
    return b;
}

std::string num(double v) {
    char b[32];
    snprintf(b, sizeof(b), "%g", v);
    return b;
}

// ---- one function's window -------------------------------------------------

struct Hit {
    bool found = false;
    float k = 0.0f;
    uint64_t at = 0;       // the instruction that produced it
    uint64_t literal = 0;  // where the literal is; 0 for an fmov immediate
};

// Addresses the code built with adrp (and adds to it), per general register.
struct Pages {
    uint64_t addr[31] = {};
    uint32_t known = 0;
    bool has(uint32_t r) const { return r < 31 && ((known >> r) & 1u) != 0; }
    void set(uint32_t r, uint64_t a) {
        if (r >= 31) return;
        addr[r] = a;
        known |= 1u << r;
    }
    void kill(uint32_t r) {
        if (r < 31) known &= ~(1u << r);
    }
    // After a call: the callee may change x0-x18 (arguments, results, the
    // scratch and intra-procedure-call registers, x18 where the platform does
    // not reserve it), and BL or BLR wrote x30. Only x19-x28 survive it.
    void call() {
        known &= ~((1u << 19) - 1u);
        kill(30);
    }
};

// Looks at up to `max` instructions of one function from `entry`. With `end`
// (drawBlurRegion: its symbol's end) it never reads at or past `end`; without
// (a function reached by a call, whose size is unknown) it stops after the
// first instruction that ends straight-line code. On the first constant it
// fills `hit` and stops; otherwise it appends the direct calls it passed to
// `calls`. Returns how many instructions it read.
size_t scanWindow(const Reader& in, uint64_t entry, uint64_t end, size_t max, bool fmov_counts,
                  Hit* hit, std::vector<uint64_t>* calls) {
    Pages pages;
    uint64_t pc = entry;
    size_t seen = 0;
    while (seen < max) {
        size_t want = std::min(kChunk, max - seen);
        if (end != 0) {
            if (pc >= end) break;
            want = static_cast<size_t>(std::min<uint64_t>(want, (end - pc) / 4));
            if (want == 0) break;
        }
        uint32_t words[kChunk];
        size_t n = want;
        while (n > 0 && !in.code(pc, words, n)) n /= 2;  // near a segment's end: what there is
        if (n == 0) break;
        for (size_t k = 0; k < n; ++k) {
            const uint32_t i = words[k];
            const uint64_t at = pc + 4 * k;
            ++seen;
            if (isAdrp(i)) {
                const uint64_t page =
                    (at & ~uint64_t{0xFFF}) + static_cast<uint64_t>(adrpOffset(i));
                pages.set(rd(i), page);
                continue;
            }
            if (isAddImm64(i) && pages.has(rn(i))) {
                const uint64_t base = pages.addr[rn(i)];
                const uint64_t imm = addImm(i);
                if (base <= kMaxAddress - imm) {
                    pages.set(rd(i), base + imm);
                } else {
                    pages.kill(rd(i));
                }
                continue;
            }
            if (isLdrQ(i)) {
                if (pages.has(rn(i))) {
                    const uint64_t base = pages.addr[rn(i)];
                    const uint64_t off = ldrQOffset(i);
                    uint8_t bytes[16];
                    if (base <= kMaxAddress - off &&
                        in.constant(base + off, bytes, sizeof(bytes))) {
                        float f[4];
                        memcpy(f, bytes, sizeof(f));
                        if (f[0] > 1.0f && f[0] < 10.0f && f[1] == 0.0f && f[2] == 0.0f &&
                            f[3] == 0.0f) {
                            *hit = {true, f[0], at, base + off};
                            return seen;
                        }
                    }
                }
                continue;  // writes a SIMD&FP register only
            }
            if (fmov_counts && isFmovS(i)) {
                const float v = fmovValue(i);
                if (v > 2.0f && v < 10.0f) {
                    *hit = {true, v, at, 0};
                    return seen;
                }
                continue;
            }
            if (isBl(i)) {
                const int64_t off = branchOffset(i);
                const uint64_t target = at + static_cast<uint64_t>(off);
                const bool wrapped = off >= 0 ? target < at : target > at;
                if (!wrapped && calls != nullptr && calls->size() < kMaxCallsPerFunction &&
                    std::find(calls->begin(), calls->end(), target) == calls->end()) {
                    calls->push_back(target);
                }
                pages.call();
                continue;
            }
            if (end == 0 && endsStraightLine(i)) return seen;
            if (isBlr(i)) {
                pages.call();
                continue;
            }
            for (uint32_t r = 0; r < 31; ++r) {
                if (pages.has(r) && mayWrite(i, r)) pages.kill(r);
            }
        }
        pc += 4 * n;
    }
    return seen;
}

// fdiv in the whole of drawBlurRegion, never past its symbol (nor past
// kMaxBody instructions of it), and up to the first word the reader refuses.
int countFdiv(const Reader& in, uint64_t vaddr, uint64_t size) {
    uint64_t left = std::min<uint64_t>(size / 4, kMaxBody);
    uint64_t pc = vaddr;
    int count = 0;
    while (left > 0) {
        uint32_t words[kChunk];
        size_t n = static_cast<size_t>(std::min<uint64_t>(kChunk, left));
        while (n > 0 && !in.code(pc, words, n)) n /= 2;  // near a segment's end: what there is
        if (n == 0) break;
        for (size_t k = 0; k < n; ++k) count += isFdiv(words[k]) ? 1 : 0;
        pc += 4 * n;
        left -= n;
    }
    return count;
}

struct FollowState {
    std::vector<uint64_t> visited;
    size_t budget = kFollowBudget;
    int functions = 0;
};

// The functions `calls` names, in call order, depth first, `kFollowDepth` deep.
// Only the literal form counts in them: an fmov immediate in some callee is too
// common to mean anything. `path` gets the functions from drawBlurRegion's
// callee down to the one holding the constant.
bool follow(const Reader& in, const std::vector<uint64_t>& calls, int depth, FollowState* st,
            Hit* hit, std::vector<uint64_t>* path) {
    for (const uint64_t target : calls) {
        if (st->budget == 0 || st->functions >= kMaxFollowed) return false;
        if (std::find(st->visited.begin(), st->visited.end(), target) != st->visited.end()) {
            continue;
        }
        st->visited.push_back(target);
        ++st->functions;
        std::vector<uint64_t> inner;
        const size_t used = scanWindow(in, target, 0, std::min(kWindow, st->budget),
                                       /*fmov_counts=*/false, hit, &inner);
        st->budget -= std::min(used, st->budget);
        if (hit->found) {
            path->push_back(target);
            return true;
        }
        if (depth < kFollowDepth && follow(in, inner, depth + 1, st, hit, path)) {
            path->insert(path->begin(), target);
            return true;
        }
    }
    return false;
}

std::string describeHit(const Hit& h) {
    std::string s = num(h.k) + " (kInputScale " + num(1.0 / h.k) + ") from ";
    if (h.literal != 0) {
        s += "the literal at " + hex(h.literal) + ", loaded at " + hex(h.at);
    } else {
        s += "fmov at " + hex(h.at);
    }
    return s;
}

// ---- segments of an ELF64 file ---------------------------------------------

struct LoadSegment {
    uint64_t vaddr = 0;
    uint64_t filesz = 0;
    uint64_t offset = 0;
    uint32_t flags = 0;
};

// Readable PT_LOAD segments, from the headers only. False on anything that is
// not a little-endian ELF64 the probe can trust; *machine is set as soon as the
// header has been read.
bool loadSegments(int fd, std::vector<LoadSegment>* out, uint16_t* machine) {
    *machine = 0;
    Elf64_Ehdr eh{};
    if (pread(fd, &eh, sizeof(eh), 0) != static_cast<ssize_t>(sizeof(eh))) return false;
    if (memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 || eh.e_ident[EI_CLASS] != ELFCLASS64 ||
        eh.e_ident[EI_DATA] != ELFDATA2LSB) {
        return false;
    }
    *machine = eh.e_machine;
    if (eh.e_phentsize != sizeof(Elf64_Phdr) || eh.e_phnum == 0 || eh.e_phnum > 64 ||
        eh.e_phoff > static_cast<uint64_t>(std::numeric_limits<off_t>::max())) {
        return false;
    }
    std::vector<Elf64_Phdr> ph(eh.e_phnum);
    const size_t bytes = ph.size() * sizeof(Elf64_Phdr);
    if (pread(fd, ph.data(), bytes, static_cast<off_t>(eh.e_phoff)) !=
        static_cast<ssize_t>(bytes)) {
        return false;
    }
    for (const Elf64_Phdr& p : ph) {
        if (p.p_type != PT_LOAD || p.p_filesz == 0 || (p.p_flags & PF_R) == 0) continue;
        if (p.p_vaddr > kMaxAddress - p.p_filesz) continue;
        if (p.p_offset > static_cast<uint64_t>(std::numeric_limits<off_t>::max()) - p.p_filesz) {
            continue;
        }
        out->push_back({p.p_vaddr, p.p_filesz, p.p_offset, p.p_flags});
    }
    return !out->empty();
}

// The segment that holds all of [vaddr, vaddr + n) and may be read that way:
// executable for code, not writable for a constant.
const LoadSegment* segmentFor(const std::vector<LoadSegment>& segs, uint64_t vaddr, size_t n,
                              BlurScaleRead what) {
    for (const LoadSegment& s : segs) {
        const bool allowed = what == BlurScaleRead::kCode ? (s.flags & PF_X) != 0
                                                          : (s.flags & PF_W) == 0;
        if (!allowed) continue;
        if (vaddr >= s.vaddr && n <= s.filesz && vaddr - s.vaddr <= s.filesz - n) return &s;
    }
    return nullptr;
}

struct FileImage {
    int fd = -1;
    std::vector<LoadSegment> segs;
};

bool readFromFile(uint64_t vaddr, uint8_t* out, size_t n, BlurScaleRead what, void* user) {
    const auto* f = static_cast<const FileImage*>(user);
    const LoadSegment* s = segmentFor(f->segs, vaddr, n, what);
    if (s == nullptr) return false;
    const off_t off = static_cast<off_t>(s->offset + (vaddr - s->vaddr));
    return pread(f->fd, out, n, off) == static_cast<ssize_t>(n);
}

#if defined(__aarch64__)
struct ProcessImage {
    uintptr_t bias = 0;
    std::vector<LoadSegment> segs;
};

bool readFromProcess(uint64_t vaddr, uint8_t* out, size_t n, BlurScaleRead what, void* user) {
    const auto* p = static_cast<const ProcessImage*>(user);
    if (segmentFor(p->segs, vaddr, n, what) == nullptr) return false;
    if (vaddr > std::numeric_limits<uintptr_t>::max() - p->bias) return false;
    memcpy(out, reinterpret_cast<const void*>(p->bias + static_cast<uintptr_t>(vaddr)), n);
    return true;
}
#endif

BlurScale notFound(std::string how) {
    BlurScale out;
    out.how = std::move(how);
    return out;
}

}  // namespace

BlurScale probeBlurScale(uint64_t vaddr, uint64_t size, BlurScaleReader read, void* user) {
    if (read == nullptr || vaddr == 0 || size < 4 || (vaddr & 3) != 0 ||
        vaddr > kMaxAddress - size) {
        return notFound("no drawBlurRegion to read; 4 assumed");
    }
    const Reader in{read, user};

    // 1. A constant in drawBlurRegion's own first instructions.
    Hit hit;
    std::vector<uint64_t> calls;
    const size_t window = scanWindow(in, vaddr, vaddr + size, kWindow, /*fmov_counts=*/true, &hit,
                                     &calls);
    if (window == 0) {
        return notFound("drawBlurRegion's code at " + hex(vaddr) + " could not be read; 4 assumed");
    }
    BlurScale out;
    if (hit.found) {
        out.inverse = hit.k;
        out.found = true;
        out.how = describeHit(hit) + " in drawBlurRegion";
        return out;
    }

    // 2. No constant: does it divide blurRect by the image's size, per axis?
    const int fdiv = countFdiv(in, vaddr, size);
    if (fdiv >= kPerAxisFdiv) {
        out.per_axis = true;
        out.found = true;
        out.how = "per axis: no constant, " + std::to_string(fdiv) +
                  " fdiv in drawBlurRegion (blurRect over the image's size)";
        return out;
    }

    // 3. The functions it calls.
    FollowState st;
    st.visited.push_back(vaddr);
    std::vector<uint64_t> path;
    if (follow(in, calls, 1, &st, &hit, &path)) {
        out.inverse = hit.k;
        out.found = true;
        out.how = describeHit(hit) + ", in drawBlurRegion";
        for (const uint64_t f : path) out.how += " -> " + hex(f);
        return out;
    }

    // 4. Nothing: AOSP's value.
    return notFound("not found: no constant in drawBlurRegion (" + std::to_string(window) +
                    " instructions, " + std::to_string(fdiv) + " fdiv) nor in the " +
                    std::to_string(st.functions) + " functions it calls; 4 assumed");
}

BlurScale probeBlurScaleInFile(const std::string& path, const Symbol& draw_blur_region) {
    if (!draw_blur_region.valid() || draw_blur_region.size == 0) {
        return notFound("no drawBlurRegion symbol with a size; 4 assumed");
    }
    FileImage img;
    img.fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (img.fd < 0) return notFound("cannot open " + path + "; 4 assumed");
    uint16_t machine = 0;
    const bool parsed = loadSegments(img.fd, &img.segs, &machine);
    BlurScale out;
    if (machine != EM_AARCH64) {
        out = notFound("not an AArch64 ELF (e_machine " + std::to_string(machine) + "); 4 assumed");
    } else if (!parsed) {
        out = notFound("no readable segments in " + path + "; 4 assumed");
    } else {
        out = probeBlurScale(draw_blur_region.value, draw_blur_region.size, readFromFile, &img);
    }
    close(img.fd);
    return out;
}

BlurScale probeBlurScaleInProcess(const std::string& exe_path, uintptr_t bias,
                                  const Symbol& draw_blur_region) {
#if defined(__aarch64__)
    if (!draw_blur_region.valid() || draw_blur_region.size == 0) {
        return notFound("no drawBlurRegion symbol with a size; 4 assumed");
    }
    ProcessImage img;
    img.bias = bias;
    const int fd = open(exe_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return notFound("cannot open " + exe_path + "; 4 assumed");
    uint16_t machine = 0;
    const bool parsed = loadSegments(fd, &img.segs, &machine);
    close(fd);
    if (!parsed || machine != EM_AARCH64) {
        return notFound("no readable AArch64 segments in " + exe_path + "; 4 assumed");
    }
    return probeBlurScale(draw_blur_region.value, draw_blur_region.size, readFromProcess, &img);
#else
    (void)exe_path;
    (void)bias;
    (void)draw_blur_region;
    return notFound("not AArch64: the probe reads A64 code only; 4 assumed");
#endif
}

}  // namespace glass
