// libglassengine.so — loaded into /system/bin/surfaceflinger.
//
// Hooks BlurFilter::drawBlurRegion and hands every region to the drawing code
// (glass_draw.cpp), which draws it as Liquid Glass in glass mode. In stock
// mode — the default — the drawing code hands it straight back and the hook
// calls the original with the arguments it was given. Switching between the
// two is a property write and takes effect on the next frame.
//
// On Android 17 it also hooks the blur filter's generateTemporaryImage, so
// that in glass mode the drawing code can make the blurs itself ("blur made
// to order", glass_draw.cpp). In stock mode that hook too passes straight
// through.
//
// What is in this file is the module's alone: finding SurfaceFlinger's
// symbols, hooking them, the boot guard. A ROM that builds the glass in
// (rom/) drives the same drawing code with none of it.
//
// Everything here runs inside SurfaceFlinger. A crash is a soft reboot, so the
// rule throughout is: any failure logs and falls through to the original
// function, leaving stock behaviour intact. There is no path that aborts, and
// no path that draws with anything unverified.

#include <android/log.h>
#include <dlfcn.h>
#include <frida-gum.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <unistd.h>
#if defined(__aarch64__)
#include <asm/hwcap.h>
#ifndef HWCAP2_BTI
#define HWCAP2_BTI (1 << 17)
#endif
#endif

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <utility>

#include "abi_check.h"
#include "blur_scale.h"
#include "crash_guard.h"
#include "elf_syms.h"
#include "exec_probe.h"
#include "glass_draw.h"
#include "glass_params.h"
#include "sf_cache.h"
#include "sf_resolve.h"
#include "sf_symbols.h"
#include "sk_bridge.h"
#include "xz_inflate.h"

#define LOG_TAG "StockGlass"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

// ---------------------------------------------------------------- config

std::string propGet(const char* name, const char* fallback) {
    char buf[PROP_VALUE_MAX] = {0};
    const int n = __system_property_get(name, buf);
    return n > 0 ? std::string(buf, n) : std::string(fallback);
}

// Where this library was loaded from: /system/lib64 under the module's overlay,
// a tmpfs directory in live mode, wherever a developer put it. The symbol cache
// sits next to it, so it is found the same way in all three.
std::string enginePath() {
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(&enginePath), &info) != 0 && info.dli_fname != nullptr) {
        return info.dli_fname;
    }
    return "/system/lib64/libglassengine.so";
}

int propInt(const char* name, int fallback) {
    const std::string v = propGet(name, "");
    if (v.empty()) return fallback;
    return static_cast<int>(strtol(v.c_str(), nullptr, 10));
}

bool exists(const std::string& p) {
    struct stat st {};
    return stat(p.c_str(), &st) == 0;
}

// /data/adb is KernelSU/Magisk territory and is the right home in production,
// but it does not exist on a plain emulator. Fall back so the same binary is
// testable without a module installed.
std::string stateDir() {
    if (exists("/data/adb")) return "/data/adb/glass";
    return "/data/local/tmp/glass";
}

// ------------------------------------------------------- bootloop guard
//
// The engine arms itself on load and disarms once the system finishes booting.
// If SurfaceFlinger dies before boot completes, the marker survives, and the
// next boot sees it and disables the engine permanently. That turns "a bad
// build bricks the phone" into "a bad build costs one boot".
//
// Deliberately does NOT trip on a crash *after* boot completed: by then the
// device is usable and the user can disable the module themselves, whereas
// disabling on any crash ever would make an unrelated SF restart silently turn
// the effect off.

struct Guard {
    std::string pending;
    std::string disabled;
};

// True when the module's post-fs-data.sh owns the bootloop guard. Then the
// engine touches no state files at all: in enforcing mode surfaceflinger may
// not even look inside /data/adb, so its own guard only produced denials.
bool g_module_guard = false;

Guard guardPaths() {
    const std::string d = stateDir();
    return {d + "/boot_pending", d + "/disabled"};
}

