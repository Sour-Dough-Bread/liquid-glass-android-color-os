// One way to decide whether this SurfaceFlinger can be driven, used everywhere.
//
// Three programs ask that question: glasscheck at install, the engine inside
// SurfaceFlinger at start-up, and the host tests against binaries pulled from
// firmware. They used to carry a loop each, and the loops had drifted apart in
// small ways (one refused a build the other accepted when SkRuntimeEffect's
// one-argument MakeForShader had been inlined away while the Options form
// survived). This is the loop, once, and the rules that judge its result.
//
// The pipeline:
//
//   symbols ──► resolve() ──► Resolution ──► judge() ──► ok / fatal reasons
//   cache   ──► sf_cache.h ─┘
//
// resolve() matches every entry point of sf_symbols.h against a symbol table by
// exact mangled name, records drift (a prefix hit whose name is not one we
// know), and asks the caller's ABI probe about every function that returns an
// object by value. judge() then applies the group rules: exactly one hook
// shape, a way to release the uniform blob, and everything the Skia bridge's
// drawReady() needs. A Resolution can also be filled from the install-time
// cache (sf_cache.h) and judged the same way, which is how the engine skips
// reading 10 MB and inflating MiniDebugInfo on every start.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "abi_check.h"
#include "elf_syms.h"
#include "sf_symbols.h"

namespace glass {
namespace sf {

// Where an entry point's address came from.
enum class Source : uint8_t {
    kNone = 0,
    kSymbols,  // a symbol table, by exact mangled name (Symbol::origin says which)
    kCache,    // the install-time cache, for this very binary (sf_cache.h)
    kScan,     // located without a name at install time (no symbols on this build)
};
const char* sourceName(Source s);

// The shapes of the hook target the engine has a replacement for. Android 14
// and Android 16 QPR0 take (radius, alpha); Android 16 QPR2 and Android 17 add
// the zoom scale before alpha. A new shape is one row here, one entry in
// sf_symbols.cpp and one replacement function in the engine; everything else
// (exactly-one, the size test, the cache) follows from the table.
struct HookShape {
    Index entry;        // the kHookTarget entry in kEntryPoints
    const char* label;  // for logs and reports
};
inline constexpr HookShape kHookShapes[] = {
    {kDrawBlurRegion, "drawBlurRegion(radius, alpha)"},
    {kDrawBlurRegionScaled, "drawBlurRegion(radius, scale, alpha)"},
    {kDrawBlurRegionScaledKind, "drawBlurRegion(radius, scale, alpha, material)"},
};
inline constexpr size_t kHookShapeCount = sizeof(kHookShapes) / sizeof(kHookShapes[0]);

// The blur filters whose generateTemporaryImage (Android 17) the engine hooks
// for backdropBlur: AOSP's three and the ones ROMs ship in their place. Any one
// is enough, none is required, and each needs nothing else in the engine
// (generateHook<K> is instantiated per row).
inline constexpr Index kGenerateFilters[] = {
    kGenerateKawase,
    kGenerateGaussian,
    kGenerateKawaseDualV2,
    kGenerateGlass,
    kGenerateDarkmoon,
};
inline constexpr size_t kGenerateFilterCount =
    sizeof(kGenerateFilters) / sizeof(kGenerateFilters[0]);

struct Resolved {
    Symbol sym;                    // link-time; invalid when absent or rejected
    Source source = Source::kNone;
    bool drifted = false;          // unresolved, and a prefix hit names something unknown
    std::string drift_name;        // the first such name, for reports
    bool abi_rejected = false;     // returns by value and does not take x8
    std::string abi_detail;
};

// The libraries a SurfaceFlinger may take Skia from instead of linking it in.
// AOSP links Skia statically; ColorOS 17 (OnePlus 12, CPH2573_17.0.0.100)
// imports every out-of-line Skia function from libhwui.so, the framework's
// copy, and keeps only RenderEngine and Skia's header-inline code. A library
// is read only when the binary names it in DT_NEEDED, so it is the very file
// the process loaded, and only for Skia: the hook targets and the rest of
// RenderEngine always come from the binary itself.
inline constexpr const char* kSkiaLibraries[] = {"libhwui.so"};
// Where the system linker namespace finds them on a device (64-bit only, like
// the engine).
inline constexpr const char* kSystemLibraryDirs[] = {"/system/lib64", "/system_ext/lib64"};

// Adds every kSkiaLibraries entry the binary loads (DT_NEEDED) to `syms`, from
// the first directory in `dirs` that has it. Returns one line per library for
// reports: what was added, or why not. Call it after loadFromFile and before
// resolve().
std::vector<std::string> addSkiaLibraries(SymbolResolver* syms,
                                          const std::vector<std::string>& dirs);
std::vector<std::string> systemLibraryDirs();

struct Resolution {
    Resolved entry[kCount];
    // The files the entries' addresses belong to, by Symbol::module: [0] the
    // binary, then the libraries (SymbolResolver::modules, or the cache's).
    std::vector<std::string> modules;
    int shape = -1;                // index into kHookShapes once judged, else -1
    bool ok = false;               // judge(): the engine can install
    size_t symbol_count = 0;       // how many function symbols the tables held
    bool had_minidebuginfo = false;
    // The ROM builds the glass into RenderEngine (rom/, GlassFilter): the
    // module would draw over it. Found by resolve() from symbols only; a cache
    // is never written for such a build, because judge() refuses it.
    bool builtin_glass = false;
    std::vector<std::string> fatal;  // judge(): why not, one line each
    std::vector<std::string> notes;  // what is worth knowing about this build

