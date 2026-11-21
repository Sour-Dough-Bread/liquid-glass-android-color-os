// Host test for the x8 walk (abi_check.h) on hand-assembled code, so it runs
// without a ROM's surfaceflinger. Each case is a pattern a real build showed
// (the firmware survey's reports, captures/sf-corpus/reports/), reduced to the
// instructions that decide it. A false "x8" here would mean calling a function
// the wrong way and crashing SurfaceFlinger; a false "NOT x8" only costs the
// function.

#include "abi_check.h"

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <map>
#include <string>

namespace {

// A sparse address space of A64 words; a read of anything not placed fails,
// as a read outside the binary's code would.
struct Code {
    std::map<uint64_t, uint32_t> words;
    void at(uint64_t addr, std::initializer_list<uint32_t> insns) {
        for (uint32_t i : insns) {
            words[addr] = i;
            addr += 4;
        }
    }
};

bool readCode(uint64_t vaddr, uint32_t* out, size_t count, void* user) {
    const auto* c = static_cast<const Code*>(user);
    for (size_t k = 0; k < count; ++k) {
        const auto it = c->words.find(vaddr + 4 * k);
        if (it == c->words.end()) return false;
        out[k] = it->second;
    }
    return true;
}

// Encoders for the few forms the cases use.
uint32_t bl(uint64_t from, uint64_t to) {
    return 0x94000000u | (static_cast<uint32_t>((to - from) / 4) & 0x03FFFFFFu);
}
uint32_t b(uint64_t from, uint64_t to) {
    return 0x14000000u | (static_cast<uint32_t>((to - from) / 4) & 0x03FFFFFFu);
}
uint32_t cbzX(uint32_t rt, uint64_t from, uint64_t to) {
    return 0xB4000000u | ((static_cast<uint32_t>((to - from) / 4) & 0x7FFFFu) << 5) | rt;
}
uint32_t tbzW(uint32_t rt, uint32_t bit, uint64_t from, uint64_t to) {
    return 0x36000000u | (bit << 19) | ((static_cast<uint32_t>((to - from) / 4) & 0x3FFFu) << 5) |
           rt;
}

constexpr uint32_t kPaciasp = 0xD503233F;
constexpr uint32_t kBtiC = 0xD503245F;
constexpr uint32_t kStpFpLr = 0xA9BF7BFD;    // stp x29, x30, [sp, #-16]!
constexpr uint32_t kMovFpSp = 0x910003FD;    // mov x29, sp
constexpr uint32_t kRet = 0xD65F03C0;
constexpr uint32_t kMovX19X8 = 0xAA0803F3;   // mov x19, x8
constexpr uint32_t kMovX27X8 = 0xAA0803FB;   // mov x27, x8
constexpr uint32_t kMovX21X4 = 0xAA0403F5;   // mov x21, x4
constexpr uint32_t kStrXzrX8 = 0xF900011F;   // str xzr, [x8]
constexpr uint32_t kStrW9X8 = 0xB9002509;    // str w9, [x8, #0x24]
constexpr uint32_t kLdrX10 = 0xF946200A;     // ldr x10, [x0, #0xc40]
constexpr uint32_t kLdrX8Guard = 0xF9401668; // ldr x8, [x19, #0x28]
constexpr uint32_t kMovzX8 = 0xD2800008;     // mov x8, #0
constexpr uint32_t kAdrpX0 = 0x90000000;     // adrp x0, .
constexpr uint32_t kStxrW8 = 0xC8087C01;     // stxr w8, x1, [x0]
constexpr uint32_t kLdpX7X8 = 0xA94023E7;    // ldp x7, x8, [sp]

int failures = 0;

void expect(const char* name, const Code& code, uint64_t entry, int depth,
            glass::ResultRegister want) {
    std::string detail;
    const glass::ResultRegister got =
        glass::aarch64ResultRegisterAt(entry, readCode, const_cast<Code*>(&code), depth, &detail);
    const bool ok = got == want;
    if (!ok) ++failures;
    printf("  [%s] %-52s %-12s %s\n", ok ? " ok " : "FAIL", name, glass::resultRegisterName(got),
           detail.c_str());
}

}  // namespace

