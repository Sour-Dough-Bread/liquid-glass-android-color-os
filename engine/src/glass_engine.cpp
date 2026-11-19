// libglassengine.so — loaded into /system/bin/surfaceflinger.
//
// Hooks BlurFilter::drawBlurRegion. In glass mode it draws the Liquid Glass
// shader over the compositor's own two inputs: a quarter-resolution blur of
// what is behind the region, and the sharp snapshot of it. In stock mode — the
// default — it calls the original with the arguments it was given and touches
// nothing else, not even the shader compiler. Switching between the two is a
// property write and takes effect on the next frame.
//
// On Android 17 it also hooks the blur filter's generateTemporaryImage, and in
// glass mode makes the blurs itself: only the ones it draws, at the radius it
// draws them at ("blur made to order", below). In stock mode that hook too
// passes straight through.
//
// Everything here runs inside SurfaceFlinger. A crash is a soft reboot, so the
// rule throughout is: any failure logs and falls through to the original
// function, leaving stock behaviour intact. There is no path that aborts, and
// no path that draws with anything unverified.

#include <android/log.h>
#include <frida-gum.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>

#include "abi_check.h"
#include "composite_memory.h"
#include "crash_guard.h"
#include "region_merge.h"
#include "region_tint.h"
#include "elf_syms.h"
#include "exec_probe.h"
#include "generated/glass_shader_src.h"
#include "glass_params.h"
#include "sf_symbols.h"
#include "sharp_bounds.h"
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

// What the engine did, for glassctl and the WebUI: logcat on a phone keeps
// about a minute, so a refusal at boot is long gone by the time anyone asks.
//   hooked       installed; Android's blur until glass is wanted
//   glass        the shader compiled and validated; glass is being drawn
//   no-glass     installed, but the shader did not validate: stock blur
//   not-hooked   declined to install (logcat -s StockGlass says why)
void engineStatus(const char* s) { __system_property_set("sys.glass.engine", s); }

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
    const Guard g = guardPaths();

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

// ----------------------------------------------------------- the shader

// Declaration order in shader/gen_glass_sksl.py. Checked against the generated
// name list at compile time, so adding a uniform to the shader without adding
// it here is caught before anything draws rather than showing up as a pane with
// one wrong number in it.
enum UniformId : size_t {
    U_size = 0, U_offset, U_crop, U_cornerExtents, U_cornerShapes, U_unitScale,
    U_refractionScale, U_refractionAmount, U_refractionSwirl, U_depthEffect,
    U_profilePower, U_profileBias, U_blurMix, U_blurFalloff, U_lensBlur, U_rimFrost, U_tintColor,
    U_frostColor, U_highlightIntensity, U_highlightDir, U_highlightWidth,
    U_highlightFalloff, U_lightIntensity, U_glassOpacity, U_saturation,
    U_hdrHeadroom, U_touchPos, U_touchGlow, U_touchLens, U_softenRadius, U_softenFast,
    U_deepBand, U_fillLighten, U_fillDarken, U_lumaLimit, U_toneScale, U_toneBias, U_filterColor,
    U_dispersionHeight, U_dispersionAmount, U_dispersionTapSpacing, U_dispersionQuadrant,
    U_noiseAmount, U_count,
};

constexpr const char* kUniformNames[U_count] = {
    "size", "offset", "crop", "cornerExtents", "cornerShapes", "unitScale",
    "refractionScale", "refractionAmount", "refractionSwirl", "depthEffect",
    "profilePower", "profileBias", "blurMix", "blurFalloff", "lensBlur", "rimFrost", "tintColor",
    "frostColor", "highlightIntensity", "highlightDir", "highlightWidth",
    "highlightFalloff", "lightIntensity", "glassOpacity", "saturation",
    "hdrHeadroom", "touchPos", "touchGlow", "touchLens", "softenRadius", "softenFast",
    "deepBand", "fillLighten", "fillDarken", "lumaLimit", "toneScale", "toneBias", "filterColor",
    "dispersionHeight", "dispersionAmount", "dispersionTapSpacing", "dispersionQuadrant",
    "noiseAmount",
};

// The tone shader's (shader/gen_glass_sksl.py, TONE_UNIFORMS): what is left of
// the glass when its lens is off. Each means what it means above.
enum ToneUniformId : size_t {
    T_size = 0, T_offset, T_crop, T_cornerExtents, T_cornerShapes, T_frostColor,
    T_glassOpacity, T_saturation, T_hdrHeadroom, T_count,
};

constexpr const char* kToneUniformNames[T_count] = {
    "size", "offset", "crop", "cornerExtents", "cornerShapes", "frostColor",
    "glassOpacity", "saturation", "hdrHeadroom",
};

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

GumInterceptor* g_interceptor = nullptr;
DrawBlurRegionFn g_original = nullptr;
DrawBlurRegionScaledFn g_original_scaled = nullptr;
uint64_t g_calls = 0;

// Every resolved entry point, indexed by glass::sf::Index.
glass::Symbol g_syms[glass::sf::kCount];
uintptr_t g_bias = 0;
glass::skia::Bridge g_sk;

// The compiled glass shader, built on first use rather than at load time:
// the engine's constructor runs before SurfaceFlinger has initialised Skia, and
// compiling there would be both too early and a needless addition to boot time.
SkRuntimeEffect* g_effect = nullptr;
bool g_shader_tried = false;
bool g_shader_ok = false;
size_t g_uniform_size = 0;
size_t g_uniform_offset[U_count] = {0};
int g_child_blurred = 0;
int g_child_sharp = 1;

// The lean builds of the glass shader (shader/gen_glass_sksl.py, "lean"): the
// same picture from less code, for a pane whose uniforms rule the rest out.
// Compiled with the glass shader and optional: without one, or with
// persist.sys.glass.leanShader=0, every pane is drawn by the full shader.
// kDeep is not glass: the lean build with no lens, for measuring (debugPane=3).
enum LeanBuild : size_t { kLean = 0, kLeanSoft, kDeep, kLeanCount };
SkRuntimeEffect* g_lean_effect[kLeanCount] = {nullptr, nullptr, nullptr};
// ...and of the tone shader: its SDF without the superellipse corner.
SkRuntimeEffect* g_tone_lean_effect = nullptr;

// The tone shader, compiled with the glass one. Optional: without it a toned
// backdrop is drawn by the glass shader with its lens off, as before.
SkRuntimeEffect* g_tone_effect = nullptr;
bool g_tone_ok = false;
size_t g_tone_uniform_size = 0;
size_t g_tone_uniform_offset[T_count] = {0};

// ---------------------------------------------------------------- stats
//
// persist.sys.glass.stats=1: one line a second saying what the engine made and
// drew, per composited frame. The cost of glass is passes, not lines of code:
// how many times the layer was blurred, whether a draw named the snapshot (and
// so had Skia copy the framebuffer), how many regions drew. harness/bench.sh
// prints the last line beside the frame time it measured.
struct Stats {
    uint32_t frames = 0;       // snapshots drawn from (a layer with blur, one frame)
    uint32_t android = 0;      // blurs Android made itself before the draw
    uint32_t deferred = 0;     // ...and ones it asked for that were put off
    uint32_t full = 0;         // blurs of the snapshot the engine made
    uint32_t derived = 0;      // blurs of a backdrop's blur (a sixteenth the pixels)
    uint32_t backdrops = 0;    // backdrops drawn by the glass shader
    uint32_t tones = 0;        // ...and by the tone shader
    uint32_t panes = 0;
    uint32_t skipped = 0;      // regions that drew nothing (off screen, or merged away)
    uint32_t stock = 0;        // regions handed back to Android
    uint32_t snapshot = 0;     // frames in which a draw read the sharp snapshot
    uint32_t covered = 0;      // panes drawn from a light blur, a backdrop being above
    uint32_t quarter = 0;      // ...and by choice (glassResolution=quarter)
    uint32_t single = 0;       // panes drawn from one blur and nothing else (paneBlur=through)
    uint32_t lean = 0;         // regions drawn by a lean build of the glass shader
    uint32_t bounded = 0;      // copies of part of the snapshot made for panes to read
    int64_t hook_ns = 0;       // time inside the draw hook, generation included
    int64_t window_ms = 0;
};
Stats g_stats;
uint32_t g_stats_frame = 0;      // the snapshot the counters last saw
bool g_stats_snapshot = false;   // ...and whether a draw has read it yet

int64_t monotonicNs() {
    struct timespec now {};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<int64_t>(now.tv_sec) * 1000000000LL + now.tv_nsec;
}

void statsFrame(uint32_t input_id) {
    if (input_id == g_stats_frame) return;
    g_stats_frame = input_id;
    g_stats_snapshot = false;
    ++g_stats.frames;
}

// A draw into the output named the snapshot. Skia cannot sample a surface
// while drawing into it, so the first such draw of a frame costs a copy of the
// whole framebuffer; Android's own blur only reads it into other surfaces.
void statsSnapshotRead() {
    if (g_stats_snapshot) return;
    g_stats_snapshot = true;
    ++g_stats.snapshot;
}

void statsFlush() {
    if (glass::param(glass::P_stats) == 0.0f) {
        g_stats.window_ms = 0;  // off: whatever was counted is not a window
        return;
    }
    const int64_t ms = monotonicNs() / 1000000;
    if (g_stats.window_ms == 0) {
        // Just switched on: count from here, not from boot.
        g_stats = Stats{};
        g_stats.window_ms = ms;
        return;
    }
    if (ms - g_stats.window_ms < 1000) return;
    const Stats& t = g_stats;
    if (t.frames != 0) {
        const float f = static_cast<float>(t.frames);
        LOGI("stats: frames=%u blurs/frame=%.2f (android %.2f + engine %.2f) derived/frame=%.2f "
             "deferred/frame=%.2f snapshot-copies/frame=%.2f bounds-copies/frame=%.2f tone=%u "
             "backdrop=%u pane=%u covered=%u quarter=%u single=%u lean=%u skipped=%u stock=%u "
             "hook=%.0fus/frame",
             t.frames, (t.android + t.full) / f, t.android / f, t.full / f, t.derived / f,
             t.deferred / f, t.snapshot / f, t.bounded / f, t.tones, t.backdrops, t.panes,
             t.covered, t.quarter, t.single, t.lean, t.skipped, t.stock,
             static_cast<double>(t.hook_ns) / 1000.0 / f);
    }
    g_stats = Stats{};
    g_stats.window_ms = ms;
}

// How pane-like a region is, in [0, 1].
//
// The refraction direction is `normal + refractionDepth * radial`, and `radial`
// points away from the region's CENTRE. On a pane-sized rectangle that is a mild
// dome; on a full-screen region it swings the direction 20-45 degrees off the
// edge normal along the long edges, dragging samples sideways and refracting
// straight backdrop lines into arcs (docs/VERIFIED.md).
//
// The clamp is what makes a steep curve safe: any region whose longest side is
// at or under paneSpanDp gets exactly 1 and is untouched — 1 to the fourth is
// still 1 — so the falloff can be as aggressive as the full-screen case needs
// without altering a single pane-sized region. Fourth power because squaring
// only reached 0.28 on a 2400 px region, which still visibly arcs; this reaches
// 0.08. 480 dp sits well above a volume panel or notification row and well below
// a full-screen scrim.
float panenessFor(float w, float h) {
    const float span = glass::param(glass::P_paneSpanDp) * glass::paramsUnitScale();
    if (span <= 0.0f) return 1.0f;   // scaling disabled; raw upstream behaviour
    const float t = std::clamp(span / std::max(std::max(w, h), 1.0f), 0.0f, 1.0f);
    return t * t * t * t;
}

// ------------------------------------------------- backdrop and panes
//
// What a region IS decides what it gets. A region whose on-screen rect spans
// the display is a backdrop: the notification shade, a dialog's blur-behind.
// Anything smaller is a pane: a notification card, a piece of the volume panel,
// a blurred dialog window. Measured on alioth (crDroid A17): the shade is one
// full-screen layer with backgroundBlurRadius 77, and each card and each piece
// of the volume panel is its own blur region; Quick Settings tiles are not
// blur regions at all.
enum class Role { kPane, kBackdropGlass, kBackdropTone };

const char* roleName(Role r) {
    switch (r) {
        case Role::kPane: return "pane";
        case Role::kBackdropGlass: return "backdrop/glass";
        case Role::kBackdropTone: return "backdrop/tone";
    }
    return "?";
}

// The radius a region is drawn with, as a multiple of the one it asked for:
// fillBlurScale for everything, times backdropBlur for a toned backdrop.
float blurScaleFor(Role role) {
    const float scale = std::clamp(glass::param(glass::P_fillBlurScale), 0.05f, 4.0f);
    if (role != Role::kBackdropTone) return scale;
    const float backdrop = glass::param(glass::P_backdropBlur);
    return backdrop >= 0.0f ? backdrop * scale : scale;
}

// persist.sys.glass.backdrop, rounded: 0 stock, 1 tone, 2 glass.
int backdropStyle() {
    const float v = glass::param(glass::P_backdrop);
    if (!(v >= 0.5f)) return 0;
    return v < 1.5f ? 1 : 2;
}

// The rect's bounds on the output buffer under `m`, perspective included.
SkRect deviceBounds(const SkMatrix& m, const SkRect& r) {
    const float xs[4] = {r.fLeft, r.fRight, r.fRight, r.fLeft};
    const float ys[4] = {r.fTop, r.fTop, r.fBottom, r.fBottom};
    float l = 1e30f, t = 1e30f, rr = -1e30f, b = -1e30f;
    for (int i = 0; i < 4; ++i) {
        float w = m.get(6) * xs[i] + m.get(7) * ys[i] + m.get(8);
        if (!(std::fabs(w) > 1e-6f)) w = 1e-6f;
        const float x = (m.get(0) * xs[i] + m.get(1) * ys[i] + m.get(2)) / w;
        const float y = (m.get(3) * xs[i] + m.get(4) * ys[i] + m.get(5)) / w;
        l = std::min(l, x); t = std::min(t, y); rr = std::max(rr, x); b = std::max(b, y);
    }
    return SkRect::MakeLTRB(l, t, rr, b);
}

