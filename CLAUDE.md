# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Stock Glass: Liquid Glass for Android, drawn by SurfaceFlinger itself. A root
module (KernelSU / Magisk / APatch) injects `libglassengine.so` into
`surfaceflinger`, hooks `BlurFilter::drawBlurRegion` and draws a refracting
glass shader wherever the system already blurs. An optional LSPosed companion
creates blur regions for surfaces Android never blurs (QS tiles, launcher
icons/folders/widgets, sliders, toasts, the lockscreen clock…). Android 14–17.

`docs/ARCHITECTURE.md` explains every design decision; `docs/VERIFIED.md` is
the lab notebook (what was measured, on which device/build, with numbers).
Read the relevant section of each before changing engine behaviour, and record
new on-device findings in VERIFIED.md. Each component has its own README
(`module/`, `companion/`, `rom/`, `auto/`) covering settings and switches.

## Commands

The scripts are bash and expect a Linux host (NDK `prebuilt/linux-x86_64`,
GNU `sed -i`, `stat -c`). Needed: Android NDK 29.0.14206865 (`ANDROID_NDK`,
default `$HOME/Android/Sdk/ndk/29.0.14206865`), Python 3, and for the
companion the SDK (build-tools 36.0.0, platform android-33; `ANDROID_BUILD_TOOLS`,
`ANDROID_JAR`) plus a JDK.

```sh
# Pinned third-party code (gitignored under engine/vendor/)
engine/vendor/fetch_skia_headers.sh
engine/vendor/fetch_xz.sh
engine/vendor/fetch_frida_gum.sh
engine/vendor/fetch_patchelf_android.sh

# Host tests (no device). CI runs both before building.
engine/test/host.sh            # composite, sharp_bounds, merge, tint, abi_walk, elf_note, library, blur_scale
module/tools/guard_test.sh     # boot guard + metamodule check under busybox ash
module/tools/watchdog_test.sh  # watchdog.sh (keep-or-revert, restart watch) under busybox ash

# Builds
engine/build.sh [abi...]       # engine/out/<abi>/libglassengine.so (arm64-v8a, x86_64)
engine/tools/build.sh          # glasscheck, the install-time preflight
module/pack.sh                 # rebuilds engine + glasscheck, -> module/out/stockglass-<version>.zip
companion/build.sh             # -> companion/out/stockglass-companion.apk
harness/build.sh               # test app that makes blur regions on demand
shader/gen_glass_sksl.py       # -> shader/build/*.sksl (engine/build.sh runs it)
```

Running a single host test: each test in `host.sh` is one `g++ -std=c++17`
line over `engine/test/<name>_test.cpp` plus the `engine/src/*.cpp` it needs;
copy that line. Tests that need a real ROM binary:
`engine/test/run.sh <path/to/surfaceflinger>[:addr] ...` (resolver against a
binary; a `libhwui.so` next to it, or in `lib64/` beside it, is read as
the device would) and `engine/test/corpus.sh` (the firmware survey, binaries
under the gitignored `captures/`).

On a Windows checkout (`core.autocrlf=true`) the scripts have CRLF endings
and fail in WSL with `bash\r`. Run them from an LF copy, not in place.

Device dev loops: `engine/install_dev.sh` (writable-system AVD only, patches
`/system/bin/surfaceflinger`), `companion/dev_attach.sh [systemui|launcher]`
(loads the companion into a debuggable AVD via a JVMTI agent, no LSPosed),
`module/tools/webui_preview.py` (serves the WebUI locally, `ksu.exec` over
`adb shell`). With several devices attached, set `ANDROID_SERIAL`.

Gradle is deliberately not used: companion, harness and `auto/*` build with
raw aapt2 → javac → d8 → zipalign → apksigner. `auto/` is a prototype and is
not built by CI or included in releases.

## Architecture (big picture)

**Two delivery tracks** share the same drawing code:
- *Track B, `module/` + `engine/`* — the shipping form. Hacky by necessity:
  symbols are resolved from the stripped binary's MiniDebugInfo, the hook is an
  inline frida-gum hook, Skia is called through resolved function pointers.
- *Track A, `rom/`* — `rom/make_patch.sh <branch>` emits a `frameworks/native`
  patch (`GlassFilter` wrapping the blur filter) for source-built ROMs; same
  drawing code, Skia calls become direct.

**Engine flow** (`engine/src/`):
1. Injection is a `DT_NEEDED` entry added by patchelf *on the device* to a copy
   of `surfaceflinger`, mounted over the original (LD_PRELOAD is stripped,
   AT_SECURE). `post-fs-data.sh` builds that overlay.
2. One resolver, `sf_resolve.*` + the entry-point table `sf_symbols.cpp`, is
   shared by the engine at startup, `glasscheck` at install, and the host
   tests — so installer and engine make the same accept/refuse decision by
   construction. `judge()` applies the group rules. Skia may also come from a
   library the binary loads (`kSkiaLibraries`: `libhwui.so` on ColorOS 17),
   so every `Symbol` carries a `module` and its address is relative to that
   file's load bias. Hook targets and RenderEngine entries must be module 0.
