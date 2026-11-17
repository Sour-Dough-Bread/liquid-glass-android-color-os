#include "abi_check.h"

#include <elf.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <vector>

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
        // Loads, pair loads, exclusives, atomics: any of Rt, Rt2 or Rs may be
        // written.
        return rd(i) == kX8 || rt2(i) == kX8 || ((i >> 16) & 31) == kX8;
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

bool aarch64ResultRegisterInFile(const std::string& path, uint64_t vaddr, uint64_t size,
                                 ResultRegister* out, std::string* detail, int* machine) {
    if (machine) *machine = 0;
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    Elf64_Ehdr eh{};
    bool ok = pread(fd, &eh, sizeof(eh), 0) == static_cast<ssize_t>(sizeof(eh)) &&
              eh.e_ident[EI_CLASS] == ELFCLASS64;
    if (ok && machine) *machine = eh.e_machine;
    ok = ok && eh.e_machine == EM_AARCH64 && eh.e_phentsize == sizeof(Elf64_Phdr);

    uint64_t file_off = 0;
    bool mapped = false;
    for (uint16_t p = 0; ok && p < eh.e_phnum && !mapped; ++p) {
        Elf64_Phdr ph{};
        if (pread(fd, &ph, sizeof(ph), static_cast<off_t>(eh.e_phoff + p * sizeof(ph))) !=
            static_cast<ssize_t>(sizeof(ph))) {
            ok = false;
            break;
        }
        if (ph.p_type != PT_LOAD || (ph.p_flags & PF_X) == 0) continue;
        if (vaddr >= ph.p_vaddr && vaddr + 4 <= ph.p_vaddr + ph.p_filesz) {
            file_off = ph.p_offset + (vaddr - ph.p_vaddr);
            mapped = true;
        }
    }

    std::vector<uint32_t> code;
    if (ok && mapped) {
        const size_t n = static_cast<size_t>(std::min<uint64_t>(32, size / 4 ? size / 4 : 32));
        code.resize(n);
        const ssize_t want = static_cast<ssize_t>(n * 4);
        ok = pread(fd, code.data(), n * 4, static_cast<off_t>(file_off)) == want;
    }
    close(fd);
    if (!ok || !mapped) return false;
    *out = aarch64ResultRegister(code.data(), code.size(), detail);
    return true;
}

ResultRegister resultRegisterInMemory(const void* fn, uint64_t size, std::string* detail) {
#if defined(__aarch64__)
    const size_t n = static_cast<size_t>(std::min<uint64_t>(32, size / 4 ? size / 4 : 32));
    return aarch64ResultRegister(static_cast<const uint32_t*>(fn), n, detail);
#else
    (void)fn;
    (void)size;
    if (detail) *detail = "not AArch64: the result address is the first argument either way";
    return ResultRegister::kX8;
#endif
}

}  // namespace glass