// `input` is the snapshot of the whole output buffer, so its size is the
// display's (or the screenshot's) — the one size every region is judged against.
//
// width() and height() are inline reads at the pinned header layout
// (layout_check.sh). RenderEngine clips blurRect to the device clip, so it must
// lie inside the image; if it does not, the read is wrong, and this is false.
bool displaySize(const SkImage* input, const SkRect& blur_rect, float* w, float* h) {
    *w = static_cast<float>(input->width());
    *h = static_cast<float>(input->height());
    const bool sane = *w > 0.0f && *h > 0.0f && *w <= 16384.0f && *h <= 16384.0f &&
                      blur_rect.fLeft >= -1.0f && blur_rect.fTop >= -1.0f &&
                      blur_rect.fRight <= *w + 1.0f && blur_rect.fBottom <= *h + 1.0f;
    if (!sane) {
        static bool s_warned = false;
        if (!s_warned) {
            s_warned = true;
            LOGW("input image reads as %.0fx%.0f, which does not contain blurRect "
                 "[%.0f %.0f %.0f %.0f]; treating every region as a pane",
                 *w, *h, blur_rect.fLeft, blur_rect.fTop, blur_rect.fRight, blur_rect.fBottom);
        }
    }
    return sane;
}

// A region whose on-screen rect spans the display. With an unreadable display
// size everything stays a pane — the behaviour from before backdrops existed.
bool coversDisplay(const SkMatrix& ctm, const SkRect& rect, const SkRect& blur_rect,
                   const SkImage* input) {
    const float cover = glass::param(glass::P_backdropCoverage);
    if (!(cover > 0.0f)) return false;  // 0 or less: nothing is a backdrop
    float w = 0.0f, h = 0.0f;
    if (!displaySize(input, blur_rect, &w, &h)) return false;
    const SkRect d = deviceBounds(ctm, rect);
    const float iw = std::min(d.fRight, w) - std::max(d.fLeft, 0.0f);
    const float ih = std::min(d.fBottom, h) - std::max(d.fTop, 0.0f);
    return iw >= cover * w && ih >= cover * h;
}

// persist.sys.glass.paneBlur, rounded: 0 own, 1 derive, 2 through.
int paneBlurMode() {
    const float v = glass::param(glass::P_paneBlur);
    if (!(v >= 0.5f)) return 0;
    return v < 1.5f ? 1 : 2;
}

// The lightest blur the compositor's quarter-resolution buffer is read
// through in the sharp screen's place, px. 4.5 dp is 12 px on a phone, which
// is where Android's own blur stops cross-fading the sharp image in to hide
// the buffer's grain (BlurFilter's kMaxCrossFadeRadius is 10); under it a
// quarter-resolution image shimmers as what is behind it moves.
uint32_t lightViewRadius() {
    return std::max<uint32_t>(
        10, static_cast<uint32_t>(std::lround(4.5f * glass::paramsUnitScale())));
}

// What a pane with nothing blurred under it reads in the sharp screen's place
// when the user has chosen that (the WebUI's Performance card): the radius of
// a blur of the snapshot, or 0 for the screen itself. `wanted_radius` is the
// radius of the pane's own blur.
//
//   glassResolution=quarter  the lightest blur the buffer holds still at; the
//                            pane's own blur is then made from that image.
//   paneBlur=through         one blur for BOTH of the pane's reads (*single),
//                            which has to stand for a mix of two pictures: the
//                            sharp screen and the pane's blur. How much of the
//                            mix is blur decides which one it is closer to:
//                            all of blurMix and, of what is left, the half the
//                            blur fill takes on average (fillLighten pulls
//                            toward the blur where it is brighter, fillDarken
//                            where it is darker). Up to 0.3 (the clear look)
//                            what is behind the glass can still be read, and
//                            only the lightest blur keeps that. From 0.9 (the
//                            frosted look) nothing of it can, and the pane's
//                            own blur is the same picture. In between, in
//                            between, on a smoothstep: the regular look, 0.72,
//                            comes out at four fifths of the way.
uint32_t chosenViewRadius(uint32_t wanted_radius, bool* single) {
    const uint32_t light = lightViewRadius();
    *single = paneBlurMode() == 2;
    if (*single) {
        if (wanted_radius <= light) return light;
        const float mix = std::clamp(glass::param(glass::P_blurMix), 0.0f, 1.0f);
        const float fill = std::clamp(
            glass::param(glass::P_fillLighten) + glass::param(glass::P_fillDarken), 0.0f, 1.0f);
        const float share = mix + (1.0f - mix) * 0.5f * fill;
        float t = std::clamp((share - 0.3f) / 0.6f, 0.0f, 1.0f);
        t = t * t * (3.0f - 2.0f * t);
        return light + static_cast<uint32_t>(
                           std::lround(t * static_cast<float>(wanted_radius - light)));
    }
    return glass::param(glass::P_glassResolution) >= 0.5f ? light : 0;
}

// ---- generateTemporaryImage: blur made to order
//
// RenderEngine blurs a layer before it draws the layer's regions: once for the
// background blur and once more for every other radius its regions ask for,
// each a pass over the layer's whole bounds (a 4x downsample, then up to four
// halvings and as many upsamples). drawBlurRegion is handed the result.
//
// Glass draws few of those blurs. A toned backdrop wants backdropBlur of the
// radius, a pane fillBlurScale of it, and a pane over a backdrop needs no blur
// of the snapshot at all (below). The engine used to ask the filter again for
// the radius it wanted and leave Android's unused: the default look blurred
// the shade three times a frame, and a fourth when Quick Settings tiles and
// notification cards were both in it, to draw one.
//
// So while glass is being drawn the hook answers in the filter's place: it
// hands RenderEngine the snapshot back, untouched, and remembers the
// arguments. drawBlurRegion then sees a "blurred" image that IS the input,
// knows nothing was made, and makes what it is about to draw at the radius it
// draws it at. Android's own blur is still made whenever Android does the
// drawing: stock mode, a backdrop left to Android, any region the engine hands
// back (drawWithAndroidsBlur).
//
// The snapshot stands in for a blur only between this hook and the
// drawBlurRegion calls it is passed to, and every one of those is hooked;
// nothing else in RenderEngine reads it (android17-release,
// SkiaRenderEngine.cpp). Android 16 and earlier have no generateTemporaryImage:
// nothing is put off there, and the blur drawn is the one Android made.
using GenerateFn = glass::skia::ReturnedSkSp (*)(const void* self, void* context,
                                                 const void* display, uint32_t radius,
                                                 void* input_sp, const SkRect* blur_rect);
constexpr size_t kFilterCount = 3;
GenerateFn g_generate[kFilterCount] = {};

struct GenerateCall {
    bool valid = false;
    size_t filter = 0;
    const void* self = nullptr;
    void* context = nullptr;
    const void* display = nullptr;
    uint32_t input_id = 0;
    SkRect blur_rect = SkRect::MakeEmpty();
};
GenerateCall g_last_generate;

// The backdrop most recently drawn, so that panes in the same layer can see it.
//
// RenderEngine snapshots a layer's input ONCE, before its background blur is
// drawn, and every blur region in that layer samples the same snapshot. On
// stock that is harmless — each region blurs the snapshot at its own radius —
// but glass that shows what is behind it would show the SHARP home screen
// through a notification card while the shade around the card is blurred. So a
// pane over a backdrop samples the backdrop's blur in place of the snapshot,
// and takes on its tone. Matched on the input image's unique ID and the blur
// rect, which together mean "this layer, this frame"; IDs are never reused.
struct BackdropRecord {
    uint32_t input_id = 0;
    SkRect blur_rect = SkRect::MakeEmpty();
    float zoom = 1.0f;
    SkImage* image = nullptr;  // one reference held
    uint32_t radius = 0;       // the radius `image` was blurred at
    bool usable = false;       // false while the backdrop is still fading in
    float saturation = 1.0f;
    float veil = 0.0f;
};
BackdropRecord g_backdrop;

void recordBackdrop(const SkImage* input, const SkRect& blur_rect, float zoom, SkImage* image,
                    uint32_t radius, bool usable, float saturation, float veil) {
    SkSafeRef(image);
    SkSafeUnref(g_backdrop.image);
    g_backdrop = {input->uniqueID(), blur_rect, zoom, image, radius, usable && image != nullptr,
                  saturation, veil};
}

const BackdropRecord* backdropUnder(const SkImage* input, const SkRect& blur_rect) {
    if (!g_backdrop.usable || g_backdrop.input_id != input->uniqueID() ||
        g_backdrop.blur_rect != blur_rect) {
        return nullptr;
    }
    return &g_backdrop;
}

// A blur of `source` over `rect` at `radius`, from the filter RenderEngine last
// asked for this layer's. `source` is the snapshot (and `rect` the layer's blur
// rect) or a blur of it (and `rect` that image's own bounds). Only when the
// last generate call was for this very input image and blur rect (this layer,
// this frame), so the context and display it captured are the ones still in
// use; anything else returns null.
//
// An image made earlier stays valid: on Ganesh a temporary image is a
// copy-on-write snapshot, so writing the filter's surfaces again copies rather
// than clobbers it (and the engine stands down on Graphite).
sk_sp<SkImage> makeBlur(const SkImage* input, const SkRect& blur_rect, SkImage* source,
                        const SkRect& rect, uint32_t radius) {
    const GenerateCall& g = g_last_generate;
    if (!g.valid || g.input_id != input->uniqueID() || g.blur_rect != blur_rect ||
        g.filter >= kFilterCount || g_generate[g.filter] == nullptr) {
        return nullptr;
    }
    // The image is an sk_sp by value, so the callee is given the address of one:
    // a slot holding one reference, as the caller's temporary would. AOSP's
    // filters only read it, but whatever a filter leaves in the slot is
    // released here, as that temporary's destructor would, so one that moved
    // out of it has taken a reference that was there to take.
    SkSafeRef(source);
    SkImage* slot = source;
    sk_sp<SkImage> blur(static_cast<SkImage*>(
        g_generate[g.filter](g.self, g.context, g.display, radius, &slot, &rect).pointer()));
    SkSafeUnref(slot);
    return blur;
}

// A pane's blur goes up to twice the radius it asked for, and never past this:
// the filter's pass count grows with the radius, and past ~300 px the quarter-
// resolution image has nothing left to blur.
constexpr uint32_t kMaxRegenRadius = 300;

// makeBlur(), remembered for the rest of the layer. Every notification card in
// the shade asks for the same radius from the same snapshot, so they share one
// image instead of making one each; a backdrop, the panes over it and a second
// group of panes at another radius want three or four in one layer. The
// input's unique ID changes every frame, so an entry is never reused across
// frames, and the next frame's first generate call lets them all go: their
// textures are back in Skia's pool before that frame makes its own.
struct MadeBlur {
    uint32_t input_id = 0;   // the layer and frame it was made for
    uint32_t source_id = 0;  // what was blurred: the snapshot, or a blur of it
    SkRect rect = SkRect::MakeEmpty();
    uint32_t radius = 0;
    SkImage* image = nullptr;  // one reference held
};
constexpr size_t kMadeBlurs = 6;
MadeBlur g_made[kMadeBlurs];
size_t g_made_next = 0;

// The snapshot whose output has been drawn into since it was taken, by the
// engine or by Android through it. Until then the snapshot still shares the
// output's texture and reading it costs nothing more; after, Skia reads a copy
// of the whole framebuffer (and a copy of part of it has to be made before).
uint32_t g_output_drawn = 0;

sk_sp<SkImage> blurCached(const SkImage* input, const SkRect& blur_rect, SkImage* source,
                          const SkRect& rect, uint32_t radius) {
    const uint32_t id = input->uniqueID();
    const uint32_t source_id = source->uniqueID();
    for (const MadeBlur& e : g_made) {
        if (e.image != nullptr && e.input_id == id && e.source_id == source_id &&
            e.radius == radius && e.rect == rect) {
            return sk_ref_sp(e.image);
        }
    }
    sk_sp<SkImage> image = makeBlur(input, blur_rect, source, rect, radius);
    if (image == nullptr) return nullptr;
    ++(source == input ? g_stats.full : g_stats.derived);
    // A blur of the snapshot made after its output was drawn into reads the
    // copy Skia then has to make of it, like a draw that names it.
    if (source == input && g_output_drawn == id) statsSnapshotRead();
    // A slot nothing in this frame holds, else the oldest.
    size_t slot = kMadeBlurs;
    for (size_t i = 0; i < kMadeBlurs; ++i) {
        if (g_made[i].image == nullptr || g_made[i].input_id != id) {
            slot = i;
            break;
        }
    }
    if (slot == kMadeBlurs) {
        slot = g_made_next;
        g_made_next = (g_made_next + 1) % kMadeBlurs;
    }
    SkSafeRef(image.get());
    SkSafeUnref(g_made[slot].image);
    g_made[slot] = {id, source_id, rect, radius, image.get()};
    return image;
}

// The snapshot's own blur at `radius`: what Android would have made.
sk_sp<SkImage> blurOfSnapshot(SkImage* input, const SkRect& blur_rect, uint32_t radius) {
    return blurCached(input, blur_rect, input, blur_rect, radius);
}

