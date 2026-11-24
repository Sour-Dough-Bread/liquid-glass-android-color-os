#include "sf_resolve.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <iterator>

#include <unistd.h>

namespace glass {
namespace sf {

const char* sourceName(Source s) {
    switch (s) {
        case Source::kSymbols: return "symbols";
        case Source::kCache: return "cache";
        case Source::kScan: return "scan";
        default: return "none";
    }
}

namespace {

// A hook needs room for its branch, and a function this small is a thunk or
// an inlined remnant rather than the body: never hook one.
constexpr uint64_t kMinHookableSize = 64;

// Spellings that share an entry's prefix and are known, but deliberately not
// resolved, so that finding them is not drift.
const char* const kKnownUnhooked[] = {
    // Android 17 QPR2's SkPath overload of drawBlurRegion, for path-shaped
    // regions (android.view.OutlineBlurRegion). Dormant on every Pixel CP4A
    // build: its flag, blur_outline_regions, is compiled false in
    // SurfaceFlinger and libgui. The RRect overload, which the engine hooks,
    // is the one every ordinary region goes through.
    "_ZN7android12renderengine4skia10BlurFilter14drawBlurRegionEP8SkCanvasRK6SkPathjffRK6SkRect"
    "5sk_spI7SkImageESD_",
};

bool isKnownMangling(const std::string& name) {
    for (size_t i = 0; i < kEntryPointCount; ++i) {
        for (const char* const* m = kEntryPoints[i].manglings; *m != nullptr; ++m) {
            if (name == *m) return true;
        }
    }
    for (const char* known : kKnownUnhooked) {
        if (name == known) return true;
    }
    return false;
}

// What the draw needs, mirroring SkBridge::drawReady() in sk_bridge.h. A row
// with an alternative is satisfied by either member. MakeForShader's Options
// form is preferred (the one-argument form is a header-inline wrapper whose
// calling convention a compiler may change; it did on alioth), so it comes
// first. findUniform and findChild are not here: without them the engine lays
// the uniforms out from its own declarations, checked against uniformSize().
struct Need {
    Index first;
    Index alternative;  // kCount: none
    const char* what;
};
const Need kNeeds[] = {
    {kMakeForShaderOpts, kMakeForShader, "SkRuntimeEffect::MakeForShader (either form)"},
    {kSkStringCtor, kCount, nullptr},
    {kSkStringDtor, kCount, nullptr},
    {kEffectUniformSize, kCount, nullptr},
    {kImageMakeShader, kCount, nullptr},
    {kDataMakeZeroInit, kCount, nullptr},
    {kSkSpDataDtor, kDataUnref, "a way to release the uniform blob"},
    {kEffectMakeShader, kCount, nullptr},
    {kMatrixPostConcat, kCount, nullptr},
    {kPaintReset, kCount, nullptr},
    {kPaintDtor, kCount, nullptr},
    {kCanvasDrawRect, kCount, nullptr},
};

std::string whyMissing(const Resolution& r, Index i) {
    const Resolved& e = r.entry[i];
    if (e.abi_rejected) return "does not take its result address in x8 (" + e.abi_detail + ")";
    if (e.drifted) return "signature changed: " + e.drift_name;
    return "not in this build's symbols (inlined away, or no symbols)";
}

const char* labelOf(Index i) { return kEntryPoints[i].label; }

// Only Skia may come from a library (kSkiaLibraries). The hook targets, the
// generate filters and every other piece of RenderEngine are hooked or
// identified in the binary's own code, and a library's copy of one would be
// code SurfaceFlinger never runs.
bool mayComeFromLibrary(const EntryPoint& ep) {
    if (ep.strategy == kHookTarget || ep.strategy == kBlurGenerate) return false;
    for (const char* const* m = ep.manglings; *m != nullptr; ++m) {
        if (strstr(*m, "7android12renderengine") != nullptr) return false;
    }
    return true;
}

void fnv(uint64_t* h, const void* data, size_t n) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) {
        *h ^= p[i];
        *h *= 0x100000001b3ULL;
    }
}

}  // namespace