bool guardShouldRun() {
    // When installed as a module, the guard lives in post-fs-data.sh instead,
    // and this one stands down rather than keeping a second marker with the
    // same meaning. The module's version is strictly stronger: it can refuse to
    // build the overlay at all, which also covers the failure this one provably
    // cannot — an unloadable library makes the linker reject surfaceflinger
    // before any of this code runs, so nothing here is in a position to disarm
    // anything. This guard remains for the development install, which patches
    // /system directly and has no scripts.
    //
    // The module says so with a property as well as a file. On a real device in
    // enforcing mode surfaceflinger may not even stat /data/adb, so the file is
    // invisible from here and every boot logged a string of SELinux denials
    // looking for it, then failed to arm a guard it did not need. Ask the
    // property first and touch no files at all.
    if (propGet("sys.glass.module", "") == "1" || exists(stateDir() + "/module")) {
        LOGI("bootloop guard owned by the module");
        g_module_guard = true;
        return true;
    }

    // Only now: guardPaths() stats /data/adb, which is what the property
    // above exists to avoid (the module and live mode both set it).
    const Guard g = guardPaths();
    if (exists(g.disabled)) {
        LOGW("disabled by %s — remove it to re-enable", g.disabled.c_str());
        return false;
    }
    if (exists(g.pending)) {
        LOGE("previous boot did not complete with the engine loaded; disabling. "
             "delete %s and %s to retry", g.pending.c_str(), g.disabled.c_str());
        FILE* f = fopen(g.disabled.c_str(), "w");
        if (f) { fputs("auto-disabled after an incomplete boot\n", f); fclose(f); }
        return false;
    }

    mkdir(stateDir().c_str(), 0700);
    FILE* f = fopen(g.pending.c_str(), "w");
    if (f == nullptr) {
        // Not fatal, but say so loudly: without this file there is no safety net.
        LOGW("could not arm the bootloop guard at %s", g.pending.c_str());
        return true;
    }
    fclose(f);
    return true;
}

void guardDisarmWhenBooted() {
    std::thread([] {
        const Guard g = guardPaths();
        // ~2 minutes is well past a normal boot; give up quietly rather than
        // leaving a thread spinning for the life of the process.
        for (int i = 0; i < 240; ++i) {
            if (propInt("sys.boot_completed", 0) == 1) {
                unlink(g.pending.c_str());
                LOGI("boot completed; guard disarmed");
                return;
            }
            struct timespec ts { 0, 500L * 1000 * 1000 };
            nanosleep(&ts, nullptr);
        }
        LOGW("boot_completed never observed; leaving the guard armed");
    }).detach();
}

// ------------------------------------------------------------- the hook

// The ABI the target actually presents. sk_sp is non-trivial for the purposes
// of calls, so each image argument arrives as a pointer to the caller's own
// temporary rather than as a register-passed pointer to the image.
using DrawBlurRegionFn = void (*)(void* self, SkCanvas* canvas, const SkRRect* effect_region,
                                  uint32_t blur_radius, float blur_alpha, const SkRect* blur_rect,
                                  void* blurred_image_sp, void* input_sp);

// Android 16 QPR2 carries an extra leading float. Floats live in their own
// register file on both AArch64 and x86-64 SysV, so the pointers do not move —
// but blurAlpha is the SECOND float here, and calling the original with one
// float where it expects two would leave the callee reading a stale register.
using DrawBlurRegionScaledFn = void (*)(void* self, SkCanvas* canvas,
                                        const SkRRect* effect_region, uint32_t blur_radius,
                                        float blur_scale, float blur_alpha,
                                        const SkRect* blur_rect, void* blurred_image_sp,
                                        void* input_sp);

// Evolution X 12.x and crDroid 13.x: the QPR2 shape and one more int, the
// region's material for EvoX's own glass (sf_symbols.h, kHookTarget).
using DrawBlurRegionScaledKindFn = void (*)(void* self, SkCanvas* canvas,
                                            const SkRRect* effect_region, uint32_t blur_radius,
                                            float blur_scale, float blur_alpha,
                                            const SkRect* blur_rect, void* blurred_image_sp,
                                            void* input_sp, int kind);

GumInterceptor* g_interceptor = nullptr;
DrawBlurRegionFn g_original = nullptr;
DrawBlurRegionScaledFn g_original_scaled = nullptr;
DrawBlurRegionScaledKindFn g_original_scaled_kind = nullptr;

// Every resolved entry point, indexed by glass::sf::Index.
glass::Symbol g_syms[glass::sf::kCount];
uintptr_t g_bias = 0;

// ------------------------------------------- generateTemporaryImage hooks
//
// The drawing code answers each generate call in the filter's place
// (glass_draw.cpp, "blur made to order"). These are its way in from the
// hooks, one per row of glass::sf::kGenerateFilters; install() hands the
// drawing code each filter's original under the same index (setGenerator).
constexpr size_t kFilterCount = glass::sf::kGenerateFilterCount;
static_assert(kFilterCount <= glass::draw::kGenerateSlots,
              "more generate filters than the drawing code has slots: raise "
              "glass::draw::kGenerateSlots");