// ---- a copy of the part of the snapshot the panes read (snapshotCopy=bounds)
//
// sharp_bounds.h has the bet and when it is taken. The copy is Skia's own
// makeSubset, made before the output is drawn into: until then the snapshot
// still shares the output's texture, so reading it into the copy costs
// nothing else, and the panes that then read the copy never name the
// snapshot, which is what would have made Skia copy all of it.
//
// The copy is not budgeted (Skia makes it as the snapshot's own texture is, a
// wrapped one), so letting go of it can delete its texture there and then
// rather than hand it to the cache. RenderEngine has two GPU contexts, one for
// protected content, and a texture has to be deleted under its own. So the
// copy remembers the context it was made in and is only let go while that one
// is current again; in between no other copy is made.
struct SharpCopy {
    uint32_t input_id = 0;                  // the snapshot it was, or was not, made of
    SkIRect bounds = SkIRect::MakeEmpty();  // where on the snapshot
    SkImage* image = nullptr;               // one reference held; null = none this frame
    const void* context = nullptr;          // the RenderEngine context it was made in
};
SharpCopy g_sharp;
glass::SharpBounds g_sharp_bounds;

// Lets go of everything made for another snapshot than `input_id`. `context`
// is the RenderEngine context now in use.
void releaseStaleBlurs(uint32_t input_id, const void* context) {
    if (g_sharp.image != nullptr && g_sharp.input_id != input_id && g_sharp.context == context) {
        SkSafeUnref(g_sharp.image);
        g_sharp = SharpCopy{};
    }
    for (MadeBlur& e : g_made) {
        if (e.image != nullptr && e.input_id != input_id) {
            SkSafeUnref(e.image);
            e = MadeBlur{};
        }
    }
    if (g_backdrop.image != nullptr && g_backdrop.input_id != input_id) {
        SkSafeUnref(g_backdrop.image);
        g_backdrop = BackdropRecord{};
    }
}

// A snapshot handed to RenderEngine in a blur's place that no drawBlurRegion
// has been given yet (its unique ID), or 0. Android 17's RenderEngine draws a
// region with every blur straight after asking for it, so this is back to 0
// before the next blur is asked for. If it is not, something else on this
// build takes the filter's result, and it was just handed a sharp snapshot:
// from then on nothing is put off, until SurfaceFlinger restarts.
uint32_t g_deferred_unseen = 0;
bool g_lazy_refused = false;

// Called by the draw hook for every region: the blur it was handed, if it was
// one that had been put off, has now reached a draw.
void deferredBlurSeen(void* blurred_sp, void* input_sp) {
    if (g_deferred_unseen == 0 || blurred_sp == nullptr || input_sp == nullptr) return;
    if (*static_cast<SkImage**>(blurred_sp) == *static_cast<SkImage**>(input_sp)) {
        g_deferred_unseen = 0;
    }
}

// Whether Android's blur can wait for the draw: only while glass is being
// drawn, since only then does every drawBlurRegion come to drawGlassCore
// first. Not before the shader has validated (the first glass frame keeps
// Android's blur), and not while the crash guard holds glass off.
bool deferGeneration() {
    if (!g_shader_ok || g_lazy_refused) return false;
    if (g_deferred_unseen != 0) {
        g_lazy_refused = true;
        g_deferred_unseen = 0;
        LOGW("a blur that was put off never reached drawBlurRegion: something else on this "
             "build uses the blur filter. Android makes its own blurs from here on "
             "(as persist.sys.glass.lazyBlur=0)");
        return false;
    }
    glass::paramsRefresh();
    return glass::paramsMode() == glass::Mode::kGlass && !glass::crashGuardHolding() &&
           glass::param(glass::P_lazyBlur) != 0.0f;
}

template <size_t K>
glass::skia::ReturnedSkSp generateHook(const void* self, void* context, const void* display,
                                       uint32_t radius, void* input_sp,
                                       const SkRect* blur_rect) {
    SkImage* input = input_sp != nullptr ? *static_cast<SkImage* const*>(input_sp) : nullptr;
    if (input != nullptr && blur_rect != nullptr) {
        g_last_generate = {true, K, self, context, display, input->uniqueID(), *blur_rect};
        releaseStaleBlurs(input->uniqueID(), context);
        if (deferGeneration()) {
            // The snapshot itself, with a reference for the caller's sk_sp:
            // drawGlassCore tells it from a blur by its being the input.
            ++g_stats.deferred;
            g_deferred_unseen = input->uniqueID();
            SkSafeRef(input);
            return glass::skia::ReturnedSkSp(input);
        }
    } else {
        g_last_generate.valid = false;
    }
    ++g_stats.android;
    // The result is a prvalue of the same type, so C++17 constructs it straight
    // into the caller's return slot: nothing is copied or destroyed here.
    return g_generate[K](self, context, display, radius, input_sp, blur_rect);
}

// The tone shader (shader/gen_glass_sksl.py): what a toned backdrop is drawn
// with. It is a saving, not a requirement, so every failure here only means
// the glass shader goes on drawing backdrops with its lens off.
void compileToneShader() {
    if (glass::kGlassToneUniformNames_count != static_cast<int>(T_count) ||
        glass::kGlassToneChildNames_count != 1) {
        LOGW("tone shader declares %d uniforms and %d children, engine knows %zu and 1; "
             "backdrops keep the glass shader",
             glass::kGlassToneUniformNames_count, glass::kGlassToneChildNames_count, T_count);
        return;
    }
    std::string err;
    SkRuntimeEffect* effect = glass::skia::compile(g_sk, glass::kGlassShaderTone, &err);
    if (effect == nullptr) {
        LOGW("tone shader did not compile (%s); backdrops keep the glass shader", err.c_str());
        return;
    }
    const size_t size = g_sk.uniformSize(effect);
    if (size == 0 || size > (1u << 16)) {
        LOGW("tone shader: implausible uniform blob size %zu; not used", size);
        return;
    }
    for (size_t i = 0; i < T_count; ++i) {
        const SkRuntimeEffect::Uniform* u = g_sk.findUniform(effect, kToneUniformNames[i]);
        if (u == nullptr || u->offset >= size) {
            LOGW("tone shader: uniform %s unresolved; not used", kToneUniformNames[i]);
            return;
        }
        g_tone_uniform_offset[i] = u->offset;
    }
    const SkRuntimeEffect::Child* c = g_sk.findChild(effect, "blurredContent");
    if (c == nullptr || c->index != 0) {
        LOGW("tone shader: child unresolved; not used");
        return;
    }
    g_tone_effect = effect;
    g_tone_uniform_size = size;
    g_tone_ok = true;
    LOGI("tone shader validated (%zu chars, %zu uniforms, blob %zu bytes)",
         sizeof(glass::kGlassShaderTone) - 1, T_count, size);
}

// The lean builds. Each declares the glass shader's uniforms and children as
// the glass shader does, so that one uniform block and one pair of children
// serve all three; a build that this Skia lays out any other way is not used.
void compileLeanShaders() {
    static constexpr struct {
        const char* name;
        const char* source;
        size_t chars;
    } kBuilds[kLeanCount] = {
        {"lean", glass::kGlassShaderLean, sizeof(glass::kGlassShaderLean) - 1},
        {"lean_soft", glass::kGlassShaderLeanSoft, sizeof(glass::kGlassShaderLeanSoft) - 1},
        {"deep", glass::kGlassShaderDeep, sizeof(glass::kGlassShaderDeep) - 1},
    };
    for (size_t b = 0; b < kLeanCount; ++b) {
        std::string err;
        SkRuntimeEffect* effect = glass::skia::compile(g_sk, kBuilds[b].source, &err);
        if (effect == nullptr) {
            LOGW("%s shader did not compile (%s); its panes keep the full shader",
                 kBuilds[b].name, err.c_str());
            continue;
        }
        bool same = g_sk.uniformSize(effect) == g_uniform_size;
        for (size_t i = 0; same && i < U_count; ++i) {
            const SkRuntimeEffect::Uniform* u = g_sk.findUniform(effect, kUniformNames[i]);
            same = u != nullptr && u->offset == g_uniform_offset[i];
        }
        const SkRuntimeEffect::Child* blurred = g_sk.findChild(effect, "blurredContent");
        const SkRuntimeEffect::Child* sharp = g_sk.findChild(effect, "sharpContent");
        same = same && blurred != nullptr && sharp != nullptr &&
               blurred->index == g_child_blurred && sharp->index == g_child_sharp;
        if (!same) {
            LOGW("%s shader lays out its uniforms or children differently; not used",
                 kBuilds[b].name);
            continue;
        }
        g_lean_effect[b] = effect;
        LOGI("%s shader validated (%zu chars, the glass shader's uniform block)",
             kBuilds[b].name, kBuilds[b].chars);
    }

    if (!g_tone_ok) return;
    std::string err;
    SkRuntimeEffect* tone = glass::skia::compile(g_sk, glass::kGlassShaderToneLean, &err);
    if (tone == nullptr) {
        LOGW("lean tone shader did not compile (%s); backdrops keep the tone shader",
             err.c_str());
        return;
    }
    bool same = g_sk.uniformSize(tone) == g_tone_uniform_size;
    for (size_t i = 0; same && i < T_count; ++i) {
        const SkRuntimeEffect::Uniform* u = g_sk.findUniform(tone, kToneUniformNames[i]);
        same = u != nullptr && u->offset == g_tone_uniform_offset[i];
    }
    const SkRuntimeEffect::Child* child = g_sk.findChild(tone, "blurredContent");
    if (!same || child == nullptr || child->index != 0) {
        LOGW("lean tone shader lays out its uniforms or child differently; not used");
        return;
    }
    g_tone_lean_effect = tone;
    LOGI("lean tone shader validated (%zu chars, the tone shader's uniform block)",
         sizeof(glass::kGlassShaderToneLean) - 1);
}

// Compiles the glass SkSL with SurfaceFlinger's own Skia and proves the
// vendored headers agree with it.
//
// The proof is findUniform/findChild: those are out-of-line calls into the
// target, and every name they return has to match a name we declared. If the
// header layout were wrong, or the effect had compiled differently, the lookups
// would come back null. Passing all 34 is strong evidence the ABI is right —
// and it is checked before anything is drawn, so a mismatch costs a log line
// instead of a soft reboot.
void tryCompileShader() {
    g_shader_tried = true;

    if (!g_sk.drawReady()) {
        LOGE("skia bridge incomplete; not compiling");
        return;
    }

    // The generated name list comes from the same file the SkSL does, so this
    // catches a shader that grew a uniform the engine does not know how to fill.
    if (glass::kGlassUniformNames_count != static_cast<int>(U_count)) {
        LOGE("shader declares %d uniforms, engine knows %zu — regenerate UniformId",
             glass::kGlassUniformNames_count, U_count);
        return;
    }

    std::string err;
    g_effect = glass::skia::compile(g_sk, glass::kGlassShaderHigh, &err);
    if (g_effect == nullptr) {
        LOGE("SkSL compile FAILED: %s", err.c_str());
        return;
    }
    LOGI("SkSL compiled by SurfaceFlinger's Skia (%zu chars)",
         sizeof(glass::kGlassShaderHigh) - 1);

    int missing = 0;
    for (size_t i = 0; i < U_count; ++i) {
        const SkRuntimeEffect::Uniform* u = g_sk.findUniform(g_effect, kUniformNames[i]);
        if (u == nullptr) {
            LOGE("uniform NOT FOUND: %s", kUniformNames[i]);
            ++missing;
            continue;
        }
        g_uniform_offset[i] = u->offset;
    }

    for (int i = 0; i < glass::kGlassChildNames_count; ++i) {
        const char* n = glass::kGlassChildNames[i];
        const SkRuntimeEffect::Child* c = g_sk.findChild(g_effect, n);
        if (c == nullptr) { LOGE("child NOT FOUND: %s", n); ++missing; continue; }
        if (strcmp(n, "blurredContent") == 0) g_child_blurred = c->index;
        if (strcmp(n, "sharpContent") == 0) g_child_sharp = c->index;
        LOGI("  child   %-16s index=%d type=%d", n, c->index, static_cast<int>(c->type));
    }

    if (missing != 0) {
        LOGE("%d of %zu uniforms/children unresolved — header/target mismatch, refusing to draw",
             missing, U_count + glass::kGlassChildNames_count);
        g_effect = nullptr;
        return;
    }
    if (g_child_blurred == g_child_sharp) {
        LOGE("both children resolved to index %d", g_child_blurred);
        g_effect = nullptr;
        return;
    }

    g_uniform_size = g_sk.uniformSize(g_effect);
    if (g_uniform_size == 0 || g_uniform_size > (1u << 16)) {
        LOGE("implausible uniform blob size %zu", g_uniform_size);
        g_effect = nullptr;
        return;
    }

    g_shader_ok = true;
    LOGI("shader validated: %zu uniforms + %d children resolved, blob %zu bytes, "
         "density %.3f px/dp, %d properties overridden",
         U_count, glass::kGlassChildNames_count, g_uniform_size, glass::paramsUnitScale(),
         glass::paramsOverriddenCount());
    compileToneShader();
    compileLeanShaders();
}

// ------------------------------------------------------------ uniforms

inline void put(uint8_t* blob, UniformId id, float a) {
    memcpy(blob + g_uniform_offset[id], &a, sizeof(float));
}
inline float peek(const uint8_t* blob, UniformId id) {
    float a;
    memcpy(&a, blob + g_uniform_offset[id], sizeof(float));
    return a;
}

// The build of the glass shader to draw a filled uniform block with: the
// leanest whose missing parts that block switches off anyway. The tests are
// the shader's own (dispersion's spread is a product with dispersionAmount;
// `noiseAmount > 0`; the press dent, boost and glow all need touchGlow; a
// corner is a superellipse when its exponent is over 2.001; `softenRadius <=
// 0` is sampleSoft's single read), so a lean build draws what the full one
// would have. debugPane=3 swaps in the build with no lens, where a pane has
// no softening either: that one is for measuring and draws no glass.
SkRuntimeEffect* effectFor(const uint8_t* blob, int debug_pane) {
    if (glass::param(glass::P_leanShader) == 0.0f) return g_effect;
    if (peek(blob, U_dispersionAmount) != 0.0f || peek(blob, U_noiseAmount) > 0.0f ||
        peek(blob, U_touchGlow) != 0.0f) {
        return g_effect;
    }
    float shapes[4];
    memcpy(shapes, blob + g_uniform_offset[U_cornerShapes], sizeof(shapes));
    for (const float n : shapes) {
        if (n > 2.001f) return g_effect;
    }
    const bool plain = peek(blob, U_softenRadius) <= 0.0f;
    SkRuntimeEffect* lean = g_lean_effect[plain ? kLean : kLeanSoft];
    if (debug_pane == 3 && plain && g_lean_effect[kDeep] != nullptr) lean = g_lean_effect[kDeep];
    if (lean == nullptr) return g_effect;
    ++g_stats.lean;
    return lean;
}

