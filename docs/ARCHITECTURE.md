# Architecture decisions

## Two delivery tracks, and which one is actually hacky

There are two ways to get this effect onto a device, and they have very
different risk profiles. It matters to keep them separate, because the
uncomfortable part below applies to only one of them.

### Track A — source patch (Lineage, crDroid, any build-from-source ROM)

A patch to `frameworks/native/libs/renderengine` that compiles the engine's
drawing into RenderEngine, against whatever `external/skia` is already in the
tree. `rom/make_patch.sh <branch>` writes it, ready for `git am`: a
`GlassFilter` that wraps whichever blur filter SkiaRenderEngine picks and takes
its `drawBlurRegion` (made `virtual` for it) and `generateTemporaryImage`.
Besides the new files, it changes one word in `BlurFilter.h`, one statement and
an include in `SkiaRenderEngine.cpp`, and two lines of `Android.bp`;
`BlurFilter.cpp` and the filters stay AOSP's, so stock mode draws AOSP's own
pixels. The drawing code is the module's, unchanged: it calls Skia through the
same table of function pointers, which the ROM build fills with direct calls.
`rom/README.md` is for maintainers.

- No symbol resolution, no inline hooking, no ABI guessing, no injection.
- Nothing can drift, because the compiler sees the same Skia the rest of the
  image sees.
- Maintainers can carry it; users flash a ROM they were flashing anyway.

**This is the non-hacky path, and it is the one that fits the actual target
audience** — crDroid/Lineage on beryllium are built from source by definition.

### Track B — KernelSU module (stock, prebuilt, or unwilling-to-reflash)

Resolve `BlurFilter::drawBlurRegion` out of the stripped binary's MiniDebugInfo,
inline-hook it, and rebuild the draw using Skia entry points called through
resolved function pointers.

This is the hacky one. It exists because a large share of users cannot or will
not change ROM, and it is the only way to reach them.

## Decision: vendor pinned Skia headers (Track B)

**Chosen.** The engine compiles against Skia public headers pinned to the target
Android release, and resolves only genuinely out-of-line symbols.

### Why headers are unavoidable

Measured on real binaries (see `VERIFIED.md`): `SkPaint()`, `SkPaint::setShader`
and `SkRuntimeEffect::uniforms()` have **no out-of-line symbol on either A14 or
A16**. They are header-inline, so there is nothing to call. Their bodies — and
the object layouts they assume — have to come from somewhere, and the only
correct source is the headers the target was built from.

The alternative considered and rejected was hand-declaring layout-compatible
structs for `SkPaint` and the builder. Smaller, but it replaces "use the exact
source the ROM was built from" with "guess and hope", which is strictly worse at
the same risk.

### Why this is tolerable rather than reckless

It *is* hacky. What keeps it on the right side of the line:

1. **The headers are not guessed.** They are the exact pinned source tree the
   ROM was built from, not an approximation.
2. **The layout-sensitive surface is tiny.** `SkPaint` plus
   `SkRuntimeEffectBuilder` — not all of Skia. Everything else goes through
   resolved function pointers where only the *signature* has to match, and
   signatures are checked against a candidate list with drift detection.
3. **`SkPaint` is about as stable as Skia gets.** It is public API, used
   everywhere, and its field set changes rarely.
4. **We develop on a snapshot-able emulator.** A wrong layout crashes
   SurfaceFlinger, which is a soft reboot — 60 seconds and a snapshot restore on
   an AVD, versus a bootloop on a phone. The whole reason the AVD came first.
5. **The kill switch bounds the blast radius in the field.** A boot-success
   marker written at `post-fs-data` and cleared once `sys.boot_completed` lands
   means a module that bootloops disables itself on the next boot.
6. **Refusal is always available.** Drift detection, strategy availability, and
   shader-compile failure all fall through to calling the original
   `drawBlurRegion`. Doing nothing is a supported outcome at every step.

### Which pin

Not "Android 16" — **Android 16 at a specific QPR**. These are genuinely
different trees:

- LineageOS 23.0 is Android 16 **QPR0**, because Google never pushed QPR1 source
  to AOSP.
- LineageOS 23.2 is Android 16 **QPR2**.
- crDroid v12.7 for beryllium (the eventual hardware target) is **QPR2**.
- The development AVD is `BE2A.250530.026.D1`, an Android 16 release build.

So the header pin is a per-target property, tracked alongside the device. The
engine must therefore never *assume* its headers match: strategy availability
and mangling drift are checked at load time against the actual binary, and a
mismatch declines to install rather than proceeding.

### What is NOT pinned

`BlurFilter::drawBlurRegion` mangles byte-identically on Android 14 and 16, and
its signature matches AOSP `main`. The hook target itself is stable; only the
Skia call surface around it moves.

## Shader construction: settled on the direct path

`SkRuntimeEffect::uniforms()` is inlined on every build measured, so the uniform
offsets are not reachable through it. Two ways out were considered:

- **`builder`** — `SkRuntimeEffectBuilder` packs uniforms for us, but its packing
  lives in *its own object layout*, so using it means depending on that layout
  too.
- **`direct`** — `SkRuntimeEffect::findUniform` returns each uniform's offset
  directly, and `SkRuntimeEffect::makeShader` takes a plain `SkData` blob. No
  builder, no builder layout.

**`direct` is what the engine uses.** `findUniform` and `findChild` resolve on
both A14 and A16, so it is also the one path that works everywhere — the builder
turned out not to be needed on either. The builder entry points are still
resolved and reported, because how much of it survives out-of-line is a useful
signal when bringing up a new target, but nothing calls them.

Using `findUniform` for the offsets has a second benefit: the engine never
reimplements Skia's uniform packing rules, and a shader whose uniforms move is
detected at load time rather than rendering wrong.

**Without `findUniform`.** Pixel's BP1A (Android 15, March and May 2025, five
devices) and Nothing OS 3.2 for the Phone (2) inline `findUniform` and
`findChild` away entirely, while `uniformSize()` survives. On those builds the
engine does reimplement the packing rule, because it is short and stable:
`SkRuntimeEffectPriv::VarAsUniform` lays the uniforms out in declaration order,
each straight after the one before, at 4 bytes a component whatever the
precision, and the children are numbered in declaration order. SkSL never drops
a uniform as unused (`ProgramUsage::isDead` says uniforms are never dead).
`engine/build.sh` packs the offsets from the same SkSL that is embedded
(`kGlassUniformPacked`), and the engine trusts them on a build only if the
effect's `uniformSize()` comes out at the packed total (268 bytes for the glass
shader, 92 for the tone shader), which it would not if a uniform had been
dropped or sized differently. Where `findUniform` exists the engine checks the
packed layout against it and logs the result, so every build that does not
need the rule tests it; `debug.glass.noFindUniform=1` forces the fallback on
such a build for trying it.

## Layout dependencies, in full

Everything the engine touches by layout rather than by call, so the list can be
re-checked when the pin moves:

| type | what is read | source |
|---|---|---|
| `SkRuntimeEffect::Result` | `effect@0`, `errorText@8` | header |
| `SkString`, `SkString::Rec` | `fLength@0`, `data@8` | header |
| `SkPaint` | size and alignment only; contents built by `reset()`. Measured 80 bytes on both A14 and A16; `ScopedPaint` adds 48 bytes of headroom | header |
| `SkPaint::fShader` | written directly — **only when `SkPaint::setShader` did not resolve** | header |
| `SkCanvas` / `MCRec` | `fMCRec`, `fMatrix` | header — **only when `SkCanvas::getTotalMatrix` did not resolve** (A16 inlined it; A14 kept it) |
| `SkData` | `writable_data()` | header |
| `SkRRect` | `rect()`, `radii()` | header |
| `SkRuntimeEffect::ChildPtr` | one `sk_sp`, 8 bytes, trivially relocatable | header |
| `SkImage` | `width()`, `height()`, `uniqueID()` (`fInfo@16`, dimensions at +16, `fUniqueID@40`), to tell a backdrop from a pane and recognise "this layer" | header; checked at run time against `blurRect` |
| `SkRefCntBase` | `fRefCnt@8`, through `sk_sp` / `SkSafeRef` on images the engine keeps | header |

Every entry says "header" because the headers are the pinned tree the ROM's Skia
was built from. Two of them — `SkCanvas::getTotalMatrix` and `SkPaint::setShader`
— are *defined* by the engine, but **only on builds that inlined them away**.
Android 16 did; Android 14 did not, and there the engine calls the target's own
copy and depends on nothing but the signature. Which path a target took is
printed at install. Both definitions were cross-checked against the target's own
compiled `drawBlurRegion`; see VERIFIED.md.

The practical consequence is that the layout surface is not fixed — it *shrinks*
on targets that kept more code out-of-line. Bringing up a new ROM starts by
running `engine/test/resolve_test` against its `surfaceflinger` to see which
column it lands in.

## Milestones

- **M0 — prove the hook, zero Skia surface.** Hook `drawBlurRegion`, alter
  `blurRadius`, call the original. Validates symbol resolution, load bias,
  inline hooking, injection, SELinux and the kill switch with no ABI risk at
  all. Obviously visible on the volume panel.
- **M1 — draw our shader. DONE.** `drawBlurRegion` replaced; two child shaders
  with `getShaderTransform` matrices; `glass_high.sksl` compiled and run through
  SF's own `SkRuntimeEffect`. Verified by neutralising every tone term and
  measuring the interior **bit-exact against stock**. Live `persist.sys.glass.*`
  tuning landed here rather than in M2, because calibrating anything without it
  meant a rebuild and an SF restart per value.