3. Hook target variants are rows in `kHookShapes`: `(radius, alpha)` ≤ A16
   QPR0, `(radius, scale, alpha)` from QPR2, Evolution X/crDroid
   `(radius, scale, alpha, material)`. A new shape = one table row + one
   `sf_symbols.cpp` entry + one replacement function. Similarly
   `kGenerateFilters` lists blur filters whose `generateTemporaryImage` is hooked.
4. `sf_cache.*`: glasscheck writes `libglassengine.symcache` (keyed by build
   ID + table hash, with first code bytes); the engine verifies it instead of
   resolving. A bad cache only costs time, never correctness.
5. `sk_bridge.*` / `glass_draw.*` draw the shader; shader compile is lazy (first
   blur call). `glass_params.cpp` holds the specs/presets.
6. Regions are classified by geometry: ≥90% of the display on each side is a
   *backdrop* (shade, dim-behind), anything smaller a *pane* (gets the glass).

**Shader**: `shader/gen_glass_sksl.py` is the single source of truth for all
SkSL variants (high, tone, lean, lean_soft, deep, tone_lean). `engine/build.sh`
embeds them into `engine/src/generated/glass_shader_src.h` (committed, but
regenerated — don't hand-edit) together with uniform name lists and a packed
uniform layout used when `findUniform` is inlined away. Never hand-edit the
harness's `.sksl` assets either.

**Runtime control** is all system properties: `persist.sys.glass.mode`
(only `glass`/`on` means glass; anything else is Android's stock blur, and stock
mode must stay pixel-identical to stock), `persist.sys.glass.preset`, then
per-setting `persist.sys.glass.<name>` overrides; layering is
override → preset → default. `module/glassctl` is the only place that knows
which properties are switches vs tuning; the Action button, WebUI and CLI all
go through it, and the WebUI gets values from `glasscheck --params` (compiled
from the same `glass_params.cpp`), so no UI holds its own copy of defaults or
presets. The engine reports state in `sys.glass.engine`.

**Companion** (`companion/`, legacy Xposed API 82): for each control it
attaches a `BackgroundBlurDrawable` from the window's `ViewRootImpl` at the
control's bounds and makes the control's fill translucent; RenderThread then
reports the region every frame, so regions track scroll/animation with no
bookkeeping. `Gate.java` draws only when the engine's mode is glass *and*
`sys.glass.engine` is `hooked`/`glass`; per-surface switches are
`persist.sys.glass.app.*`. The hook table per surface is in ARCHITECTURE.md
("Hooks on Android 17"). `devloader/` is a dev-only Xposed API stand-in, not
packaged into the module's classes.dex.

## Invariants that prevent bootloops

- **No undefined symbols in `libglassengine.so`.** SurfaceFlinger exports almost
  nothing, so any un-inlined Skia call becomes an unresolvable symbol and the
  device bootloops. Model every bridged Skia signature with pointers only
  (non-trivial types by pointer; returns via explicit sret first argument).
  `engine/build.sh` links with `-Wl,--no-undefined`; keep it. `-static-libstdc++`
  is mandatory.
- **Refusal is always a valid outcome.** Drift, missing strategy, shader
  compile failure, Graphite backend → call the original `drawBlurRegion`.
- **Layout dependencies** on Skia types are listed in ARCHITECTURE.md
  ("Layout dependencies, in full"); update that table if you touch a new field.
  Skia headers are pinned to the target release, but the engine must never
  assume they match — verify at load time.
- **Boot guard lives in the scripts** (`post-fs-data.sh` arms, `service.sh`
  disarms on `sys.boot_completed`), because the failure happens in the linker
  before engine code runs. Test changes with `module/tools/guard_test.sh`.
- **Updates never rewrite a mapped file**: `customize.sh` renames live paths
  first (truncating the running `.so` crashed SurfaceFlinger once).
- Module shell scripts run under Android's `/system/bin/sh` or, from the root
  manager, busybox's standalone ash, where applets shadow PATH (`svc`,
  `reboot`, `pidof`, `sleep`). Call Android's own tools by absolute path when
  it matters. Keep the scripts POSIX and LF-only (`pack.sh` strips CRs, but
  don't rely on it elsewhere).
- **The watchdog** (`module/watchdog.sh`): glass switched on from stock, and the
  first boot of a new engine with glass on, stays only if confirmed (Volume Up,
  WebUI Keep, `glassctl keep`). Repeated SurfaceFlinger or System UI restarts
  revert the mode, and SurfaceFlinger crash-looping in stock mode disables the
  module and reboots. Any new path that turns glass on should go through
  `glassctl glass` so it gets the same question.
- Companion hooks fail closed: catch everything, leave the surface stock, log
  one line under the `StockGlassApp` tag.

## Releases

`.github/workflows/release.yml` builds and publishes every push to `main`.
A new `version=` in `module/module.prop` (bump `versionCode` with it) becomes
that version's release; any other push becomes a pre-release named like
`v0.4.21-3-g1a2b3c4` (`.github/scripts/release.sh` rewrites module.prop in CI
for that). The companion's version is in `companion/AndroidManifest.xml` and is
only built when the signing-key secret is present; every build must keep the
same key so it installs over earlier ones.

Commit messages: a short imperative or `vX.Y.Z: …` subject, then prose
paragraphs explaining what changed and what was verified where.