// ...and of the tone shader, whose one lean part is the corner.
SkRuntimeEffect* toneEffect() {
    if (g_tone_lean_effect == nullptr || glass::param(glass::P_leanShader) == 0.0f ||
        glass::param(glass::P_cornerShape) > 2.001f) {
        return g_tone_effect;
    }
    ++g_stats.lean;
    return g_tone_lean_effect;
}
inline void put(uint8_t* blob, UniformId id, float a, float b) {
    const float v[2] = {a, b};
    memcpy(blob + g_uniform_offset[id], v, sizeof(v));
}
inline void put(uint8_t* blob, UniformId id, float a, float b, float c) {
    const float v[3] = {a, b, c};
    memcpy(blob + g_uniform_offset[id], v, sizeof(v));
}
inline void put(uint8_t* blob, UniformId id, float a, float b, float c, float d) {
    const float v[4] = {a, b, c, d};
    memcpy(blob + g_uniform_offset[id], v, sizeof(v));
}

inline void putTone(uint8_t* blob, ToneUniformId id, float a) {
    memcpy(blob + g_tone_uniform_offset[id], &a, sizeof(float));
}
inline void putTone(uint8_t* blob, ToneUniformId id, float a, float b) {
    const float v[2] = {a, b};
    memcpy(blob + g_tone_uniform_offset[id], v, sizeof(v));
}
inline void putTone(uint8_t* blob, ToneUniformId id, float a, float b, float c, float d) {
    const float v[4] = {a, b, c, d};
    memcpy(blob + g_tone_uniform_offset[id], v, sizeof(v));
}

// ------------------------------------------------------- Liquid Material
//
// materialTint colours the glass with the Material You palette. SurfaceFlinger
// cannot see the palette, so glass-theme.sh reads system_accent1_500 with
// `cmd overlay lookup` and hands it over as accentR/G/B. Two things change, by
// the same amount:
//
//  - What the glass transmits is filtered by the accent, scaled so its largest
//    channel is 1: coloured glass, and a filter can only take light away, so
//    nothing overshoots. At full strength a muted Tonal Spot palette takes
//    about a quarter of the light out, in its own hue.
//  - Every neutral colour the glass adds of its own (the veil and frost, the
//    dimming tint, the tone's lift) moves toward the palette at the SAME luma,
//    so the glass keeps its brightness and only changes hue. Where the palette
//    colour at that luma would need a channel above 1 (a light veil), it gets
//    exactly as much of the palette's chroma as fits, which is what the
//    palette's own light tones look like.
//
// The rim light stays white: it is added after all of this.
float lumaOf(const float c[3]) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }

float materialStrength(float accent[3]) {
    using namespace glass;
    accent[0] = std::clamp(param(P_accentR), 0.0f, 1.0f);
    accent[1] = std::clamp(param(P_accentG), 0.0f, 1.0f);
    accent[2] = std::clamp(param(P_accentB), 0.0f, 1.0f);
    // No palette known yet: nothing to tint with.
    if (std::max({accent[0], accent[1], accent[2]}) < 0.02f) return 0.0f;
    return std::clamp(param(P_materialTint), 0.0f, 1.0f);
}

// `c`, moved toward the palette by `m`, at c's own luma.
void materialColour(const float c[3], const float accent[3], float m, float out[3]) {
    out[0] = c[0]; out[1] = c[1]; out[2] = c[2];
    if (m <= 0.0f) return;
    const float l = lumaOf(c);
    const float la = std::max(lumaOf(accent), 1e-3f);
    // The accent's chroma, as an offset from grey that leaves luma unchanged.
    float d[3];
    for (int i = 0; i < 3; ++i) d[i] = accent[i] * (l / la) - l;
    // As much of it as fits in [0, 1] at this luma.
    float fit = 1.0f;
    for (int i = 0; i < 3; ++i) {
        if (d[i] > 1e-6f) fit = std::min(fit, (1.0f - l) / d[i]);
        if (d[i] < -1e-6f) fit = std::min(fit, l / -d[i]);
    }
    fit = std::max(fit, 0.0f);
    for (int i = 0; i < 3; ++i) out[i] = c[i] + ((l + d[i] * fit) - c[i]) * m;
}

// The filter on what the glass transmits.
void materialFilter(const float accent[3], float m, float out[3]) {
    const float top = std::max({accent[0], accent[1], accent[2], 1e-3f});
    for (int i = 0; i < 3; ++i) out[i] = 1.0f + (accent[i] / top - 1.0f) * m;
}

// How much of the compositor's blur the refraction rim keeps, in [0, 1].
//
// A negative lensBlur means "derive it from the radius the app asked for",
// which is the default and the whole point: the rim reads the sharp child, so
// without this it ignores that radius entirely — measured at 9x the fine detail
// of the blurred content it sits in. Scaling with the radius means a light blur
// still gets a crisp refractive lens while a heavy one is honoured rather than
// undone. `blurRadius` is an argument to drawBlurRegion that the engine
// otherwise discards.
float effectiveLensBlur(uint32_t radius) {
    const float explicit_value = glass::param(glass::P_lensBlur);
    if (explicit_value >= 0.0f) return std::clamp(explicit_value, 0.0f, 1.0f);
    const float ref = std::max(glass::param(glass::P_lensBlurRef), 1.0f);
    return std::clamp(static_cast<float>(radius) / ref, 0.0f, 1.0f);
}

// How far into the glass a region is, in [0, 1], from the radius it asked for.
//
// Stock cross-fades the sharp snapshot into the blurred one while the radius is
// below the filter's crossfade radius, so a blur animating in from 0 starts as
// the untouched backdrop and eases in. Without the same ramp the glass would pop
// in at full strength on the first frame of every shade pull and dialog.
float fadeFor(uint32_t radius) {
    const float cross = glass::param(glass::P_crossFadeRadius);
    if (cross <= 0.0f) return 1.0f;
    return std::clamp(static_cast<float>(radius) / cross, 0.0f, 1.0f);
}

// Mirrors harness GlassView.drawPane() line for line, so a value calibrated
// there means the same thing here — except that every term is scaled by `fade`.
//
// At fade 0 the shader reduces to the sharp snapshot at the undisplaced
// position, which is what stock draws at radius 0: no displacement, no
// dispersion, no frost or tint, saturation 1, no grain, no light, and blurMix
// and lensBlur both 0. At fade 1 nothing is scaled.
//
// `role` and `under` are what differ between the three looks:
//
//  - a pane draws exactly as before, except that over a toned backdrop it
//    composes that backdrop's tone under its own. Saturation multiplies (the
//    luma a saturation mix pivots on is unchanged by it, so two in a row are
//    one at the product), and two veils of the same colour combine as
//    1 - (1-a)(1-b), which is what drawing one over the other gives.
//  - a glass backdrop draws as a pane.
//  - a toned backdrop is the same shader with the lens switched off: no
//    refraction, dispersion, light, grain or rim, the blurred child everywhere
//    including the rim band, and only saturation and a frost-coloured veil.
struct Under {
    float saturation = 1.0f;
    float veil = 0.0f;
};

// The outline the shader draws: a rect and its four corner radii, in
// SkRRect's corner order (UL, UR, LR, LL). Usually one region's own round rect;
// for regions merged into one pane (below), the outline of all of them.
struct Shape {
    SkRect rect;
    SkVector radii[4];

    static Shape of(const SkRRect& rr) {
        Shape s{rr.rect(), {}};
        for (int c = 0; c < 4; ++c) s.radii[c] = rr.radii(static_cast<SkRRect::Corner>(c));
        return s;
    }
};

// What the see-through child is, which decides how finely it has to be read.
struct SeeThrough {
    float soften = 0.0f;   // softenDp in px, an Android blur radius; 0 = none
    // The child is a backdrop's quarter-resolution blur, not the snapshot. It
    // has no detail finer than four pixels: softening takes four taps for
    // nine, and the dispersion taps one per dp for one per pixel.
    bool blurred = false;
    // Another layer's backdrop is about to blur this pane (coveredRadius): a
    // colour fringe a few dp wide does not survive that, so it is not drawn.
    bool covered = false;
};

void fillUniforms(uint8_t* blob, const Shape& region, float blur_alpha, uint32_t radius,
                  Role role, const Under& under, const glass::RegionTint& region_tint,
                  const SeeThrough& view) {
    using namespace glass;
    const bool tone = role == Role::kBackdropTone;
    const SkRect r = region.rect;
    const float w = r.width();
    const float h = r.height();
    const float dp = paramsUnitScale();
    const float fade = fadeFor(radius);

    put(blob, U_size, w, h);
    put(blob, U_offset, -r.fLeft, -r.fTop);
    // `crop` clamps the coordinate handed to both children. On the app-side port
    // it stops an out-of-bounds eval returning transparent black and painting a
    // black rim; here both children are built kClamp by us, so it has nothing
    // left to do. A rect that can never clip keeps the shader byte-identical to
    // the calibrated one instead of introducing a second, different clamp.
    put(blob, U_crop, -1.0e6f, -1.0e6f, 1.0e6f, 1.0e6f);

    // Packed (BL, BR, TR, TL) to match the shader's quadrant select. The floor
    // exists because a background-blur region is very often a plain rectangle,
    // and a glass pane with no corner radius reads as a grey box. A toned
    // backdrop is not a pane and keeps the corners it was given.
    const float floor_px = tone ? 0.0f : param(P_cornerRadiusDp) * dp;
    const float cap = std::min(w, h) * 0.5f;
    auto corner = [&](SkRRect::Corner c) {
        return std::min(cap, std::max(region.radii[c].fX, floor_px));
    };
    put(blob, U_cornerExtents, corner(SkRRect::kLowerLeft_Corner),
        corner(SkRRect::kLowerRight_Corner), corner(SkRRect::kUpperRight_Corner),
        corner(SkRRect::kUpperLeft_Corner));
    const float shape = param(P_cornerShape);
    put(blob, U_cornerShapes, shape, shape, shape, shape);
    put(blob, U_unitScale, dp);

    put(blob, U_refractionScale, param(P_refractionWidthDp) * dp,
        param(P_refractionHeightDp) * dp);
    put(blob, U_refractionAmount, tone ? 0.0f : param(P_refractionAmountDp) * dp * fade);
    put(blob, U_refractionSwirl, tone ? 0.0f : param(P_refractionSwirl));

    put(blob, U_depthEffect, tone ? 0.0f : param(P_refractionDepth) * panenessFor(w, h));
    put(blob, U_profilePower, param(P_curvePower));
    put(blob, U_profileBias, param(P_curveBias));

    put(blob, U_dispersionHeight, param(P_dispersionHeightDp) * dp);
    // fringe=0 is a fringe of no width, which the shader takes as one plain
    // read at the refracted place.
    put(blob, U_dispersionAmount,
        tone || view.covered || param(P_fringe) == 0.0f
            ? 0.0f : param(P_dispersionAmountDp) * dp * fade);
    // One tap per dp is the widest spacing that keeps the shader's "spread >= 2
    // taps" invariant (its MEDIUM tier).
    put(blob, U_dispersionTapSpacing,
        view.blurred ? std::max(param(P_dispersionTapSpacingPx), dp)
                     : param(P_dispersionTapSpacingPx));
    put(blob, U_dispersionQuadrant, param(P_dispersionQuadrant));

    put(blob, U_blurMix, (tone ? 1.0f : param(P_blurMix)) * fade);
    put(blob, U_blurFalloff, param(P_blurFalloff));
    put(blob, U_lensBlur, (tone ? 1.0f : effectiveLensBlur(radius)) * fade);
    put(blob, U_rimFrost, tone ? 0.0f : std::clamp(param(P_rimFrost), 0.0f, 1.0f));

    // Liquid Material (above): the same strength for every colour, so a pane
    // over a toned backdrop composes the backdrop's veil in the colour the
    // backdrop actually drew it.
    float accent[3];
    const float material = materialStrength(accent);
    const float tint_in[3] = {param(P_tintR), param(P_tintG), param(P_tintB)};
    float tint[3];
    materialColour(tint_in, accent, material, tint);
    if (region_tint.on && !tone) {
        // A tint the region asked for itself (region_tint.h): laid over the
        // glass in its own colour, in place of the preset's dimming tint.
        put(blob, U_tintColor, region_tint.rgb[0], region_tint.rgb[1], region_tint.rgb[2],
            std::clamp(region_tint.strength, 0.0f, 1.0f) * fade);
    } else {
        put(blob, U_tintColor, tint[0], tint[1], tint[2], tone ? 0.0f : param(P_tintA) * fade);
    }
    float frost = tone ? param(P_backdropVeil) * fade : param(P_frost) * fade;
    float saturation = 1.0f + ((tone ? param(P_backdropSaturation) : param(P_saturation)) - 1.0f) * fade;
    if (!tone) {
        frost = 1.0f - (1.0f - frost) * (1.0f - std::clamp(under.veil, 0.0f, 1.0f));
        saturation *= std::max(under.saturation, 0.0f);
    }
    const float frost_in[3] = {param(P_frostR), param(P_frostG), param(P_frostB)};
    float frost_rgb[3];
    materialColour(frost_in, accent, material, frost_rgb);
    put(blob, U_frostColor, frost_rgb[0], frost_rgb[1], frost_rgb[2],
        std::clamp(frost, 0.0f, 1.0f));
    put(blob, U_saturation, saturation);

    // Regular glass: the blur fill and the tone matrix. A toned backdrop keeps
    // the identity; it is Android's blur with a veil, not a pane. The two fill
    // weights share one blend, so together they cannot pass 1.
    float lighten = tone ? 0.0f : std::clamp(param(P_fillLighten), 0.0f, 1.0f);
    float darken = tone ? 0.0f : std::clamp(param(P_fillDarken), 0.0f, 1.0f);
    if (lighten + darken > 1.0f) {
        const float k = 1.0f / (lighten + darken);
        lighten *= k;
        darken *= k;
    }
    put(blob, U_fillLighten, lighten * fade);
    put(blob, U_fillDarken, darken * fade);
    put(blob, U_lumaLimit, tone ? 0.0f : std::max(0.0f, param(P_lumaLimit)) * fade);
    put(blob, U_toneScale, tone ? 1.0f : 1.0f + (param(P_toneScale) - 1.0f) * fade);
    const float bias = tone ? 0.0f : param(P_toneBias) * fade;
    const float bias_in[3] = {bias, bias, bias};
    float bias_rgb[3];
    materialColour(bias_in, accent, material, bias_rgb);
    put(blob, U_toneBias, bias_rgb[0], bias_rgb[1], bias_rgb[2]);
    float filter[3];
    materialFilter(accent, tone ? 0.0f : material * fade, filter);
    if (region_tint.on && !tone) {
        // ...and what it transmits is filtered by that colour as well.
        float own[3];
        float rgb[3] = {region_tint.rgb[0], region_tint.rgb[1], region_tint.rgb[2]};
        materialFilter(rgb, std::clamp(region_tint.strength, 0.0f, 1.0f) * fade, own);
        for (int i = 0; i < 3; ++i) filter[i] *= own[i];
    }
    put(blob, U_filterColor, filter[0], filter[1], filter[2]);
    put(blob, U_lightIntensity, tone ? 0.0f : param(P_light) * fade);
    put(blob, U_noiseAmount, tone ? 0.0f : param(P_noise) * fade);
    // blurAlpha is what stock folds into the paint's alpha. Folding it into the
    // shader instead keeps the paint at its default blend mode, which matters:
    // stock can use kSrc because its shader fills the whole rect opaquely,
    // while ours leaves the corners transparent and kSrc would punch them
    // through to black.
    put(blob, U_glassOpacity, param(P_glassOpacity) * std::clamp(blur_alpha, 0.0f, 1.0f));

    const float angle = param(P_highlightAngleDeg) * 3.14159265358979323846f / 180.0f;
    put(blob, U_highlightIntensity, tone ? 0.0f : param(P_highlightIntensity) * fade);
    put(blob, U_highlightDir, std::cos(angle), std::sin(angle));
    put(blob, U_highlightWidth, param(P_highlightWidthDp) * dp);
    put(blob, U_highlightFalloff, std::max(0.01f, param(P_highlightFalloff)));

    put(blob, U_hdrHeadroom, std::max(1.0f, param(P_hdrHeadroom)));

    put(blob, U_touchPos, w * 0.5f, h * 0.5f);
    put(blob, U_touchGlow, tone ? 0.0f : param(P_touchGlow));
    put(blob, U_touchLens, tone ? 0.0f : param(P_touchLens));
    put(blob, U_softenRadius, tone ? 0.0f : view.soften);
    // 1: four taps of a child that is a blur already. 2: five of the sharp
    // screen (softenTaps=5). 0: the nine.
    put(blob, U_softenFast,
        view.blurred ? 1.0f : param(P_softenTaps) < 7.0f ? 2.0f : 0.0f);
    // How far in from the edge the edge still does anything (the shader's
    // `deep`): the refraction band, whichever axis is wider, the rim light and
    // the 7 dp sheen. Half a pixel over, so that rounding in the shader's own
    // band cannot put a fragment on the short path a hair early.
    put(blob, U_deepBand,
        std::max({param(P_refractionWidthDp) * dp, param(P_refractionHeightDp) * dp,
                  param(P_highlightWidthDp) * dp, 7.0f * dp, 1.0f}) + 0.5f);
}

