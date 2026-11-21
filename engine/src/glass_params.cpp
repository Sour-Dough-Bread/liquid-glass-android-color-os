#include "glass_params.h"

#include <sys/system_properties.h>
#include <time.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace glass {
namespace {

struct Spec {
    const char* name;
    float def;
    // Names the property may hold instead of a number; index = value.
    const char* const* choices = nullptr;
};

constexpr const char* kBackdropChoices[] = {"stock", "tone", "glass", nullptr};
constexpr const char* kPaneBlurChoices[] = {"own", "derive", "through", nullptr};
constexpr const char* kSnapshotCopyChoices[] = {"full", "bounds", nullptr};
constexpr const char* kGlassResolutionChoices[] = {"full", "quarter", nullptr};

// Defaults are the `regular` variant of expo-liquid-glass-view's
// REGULAR_DEFAULTS, with three deliberate divergences:
//
//  - blurMix / blurFalloff have no upstream counterpart at all; they were swept
//    on the A16 AVD (docs/VERIFIED.md).
//
//  - frost and saturation are much lower than upstream, because upstream owns
//    the WHOLE effect: it applies its own blur and its own frost as one design.
//    On the compositor path the blur is already there and was chosen by whoever
//    requested the region, so stacking upstream's dark veil and 1.8x saturation
//    on top double-counts it. Measured on the same interior crop: upstream's
//    values put the middle 26% darker and 23% more saturated than the blur the
//    app actually asked for. These land at -8.8% luma, which still reads as a
//    surface rather than a hole while leaving the app's blur recognisable.
//
//    This is the one place where a number does NOT transfer verbatim from the
//    harness, and the reason is structural rather than a matter of taste.
constexpr Spec kSpecs[] = {
    {"cornerRadiusDp", 0.0f},   // 0 = honour the SkRRect exactly
    {"cornerShape", 2.0f},      // 2 = circular, which is all an SkRRect can mean

    {"refractionWidthDp", 20.0f},
    {"refractionHeightDp", 20.0f},
    {"refractionAmountDp", 60.0f},
    {"refractionDepth", 1.0f},
    {"refractionSwirl", 0.0f},
    // The refraction direction is `normal + refractionDepth * radial`, and
    // `radial` points away from the region's CENTRE. On a pane-sized rectangle
    // that is a mild dome. On a full-screen region it swings the direction
    // 20-45 degrees off the edge normal along the long edges, which drags
    // samples sideways and refracts straight backdrop lines into arcs
    // (docs/VERIFIED.md). This is the span, in dp, above which a region stops
    // being treated as a pane: refractionDepth is scaled down past it, so the
    // long edges fall back to refracting along their own normal. 0 disables the
    // scaling entirely and restores the raw upstream behaviour.
    {"paneSpanDp", 480.0f},
    {"curvePower", 1.0f},
    {"curveBias", 0.0f},

    {"dispersionAmountDp", 6.0f},
    {"dispersionHeightDp", 20.0f},
    {"dispersionTapSpacingPx", 1.0f},  // HIGH tier: one tap per pixel
    {"dispersionQuadrant", 0.0f},

    // A blur on the see-through view itself, as an Android blur radius in dp.
    // 0 = sharp. Only matters where the glass shows the sharp view, which with
    // the calibrated blurMix 1 is just the rim; `clear` is where it shows.
    {"softenDp", 0.0f},
    {"blurMix", 1.0f},
    {"blurFalloff", 2.0f},
    // The lens reads the sharp child, so the refraction band ignores the blur
    // radius the compositor chose for the region — measured at 9x the fine
    // detail of the surrounding blurred content. This folds the blurred child
    // back into the lens at the same displaced position. See docs/VERIFIED.md.
    //
    // NEGATIVE means derive it from that radius instead of pinning it, which is
    // the default. The reasoning: an app asking for radius 80 wants the content
    // behind it hidden, and revealing sharp pixels in a 52 px band around the
    // edge works directly against that. An app asking for radius 12 wants a
    // light veil, and a crisp refractive lens there costs it nothing. So the rim
    // honours the blur in proportion to how much blur was asked for. Any value
    // in [0, 1] overrides and pins it.
    {"lensBlur", -1.0f},
    // The radius at which auto reaches 1.0.
    //
    // MEASURED, not chosen: the compositor's Kawase blur saturates. Sweeping the
    // radius a window actually requests and measuring the backdrop's remaining
    // fine detail gives 0% blur at 4, 62% at 8, 90% at 12 — and then a plateau,
    // within 1.5% all the way to 48. That matches the radius/6 pass-count rule
    // running out of passes.
    //
    // The first value here was 80, the radius a background-blur window asks for,
    // which was 6.7x too high: at radius 22 the backdrop is already fully
    // blurred while the rim was still 72% sharp. That gap is a visible band of
    // legible content around the edge of everything the system blurs, and it
    // covers essentially every radius in real use.
    {"lensBlurRef", 12.0f},
    // Stock drawBlurRegion cross-fades the sharp snapshot into the blurred one
    // while the requested radius is below BlurFilter::mMaxCrossFadeRadius, so a
    // blur animating in from radius 0 starts as the untouched backdrop. The
    // glass follows the same ramp: every term is scaled by radius / this, and at
    // radius 0 the shader reduces to the sharp snapshot, which is exactly what
    // stock draws there. 10 is the BlurFilter default that Kawase and
    // KawaseDualFilterV2 both inherit (AOSP android17-release); Gaussian passes
    // 0, which disables the ramp. getMaxCrossFadeRadius() is inlined away on
    // every A17 build checked, so it is a parameter rather than a read.
    {"crossFadeRadius", 10.0f},
    // iOS's regular glass (the shader's fillLighten): 0 and 0 is the plain mix.
    {"fillLighten", 0.0f},
    {"fillDarken", 0.0f},
    // The blurred read, as a multiple of the radius the app asked for: 1 =
    // Android's own. Anything else regenerates it at that radius, which needs
    // generateTemporaryImage (Android 17); elsewhere it stays at 1.
    {"fillBlurScale", 1.0f},

    {"frost", 0.12f},
    // The shader documents frostColor.rgb as "the system background colour".
    // These are the DARK-theme values, applied regardless of the actual theme —
    // SurfaceFlinger has no view of it. Until something that does know sets
    // these (see docs/ARCHITECTURE.md), light-mode content gets tinted toward
    // dark grey, which is wrong but subtle at frost 0.12.
    {"frostR", 0.08f},
    {"frostG", 0.08f},
    {"frostB", 0.09f},
    {"tintR", 0.0f},
    {"tintG", 0.0f},
    {"tintB", 0.0f},
    {"tintA", 0.0f},
    {"saturation", 1.15f},
    {"lumaLimit", 0.0f},
    {"toneScale", 1.0f},
    {"toneBias", 0.0f},
    {"light", 0.0f},
    {"noise", 0.05f},
    {"glassOpacity", 1.0f},

    {"materialTint", 0.0f},
    {"accentR", 0.0f},
    {"accentG", 0.0f},
    {"accentB", 0.0f},
    {"variant", 0.0f},

    {"highlightIntensity", 0.25f},
    {"highlightAngleDeg", 180.0f},
    {"highlightWidthDp", 0.75f},
    {"highlightFalloff", 1.0f},

    // What a backdrop gets: a region spanning the display, which is how the
    // notification shade and a dialog's blur-behind arrive, as opposed to a
    // pane drawn over it (a notification card, the volume panel, a blurred
    // dialog window). Measured on alioth, crDroid A17: the shade is one
    // full-screen layer with backgroundBlurRadius 77, and every notification
    // card and each piece of the volume panel is its own blur region.
    //   stock  Android's blur, untouched (the default)
    //   tone   Android's blur, desaturated, veiled and optionally lighter
    //   glass  Liquid Glass, as on a pane
    {"backdrop", 0.0f, kBackdropChoices},
    // A region counts as a backdrop when its on-screen rect spans at least this
    // fraction of the display in BOTH directions. 0.9 keeps a full-width
    // bottom sheet or a large dialog a pane.
    {"backdropCoverage", 0.9f},
    // tone only. The fraction of the requested radius the backdrop is actually
    // blurred at: Android 17 asks for 77 on the shade, and 0.35 regenerates it
    // at 27 so the home screen stays readable through it. Needs
    // generateTemporaryImage (Android 17); elsewhere the backdrop keeps the
    // requested radius.
    {"backdropBlur", 1.0f},
    {"backdropSaturation", 1.0f},
    // tone only. A veil in the frost colour, which follows the theme: it
    // darkens the backdrop in dark mode and lightens it in light mode.
    {"backdropVeil", 0.0f},

    {"hdrHeadroom", 1.0f},
    {"density", 0.0f},

    {"touchGlow", 0.0f},
    {"touchLens", 0.0f},

    {"radius", 0.0f},
    {"bypass", 0.0f},

    // Android blurs a layer once for every radius its regions ask for, before
    // drawBlurRegion is called, and glass draws few of them at the radius they
    // asked for: a toned backdrop wants backdropBlur of it, a pane
    // fillBlurScale of it, a pane over a backdrop none of its own. 1 puts each
    // blur off until the region is drawn, so only what is drawn is made
    // (Android 17, where generateTemporaryImage can be hooked). 0 is the
    // behaviour before it: Android's blur first, the engine's on top.
    {"lazyBlur", 1.0f},
    // The blurred read of a pane that lies over a backdrop in its own layer (a
    // Quick Settings tile or a notification card on the shade):
    //   own      a blur of the layer's snapshot at the pane's own radius, as
    //            everywhere else: a second full-screen blur each frame, read
    //            after the backdrop was drawn, which also costs a copy of the
    //            framebuffer
    //   derive   the same radius, made by blurring the backdrop's own
    //            quarter-resolution blur a little further: a sixteenth of the
    //            pixels and no copy (the default)
    //   through  the backdrop's blur itself, nothing more made. Unlike the
    //            other two this changes the look (the pane is less frosted),
    //            and it reaches further: a pane with NO backdrop under it (a
    //            home screen icon, a keyboard, the volume panel) reads one
    //            blur of the snapshot in place of the sharp screen and a blur
    //            of its own: the lightest blur for clear glass, the pane's
    //            own for frosted, between them in between (chosenViewRadius).
    //            One read, one blur, no copy of the framebuffer: "One layer"
    //            on the WebUI's Performance card. Android 17.
    {"paneBlur", 1.0f, kPaneBlurChoices},
    // 1 draws a toned backdrop with a shader of its own (one read, no lens);
    // 0 with the glass shader and its lens off, as before. Same picture.
    {"toneShader", 1.0f},
    // A pane with no backdrop of its own reads the snapshot (glass shows what
    // is behind it), and Skia copies the whole framebuffer for that. When a
    // later layer's backdrop blurs the result anyway (the shade or a dialog's
    // blur-behind over a home screen of glass icons), 1 has such a pane read a
    // light blur of the snapshot instead, at half the radius the backdrop
    // above it was last drawn with: no copy, coarser reads, and nothing the
    // backdrop leaves visible. 0 draws every pane in full, as before.
    {"coveredPanes", 1.0f},
    // A pane that shows the snapshot (home screen icons, a keyboard, the
    // volume panel) makes Skia copy the whole framebuffer, because the output
    // cannot be sampled while it is drawn into.
    //   full    that copy, as always (the default)
    //   bounds  the engine copies only what the layer's panes read, before
    //           anything is drawn, and the panes read the copy: far fewer
    //           pixels for a keyboard or a panel, but a texture allocated
    //           every frame. Android 17. Faster on the AVD's GPU; not yet
    //           measured on a phone, which is why it is not the default.
    {"snapshotCopy", 0.0f, kSnapshotCopyChoices},
    // The glass shader tests its uniforms for what a pane does not use (no
    // fringe, no grain, no press, no softening, no superellipse corner) and a
    // phone's GPU may pay for it all the same. 1 draws such a pane with a
    // build of the shader that has those parts left out of its text, and a
    // toned backdrop with the tone shader less its superellipse corner: the
    // same picture. 0 draws everything with the full shaders, as before.
    {"leanShader", 1.0f},

    // ---- the look traded for speed
    //
    // Unlike the switches above, these change what is drawn, each by a step
    // someone may well prefer to pay in frame time instead. Together with
    // paneBlur=through they are the WebUI's Performance card. Layers and
    // resolution default to the best-looking setting; the fringe does not.
    //
    // The colour fringe at a pane's edge (dispersionAmountDp) is a walk of up
    // to sixteen reads along the refraction, one per pixel, in the outer few
    // dp of every pane; a small icon is nearly all edge. 0 leaves it out: the
    // edge still bends what is behind it, in one read. A switch rather than
    // dispersionAmountDp=0, which a preset would set back.
    //
    // Off unless asked for, since v0.4.20: on a phone it costs 11 ms a frame
    // of the Poco F1's home screen and 1.8 of the F3's, for a detail a few
    // pixels wide that the phones' owner found the rim no worse without.
    // `prism`, which is named for it, turns it on for itself; a value set by
    // hand wins over that either way.
    {"fringe", 0.0f},
    // What a pane with no backdrop under it (a home screen icon, a keyboard,
    // the volume panel) shows through itself.
    //   full     the screen behind it at full resolution, softened by
    //            softenDp. Costs a copy of the framebuffer every frame and
    //            the shader's finest reads (the default)
    //   quarter  the compositor's quarter-resolution buffer, blurred by a few
    //            dp, as a pane on the shade reads the shade's: no copy, coarse
    //            reads, and what is behind the glass is blurrier. The pane's
    //            own blur is made from that buffer. Android 17. Nothing to
    //            add under paneBlur=through, which reads one blur already
    {"glassResolution", 0.0f, kGlassResolutionChoices},
    // How many reads softenDp takes of the sharp screen, per pixel of a pane
    // that shows it: 9 (a 3x3 grid) or 5 (a rotated grid: the centre and one
    // tap in each row and column around it, the same spread). On a phone's
    // GPU the nine are the most expensive thing a clear pane does.
    {"softenTaps", 9.0f},
    // How frosted a pane's rim is against its inside (the shader's rimFrost
    // has the arithmetic). 0: the frost fades out toward the edge over the
    // lens band (blurFalloff), so the rim shows what is behind at 1/(1 -
    // blurMix) the strength the inside does: the look of `regular`, `frosted`,
    // `prism` and `edge`, whose inside is all blur and whose rim is the lens.
    // 1: the rim is mixed like the inside. `basic` and `clear` set it: they
    // show the screen through the whole pane, and a rim clearer than the pane
    // reads as a defect there (reported from the phone, 2026-11-19).
    {"rimFrost", 0.0f},
    // For measuring only (harness/phone_ab.sh): where a pane's time goes.
    //   1  every pane is drawn with the tone shader: its shape, one read of
    //      its blur, a saturation and a veil. The same number of draws with
    //      a fifth of the uniforms and next to no arithmetic, so what is
    //      left of the frame against 0 is what the glass shader itself costs
    //      per pixel, and what is left against 2 what a draw costs
    //   2  no pane is drawn and no blur made for it
    //   3  a pane with no softening is drawn by the glass shader's lean
    //      build with the lens left out of it: the blur, the see-through
    //      read, the tone chain, no bend and no rim. Against the lean build
    //      itself that is what the lens costs once it is really gone; against
    //      1, what the rest of the glass shader costs. Needs leanShader
    {"debugPane", 0.0f},
    // Evolution X 12.x draws a blur region a client tagged with one of its
    // materials (1 to 11: Quick Settings tiles and panel, notification cards
    // and heads-ups, the dock) as its own glass, and so does crDroid 13.x's
    // SurfaceFlinger, though crDroid's SystemUI never tags one. 0: in glass
    // mode StockGlass draws them as it draws every other region. 1: the ROM
    // draws them its own way in glass mode too. Stock mode leaves every
    // region to the ROM either way.
    {"romMaterials", 0.0f},
    {"stats", 0.0f},
};
// One entry per ParamId, in the same order: a missing one would shift every
// name after it onto the wrong parameter.
static_assert(sizeof(kSpecs) / sizeof(kSpecs[0]) == P_count, "kSpecs out of step with ParamId");

// ---------------------------------------------------------------- presets
//
// A preset only lists what it changes; everything else is the default above.
// Per-parameter overrides sit on top of whichever preset is selected, so
// "frosted, but with less refraction" is one preset plus one property.
//
// The frost COLOUR is deliberately absent from every preset: it follows the
// system theme (glass-theme.sh), and a preset that pinned it would fight that.

// iOS 26's Clear variant. The reference was a GNOME Shell Liquid Glass
// extension set to its clear style: a blur of radius 2 at half resolution (all
// but sharp), a #211921 tint at 0.31, a strong displaced edge and a 0.42 rim
// with a directional power of 1.9. Translated to this path:
//
//  - softenDp 2 takes the edge off that sharp view, as iOS's clear glass and
//    the desktop reference's radius-2 blur both do: still see-through, no
//    longer pin-sharp.
//  - blurMix 0.3 keeps the interior mostly the sharp view of what is behind,
//    and lensBlur is pinned to the same 0.3, with rimFrost 1, so the rim is
//    no sharper than the interior (auto would blur it fully at any radius
//    over 12; lensBlur without rimFrost left the rim 1.4 times as clear).
//  - The tint does the legibility work, as Apple's dimming layer does. It is a
//    31% blend toward a near-black, so on its own it takes 31% of the colour
//    out of everything seen through the pane; saturation 1.3 puts most of it
//    back (0.69 x 1.3 = 0.9 of the original chroma), as Apple's materials lift
//    saturation rather than let the dimming wash it out.
//  - The backdrop is toned rather than left alone: lighter (backdropBlur 0.35,
//    so what is behind stays recognisable) and veiled. It KEEPS its colour:
//    backdropSaturation 1.3 measured on alioth's expanded shade as mean chroma
//    0.074 against stock's 0.071, where the 0.0 this preset shipped with in
//    0.4.0 turned the wallpaper grey (0.045) and every card over it with it.
//    backdropSaturation 0 still gives the monotone version.
constexpr PresetEntry kClear[] = {
    {P_frost, 0.0f},
    {P_saturation, 1.3f},
    {P_softenDp, 2.0f},
    {P_noise, 0.0f},
    {P_blurMix, 0.3f},
    {P_lensBlur, 0.3f},
    {P_rimFrost, 1.0f},
    {P_tintR, 0.13f},
    {P_tintG, 0.10f},
    {P_tintB, 0.13f},
    {P_tintA, 0.31f},
    {P_refractionWidthDp, 24.0f},
    {P_refractionHeightDp, 24.0f},
    {P_highlightIntensity, 0.42f},
    {P_highlightWidthDp, 1.0f},
    {P_highlightFalloff, 1.9f},
    {P_backdrop, 1.0f},
    {P_backdropBlur, 0.35f},
    {P_backdropSaturation, 1.3f},
    {P_backdropVeil, 0.15f},
};

// Android's blur, untouched inside the region; the glass lives only at the
// edge. frost 0 / saturation 1 / noise 0 is the combination measured bit-exact
// against stock in the interior (docs/VERIFIED.md). This was `clear` in 0.3.
constexpr PresetEntry kEdge[] = {
    {P_frost, 0.0f},
    {P_saturation, 1.0f},
    {P_noise, 0.0f},
};

// A milkier pane: more frost and colour, a softer lens. Still well short of
// upstream's 0.36 / 1.8, which measured 26% darker than the blur the app asked
// for on this path.
constexpr PresetEntry kFrosted[] = {
    {P_frost, 0.26f},
    {P_saturation, 1.3f},
    {P_noise, 0.06f},
    {P_refractionAmountDp, 44.0f},
    {P_highlightIntensity, 0.32f},
};

// The lens turned up: a wider, deeper rim with strong colour fringing and a
// brighter edge, over a light frost.
constexpr PresetEntry kPrism[] = {
    {P_frost, 0.08f},
    {P_saturation, 1.25f},
    {P_refractionWidthDp, 26.0f},
    {P_refractionHeightDp, 26.0f},
    {P_refractionAmountDp, 90.0f},
    {P_dispersionAmountDp, 12.0f},
    {P_fringe, 1.0f},
    {P_highlightIntensity, 0.4f},
};

// The WebUI's Basic mode: one slider from clear through regular to frosted,
// computed below from persist.sys.glass.variant and the theme.
bool basicCompute(ParamId id, float* out);

#define PRESET(name, summary, table) {name, summary, table, sizeof(table) / sizeof(table[0])}
const Preset kPresets[] = {
    {"basic", "Set by the Basic sliders: clear, regular or frosted, or between (default)",
     nullptr, 0, basicCompute},
    PRESET("clear", "iOS-style clear glass over a lighter, veiled backdrop", kClear),
    // The calibrated defaults, unchanged; listed so the name exists.
    {"regular", "Light frost and colour lift with a refracting rim", nullptr, 0},
    PRESET("frosted", "Milkier pane, more colour, softer lens", kFrosted),
    PRESET("prism", "Strong lens with wide colour fringing at the edge", kPrism),
    PRESET("edge", "Android's own blur inside; glass only at the edge", kEdge),
};
#undef PRESET
constexpr size_t kPresetCount = sizeof(kPresets) / sizeof(kPresets[0]);
// basic, which at variant 0 is exactly `clear`: a device that never chose a
// preset looks the same as before basic existed.
constexpr size_t kDefaultPreset = 0;

// persist.sys.glass.mode and .preset hold names, not numbers, so they get their
// own slot type. Same caching scheme as the float ones.
struct StrSlot {
    const char* key;
    const prop_info* pi = nullptr;
    uint32_t serial = ~0u;
    char name[96] = {0};
    char value[PROP_VALUE_MAX] = {0};
};
StrSlot g_mode_slot{"mode"};
StrSlot g_preset_slot{"preset"};

struct Slot {
    const prop_info* pi = nullptr;
    uint32_t serial = ~0u;
    float value = 0.0f;
    bool overridden = false;
};

Slot g_slots[P_count];  // indexed by ParamId, so &slot - g_slots recovers the id
float g_effective[P_count];  // override, else preset, else default
int64_t g_last_rescan_ms = 0;
int64_t g_last_check_ms = 0;
// How often the properties are looked at, at most. Asking for a property's
// serial was a read of shared memory when this was written; since Android 14
// bionic asks getuid() on the way (the appcompat overrides), which is a system
// call, and paramsRefresh runs for every region drawn. On a Poco F3, with
// thirty properties set and thirty panes on screen at 120 Hz, that was 7% of
// the render thread. Twenty times a second is as live as a slider drag is.
constexpr int64_t kCheckMs = 50;
char g_names[P_count][96];
float g_unit_scale = 2.75f;
float g_system_scale = 2.75f;  // the display's own, for when no density is set
bool g_ready = false;
Mode g_mode = Mode::kStock;
size_t g_preset = kDefaultPreset;
unsigned g_generation = 0;

void readCb(void* cookie, const char*, const char* value, uint32_t) {
    auto* slot = static_cast<Slot*>(cookie);
    // A property cannot be deleted, so an empty value is how a tuning session
    // says "put this one back". Without it the only way out of an experiment is
    // to remember every default and type them all back in.
    if (value == nullptr || value[0] == '\0') {
        slot->value = kSpecs[slot - g_slots].def;
        slot->overridden = false;
        return;
    }
    // "default" reads better than an empty string in a script, and means the
    // same thing.
    if (strcmp(value, "default") == 0) {
        slot->overridden = false;
        return;
    }
    char* end = nullptr;
    float v = strtof(value, &end);
    if (end == value) {
        // Not a number: a name, for the parameters that have them.
        const char* const* names = kSpecs[slot - g_slots].choices;
        int hit = -1;
        for (int i = 0; names != nullptr && names[i] != nullptr; ++i) {
            if (strcmp(value, names[i]) == 0) hit = i;
        }
        if (hit < 0) return;  // unparseable: keep whatever we had
        v = static_cast<float>(hit);
    }
    slot->value = v;
    slot->overridden = true;
}

float systemDensity() {
    // ro.sf.lcd_density is the value SurfaceFlinger itself uses. Emulator
    // images carry qemu.sf.lcd_density instead, and a user-set density lands in
    // persist.sys.lcd_density. First one that answers wins.
    static const char* const kDensityProps[] = {
        "ro.sf.lcd_density",
        "qemu.sf.lcd_density",
        "persist.sys.lcd_density",
    };
    for (const char* name : kDensityProps) {
        char buf[PROP_VALUE_MAX] = {0};
        if (__system_property_get(name, buf) <= 0) continue;
        const long dpi = strtol(buf, nullptr, 10);
        if (dpi > 0) return static_cast<float>(dpi) / 160.0f;
    }
    return 0.0f;
}

void readStrCb(void* cookie, const char*, const char* value, uint32_t) {
    auto* slot = static_cast<StrSlot*>(cookie);
    snprintf(slot->value, sizeof(slot->value), "%s", value != nullptr ? value : "");
}

// Returns true when the value changed.
bool refreshStr(StrSlot& s, bool rescan) {
    if (s.pi == nullptr) {
        if (!rescan) return false;
        s.pi = __system_property_find(s.name);
        if (s.pi == nullptr) return false;
    }
    const uint32_t serial = __system_property_serial(s.pi);
    if (serial == s.serial) return false;
    s.serial = serial;
    __system_property_read_callback(s.pi, readStrCb, &s);
    return true;
}

Mode modeFrom(const char* v) {
    // Only an explicit "glass" (or "on") draws anything. A typo, an empty value
    // or a name from a future version all land on stock, which is the outcome
    // that can never surprise anyone.
    return strcmp(v, "glass") == 0 || strcmp(v, "on") == 0 ? Mode::kGlass : Mode::kStock;
}

size_t presetFrom(const char* v) {
    for (size_t i = 0; i < kPresetCount; ++i) {
        if (strcmp(v, kPresets[i].name) == 0) return i;
    }
    return kDefaultPreset;
}

// What a parameter's own property says, ignoring presets: the input side of a
// computed preset, which must not depend on the preset it computes.
float raw(ParamId id) { return g_slots[id].overridden ? g_slots[id].value : kSpecs[id].def; }

bool darkFrost() {
    const float l = 0.2126f * raw(P_frostR) + 0.7152f * raw(P_frostG) + 0.0722f * raw(P_frostB);
    return l < 0.5f;
}

// Clear at 0, regular at 0.5, frosted at 1, linear in between.
float stops(float v, float clear, float regular, float frosted) {
    v = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
    if (v <= 0.5f) return clear + (regular - clear) * (v * 2.0f);
    return regular + (frosted - regular) * ((v - 0.5f) * 2.0f);
}

// The `basic` preset.
//
// Clear is the `clear` preset exactly, value for value, so the Basic slider at
// its left end is what the phone already showed.
//
// Regular is iOS's regular glass as Core Animation draws it, from the uniforms
// a WebGPU reimplementation of QuartzCore was measured passing
// (projects/uikit-even-how-datamine, FINDINGS.md 5.1): half the blurred read
// (blurMix 0.5), the see-through read pulled 90% toward the blur where the blur
// is brighter in the light theme and darker in the dark one, then a colour
// matrix, light: scale 0.504, saturation 1.905, bias 0.52, a milky white pane;
// dark: a luma limiter at 0.40, saturation 1.3, bias 0.125. Its dimming tint
// fades out on the way there, since the matrix does that job.
//
// Frosted has no reference: it is regular pushed further, mostly blur (0.85)
// with a heavier veil, and the shade's backdrop blurred more as well.
//
// lensBlur follows blurMix all the way, and rimFrost is 1, so the rim is never
// sharper than the pane it borders, as in `clear`.
bool basicCompute(ParamId id, float* out) {
    const float v = raw(P_variant);
    const bool dark = darkFrost();
    switch (id) {
        case P_blurMix:
        case P_lensBlur: *out = stops(v, 0.3f, 0.5f, 0.85f); return true;
        case P_rimFrost: *out = 1.0f; return true;
        case P_fillLighten: *out = dark ? 0.0f : stops(v, 0.0f, 0.9f, 0.9f); return true;
        case P_fillDarken: *out = dark ? stops(v, 0.0f, 0.9f, 0.9f) : 0.0f; return true;
        case P_saturation:
            *out = dark ? stops(v, 1.3f, 1.3f, 1.25f) : stops(v, 1.3f, 1.905f, 1.6f);
            return true;
        case P_lumaLimit:
            *out = dark ? stops(v, 0.0f, 0.40f, 0.45f) : stops(v, 0.0f, 0.06f, 0.06f);
            return true;
        case P_toneScale:
            *out = dark ? stops(v, 1.0f, 1.0f, 0.9f) : stops(v, 1.0f, 0.504f, 0.42f);
            return true;
        case P_toneBias:
            *out = dark ? stops(v, 0.0f, 0.125f, 0.13f) : stops(v, 0.0f, 0.52f, 0.58f);
            return true;
        case P_tintA: *out = stops(v, 0.31f, 0.0f, 0.0f); return true;
        case P_backdropBlur: *out = stops(v, 0.35f, 0.5f, 0.75f); return true;
        case P_backdropVeil: *out = stops(v, 0.15f, 0.2f, 0.3f); return true;
        default: break;
    }
    // Everything else is clear's, at every position.
    for (const PresetEntry& e : kClear) {
        if (e.id == id) { *out = e.value; return true; }
    }
    return false;
}

void recomputeEffective() {
    for (size_t i = 0; i < P_count; ++i) {
        g_effective[i] = g_slots[i].overridden ? g_slots[i].value
                                               : presetValue(g_preset, static_cast<ParamId>(i));
    }
}

}  // namespace