- **M2 — glassify-all.** Mostly a tuning and policy milestone now that the
  property channel exists. Open questions it has to answer, all raised by M1's
  measurements:
  - `refractionDepth` needs to scale with region size, or full-screen regions
    refract into arcs.
  - a corner-radius floor for the plain-rect regions stock actually produces.
  - whether to opt *out* of regions (a full-screen recents backdrop is arguably
    not something to glassify).
- **M3 — LSPosed add-on. PROTOTYPE (`companion/`).** Regions *created* rather
  than restyled. M1 measured that stock A16 produces exactly **one** blur region
  ever, but A17 on alioth already makes every notification card a region, so on
  A17 the add-on is for what Android never blurs: Quick Settings tiles,
  launcher folders, themed app icons (crDroid's "Minimal" style) and home
  screen widgets work on the A17 AVD, and so does a lighter shade scrim; notification cards and heads-ups
  are built against crDroid's SystemUI and wait for LSPosed on alioth.
  Full-colour icons are not built. See "The companion" below. Region inflation
  for effects that draw outside their bounds is still open.

## Glass is a mode, not a replacement

**Decision (v0.3): Android's blur is the default, and Liquid Glass is a mode the
user switches on.** Before v0.3, loading the engine meant every blur region was
glass. Now one property decides, live:

```
persist.sys.glass.mode    glass  → draw the glass over the regions Android produces
                          other  → call the original drawBlurRegion, untouched
```

Anything other than an explicit `glass` (or `on`) is stock, so a typo, an empty
value or a name from a later version can only ever produce Android's blur. The
old `bypass=1` still forces stock.

Why a runtime switch rather than "install = on":

- **It is the honest framing of what the module is.** The compositor already
  decided where to blur and how much; glass is a way of drawing that decision,
  and a user should be able to compare the two without a reboot.
- **It is a safety valve on new ROMs.** Stock mode puts nothing of ours in the
  path except the trampoline: the shader is not even compiled until glass is
  first selected. Someone who switches back because glass misbehaves on their
  build gets measurably stock output (VERIFIED.md), not "glass with the effect
  turned down".
- **The hook stays installed in stock mode** so switching is instant. The cost is
  one jump and a property-serial check per blur region per frame. `enabled=0`
  remains for not installing the hook at all, at the next boot.

### Parameters are layered: override → preset → default

```
persist.sys.glass.<name>     per-setting override (glassctl set, WebUI sliders)
persist.sys.glass.preset     clear | regular | frosted | prism | edge
kSpecs in glass_params.cpp   the calibrated defaults
```

Presets live **in the engine**, not in a script that writes properties. A preset
is then one property, switching is atomic, an override on top of a preset is
visible as an override, and a preset can be selected with nothing but `setprop`.
A preset lists only what it changes; the frost *colour* is in none of them,
because glass-theme.sh owns it.

### One source of truth for the UIs

The WebUI and `glassctl` never hold a copy of the presets or the defaults.
`glasscheck --params` is compiled from the same `glass_params.cpp` as the engine
and prints the tables plus the live values as JSON; `glassctl json` wraps it with
what only the module knows (is the engine loaded, is theme-following on); the
WebUI renders that. Slider *ranges* are the only UI-owned numbers.

```
WebUI ──ksu.exec──▶ glassctl ──▶ setprop persist.sys.glass.*  ──▶ engine (≤ 500 ms)
Action button ────▶ glassctl ──▶ glasscheck --params ◀── glass_params.cpp ──▶ engine
```

`glassctl` is the only place that knows which properties are switches and which
are tuning, so `reset` and `preset` behave the same from every entry point.

## Backdrops and panes (v0.4)

**Decision: what a region is decides what it gets.** A region whose on-screen
rect spans the display (90% of each side, `backdropCoverage`) is a *backdrop*:
the notification shade, a dialog's blur-behind. Anything smaller is a *pane*: a
notification card, a piece of the volume panel, a blurred dialog. Panes get
Liquid Glass; the backdrop gets `stock`, `tone` or `glass` (`backdrop`, which a
preset may set).

Why geometry rather than anything SurfaceFlinger labels: `drawBlurRegion` is
called the same way for a layer's background blur and for its blur regions, and
the layer's name never reaches it. Geometry is also what the user means. On
alioth the shade is one full-screen background blur, and each card and each
piece of the volume panel is a separate region (VERIFIED.md), so the split
falls exactly where "shade Android blur, controls liquid" puts it. The display
size is the size of `input`, the snapshot of the whole output buffer, so a
screenshot or a rotated buffer is judged against itself.

Three consequences shaped the code:

1. **A toned backdrop is the glass shader with the lens off**: no refraction,
   dispersion, light, grain or rim, the blurred child everywhere, plus
   saturation and a veil in the frost colour, which follows the theme. No second
   shader to compile or keep in step.
2. **A lighter backdrop blur needs the filter's own arguments.** `drawBlurRegion`
   is handed the finished blur but not the GPU context or display it was made
   with. On Android 17 the engine also hooks `generateTemporaryImage` (all three
   filters) purely to remember them, and when a toned backdrop wants
   `backdropBlur < 1` it asks the same filter for the same rect at the lower
   radius. Only when the last generate call was for this very input and rect, so
   the remembered context is the one in use. Elsewhere the backdrop keeps the
   requested radius.
3. **A pane over a backdrop looks through the backdrop.** RenderEngine snapshots
   a layer's input once, before its background blur is drawn, and every region
   in that layer samples the same snapshot. Clear glass on a notification card
   would then show the sharp home screen inside a blurred shade. So the engine
   remembers the last backdrop (input image ID, blur rect, zoom, the blur it was
   drawn with) and a pane in the same layer uses that blur as its "sharp" child
   and composes the backdrop's tone under its own: saturations multiply, veils of
   one colour combine as 1 − (1 − a)(1 − b).
4. **Pieces of one surface are one pane** (`region_merge.h`). crDroid's volume
   panel is a rounded cap and a square-topped body that overlap. Regions that
   share one axis's extent and overlap along the other are outlined as their
   union, and each draws only its own part. `drawBlurRegion` sees one region at
   a time, so the cap, drawn first, learns about the body from a short memory
   of earlier frames. That memory is keyed by each pane's slot, its place in
   the layer's draw order. A pane that has moved since the last frame borrows
   nothing from earlier frames. Without that rule (0.4.2–0.4.6) a moving pane
   merged with its own earlier positions and trailed a squared-off smear
   (VERIFIED.md, v0.4.7).

What the compositor cannot do: Quick Settings tiles, launcher folders and icons
are not blurred by Android at all, so they never reach `drawBlurRegion`. Those
need an app-side module that makes them regions: "The companion" below.

## What a frame costs: blur made to order

**Decision: the engine makes the blur it draws, and only that.** Glass used to
be drawn on top of everything Android had already done for a blur layer. Per
layer, per frame, RenderEngine takes a snapshot of the output, blurs the
layer's whole bounds once for its background blur and once more for every
other radius its regions ask for (each a 4x downsample, up to four halvings
and as many upsamples), and hands each result to `drawBlurRegion`. Measured
on the A17 AVD with a real GPU (VERIFIED.md), a shade with tiles and cards
cost 7.3 ms of RenderEngine time a frame as glass against 5.0 as Android's
blur, and four things made the difference:

- **Blurs nobody drew.** A toned backdrop wants `backdropBlur` of the radius,
  a pane `fillBlurScale` of it. The engine asked the filter again and left
  Android's result unused: the default look blurred the shade three times a
  frame (77 px unused, 27 px for the backdrop, the tiles' own), four with
  notification cards.
- **A copy of the framebuffer.** Skia will not sample a surface while drawing
  into it, so a draw that names the snapshot makes it copy the whole output
  first (`SkImage_Ganesh::ProxyChooser`). Android's blur reads the snapshot
  only into other surfaces and skips that copy; the glass shader named the
  snapshot in every draw, a toned backdrop's included, which never reads it.
- **The lens on a backdrop.** A toned backdrop ran the whole glass shader over
  the full screen (the SDF gradient, three normalizes, two `pow`) to add
  nothing.
- **Fine reads of a blur.** A pane over a backdrop read the backdrop's
  quarter-resolution blur nine times per pixel to soften it by 2 dp, and once
  per pixel along the dispersion.

What the engine does about each:

1. **The blur is put off until it is drawn** (`generateHook`, Android 17).
   While glass is on, the hook on `generateTemporaryImage` answers in the
   filter's place: it hands RenderEngine the snapshot back, untouched.
   `drawBlurRegion` then sees a "blurred" image that is the input, and makes
   what it is about to draw, at the radius it draws it at (`blurCached`, one
   per source and radius per frame). Whenever Android does the drawing (stock
   mode, a backdrop left to Android, any region handed back) the blur Android
   asked for is made first (`drawWithAndroidsBlur`). RenderEngine draws a
   region with every blur straight after asking for it; if a build ever asks
   and does not draw, the engine notices at the next request and stops
   putting blurs off until SurfaceFlinger restarts. `lazyBlur=0` is the old
   behaviour.
2. **A toned backdrop has a shader of its own** (`glass_tone.sksl`): one
   child, one read, the same SDF. Line for line it is the glass shader with
   every lens term at zero, and the two draw the same pixels (VERIFIED.md).
   It does not name the snapshot, so nothing is copied for it. While the blur
   is still fading in, the glass shader draws the backdrop as before, since
   that cross-fade does read the snapshot. `toneShader=0` turns it off.
3. **A pane over a backdrop makes its blur from the backdrop's** (`paneBlur`).
   Nothing behind such a pane is sharper than the backdrop's blur, so its own
   blur at radius R is the backdrop's (radius B) blurred by sqrt(R^2 - B^2)
   more: the filter run on a quarter-resolution image, a sixteenth of the
   pixels, sampled at 16 px to the texel. Nothing reads the snapshot after the
   backdrop is drawn, so there is no copy. `own` is the old path (a second
   full blur and the copy); `through` makes nothing and uses the backdrop's
   blur for both reads, which is cheaper still and visibly less frosted. The
   shader reads a blurred see-through child coarsely: four softening taps for
   nine (none once the backdrop is blurred at three times `softenDp`), one
   dispersion tap per dp for one per pixel.
4. **A pane under another layer's backdrop reads a light blur**
   (`coveredPanes`, `composite_memory.h`). With the shade open over a home
   screen of glass icons, the launcher's panes were drawn in full, snapshot
   copy included, and then blurred by the shade. The engine remembers which
   layers the last two composites blurred, in order, and which drew a
   backdrop over the whole output; a pane with no backdrop of its own, in a
   layer that had one above it both times, reads a blur of the snapshot at
   half that backdrop's radius where it read the snapshot. That grows the
   backdrop's own blur of it by under 12% and is the path of 3 from there. A
   wrong guess lasts one composite: the frame after a shade vanishes without
   animating shows what is behind the icons' glass blurred by a few pixels.
   Two composites, not one, because outputs take turns: while one app is
   recorded or cast at the display's size, a composite without the shade
   runs between every two of the display's, and with a memory of one each
   would be taken for what the other follows, on every frame.
5. **Most of a large pane takes a short way through the shader** (`deep`).
   Past the refraction band, the rim light and the sheen (`deepBand`) a
   fragment is undisplaced with t = 1: no gradient, normalize, `pow` or
   dispersion. And the interior no longer reads the blurred child twice.
   That is what the text of the shader says. On the Poco F1 a pane with its
   band a third as wide cost what it did before (VERIFIED.md, v0.4.20), which
   is where the lean builds below come from.
6. **A pane outside the layer's visible bounds draws nothing**: a Quick
   Settings page not swiped to, launcher pages either side. It keeps its place
   in the merge memory and costs no blur and no shader.

What is left is the copy for panes that do show the snapshot: home screen
icons, a keyboard, the volume panel. On the AVD's GPU it is the whole
difference between such a pane and Android's blur of the same region (about
0.5 ms a frame; with a child that is not the snapshot the glass costs what
stock does).

7. **That copy can be of the panes' bounds only** (`snapshotCopy=bounds`,
   `sharp_bounds.h`; off by default). Skia copies the whole output because a
   draw names the snapshot. If the engine copies the part the panes read
   itself, before anything is drawn into the output
   (`SkImage_Base::makeSubset` through `SkCanvas::baseRecorder`, both
   optional, Android 17), and the panes read that copy, nothing names the
   snapshot and the full copy is never made. The first pane of a layer has to
   decide before the others arrive, so the copy covers where the layer's
   panes read in the last frame, grown by 32 px, and is made only when they
   stood still for two frames and together read under 60% of the output: a
   keyboard, a volume panel, a toast. A pane outside the copy reads the
   snapshot, as before. On the AVD it draws the same pixels and brings a
   volume panel to what Android's blur costs. It is not the default because
   the copy is an unbudgeted texture, allocated and freed every frame, and
   what that costs on a phone's driver has not been measured; and it does
   nothing for a home screen, whose icons read most of the output between
   them.