// The tone shader's uniforms: the ones fillUniforms gives a toned backdrop,
// value for value, less everything that is zero there. Only past the fade-in
// (the caller checks), so nothing is scaled by it.
void fillToneUniforms(uint8_t* blob, const Shape& region, float blur_alpha) {
    using namespace glass;
    const SkRect r = region.rect;
    const float w = r.width();
    const float h = r.height();
    putTone(blob, T_size, w, h);
    putTone(blob, T_offset, -r.fLeft, -r.fTop);
    putTone(blob, T_crop, -1.0e6f, -1.0e6f, 1.0e6f, 1.0e6f);
    // A backdrop keeps the corners it was given: no floor.
    const float cap = std::min(w, h) * 0.5f;
    auto corner = [&](SkRRect::Corner c) { return std::min(cap, std::max(region.radii[c].fX, 0.0f)); };
    putTone(blob, T_cornerExtents, corner(SkRRect::kLowerLeft_Corner),
            corner(SkRRect::kLowerRight_Corner), corner(SkRRect::kUpperRight_Corner),
            corner(SkRRect::kUpperLeft_Corner));
    const float shape = param(P_cornerShape);
    putTone(blob, T_cornerShapes, shape, shape, shape, shape);

    float accent[3];
    const float material = materialStrength(accent);
    const float frost_in[3] = {param(P_frostR), param(P_frostG), param(P_frostB)};
    float frost_rgb[3];
    materialColour(frost_in, accent, material, frost_rgb);
    putTone(blob, T_frostColor, frost_rgb[0], frost_rgb[1], frost_rgb[2],
            std::clamp(param(P_backdropVeil), 0.0f, 1.0f));
    putTone(blob, T_saturation, 1.0f + (param(P_backdropSaturation) - 1.0f) * 1.0f);
    putTone(blob, T_glassOpacity, param(P_glassOpacity) * std::clamp(blur_alpha, 0.0f, 1.0f));
    putTone(blob, T_hdrHeadroom, std::max(1.0f, param(P_hdrHeadroom)));
}

// ---------------------------------------------------------- replacement

uint32_t effectiveRadius(uint32_t radius) {
    const auto forced = static_cast<uint32_t>(glass::param(glass::P_radius));
    return forced != 0 ? forced : radius;
}

// Says so once whenever the mode, the preset or the backdrop style changes.
// What the shader is actually being handed is the first thing to check when a
// setting "does nothing", and a capture taken before a property landed looks
// exactly like one.
void reportMode() {
    static bool s_reported = false;
    static glass::Mode s_mode = glass::Mode::kStock;
    static size_t s_preset = ~size_t{0};
    static int s_backdrop = -1;
    const glass::Mode mode = glass::paramsMode();
    const size_t preset = glass::paramsPresetIndex();
    const int backdrop = backdropStyle();
    if (s_reported && mode == s_mode && preset == s_preset && backdrop == s_backdrop) return;
    s_reported = true;
    s_mode = mode;
    s_preset = preset;
    s_backdrop = backdrop;
    if (mode == glass::Mode::kGlass) {
        static const char* const kStyles[] = {"stock", "tone", "glass"};
        LOGI("mode: glass, preset %s, backdrop %s, %d properties overridden",
             glass::presetAt(preset).name, kStyles[backdrop], glass::paramsOverriddenCount());
    } else {
        LOGI("mode: stock — Android's own blur, untouched");
    }
}

// Whether this draw is worth a log line: the first time a region's geometry,
// role or parameters are seen, not every frame. A shade with three cards is
// four draws a frame with four different rects, so "changed since the last
// call" would be true on every call; this remembers the last few instead, and
// a hard rate limit covers a scrolling list where every frame is new geometry.
bool shouldLog(const SkRect& rect, const SkRect& blur_rect, Role role, bool faded, bool zoomed) {
    struct Seen {
        SkRect rect, blur;
        Role role;
        bool faded, zoomed;
    };
    static Seen s_seen[16];
    static size_t s_count = 0, s_next = 0;
    static unsigned s_gen = ~0u;
    static int64_t s_window_ms = 0;
    static int s_in_window = 0;

    const unsigned gen = glass::paramsGeneration();
    if (gen != s_gen) { s_gen = gen; s_count = 0; s_next = 0; }
    for (size_t i = 0; i < s_count; ++i) {
        const Seen& e = s_seen[i];
        if (e.rect == rect && e.blur == blur_rect && e.role == role && e.faded == faded &&
            e.zoomed == zoomed) {
            return false;
        }
    }
    s_seen[s_next] = {rect, blur_rect, role, faded, zoomed};
    s_next = (s_next + 1) % 16;
    if (s_count < 16) ++s_count;

    struct timespec now {};
    clock_gettime(CLOCK_MONOTONIC, &now);
    const int64_t ms = static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
    if (ms - s_window_ms >= 1000) { s_window_ms = ms; s_in_window = 0; }
    return ++s_in_window <= 12;
}

// ------------------------------------------- one pane from overlapping regions
//
// Regions that are pieces of one surface are outlined as one: region_merge.h
// has the rules and why. Here it is only fed: the shape in Skia's terms, and
// the region's slot, its place among the panes drawn from this snapshot (this
// layer, this frame), which stays the same from frame to frame while the
// window's regions do. Every pane takes a slot, merged or not, so that one
// region gaining or losing its tint does not renumber the rest.
glass::RegionMerger g_merger;

int paneSlot(uint32_t input_id) {
    static uint32_t s_input = 0;
    static int s_slot = -1;
    if (input_id != s_input) {
        s_input = input_id;
        s_slot = -1;
    }
    return ++s_slot;
}