template <size_t K>
glass::skia::ReturnedSkSp generateHook(const void* self, void* context, const void* display,
                                       uint32_t radius, void* input_sp,
                                       const SkRect* blur_rect) {
    // A prvalue of the same type, so C++17 constructs it straight into the
    // caller's return slot: nothing is copied or destroyed here.
    return glass::draw::generate(K, self, context, display, radius, input_sp, blur_rect);
}

// generateHook<K> for every row of glass::sf::kGenerateFilters.
template <size_t... K>
std::array<void*, sizeof...(K)> generateHooks(std::index_sequence<K...>) {
    return {{reinterpret_cast<void*>(&generateHook<K>)...}};
}

// ------------------------------------------------------------ draw hooks
//
// One replacement per shape of drawBlurRegion (sf_resolve.h, kHookShapes).
// Each hands the region to the drawing code with a way back to the original,
// which it calls with the blur and radius the drawing code passes and every
// other argument as it came.
struct HookCall {
    void* self;
    SkCanvas* canvas;
    const SkRRect* region;
    float scale;
    float alpha;
    const SkRect* blur_rect;
    void* input_sp;
    int kind;
};

void callOriginal(void* cookie, void* blurred_sp, uint32_t radius) {
    const HookCall& c = *static_cast<const HookCall*>(cookie);
    g_original(c.self, c.canvas, c.region, radius, c.alpha, c.blur_rect, blurred_sp, c.input_sp);
}

void callOriginalScaled(void* cookie, void* blurred_sp, uint32_t radius) {
    const HookCall& c = *static_cast<const HookCall*>(cookie);
    g_original_scaled(c.self, c.canvas, c.region, radius, c.scale, c.alpha, c.blur_rect,
                      blurred_sp, c.input_sp);
}

void callOriginalScaledKind(void* cookie, void* blurred_sp, uint32_t radius) {
    const HookCall& c = *static_cast<const HookCall*>(cookie);
    g_original_scaled_kind(c.self, c.canvas, c.region, radius, c.scale, c.alpha, c.blur_rect,
                           blurred_sp, c.input_sp, c.kind);
}

void drawGlass(void* self, SkCanvas* canvas, const SkRRect* region, uint32_t radius, float alpha,
               const SkRect* blur_rect, void* blurred_sp, void* input_sp) {
    HookCall c{self, canvas, region, 1.0f, alpha, blur_rect, input_sp, glass::draw::kNoMaterial};
    glass::draw::blurRegion(canvas, region, radius, 1.0f, alpha, blur_rect, blurred_sp, input_sp,
                            glass::draw::kNoMaterial,
                            g_original != nullptr ? &callOriginal : nullptr, &c);
}

void drawGlassScaled(void* self, SkCanvas* canvas, const SkRRect* region, uint32_t radius,
                     float scale, float alpha, const SkRect* blur_rect, void* blurred_sp,
                     void* input_sp) {
    // The extra float is BlurFilter's zoomScale (AOSP names it in
    // android16-qpr2-release and android17-release): stock scales both children
    // about the blur rect by it, and so do blurredTransform/sharpTransform. It
    // used to be handed back to stock whenever it was not 1; it is honoured now,
    // and the draw log reports it whenever it moves off or back to 1.
    HookCall c{self, canvas, region, scale, alpha, blur_rect, input_sp, glass::draw::kNoMaterial};
    glass::draw::blurRegion(canvas, region, radius, scale, alpha, blur_rect, blurred_sp, input_sp,
                            glass::draw::kNoMaterial,
                            g_original_scaled != nullptr ? &callOriginalScaled : nullptr, &c);
}

// Evolution X 12.x and crDroid 13.x. `kind` is the region's material
// (glass_draw.cpp, "materials", says what each means and which the glass
// draws). Every call to the original carries it as it came.
void drawGlassScaledKind(void* self, SkCanvas* canvas, const SkRRect* region, uint32_t radius,
                         float scale, float alpha, const SkRect* blur_rect, void* blurred_sp,
                         void* input_sp, int kind) {
    HookCall c{self, canvas, region, scale, alpha, blur_rect, input_sp, kind};
    glass::draw::blurRegion(
        canvas, region, radius, scale, alpha, blur_rect, blurred_sp, input_sp, kind,
        g_original_scaled_kind != nullptr ? &callOriginalScaledKind : nullptr, &c);
}