8. **A pane is drawn by the leanest build of the shader that fits it**
   (`leanShader`, v0.4.20; `effectFor`). The glass shader decides what a pane
   does from its uniforms: no fringe when `dispersionAmount` is 0, no grain,
   no press effect, a circular corner when its exponent is 2, a single read
   where there is no softening. The generator builds it twice more with those
   parts left out of the text (`lean`, and `lean_soft` which keeps the
   softening), each line that is left being the full shader's line with the
   zero put in, and the tone shader once more without its superellipse
   corner. The glass builds declare the full shader's uniforms, used or not,
   so one uniform block and one pair of children serve all three; the engine
   checks at compile time that this Skia lays them out alike and otherwise
   does not use the build. At the default settings every pane is drawn by one
   of them. A fourth build, `deep`, is `lean` with no lens at all and draws
   no glass: `debugPane=3` puts it in for measuring. The picture is the same to within one level
   in 255 on under 1% of pixels (two programs round differently), and a few
   pixels on the diagonal of a square corner, where two edges are equally
   near, fall to the other edge. On a phone it is the largest single gain
   so far: a Quick Settings frame on the F1 (Adreno 630, one layer, fringe
   off) went from 15.3 ms to 10.6 over the lock screen and from 24.6 to
   14.1 with the launcher's icons under the shade, where Android's blur is
   7.6 (VERIFIED.md, "v0.4.20 on both phones"). The full shader never ran
   the left-out code for those panes, and the GPU charged for it anyway.

