# Stock Glass

An optional Liquid Glass mode for Android's own blur. Switch it on and the
blurred panes the system already draws (notification cards, the volume panel, a
blurred dialog) become refracting glass, with chromatic dispersion at the rim and
a lit edge. The full-screen blur behind them, such as the notification shade,
keeps Android's blur, gets a lighter, veiled version of it, or turns to glass
too: your choice. Switch it off and Android's blur is back, pixel for pixel.

Nothing is themed and no app is modified. The module hooks one function inside
SurfaceFlinger, `BlurFilter::drawBlurRegion`, and in glass mode draws over the
same two images the compositor had already prepared.

## Off until you turn it on

Installing the module does not change how anything looks. Android's blur is
drawn exactly as before (measured identical to the pixel) until you pick
Liquid Glass. Switching is live in both directions, with no reboot after the
first one.

| where | what you get |
|---|---|
| **Action button** (Magisk 28+, KernelSU, APatch) | flips between Android blur and Liquid Glass |
| **WebUI** (KernelSU, KernelSU-Next, APatch) | mode and three Basic sliders; Advanced shows presets and every setting |
| **`glassctl`** (any root shell) | everything the WebUI does |

```sh
sh /data/adb/modules/stockglass/glassctl glass      # Liquid Glass
sh /data/adb/modules/stockglass/glassctl stock      # Android's blur
sh /data/adb/modules/stockglass/glassctl            # what is in force
```

## Basic mode

The WebUI opens on three sliders:

| slider | from | to |
|---|---|---|
| **Blur transparency** | Clear: see-through glass with a 2 dp softening (the `clear` preset exactly) | Regular, then Frosted |
| **Blur intensity** | lighter: a quarter of the blur each app asks for | heavier: twice it. The middle is the app's own blur. Android 17 only |
| **Tint intensity** | Liquid Glass | Liquid Material: the glass takes the colour of your wallpaper palette |

Regular is iOS's regular glass as Core Animation draws it: half of what is
behind is a heavy blur, and the see-through half is pulled toward that blur
where the blur is brighter (light theme) or darker (dark theme), so the pane
reads as frosted while keeping its edges. Then a tone: in light mode a milky
white lift, in dark mode a limiter on highlights and a small lift. The numbers
come from a WebGPU reimplementation of QuartzCore, not from an iPhone. Frosted
pushes the same recipe further: mostly blur, a heavier veil, and the shade
blurred more.

Liquid Material filters what the glass transmits through the palette's accent
and moves its veil and tint toward the same hue at the same brightness. The rim
light stays white.

**Advanced** shows the presets and every setting, with the values the sliders
produce, so there is one set of numbers. Picking a named preset there takes
over from Blur transparency until you move it again; intensity and tint apply
to every preset.

```sh
glassctl basic 0.5        # Regular
glassctl intensity 1.5    # 1.5x the blur each app asks for
glassctl material 0.4     # 40% Liquid Material
```

## Presets

| preset | panes | shade |
|---|---|---|
| `basic` | set by Blur transparency: `clear` at 0, Regular at 0.5, Frosted at 1, and anything between (the default) | toned, blurred more toward Frosted |
| `clear` | iOS-style clear glass: see-through with a 2 dp softening, dimmed, strong refracting edge | toned: lighter, veiled, colour kept (slightly lifted) |
| `regular` | light frost and colour lift with a refracting rim | Android's blur |
| `frosted` | milkier pane, more colour, softer lens | Android's blur |
| `prism` | strong lens with wide colour fringing at the edge | Android's blur |
| `edge` | Android's own blur inside the pane, untouched; the glass lives only at the edge (`clear` before 0.4) | Android's blur |

```sh
glassctl presets
glassctl preset regular
```

Picking a preset clears any tuning, so the preset looks as designed.

## The shade and the panes

Android draws two kinds of blur, and the module treats them differently.

- A **backdrop** covers the screen: the notification shade, a dialog's
  blur-behind.
- A **pane** is anything smaller drawn over it: each notification card, each
  piece of the volume panel, a blurred dialog window.

Panes always get Liquid Glass. The backdrop gets one of three styles:

| style | what the shade looks like |
|---|---|
| `stock` | Android's blur, untouched |
| `tone` | Android's blur, lighter and veiled in the theme's colour; `backdropSaturation` sets its colour (0 is monotone) |
| `glass` | Liquid Glass, like a pane |

```sh
glassctl backdrop tone
glassctl backdrop stock
```

A pane over the shade looks through the shade as it is drawn, so a notification
card refracts the toned shade rather than the home screen beneath it.

Quick Settings tiles are not blurred by Android, so the compositor never sees
them, and the same goes for launcher folders and icons. Those need an app-side
module and are not part of this one.

## Tuning

Any setting can be overridden on top of the preset. Overrides stay when you
switch between Android blur and glass, and are cleared when you pick a preset.

