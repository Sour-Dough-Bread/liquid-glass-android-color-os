# StockGlass in a ROM

This directory makes a patch for `frameworks/native` that builds StockGlass
into SurfaceFlinger. A ROM that carries it draws Liquid Glass over window
blurs with no root, no module and no hooking. Users switch it on the same way
they switch the module on, with `persist.sys.glass.mode=glass`, and every
`persist.sys.glass.*` property, preset and default means what it means for
the module.

Generated patches are not kept in git. Make one for the tree you are patching
(below), and make it again when the engine changes.

## What the patch does

RenderEngine picks one blur filter at start-up (Kawase, Gaussian, the Kawase
dual filter, or a fork's own). The patch wraps that filter in a `GlassFilter`,
which takes over `drawBlurRegion`:

- With `persist.sys.glass.mode=glass`, each blur region is drawn with the
  Liquid Glass shader: backdrops (the shade, a dialog's blur) and panes
  (notification cards, the volume panel) get their own looks, the lean shader
  builds draw the panes that allow them, and regions that carry a tint get it.
- With any other value, or none, the wrapped filter draws the region exactly
  as it would have. Glass is opt-in and Android's blur is the default.
- With `persist.sys.glass.enabled=0` at boot there is no wrapper at all.
- On Android 17 it also answers `generateTemporaryImage` while glass is on, so
  that each blur is made once, at the radius the region is drawn at ("blur
  made to order" in the engine). On Android 16 QPR2 it leaves `generate()`
  alone, as the module does there, and draws from the blur Android made.
- If SurfaceFlinger dies while drawing glass, the next one starts with
  `persist.sys.glass.mode=stock` (the module's crash guard). If RenderEngine
  runs on Graphite, the glass stands down at the first region it would draw.
- On Evolution X and crDroid, whose `drawBlurRegion` takes each region's
  material and whose own Liquid Glass comes with it, the material is passed
  through: below.

The drawing code is the module's, compiled as it is. The patch adds:

| File | What it is |
| --- | --- |
| `libs/renderengine/skia/filters/GlassFilter.{h,cpp}` | the wrapper ([glue/](glue/)) |
| `libs/renderengine/skia/filters/GlassFilterApi.h` | which `BlurFilter` this tree has, found by [tools/blur_api.py](tools/blur_api.py) |
| `libs/renderengine/skia/filters/glass/` | `librenderengine_glass`: the engine's drawing code (`glass_draw.{h,cpp}`, `sk_shared.cpp`), shader, parameter tables and crash guard from `engine/src`, and a Skia bridge that calls the linked Skia ([glue/glass/](glue/glass/)) |

and changes three existing files:

- `BlurFilter.h`: `drawBlurRegion` becomes `virtual`, and so do `setDisplay`
  and `lensReach` where the tree has them.
- `SkiaRenderEngine.cpp`: includes `GlassFilter.h`, and after the
  `switch (blurAlgorithm)` in the constructor wraps the filter with
  `mBlurFilter = GlassFilter::wrap(mBlurFilter, mRuntimeEffectManager);`.
- `Android.bp`: `GlassFilter.cpp` joins `librenderengine_skia_sources`, and
  `librenderengine` takes `librenderengine_glass` in `whole_static_libs`.

`BlurFilter.cpp` and the filters themselves are not touched.

### Evolution X and crDroid

Evolution X 12.x (`Evolution-X/frameworks_native`, branch `cnb`) and crDroid
13.x (`crdroidandroid/android_frameworks_native`, branch `17.0`) carry their
own Liquid Glass. Their `drawBlurRegion` takes the region's material last:
-1 for a window's own blur, 0 for the notification shade's, and 1 to 11 for
the materials a client asks for (11 is the dock). Their filter also has
`setDisplay`, which picks their glass per display, and Evolution X's has
`lensReach`, which says how far past a region their glass reads.

- `GlassFilter` overrides `drawBlurRegion` with the material and hands it to
  the drawing code. Which materials StockGlass draws is decided in one place,
  `glassTakesMaterial()` in `engine/src/glass_draw.cpp`, the same for the
  module and the patch. In glass mode StockGlass draws every region,
  materials 1 to 11 included, unless `persist.sys.glass.romMaterials=1`
  leaves those to the ROM's own glass. A material above 11 always stays the
  ROM's. A region StockGlass leaves goes to the wrapped filter with its
  material and the radius it asked for.
- In stock mode every region goes to the wrapped filter with its material,
  so the ROM's own Liquid Glass works as before when its user has it on.
- `setDisplay` and `lensReach` become `virtual` and `GlassFilter` passes them
  on to the filter it wraps; without that, they would reach only
  `GlassFilter`'s own `BlurFilter` part, and the ROM's glass would never be
  switched on.

`tools/blur_api.py` checks that `SkiaRenderEngine.cpp` calls nothing else on
its filter that `GlassFilter` would have to pass on. A fork that does stops
the script with the name of the call.

## Making the patch

```sh
cd /path/to/stock-glass
rom/make_patch.sh android17-release        # AOSP, fetched from android.googlesource.com
rom/make_patch.sh lineage-24.0             # LineageOS, fetched from github.com/LineageOS
rom/make_patch.sh evox-cnb                 # Evolution X, branch cnb
rom/make_patch.sh crdroid-17.0             # crDroid, branch 17.0
rom/make_patch.sh mytree --tree /path/to/rom/frameworks/native   # any fork, from your checkout
```

Without `--tree` the script fetches the three files it edits at the branch's
current commit and records that commit in the patch. The patch lands in
`rom/out/<branch>/0001-renderengine-stockglass-liquid-glass.patch`, and the
patched files in `rom/out/<branch>/tree/`.

The edits are found by what the code says, not by line numbers, so a fork
that moved things around still gets a patch. If an anchor is missing (no
`switch (blurAlgorithm)` that assigns `mBlurFilter`, say), the script stops
and says which.

Checked so far:

| Branch | Base commit | Notes |
| --- | --- | --- |
| AOSP `android17-release` | `ae266dcb706d` | |
| AOSP `android16-qpr2-release` | `fcbde2bcff56` | |
| LineageOS `lineage-24.0` | `a96adf2c6e62` | renderengine identical to AOSP 17; either patch applies |
| LineageOS `lineage-23.2` | `26f530caefb3` | one backported hunk in `SkiaRenderEngine.cpp`, away from the patch; the AOSP 16 QPR2 patch applies too |
| Evolution X `cnb` | `dfe5f014b7b2` | material, `setDisplay`, `lensReach`; `KawaseDarkmoon` in the switch |
| crDroid `17.0` | `c5f9e4d3fe25` | material and `setDisplay`; `KawaseDarkmoon` in the switch |

## Applying it

```sh
cd /path/to/rom/frameworks/native
git am /path/to/0001-renderengine-stockglass-liquid-glass.patch
```

Then build as usual. Nothing goes in the device tree: `librenderengine` is
linked into SurfaceFlinger, and the new library comes with it.

## Properties

The same as the module's, so `glassctl`, its WebUI and the LSPosed companion
read and write the same names.

| Property | Values |
| --- | --- |
| `persist.sys.glass.mode` | `glass` (or `on`) draws glass; anything else, including unset, is Android's blur |
| `persist.sys.glass.preset` | `basic` (default), `clear`, `regular`, `frosted`, `prism`, `edge` |
| `persist.sys.glass.variant`, `.fillBlurScale`, `.materialTint` | the Basic sliders |
| `persist.sys.glass.<name>` | any engine parameter; `engine/src/glass_params.cpp` has the list and defaults |
| `persist.sys.glass.enabled` | `0` at boot leaves the filter unwrapped |

SurfaceFlinger writes `sys.glass.engine` (`hooked`, then `glass` or
`no-glass` at the first glass frame), `sys.glass.regen` (`1` where blurs are
made to order), and the crash guard's `sys.glass.active` and
`sys.glass.tripped`. It also sets `sys.glass.builtin=1`, which the module
never does, so tools can tell the built-in engine from the module.

## SELinux

Nothing is required. AOSP labels `persist.sys.*` and `sys.*` as
`system_prop`, and stock policy already lets SurfaceFlinger read and set it,
lets Settings (`system_app`) set it and lets every app and the shell read it.
The module's own rules are for hooking code at runtime and for ROMs whose
policy differs from AOSP's; the patch hooks nothing.

The one thing stock policy refuses is the shell setting these properties, so
`adb shell setprop persist.sys.glass.mode glass` fails on a user build. A ROM
that wants that can add [sepolicy/glass.te](sepolicy/glass.te) and
[sepolicy/property_contexts](sepolicy/property_contexts) to its private
platform policy (for LineageOS, `device/lineage/sepolicy/common/private`).
They give the properties their own type with the same access as before, plus
`set_prop` for the shell.

## Switching it on without root

- **A Settings switch.** Settings runs as `system_app` and may set the
  properties on stock policy:

  ```java
  SystemProperties.set("persist.sys.glass.mode", on ? "glass" : "stock");
  SystemProperties.set("persist.sys.glass.preset", "frosted");
  ```

  SurfaceFlinger picks the change up within half a second. Nothing restarts.
- **adb shell**, with the optional policy above:
  `adb shell setprop persist.sys.glass.mode glass`.
- **glassctl** from the module also runs as a plain shell script once its
  `setprop` calls are allowed (the optional policy). `preset` and `status`
  also need `glasscheck`, which glassctl looks for at `bin/<abi>/glasscheck`
  beside itself. Its engine line looks for the module's library in
  SurfaceFlinger, so for now it reports a built-in engine as absent.

Two things the module does from root scripts have no counterpart in the
patch. `glass-theme.sh` follows light and dark mode (`frostR/G/B`) and the
Material You accent (`accentR/G/B`); without it the glass keeps the dark
frost and Liquid Material has no colour. A ROM can set those six properties
from wherever it already tracks the theme, as `system_server` or Settings.
The companion's restyling of SystemUI and the launcher needs LSPosed.

## How the patch stays in step with the module

Everything in it comes from this repository when the script runs:
`engine/src` for the drawing code, the shader
(`engine/src/generated/glass_shader_src.h`, checked byte for byte) and the
parameter tables, and `rom/glue/` for the wrapper. Change the engine, run the
script again, and the patch carries the change.

`engine/src` keeps the drawing half apart from the module's: `glass_draw.cpp`
and `sk_shared.cpp` are what both build, behind the API in `glass_draw.h`;
`glass_engine.cpp` (the hooks, symbol resolution, boot guard) and
`sk_bridge.cpp` (the module's own Skia members and `compile()`) are the
module's alone and never go into the patch.

## What has not been checked

Every new and changed source compiles with `-Werror` and Soong's flags
against the real headers of all six branches, using the NDK's clang
(`captures/sf-corpus/reports/rom_patch.md` has the details). Not done yet:
a full ROM build, and running it on a phone. `SkiaRenderEngine.cpp` itself
was checked through a stand-in that compiles the added lines against its
real member declarations, because the whole file needs libgui's generated
binder headers. The patch's `Android.bp` has not been through Soong.

The StockGlass repository has no licence file yet. The new `Android.bp`
assumes `frameworks/native`'s Apache-2.0, which only the author can grant.
