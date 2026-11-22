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

## Performance

Under the sliders, the WebUI has three switches for the parts of the look that
cost the most to draw. Everything else the engine does to save time draws the
same picture and is always on; these do not, so they are yours to choose.

| setting | default | the other choice |
|---|---|---|
| **Blur layers** | Two: glass mixes the screen behind it with a blur of it | One, faster: a single blur stands in for both. Clear glass turns slightly soft, Regular loses the detail that showed through it, Frosted looks the same. Tiles and cards on the shade show the shade's blur rather than a stronger one of their own |
| **Edge colour fringe** | Off: the rim bends what is behind it, without the rainbow | On, slower: the rainbow fringe at the rim. The `prism` preset turns it on for itself |
| **See-through resolution** | Full: clear glass shows the screen itself | Quarter, faster: a quarter-size copy of it, blurred by 4.5 dp, so the whole screen is not copied every frame. Only matters with two layers |

The fringe was on by default up to v0.4.19 and is off from v0.4.20. Turn it
on here if you want it back.

What they buy on a phone, in RenderEngine time a frame (a frame has 8.3 ms at
120 Hz and 16.7 at 60):

| | Android's blur | glass, defaults | Quarter | One layer | fringe on |
|---|---|---|---|---|---|
| Poco F3 (Adreno 650, 120 Hz), dragging a home screen of glass icons | 2.0 ms | 12.7 | 9.9 | 8.9 | 14.4 |
| Poco F3, opening and closing Quick Settings | 6.2 ms | 17.5 | 17.1 | 15.1 | 18.4 |
| Poco F3, scrolling the app drawer | 5.8 ms | | 16.7 | 17.2 | 19.6 |
| Poco F1 (Adreno 630, 60 Hz), swiping the home screen | 3.2 ms | 20.5 | | 16.5 | 31.8 |
| Poco F1, opening and closing Quick Settings | 6.8 ms | 18.1 | | 17.0 | 18.7 |

Quarter and One layer are each with the fringe off, as the defaults have it;
an empty cell was not measured. These are v0.4.18's and v0.4.19's numbers.

At the defaults the F3's home screen takes 12.7 ms of the 8.3 a frame has at
120 Hz, so it does not hold 120 Hz; at the fastest setting about one frame in
four is still late. On the F1 the fringe alone cost a third of the home
screen's frame, which is why it is off unless asked for. Most of what was left
was the glass shader's own work for every pixel of every pane, which no
setting removes.

v0.4.20 takes a large part of that away without a setting. It draws a pane
with a slimmer build of the shader whenever the pane uses nothing the build
leaves out, which at the defaults is every pane, and the picture is the same.
On the F1, opening and closing Quick Settings with One layer went from 24.6
ms a frame to 14.1 in one run, with Android's blur at 7.6. The table above
has not been measured again, so expect its glass columns to be lower now.
`leanShader=0` (a switch, below) brings the full shader back for comparing.