ResultRegister fileAbiProbe(const Symbol& sym, std::string* detail, void* user) {
    const auto* paths = static_cast<const std::vector<std::string>*>(user);
    if (paths == nullptr || sym.module >= paths->size()) {
        // No file to ask: refuse rather than answer for code nobody read.
        if (detail != nullptr) *detail = "no file for this symbol's module";
        return ResultRegister::kUndetermined;
    }
    ResultRegister rr = ResultRegister::kX8;
    int machine = 0;
    if (!aarch64ResultRegisterInFile((*paths)[sym.module], sym.value, sym.size, &rr, detail,
                                     &machine)) {
        return ResultRegister::kX8;
    }
    return rr;
}

std::string moduleName(const Resolution& r, uint8_t module) {
    if (module == 0 || module >= r.modules.size()) return {};
    const std::string& p = r.modules[module];
    const size_t slash = p.rfind('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

std::vector<std::string> systemLibraryDirs() {
    return std::vector<std::string>(std::begin(kSystemLibraryDirs), std::end(kSystemLibraryDirs));
}

std::vector<std::string> addSkiaLibraries(SymbolResolver* syms,
                                          const std::vector<std::string>& dirs) {
    std::vector<std::string> report;
    for (const char* lib : kSkiaLibraries) {
        bool needed = false;
        for (const std::string& n : syms->needed()) needed = needed || n == lib;
        if (!needed) continue;
        std::string found;
        for (const std::string& d : dirs) {
            const std::string p = d + "/" + lib;
            if (access(p.c_str(), R_OK) == 0) {
                found = p;
                break;
            }
        }
        if (found.empty()) {
            report.push_back(std::string(lib) + ": loaded by the binary, but not found");
            continue;
        }
        const size_t n = syms->addLibrary(found);
        char buf[600];
        if (n > 0) {
            snprintf(buf, sizeof(buf), "Skia from %s: %zu exported functions", found.c_str(), n);
        } else {
            snprintf(buf, sizeof(buf), "%s: not used (%s)", found.c_str(), syms->error().c_str());
        }
        report.push_back(buf);
    }
    return report;
}

bool Resolution::canRegenerateBlur() const {
    for (Index i : kGenerateFilters) {
        if (has(i) && sym(i).size > kMinHookableSize) return true;
    }
    return false;
}

bool Resolution::canCopySubset() const {
    return has(kImageMakeSubset) && has(kCanvasBaseRecorder);
}

void resolve(const SymbolResolver& syms, AbiProbe abi, void* abi_user, Resolution* out) {
    *out = Resolution{};
    out->symbol_count = syms.count();
    out->had_minidebuginfo = syms.hadMiniDebugInfo();
    out->modules = syms.modules();

    for (size_t i = 0; i < kEntryPointCount; ++i) {
        const EntryPoint& ep = kEntryPoints[i];
        Resolved& r = out->entry[i];
        const bool own_only = !mayComeFromLibrary(ep);
        for (const char* const* m = ep.manglings; *m != nullptr; ++m) {
            const Symbol s = syms.find(*m);
            if (s.valid() && !(own_only && s.module != 0)) {
                r.sym = s;
                r.source = Source::kSymbols;
                break;
            }
        }
        if (!r.sym.valid()) {
            // A prefix hit means the method exists under a name we have not
            // vetted, which may be a different ABI. A hit on a spelling some
            // entry does know is not drift: the other drawBlurRegion shape,
            // MakeForShader's Options form next to its wrapper.
            if (ep.prefix != nullptr) {
                for (const std::string& name : syms.findByPrefix(ep.prefix)) {
                    if (isKnownMangling(name)) continue;
                    if (own_only && syms.find(name).module != 0) continue;
                    r.drifted = true;
                    r.drift_name = name;
                    break;
                }
            }
            continue;
        }
    }
    // rom/make_patch.sh's filter. Any of its methods will do: the class is
    // only ever there because the ROM carries the patch.
    out->builtin_glass =
        !syms.findByPrefix("_ZN7android12renderengine4skia11GlassFilter").empty();
    probeAbi(out, abi, abi_user);
    judge(out);
}

void probeAbi(Resolution* out, AbiProbe abi, void* abi_user) {
    if (abi == nullptr) return;
    for (size_t i = 0; i < kEntryPointCount; ++i) {
        Resolved& r = out->entry[i];
        if (!kEntryPoints[i].result_in_memory || !r.sym.valid()) continue;
        std::string detail;
        const ResultRegister rr = abi(r.sym, &detail, abi_user);
        if (rr == ResultRegister::kX8) continue;
        r.abi_rejected = true;
        r.abi_detail = std::string(resultRegisterName(rr)) + ": " + detail;
        r.sym = Symbol{};
        r.source = Source::kNone;
    }
}

void judge(Resolution* r) {
    r->ok = false;
    r->shape = -1;
    r->fatal.clear();
    r->notes.clear();

    // ---- the hook target: exactly one shape, big enough to hook
    int found = 0;
    for (size_t s = 0; s < kHookShapeCount; ++s) {
        if (r->has(kHookShapes[s].entry)) {
            ++found;
            r->shape = static_cast<int>(s);
        }
    }
    if (found > 1) {
        r->shape = -1;
        r->fatal.push_back("more than one drawBlurRegion shape present; cannot tell which runs");
    } else if (found == 0) {
        std::string drift;
        for (size_t s = 0; s < kHookShapeCount && drift.empty(); ++s) {
            if (r->entry[kHookShapes[s].entry].drifted) drift = r->entry[kHookShapes[s].entry].drift_name;
        }
        r->fatal.push_back(drift.empty()
                               ? "no BlurFilter::drawBlurRegion (inlined away, or no symbols)"
                               : "BlurFilter::drawBlurRegion has a shape this engine does not "
                                 "know: " + drift);
    } else {
        const Symbol& t = r->sym(kHookShapes[r->shape].entry);
        if (t.size <= kMinHookableSize) {
            char buf[96];
            snprintf(buf, sizeof(buf), "drawBlurRegion is only %" PRIu64 " bytes; nothing to hook",
                     t.size);
            r->fatal.push_back(buf);
            r->shape = -1;
        }
    }

    // A cache is read, not resolved, so the rule resolve() applies is checked
    // again here: anything hooked or identified lives in the binary itself.
    for (size_t i = 0; i < kEntryPointCount; ++i) {
        if (r->has(static_cast<Index>(i)) && r->entry[i].sym.module != 0 &&
            !mayComeFromLibrary(kEntryPoints[i])) {
            r->fatal.push_back(std::string(kEntryPoints[i].label) +
                               ": must be the binary's own, not a library's");
            r->shape = -1;
        }
        if (r->entry[i].sym.module != 0 && r->entry[i].sym.module >= r->modules.size() &&
            r->has(static_cast<Index>(i))) {
            r->fatal.push_back(std::string(kEntryPoints[i].label) + ": from an unknown module");
        }
    }

    if (r->builtin_glass) {
        r->fatal.push_back("this ROM has Stock Glass built into RenderEngine (GlassFilter): "
                           "the module is not needed, and would draw over it");
    }

    // ---- everything the Skia bridge needs to compile and draw
    for (const Need& n : kNeeds) {
        if (r->has(n.first)) continue;
        if (n.alternative != kCount && r->has(n.alternative)) continue;
        std::string line = n.what != nullptr ? n.what : labelOf(n.first);
        line += ": " + whyMissing(*r, n.first);
        if (n.alternative != kCount) {
            line += "; " + std::string(labelOf(n.alternative)) + ": " + whyMissing(*r, n.alternative);
        }
        r->fatal.push_back(line);
    }

    // ---- what is worth knowing even when it works
    if (r->shape >= 0 && kHookShapes[r->shape].entry == kDrawBlurRegionScaledKind) {
        r->notes.push_back("Evolution X's glass (EvoX 12.x, crDroid 13.x): StockGlass draws "
                           "where Android blurs, and leaves regions that ask for one of its "
                           "materials to the ROM");
    }
    for (size_t m = 1; m < r->modules.size(); ++m) {
        int from = 0;
        for (size_t i = 0; i < kEntryPointCount; ++i) {
            from += r->has(static_cast<Index>(i)) && r->entry[i].sym.module == m;
        }
        if (from > 0) {
            r->notes.push_back(std::to_string(from) + " Skia entry points from " +
                               moduleName(*r, static_cast<uint8_t>(m)) +
                               ", which this SurfaceFlinger takes Skia from");
        }
    }
    if (r->entry[kMakeForShader].abi_rejected && r->has(kMakeForShaderOpts)) {
        r->notes.push_back("MakeForShader(SkString) does not take x8; compiling through the "
                           "Options form");
    }
    for (size_t i = 0; i < kEntryPointCount; ++i) {
        const Resolved& e = r->entry[i];
        const Strategy st = kEntryPoints[i].strategy;
        if (e.drifted && st != kCore && st != kDataRelease && st != kHookTarget) {
            r->notes.push_back(std::string(kEntryPoints[i].label) +
                               ": signature changed (" + e.drift_name + "); not used");
        }
    }
    if (!r->has(kFindUniform) || !r->has(kFindChild)) {
        const char* which = !r->has(kFindUniform) && !r->has(kFindChild) ? "findUniform and findChild"
                            : !r->has(kFindUniform)                     ? "findUniform"
                                                                        : "findChild";
        r->notes.push_back(std::string("no ") + which +
                           ": the shader's own declarations lay out its uniforms, checked "
                           "against uniformSize()");
    }
    if (!r->has(kCanvasGetTotalMatrix)) r->notes.push_back("getTotalMatrix: engine's own definition");
    if (!r->has(kPaintSetShader)) r->notes.push_back("setShader: engine's own definition");
    if (!r->has(kMatrixInvertNonIdentity)) r->notes.push_back("invertNonIdentity: engine's own inverse");
    if (!r->canRegenerateBlur()) {
        r->notes.push_back("no generateTemporaryImage: a toned backdrop keeps the radius it asked for");
    }
    if (!r->canCopySubset()) r->notes.push_back("no SkImage_Base::makeSubset(SkRecorder*): snapshotCopy=bounds unavailable");

    r->ok = r->fatal.empty();
}

uint64_t tableHash() {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < kEntryPointCount; ++i) {
        const EntryPoint& ep = kEntryPoints[i];
        fnv(&h, ep.label, strlen(ep.label) + 1);
        for (const char* const* m = ep.manglings; *m != nullptr; ++m) fnv(&h, *m, strlen(*m) + 1);
        const uint8_t tail[2] = {static_cast<uint8_t>(ep.strategy),
                                 static_cast<uint8_t>(ep.result_in_memory)};
        fnv(&h, tail, sizeof(tail));
    }
    for (size_t s = 0; s < kHookShapeCount; ++s) {
        const uint32_t e = static_cast<uint32_t>(kHookShapes[s].entry);
        fnv(&h, &e, sizeof(e));
    }
    for (const char* known : kKnownUnhooked) fnv(&h, known, strlen(known) + 1);
    return h;
}

std::string describe(const Resolution& r, size_t index) {
    const EntryPoint& ep = kEntryPoints[index];
    const Resolved& e = r.entry[index];
    char buf[512];
    if (e.sym.valid()) {
        const std::string lib = moduleName(r, e.sym.module);
        snprintf(buf, sizeof(buf), "  %-8s %-44s 0x%-8" PRIx64 " %6" PRIu64 " B  %s%s%s%s%s",
                 strategyName(ep.strategy), ep.label, e.sym.value, e.sym.size,
                 sourceName(e.source), e.source == Source::kSymbols ? " " : "",
                 e.source == Source::kSymbols ? symbolOriginName(e.sym.origin) : "",
                 lib.empty() ? "" : " in ", lib.c_str());
    } else {
        snprintf(buf, sizeof(buf), "  %-8s %-44s --        %s", strategyName(ep.strategy),
                 ep.label,
                 e.abi_rejected ? ("not x8 (" + e.abi_detail + ")").c_str()
                 : e.drifted    ? ("drift: " + e.drift_name).c_str()
                                : "absent");
    }
    return buf;
}

}  // namespace sf
}  // namespace glass