void paramsInit() {
    for (size_t i = 0; i < P_count; ++i) {
        g_slots[i].value = kSpecs[i].def;
        g_effective[i] = kSpecs[i].def;
        snprintf(g_names[i], sizeof(g_names[i]), "persist.sys.glass.%s", kSpecs[i].name);
    }
    for (StrSlot* s : {&g_mode_slot, &g_preset_slot}) {
        snprintf(s->name, sizeof(s->name), "persist.sys.glass.%s", s->key);
    }
    g_ready = true;
    g_last_rescan_ms = -1000000;  // first refresh always looks everything up
    g_last_check_ms = -1000000;
    // The default preset's values, before any property is read. paramsRefresh
    // only recomputes when something CHANGED, and on a device where no
    // persist.sys.glass.* has ever been set nothing does — which left the
    // built-in defaults in force while the preset reported was `clear`. That
    // was invisible while the default preset was `regular`, which IS the
    // built-in defaults.
    recomputeEffective();
    paramsRefresh();

    const float sys = systemDensity();
    g_system_scale = sys > 0.0f ? sys : 2.75f;
    const float override_density = g_effective[P_density];
    g_unit_scale = override_density > 0.0f ? override_density : g_system_scale;
}

void paramsRefresh() {
    if (!g_ready) return;

    // __system_property_find is the only expensive step, and it is only needed
    // for a property that did not exist last time we looked. Rescanning on a
    // call counter looks equivalent and is not: a blur region that is drawn a
    // handful of times per experiment may never reach the counter, so a
    // property being set for the FIRST time appears to do nothing. Half a
    // second bounds both the cost and the latency, whatever the frame rate.
    struct timespec now {};
    clock_gettime(CLOCK_MONOTONIC, &now);
    const int64_t ms = static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
    if (ms - g_last_check_ms < kCheckMs) return;
    g_last_check_ms = ms;
    const bool rescan = ms - g_last_rescan_ms >= 500;
    if (rescan) g_last_rescan_ms = ms;

    bool changed = false;
    for (size_t i = 0; i < P_count; ++i) {
        Slot& s = g_slots[i];
        if (s.pi == nullptr) {
            if (!rescan) continue;
            s.pi = __system_property_find(g_names[i]);
            if (s.pi == nullptr) continue;
        }
        const uint32_t serial = __system_property_serial(s.pi);
        if (serial == s.serial) continue;
        s.serial = serial;
        __system_property_read_callback(s.pi, readCb, &s);
        changed = true;
    }

    if (refreshStr(g_preset_slot, rescan)) {
        const size_t p = presetFrom(g_preset_slot.value);
        if (p != g_preset) { g_preset = p; changed = true; }
    }
    if (refreshStr(g_mode_slot, rescan)) {
        const Mode m = modeFrom(g_mode_slot.value);
        if (m != g_mode) { g_mode = m; ++g_generation; }
    }

    if (changed) {
        recomputeEffective();
        ++g_generation;
    }

    // Either way, every time: a density taken back (the property emptied)
    // used to stay in force until SurfaceFlinger restarted.
    const float override_density = g_effective[P_density];
    g_unit_scale = override_density > 0.0f ? override_density : g_system_scale;
}

