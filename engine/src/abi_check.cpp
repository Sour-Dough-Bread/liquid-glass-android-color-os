#include "abi_check.h"

#include <elf.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "elf_syms.h"

namespace glass {
namespace {

constexpr uint32_t kX8 = 8;

uint32_t rd(uint32_t i) { return i & 31; }          // Rd / Rt
uint32_t rn(uint32_t i) { return (i >> 5) & 31; }   // Rn
uint32_t rt2(uint32_t i) { return (i >> 10) & 31; } // Rt2 of a pair

bool isBranchOrStop(uint32_t i) {
    if (i == 0) return true;                               // udf #0
    if ((i & 0x7C000000) == 0x14000000) return true;       // B, BL
    if ((i & 0xFE000000) == 0xD6000000) return true;       // BR, BLR, RET, ...
    if ((i & 0x7E000000) == 0x34000000) return true;       // CBZ, CBNZ
    if ((i & 0x7E000000) == 0x36000000) return true;       // TBZ, TBNZ
    if ((i & 0xFF000010) == 0x54000000) return true;       // B.cond
    if ((i & 0xFF000000) == 0xD4000000) return true;       // SVC, BRK, ...
    return false;
}

bool isHint(uint32_t i) {
    // NOP, BTI, PACIASP and the rest of the HINT space: no general registers.
    return (i & 0xFFFFF01F) == 0xD503201F;
}

bool isLoadStore(uint32_t i) { return (i & 0x0A000000) == 0x08000000; }
bool isLoadLiteral(uint32_t i) { return (i & 0x3B000000) == 0x18000000; }
bool isVectorLoadStore(uint32_t i) { return (i & 0x04000000) != 0; }  // V bit

// Integer stores whose Rt (and Rt2) are READ, by exact encoding family. Only
// these count as "x8 was stored"; anything else with Rt == x8 is treated as a
// possible write.
bool isIntegerStoreReading(uint32_t i, uint32_t reg) {
    // STR (immediate, unsigned offset), 32/64-bit:  1x11 1001 00..
    if ((i & 0xBFC00000) == 0xB9000000) return rd(i) == reg;
    // STR/STUR (unscaled, pre/post-index, register), 32/64-bit: 1x11 1000 000.
    if ((i & 0xBFE00000) == 0xB8000000) return rd(i) == reg;
    // STP 32/64-bit: signed offset, pre-index, post-index.
    const uint32_t stp = i & 0x7FC00000;
    if (stp == 0x29000000 || stp == 0x29800000 || stp == 0x28800000) {
        return rd(i) == reg || rt2(i) == reg;
    }
    return false;
}

bool isIntegerStore(uint32_t i) {
    if ((i & 0xBFC00000) == 0xB9000000) return true;
    if ((i & 0xBFE00000) == 0xB8000000) return true;
    const uint32_t stp = i & 0x7FC00000;
    return stp == 0x29000000 || stp == 0x29800000 || stp == 0x28800000;
}

// A read of x8 by one of the narrow, unambiguous forms.
bool readsX8(uint32_t i) {
    // MOV Xd, X8 (ORR Xd, XZR, X8).
    if ((i & 0xFFFFFFE0) == 0xAA0803E0) return true;
    // ADD/SUB (immediate) with Rn = x8, including MOV to/from SP.
    if ((i & 0x1F800000) == 0x11000000 && rn(i) == kX8) return true;
    // Any load or store addressed through x8 (not the literal form, which has
    // no base register).
    if (isLoadStore(i) && !isLoadLiteral(i) && rn(i) == kX8) return true;
    // x8 itself stored somewhere.
    if (isIntegerStoreReading(i, kX8)) return true;
    return false;
}

bool mayWriteX8(uint32_t i) {
    if (isHint(i)) return false;
    if (isLoadStore(i)) {
        if (isVectorLoadStore(i) && !isLoadLiteral(i)) {
            // SIMD/FP transfer: Rt names a vector register. A pair load's Rt2
            // likewise. Only the base can be a general register, and it was
            // checked as a read above.
            return false;
        }
        if (isLoadLiteral(i)) return !isVectorLoadStore(i) && rd(i) == kX8;
        if (isIntegerStore(i)) return false;  // Rt is read, not written
        // Which fields a load writes depends on its class, and bits 10-20 are
        // an immediate in the common single-register forms: reading them as
        // registers called `ldr x10, [x0, #0x2310]` a write of x8 (Android
        // 15's getTotalMatrix). Pairs write Rt and Rt2; exclusives and
        // compare-and-swap may write Rt, Rt2 and Rs (a store-exclusive's
        // status); atomics write Rt and, for the swaps, Rs; every other load
        // writes Rt alone (a writeback base is a read of it first, caught
        // above).
        const bool pair = (i & 0x3A000000) == 0x28000000;
        const bool exclusive = (i & 0x3F000000) == 0x08000000;
        const bool atomic = (i & 0x3B200C00) == 0x38200000;
        const uint32_t rs = (i >> 16) & 31;
        if (pair) return rd(i) == kX8 || rt2(i) == kX8;
        if (exclusive) return rd(i) == kX8 || rt2(i) == kX8 || rs == kX8;
        if (atomic) return rd(i) == kX8 || rs == kX8;
        return rd(i) == kX8;
    }
    // Everything else: the destination field is bits 4:0. Compares write XZR
    // (31) there, so they pass; an FP/SIMD op writing v8 is a false alarm,
    // which only makes the answer more cautious.
    return rd(i) == kX8;
}

std::string hex(uint32_t v) {
    char b[16];
    snprintf(b, sizeof(b), "%08x", v);
    return b;
}

// How many instructions a scan looks at, as before calls were followed.
constexpr size_t kScanCount = 32;
// How many calls deep a passed-through x8 is followed.
constexpr int kFollowDepth = 2;

bool isBl(uint32_t i) { return (i & 0xFC000000) == 0x94000000; }
bool isB(uint32_t i) { return (i & 0xFC000000) == 0x14000000; }

int64_t branchOffset(uint32_t i) {
    int64_t imm = i & 0x03FFFFFF;
    if (imm & 0x02000000) imm -= 0x04000000;
    return imm * 4;
}

struct FileReader {
    const std::string* path;
    std::vector<ExecSegment> segs;
};

bool readFromFile(uint64_t vaddr, uint32_t* out, size_t count, void* user) {
    const auto* r = static_cast<const FileReader*>(user);
    return readCodeAt(*r->path, r->segs, vaddr, count * 4, reinterpret_cast<uint8_t*>(out));
}

#if defined(__aarch64__)
struct ProcessReader {
    uintptr_t bias;
    const std::vector<ExecSegment>* segs;
};

bool readFromProcess(uint64_t vaddr, uint32_t* out, size_t count, void* user) {
    const auto* r = static_cast<const ProcessReader*>(user);
    if (!inExecSegment(*r->segs, vaddr, count * 4)) return false;
    memcpy(out, reinterpret_cast<const void*>(r->bias + vaddr), count * 4);
    return true;
}
#endif

}  // namespace

const char* resultRegisterName(ResultRegister r) {
    switch (r) {
        case ResultRegister::kX8: return "x8";
        case ResultRegister::kNotX8: return "NOT x8";
        default: return "undetermined";
    }
}

ResultRegister aarch64ResultRegister(const uint32_t* code, size_t count, std::string* detail) {
    for (size_t k = 0; k < count; ++k) {
        const uint32_t i = code[k];
        if (readsX8(i)) {
            if (detail) *detail = "x8 read at +" + std::to_string(k * 4) + " (" + hex(i) + ")";
            return ResultRegister::kX8;
        }
        if (isBranchOrStop(i)) {
            if (detail) *detail = "branch at +" + std::to_string(k * 4) + " before x8 was read";
            return ResultRegister::kUndetermined;
        }
        if (mayWriteX8(i)) {
            if (detail) {
                *detail = "x8 overwritten at +" + std::to_string(k * 4) + " (" + hex(i) +
                          ") before any read";
            }
            return ResultRegister::kNotX8;
        }
    }
    if (detail) *detail = "x8 not read in the first " + std::to_string(count) + " instructions";
    return ResultRegister::kUndetermined;
}

namespace {

bool isConditionalBranch(uint32_t i) {
    return (i & 0xFF000010) == 0x54000000 ||  // B.cond
           (i & 0x7E000000) == 0x34000000 ||  // CBZ, CBNZ
           (i & 0x7E000000) == 0x36000000;    // TBZ, TBNZ
}

int64_t conditionalOffset(uint32_t i) {
    if ((i & 0x7E000000) == 0x36000000) {  // TBZ/TBNZ: imm14 at bits 18:5
        int64_t imm = (i >> 5) & 0x3FFF;
        if (imm & 0x2000) imm -= 0x4000;
        return imm * 4;
    }
    int64_t imm = (i >> 5) & 0x7FFFF;  // B.cond, CBZ/CBNZ: imm19 at bits 23:5
    if (imm & 0x40000) imm -= 0x80000;
    return imm * 4;
}

// How far a scan may go: instructions in all, conditional branches explored,
// calls followed. Bounded so that loops and branchy code end in "undetermined",
// never in a long walk.
struct Budget {
    int instructions = 256;
    int branches = 6;
};

std::string at(uint64_t v) {
    char b[24];
    snprintf(b, sizeof(b), "%llx", static_cast<unsigned long long>(v));
    return b;
}

// Walks the code from `vaddr` along every path the budget allows, with x8
// untouched so far. Reading x8 before writing it on ANY path proves the
// standard convention: a function given its result address in x0 holds
// garbage in x8 on every path, and the compiler never reads garbage. So one
// path that reads x8 is an answer; "NOT x8" needs every path explored to
// write it first.
ResultRegister walk(uint64_t vaddr, CodeReader read, void* user, int calls, Budget& budget,
                    std::string* detail) {
    uint64_t pc = vaddr;
    while (budget.instructions > 0) {
        // Read a block at a time; near the end of a segment, what there is.
        uint32_t code[kScanCount];
        size_t n = kScanCount;
        while (n > 0 && !read(pc, code, n, user)) n /= 2;
        if (n == 0) {
            if (detail) *detail = "no code to read at " + at(pc);
            return ResultRegister::kUndetermined;
        }
        for (size_t k = 0; k < n && budget.instructions > 0; ++k, pc += 4) {
            --budget.instructions;
            const uint32_t i = code[k];
            if (readsX8(i)) {
                if (detail) *detail = "x8 read at " + at(pc) + " (" + hex(i) + ")";
                return ResultRegister::kX8;
            }
            if (isB(i)) {
                // A jump, or a tail call: either way the code at the target
                // runs next with x8 as it is. Keep walking there.
                pc = pc + static_cast<uint64_t>(branchOffset(i)) - 4;
                k = n;  // re-read from the new pc (the loop adds 4 back)
                continue;
            }
            if (isBl(i)) {
                // x8 is caller-saved: it cannot outlive the call, so only the
                // callee can use what the caller put there.
                if (calls <= 0) {
                    if (detail) *detail = "call at " + at(pc) + ", not followed deeper";
                    return ResultRegister::kUndetermined;
                }
                const uint64_t target = pc + static_cast<uint64_t>(branchOffset(i));
                std::string inner;
                const ResultRegister r = walk(target, read, user, calls - 1, budget, &inner);
                if (detail) {
                    *detail = "x8 handed on by the call at " + at(pc) + ", whose answer is " +
                              resultRegisterName(r) + " (" + inner + ")";
                }
                return r == ResultRegister::kX8 ? ResultRegister::kX8
                                                : ResultRegister::kUndetermined;
            }
            if (isConditionalBranch(i)) {
                if (budget.branches <= 0) {
                    if (detail) *detail = "too many branches before x8 was read (at " + at(pc) + ")";
                    return ResultRegister::kUndetermined;
                }
                --budget.branches;
                const uint64_t taken = pc + static_cast<uint64_t>(conditionalOffset(i));
                std::string d_fall, d_taken;
                const ResultRegister fall = walk(pc + 4, read, user, calls, budget, &d_fall);
                if (fall == ResultRegister::kX8) {
                    if (detail) *detail = d_fall + ", past the branch at " + at(pc);
                    return fall;
                }
                const ResultRegister jump = walk(taken, read, user, calls, budget, &d_taken);
                if (jump == ResultRegister::kX8) {
                    if (detail) *detail = d_taken + ", where the branch at " + at(pc) + " goes";
                    return jump;
                }
                if (fall == ResultRegister::kNotX8 && jump == ResultRegister::kNotX8) {
                    if (detail) *detail = "both ways from " + at(pc) + ": " + d_fall + "; " + d_taken;
                    return ResultRegister::kNotX8;
                }
                if (detail) *detail = "neither way from " + at(pc) + " settles it: " + d_fall + "; " + d_taken;
                return ResultRegister::kUndetermined;
            }
            if (isBranchOrStop(i)) {
                if (detail) *detail = "branch or return at " + at(pc) + " before x8 was read";
                return ResultRegister::kUndetermined;
            }
            if (mayWriteX8(i)) {
                if (detail) *detail = "x8 overwritten at " + at(pc) + " (" + hex(i) + ") before any read";
                return ResultRegister::kNotX8;
            }
        }
    }
    if (detail) *detail = "x8 not read within the scan budget";
    return ResultRegister::kUndetermined;
}

}  // namespace

ResultRegister aarch64ResultRegisterAt(uint64_t vaddr, CodeReader read, void* user, int depth,
                                       std::string* detail) {
    Budget budget;
    return walk(vaddr, read, user, depth, budget, detail);
}

bool aarch64ResultRegisterInFile(const std::string& path, uint64_t vaddr, uint64_t size,
                                 ResultRegister* out, std::string* detail, int* machine) {
    (void)size;  // the scan stops at the first branch or return, inside the function
    if (machine) *machine = 0;
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    Elf64_Ehdr eh{};
    const bool header = pread(fd, &eh, sizeof(eh), 0) == static_cast<ssize_t>(sizeof(eh)) &&
                        eh.e_ident[EI_CLASS] == ELFCLASS64;
    close(fd);
    if (!header) return false;
    if (machine) *machine = eh.e_machine;
    if (eh.e_machine != EM_AARCH64) return false;

    FileReader reader{&path, readExecSegments(path)};
    if (!inExecSegment(reader.segs, vaddr, 4)) return false;
    *out = aarch64ResultRegisterAt(vaddr, readFromFile, &reader, kFollowDepth, detail);
    return true;
}

ResultRegister resultRegisterInProcess(uint64_t vaddr, uintptr_t bias, const void* segs,
                                       std::string* detail) {
#if defined(__aarch64__)
    ProcessReader reader{bias, static_cast<const std::vector<ExecSegment>*>(segs)};
    return aarch64ResultRegisterAt(vaddr, readFromProcess, &reader, kFollowDepth, detail);
#else
    (void)vaddr;
    (void)bias;
    (void)segs;
    if (detail) *detail = "not AArch64: the result address is the first argument either way";
    return ResultRegister::kX8;
#endif
}

}  // namespace glass