```sh
glassctl set refractionAmountDp 40     # on top of the preset
glassctl unset refractionAmountDp      # back to the preset's value
glassctl reset                         # drop every override
```

These are ordinary properties, so `setprop persist.sys.glass.<name> <value>`
works too, and an empty value means "back to the preset". Changes apply within
half a second.

Useful ones. The defaults listed are the built-in values, which is exactly what
`regular` uses; the other presets change some of them (`glassctl json` shows
what is in force).

| setting | default | what it does |
|---|---|---|
| `frost` | 0.12 | how milky the interior is |
| `saturation` | 1.15 | colour boost |
| `softenDp` | 0 | a blur on what is seen through the glass, as an Android blur radius in dp; `clear` uses 2 |
| `refractionAmountDp` | 60 | how far the rim bends what it samples |
| `refractionWidthDp` / `HeightDp` | 20 | how deep the refracting rim is |
| `dispersionAmountDp` | 6 | chromatic fringe width |
| `highlightIntensity` | 0.25 | the lit edge |
| `cornerRadiusDp` | 0 | floor on a region's own corner radius |
| `tintR/G/B`, `tintA` | 0 | a colour tint and its strength |
| `crossFadeRadius` | 10 | below this blur radius the glass fades in, as Android's blur does |
| `lensBlur` | −1 | rim blur; negative = derive from the app's blur radius |
| `paneSpanDp` | 480 | above this span the glass's dome flattens out |
| `blurMix` | 1 | how much of the interior is the blurred read rather than the see-through one |
| `fillLighten` / `fillDarken` | 0 | regular glass: pull the see-through read toward a brighter / darker blur |
| `toneScale`, `toneBias` | 1, 0 | regular glass: colour = scale × saturated colour + bias |
| `lumaLimit` | 0 | regular glass: pull highlights down (the reference's dark mode uses 0.4) |

For a toned backdrop:

| setting | default | what it does |
|---|---|---|
| `backdrop` | `stock` | `stock`, `tone` or `glass` (the preset may set it) |
| `backdropBlur` | 1 | fraction of the shade's blur radius actually used; 0.35 keeps the home screen readable. Android 17 |
| `backdropSaturation` | 1 | 1 keeps the colour, more lifts it (`clear` uses 1.3), 0 is monotone |
| `backdropVeil` | 0 | a veil in the theme's colour: darker in dark mode, lighter in light mode |
| `backdropCoverage` | 0.9 | how much of the screen, in both directions, a blur must cover to count as a backdrop |

Switches, which `reset` and `preset` leave alone:

| property | default | what it does |
|---|---|---|
| `mode` | `stock` | `glass` draws Liquid Glass; anything else is Android's blur |
| `preset` | `basic` | see above |
| `variant` | 0 | Blur transparency: what `basic` is, 0 clear to 1 frosted |
| `fillBlurScale` | 1 | Blur intensity: the blurred read, times the radius the app asked for. Android 17 |
| `materialTint` | 0 | Tint intensity: 0 plain glass, 1 fully Liquid Material |
| `follow_theme` | on | `0` = stop tracking the theme, manage `frostR/G/B` and `accentR/G/B` by hand |
| `enabled` | 1 | `0` = do not even install the hook at the next boot |

`bypass=1` from earlier versions still forces Android's blur.

## Requirements

- KernelSU, KernelSU-Next, APatch or Magisk
- On KernelSU 3.0 or later, a **metamodule** such as
  [meta-overlayfs](https://github.com/KernelSU-Modules-Repo/meta-overlayfs).
  KernelSU itself no longer mounts modules; without a metamodule this one
  installs but is never mounted, and the installer, `glassctl` and the WebUI all
  say so. KernelSU-Next, APatch and Magisk mount modules themselves.
- A 64-bit device (arm64-v8a or x86_64)
- Android 14, 15, 16 or 17
- `ro.surface_flinger.supports_background_blur` = 1, and
  `persist.sys.sf.disable_blurs` not 1. If your ROM has blur disabled there is
  nothing for this to act on.

## Will it work on my ROM?

The installer answers that before writing anything. It resolves every Skia entry
point the engine needs out of your `surfaceflinger`'s own debug symbols, and
aborts with a list if any are missing. A ROM it cannot drive costs you an error
message, not a boot.

## Light and dark, and the palette

The frost colour follows the system theme, and Liquid Material follows the
wallpaper palette. SurfaceFlinger cannot see either, so the module watches them
and sets `frostR/G/B` and `accentR/G/B` (from `system_accent1_500`) for you,
within about 15 seconds of a change. Which way Regular and Frosted go, lighten
or darken, follows the frost colour. Turn all of it off in the WebUI or with
`setprop persist.sys.glass.follow_theme 0`.

## Tinted glass for apps

A blur region has no field for a colour, but its four corner radii are floats
that reach SurfaceFlinger exactly as the app set them. So an app (or an
app-side module, say for a Quick Settings tile that is on) asks for tinted
glass by putting the tint in the sub-pixel part of the corner radii:

```
corner radius = whole pixels + (2 * q + 1) / 512        q = 0..255

top-left: red   top-right: green   bottom-right: blue   bottom-left: strength
```

Every corner must carry it, and each radius must stay at least a pixel below
half the region's shorter side (Skia scales down corners that do not fit, which
would erase the fractions). With the module in glass mode the region is drawn
as glass in that colour: what it transmits is filtered by it, and the colour is
laid over it at that strength. In Android's own blur the corners are off by
under a pixel, which nobody can see. Ordinary radii never decode as a tint: a
radius computed from dp lands on a multiple of 1/16 px or coarser, and these are
odd multiples of 1/512.

```java
// BackgroundBlurDrawable d = viewRootImpl.createBackgroundBlurDrawable();
static float corner(float radiusPx, int q) {
    return (float) Math.floor(radiusPx) + (2 * q + 1) / 512f;
}
d.setCornerRadius(corner(r, red), corner(r, green), corner(r, alpha), corner(r, blue));
//                top-left        top-right          bottom-left        bottom-right
```

(`setCornerRadius(tl, tr, bl, br)` takes bottom-left before bottom-right.)

## The companion's parts

Quick Settings tiles, launcher icons, folders, widgets and the rest are never
blurred by Android, so the engine never sees them. The Stock Glass Companion, a
separate LSPosed module, makes them blur regions that the engine turns into
glass (`companion/README.md`). The lockscreen clock is the exception: glass
shaped like its digits cannot be a blur region, so the companion draws it in
System UI itself, over a capture of the wallpaper. Each part can
be switched off on its own: in the WebUI under **Advanced → Apps**, or from a
root shell:

```
glassctl app                  # every part, on or off
glassctl app icons off        # leave themed app icons as Android draws them
glassctl app tint 0.6         # how strongly an active QS tile's glass takes its colour
glassctl app tilecolor 0.5    # ...and how much of its own colour it keeps over that
glassctl app scrimcolor black # the shade's background in one colour: stock, black, white or tint
```

A switch reaches every app at once. The companion draws nothing while the
engine is in Android blur mode, whatever these say.

## If a boot ever fails

It disables itself. A marker is written before the compositor starts and cleared
only once the system finishes booting, so a boot that never completes leaves it
behind, and the next boot sees it and builds no overlay at all — the device comes
up exactly as if the module were not installed.

To re-enable after that:

```sh
rm /data/adb/glass/disabled
```

`/data/adb/glass/last_boot.log` says what happened.

## What it does to your system

- Copies your `surfaceflinger`, adds one `DT_NEEDED` entry to the copy, and
  mounts the copy over the original. Your real binary is never written to.
- Adds `/system/lib64/libglassengine.so`, again by mount.
- Adds two SELinux rules for `surfaceflinger` (`sepolicy.rule`): `execmem`, so
  it can allocate the hook's trampolines, and `execmod` on its own binary, so
  the first instructions of the hooked functions can be patched. Android denies
  both by default and the hook cannot be installed without them. They loosen
  W^X for the compositor process only. If they ever fail to apply, the engine
  notices before hooking anything and leaves Android's blur alone.
- Lets `surfaceflinger` set `sys.*` and `persist.sys.*` properties
  (`system_prop`). Stock policy already allows it on the devices measured; the
  rule is there for builds that differ. The engine uses it for its crash guard
  and to report its status: see below.
- Keeps state in `/data/adb/glass`, removed on uninstall.
- Runs one small shell loop to track the light/dark theme.

In Android-blur mode the hook is still installed, so that switching is live, but
it passes every call straight to the original. The glass shader is not even
compiled until you first switch glass on.

Uninstalling restores everything by removing the mount. There is nothing to undo.

## If SurfaceFlinger crashes with glass on

While it draws glass, the engine keeps `sys.glass.active=1`. If SurfaceFlinger
dies and restarts, the new one finds that, switches the saved mode back to
Android's blur, and notes the time in `sys.glass.tripped`. You get one crash and
a working screen, not a loop. `glassctl status` and the WebUI say it happened;
`glassctl glass` turns glass back on and clears the note. The crash itself is in
`/data/tombstones`.

Updating while glass is on: from 0.4.10 on, the installer swaps the running
files out by rename, so an update cannot crash SurfaceFlinger. Installing 0.4.9
or older over a running copy under meta-overlayfs can crash it once, and glass
then comes back off: `glassctl glass` turns it on again.

This is separate from the boot guard above, which only catches a boot that never
completes. `sys.glass.engine` says what the engine did at load: `hooked`, `glass`
(the shader validated), `no-glass` (it did not; Android's blur is drawn) or
`not-hooked`.

## Logs

```sh
adb logcat -s StockGlass
```

At startup it reports which Skia entry points it found and which code path this
ROM took. It logs every mode, preset or backdrop change, and per blur region it
reports whether it was treated as a backdrop or a pane, its geometry, and the
parameters in force, whenever any of them changes.