9. **Three settings change the look** (the WebUI's Performance card,
   `glassctl perf`). Everything above draws what was drawn before. These do
   not. Two layers and full resolution, the best-looking values, are the
   defaults; the colour fringe is off unless asked for.
   - *Blur layers: one* (`paneBlur=through`). A pane mixes two pictures of
     what is behind it, the screen and a blur of it. With one layer it reads
     a single image for both. Over a backdrop that is the backdrop's blur, and
     nothing more is made for the pane. With nothing blurred under it, it is
     one blur of the snapshot, made before anything is drawn into the output,
     so no copy either. That one blur stands for a mix of two pictures, and
     its radius (`chosenViewRadius`) follows which of them the mix is closer
     to: mostly see-through (clear glass, a blur share of 0.3 or less) gets
     the lightest blur a quarter-resolution image holds still at, 4.5 dp,
     because only that keeps what is behind readable; mostly blur (0.9 and
     up, frosted) gets the pane's own blur, which is the same picture;
     between them a smoothstep.
   - *Edge colour fringe* (`fringe`, 0 since v0.4.20: it cost 11 ms a frame
     on the F1's home screen for a detail a few pixels wide). On, the rim
     walks up to sixteen reads a pixel along the refraction; off it is one
     read. `prism` sets it for itself, and a value set by hand wins over the
     preset either way. In the presets that leave `lensBlur` automatic
     (`regular`, `frosted`, `prism`, `edge`) the lens's own reads are mixed
     away entirely once a pane's blur reaches `lensBlurRef` (12), the fringe
     with them, so on such a pane the walk was paid for and never seen.
   - *See-through resolution: quarter* (`glassResolution=quarter`). With two
     layers, a pane with nothing blurred under it reads that lightest blur in
     the screen's place and makes its own blur from the same image, as a pane
     over a backdrop does: no copy, coarse reads.

   A choice waits for the pane's blur to have faded in: until then the shader
   cross-fades the sharp screen in, as stock does, and a blur that came on at
   once would pop.

`persist.sys.glass.stats=1` logs, once a second, what was made and drawn per
frame (blurs by who made them, snapshot copies, panes by path).
`harness/bench.sh` and `harness/ab.sh` time fixed scenes with it
(`BenchActivity`: a backdrop that scrolls every frame under a window of blur
regions laid out from the region maps measured on alioth), and
`harness/shots.sh` with `shotdiff.py` compare what two engines draw.

## The module (Track B, shipping form)

`module/` builds a flashable KernelSU / Magisk module, ~3.4 MB.

```
customize.sh      refuses to install on a ROM the engine cannot drive;
                  on an update, swaps running files out by rename
post-fs-data.sh   builds the overlay, owns the bootloop guard
service.sh        disarms the guard once sys.boot_completed lands
glassctl          mode, presets, tuning; the one script every UI calls
action.sh         the manager's Action button: glass ↔ Android blur
webroot/          KernelSU / APatch WebUI
bin/<abi>/        patchelf + glasscheck, both built for Android
libs/<abi>/       libglassengine.so
```

`module/tools/webui_preview.py` serves the WebUI in a desktop browser with a
`ksu.exec` stand-in that runs each command over `adb shell`, so the page can be
iterated on against a real device's glassctl and engine.

Three decisions worth keeping:

1. **Injection is a `DT_NEEDED` entry on a copy**, mounted over the original.
   `LD_PRELOAD` is not available — surfaceflinger runs with `AT_SECURE=1`, so
   bionic strips it. See VERIFIED.md.
2. **The preflight is the same resolver the engine uses.** `glasscheck` runs at
   install time against the device's own `surfaceflinger` and aborts with a list
   of what is missing. An incompatible ROM costs an error message, not a boot.
3. **The guard lives in the scripts, not the engine.** It has to, because the
   failure it exists for happens in the linker before any engine code runs.
4. **An update never rewrites a file the running system maps.** Under
   meta-overlayfs the metamodule copies `system/` with `cp -af` into the live
   overlay's lower directory, which truncates `libglassengine.so` under
   SurfaceFlinger and takes its relocated pages with it (it crashed, 0.4.9 over
   0.4.8). `customize.sh` first gives each live path a new file by rename, so
   the running copy stays intact until the reboot. See VERIFIED.md, "Updating
   over a running copy".

## Many SurfaceFlingers: one resolver, a cache, and hook shapes

The module meets a different `surfaceflinger` on every ROM: AOSP and Pixel
releases with their QPRs, LineageOS and the ROMs built on it, and OEM builds.
What changes between them is known from the builds measured: the hook target
gains a parameter (`zoomScale` at A16 QPR2), helpers get inlined or not
(`getTotalMatrix`, `invertNonIdentity`), a compiler re-conventions a function
it can see every caller of (`MakeForShader` on alioth), and Pixel's A17 QPR
line splits `drawBlurRegion` into thin wrappers around `setupBlurPaint` and
adds an `SkPath` overload. Three pieces keep that manageable:

1. **One resolver** (`engine/src/sf_resolve.*`). glasscheck at install, the
   engine at start-up and the host tests used to carry a resolution loop each,
   and the loops had drifted apart (one refused a build the other accepted).
   Now there is one pipeline: match every entry point of `sf_symbols.cpp` by
   exact mangled name, record drift (a prefix hit whose name no entry knows,
   so the other `drawBlurRegion` shape is not drift and a new one is), ask the
   caller's ABI probe about every by-value return, then `judge()` the result
   with the group rules: exactly one hook shape big enough to hook, a way to
   release the uniform blob, and everything `SkBridge::drawReady()` needs.
   The engine and the installer therefore make the same decision by
   construction, and the host test (`engine/test/run.sh`) makes it too.
2. **Hook shapes as a table** (`kHookShapes`). A new shape of the hook target
   is one row there, one entry in `sf_symbols.cpp` and one replacement
   function in the engine; exactly-one, the size test and the cache follow
   from the table, and a `static_assert` keeps the replacements in step.
   There are three: `(radius, alpha)` up to A16 QPR0, `(radius, scale, alpha)`
   from QPR2, and Evolution X 12.x's `(radius, scale, alpha, material)`. EvoX added
   a glass of its own to RenderEngine on its Android 17 branch
   (`Evolution-X/frameworks_native`, `cnb`, "renderengine: Draw blur regions as
   Liquid Glass", written 2026-10-05, merged 2026-11-05), and crDroid took the
   same commit into its `17.0` branch on 2026-11-11, so crDroid 13.x builds
   from then on have this shape too. The extra `int` is the region's material: -1
   for a layer's own blur, 0 for the notification shade's, and 1 to 11 for
   ColorOS-style materials a client can ask for in a blur region (11 is the
   dock), which EvoX draws as its glass whatever its Liquid Glass switch says.
   The switch, `persist.sys.evolution.liquid_glass`, is off unless something
   sets it (no build.prop does), so out of the box EvoX 12.x draws Android's
   blur. The replacement draws StockGlass where AOSP would have drawn a plain
   blur (material 0 and below), leaves the materials a client chose to EvoX,
   and passes the `int` through on every call to the original. A replacement
   for the QPR2 shape would call the original with whatever w7 held by then.
   This shape is what "Flash failed" on EvoX meant: the installer refused a
   shape it did not know, whether or not EvoX's glass was on.
3. **Resolve at install, verify at boot** (`engine/src/sf_cache.*`). glasscheck
   already reads the whole binary and inflates its MiniDebugInfo at install,
   so it writes down what it found: `libglassengine.symcache` next to the
   engine in the overlay, keyed by the binary's GNU build ID and the
   entry-point table's hash, with the first 16 code bytes of every entry. At
   start the engine reads a few kilobytes and compares those bytes with the
   code in memory instead of resolving (0.3 s before SurfaceFlinger's main on
   the F1). Anything wrong with the cache (another build ID, another table, a
   byte that differs, truncation) means resolving as before: a cache can cost
   time, never correctness. It is also the way in for builds without symbols:
   an address found at install by other means is handed over the same way,
   marked `scan`, so the clever part runs where a mistake costs a declined
   install and can be tested on the host, and the engine only ever runs the
   cheap, strict check.

Two smaller tables follow the same pattern. The blur filters whose
`generateTemporaryImage` the engine hooks for `backdropBlur` are rows of
`kGenerateFilters`: AOSP's three, and Project Infinity-X's `GlassBlurFilter`,
which replaces all of AOSP's Kawase filters on that ROM (Infinity-X 4.x, A17;
its source credits AxionOS). The engine instantiates one `generateHook<K>` per
row, so a ROM's own filter with AOSP's signature is one mangling and one row.
And the uniform layout has a fallback for builds that inline `findUniform`
away (see "Shader construction" above).

**Graphite, asked of the canvas.** `install()` stands down when
`debug.renderengine.backend` or `debug.renderengine.graphite` says Graphite,
but those properties are not the only way there. Lineage 24's Pixel 8 build
sets `debug.renderengine.graphite_preview_optin=true` from the vendor
partition, and RenderEngine then follows a server-side aconfig rollout flag
(`graphite_renderengine_preview_rollout`): one flag flip moves it to Graphite
with nothing for `install()` to see. So the draw hook also asks the canvas, at
every blur until the answer is yes: `SkCanvas::recorder()` returns the top
device's Graphite `Recorder`, which only a Graphite device has. Yes means
standing down for the life of the process (`sys.glass.graphite=1`, shown by
`glassctl status`), before the shader is ever compiled, so no blur is ever put
off there either. A canvas that wraps another (a Skia capture) answers null,
which can cost the check but never the glass.

`glasscheck --json` prints the whole resolution for tooling,
`--cache-out FILE` writes the cache and `--verify-cache FILE` runs the
engine's check against the file. The firmware survey that drove this lives
in `captures/sf-corpus/` (not in git: ROM binaries); its reports are
summarised in VERIFIED.md.

## The companion: regions for what Android never blurs (M3)

`companion/` is an LSPosed module (legacy Xposed API 82, built without Gradle
like the harness). The engine can only restyle blur regions that exist, so the
companion makes them: each control gets a `BackgroundBlurDrawable` from its
window's `ViewRootImpl`, the object SystemUI uses for notification cards, drawn
at the control's bounds, and the control's own fill turns translucent so the
glass under it shows. Drawing that drawable records an empty RenderNode;
RenderThread reports where it lands every frame, and the window's aggregator
sends rect, corner radii and blur radius with the same frame's buffer. A region
therefore follows scrolling and animation exactly, with no position bookkeeping
in the module, and the engine draws it as a pane.

### Hooks on Android 17

| surface | process | before-hook | what it changes |
|---|---|---|---|
| QS tiles | `com.android.systemui` | `…qs.panels.ui.compose.infinitegrid.TileKt.TileExpandable` (a static composable) | wraps `color: () -> Color`, alpha × `app.fill` (neutral fill) or × `app.accent` (coloured: an active tile); adds a `drawBehind` to `modifier` that draws the tile's region with the corner radii of its `Shape`. With `app.tint` > 0 an active tile's region asks the engine for glass tinted in the tile's own colour (the v0.4.6 corner-radius encoding, `module/README.md` "Tinted glass for apps"), at `app.tint` (0.6) times how far its colour is from neutral, and its fill keeps `app.tilecolor` (0.5) over it, so an active tile is about 80% its colour and the glass shows through. Much less loses a dark-theme tile's dark icon over the dark shade (VERIFIED.md). The shape's radii are read on every draw: A17 animates a tile's corners inside one remembered `RoundedCornerShape`, so a cache by shape and size kept the corners the tile had before it was turned on or off. An after-hook on `…qs.composefragment.QSFragmentCompose.setFancyClipping` records the rounded rect the notification stack covers while the shade opens and closes, which the QS root view clips out of its drawing; a tile whose region overlaps it is faded to 0. On Google's build a dual-target large tile shows "on" in its icon square only, which is not a region |
| folder icons | `com.android.launcher3`, `com.google.android.apps.nexuslauncher` | `com.android.launcher3.folder.PreviewBackground.drawBackground(Canvas)` | `mBgColor` alpha × `app.fill`; region at the folder shape's bounds (`basePreviewOffsetX/Y`, `getScaledRadius()`), corners from the theme's folder shape |
| themed app icons | the same two | `…launcher3.icons.FastBitmapDrawable.draw(Canvas)`, then `…icons.mono.ThemedIconDelegate.drawContent(BitmapInfo, IconShape, Canvas, Rect, Paint)` | the plate's `Paint` alpha × `app.fill`, glyph untouched; region at the theme's icon shape in the drawable's bounds (`IconShape.path`/`pathSize`, scaled by `IconNormalizer.ICON_VISIBLE_AREA_FACTOR` as `drawContent` does), corners measured from the path; home screen and dock under `app.icons`, the app drawer's grid, predictions and search results (`mDisplay` 1, 8, 9) under `app.drawer` |
| home screen widgets | the same two | `com.android.launcher3.CellLayout.dispatchDraw(Canvas)`, which gives each `…launcher3.widget.LauncherAppWidgetHostView` on the page a drawable in its `ViewOverlay` | the plate's alpha × `app.fill`, or × `app.contrast` where the widget's text would be lost; the plate is the view the launcher rounds (`RoundedCornerEnforcement.findBackground`); region at the launcher's enforced rect and radius, else the plate's outline, else `system_app_widget_background_radius`; workspace only |
| shade scrims | `com.android.systemui` | the `UNLOCKED` and `SHADE_LOCKED` `…statusbar.phone.ScrimState`s' `setShadePanelColor(int)` / `setNotificationScrimColor(int)` | the colour's alpha × `app.scrim`; with `app.scrimcolor` black, white or tint the behind scrim takes that colour and the notifications scrim is cleared, so the shade is one colour; `ScrimController.updateThemeColors()` and `applyAndDispatchState()` re-apply it on a switch |
| notification cards | `com.android.systemui` | `com.android.internal.graphics.drawable.BackgroundBlurDrawable.setColor(int)` when its callback is a `…notification.row.NotificationBackgroundView` | the card fill's alpha × `app.cards`; the region is the framework's own |
| heads-up notifications | `com.android.systemui` | after-hook on `…notification.row.ExpandableNotificationRow.usesTransparentBackground()` | true instead of false while a heads-up must stay on screen, when row transparency is on and not on the keyguard; LSPosed only |
| QS brightness slider, crDroid's volume slider | `com.android.systemui` | `androidx.compose.material3.SliderDefaults.Track-mnvyFg4` (an instance method of `SliderDefaults.INSTANCE`) | `colors` (a `SliderColors`) remade with the active track's alpha × `app.accent` and the inactive track's × `app.fill`, thumb and ticks unchanged; a `drawBehind` on `modifier` draws a region at the track's bounds with its outer corner radius (the `cornerDp` argument); only a track in the shade's window (`TYPE_NOTIFICATION_SHADE`), not the volume or brightness dialogs |
| text selection toolbar (Cut, Copy, Paste) | `com.android.systemui` | `…selectiontoolbar.app.ui.RemoteSelectionToolbar.show(ShowInfo)` | `ViewGlass` on its public `contentContainer` (the framework's `floating_popup_container`), shadow off; every app's toolbar, with no app in scope (below) |
| text toasts | `com.android.systemui` | `android.widget.ToastPresenter.show(View, IBinder, IBinder, int, int, int, int, float, float, ITransientNotificationCallback, boolean)` (the framework class, in SystemUI's process) | `ViewGlass` on the toast's root (`text_toast`, background `toast_frame`), shadow off; only views holding SystemUI's `R.id.text`; the plate keeps `app.fill`, or `app.contrast` when its tone is not the theme's; every app's text toast, with no app in scope (below) |
| screenshot action bar | `com.android.systemui:screenshot` | `…screenshot.ScreenshotShelfViewProxy$prepareEntranceAnimation$1.onPreDraw()` | `ViewGlass` on `ScreenshotShelfView.actionsContainerBackground`, shadow off |
| drawer search box | the two launchers | `…allapps.search.AppsSearchContainerLayout.onLayout` (crDroid), `…nexuslauncher.allapps.UniversalSearchInputView.onLayout` (Pixel) | `ViewGlass` on the view; it follows the launcher's own pill alpha (Pixel hides the pill while search is open) and crDroid's new background on every layout |
| dock search bar, crDroid | `com.android.launcher3` | `com.android.launcher3.qsb.QsbLayout.onMeasure` | `ViewGlass` on the accent ring (`outer`), else the pill (`inner`); the pill and the Gemini circle inside the ring get only the fill cut; fades as the drawer opens |
| dock search bar, Pixel | `com.google.android.apps.nexuslauncher` | the widget hook above: the bar is an app widget (`OseWidgetView`, Google's RemoteViews) that `Hotseat` holds directly | the widget path, under `app.search`; its pill is the full-width images in the RemoteViews (two `GradientDrawable` images), with their own corner radius |
| lockscreen clock | `com.android.systemui` | `…customization.clocks.view.DigitalClockTextView.onLayout` and `…shared.clocks.view.FlexClockViewGroup.onLayout` (the flex clock, both builds); crDroid's `…systemui.clocks.ClockStyle.applyClockAlpha()` (its own clock styles); `com.android.keyguard.ClockEventController.setClock(ClockController)` (clock faces from plugins, both builds) | no region: a `RenderEffect` on the clock's view (the large clock's digit group, the small clock's text, each `TextClock` of a crDroid style whose digits are at least 24 dp tall, a plugin face's view) draws the glyphs as glass refracting a capture of the screen behind them (below); shade window only, never while dozing |

A17's Quick Settings are Compose only, in crDroid 13.1 and in the AVD's
SystemUIGoogle alike: `QSTileViewImpl`, `QSPanel` and `TileLayout` are gone, so
there is no tile View to hook. `TileExpandable` passes its colour lambda to
`rememberExpandableController`, and `Expandable`'s `animatedBackground` reads it
at draw time, so the one hook covers fill and bounds. Its parameters are matched
by type, because crDroid's has an extra boolean. Compose is reached by
reflection into SystemUI's own R8-processed copy (names are kept, and each one
was checked in both builds), with Kotlin lambdas made as `Proxy`s of the host's
`Function0`/`Function1`.

Themed icons are the one kind of launcher icon whose plate is drawn at draw
time. A full-colour icon is a cached bitmap with its background baked in (see
"Not built"), but `ThemedIconDelegate.drawContent` draws the shape's shadow
layer and then the plate with the `Paint` it is given, clipped to the theme's
icon shape, and only then the tinted monochrome glyph with its own paint. So a
translucent copy of that `Paint` fades the plate and its shadow and leaves the
glyph. crDroid's ThemePicker "Minimal" app icon style
(`ThemePickerIconStyle.MONOCHROME`) sets the launcher's themed-icons preference,
so on crDroid it is the same code path; this is read from the source and not
yet run there. `drawContent` does not know which view it draws for, and a region
needs the view's window: the hook on `FastBitmapDrawable.draw`, its caller,
notes the drawable and canvas, and `drawContent` pairs with it only if both
match. The drawable's callback is the `BubbleTextView`, and one displaying on
the workspace (`mDisplay` 0: home screen and dock) or in the app drawer (1, 8,
9: `displayIsAppDrawer()`) gets glass. crDroid themes the drawer's icons when its
drawer setting asks for themed icons (`mThemeAllAppsIcons`); the Pixel launcher
does not. The drawer's grid scrolls under its search box and off its bottom
edge, where the RecyclerView clips the icon but not the region, so a drawer
icon's region fades by the share of its plate (`getIconBounds`) that
`getLocalVisibleRect` still shows. It keeps the punch: under a plate it clears
the drawer's sheet, so the plate is a window onto the glass rather than glass
veiled by the sheet. Folder previews, the open folder, drag views and software
snapshots stay stock, and so does an unpinned dock prediction
(`PredictedAppIcon`), which paints its own plate and ring over the icon.

A widget is a RemoteViews tree that the launcher only hosts, and neither
launcher's host view declares a draw method, so the hook is on the page:
`CellLayout.dispatchDraw` gives each widget host on it a drawable in the host's
`ViewOverlay`. The overlay draws with the host's canvas after the widget, so
the region follows page scrolls, the spring-loaded shrink and reorder
animations, and it does not punch, which would erase the widget drawn under
it. The plate is the view the launcher itself rounds
(`RoundedCornerEnforcement.findBackground`: `@android:id/background`, else the
first view that draws), and only its alpha changes; text, images and the
widget's other backgrounds keep theirs. A RemoteViews update can replace the
plate without the launcher redrawing anything, so a pre-draw check redraws the
overlay when the widget's content, plate or plate position changes. The
widget picker's previews and a dragged widget stay stock.

A widget's text is drawn for its own plate, so with less plate the glass can
swallow it: dark text on a light plate over a dark wallpaper (the Calendar
month widget in the AVD's light theme), light text on a dark plate over a light
one. Such a widget keeps `app.contrast` (0.65) of its plate instead of
`app.fill` (0.3). The plate's tone is the mean colour of a copy drawn at 8×8
(a selector's current child), read before its alpha first changes; the
wallpaper's is Android's own (`WallpaperColors` hints: dark below a mean
luminance of 0.3, light above 0.7), followed through
`WallpaperManager.addOnColorsChangedListener`. Dark text is guarded on a dark
wallpaper only, light text on any wallpaper Android does not call dark: white
text loses its contrast well before a wallpaper counts as light. A plate that
cannot be read is taken to follow the system theme, as widgets usually do.

`ViewGlass` covers the rest: a view whose own background is the control's
plate (a popup's container, a search box). A drawable in the view's
`ViewOverlay` draws the region at the view's outline (rect and radius) and the
background keeps `app.fill` of its alpha; text and icons are untouched. The
overlay is drawn after the background, so the alpha is set in a pre-draw
listener instead, after the host's animations and before the background is
drawn. Each time the host sets the background's alpha itself (Pixel's drawer
pill fades to 0 while search is open), that becomes the background's own, the
fill is taken of it, and the region follows: full strength at the highest alpha
seen, gone at 0. `shadowless()` also clears the view's outline shadow colours,
since the shadow is drawn under the whole outline and showed through a
translucent background: the toolbar's glass came out darker than its stock
fill until the shadow was off. A view added to a new
window (the screenshot UI, each time) gets its listeners there again: a tree
observer belongs to the window, not to the view, and the per-window sync in
`Region` re-registers the same way.

On A17 an app does not draw its own text selection toolbar. With
`android.permission.flags.system_selection_toolbar_enabled` (on in both builds
in hand) and the `selection_toolbar` service present, `FloatingToolbar` makes a
`RemoteFloatingToolbarPopup`: a transparent `PopupWindow` holding a
`setZOrderOnTop` SurfaceView, into which SystemUI's
`SysUiSelectionToolbarRenderService` draws the toolbar through a
`SurfaceControlViewHost`. So every app's toolbar is a view tree in SystemUI's
process, already in scope, and its region is on the SCVH's layer
(`blurRegions.size()=1` there). An app on a build without that flag, or with an
old target, uses `LocalFloatingToolbarPopup` in its own process; that path is
not hooked, and would need every app in the module's scope.

Since Android 12, apps do not draw their text toasts either: the toast goes
to NotificationManagerService, which asks SystemUI's `ToastUI` to show it.
`ToastUI` builds a `SystemUIToast` (SystemUI's `text_toast` layout: the app's
icon and the text on the framework's `toast_frame`, a rounded plate in the
theme's surface colour with 2 dp of elevation) and passes its view to a new
`ToastPresenter`, whose `show()` adds it as a window of its own (`TYPE_TOAST`,
titled `Toast`); `hide()` removes the window after the exit animation. So the
region is in the toast's own window and goes with it: SurfaceFlinger has no
layer left to draw it on. The region follows the view's alpha and scale through
`SystemUIToast`'s entrance and exit. A toast floats over an app SystemUI cannot
see, so for the contrast rule (widgets, above) its backdrop is taken to be in
the system theme, as apps usually are: A17's plate follows the theme and keeps
`app.fill`; a plate whose tone differs from the theme (a ROM's fixed toast
style) keeps `app.contrast`. A `ToastPlugin`'s view and custom-view toasts are
left alone; the latter are deprecated and drawn in the app's own process.

The screenshot UI runs in SystemUI's second process,
`com.android.systemui:screenshot`, which LSPosed also loads the module into;
`Entry` routes it to the screenshot hook only. Its window is added for each
screenshot and removed on dismissal; the pill starts GONE and its overlay draws
once it is shown, and the region follows its alpha and slide through the
entrance and the dismissal.

The brightness slider is a Material 3 `Slider` whose track SystemUI draws
through `SliderDefaults.Track-mnvyFg4`, as the tiles draw through
`TileExpandable`: one before-hook remakes the colours and adds the
`drawBehind`. Compose restarts `Track` with the arguments its body received,
which are the hook's, so the hook recognises its own modifier and colours and
never wraps or fades twice; it reads a snapshot state in the caller's
composition, so a switch recomposes the caller. The volume dialog and the
brightness dialog reach the same method from their own windows and stay stock.
One region covers the whole track, the 6 dp gaps beside the thumb included;
crDroid's auto-brightness and ringer buttons beside the sliders are separate
`IconButton`s and stay stock (Not built).

The lockscreen clock is the one control whose glass the companion draws
itself (`ClockGlass`). A region is a rounded rect, and SurfaceFlinger never sees
the glyphs; handing the engine a mask would need a file channel and sepolicy,
and could not follow the clock's per-frame animations. So it is done in
SystemUI, as Kyant0's AndroidLiquidGlass catalog does its lockscreen clock in
its app (`LockScreenContent.kt`, `SdfShader.kt`, Apache-2.0; its look and
parameters are reimplemented, no code is copied). A `RenderEffect` on the
clock's view takes whatever the view draws as the glass's shape, so the flex
clock's font weight, size and digit animations need nothing extra. The demo
reads a signed distance field from a prebaked texture; here it is rebuilt from
the glyphs on every draw. They are Gaussian-blurred, and under a straight edge
the blurred coverage is Φ(d/σ), so the logistic approximation of Φ gives the
depth d back in closed form, and the coverage's gradient gives the edge's
normal n. An AGSL shader samples the backdrop at p − k·h·n, with the demo's
profile k = circleMap(1 − d/band) (1 at the edge, 0 from `band` inwards), then
applies its 25% white fill, colour controls (brightness −0.1, contrast 0.75,
saturation 1.5) and a bevel lit from 45°. `DST_IN` with the clock's own drawing
keeps the result to the glyphs and their antialiased edges. Sizes follow the
digit height H measured with the clock's paint, in the demo's proportions:
band 0.09 H, refraction height 0.45 H.

The backdrop is the screen without the shade window, which holds the clock:
`IWindowManager.captureDisplay` with that window's layer excluded, at a quarter
of the display's resolution (the demo blurs its backdrop by 2 dp; the bilinear
upscale does about as much). Both SystemUI builds hold `READ_FRAME_BUFFER`. The
capture runs on a background thread, 25–80 ms on the AVD. It is taken once the
clock has sat on the lockscreen for 400 ms, not dozing and at a steady alpha,
and again after every doze and every wallpaper change. It is never taken while
dozing, or while the keyguard comes or goes, when it would show the app behind.
Each frame the backdrop's matrix maps the screen into the clock's own
coordinates, so the glass stays put on the wallpaper while the clock moves
(the switch between the large and small clock, the shade pulled down).
`IWindowManager.screenshotWallpaper` would have been simpler, but it crops the
wallpaper window's own layer in that layer's coordinates
(`WallpaperController.screenshotWallpaperLocked`), which need not line up with
the screen where the wallpaper is offset for the home pages (not tested); the
display capture is what lies behind the clock by construction. Plain glass, translucent white
with the same bevel and no refraction, stands in when there is nothing to
refract: a live wallpaper (anything but SystemUI's own image and gradient
wallpapers; a capture would not move), a failed capture, or `app.clock=plain`.
The clock is stock while dozing, and the effect crossfades with the clock's own
drawing through the doze transition. It is also stock outside the shade window
(the wallpaper picker's preview, which SystemUI draws too), and until the first
capture is back; then the glass fades in over 250 ms. It is not a region and
costs no composition: one blur and one shader pass over the clock's view when
its content or position changes.

Clock faces from plugins get the same glass. crDroid ships Pixel's
(`SystemUIClocks-BigNum`, `-Calligraphy`, `-Growth`, `-Inflate`, `-Metro` and
`-NumOverlap` in `/system_ext/priv-app`); SystemUI loads each through its
`PluginManager` with a class loader of its own and its own copy of the clock
view classes, so hooks on SystemUI's classes never see them.
`ClockEventController.setClock` is where SystemUI takes every new clock, from
any provider, with both faces' views made; the hook there hands each face's
view to the same code, skipping views from SystemUI's own class loader (the
flex clock, hooked as above) and analog faces (an id starting `ANALOG`: BigNum,
hands on a dial). Each face's view group draws all of its digits itself
(`DigitalClockViewGroup.onDraw` calls each digit's `draw(Canvas)`), so the group
gets the effect, and Inflate's and NumOverlap's overlapping digits make one
piece of glass. Plain glass would then make all four digits one grey shape, and
these faces are coloured per digit, so a face's own drawing tints its glass:
the shader runs twice over the same blur, once for the unlit glass and once for
its light, and the result is (glass·(1 − t) + face·t) screened with the light,
t being `app.clocktint` (default 0.5; 0 is the flex clock's clear glass). Mixing
before the light keeps the rim as bright as on clear glass, where tinting after
it dimmed the rim by the same factor; multiplying by the face's colours made
dark glass of dark fills. A face's own `dozeFraction` drives the doze
crossfade, and its digit height comes from its text paint, or, for Metro's
Lottie digits, 0.7 of its tallest digit view. Inflate's small clock measures
itself from its digits' positions before they are laid out, so its width is 0
until the next minute's re-measure, with its digits drawn outside it, where an
effect does not reach (and its date drawn under it): the companion asks for
that layout once, at once.

The notification shade needs no new regions on A17. With notification row
transparency on (`Settings.Secure notification_row_transparency`, on on
alioth), `NotificationBackgroundView.setBlurBackgroundEnabled(true)` gives each
card a `BackgroundBlurDrawable`, and the engine already draws every card as a
pane. Paint hides the panes: the drawable fills the card itself (the theme's
`surface_effect_1`, `#8a121317` in alioth's dark theme), and under the card lie
the shade's behind scrim (alpha 0.725 on alioth and the AVD, the shade panel
colour's) and notifications scrim (0.5). Together they cover about 94% of each
pane, so glass mode multiplies the scrims by `app.scrim` and the card fill by
`app.cards`. The keyguard, bouncer and AOD scrims are not touched. crDroid's
own Settings → UI → Shade style → Glass (`shade_color_style` 1) lightens the
same two scrim colours from the theme side, down to a contrast floor capped at
0.72 behind; the companion's cut applies on top of whichever style is chosen.
A heads-up has no region at all: `usesTransparentBackground()` returns false
for a row that must stay on screen, so the row drops its drawable and paints an
opaque colour over the app below. The after-hook keeps the row's own drawable,
region and translucent fill instead, and the engine draws it over the app. The
A17 AVD's SystemUIGoogle has no `setBlurBackgroundEnabled`, so there the
companion changes the scrims only.

Framework behaviours the companion changes:

- **Punch per surface.** The framework paints the drawable's shape in
  transparent `SRC`, erasing what the window drew there. Under a tile that is
  the shade's scrim, so the tile showed the unveiled home screen, darker than its
  surroundings in a light theme. QS tiles therefore clear the xfermode
  (transparent `SRC_OVER` draws nothing, and the node still reports its
  position). Launcher regions keep the punch: nothing the launcher draws belongs
  under an icon or a folder, and after some cold starts the Pixel launcher
  leaves a dock prediction (ring, plate and glyph) under each pinned dock icon,
  which the stock opaque plate hides and a translucent one showed. `app.punch=1`
  or `0` forces one way everywhere.
- **Regions follow what hides the control.** A region is not part of the view
  tree, so an ancestor's alpha or visibility does nothing to it. Before each
  frame, a pre-draw listener scales every region by its control's ancestors'
  alpha and visibility, and launcher regions also by the app drawer's position
  (`AllAppsTransitionController.mProgress`: 1 closed, 0 open). A17's launchers do
  not fade the workspace for the drawer: every ancestor of a home-screen icon
  stays visible at alpha 1 while the drawer's translucent panel slides over, and
  the glass left under it showed through as faint discs. Compose fades with
  graphics layers, whose alpha hwui applies to the layer's RenderNode and never
  to a region inside it, so QS tiles and sliders also multiply in the alpha of
  every layer from their draw node out to the root (`NodeCoordinator.layer`,
  `lastLayerAlpha`, an explicit layer's own alpha: the fields Compose reads for
  `isTransparent()`). Glass under a tile row that QS had faded to 0 stayed at
  full strength before (VERIFIED.md). The window's blur aggregator copies its
  regions in a pre-draw listener of its own, registered when the window got its
  first region, so before the companion's: what the sync changed reached
  SurfaceFlinger a frame late, and never if the window stopped drawing straight
  after (the last frame of a fade). The companion moves the aggregator's
  listener behind its own when it starts watching a window, so the change goes
  with the same frame. A change made while drawing still reaches SurfaceFlinger
  a frame later, so a region that changes asks for that frame.
- **Moves on RenderThread alone ask for a UI frame.** hwui reports a region's
  position on every frame it renders, but the aggregator only sends regions from
  frames the UI thread drew. Each region also listens to its node's position
  (`RenderNode.addPositionUpdateListener`), and a move on a frame no pre-draw
  started asks the window for one (`app.rtfollow`, default on). A17 has no
  RenderThread backend for `View.animate()`, so this has not caught a case on
  the AVD; it is a guard.
- **A region's rect is held to what hwui last reported.** The aggregator
  queues each position hwui reports by the frame number of the window's
  surface and applies them in number order with the frame it sends. It sends
  none while no region is above alpha 0 (overview and app launches fade the
  launcher's pages), so moves pile up; when the window stops, hwui reports
  every region lost as frame 0, and a window that starts again draws into a
  new surface whose frames count from 1. The old moves then sort after
  everything the new surface reports and come back once its count passes
  theirs: a home screen page drawn as glass over another at the scale overview
  left it at, a loss stuck behind the new numbers, or glass under the dock a
  page width off. hwui reports a node again only when it moves, so nothing
  corrected those. Two parts:
  - Before each frame, the sync applies any queued move numbered past the
    surface's next frame (only an earlier surface can have numbered it), in
    order, so the losses and the new surface's positions land after them
    (`Region.replayOldSurface`; it logs `applied N region moves from frames
    A-B of an earlier surface`). Looking only on a new surface's first frame
    was not enough (VERIFIED.md).
  - The region's own position listener (above) also keeps where hwui last put
    the node, or that it lost it, in the order hwui reports them; it is moved
    ahead of the drawable's own listener, so it has each move before the
    aggregator does. 300 ms after a window draws, and every 300 ms while it
    keeps drawing, if the aggregator has applied everything it queued, a region
    whose rect differs from that record gets the record's rect, and the next
    frame sends it (`Region.sweep`; it logs `put N region rects ... back`).
    That catches a misordering whatever its cause, for a field read per region
    per sweep. A per-frame check of whether each control is on screen
    (`getGlobalVisibleRect`) did the same job for stale glass at about 3 ms of
    the launcher's UI thread per frame on the AVD, and could not correct glass
    left on a control that is on screen.
- **Switches are pushed, not only pulled.** A control re-reads the switches when
  it is drawn, and a host does not redraw what it has no reason to. Tiles in an
  open QS and a folder on an idle home screen kept their regions and translucent
  fills after a switch to stock, and stock SurfaceFlinger drew those regions as
  plain blur. On a change the companion redraws every control (tiles read a Compose
  snapshot state at draw time, which it writes; folder hosts are invalidated),
  and an idle host hears of a change through
  `SystemProperties.addChangeCallback`, which runs on `SYSPROPS_TRANSACTION`
  (`service call activity 1599295570`). `setprop` alone sends none, so
  `glassctl` should send it after every write. Until it does, a switch reaches a
  control when that control is next shown.

### What a region costs

Per layer with blur, per frame, stock SkiaRenderEngine makes one blur
*generation* per distinct radius, over the layer's whole bounds clipped to the
display, not over the region. Then it makes one `drawBlurRegion` per region,
which with the engine is one glass shader pass over that region's rect. With
glass on, the engine makes the generations itself, and fewer of them ("What a
frame costs", above). So:

- N tiles in the shade cost N small shader passes and one blur of the
  backdrop's own quarter-resolution blur (a sixteenth of the pixels of a
  generation), whatever radius they ask for: the framework's 100 dp (263 px
  on the AVD) against the shade's 89 (AVD) or 77 (alioth). Before blurs were
  made to order they cost a full-screen generation.
- Android 17 draws no blur at all for a layer once the blur requests of
  layers whose owner is not privileged pass ten (CompositionEngine's
  `Output.cpp`, `kMaxBlurRequests`, b/399120953): each region counts, and so
  does a background blur. SystemUI and the recents launcher hold permissions
  that exempt them. A third-party launcher or any app in the module's scope
  would lose every region on screen with the eleventh.
- Regions force GPU composition. The open shade is GPU-composited anyway, but the
  home screen otherwise need not be: a folder region costs a GPU composition on
  every launcher frame (page scrolls, app-launch animations), and it blurs only
  the wallpaper. With themed icons every icon on the home screen and dock is a
  region too: 5 on the AVD's sparse home screen, 20–30 on a full one, each one
  glass pass per composited launcher frame, and so is each widget. Not
  measured yet.
- Nothing is composed while nothing changes, so an idle QS or home screen costs
  nothing extra.
- Off-screen pager pages are still regions (QS page 2: four), one call each
  that draws nothing visible.

Measured on the A17 AVD with `companion/tools/cost.sh`, before blurs were made
to order: TimeStats RenderEngine time per composited frame over 6 QS
open/close cycles, two passes in opposite orders. This is SwiftShader, so only
the ratios carry over (VERIFIED.md has the same cycle on a real GPU, before
and after):

| | ms per frame |
|---|---|
| stock | 36–46 |
| glass, tiles off (`app.qs=0`) | 103–111 |
| glass, 14 tile regions | 128–135 |
| the same at the shade's radius (`app.radius=89`) | 127 |

The tiles add 16–31% to RenderEngine time on top of glass. Matching the shade's
radius was within the noise. A real-GPU pass on alioth is still owed. Two
cheaper designs were not built: tiles that follow the shade's live radius
(SystemUI sets it in `BlurUtils.applyBlur`), and hiding regions of tiles
outside the window.

### How it stays opt-in

1. **LSPosed scope.** The APK has no activity, service, receiver or permission.
   LSPosed loads it only into the apps ticked in its scope; `xposedscope` just
   suggests SystemUI and the two launchers.
2. **The engine's switch.** `Gate` reads the engine's own properties and draws
   only when `persist.sys.glass.mode` is `glass`/`on`, `bypass` is not 1 and
   `enabled` is not 0. The Action button, the WebUI and `glassctl` therefore
   switch the controls together with the compositor.
3. **A live engine.** The mode persists, so it still says glass when the module
   is disabled, stood down by its boot guard, or removed. Stock SurfaceFlinger
   would then blur the regions into frosted tiles. So the companion also needs
   `sys.glass.engine` to be `hooked` or `glass`; `not-hooked`, `no-glass` and
   empty mean stock. On a debuggable build an empty value passes, since a dev
   install of the engine may predate the report (the AVD's does). alioth
   reports `glass`.
4. **Per surface:** `persist.sys.glass.app.qs=0`, `persist.sys.glass.app.folders=0`,
   `persist.sys.glass.app.icons=0`, `persist.sys.glass.app.widgets=0`,
   `persist.sys.glass.app.shade=0` (scrims and cards),
   `persist.sys.glass.app.headsup=0`, `persist.sys.glass.app.sliders=0`,
   `persist.sys.glass.app.toolbar=0`, `persist.sys.glass.app.screenshot=0`,
   `persist.sys.glass.app.toast=0`,
   `persist.sys.glass.app.drawer=0` (drawer icons),
   `persist.sys.glass.app.search=0` (drawer and dock search boxes),
   `persist.sys.glass.app.clock=0` (the lockscreen clock; `plain` keeps its
   glass but refracts nothing; `persist.sys.glass.app.clocktint`, 0 to 1, is
   how much of a plugin face's own colours its glass keeps). Themed icons also need the user's
   own choice of themed (on crDroid, "Minimal") icons; full-colour icons are
   never touched. Cards and heads-ups need the user's notification row
   transparency.
5. **Off is stock.** No region, and the host's colour returned unchanged: 0
   shade regions in stock mode, with `app.qs=0`, and with the engine reporting
   `no-glass` (VERIFIED.md).
6. **Fail closed.** Every hook catches everything. A missing class, method or
   field leaves that surface stock, with one line in logcat (`StockGlassApp`)
   and the LSPosed log.

### Not built

- **Full-colour icon backgrounds.** Themed icons are built (above). Full-colour
  icons reach the screen as cached bitmaps (`BaseIconFactory` and the icon
  cache) with the adaptive icon's background layer baked in. Glass under them
  would need them re-rendered without it (a hook in the icon factory, plus
  invalidating the cache through `IconProvider`'s system state), and an app's
  own background layer is part of its look, where a themed plate is only the
  theme's.
- Icon regions are rounded rects measured from the theme's shape: exact for a
  circle or a rounded square, closest at the corners for a squircle or a
  cookie.
- **The open folder panel.** Its region would blur the wallpaper but not the
  workspace icons behind it, which are in the same layer, so it would read as a
  hole. Left stock.
- **Recoloured widget content.** A widget's text and icons stay as the app drew
  them; where they would be lost on glass, the widget keeps more plate (above).
  Forcing them light, as a "clear" home screen style would, means overriding
  the widget's colour resources (`AppWidgetHostView.setColorResources`) and
  works only for widgets that take their colours from the system palette.
- Regions are rounded rects and ignore ancestor clips. Neither showed on QS,
  folders, icons or widgets on the AVD; drawer icons fade by their visible share
  instead (above).
- **The drawer's sheet.** The launcher draws it in `ScrimView` (the rounded
  sheet, in crDroid's "Background opacity"), over a window that already blurs
  the wallpaper behind it (the depth controller). A region for it would cover
  over 90% of the display both ways, which the engine treats as a backdrop
  (Android's blur, or the `backdrop` style), not a pane. Left as the launcher
  draws it; its icons and search box have glass.
- **crDroid's auto-brightness and ringer buttons.** Static composables with no
  modifier argument; glass there would mean hooking `animateColorAsState`,
  `clip` and `IconButton` for every call in SystemUI, behind a thread-local flag.
- **A tinted active track.** A slider's region is one pane under the whole
  track; tinting only the active part, as an active tile is tinted, needs a
  second region that follows the thumb.
- **Other clock faces.** The flex clock (A17's default, large and small),
  crDroid's text-based clock styles and the digital plugin faces get glass.
  BigNum, the analog plugin face, and crDroid's analog and image styles
  (`AnalogClockView`, `NothingAnalogClockView`) stay stock. The clock's date
  line, smartspace and lock icon stay stock too, as on iOS.
- **Per-digit glass on overlapping faces.** Inflate's and NumOverlap's digits
  overlap, and one group draws them all, so they share one shape and one
  bevel; each digit stays apart by its own colour and outline (the tint).
  Glass per digit would mean redrawing each digit into a layer of its own.

On the AVD there is no LSPosed, so `companion/dev_attach.sh` loads the same APK
through a JVMTI agent standing in for the Xposed framework.
`companion/README.md` covers it and its limits, and how crDroid's clock plugins,
which the AVD's SystemUI would refuse (a signature permission), are tested there.

## Still open
- On a phone glass costs three to ten times Android's blur, and what it costs
  is not what the AVD showed (VERIFIED.md, "Where the glass's time goes on a
  phone"). alioth (Adreno 650, 120 Hz), home screen swiped: 2.1 ms a frame
  for Android's blur, 13.2 for glass, 8.8 with See-through resolution at
  Quarter, 7.8 with one Blur layer, all three with the colour fringe on, the
  default until v0.4.20. The F1 (Adreno 630, 60 Hz): 3.2, 31.8, 20.1, 19.8,
  and with the fringe off 20.5 for glass and 16.5 with one layer. The Performance settings
  are the difference between holding the refresh rate and not, and even the
  fastest misses a quarter of alioth's frames while a home screen is dragged.
  What is left was measured by drawing the panes flat and not at all
  (`debugPane`, VERIFIED.md): of what panes add to a frame, the glass
  shader costs 3.5 ms more than the flat one (home screen), 6.2 (Quick
  Settings) and 9.0 (app drawer) on alioth, and 9.5 on the F1's home screen;
  a draw per pane is 2.1 to 2.9. Neither is a setting:
  - The glass shader. It is not the lens band as the shader's text has it.
    On the F1's home screen (one layer, fringe off, v0.4.19) a read of the
    blur costs about 0.2 ms a frame over all the panes; a band a third as
    wide, which puts most pixels on the `deep` path, changed nothing; and
    with the lens off and one read, 7.5 of the 9.5 ms were still there
    (VERIFIED.md, v0.4.20). The arithmetic written for those cases is a
    fraction of the full path's, so the GPU was paying for code a branch
    should have skipped. The lean builds (item 8) tested that on the F1 and
    it held: three quarters of what glass panes cost over flat ones in Quick
    Settings went with the left-out code, most likely with its texture reads
    (the tone shader, whose lean build loses only arithmetic, gained
    nothing). What that leaves there: 1.1 ms for the lens, 0.4 for the
    see-through read and the tone chain, 1.4 for drawing the panes at all.
    Next in the same direction: `lean_soft`, which draws the panes that
    soften (two layers), still holds both softening kernels, nine taps and
    five, where a pane uses one; and a pane that is all interior could have
    a build without the lens. Both wait for home-screen numbers, which
    v0.4.20 does not have yet on either phone. The baked lens (for each
    shape, the bend, the rim light and the edge drawn once into an image the
    pane's shader reads) could save at most that 1.1 ms and stays shelved.
  - In the presets that leave `lensBlur` automatic, a pane whose blur is at
    `lensBlurRef` or more shows nothing of its sharp child, yet names it:
    the screen is copied and read for nothing. Drawing such a pane with its
    blur as both children would be the same picture.
  - The render thread's CPU: about 0.06 to 0.15 ms a pane, because every pane
    is a Skia draw of its own where Android's blur batches a layer's regions
    into one. Panes of one size on a grid (launcher icons, Quick Settings
    tiles) could share their uniforms, with the pane's place worked out in
    the shader from a lattice, and Skia would then merge them.
  Panes under an open shade (the launcher's icons, 6 of the 10.8 ms glass
  adds to alioth's Quick Settings) still run the whole shader, less the
  fringe and the softening since v0.4.17. Under a heavy backdrop they could
  be drawn flat; under the toned one alioth uses (27 px) they still show.
- The engine took about 0.3 s to find its symbols and hook when
  SurfaceFlinger starts (318 ms on the Poco F1), before SurfaceFlinger's own
  start-up. The install-time symbol cache ("Many SurfaceFlingers" above) hands
  it the addresses instead; what it saves on a phone is not measured yet.
- The companion has only run under its dev loader on the A17 AVD. LSPosed on
  A17, crDroid's Launcher3QuickStep at run time (folders, widgets, and icons in
  the "Minimal" style), crDroid's notification cards and heads-ups (LSPosed only),
  and region cost on a real GPU are unverified.
- arm64 has loaded and run in stock mode on alioth (crDroid 13.1, A17), but
  glass has not yet drawn a frame there. Two arm64-only failures so far, both
  invisible on the x86_64 AVDs: an SELinux `execmem` denial at load (fixed by a
  pre-hook check plus `sepolicy.rule`), and a compiler-changed calling
  convention on `MakeForShader(SkString)` that crashed the first glass frame
  (fixed by compiling through the Options form, checking every by-value
  callee's result register, and a crash guard). VERIFIED.md has both.
- The calling-convention check walks a few hundred instructions (calls and
  both arms of a branch, `abi_check.h`), but it can only say "x8" for read
  encodings it knows, so a ROM whose code looks different gets "undetermined"
  and no glass, not a crash. Only by-value RETURNS are checked; a compiler
  could also drop or reorder plain arguments of a function it sees every
  caller of. None of the bound functions on alioth do (read off the
  disassembly), but a new target deserves the same look.
- Branch protection. 21 of the 64 surfaceflingers in the corpus are built with
  BTI and PAC (the Pixel 8 and later from Android 17 QPR1, EvoX 12.x and
  Lineage for garnet, the FP6, the S22, Nothing's Qualcomm builds, Motorola's
  mona), and on an ARMv9 core bionic maps their code guarded: an indirect
  branch must land on a `bti` or `paciasp`. The engine's calls into SurfaceFlinger do, since every
  function it calls starts with one in all 21 (checked at start, and a function
  without one would lose the guard on its first page instead). frida's way
  back from its trampoline into the hooked function lands on an ordinary
  instruction, so the engine takes the guard off the pages it patches before
  hooking. The start-up log and `sys.glass.bti` say which case a phone is
  (`glassctl status` mentions it when it is guarded). Not yet run on a phone
  that enforces BTI: alioth and the F1 are ARMv8.x.
- Live mode (`glassctl live`, `module/live.sh`): the engine for one boot
  without the module's mount, for root that arrives after boot. It has run on
  the A17 AVD only, enforcing (rules added through a stand-in for the root
  manager's policy tool) and permissive; `ksud` and `apd` were checked against
  their source only. Loading it at boot from post-fs-data.sh (KernelSU 3
  without a metamodule, no restart) is drafted in
  `captures/sf-corpus/reports/rootless.md` but not wired in: it needs
  post-mount.sh to count it as mounted, and a measured real boot.
- The ROM patch (`rom/`) compiles against AOSP android17-release and
  android16-qpr2-release and LineageOS 24.0 and 23.2, with each branch's own
  headers and renderengine's `-Werror` flags, and applies with `git am`. It
  has not been built in a full tree or booted. It cuts the drawing code out of
  `glass_engine.cpp` with a script (`rom/split/`) until engine/src is split
  for it. Evolution X 12.x (and crDroid 13.x) need a reworked patch, because
  their `drawBlurRegion` takes a material. A ROM that carries it is refused by
  the module's installer (the resolver looks for `GlassFilter`).
- RenderEngine on Vulkan. Lineage 24 for the Galaxy S20 (x1s) sets
  `debug.renderengine.backend=skiavkthreaded` (Ganesh on Vulkan). The engine's
  draw has only run on Ganesh GL.
- Devices whose vendor switched window blur off
  (`ro.surface_flinger.supports_background_blur` 0 or unset: Nothing Phone (3a),
  CMF Phone 2 Pro) install fine and draw nothing; the installer and
  `glassctl status` say why. Setting the property from the module before
  SurfaceFlinger starts would turn blur on (an opt-in, since the vendor chose
  it off, presumably for GPU cost); not built.
- Every AVD runs permissive. Test with `setenforce 1`, or with `hookprobe`
  under `runcon u:r:surfaceflinger:s0`, before trusting an emulator result on
  anything SELinux could refuse.
- The backdrop and pane split, the toned backdrop and the lighter regenerated
  blur were verified on the A17 AVD (SwiftShader, Ganesh GL). On alioth only the
  region map is measured; the look of `clear` over real notification cards,
  whose own backgrounds sit on top of the glass, is not yet seen.
- `clear` is modelled on a desktop reference, not measured against iOS.
- `basic`'s Regular stop uses the blend and tone of a WebGPU reimplementation of
  QuartzCore (uikit.even.how), not values measured on an iPhone; Frosted has no
  reference at all. Blur intensity (`fillBlurScale`) on panes and region tints
  have not drawn a frame on Android 17 yet (VERIFIED.md, v0.4.6).
- `lensBlurRef = 12` was measured on the QPR0 Kawase path. QPR2 and A17 choose
  KawaseDualFilterV2; re-run the radius sweep on a device.
- The `frosted` and `prism` presets were judged by eye on the emulator probe,
  not calibrated. `clear` and `regular` are measured (VERIFIED.md).