    const Symbol& sym(Index i) const { return entry[i].sym; }
    bool has(Index i) const { return entry[i].sym.valid(); }

    // What the optional parts make possible, for logs and the WebUI.
    bool canRegenerateBlur() const;  // any generateTemporaryImage resolved
    bool canCopySubset() const;      // snapshotCopy=bounds
};

// Answers "does this by-value function take its result address in x8?" for a
// resolved symbol. The engine asks it of the code in memory, glasscheck of the
// file on disk. Return kX8 when the question does not apply (x86-64).
using AbiProbe = ResultRegister (*)(const Symbol& sym, std::string* detail, void* user);

// The probe for files on disk; `user` is a const std::vector<std::string>*
// holding each module's path (SymbolResolver::modules, Resolution::modules),
// and each symbol is asked of its own module's file. Asks only where the
// question applies (an AArch64 file, an address in its code), the way
// glasscheck always has, and answers kX8 otherwise.
ResultRegister fileAbiProbe(const Symbol& sym, std::string* detail, void* user);

// The module's file name for reports: "libhwui.so", or "" for the binary.
std::string moduleName(const Resolution& r, uint8_t module);

// Fills `out` from a symbol table, runs probeAbi() and judges it. `abi` may be
// null (no check: host tools that only report).
void resolve(const SymbolResolver& syms, AbiProbe abi, void* abi_user, Resolution* out);

// Asks `abi` about every resolved entry that returns an object by value and
// drops the ones that do not take x8. resolve() calls it; the engine calls it
// again on entries loaded from the cache, since the answer is about the code
// in memory, then judges.
void probeAbi(Resolution* r, AbiProbe abi, void* abi_user);

// Applies the group rules to entries already filled in, from resolve() or from
// the cache. Sets ok, shape and fatal; adds notes. Idempotent.
void judge(Resolution* r);

// A fingerprint of the entry-point table: every label, mangling and strategy
// in order. The cache records it, so an engine whose table changed never
// trusts a cache written by an older glasscheck.
uint64_t tableHash();

// One line per entry for logs and reports: label, state, address, source.
std::string describe(const Resolution& r, size_t index);

}  // namespace sf
}  // namespace glass
