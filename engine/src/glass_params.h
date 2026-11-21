// Live-tunable shader parameters, read from system properties.
//
// The names are deliberately identical to the harness's
// /data/local/tmp/glass_params.txt keys (harness/java/.../Params.java), so a
// number calibrated on the app-side harness transfers to the compositor
// verbatim — `setprop persist.sys.glass.saturation 1.8` and the same value that
// was swept on a View is now in SurfaceFlinger.
//
// Three layers decide the value a parameter has, highest first:
//
//   1. persist.sys.glass.<name>     a per-parameter override, set by hand, by
//                                   glassctl or by the WebUI
//   2. persist.sys.glass.preset     a named set of values (clear, regular, ...)
//   3. the built-in default         kSpecs in glass_params.cpp
//
// and one switch decides whether any of it is drawn at all:
//
//   persist.sys.glass.mode          "glass" draws Liquid Glass over the blur
//                                   regions Android already produces. Anything
//                                   else, including unset, leaves Android's own
//                                   blur exactly as it was. Stock is the default:
//                                   the module is opt-in, not a replacement.
//
// Reading ~35 properties on the render thread, per blur region, per frame,
// would be absurd if it meant a trie lookup and a string parse each time. It
// does not: __system_property_find is done once per property and cached, and
// __system_property_serial is a plain load from the property page that only
// changes when someone writes the property. So the steady-state cost is one
// relaxed atomic load per parameter, and the strtof happens once per setprop.
#pragma once

#include <cstddef>