// -------------------------------------------------------------- install

// Branch protection. Some ROMs build SurfaceFlinger with BTI (Pixel from
// Android 17 QPR1, Lineage 24 for garnet, Nothing's Qualcomm builds), and on
// an ARMv9 core its code pages are then guarded: an indirect branch may land
// only on a `bti` or `paciasp`. frida's way back from the trampoline into the
// hooked function is such a branch, to an ordinary instruction. The page
// frida patches loses its guard when frida re-protects it (mprotect keeps
// PROT_BTI only when asked to), but the way back lands on the next page when
// the patch ends at a page boundary, and that page would still be guarded:
// SIGILL in SurfaceFlinger, about one function start in 256. So the pages
// from the target to just past the longest patch are made plain read-and-
// execute first, before anything is patched, so that no frame can ever run
// through a half-made hook. Nothing else changes for them, and where BTI is
// not enforced nothing changes at all. Not yet run on a phone that enforces
// it: alioth and the F1 are ARMv8.x.
//
// The engine's own calls into SurfaceFlinger are indirect too (BLR through
// the resolved pointers), so each must land on a `bti c`, `bti jc` or
// `paciasp`/`pacibsp`. On all 22 BTI-built surfaceflingers in the corpus every
// function the engine calls starts with one: the compiler keeps a landing pad
// on anything whose address can escape. landsOnPad() checks that again on the
// running build. libglassengine.so is not BTI-built, so its own pages are never
// guarded, and frida's trampolines live in pages of its own.
bool cpuHasBti() {
#if defined(__aarch64__)
    return (getauxval(AT_HWCAP2) & HWCAP2_BTI) != 0;
#else
    return false;
#endif
}

void dropBranchGuard(const void* target, const char* label) {
#if defined(__aarch64__)
    // How far past a hook target frida's patch can reach: its longest
    // redirect is four instructions (LDR, BR and an 8-byte literal), and the
    // way back into the function lands right after what it overwrote.
    constexpr uintptr_t kPatchReach = 32;
    const uintptr_t page = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
    const uintptr_t at = reinterpret_cast<uintptr_t>(target);
    const uintptr_t start = at & ~(page - 1);
    const uintptr_t end = (at + kPatchReach + page - 1) & ~(page - 1);
    if (mprotect(reinterpret_cast<void*>(start), end - start, PROT_READ | PROT_EXEC) != 0) {
        LOGW("could not clear branch-target guarding around %s (%s)%s", label, strerror(errno),
             cpuHasBti() ? "; this CPU implements BTI" : "");
    }
#else
    (void)target;
    (void)label;
#endif
}

// Whether a BLR may land on the first instruction of fn in a guarded page.
bool landsOnPad(const void* fn) {
    uint32_t w;
    memcpy(&w, fn, sizeof(w));
    return w == 0xd503245f     // bti c
           || w == 0xd50324df  // bti jc
           || w == 0xd503233f  // paciasp
           || w == 0xd503237f;  // pacibsp
}