float param(ParamId id) { return g_effective[id]; }

Mode paramsMode() {
    // bypass predates the mode switch and still wins, so nothing that used to
    // turn the glass off stops doing so.
    if (g_effective[P_bypass] != 0.0f) return Mode::kStock;
    return g_mode;
}

size_t paramsPresetIndex() { return g_preset; }

unsigned paramsGeneration() { return g_generation; }

const char* paramName(ParamId id) { return kSpecs[id].name; }

float paramDefault(ParamId id) { return kSpecs[id].def; }

bool paramOverridden(ParamId id) { return g_slots[id].overridden; }

const char* const* paramChoices(ParamId id) { return kSpecs[id].choices; }

size_t presetCount() { return kPresetCount; }

const Preset& presetAt(size_t index) { return kPresets[index < kPresetCount ? index : kDefaultPreset]; }

size_t defaultPresetIndex() { return kDefaultPreset; }

bool paramsDarkTheme() { return darkFrost(); }

float presetValue(size_t preset, ParamId id) {
    const Preset& p = presetAt(preset);
    float computed = 0.0f;
    if (p.compute != nullptr && p.compute(id, &computed)) return computed;
    for (size_t i = 0; i < p.count; ++i) {
        if (p.entries[i].id == id) return p.entries[i].value;
    }
    return kSpecs[id].def;
}

float paramsUnitScale() { return g_unit_scale < 0.1f ? 0.1f : g_unit_scale; }

int paramsOverriddenCount() {
    int n = 0;
    for (const Slot& s : g_slots) n += s.overridden ? 1 : 0;
    return n;
}

}  // namespace glass