A desktop GPU shows none of this (on the Android 17 emulator glass is within a
millisecond of Android's blur and the fringe costs nothing), so measure on the
phone: `harness/phone_ab.sh` in the source tree. Blur layers and See-through
resolution need Android 17; the fringe switch works everywhere.

```sh
glassctl perf                      # what the three are at
glassctl perf layers one           # two | one
glassctl perf fringe on            # on | off
glassctl perf resolution quarter   # full | quarter
glassctl perf fast                 # all three at their fastest
glassctl perf best                 # all three at their best-looking: two layers, fringe on, full
```

Like the mode, they are switches: `reset` and picking a preset leave them
alone.

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
| `rimFrost` | 0 (`basic` and `clear`: 1) | how frosted the rim is against the inside. `0` fades the frost out toward the edge, so the rim shows what is behind more clearly than the pane does; `1` mixes the rim exactly as the inside |
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

How the engine goes about it, also left alone by `reset` and `preset`. The
defaults are the fast paths, and each of them draws what the slow one drew;
the switches are there for a ROM on which one misbehaves, and for measuring.
The exceptions are the three Performance settings, which change the look and
are listed here under the names the engine knows them by. `lazyBlur`,
`paneBlur`, `coveredPanes`, `snapshotCopy` and `glassResolution` need Android
17; before it Android's own blurs are the ones drawn, as they always were.

| property | default | what it does |
|---|---|---|
| `lazyBlur` | 1 | make only the blurs that are drawn, at the radius they are drawn at. `0` = Android blurs every region first and the engine blurs again where it wants another radius, as before |
| `paneBlur` | `derive` | the blur inside a tile or a card that lies on the shade. `derive` makes it from the shade's own blur (cheap, same look); `own` blurs the screen again for it, as before. `through` is **Blur layers: One** (above) and changes the look: such a pane shows the shade's blur, and glass with nothing blurred under it reads one blur in place of the screen and a blur of it |
| `toneShader` | 1 | draw a toned backdrop with a small shader of its own. `0` = with the glass shader, lens off |
| `coveredPanes` | 1 | glass under an open shade or a dialog's blur (home screen icons, a keyboard) is drawn from a light blur instead of the sharp screen, since the blur above hides the difference. `0` = always in full |
| `snapshotCopy` | `full` | glass that shows the screen sharp (a keyboard, the volume panel, a toast) makes the compositor copy the whole screen once a frame. `bounds` copies only the part that glass reads: same picture, less work on the emulator's GPU, not yet measured on a phone. Worth trying if those feel heavy; `glassctl set stats 1` shows `bounds-copies` when it is in use |
| `leanShader` | 1 | a pane with no fringe, grain, press effect or superellipse corner is drawn by a build of the glass shader that leaves those parts out (and the softening, where a pane has none), and a toned backdrop by the tone shader less its superellipse corner: the same picture from less code, since a phone's GPU may pay for what the full shader only tests for. `0` = everything with the full shaders. New in v0.4.20 and not yet measured on a phone; `stats` counts its draws as `lean` |
| `fringe` | 0 | **Edge colour fringe** (above). `1` draws the colour fringe, `dispersionAmountDp` wide; `prism` sets it for itself. A value set here wins over the preset either way, so `0` keeps it out of `prism` too |
| `glassResolution` | `full` | **See-through resolution** (above): `quarter` has glass with nothing blurred under it read a quarter-size, lightly blurred copy of the screen |
| `softenTaps` | 9 | how many times a pixel of clear glass reads the screen to soften it by `softenDp`. `5` is cheaper on a phone and slightly rougher on sharp edges seen through the glass (ghosted outlines at 2x zoom). Only matters with two layers at full resolution |
| `debugPane` | 0 | for measuring only: `1` draws every pane flat (its blur in its shape, no glass), `2` draws none, `3` draws a pane that has no softening with the glass shader less its lens (no bend, no rim; needs `leanShader`), so that `harness/phone_ab.sh` can tell what the lens, the rest of the glass shader and a draw each cost |
| `romMaterials` | 0 | Evolution X 12.x tags some blur regions with a material of its own and draws them as its glass: its ColorOS-style Quick Settings panel (tiles and the panel behind them) and, with its Liquid Glass switch on and the ColorOS look, notification cards and heads-ups on the lock screen and the glass behind the dual shade. In glass mode Stock Glass draws those too, in its own look. `1` leaves them to Evolution X's glass while Stock Glass draws everything else. crDroid 13.x's SurfaceFlinger has the same code, but crDroid's SystemUI never tags a region, so this changes nothing there |
| `stats` | 0 | `1` logs once a second what was blurred and drawn per frame (below) |

## Requirements

- KernelSU, KernelSU-Next, APatch or Magisk
- On KernelSU 3.0 or later, a **metamodule** such as
  [meta-overlayfs](https://github.com/KernelSU-Modules-Repo/meta-overlayfs).
  KernelSU itself no longer mounts modules; without a metamodule this one
  installs but is never mounted, and the installer, `glassctl` and the WebUI all
  say so. They also say when the metamodule is there but cannot mount anything:
  disabled or marked for removal in the manager, or its `module.prop` no longer
  marking it as a metamodule (a hard reset while the file was being rewritten
  can leave it empty or all zero bytes; installing the metamodule again fixes
  that). KernelSU-Next, APatch and Magisk mount modules themselves.
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

v0.4.21 added experimental support for more ROMs, Evolution X 12.x and
crDroid 13.x among them, from a survey of 66 firmware builds. On a phone it
has so far run only on a Poco F3 with a crDroid build from before those
changes; the [main README](../README.md#other-roms-experimental) lists what
changed.

## Root that comes after boot: live mode

Some setups never mount the module: root loaded after boot (temporary root),
KernelSU 3 without a metamodule, or a metamodule that broke. Live mode loads
the engine anyway, for this boot only:

```sh
sh /data/adb/modules/stockglass/glassctl live check   # what it needs; changes nothing
sh /data/adb/modules/stockglass/glassctl live         # load it now
sh /data/adb/modules/stockglass/glassctl unlive       # take it out again
```

It puts a copy of your `surfaceflinger` that loads the engine in a RAM disk,
mounts it over the real one, and restarts SurfaceFlinger. That restart is a
soft reboot: the screen goes black for about ten seconds and every app starts
again, including the terminal or manager you ran it from (the switch finishes
by itself). A reboot undoes everything; nothing is written to your system.

- It needs root, and on an enforcing device a root manager that can add SELinux
  rules while running (KernelSU, KernelSU-Next, APatch and Magisk all can). It
  never switches SELinux to permissive.
- If SurfaceFlinger does not come up with the engine, or dies twice within 90
  seconds, live mode takes itself out and the ROM's own SurfaceFlinger comes
  back.
- So far it has only run on the Android emulator. Run `glassctl live check`
  first on a phone.

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

A boot in which the overlay was never mounted is not counted against it: after
the mounts, `post-mount.sh` notes whether `libglassengine.so` is in place, and if
it was not, SurfaceFlinger ran the ROM's own build that boot and whatever stopped
the boot (a GPU driver failing to start, for one) was something else. The
module then stays enabled, and `last_boot.log` says why.

To re-enable after that:

```sh
rm /data/adb/glass/disabled
```

Installing the module again also clears it.

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

`glassctl set stats 1` adds one line a second while blur is on screen, which is
what to read when the glass feels heavy somewhere:

```
stats: frames=60 blurs/frame=1.00 (android 0.00 + engine 1.00) derived/frame=2.00 deferred/frame=3.00 snapshot-copies/frame=0.00 bounds-copies/frame=0.00 tone=60 backdrop=0 pane=540 covered=0 quarter=0 single=0 lean=540 skipped=0 stock=0 hook=161us/frame
```

`frames` counts blur layers composed (two a frame with the shade over a home
screen of glass icons). `blurs/frame` is full-screen blurs, by who made them;
`derived` are the small ones made from a backdrop's blur. `snapshot-copies`
is how often a frame had to copy the whole screen because a pane shows it
sharp: 1 on the home screen and for a keyboard, 0 in the shade
(`bounds-copies` counts the smaller ones `snapshotCopy=bounds` makes in their
place). `covered`
counts panes drawn under another layer's blur, `quarter` and `single` the ones
drawn from the quarter-size copy and from one blur (the Performance settings),
`lean` the regions a lean build of the glass shader drew (`leanShader`),
`skipped` regions that were off screen or merged away, `stock` regions handed back to Android, and `hook` is
the engine's own time on SurfaceFlinger's render thread.