bool install() {
    const std::string exe = glass::selfExePath();
    if (exe.find("surfaceflinger") == std::string::npos) {
        // Loaded into something unexpected. Do nothing rather than guess.
        LOGW("not surfaceflinger (%s); standing down", exe.c_str());
        return false;
    }

    // The glass samples the compositor's sharp snapshot, `input`. On Ganesh —
    // GL or Vulkan, what every shipping RenderEngine uses — that is always a
    // copy-on-write snapshot, even where RenderEngine asks for a "temporary"
    // image: SkSurface_Base::onMakeTemporaryImage is a tail call to
    // makeImageSnapshot on every build checked (docs/VERIFIED.md). Graphite
    // overrides it with a genuinely temporary image, which would alias the
    // surface being drawn into from the second blur layer of a frame onwards.
    // Graphite is experimental in RenderEngine; stand down rather than sample
    // undefined memory.
    if (propGet("debug.renderengine.graphite", "") == "true" ||
        propGet("debug.renderengine.backend", "").find("graphite") != std::string::npos) {
        LOGW("RenderEngine is on Graphite; the glass is only verified on Ganesh. standing down");
        return false;
    }

    const auto t0 = std::chrono::steady_clock::now();

    bool found = false;
    const uintptr_t bias = glass::moduleLoadBias(exe.c_str(), &found);
    if (!found) {
        LOGE("no load mapping for %s", exe.c_str());
        return false;
    }
    g_bias = bias;

    // Branch protection: a BTI-built SurfaceFlinger on a CPU that implements
    // BTI runs with its code guarded (bionic maps it so), which is when
    // dropBranchGuard and landsOnPad matter. The file is the patched copy
    // that is running; patchelf moves the property note but keeps it.
    uint32_t features = 0;
    const bool bti_built = glass::readAarch64Features(exe, &features) &&
                           (features & glass::kAarch64FeatureBti) != 0;
    const bool bti_guarded = bti_built && cpuHasBti();
    __system_property_set("sys.glass.bti", bti_guarded ? "guarded"
                                           : bti_built ? "built"
                                           : cpuHasBti() ? "cpu"
                                                         : "none");
    if (bti_guarded) {
        LOGI("surfaceflinger is BTI-built and this CPU enforces BTI: its code is guarded, and "
             "the pages at each hook lose their guard");
    } else if (bti_built) {
        LOGI("surfaceflinger is BTI-built; this CPU does not implement BTI, so nothing is guarded");
    }

    // A function that returns an object by value is only called if it takes the
    // result address the standard way (x8 on AArch64). The compiler is free to
    // change that for a function it can see every caller of, and on the arm64
    // A17 builds measured it did for MakeForShader(SkString): called the
    // standard way it crashed SurfaceFlinger on the first glass frame. Asked of
    // the code in memory; one that fails is treated as absent (sf_resolve.h).
    // Calls the scan follows stay inside the code segments of the function's
    // own file: the binary, or the library it takes Skia from (sf_resolve.h,
    // kSkiaLibraries), each at its own load bias.
    struct ProbeContext {
        std::vector<uintptr_t> biases;                      // by Symbol::module
        std::vector<std::vector<glass::ExecSegment>> segs;  // by Symbol::module
        std::string missing;                                // a module not mapped here
    };
    auto probe = [](const glass::Symbol& s, std::string* detail,
                    void* user) -> glass::ResultRegister {
        const auto* c = static_cast<const ProbeContext*>(user);
        if (s.module >= c->biases.size() || c->segs[s.module].empty()) {
            if (detail != nullptr) *detail = "its file is not mapped in this process";
            return glass::ResultRegister::kUndetermined;
        }
        return glass::resultRegisterInProcess(s.value, c->biases[s.module], &c->segs[s.module],
                                              detail);
    };
    ProbeContext probe_ctx;
    const std::vector<glass::ExecSegment> exe_segs = glass::readExecSegments(exe);
    // Module 0 is the binary; the rest are found in /proc/self/maps by path.
    auto mapModules = [&](const std::vector<std::string>& paths) {
        probe_ctx.biases.assign(1, bias);
        probe_ctx.segs.assign(1, exe_segs);
        probe_ctx.missing.clear();
        for (size_t m = 1; m < paths.size(); ++m) {
            bool mapped = false;
            const uintptr_t b = glass::moduleLoadBias(paths[m].c_str(), &mapped);
            probe_ctx.biases.push_back(mapped ? b : 0);
            probe_ctx.segs.push_back(mapped ? glass::readExecSegments(paths[m])
                                            : std::vector<glass::ExecSegment>{});
            if (!mapped && probe_ctx.missing.empty()) probe_ctx.missing = paths[m];
        }
    };
    mapModules({exe});
    void* const probe_bias = &probe_ctx;

    // The install-time cache first (sf_cache.h): glasscheck resolved this very
    // binary and recorded the addresses with their first code bytes. Checking
    // them reads a few kilobytes; resolving reads the whole binary and inflates
    // its MiniDebugInfo. Anything wrong with the cache means resolving as
    // before, so it can cost time and never correctness.
    glass::sf::Resolution res;
    std::string why;
    const std::string cache = glass::sf::cachePathFor(enginePath());
    bool from_cache = glass::sf::readCacheForProcess(cache, exe, bias, &res, &why);
    if (from_cache) {
        mapModules(res.modules);
        glass::sf::probeAbi(&res, probe, probe_bias);
        glass::sf::judge(&res);
        if (!res.ok) {
            LOGW("the symbol cache does not add up (%s); resolving from symbols",
                 res.fatal.empty() ? "?" : res.fatal[0].c_str());
            from_cache = false;
        }
    } else {
        LOGI("symbol cache not used: %s", why.c_str());
    }
    if (!from_cache) {
        glass::SymbolResolver resolver;
        if (!resolver.loadFromFile(exe, glass::inflateXzAdapter, nullptr)) {
            LOGE("symbol load failed: %s", resolver.error().c_str());
            return false;
        }
        for (const std::string& line :
             glass::sf::addSkiaLibraries(&resolver, glass::sf::systemLibraryDirs())) {
            LOGI("%s", line.c_str());
        }
        mapModules(resolver.modules());
        glass::sf::resolve(resolver, probe, probe_bias, &res);
    }
    // Every address is relative to its own file's load bias. A library the
    // resolution relies on but that this process has not mapped would leave
    // nothing to call; refuse rather than guess where it is.
    mapModules(res.modules);
    if (res.ok && !probe_ctx.missing.empty()) {
        LOGE("%s is not mapped in this process; refusing to install", probe_ctx.missing.c_str());
        return false;
    }
    for (size_t m = 1; m < res.modules.size(); ++m) {
        LOGI("module %zu: %s at 0x%llx", m, res.modules[m].c_str(),
             static_cast<unsigned long long>(probe_ctx.biases[m]));
    }
    for (size_t i = 0; i < glass::sf::kEntryPointCount; ++i) {
        if (res.entry[i].abi_rejected) {
            LOGW("%s does not take its result address in x8 (%s); not calling it",
                 glass::sf::kEntryPoints[i].label, res.entry[i].abi_detail.c_str());
        }
    }
    for (const std::string& n : res.notes) LOGI("note: %s", n.c_str());
    if (!res.ok) {
        for (const std::string& f : res.fatal) LOGE("cannot drive this build: %s", f.c_str());
        LOGE("refusing to install; Android's blur is untouched");
        return false;
    }
    for (size_t i = 0; i < glass::sf::kCount; ++i) g_syms[i] = res.entry[i].sym;

    // A RenderEngine that keeps a layer's blur from one frame to the next
    // (sf_symbols.cpp, kBlurCacheValid) would keep a snapshot handed back in
    // a blur's place as that layer's blur, and draw it on the frames after,
    // where nothing tells it from a blur any more. Nothing is put off there.
    if (g_syms[glass::sf::kBlurCacheValid].valid()) {
        glass::draw::refuseLazyBlur();
        LOGI("this RenderEngine can keep blurs between frames; Android's blur is never put "
             "off here");
    }

    // Exactly one shape of drawBlurRegion (judge() made sure), and which one
    // decides the replacement installed.
    static_assert(glass::sf::kHookShapeCount == 3,
                  "a hook shape without a replacement function: add one below");
    void* const replacements[glass::sf::kHookShapeCount] = {
        reinterpret_cast<void*>(&drawGlass), reinterpret_cast<void*>(&drawGlassScaled),
        reinterpret_cast<void*>(&drawGlassScaledKind)};
    void** const original_slots[glass::sf::kHookShapeCount] = {
        reinterpret_cast<void**>(&g_original), reinterpret_cast<void**>(&g_original_scaled),
        reinterpret_cast<void**>(&g_original_scaled_kind)};
    const glass::sf::HookShape& shape = glass::sf::kHookShapes[res.shape];
    const glass::Symbol draw = res.sym(shape.entry);
    void* const replacement = replacements[res.shape];
    LOGI("hook target: %s, resolved from %s", shape.label,
         from_cache ? "the install-time cache" : "symbols");

    // Report which path through the bridge this target takes, because that is
    // the first question worth asking when a new build misbehaves.
    LOGI("%s; blob release via %s; getTotalMatrix %s; setShader %s",
         from_cache ? "symbols: cached" : (std::to_string(res.symbol_count) + " symbols").c_str(),
         g_syms[glass::sf::kSkSpDataDtor].valid() ? "sk_sp dtor"
             : (g_syms[glass::sf::kDataUnref].valid() ? "SkNVRefCnt::unref" : "NOTHING"),
         g_syms[glass::sf::kCanvasGetTotalMatrix].valid() ? "resolved" : "OUR DEFINITION",
         g_syms[glass::sf::kPaintSetShader].valid() ? "resolved" : "OUR DEFINITION");

    // Everything bound here is called through a pointer. On a guarded build, a
    // function that does not start on a landing pad (none in the corpus) loses
    // the guard on its first page instead of faulting at the first draw.
    int unpadded = 0;
    auto addr = [&](size_t i) -> void* {
        if (!g_syms[i].valid() || g_syms[i].module >= probe_ctx.biases.size()) return nullptr;
        void* fn = reinterpret_cast<void*>(probe_ctx.biases[g_syms[i].module] + g_syms[i].value);
        if (bti_guarded && !landsOnPad(fn)) {
            LOGW("%s starts without a landing pad; its page loses the guard",
                 glass::sf::kEntryPoints[i].label);
            dropBranchGuard(fn, glass::sf::kEntryPoints[i].label);
            ++unpadded;
        }
        return fn;
    };
    glass::skia::Bridge& sk = glass::draw::bridge();
#define BIND(field, index) \
    sk.field = reinterpret_cast<decltype(sk.field)>(addr(glass::sf::index))
    BIND(MakeForShader, kMakeForShader);
    BIND(MakeForShaderOpts, kMakeForShaderOpts);
    BIND(SkStringCtor, kSkStringCtor);
    BIND(SkStringDtor, kSkStringDtor);
    BIND(findUniform, kFindUniform);
    BIND(findChild, kFindChild);
    BIND(uniformSize, kEffectUniformSize);
    // For trying, on a build that has them, the layout a build without them
    // gets (uniformOffsets): set before SurfaceFlinger starts.
    if (propGet("debug.glass.noFindUniform", "") == "1") {
        sk.findUniform = nullptr;
        sk.findChild = nullptr;
        LOGW("debug.glass.noFindUniform=1: uniforms laid out as on a build without findUniform");
    }
    BIND(ImageMakeShader, kImageMakeShader);
    BIND(DataMakeZeroInit, kDataMakeZeroInit);
    BIND(SkSpDataDtor, kSkSpDataDtor);
    BIND(DataUnref, kDataUnref);
    BIND(EffectMakeShader, kEffectMakeShader);
    BIND(MatrixPostConcat, kMatrixPostConcat);
    BIND(MatrixInvertNonIdentity, kMatrixInvertNonIdentity);
    BIND(PaintReset, kPaintReset);
    BIND(PaintDtor, kPaintDtor);
    BIND(CanvasDrawRect, kCanvasDrawRect);
    BIND(CanvasGetTotalMatrix, kCanvasGetTotalMatrix);
    BIND(PaintSetShader, kPaintSetShader);
    BIND(ImageMakeSubset, kImageMakeSubset);
    BIND(CanvasBaseRecorder, kCanvasBaseRecorder);
    BIND(CanvasRecorder, kCanvasRecorder);
#undef BIND
    if (bti_guarded && unpadded == 0) {
        LOGI("every function the engine calls starts on a landing pad");
    }
    if (!sk.drawReady()) {
        LOGE("skia bridge incomplete after binding; refusing to install");
        return false;
    }
    LOGI("compiling through %s", sk.MakeForShaderOpts != nullptr
                                     ? "MakeForShader(SkString, const Options&)"
                                     : "MakeForShader(SkString)");
    LOGI("a copy of part of the snapshot (snapshotCopy=bounds): %s",
         sk.ImageMakeSubset != nullptr && sk.CanvasBaseRecorder != nullptr
             ? "available" : "not on this build; panes read the whole snapshot");

    void* target = reinterpret_cast<void*>(bias + draw.value);
    LOGI("engine believes sizeof(SkPaint)=%zu align=%zu (headers pinned to "
         "android16-release; layout-identical to android17-release)",
         sizeof(SkPaint), alignof(SkPaint));
    LOGI("drawBlurRegion link=0x%llx bias=0x%llx runtime=%p (%llu bytes)",
         static_cast<unsigned long long>(draw.value), static_cast<unsigned long long>(bias),
         target, static_cast<unsigned long long>(draw.size));

    // How Android's blur maps onto blurRect on this build (blur_scale.h): 4 on
    // AOSP, 4.545 on Lunaris 3.13, per axis on Infinity-X 4.x. Read out of
    // drawBlurRegion's own code and the functions it calls, so before any of
    // them is hooked. Per axis, too, where drawBlurRegion asks the filter for
    // the image's size (ColorOS 17). Otherwise not found means 4, as before.
    glass::BlurScale blur_scale = glass::probeBlurScaleInProcess(exe, bias, draw);
    glass::applyFilterSizedImage(&blur_scale, g_syms[glass::sf::kBlurredImageSize].valid());
    glass::draw::setBlurScale(blur_scale.inverse, blur_scale.per_axis);
    LOGI("blur scale: %s", blur_scale.how.c_str());

    // gum needs executable anonymous memory and the right to re-protect a
    // patched text page, and it does not survive being refused either: on
    // alioth the denied allocation became a null dereference in this
    // constructor, before SurfaceFlinger's main(), on every start. Ask first,
    // on throwaway mappings, and stand down if the answer is no.
    std::string denied;
    if (!glass::canPatchCode(&denied)) {
        LOGE("SELinux will not let surfaceflinger patch code (%s). The module's sepolicy.rule "
             "grants execmem and execmod; it did not apply here. Not hooking: Android's blur is "
             "untouched", denied.c_str());
        return false;
    }

    gum_init_embedded();
    g_interceptor = gum_interceptor_obtain();
    if (g_interceptor == nullptr) {
        LOGE("gum_interceptor_obtain failed");
        return false;
    }

    dropBranchGuard(target, "drawBlurRegion");
    gum_interceptor_begin_transaction(g_interceptor);
    void** original_slot = original_slots[res.shape];
    const GumReplaceReturn r = gum_interceptor_replace(
        g_interceptor, target, replacement, reinterpret_cast<gpointer*>(original_slot), nullptr);
    gum_interceptor_end_transaction(g_interceptor);

    if (r != GUM_REPLACE_OK) {
        LOGE("replace failed: %d", static_cast<int>(r));
        return false;
    }
    if (*original_slot == nullptr) {
        LOGE("no trampoline to the original; reverting");
        gum_interceptor_revert(g_interceptor, target);
        return false;
    }

    // generateTemporaryImage, for backdropBlur only. Each one is optional: one
    // that does not resolve or will not hook leaves a toned backdrop at the
    // radius it asked for, and the draw hook above is unaffected either way.
    // Installed after the draw hook, in a transaction of its own, so a failure
    // here can never leave the draw half-hooked.
    const auto gen_hook = generateHooks(std::make_index_sequence<kFilterCount>{});
    int generate_hooked = 0;
    gum_interceptor_begin_transaction(g_interceptor);
    for (size_t k = 0; k < kFilterCount; ++k) {
        const size_t index = glass::sf::kGenerateFilters[k];
        const glass::Symbol& sym = g_syms[index];
        if (!sym.valid()) continue;
        const char* label = glass::sf::kEntryPoints[index].label;
        if (sym.size <= 64) {
            LOGW("%s is only %llu bytes; not hooking it", label,
                 static_cast<unsigned long long>(sym.size));
            continue;
        }
        void* gen_target = reinterpret_cast<void*>(bias + sym.value);
        dropBranchGuard(gen_target, label);
        gpointer original = nullptr;
        const GumReplaceReturn gr = gum_interceptor_replace(g_interceptor, gen_target,
                                                            gen_hook[k], &original, nullptr);
        if (gr != GUM_REPLACE_OK || original == nullptr) {
            LOGW("could not hook %s (%d); backdropBlur will not use it", label,
                 static_cast<int>(gr));
            if (gr == GUM_REPLACE_OK) gum_interceptor_revert(g_interceptor, gen_target);
            continue;
        }
        // generateHook<k> reaches it through the drawing code once this
        // transaction ends, as it did when gum wrote it there itself.
        glass::draw::setGenerator(k, reinterpret_cast<glass::draw::GenerateFn>(original));
        ++generate_hooked;
        LOGI("hooked %s (lighter backdrop blur available)", label);
    }
    gum_interceptor_end_transaction(g_interceptor);
    if (generate_hooked == 0) {
        LOGI("no generateTemporaryImage on this build; backdropBlur and fillBlurScale keep "
             "the requested radius");
    }
    // For glassctl and the WebUI: whether Blur Intensity can do anything here.
    glass::draw::setRegen(generate_hooked != 0);

    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
    LOGI("hook installed in %lld ms", static_cast<long long>(ms));
    return true;
}

}  // namespace

__attribute__((constructor)) static void glassInit() {
    if (!glass::draw::enabledAtBoot()) {
        LOGI("disabled by persist.sys.glass.enabled=0");
        return;
    }
    if (!guardShouldRun()) return;

    // Before anything reads the mode: if the last SurfaceFlinger died drawing
    // glass, this one starts stock.
    glass::crashGuardStart();
    glass::paramsInit();

    if (install()) {
        glass::draw::setStatus("hooked");
        LOGI("hook ready; persist.sys.glass.mode=glass to draw Liquid Glass, anything else "
             "keeps Android's blur");
        if (!g_module_guard) guardDisarmWhenBooted();
    } else {
        glass::draw::setStatus("not-hooked");
        // Nothing was hooked, so nothing can be blamed on us — disarm
        // immediately rather than tripping the guard on the next boot.
        if (!g_module_guard) unlink(guardPaths().pending.c_str());
    }
}