int64_t monotonicMs() {
    struct timespec now {};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

glass::MergeShape toMerge(const Shape& s) {
    glass::MergeShape m;
    m.rect = {s.rect.fLeft, s.rect.fTop, s.rect.fRight, s.rect.fBottom};
    for (int c = 0; c < 4; ++c) {
        m.rx[c] = s.radii[c].fX;
        m.ry[c] = s.radii[c].fY;
    }
    return m;
}

// Fills `shape` (what the shader outlines) and `draw` (what this call covers).
// Returns false when a neighbour drawn earlier this frame already covered all
// of this region. `merged` says whether anything was merged.
bool mergeNeighbours(const SkRRect& region, uint32_t input_id, uint32_t radius, int slot,
                     Shape* shape, SkRect* draw, int* merged) {
    glass::MergeShape out;
    glass::MergeRect cover;
    const bool any = g_merger.merge(toMerge(Shape::of(region)), input_id, radius, slot,
                                    monotonicMs(), &out, &cover, merged);
    shape->rect = SkRect::MakeLTRB(out.rect.l, out.rect.t, out.rect.r, out.rect.b);
    for (int c = 0; c < 4; ++c) shape->radii[c] = {out.rx[c], out.ry[c]};
    *draw = SkRect::MakeLTRB(cover.l, cover.t, cover.r, cover.b);
    return any;
}

// ------------------------------------------------- what the frame did last
//
// Which layers the last composite blurred, in order, and which of them drew a
// backdrop over the whole output: composite_memory.h has the rules and why. It
// is what lets a pane under an open shade skip reading the sharp snapshot.
glass::CompositeMemory g_composites;

// The copy this pane may read in the snapshot's place, or null when it reads
// the snapshot. Called for every pane that shows the snapshot, in draw order:
// the first of a layer in a frame decides whether a copy is made, from where
// the layer's panes read in the frames before (sharp_bounds.h).
const SkIRect* sharpCopyFor(SkCanvas* canvas, SkImage* input, const SkRect& blur_rect,
                            const SkMatrix& ctm, const Shape& shape, float zoom) {
    using namespace glass;
    if (param(P_snapshotCopy) < 0.5f || g_sk.ImageMakeSubset == nullptr ||
        g_sk.CanvasBaseRecorder == nullptr) {
        return nullptr;
    }
    // The copy stands in for the snapshot by a translation, and nothing more.
    if (zoom != 1.0f || ctm.get(1) != 0.0f || ctm.get(3) != 0.0f || ctm.get(6) != 0.0f ||
        ctm.get(7) != 0.0f || ctm.get(8) != 1.0f) {
        return nullptr;
    }
    float w = 0.0f, h = 0.0f;
    if (!displaySize(input, blur_rect, &w, &h)) return nullptr;

    // How far outside its own outline the shader reads. The soften taps and
    // the dispersion reach a few pixels either way. The refraction reads up to
    // refractionAmount inward of the edge, which in a pane thinner than that
    // comes out the far side. A negative amount or bias reads outward instead,
    // and a swirl leans whichever way: then the whole of it is margin.
    const float dp = paramsUnitScale();
    const float sigma = 0.57735f * std::max(0.0f, param(P_softenDp)) * dp + 0.5f;
    const float touch = 1.0f + 0.35f * std::max(0.0f, param(P_touchGlow) * param(P_touchLens));
    const float bend = std::fabs(param(P_refractionAmountDp)) * dp *
                       (1.0f + std::fabs(param(P_curveBias))) * touch;
    const bool outward = param(P_refractionAmountDp) < 0.0f || param(P_curveBias) < 0.0f ||
                         param(P_refractionSwirl) != 0.0f;
    float margin = 0.5f * std::fabs(param(P_dispersionAmountDp)) * dp * touch + 2.0f * sigma + 6.0f;
    margin += outward ? bend
                      : std::max(0.0f, bend - std::min(shape.rect.width(), shape.rect.height()));
    const SkRect dev = deviceBounds(
        ctm, SkRect::MakeLTRB(shape.rect.fLeft - margin, shape.rect.fTop - margin,
                              shape.rect.fRight + margin, shape.rect.fBottom + margin));
    const ReadRect reach{dev.fLeft, dev.fTop, dev.fRight, dev.fBottom};

    const uint32_t id = input->uniqueID();
    // The context in use is the one the layer's blur was asked for in.
    const bool known = g_last_generate.valid && g_last_generate.input_id == id;
    ReadRect copy;
    const bool first = g_sharp_bounds.note(
        {blur_rect.fLeft, blur_rect.fTop, blur_rect.fRight, blur_rect.fBottom}, w, h, id, reach,
        monotonicNs(), &copy);
    if (first && known &&
        (g_sharp.image == nullptr || g_sharp.context == g_last_generate.context)) {
        SkSafeUnref(g_sharp.image);
        g_sharp = SharpCopy{};
        g_sharp.input_id = id;
        g_sharp.context = g_last_generate.context;
        if (!copy.empty() && g_output_drawn != id) {
            const SkIRect bounds = SkIRect::MakeLTRB(
                static_cast<int32_t>(std::floor(copy.l)), static_cast<int32_t>(std::floor(copy.t)),
                static_cast<int32_t>(std::ceil(copy.r)), static_cast<int32_t>(std::ceil(copy.b)));
            void* recorder = g_sk.CanvasBaseRecorder(canvas);
            if (recorder != nullptr && !bounds.isEmpty()) {
                g_sharp.image = static_cast<SkImage*>(
                    g_sk.ImageMakeSubset(input, recorder, &bounds, {}).pointer());
                if (g_sharp.image != nullptr) {
                    g_sharp.bounds = bounds;
                    ++g_stats.bounded;
                }
            }
        }
    }
    if (g_sharp.image == nullptr || g_sharp.input_id != id) return nullptr;
    const ReadRect have{static_cast<float>(g_sharp.bounds.fLeft),
                        static_cast<float>(g_sharp.bounds.fTop),
                        static_cast<float>(g_sharp.bounds.fRight),
                        static_cast<float>(g_sharp.bounds.fBottom)};
    return have.contains(reach.within(w, h)) ? &g_sharp.bounds : nullptr;
}

// Whether `rect` spans the whole output, to the pixel: what a backdrop has to
// do for the layers under it to count as covered.
bool coversWhole(const SkMatrix& ctm, const SkRect& rect, float w, float h) {
    const SkRect d = deviceBounds(ctm, rect);
    return d.fLeft <= 0.5f && d.fTop <= 0.5f && d.fRight >= w - 0.5f && d.fBottom >= h - 0.5f;
}

// Draws the region. Returns false when it declined — stock mode, a backdrop
// left to Android, or anything unverified — in which case the caller, which is
// the only place that knows this build's exact signature, hands the call back
// to the original.
bool drawGlassCore(SkCanvas* canvas, const SkRRect* region, uint32_t requested_radius,
                   float zoom, float alpha, const SkRect* blur_rect, void* blurred_sp,
                   void* input_sp) {
    const uint64_t call = g_calls++;

    // Mode first, before anything else of ours runs. In stock mode the engine
    // must not even compile the shader: someone who switched to stock because
    // the glass misbehaved on their ROM should get Android's blur and nothing
    // of ours in the path, apart from this trampoline.
    glass::paramsRefresh();
    reportMode();
    if (glass::paramsMode() != glass::Mode::kGlass || glass::crashGuardHolding()) {
        glass::crashGuardDisarm();
        return false;
    }
    // From here on our code draws, so a crash from here on is ours: say so in a
    // property that outlives this process (crash_guard.h).
    glass::crashGuardArm();

    if (canvas == nullptr || region == nullptr || blur_rect == nullptr ||
        blurred_sp == nullptr || input_sp == nullptr) {
        return false;
    }
    // A zoom of zero or less would collapse or mirror the content; stock never
    // sends one, so if it ever appears it is not a shape to guess at.
    if (!(zoom > 0.0f) || !std::isfinite(zoom)) return false;

    // P_radius forces the radius the glass's own maths sees (lensBlur, the fade).
    // The blur itself is made at the real one.
    const uint32_t radius = effectiveRadius(requested_radius);

    auto* blurred = *static_cast<SkImage**>(blurred_sp);
    auto* input = *static_cast<SkImage**>(input_sp);
    if (blurred == nullptr || input == nullptr) return false;
    // generateHook put this region's blur off until now: what arrived as the
    // blurred image is the snapshot itself, and nothing has been made yet.
    const bool deferred = blurred == input;
    statsFrame(input->uniqueID());
    const float out_w = static_cast<float>(input->width());
    const float out_h = static_cast<float>(input->height());
    const int pass = g_composites.begin(
        canvas, input->uniqueID(), out_w, out_h,
        {blur_rect->fLeft, blur_rect->fTop, blur_rect->fRight, blur_rect->fBottom},
        monotonicNs());

    const SkRect rect = region->rect();
    const SkMatrix ctm = glass::skia::canvasTotalMatrix(g_sk, canvas);
    const float fade = fadeFor(radius);

    // What this region is, and so what it gets.
    Role role = Role::kPane;
    if (coversDisplay(ctm, rect, *blur_rect, input)) {
        const int style = backdropStyle();
        if (style == 0) {
            // Android draws it, with the blur it asked for (drawWithAndroidsBlur
            // makes that if it was put off; the same image, by the cache).
            // Remembered all the same, so that panes over it see the blur it
            // was drawn with rather than the raw snapshot.
            sk_sp<SkImage> own = deferred ? blurOfSnapshot(input, *blur_rect, requested_radius)
                                          : sk_ref_sp(blurred);
            recordBackdrop(input, *blur_rect, zoom, own.get(), requested_radius, fade >= 1.0f,
                           1.0f, 0.0f);
            if (fade >= 1.0f && alpha >= 0.999f && own != nullptr &&
                coversWhole(ctm, rect, out_w, out_h)) {
                g_composites.cover(pass, requested_radius);
            }
            return false;
        }
        role = style == 1 ? Role::kBackdropTone : Role::kBackdropGlass;
    }

    // Compiled the first time glass is actually wanted. That costs a one-off
    // ~300 ms SkSL compile on the frame glass is first switched on, which is a
    // better trade than compiling for a user who never turns it on.
    if (!g_shader_tried) {
        tryCompileShader();
        engineStatus(g_shader_ok ? "glass" : "no-glass");
    }
    if (!g_shader_ok) return false;

    // persist.sys.glass.debugPane, for measuring: 2 draws no pane at all (1 and
    // 3, further down, draw it with a lesser shader).
    const int debug_pane =
        role == Role::kPane ? static_cast<int>(glass::param(glass::P_debugPane)) : 0;
    if (debug_pane == 2) {
        ++g_stats.skipped;
        return true;
    }

    // A pane over a backdrop in the same layer looks through the backdrop.
    const BackdropRecord* under_rec =
        role == Role::kPane ? backdropUnder(input, *blur_rect) : nullptr;
    Under under;
    if (under_rec != nullptr) {
        under.saturation = under_rec->saturation;
        under.veil = under_rec->veil;
    }

    // Its place among the panes drawn from this snapshot (region_merge.h).
    const int slot = role == Role::kPane ? paneSlot(input->uniqueID()) : -1;

    // A tint the region carries in its corner radii (region_tint.h). Read from
    // the region's own radii, before any merge replaces them.
    glass::RegionTint region_tint;
    if (role == Role::kPane) {
        float rx[4], ry[4];
        const SkRRect::Corner order[4] = {SkRRect::kUpperLeft_Corner, SkRRect::kUpperRight_Corner,
                                          SkRRect::kLowerRight_Corner, SkRRect::kLowerLeft_Corner};
        for (int i = 0; i < 4; ++i) {
            rx[i] = region->radii(order[i]).fX;
            ry[i] = region->radii(order[i]).fY;
        }
        region_tint = glass::decodeRegionTint(rx, ry);
    }

    // Panes that are pieces of one surface are outlined as one (see above). A
    // tinted region is its own control, never a piece of its neighbour.
    Shape shape = Shape::of(*region);
    SkRect draw_rect = rect;
    int merged = 0;
    if (role == Role::kPane && !region_tint.on &&
        !mergeNeighbours(*region, input->uniqueID(), radius, slot, &shape, &draw_rect,
                         &merged)) {
        ++g_stats.skipped;
        return true;  // a neighbour drawn earlier this frame covered all of it
    }

    // A pane that lands wholly outside the layer's visible bounds draws
    // nothing: a Quick Settings page not swiped to yet, the launcher's pages
    // either side of this one. It has its slot and its place in the merger's
    // memory already, and costs nothing more: no blur, no shader. blurRect is
    // the layer's bounds on the output, clipped to it, and RenderEngine clips
    // the canvas to the same bounds.
    if (role == Role::kPane) {
        const SkRect dev = deviceBounds(ctm, draw_rect);
        if (!(dev.fRight > blur_rect->fLeft && dev.fLeft < blur_rect->fRight &&
              dev.fBottom > blur_rect->fTop && dev.fTop < blur_rect->fBottom)) {
            ++g_stats.skipped;
            return true;
        }
    }

    // ---- the blur this region is drawn with ("blur made to order", above)
    //
    // The radius may be another than the one it asked for: a toned backdrop
    // asks for backdropBlur of it, and every region for fillBlurScale of it
    // (the Basic mode's Blur Intensity).
    const float wanted = blurScaleFor(role);
    uint32_t wanted_radius = requested_radius;
    if (wanted >= 0.0f && std::fabs(wanted - 1.0f) > 0.01f && requested_radius > 1) {
        wanted_radius = std::clamp<uint32_t>(
            static_cast<uint32_t>(std::lround(requested_radius * wanted)), 1,
            std::min<uint32_t>(requested_radius * 2, kMaxRegenRadius));
    }

    // A pane with no backdrop of its own reads a blur of the snapshot where it
    // would have read the snapshot, as if a backdrop at that radius lay under
    // it, in two cases: a later layer's backdrop is about to blur it anyway
    // (coveredRadius), or the user has chosen it (chosenViewRadius: one blur
    // for the whole pane, or a quarter-resolution view). The heavier of the
    // two radii, when both hold. A choice waits for the pane's blur to have
    // faded in: until then the glass shader cross-fades the sharp screen in,
    // as stock does, and a blur that came on at once would pop.
    BackdropRecord soft_rec;
    sk_sp<SkImage> soft_image;
    bool soft_by_choice = false;
    bool soft_single = false;
    if (role == Role::kPane && under_rec == nullptr && deferred) {
        const uint32_t covered = glass::param(glass::P_coveredPanes) != 0.0f
                                     ? g_composites.coveredRadius(pass) : 0;
        const uint32_t chosen =
            fade >= 1.0f ? chosenViewRadius(wanted_radius, &soft_single) : 0;
        soft_by_choice = chosen != 0;
        const uint32_t soft_radius = std::max(covered, chosen);
        if (soft_radius != 0) {
            soft_image = blurOfSnapshot(input, *blur_rect, soft_radius);
            if (soft_image != nullptr) {
                soft_rec = {input->uniqueID(), *blur_rect, zoom, soft_image.get(), soft_radius,
                            true, 1.0f, 0.0f};
                under_rec = &soft_rec;
                ++(soft_single ? g_stats.single : covered != 0 ? g_stats.covered
                                                              : g_stats.quarter);
            } else {
                soft_by_choice = soft_single = false;
            }
        }
    }

    enum class Source { kAndroid, kSnapshot, kDerived, kBackdrop };
    Source source = Source::kAndroid;
    sk_sp<SkImage> made;                       // a blur made here, kept for the draw
    SkImage* blurred_used = nullptr;
    float blurred_texel = 4.0f;                // output pixels per pixel of it
    float blurred_zoom = zoom;
    uint32_t drawn_radius = requested_radius;  // the radius it is blurred at

    // A pane over a backdrop sees nothing the backdrop has not blurred first,
    // so its own blur can be made from the backdrop's: the same picture as a
    // blur of the snapshot at the pane's radius (radii add in quadrature),
    // from an image a sixteenth the size, with no second pass over the screen.
    // And nothing reads the snapshot after the backdrop was drawn into the
    // output, which is what made Skia copy the framebuffer (statsSnapshotRead).
    // Only for a blur that was put off: one Android already made is used.
    //
    // A pane that reads a blur of the snapshot in the snapshot's place
    // (soft_rec) has no other blur of the snapshot made for it: its own is
    // derived from that one, whatever paneBlur says, so that its frost does
    // not change as a backdrop above it comes and goes. Unless that one blur
    // is all it was to read (paneBlur=through).
    int pane_blur = 0;
    if (under_rec == &soft_rec) {
        pane_blur = soft_single ? 2 : 1;
    } else if (deferred && under_rec != nullptr) {
        pane_blur = paneBlurMode();
    }
    if (pane_blur != 0) {
        source = Source::kBackdrop;
        blurred_used = under_rec->image;
        blurred_zoom = under_rec->zoom;
        drawn_radius = under_rec->radius;
        if (pane_blur == 1 && wanted_radius > under_rec->radius) {
            const float w2 = static_cast<float>(wanted_radius) * static_cast<float>(wanted_radius);
            const float b2 = static_cast<float>(under_rec->radius) *
                             static_cast<float>(under_rec->radius);
            // In the backdrop image's own pixels, four to an output pixel.
            const float extra = std::sqrt(w2 - b2) * 0.25f;
            if (extra >= 1.5f) {
                // The part of the image the blur fills: all of it, except with
                // the filter's preallocated surfaces, which are the display's
                // size whatever the layer's.
                const float bw = std::min(static_cast<float>(under_rec->image->width()),
                                          std::ceil(blur_rect->width() * 0.25f));
                const float bh = std::min(static_cast<float>(under_rec->image->height()),
                                          std::ceil(blur_rect->height() * 0.25f));
                made = blurCached(input, *blur_rect, under_rec->image, SkRect::MakeWH(bw, bh),
                                  std::clamp<uint32_t>(static_cast<uint32_t>(std::lround(extra)),
                                                       1, kMaxRegenRadius));
                if (made != nullptr) {
                    source = Source::kDerived;
                    blurred_used = made.get();
                    blurred_texel = 16.0f;
                    drawn_radius = wanted_radius;
                }
            }
        }
    } else {
        if (deferred || wanted_radius != requested_radius) {
            made = blurOfSnapshot(input, *blur_rect, wanted_radius);
        }
        if (made != nullptr) {
            source = Source::kSnapshot;
            blurred_used = made.get();
            drawn_radius = wanted_radius;
        } else if (!deferred) {
            blurred_used = blurred;  // Android's own, at the radius it asked for
        } else {
            // Put off, and now it cannot be made: the generate call this
            // region's snapshot came with is not the last one seen. The caller
            // tries once more for Android's blur and otherwise draws nothing.
            static bool s_warned = false;
            if (!s_warned) {
                s_warned = true;
                LOGW("a blur that was put off could not be made (generate call did not match "
                     "this layer); persist.sys.glass.lazyBlur=0 keeps Android's own");
            }
            return false;
        }
    }

    // softenDp blurs the see-through view a little. Over a backdrop that view
    // is the backdrop's blur, which the shader reads more coarsely (SeeThrough),
    // and once the backdrop is blurred at three times the softening or more it
    // adds under 6% to the blur and is left out.
    SeeThrough view;
    view.soften = std::max(0.0f, glass::param(glass::P_softenDp)) * glass::paramsUnitScale();
    if (pane_blur != 0) {
        view.blurred = true;
        const float under_radius = static_cast<float>(under_rec->radius);
        if (under_rec == &soft_rec && soft_by_choice) {
            // The blur stands in for the sharp screen softened by softenDp, so
            // only what it lacks of that is added (radii add in quadrature):
            // nothing, at the defaults.
            view.soften = view.soften > under_radius
                              ? std::sqrt(view.soften * view.soften - under_radius * under_radius)
                              : 0.0f;
        } else if (under_rec == &soft_rec || under_radius >= 3.0f * view.soften) {
            // ...and so is softening it under another layer's backdrop, which
            // blurs it again by more than this would (coveredRadius).
            view.soften = 0.0f;
        }
        view.covered = under_rec == &soft_rec && !soft_by_choice;
    }

    const bool faded = fade < 1.0f;
    const bool zoomed = zoom != 1.0f;
    if (call < 2 || shouldLog(rect, *blur_rect, role, faded, zoomed)) {
        const SkRect dev = deviceBounds(ctm, rect);
        const float paneness = panenessFor(rect.width(), rect.height());
        char note[96] = "";
        int used = 0;
        if (source == Source::kDerived) {
            used = snprintf(note, sizeof(note), " (blurred at %u, from the backdrop's)",
                            drawn_radius);
        } else if (source == Source::kBackdrop) {
            used = snprintf(note, sizeof(note), " (the backdrop's blur, %u)", drawn_radius);
        } else if (source == Source::kSnapshot && drawn_radius != requested_radius) {
            used = snprintf(note, sizeof(note), " (blurred at %u)", drawn_radius);
        }
        if (region_tint.on && used >= 0 && used < static_cast<int>(sizeof(note))) {
            snprintf(note + used, sizeof(note) - used, " tint=#%02x%02x%02x@%.2f",
                     static_cast<int>(std::lround(region_tint.rgb[0] * 255)),
                     static_cast<int>(std::lround(region_tint.rgb[1] * 255)),
                     static_cast<int>(std::lround(region_tint.rgb[2] * 255)),
                     region_tint.strength);
        }
        LOGI("draw #%llu %s r=%u%s a=%.2f zoom=%.3f fade=%.2f device=[%.0f %.0f %.0f %.0f] "
             "of %dx%d region=[%.0f %.0f %.0f %.0f] blurRect=[%.0f %.0f %.0f %.0f] radii=%.1f%s "
             "| preset=%s variant=%.2f frost=%.2f sat=%.2f mix=%.2f fill=%.2f/%.2f tone=%.2f+%.2f "
             "material=%.2f refr=%.0f disp=%.0f hl=%.2f paneness=%.3f lensBlur=%.2f",
             static_cast<unsigned long long>(call), roleName(role), radius,
             note, alpha, zoom, fade,
             dev.fLeft, dev.fTop, dev.fRight, dev.fBottom, input->width(), input->height(),
             rect.fLeft, rect.fTop, rect.fRight, rect.fBottom, blur_rect->fLeft,
             blur_rect->fTop, blur_rect->fRight, blur_rect->fBottom,
             region->radii(SkRRect::kUpperLeft_Corner).fX,
             under_rec == &soft_rec
                 ? (soft_single ? (merged != 0 ? " merged, one blur" : " one blur")
                                : (merged != 0 ? " merged, through a light blur"
                                               : " through a light blur"))
                 : merged != 0 ? (under_rec != nullptr ? " merged, over backdrop" : " merged")
                               : (under_rec != nullptr ? " over backdrop" : ""),
             glass::presetAt(glass::paramsPresetIndex()).name, glass::param(glass::P_variant),
             glass::param(glass::P_frost), glass::param(glass::P_saturation),
             glass::param(glass::P_blurMix), glass::param(glass::P_fillLighten),
             glass::param(glass::P_fillDarken), glass::param(glass::P_toneScale),
             glass::param(glass::P_toneBias), glass::param(glass::P_materialTint),
             glass::param(glass::P_refractionAmountDp),
             glass::param(glass::P_dispersionAmountDp), glass::param(glass::P_highlightIntensity),
             paneness, effectiveLensBlur(radius));
        if (source == Source::kAndroid && wanted_radius != requested_radius) {
            LOGW("no blur at %.2fx for this region (%s); using the requested radius", wanted,
                 g_generate[0] || g_generate[1] || g_generate[2]
                     ? "generate call did not match this layer"
                     : "generateTemporaryImage not hooked on this build");
        }
    }

    const SkSamplingOptions linear(SkFilterMode::kLinear, SkMipmapMode::kNone);
    const SkMatrix blur_matrix =
        glass::skia::blurredTransform(g_sk, canvas, *blur_rect, blurred_zoom, blurred_texel);

    // Each sk_sp comes back as raw bytes and is adopted here, taking over the
    // one reference the callee constructed.
    sk_sp<SkShader> blurred_shader(static_cast<SkShader*>(
        g_sk.ImageMakeShader(blurred_used, SkTileMode::kClamp, SkTileMode::kClamp, &linear,
                             &blur_matrix).pointer()));
    if (blurred_shader == nullptr) {
        LOGE("child shader construction failed");
        return false;
    }

    // Past its fade-in a toned backdrop is its blur, a saturation and a veil,
    // which the tone shader draws from the one child. While the blur is still
    // ramping up the glass shader cross-fades the sharp snapshot in, as stock
    // does.
    // ...and debugPane=1 draws every pane with it, to cost the glass shader.
    const bool tone_only = (role == Role::kBackdropTone && !faded && g_tone_ok &&
                            glass::param(glass::P_toneShader) != 0.0f) ||
                           (debug_pane == 1 && g_tone_ok);

    sk_sp<SkShader> glass_shader;
    if (tone_only) {
        SkData* uniforms =
            static_cast<SkData*>(g_sk.DataMakeZeroInit(g_tone_uniform_size).pointer());
        if (uniforms == nullptr) return false;
        fillToneUniforms(static_cast<uint8_t*>(uniforms->writable_data()), shape, alpha);
        void* children[1] = {blurred_shader.get()};
        void* uniform_slot = uniforms;
        glass_shader.reset(static_cast<SkShader*>(
            g_sk.EffectMakeShader(toneEffect(), &uniform_slot, children, 1, nullptr).pointer()));
        glass::skia::releaseData(g_sk, &uniform_slot);
    } else {
        sk_sp<SkShader> sharp_shader;
        if (source == Source::kBackdrop) {
            // The backdrop's blur is both reads: the same image, the same matrix.
            sharp_shader = blurred_shader;
        } else if (under_rec != nullptr) {
            // What is behind this pane is the backdrop as it was drawn: its
            // blur, at its zoom, quarter resolution like any blurred child.
            const SkMatrix under_matrix =
                glass::skia::blurredTransform(g_sk, canvas, *blur_rect, under_rec->zoom);
            sharp_shader.reset(static_cast<SkShader*>(
                g_sk.ImageMakeShader(under_rec->image, SkTileMode::kClamp, SkTileMode::kClamp,
                                     &linear, &under_matrix).pointer()));
        } else if (const SkIRect* copy =
                       sharpCopyFor(canvas, input, *blur_rect, ctm, shape, zoom)) {
            // A copy of the part of the snapshot the layer's panes read: the
            // snapshot's own pixels, moved to the copy's corner.
            const SkMatrix copy_matrix =
                glass::skia::blurredTransform(g_sk, canvas, SkRect::Make(*copy), 1.0f, 1.0f);
            sharp_shader.reset(static_cast<SkShader*>(
                g_sk.ImageMakeShader(g_sharp.image, SkTileMode::kClamp, SkTileMode::kClamp,
                                     &linear, &copy_matrix).pointer()));
        } else {
            const SkMatrix sharp_matrix =
                glass::skia::sharpTransform(g_sk, canvas, *blur_rect, zoom);
            sharp_shader.reset(static_cast<SkShader*>(
                g_sk.ImageMakeShader(input, SkTileMode::kClamp, SkTileMode::kClamp, &linear,
                                     &sharp_matrix).pointer()));
            statsSnapshotRead();
        }
        if (sharp_shader == nullptr) {
            LOGE("child shader construction failed");
            return false;
        }

        // SkData is allocated in place and declares a private operator delete,
        // so sk_sp<SkData> cannot be instantiated outside Skia. Raw pointer
        // plus an explicit release it is.
        SkData* uniforms = static_cast<SkData*>(g_sk.DataMakeZeroInit(g_uniform_size).pointer());
        if (uniforms == nullptr) return false;
        fillUniforms(static_cast<uint8_t*>(uniforms->writable_data()), shape, alpha, radius, role,
                     under, region_tint, view);
        SkRuntimeEffect* const effect =
            effectFor(static_cast<const uint8_t*>(uniforms->writable_data()), debug_pane);

        // ChildPtr holds exactly one sk_sp<SkFlattenable>, and SkShader reaches
        // SkFlattenable through single non-virtual inheritance, so an array of
        // raw shader pointers *is* a ChildPtr[2]. makeShader takes its own
        // references; the sk_sp locals above still own ours and release them
        // on the way out.
        void* children[2] = {nullptr, nullptr};
        children[g_child_blurred] = blurred_shader.get();
        children[g_child_sharp] = sharp_shader.get();

        // The uniforms blob is passed by value, so it moves into the shader on
        // the success path and the slot comes back null. Anything else means
        // the call bailed out and the reference is still ours.
        void* uniform_slot = uniforms;
        glass_shader.reset(static_cast<SkShader*>(
            g_sk.EffectMakeShader(effect, &uniform_slot, children, 2, nullptr).pointer()));
        glass::skia::releaseData(g_sk, &uniform_slot);
    }

    if (glass_shader == nullptr) {
        LOGE("SkRuntimeEffect::makeShader returned null");
        return false;
    }

    glass::skia::ScopedPaint paint(g_sk);
    glass::skia::paintSetShader(g_sk, paint.get(), glass_shader.release());
    // drawRect, not drawRRect: the shader owns its shape and its antialiasing
    // through the SDF, and clipping to the round rect would feather the edge
    // twice. The default blend mode is kSrcOver, deliberately — see fillUniforms.
    g_sk.CanvasDrawRect(canvas, &draw_rect, paint.get());
    g_output_drawn = input->uniqueID();

    if (role != Role::kPane) {
        const bool toned = role == Role::kBackdropTone;
        ++(tone_only ? g_stats.tones : g_stats.backdrops);
        recordBackdrop(input, *blur_rect, zoom, blurred_used, drawn_radius, fade >= 1.0f,
                       toned ? glass::param(glass::P_backdropSaturation) : 1.0f,
                       toned ? glass::param(glass::P_backdropVeil) : 0.0f);
        // A toned backdrop shows only blur. A glass one shows the snapshot
        // through itself, so what is under it is still seen sharp.
        if (toned && fade >= 1.0f && alpha >= 0.999f &&
            glass::param(glass::P_glassOpacity) >= 0.999f &&
            coversWhole(ctm, rect, out_w, out_h)) {
            g_composites.cover(pass, drawn_radius);
        }
    } else {
        ++g_stats.panes;
    }
    return true;
}

// Hands a region back to Android with the blur Android asked for. Usually that
// is the one it made itself. If generateHook put it off (the "blurred" image is
// the snapshot), it is made now, at the radius asked for; and if that cannot
// be done the region is left undrawn, since stock would stretch the sharp
// snapshot over it as though it were a quarter-resolution blur.
template <typename Call>
void drawWithAndroidsBlur(void* blurred_sp, void* input_sp, const SkRect* blur_rect,
                          uint32_t radius, Call&& call) {
    ++g_stats.stock;
    SkImage* blurred = blurred_sp != nullptr ? *static_cast<SkImage**>(blurred_sp) : nullptr;
    SkImage* input = input_sp != nullptr ? *static_cast<SkImage**>(input_sp) : nullptr;
    if (input != nullptr) g_output_drawn = input->uniqueID();  // Android is about to draw
    if (blurred == nullptr || blurred != input) {
        call(blurred_sp);
        return;
    }
    sk_sp<SkImage> real =
        blur_rect != nullptr ? blurOfSnapshot(input, *blur_rect, radius) : nullptr;
    if (real == nullptr) {
        static bool s_warned = false;
        if (!s_warned) {
            s_warned = true;
            LOGE("a blur that was put off could not be made for Android to draw; the region "
                 "is left unblurred. persist.sys.glass.lazyBlur=0 keeps Android's own");
        }
        return;
    }
    // An sk_sp by value is the address of one: a slot holding one reference,
    // as the caller's temporary would. Whatever the callee leaves in it is
    // ours to release, as that temporary's destructor would.
    SkImage* slot = real.release();
    call(&slot);
    SkSafeUnref(slot);
}

void drawGlass(void* self, SkCanvas* canvas, const SkRRect* region, uint32_t radius, float alpha,
               const SkRect* blur_rect, void* blurred_sp, void* input_sp) {
    const int64_t t0 = glass::param(glass::P_stats) != 0.0f ? monotonicNs() : 0;
    deferredBlurSeen(blurred_sp, input_sp);
    if (!drawGlassCore(canvas, region, radius, 1.0f, alpha, blur_rect, blurred_sp, input_sp) &&
        g_original != nullptr) {
        drawWithAndroidsBlur(blurred_sp, input_sp, blur_rect, radius, [&](void* blurred) {
            g_original(self, canvas, region, effectiveRadius(radius), alpha, blur_rect, blurred,
                       input_sp);
        });
    }
    if (t0 != 0) g_stats.hook_ns += monotonicNs() - t0;
    statsFlush();
}

void drawGlassScaled(void* self, SkCanvas* canvas, const SkRRect* region, uint32_t radius,
                     float scale, float alpha, const SkRect* blur_rect, void* blurred_sp,
                     void* input_sp) {
    // The extra float is BlurFilter's zoomScale (AOSP names it in
    // android16-qpr2-release and android17-release): stock scales both children
    // about the blur rect by it, and so do blurredTransform/sharpTransform. It
    // used to be handed back to stock whenever it was not 1; it is honoured now,
    // and the draw log reports it whenever it moves off or back to 1.
    const int64_t t0 = glass::param(glass::P_stats) != 0.0f ? monotonicNs() : 0;
    deferredBlurSeen(blurred_sp, input_sp);
    if (!drawGlassCore(canvas, region, radius, scale, alpha, blur_rect, blurred_sp, input_sp) &&
        g_original_scaled != nullptr) {
        drawWithAndroidsBlur(blurred_sp, input_sp, blur_rect, radius, [&](void* blurred) {
            g_original_scaled(self, canvas, region, effectiveRadius(radius), scale, alpha,
                              blur_rect, blurred, input_sp);
        });
    }
    if (t0 != 0) g_stats.hook_ns += monotonicNs() - t0;
    statsFlush();
}

// -------------------------------------------------------------- install

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

    glass::SymbolResolver resolver;
    if (!resolver.loadFromFile(exe, glass::inflateXzAdapter, nullptr)) {
        LOGE("symbol load failed: %s", resolver.error().c_str());
        return false;
    }

    // Resolve every entry point now, including the builder ones the draw does
    // not use, so the log shows exactly what a new target has to work with.
    bool strategy_ok[glass::sf::kStrategyCount];
    for (bool& ok : strategy_ok) ok = true;
    glass::Symbol draw{};
    for (size_t i = 0; i < glass::sf::kEntryPointCount; ++i) {
        const glass::sf::EntryPoint& ep = glass::sf::kEntryPoints[i];
        glass::Symbol hit{};
        for (const char* const* m = ep.manglings; *m != nullptr; ++m) {
            hit = resolver.find(*m);
            if (hit.valid()) break;
        }
        if (!hit.valid()) {
            // kHookTarget and kDataRelease are groups of alternatives; a missing
            // member is expected, so it must not condemn the group.
            if (ep.strategy != glass::sf::kHookTarget &&
                ep.strategy != glass::sf::kDataRelease) {
                strategy_ok[ep.strategy] = false;
            }
            // A prefix hit with no exact hit means the signature drifted into a
            // form we have not vetted — that is an ABI change, not an absence.
            // Drift is only fatal where the draw actually depends on the
            // symbol. On an optional or informational entry we have a fallback
            // or no use at all, and refusing to install would be theatre.
            // A hook-target variant is expected to be absent whenever the other
            // shape is present, and its prefix matches that shape, so it is not
            // drift. The "exactly one shape" test below is the real check.
            if (ep.prefix != nullptr && ep.strategy != glass::sf::kHookTarget &&
                !resolver.findByPrefix(ep.prefix).empty()) {
                const bool fatal = ep.strategy == glass::sf::kCore ||
                                   ep.strategy == glass::sf::kDataRelease;
                LOGE("SIGNATURE DRIFT on %s%s", ep.label,
                     fatal ? " — refusing to install" : " (have a fallback; continuing)");
                if (fatal) return false;
            }
            if (ep.strategy == glass::sf::kCore) LOGE("missing core entry point: %s", ep.label);
            continue;
        }
        g_syms[i] = hit;
    }

    // Exactly one shape of drawBlurRegion, and which one decides the
    // replacement installed.
    const bool plain = g_syms[glass::sf::kDrawBlurRegion].valid();
    const bool scaled = g_syms[glass::sf::kDrawBlurRegionScaled].valid();
    void* replacement = nullptr;
    if (plain && !scaled) {
        draw = g_syms[glass::sf::kDrawBlurRegion];
        replacement = reinterpret_cast<void*>(&drawGlass);
    } else if (scaled && !plain) {
        draw = g_syms[glass::sf::kDrawBlurRegionScaled];
        replacement = reinterpret_cast<void*>(&drawGlassScaled);
    } else if (plain && scaled) {
        LOGE("both drawBlurRegion shapes present; cannot tell which one runs");
        return false;
    } else {
        LOGE("no known drawBlurRegion shape; refusing to install");
        return false;
    }
    LOGI("hook target: %s", plain ? "drawBlurRegion(radius, alpha)"
                                  : "drawBlurRegion(radius, scale, alpha)");

    if (!strategy_ok[glass::sf::kCore] || !draw.valid()) {
        LOGE("core entry points missing; refusing to install");
        return false;
    }
    // kDataRelease is a group of alternatives, so strategy_ok is the wrong test
    // for it: what matters is that the bridge ends up with one of them.
    // kDataRelease is a group of alternatives and kOptional is never required,
    // so strategy_ok is the wrong test for both; what matters is what the bridge
    // ended up with. Report it, because "which path did this target take" is the
    // first question worth asking when a new build misbehaves.
    LOGI("%zu symbols; builder %s; blob release via %s; getTotalMatrix %s; setShader %s",
         resolver.count(), strategy_ok[glass::sf::kBuilder] ? "complete" : "partial (unused)",
         g_syms[glass::sf::kSkSpDataDtor].valid() ? "sk_sp dtor"
             : (g_syms[glass::sf::kDataUnref].valid() ? "SkNVRefCnt::unref" : "NOTHING"),
         g_syms[glass::sf::kCanvasGetTotalMatrix].valid() ? "resolved" : "OUR DEFINITION",
         g_syms[glass::sf::kPaintSetShader].valid() ? "resolved" : "OUR DEFINITION");

    if (draw.size <= 64) {
        LOGE("drawBlurRegion is only %llu bytes — inlined, nothing to hook",
             static_cast<unsigned long long>(draw.size));
        return false;
    }

    bool found = false;
    const uintptr_t bias = glass::moduleLoadBias(exe.c_str(), &found);
    if (!found) {
        LOGE("no load mapping for %s", exe.c_str());
        return false;
    }

    g_bias = bias;

    // A function that returns an object by value is only called if it takes the
    // result address the standard way (x8 on AArch64). The compiler is free to
    // change that for a function it can see every caller of, and on the arm64
    // A17 builds measured it did for MakeForShader(SkString): called the
    // standard way it crashed SurfaceFlinger on the first glass frame. One that
    // fails is treated as absent: an optional one falls back, a core one makes
    // the bridge incomplete and the engine stays out of the way.
    for (size_t i = 0; i < glass::sf::kEntryPointCount; ++i) {
        const glass::sf::EntryPoint& ep = glass::sf::kEntryPoints[i];
        if (!ep.result_in_memory || !g_syms[i].valid()) continue;
        std::string detail;
        const glass::ResultRegister rr = glass::resultRegisterInMemory(
            reinterpret_cast<const void*>(bias + g_syms[i].value), g_syms[i].size, &detail);
        if (rr == glass::ResultRegister::kX8) continue;
        LOGW("%s does not take its result address in x8 (%s: %s); not calling it", ep.label,
             glass::resultRegisterName(rr), detail.c_str());
        g_syms[i] = glass::Symbol{};
    }

    auto addr = [&](size_t i) -> void* {
        return g_syms[i].valid() ? reinterpret_cast<void*>(bias + g_syms[i].value) : nullptr;
    };
#define BIND(field, index) \
    g_sk.field = reinterpret_cast<decltype(g_sk.field)>(addr(glass::sf::index))
    BIND(MakeForShader, kMakeForShader);
    BIND(MakeForShaderOpts, kMakeForShaderOpts);
    BIND(SkStringCtor, kSkStringCtor);
    BIND(SkStringDtor, kSkStringDtor);
    BIND(findUniform, kFindUniform);
    BIND(findChild, kFindChild);
    BIND(uniformSize, kEffectUniformSize);
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
#undef BIND
    if (!g_sk.drawReady()) {
        LOGE("skia bridge incomplete after binding; refusing to install");
        return false;
    }
    LOGI("compiling through %s", g_sk.MakeForShaderOpts != nullptr
                                     ? "MakeForShader(SkString, const Options&)"
                                     : "MakeForShader(SkString)");
    LOGI("a copy of part of the snapshot (snapshotCopy=bounds): %s",
         g_sk.ImageMakeSubset != nullptr && g_sk.CanvasBaseRecorder != nullptr
             ? "available" : "not on this build; panes read the whole snapshot");

    void* target = reinterpret_cast<void*>(bias + draw.value);
    LOGI("engine believes sizeof(SkPaint)=%zu align=%zu (headers pinned to "
         "android16-release; layout-identical to android17-release)",
         sizeof(SkPaint), alignof(SkPaint));
    LOGI("drawBlurRegion link=0x%llx bias=0x%llx runtime=%p (%llu bytes)",
         static_cast<unsigned long long>(draw.value), static_cast<unsigned long long>(bias),
         target, static_cast<unsigned long long>(draw.size));

    // gum needs executable anonymous memory and the right to re-protect a
    // patched text page, and it does not survive being refused either: on
    // alioth the denied allocation became a null dereference in this
    // constructor, before SurfaceFlinger's main(), on every start. Ask first,
    // on throwaway mappings, and stand down if the answer is no.
    std::string why;
    if (!glass::canPatchCode(&why)) {
        LOGE("SELinux will not let surfaceflinger patch code (%s). The module's sepolicy.rule "
             "grants execmem and execmod; it did not apply here. Not hooking: Android's blur is "
             "untouched", why.c_str());
        return false;
    }

    gum_init_embedded();
    g_interceptor = gum_interceptor_obtain();
    if (g_interceptor == nullptr) {
        LOGE("gum_interceptor_obtain failed");
        return false;
    }

    gum_interceptor_begin_transaction(g_interceptor);
    void** original_slot = plain ? reinterpret_cast<void**>(&g_original)
                                 : reinterpret_cast<void**>(&g_original_scaled);
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
    const size_t gen_index[kFilterCount] = {glass::sf::kGenerateKawase,
                                            glass::sf::kGenerateGaussian,
                                            glass::sf::kGenerateKawaseDualV2};
    void* const gen_hook[kFilterCount] = {reinterpret_cast<void*>(&generateHook<0>),
                                          reinterpret_cast<void*>(&generateHook<1>),
                                          reinterpret_cast<void*>(&generateHook<2>)};
    int generate_hooked = 0;
    gum_interceptor_begin_transaction(g_interceptor);
    for (size_t k = 0; k < kFilterCount; ++k) {
        const glass::Symbol& sym = g_syms[gen_index[k]];
        if (!sym.valid()) continue;
        const char* label = glass::sf::kEntryPoints[gen_index[k]].label;
        if (sym.size <= 64) {
            LOGW("%s is only %llu bytes; not hooking it", label,
                 static_cast<unsigned long long>(sym.size));
            continue;
        }
        void* gen_target = reinterpret_cast<void*>(bias + sym.value);
        const GumReplaceReturn gr =
            gum_interceptor_replace(g_interceptor, gen_target, gen_hook[k],
                                    reinterpret_cast<gpointer*>(&g_generate[k]), nullptr);
        if (gr != GUM_REPLACE_OK || g_generate[k] == nullptr) {
            LOGW("could not hook %s (%d); backdropBlur will not use it", label,
                 static_cast<int>(gr));
            if (gr == GUM_REPLACE_OK) gum_interceptor_revert(g_interceptor, gen_target);
            g_generate[k] = nullptr;
            continue;
        }
        ++generate_hooked;
        LOGI("hooked %s (lighter backdrop blur available)", label);
    }
    gum_interceptor_end_transaction(g_interceptor);
    if (generate_hooked == 0) {
        LOGI("no generateTemporaryImage on this build; backdropBlur and fillBlurScale keep "
             "the requested radius");
    }
    // For glassctl and the WebUI: whether Blur Intensity can do anything here.
    __system_property_set("sys.glass.regen", generate_hooked != 0 ? "1" : "0");

    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
    LOGI("hook installed in %lld ms", static_cast<long long>(ms));
    return true;
}

}  // namespace

__attribute__((constructor)) static void glassInit() {
    if (propInt("persist.sys.glass.enabled", 1) == 0) {
        LOGI("disabled by persist.sys.glass.enabled=0");
        return;
    }
    if (!guardShouldRun()) return;

    // Before anything reads the mode: if the last SurfaceFlinger died drawing
    // glass, this one starts stock.
    glass::crashGuardStart();
    glass::paramsInit();

    if (install()) {
        engineStatus("hooked");
        LOGI("hook ready; persist.sys.glass.mode=glass to draw Liquid Glass, anything else "
             "keeps Android's blur");
        if (!g_module_guard) guardDisarmWhenBooted();
    } else {
        engineStatus("not-hooked");
        // Nothing was hooked, so nothing can be blamed on us — disarm
        // immediately rather than tripping the guard on the next boot.
        if (!g_module_guard) unlink(guardPaths().pending.c_str());
    }
}