namespace glass {

// Order is arbitrary; it is the *name* that has to match the harness.
enum ParamId : size_t {
    // geometry
    P_cornerRadiusDp = 0,  // floor applied to the SkRRect's own radii, see below
    P_cornerShape,
    // refraction
    P_refractionWidthDp,
    P_refractionHeightDp,
    P_refractionAmountDp,
    P_refractionDepth,
    P_refractionSwirl,
    P_paneSpanDp,  // above this span the region stops being treated as a pane
    P_curvePower,
    P_curveBias,
    // dispersion
    P_dispersionAmountDp,
    P_dispersionHeightDp,
    P_dispersionTapSpacingPx,
    P_dispersionQuadrant,
    // frost handover
    P_softenDp,     // blur on the see-through view itself, as an Android radius
    P_blurMix,
    P_blurFalloff,
    P_lensBlur,     // how much of the compositor's blur the rim keeps; <0 = auto
    P_lensBlurRef,  // the blur radius at which auto reaches 1.0
    P_crossFadeRadius,  // below this radius the glass fades in, as stock's blur does
    P_fillLighten,  // regular glass: see-through pulled toward a brighter blur
    P_fillDarken,   // regular glass: see-through pulled toward a darker blur
    P_fillBlurScale,  // the blurred read's radius, as a multiple of the one asked for
    // tone
    P_frost,
    P_frostR,
    P_frostG,
    P_frostB,
    P_tintR,
    P_tintG,
    P_tintB,
    P_tintA,
    P_saturation,
    P_lumaLimit,    // regular glass: highlights pulled down, k in 1 - k * luma
    P_toneScale,    // regular glass: colour = toneScale * saturated + toneBias
    P_toneBias,
    P_light,
    P_noise,
    P_glassOpacity,
    // Liquid Material: the glass takes the Material You palette's colour
    P_materialTint,  // 0 = plain Liquid Glass, 1 = fully tinted
    P_accentR,       // the palette's accent (system_accent1_500), set by
    P_accentG,       // glass-theme.sh; all 0 = no palette known
    P_accentB,
    // The `basic` preset's one input: 0 clear, 0.5 regular, 1 frosted, and
    // anything between. Other presets ignore it.
    P_variant,
    // highlight
    P_highlightIntensity,
    P_highlightAngleDeg,
    P_highlightWidthDp,
    P_highlightFalloff,
    // backdrop: a region that covers the display (the shade, a dialog's
    // blur-behind) as opposed to a pane drawn over it
    P_backdrop,            // 0 stock, 1 tone, 2 glass; also accepts the names
    P_backdropCoverage,    // fraction of each display side a backdrop must span
    P_backdropBlur,        // tone: fraction of the requested radius actually blurred
    P_backdropSaturation,  // tone: 0 = monotone, 1 = as Android drew it
    P_backdropVeil,        // tone: veil in the theme's frost colour, 0..1
    // display
    P_hdrHeadroom,
    P_density,  // px per dp; 0 = derive from ro.sf.lcd_density
    // press (phase 3)
    P_touchGlow,
    P_touchLens,
    // engine
    P_radius,  // != 0 overrides the compositor's blur radius. Debug aid from M0.
    P_bypass,  // != 0 forces stock whatever the mode says. Kept for old scripts.
    // what the glass costs (glass_draw.cpp, "Blur made to order")
    P_lazyBlur,    // 0 = Android blurs every region before the engine sees it
    P_paneBlur,    // a pane's blurred read: 0 own, 1 derive, 2 through; or the names
    P_toneShader,  // 0 = a toned backdrop runs the whole glass shader
    P_coveredPanes,  // 0 = panes under another layer's backdrop still read the snapshot
    P_snapshotCopy,  // what a pane showing the snapshot reads: 0 full, 1 bounds
    P_leanShader,    // 0 = every pane is drawn by the full glass shader
    // the look traded for speed (the WebUI's Performance card)
    P_fringe,           // 0 = no colour fringe at a pane's edge
    P_glassResolution,  // what glass shows through it: 0 full, 1 quarter
    P_softenTaps,       // reads softenDp takes of the sharp screen: 9 or 5
    P_rimFrost,         // 1 = the rim as frosted as the inside; 0 = the frost fades out to it
    P_debugPane,        // measuring aid: 1 draws panes flat (the tone shader), 2 not at
                        // all, 3 with the glass shader less its lens
    P_romMaterials,     // 1 = regions with an Evolution X material are left to the ROM
    P_stats,       // != 0 logs what the engine made and drew, once a second
    P_count,
};

enum class Mode { kStock, kGlass };

struct PresetEntry {
    ParamId id;
    float value;
};

struct Preset {
    const char* name;     // what persist.sys.glass.preset holds
    const char* summary;  // one line, for glasscheck --presets and the WebUI
    const PresetEntry* entries;
    size_t count;
    // Computed instead of listed: the value for `id` given the live inputs, or
    // false to fall through to the built-in default. Used by `basic`, whose
    // values follow the variant slider and the theme.
    bool (*compute)(ParamId id, float* out) = nullptr;
};

// Refreshes any property whose serial changed. Cheap enough to call on every
// draw: the trie lookup for properties that do not exist yet is rate-limited
// internally, everything else is one relaxed atomic load per parameter.
void paramsRefresh();

// Resolves property handles and applies defaults. Safe to call before the
// property service is fully up; anything unset simply keeps its default.
void paramsInit();

float param(ParamId id);

// Device pixels per dp, from P_density or ro.sf.lcd_density. Always >= 0.1.
float paramsUnitScale();

// Whether the theme is dark, as the frost colour says: glass-theme.sh sets it
// from the system theme, so a dark frost colour means a dark theme. Someone
// who sets the frost colour by hand gets the glass that suits it.
bool paramsDarkTheme();

// For the install-time log: how many properties were actually set.
int paramsOverriddenCount();

// The switch and the preset in force, as of the last paramsRefresh().
Mode paramsMode();
size_t paramsPresetIndex();

// Bumped whenever an effective value, the mode or the preset changes, so a
// caller can log "what the shader is being handed" once per change rather than
// once per frame.
unsigned paramsGeneration();

// The static tables, for anything that has to show them — glasscheck --params
// is how the WebUI and glassctl learn the presets without keeping a copy.
const char* paramName(ParamId id);
float paramDefault(ParamId id);
bool paramOverridden(ParamId id);
// Names a parameter also accepts in place of a number, index = value;
// nullptr-terminated. nullptr when it takes numbers only.
const char* const* paramChoices(ParamId id);
size_t presetCount();
const Preset& presetAt(size_t index);
// The preset's value for `id`, or the built-in default when it does not set it.
float presetValue(size_t preset, ParamId id);
size_t defaultPresetIndex();

}  // namespace glass
