// Does a function that returns a C++ object by value really take the address of
// that object in x8?
//
// AArch64 passes the address a by-value result is to be constructed at in x8,
// a register of its own, and the engine's bridge (sk_bridge.h) relies on that.
// But a compiler that can see every caller of a function is allowed to change
// how it is called, and on alioth's crDroid A17 SurfaceFlinger it did exactly
// that to SkRuntimeEffect::MakeForShader(SkString) — the header-inline wrapper:
// its out-of-line copy takes the result address in x0 and the SkString in x1,
// like an ordinary first argument. Called the standard way, it moved a string
// out of whatever x1 held — a pointer into the engine's read-only SkSL text —
// and SurfaceFlinger died on the first glass frame, then on every restart.
//
// x86-64 cannot show this: there the result address goes in rdi, which is also
// where a plain first argument goes, so the changed and the standard
// conventions are the same machine code. Every emulator run was x86-64.
//
// So before the engine calls any such function on arm64 it reads the
// function's first instructions and asks one question: is x8 read before
// anything could have overwritten it? A function that takes its result in x8
// must save or use x8 before its first call (x8 is not preserved across calls),
// and clang does that in the prologue — `mov x19, x8`, or a store through x8.
// Only a short list of exact encodings counts as a read. The walk follows the
// code a little way: an unconditional branch, a call made while x8 is still
// untouched (the callee's answer is the answer), and both arms of a
// conditional branch, within a small budget. A read on any path proves the
// standard convention; "not x8" needs a write first on every path walked;
// anything else gives up. Giving up means the engine does not call that
// function, and glass stays off rather than guessing: a false "no" costs the
// effect, a false "yes" costs a crash loop.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace glass {

enum class ResultRegister {
    kX8,            // x8 is read before it can have been written: standard
    kNotX8,         // x8 may be overwritten before any read: not standard
    kUndetermined,  // a branch, or nothing recognisable, before an answer
};

const char* resultRegisterName(ResultRegister r);

// Scans `count` A64 instructions from a function's entry. `detail` (optional)
// gets the instruction index and encoding the answer was decided on.
ResultRegister aarch64ResultRegister(const uint32_t* code, size_t count, std::string* detail);

// Copies `count` A64 instructions at link-time address `vaddr` into `out`.
// Returns false when any of them lies outside the code the reader knows, so a
// followed call can never send the scan into unmapped memory.
using CodeReader = bool (*)(uint64_t vaddr, uint32_t* out, size_t count, void* user);

// aarch64ResultRegister, walking the code instead of reading it in a line. A
// BL or B reached while x8 is still untouched hands the caller's x8 to the
// callee unchanged, so the callee's answer is the answer: x8 cannot survive
// the call otherwise, being caller-saved. Android 15's SkImage::makeShader is
// exactly that, a wrapper that takes a reference and calls the function that
// builds the shader, and the plain scan could only say "a branch first".
// Calls are followed up to `depth` deep. A conditional branch is walked both
// ways (the null checks in front of `mov x19, x8` in Android 17's Kawase
// generateTemporaryImage): a read on either arm decides kX8, and kNotX8 needs
// both arms to write x8 first. The walk stops after a few hundred
// instructions or a handful of branches, undetermined.
ResultRegister aarch64ResultRegisterAt(uint64_t vaddr, CodeReader read, void* user, int depth,
                                       std::string* detail);

// The same question about a function in an ELF file on disk, by link-time
// address — what the installer's preflight asks before anything is installed.
// Returns false if the file is not an AArch64 ELF (the question does not
// apply: *machine says what it was) or the address is not in an executable
// segment; true with *out filled otherwise.
bool aarch64ResultRegisterInFile(const std::string& path, uint64_t vaddr, uint64_t size,
                                 ResultRegister* out, std::string* detail, int* machine);

// The same question about a function in this process, as the engine asks it
// before binding: the function at link-time address `vaddr` of a binary loaded
// at `bias`, with calls followed only into the binary's own executable
// segments (`segs`, from readExecSegments in elf_syms.h, passed as an opaque
// pointer to keep this header free of it). On anything but AArch64 the answer
// is kX8 by definition: on x86-64 the result address is the first argument
// under either convention, so there is nothing to tell apart.
ResultRegister resultRegisterInProcess(uint64_t vaddr, uintptr_t bias, const void* segs,
                                       std::string* detail);

}  // namespace glass