int main() {
    using glass::ResultRegister;
    Code c;

    // Android 15's SkCanvas::getTotalMatrix: the first load's imm12 has
    // 01000 in bits 14:10, which the old decoder read as an Rt2 of x8.
    c.at(0x1000, {kLdrX10, kStrW9X8, kRet});
    expect("getTotalMatrix: ldr imm12 is not a register", c, 0x1000, 2, ResultRegister::kX8);

    // Android 15's SkImage::makeShader: a wrapper that never touches x8 and
    // hands it to SkImageShader::Make.
    c.at(0x2000, {kPaciasp, kStpFpLr, kMovFpSp, bl(0x200C, 0x2400), kRet});
    c.at(0x2400, {kBtiC, kMovX19X8, kRet});
    expect("makeShader: x8 passed through a call", c, 0x2000, 2, ResultRegister::kX8);
    expect("the same call, not followed", c, 0x2000, 0, ResultRegister::kUndetermined);

    // A tail call: B to a function that reads x8.
    c.at(0x2800, {kBtiC, b(0x2804, 0x2400)});
    expect("tail call to a function reading x8", c, 0x2800, 2, ResultRegister::kX8);

    // Nothing OS 3.2's SkRuntimeEffect::makeShader: a tbz whose taken arm
    // stores the null result through x8; here the fall-through writes x8
    // first, so only the taken arm can answer.
    c.at(0x3000, {tbzW(9, 2, 0x3000, 0x300C), kMovzX8, kRet, kStrXzrX8, kRet});
    expect("tbz: the taken arm reads x8", c, 0x3000, 2, ResultRegister::kX8);
    c.at(0x3100, {tbzW(9, 2, 0x3100, 0x310C), kMovX27X8, kRet, kStrXzrX8, kRet});
    expect("tbz: the fall-through reads x8", c, 0x3100, 2, ResultRegister::kX8);

    // Android 17's KawaseBlurFilter::generateTemporaryImage: a null check
    // (LOG_ALWAYS_FATAL_IF) before `mov x19, x8`; the fatal arm calls out of
    // the code we can read.
    c.at(0x4000, {kPaciasp, cbzX(1, 0x4004, 0x4014), kMovX21X4, kMovX19X8, kRet, kAdrpX0,
                  bl(0x4018, 0x90000)});
    expect("cbz null check before mov x19, x8", c, 0x4000, 2, ResultRegister::kX8);

    // Nothing OS 3.2's SkRuntimeEffect::MakeForShader(SkString): the stack
    // guard loaded into x8 first. Genuinely not the standard convention.
    c.at(0x5000, {kPaciasp, kStpFpLr, kMovFpSp, kLdrX8Guard, kMovX19X8, kRet});
    expect("x8 overwritten by a load first", c, 0x5000, 2, ResultRegister::kNotX8);

    // Both arms write x8 before reading it.
    c.at(0x6000, {cbzX(1, 0x6000, 0x600C), kMovzX8, kRet, kLdrX8Guard, kRet});
    expect("both arms write x8 first", c, 0x6000, 2, ResultRegister::kNotX8);

    // One arm writes x8, the other returns without a word on it: not enough
    // for "NOT x8".
    c.at(0x7000, {cbzX(1, 0x7000, 0x700C), kMovzX8, kRet, kRet});
    expect("one arm writes, the other says nothing", c, 0x7000, 2, ResultRegister::kUndetermined);

    // A loop that never reads or writes x8 ends in the budget, not a hang.
    c.at(0x8000, {b(0x8000, 0x8000)});
    expect("a tight loop runs out of budget", c, 0x8000, 2, ResultRegister::kUndetermined);

    // Store-exclusive status and pair loads write their second register.
    c.at(0x9000, {kStxrW8, kMovX19X8, kRet});
    expect("stxr's status register is a write", c, 0x9000, 2, ResultRegister::kNotX8);
    c.at(0x9100, {kLdpX7X8, kMovX19X8, kRet});
    expect("ldp's second register is a write", c, 0x9100, 2, ResultRegister::kNotX8);

    // A call out of the readable code, with x8 untouched: no answer.
    c.at(0xA000, {kBtiC, bl(0xA004, 0x90000), kRet});
    expect("a call into unreadable code", c, 0xA000, 2, ResultRegister::kUndetermined);

    printf("%s\n", failures == 0 ? "abi_walk_test: all passed" : "abi_walk_test: FAILURES");
    return failures == 0 ? 0 : 1;
}
