# Verified facts

Everything here was measured, not inferred. Re-verify anything marked
**per-device** on a new target before relying on it.

## The hook seam

`android::renderengine::skia::BlurFilter::drawBlurRegion(SkCanvas*, SkRRect const&, unsigned int, float, SkRect const&, sk_sp<SkImage>, sk_sp<SkImage>)`

| | Android 14 (redroid x86_64) | Android 16 (AVD `default` x86_64, BE2A.250530.026.D1) |
|---|---|---|
| symbol present, not inlined | yes, 1471 bytes | yes, **2660 bytes** @ `0x248080` |
| signature | identical | identical |
| `.gnu_debugdata` present | yes | yes |
| recovered FUNC symbols | 19,691 | **22,669** |

Confirmed identical against AOSP `main`'s `BlurFilter.h`, so 14/15/16 share one
hook signature. `drawBlurRegion` is **concrete and non-virtual on the base
class**, so it is one symbol regardless of which filter implementation the ROM
selects, and it sits above the GL/Vk split.

### Where the code actually lives

There is **no `/system/lib64/librenderengine.so`** and **no
`libsurfaceflinger.so`** on either build. `/system/bin/surfaceflinger` is a
monolith (7.9 MB on A14, 8.8 MB on A16) with RenderEngine *and* Skia statically
linked in. Confirmed by the embedded Kawase SkSL (`in_blurOffset`,
`child.eval(xy + float2(...))`) sitting in the binary's string table.

### Symbol resolution

The binary is stripped — `.dynsym` has ~1690 entries and zero RenderEngine
symbols, so `dlsym` will never find `drawBlurRegion`. But `.gnu_debugdata`
(MiniDebugInfo) carries an LZMA-compressed ELF with a full `.symtab`:

```
objcopy --dump-section .gnu_debugdata=dbg.xz /system/bin/surfaceflinger /dev/null
xz -d -c dbg.xz > dbg.elf
readelf -sW dbg.elf        # 22,669 FUNC symbols, with sizes
```

Resolve by demangled name, add the PIE load base from `/proc/<pid>/maps`. The
*mangled* name encodes the parameter types, so signature drift across releases
is detectable at runtime rather than being a per-device human task.

### Also present in the A16 binary

- `SkRuntimeEffect::MakeForShader(SkString)` — compile SkSL using SF's own Skia,
  no need to bundle one
- `SkRuntimeEffect::makeShader(sk_sp<SkData const>, SkSpan<ChildPtr const>, SkMatrix const*)`
  — bind two children plus a uniform blob, which is exactly the call the engine needs
- `KawaseBlurFilter::generate(SkiaGpuContext*, unsigned int, sk_sp<SkImage>, SkRect const&)`
  — note `SkiaGpuContext*`, the A15+ replacement for `GrRecordingContext*`
- `GaussianBlurFilter::generate(...)` — same new signature

## Blur availability

**per-device.** `ro.surface_flinger.supports_background_blur` is **absent from
the A16 system image's `build.prop`** but reads `1` at runtime — it comes from
vendor. Check the running value, not the image. If a target ROM has it unset,
`drawBlurRegion` is never called and the hook will look broken while being fine.

## The shader contract

Both children carry their own local matrix, built caller-side:

```
blurredContent = blurredImage->makeShader(kClamp, kClamp, linear, &Mblur)
                 Mblur  = getShaderTransform(canvas, blurRect, kInverseInputScale)  // 4.0
sharpContent   = input->makeShader(kClamp, kClamp, linear, &Msharp)
                 Msharp = getShaderTransform(canvas, blurRect, 1.0)
```

so **both are evaluated at the same coordinate** despite `blurredContent` being
quarter resolution (`kInputScale = 0.25f`). The shader must not compensate for
the 0.25 anywhere.

Uniform framing, in canvas-local space (the space `effectRegion` and `blurRect`
live in):

```
offset = -effectRegion.rect().topLeft()      pixels = fragCoord + offset
size   =  effectRegion.rect().size()
crop   =  blurRect
```

Draw with `drawRect(effectRegion.rect())`, **not** `drawRRect(effectRegion)`:
the shader owns its antialiasing via the SDF, and clipping to the round rect
would feather the edge twice — and would foreclose ever drawing outside the
region.

Because both children are built `kClamp` by us, `crop` is belt-and-braces here,
unlike the app-side port where an out-of-bounds chain-input `eval` returns
transparent black and paints a black rim.

## Gotchas found the hard way

- **`unpremul` is a Skia builtin** (`$pure half4 unpremul(half4)`). Declaring
  `float4 unpremul(half4)` fails with "differ only in return type" and cascades
  into four bogus follow-on errors. Renamed to `toStraight`. The same collision
  exists in RenderEngine's Skia, where it would cost a soft reboot to find.
- **`packed` is a reserved word** in SkSL (inherited from the app-side port).
- build-tools 33.0.2's `d8` (R8 3.3.20) NPEs on JDK 25 class files, and its
  `aapt2` cannot parse a modern `android.jar` resource table. Use 36.0.0.
- JDK 25's `javac` rejects `-bootclasspath` alongside `-source`/`-target`.

## Measured behaviour (A16 AVD, 1080x2400 @ 420dpi, density 2.625)

At the upstream `regular` defaults, HIGH tier (16 taps, one per pixel), sampling
the sharp child:

| check | result |
|---|---|
| deep pane interior vs raw backdrop, refraction isolated | **maxdiff 0** — bit exact |
| top rim band vs raw backdrop | maxdiff 211, mean 37 — refracting |
| dispersion on/off, pane interior | **maxdiff 0** — confined to the rim band |
| dispersion on/off, rim over colour blocks | max channel separation **258** |
| dispersion on/off, rim over 1–4px white lines | max channel separation 186 |

The bit-exact interior also confirms the tone chain is a true identity at
frost=0 / tint=0 / saturation=1, i.e. the `bdAlpha`/`cover` algebra collapses
correctly — so it can be stripped for the SF build once calibration is signed
off, since both SF snapshots are opaque.

## Harness

`harness/build.sh` — no Gradle. aapt2 → javac → d8 → zipalign → apksigner.
Tuning loop is `adb push <file> /data/local/tmp/glass_params.txt`; the view
stats mtime every 300 ms and reloads. **Every reload resets to defaults first** —
an earlier version overlaid, so a key absent from the file kept its previous
value and one experiment silently inherited another's settings.

## Calibration of the two new uniforms

`blurMix` and `blurFalloff` are additions for the `drawBlurRegion` contract and
have no upstream iOS calibration behind them. Swept on the A16 AVD, measuring
luminance std-dev in the refraction band vs. well past it (pane 1, 20dp band =
53 px at density 2.625):

| | rim-band contrast | deep-interior contrast |
|---|---|---|
| `blurFalloff` 0.5 / 1.0 / 2.0 / 4.0 (at mix 1.0) | 12.85 / 19.02 / 25.49 / 29.32 | **4.18 at every value** |
| `blurMix` 0.0 / 0.5 / 0.75 / 1.0 (at falloff 2.0) | 42.43 / 31.40 / 27.54 / 25.49 | 20.05 / 10.59 / 6.36 / 4.18 |

The flat interior column is the useful result: **`blurFalloff` is a pure rim
control and cannot leak inward**, while `blurMix` moves both.

Chosen: **`blurMix = 1.0`, `blurFalloff = 2.0`.**

- mix 1.0 — anything less leaves ghosted backdrop structure that reads as a
  smudge, and the compositor already picked that blur radius for the region.
- falloff 2.0 — captures 25.5 of the ~29.3 available rim contrast; 0.5→2.0
  gains 12.6 but 2.0→4.0 only 3.8, and by 4.0 backdrop lines bleed into the
  side edges.

Sweep rig: `harness/sweep.sh <key> <values…> [-- fixed=…]`, writes a labelled
contact sheet.

## Skia ABI surface (measured, A16 vs A14)

The engine cannot simply call Skia: SurfaceFlinger statically links it, and how
much survives as *out-of-line* code is a per-build compiler decision. Measured
by resolving each mangled name against the recovered symbol table
(`engine/test/run.sh`):

| entry point | A16 | A14 |
|---|---|---|
| `BlurFilter::drawBlurRegion` | 0x248080 (2660 B) | 0x31afb0 (1471 B) |
| `SkRuntimeEffect::MakeForShader(SkString)` | yes | yes |
| `SkImage::makeShader(tile,tile,sampling,matrix*)` | yes | yes |
| `SkCanvas::drawRect(SkRect&,SkPaint&)` | yes | yes |
| `SkString` ctor/dtor | yes | yes |
| `SkPaint` dtor / copy ctor | yes | yes |
| **`SkPaint()` default ctor** | **inlined** | **inlined** |
| **`SkPaint::setShader`** | **inlined** | **inlined** |
| **`SkRuntimeEffect::uniforms()`** | **inlined** | **inlined** |
| `SkRuntimeEffectBuilder` ctor/dtor/makeShader | yes | **inlined** |
| `SkRuntimeEffectBuilder::writableUniformData` | yes | yes |
| `BuilderUniform::set<float>(const float*, int)` | yes | **inlined** |
| `SkRuntimeEffect::makeShader(SkData, SkSpan<ChildPtr…>, SkMatrix*)` | yes | yes (one `K` apart) |

Two consequences.

**1. The engine must compile against Skia headers.** `SkPaint()`, `setShader`
and `uniforms()` have no symbol to call, so their inline bodies and the object
layouts have to come from headers matching the target's Skia. Only genuinely
out-of-line entry points get resolved and called through function pointers.

**2. Shader construction needs two strategies.**

- `builder` — `SkRuntimeEffectBuilder` does the uniform packing. Available on
  A16. Preferred, because `SkRuntimeEffect::uniforms()` is inlined, so the
  uniform offsets are not otherwise reachable without reimplementing Skia's
  packing rules.
- `direct` — hand-built `SkData` blob + `SkRuntimeEffect::makeShader`. The only
  option on A14.

`engine/src/sf_symbols.cpp` tags every entry point with its strategy; the engine
picks the first fully-resolved one and declines to install if none is.

### Mangling drift

Names are matched against a *candidate list*, newest first, because Skia's
templated signatures drift without the ABI changing:

- A16 `SkSpan<ChildPtr const>` vs A14 `SkSpan<ChildPtr>` — one `K`, same
  {pointer,length} ABI.
- C1/C2 and D1/D2 constructor/destructor variants differ by build.

A **prefix hit with no exact hit** is treated as signature drift and blocks
installation; no prefix hit at all just means inlined, which only rules out a
strategy. `drawBlurRegion` itself mangles byte-identically on A14 and A16.

### Host test

`engine/test/run.sh <binary>[:<expected-hex-addr>] …` — parses real pulled
`surfaceflinger` binaries on the host. Both A14 and A16 pass. This is the same
parsing code that later runs inside SurfaceFlinger, where a bad offset is a soft
reboot, so it is exercised here first.

## Vendored Skia headers

`engine/vendor/fetch_skia_headers.sh [branch]` — blobless sparse clone of
`platform/external/skia`, headers only. **354 files, 4.4 MB.** No sources are
vendored and nothing is linked against; every out-of-line Skia call still goes
through a pointer resolved from the target's own MiniDebugInfo.

Current pin: `android16-release` @ `bcb0f77c44783b1800ba37641ba7ecab04f05e07`
(matches the dev AVD, `BE2A.250530.026.D1`).

AOSP publishes the QPR split as separate branches, which is exactly the
granularity the pin needs:

```
android16-release        Android 16 QPR0   LineageOS 23.0
android16-qpr1-release   Android 16 QPR1
android16-qpr2-release   Android 16 QPR2   LineageOS 23.2, crDroid v12.7 (beryllium)
```

### The build config is load-bearing

`include/config/SkUserConfig.h` does **not** exist in the tree. AOSP supplies its
own at `android/include/config/SkUserConfig.h`, and it must be vendored too — it
defines `SK_GANESH`, `SK_GL`, `SK_VULKAN`, `SK_ENABLE_ANDROID_UTILS`,
`SK_SUPPORT_PDF`, `SK_ENABLE_PRECOMPILE` and more, all of which change which
fields exist in Skia objects. Substituting a hand-written config would silently
produce a different layout from the binary being hooked. The fetch script fails
hard if that file is missing from the chosen branch.

`SkUserConfig.h` includes `"SkUserConfigManual.h"` unqualified, which lives in
`include/config/`, so that directory must be on the include path as well.

### Include paths

```
-I vendor/skia/android          # AOSP SkUserConfig.h, must come first
-I vendor/skia                  # "include/core/SkPaint.h" style paths
-I vendor/skia/include/config   # unqualified SkUserConfigManual.h
```

Verified compiling with NDK r29 (clang 21) for `aarch64-linux-android30` and
`x86_64-linux-android30`.

### Layout constants (identical on arm64 and x86_64)

| type | size |
|---|---|
| `SkPaint` | 80 |
| `SkRuntimeShaderBuilder` | 40 |
| `SkRRect` | 52 |
| `SkString` | 8 |

These are the numbers a header/target mismatch would show up as, so they are
recorded here as a baseline to re-check whenever the pin moves.

### NDK note

NDK r29's sysroot tops out at **API 35** — `--target=*-linux-android36` fails to
link with `cannot open crtbegin_so.o`. The engine targets API 30, which is well
below every supported Android version and keeps the .so broadly loadable.

## Hook backend: frida-gum

Three candidates were evaluated against the requirement "inline-hook an internal
function, on arm64 *and* x86_64" (x86_64 is non-negotiable — the whole dev loop
runs on an AVD, and an arm64 AVD on an x86_64 host has no KVM).

| library | verdict |
|---|---|
| **ByteHook** | Not a candidate. PLT hook library; `drawBlurRegion` is an internal call that never goes through a PLT. |
| **ShadowHook** | Rejected. armeabi-v7a / arm64-v8a only — cannot run on the x86_64 AVD at all. |
| **Dobby** | Rejected after attempting the build. Broken upstream, two independent ways. |
| **frida-gum** | **Chosen.** Prebuilt devkits published for android-arm64 and android-x86_64; no build step. |

### Why Dobby failed

Its `CMakeLists.txt` contains exactly **one** architecture guard in the entire
file (`if (PROCESSOR.X86_64 OR PROCESSOR.X86)`). The arm/arm64 sources are added
unconditionally, so an x86_64 build tries to assemble `blr`/`br` and dies. The
detection in `cmake/build_environment_check.cmake` sets `PROCESSOR.AARCH64`
while `cmake/compiler_and_linker.cmake` tests `PROCESSOR.ARM64`, which is never
set anywhere. Separately, its arm64 assembly does not assemble under NDK r29
(clang 21): *invalid symbol kind for ADRP relocation*, *invalid fixup for add
(uimm12)*. Its Linux backend also references `RuntimeModule::load_address`
(renamed `base`) and treats `MemRange::start` as a field when it is a method.

### frida-gum sizing

Version 17.17.0, pinned. The static devkit is ~70–74 MB, but it links down hard:

| ABI | unstripped | stripped (`-s`, `--gc-sections`, `--exclude-libs,ALL`) |
|---|---|---|
| arm64-v8a | 20 MB | **2.2 MB** |
| x86_64 | 20 MB | **3.4 MB** |

API notes for this version: `gum_interceptor_attach(interceptor, target,
listener, options)` takes a nullable `const GumAttachOptions*` as its fourth
argument (no flags enum), and `gum_make_call_listener(on_enter, on_leave, data,
data_destroy)` avoids all GObject boilerplate.

**ABI caveat for the hook:** `drawBlurRegion`'s `blurAlpha` is a `float`, so it
travels in a floating-point register and does **not** occupy a slot in the
integer argument sequence that `gum_invocation_context_get_nth_argument` indexes.
The integer args are therefore: 0 `this`, 1 `canvas`, 2 `effectRegion`,
3 `blurRadius`, 4 `blurRect`, 5 `blurredImage`, 6 `input`.

## xz decompression on device

Android ships no `xz`, `xzcat` or `unxz` — verified on an A16 image, toybox has
neither. `.gnu_debugdata` is xz-compressed, so the engine carries a minimal
XZ Embedded decoder (`engine/vendor/fetch_xz.sh`, 9 files, 136 KB).

Only the **CRC32** path is needed: reading the stream flags byte of a real
`.gnu_debugdata` blob gives check type 1 (CRC32), so crc64 and sha256 are not
vendored. BCJ filter support **is** included as insurance — omitting it would
fail on some targets only, which is the worst kind of bug.

The `.c` files must be compiled as C. Under a C++ front end their `lzma_state`
enum arithmetic fails to compile.

Verified byte-identical to system `xz -d` on both real blobs:

| source | compressed | decompressed |
|---|---|---|
| A16 `surfaceflinger` | 519,552 | 3,174,048 |
| A14 `surfaceflinger` | 374,308 | 2,709,712 |

## M0 — hook proven on device (A16 AVD, x86_64)

The whole injection chain works with zero Skia ABI surface.

```
23062 symbols; strategies: builder=yes direct=yes
drawBlurRegion link=0x248080 bias=0x5a98fb320000 runtime=0x5a98fb568080 (2660 bytes)
hook installed in 202 ms
drawBlurRegion #5 radius 80 -> 250 (forced=250) rect=[0 0 1080 2400]
```

`link=0x248080` is exactly what the host test predicted from the same binary.

**Visual proof.** Forcing the radius down against a dialog's native 80:

| | max pixel diff | mean | pixels changed >8 |
|---|---|---|---|
| native 80 vs forced 4 | 132 | 10.85 | **1,150,553** (44% of screen) |

### Injection

`patchelf --add-needed libglassengine.so` on a copy of `/system/bin/surfaceflinger`,
then replace the directory entry. No ptrace, no SELinux ptrace rules, no startup
race, and it re-applies automatically whenever SurfaceFlinger restarts.

A running executable's inode cannot be overwritten (ETXTBSY), but `mv` over it
is fine — the live process keeps the old inode until it restarts.

SELinux contexts must be restored after writing:

```
/system/bin/surfaceflinger    u:object_r:surfaceflinger_exec:s0
/system/lib64/*.so            u:object_r:system_lib_file:s0
```

### `-static-libstdc++` is mandatory

The NDK defaults to `libc++_shared.so`, which **does not exist on a device** —
the platform has its own libc++. A shared-STL build makes surfaceflinger
unlinkable and bootloops the device:

```
CANNOT LINK EXECUTABLE "/system/bin/surfaceflinger":
library "libc++_shared.so" not found: needed by /system/lib64/libglassengine.so
```

**The bootloop guard cannot catch this** — the failure is in the linker, before
any of our code runs. The guard protects against crashes *in* the engine, not
against making the binary unloadable. `build.sh` therefore refuses to emit a .so
whose `DT_NEEDED` list contains anything outside
`libc.so|libm.so|libdl.so|liblog.so`.

Recovery was a single `mv` from the `/system/bin/surfaceflinger.orig` backup the
installer leaves behind; no AVD wipe was needed.

### Things that wasted time, recorded so they do not again

- **Kawase saturates.** `radius / 6` clamped to `kMaxPasses` means radius 80 and
  radius 250 produce *byte-identical* output. Testing an override by making the
  blur bigger proves nothing; force it **down**.
- **SurfaceFlinger does not recomposite a static screen.** `screencap` returns
  the last composited buffer, so an A/B where nothing on screen moved yields
  three byte-identical PNGs. Force a fresh composite between captures.
- **Every SystemUI blur is transient.** The shade animates its radius 0→20 during
  expansion then goes opaque; a screenshot taken afterwards can never contain a
  blur region. The harness now shows a dialog with `FLAG_BLUR_BEHIND` +
  `setBlurBehindRadius` to hold one indefinitely.
- **`setBackgroundBlurRadius` alone produced no blur region** on a stock dialog
  theme — that path needs a genuinely translucent window. `dumpsys` showed
  `mBlurEnabled=true` with no layer carrying a radius. `FLAG_BLUR_BEHIND` has no
  such requirement.
- **`cmd | grep -q` under `set -o pipefail`** reports failure: grep exits early,
  SIGPIPEs the producer, and pipefail surfaces that. Capture first, then test.

### Known gap

`could not arm the bootloop guard at /data/local/tmp/glass/boot_pending` — the
`surfaceflinger` SELinux domain cannot write there. Harmless on a dev emulator
(the engine logs and continues) but the production KernelSU module needs a
`sepolicy.rule` granting write access to its state directory, or the guard is
inert exactly when it matters.

## M1 (part 1) — the shader compiles inside SurfaceFlinger

`glass_high.sksl` is compiled by SurfaceFlinger's own Skia, through
`SkRuntimeEffect::MakeForShader` resolved from MiniDebugInfo, and every declared
name resolves:

```
SkSL compiled by SurfaceFlinger's Skia (20852 chars)
  uniform size           offset=  0 type=1 count=1
  uniform offset         offset=  8 type=1 count=1
  uniform crop           offset= 16 type=3 count=1
  uniform cornerExtents  offset= 32 type=3 count=1
  child   blurredContent index=0 type=0
  child   sharpContent   index=1 type=0
shader validated: 32 uniforms + 2 children all resolved
```

This is the ABI proof, not just a compile: `findUniform`/`findChild` are
out-of-line calls into the target, and the offsets they return are consistent
with the declaration order (float2@0, float2@8, float4@16, float4@32). If the
vendored headers disagreed with the target's Skia, the lookups would return
null. It runs **before anything is drawn**, so a mismatch costs a log line
rather than a soft reboot, and the engine refuses to draw unless all 34 resolve.

Uniform offsets coming from `findUniform` also mean the engine never has to
reimplement Skia's uniform packing rules.

### The rule that makes this safe: no undefined symbols, ever

Including Skia headers is not free. **Any Skia call clang cannot inline becomes
an undefined symbol in our .so**, and the runtime linker resolves those against
surfaceflinger's `.dynsym`, which exports essentially nothing. The result is not
a subtle failure — the executable becomes unlinkable and the device bootloops:

```
CANNOT LINK EXECUTABLE "/system/bin/surfaceflinger": cannot locate symbol
"_ZN8SkStringC1ERKS_" referenced by "/system/lib64/libglassengine.so"
```

That came from declaring a function pointer the natural way,
`Result (*)(SkString)`, and calling it with an SkString: ABI-correct, but it
makes clang emit a call to SkString's copy constructor to build the by-value
argument.

**So every bridged signature is modelled in pointers only.** Under the Itanium
C++ ABI a type that is "non-trivial for the purposes of calls" — non-trivial
copy ctor, move ctor or destructor, which covers `sk_sp`, `SkString`, and
anything containing them — is passed as a pointer to a caller-owned temporary,
and returned through a hidden sret pointer as the first argument:

```
Result MakeForShader(SkString)
  ->  void MakeForShader(Result* sret, SkString* byval)
```

Identical machine code, zero emitted calls. `build.sh` links with
**`-Wl,--no-undefined`** so any regression is a build error instead of a
bootloop, and the .so is verified to have zero undefined `Sk*` symbols.

Layout facts used (headers only for sizing and plain data reads, never calls):

| type | layout |
|---|---|
| `SkRuntimeEffect::Result` | `{ sk_sp<SkRuntimeEffect> effect@0; SkString errorText@8 }` |
| `SkString` | `{ sk_sp<Rec> fRec }`, 8 bytes |
| `SkString::Rec` | `{ uint32_t fLength@0; atomic<int32_t> fRefCnt@4; char data@8 }` |

Reading `Rec` directly is what preserves SkSL compiler diagnostics — without
them a shader that fails only inside the compositor is close to undebuggable.

### Compile is lazy, on purpose

The engine's constructor runs before SurfaceFlinger has initialised Skia, so the
shader is compiled on the first `drawBlurRegion` call instead. That also keeps
it off the boot path, and means an ABI mistake surfaces as a crash when blur is
first used — recoverable — rather than at boot.

## M1 (part 2) — the glass is drawn by SurfaceFlinger

`drawBlurRegion` is now **replaced**, not observed. The replacement builds two
child shaders from the compositor's own images, packs a 208-byte uniform blob,
and draws the glass; anything that fails falls through to the original.

### The one measurement that proves the whole chain

With every tone and displacement term neutralised
(`refractionAmountDp=dispersionAmountDp=frost=noise=light=highlightIntensity=tintA=0`,
`saturation=1`, `blurMix=1`) the shader reduces to "show `blurredContent`", which
is exactly what stock draws. Against a stock capture of the same frame:

| region | result |
|---|---|
| deep interior (x 200–880, y 600–1600) | **maxdiff 0** — bit exact |
| left/right rim | diff decays 185 → 0 over exactly **52 px** |
| top/bottom rim | ~34 px |

52 px is `refractionScale.x` = 20 dp × 2.625 to the pixel, and the rim is where
`blurFalloff` hands back to the sharp lens — by design, not residue.

A bit-exact interior means the blurred child's local matrix, the uniform offsets,
the paint, the blend mode and the draw call are **all** correct simultaneously.
Nothing else produces that result by accident.

### getShaderTransform, read out of the target rather than guessed

Disassembling the target's own `drawBlurRegion` (0x248080, 2660 bytes) and
resolving its call targets against MiniDebugInfo gives the exact composition:

```cpp
SkMatrix m = SkMatrix::Scale(scale, scale);
m.postConcat(SkMatrix::Translate(blurRect.fLeft, blurRect.fTop));
SkMatrix inv;                       // identity fast path when the CTM is identity
canvas->getTotalMatrix().invert(&inv);
m.postConcat(inv);
```

The `Translate` is applied **before** the inverse CTM, so `blurRect` is in
**device space** while `effectRegion` is in canvas-local space. That is why
`crop` is not `blurRect`: they are not in the same space. Both children are built
`kClamp` by us, so `crop` is set wide enough never to clip.

The same disassembly gives stock's `SkPaint()` for free — zero everything, then
`fMiterLimit = 4.0f` and `fColor4f.fA = 1`, with `fBitfields` left at 0 (recent
Skia keeps the blend mode in `fBlender`, not the bitfield).

### Two members the target has no symbol for

`SkCanvas::getTotalMatrix` and `SkPaint::setShader` are declared by the pinned
headers but the optimizer inlined every call site and dropped the out-of-line
copy — on both the A14 and A16 images. They are **defined by the engine**, which
is an ordinary out-of-class definition of a declared member and therefore has
legitimate access to the privates involved:

```cpp
SkMatrix SkCanvas::getTotalMatrix() const { return fMCRec->fMatrix.asM33(); }
void SkPaint::setShader(sk_sp<SkShader> shader) { fShader = std::move(shader); }
```

This is *not* a layout guess. `MCRec` is defined in `SkCanvas.h` (line ~2479) and
`fShader` in `SkPaint.h`, so both bodies and every offset come from the same
pinned tree the ROM's Skia was built from. Cross-checked against the target's
compiled code, which loads `fMCRec` at `+0xc40`, reads `fMatrix` at `+0x18`
within it, and picks `fMat[0,4,12,1,5,13,3,7,15]` — exactly `SkM44::asM33`.

`SkPaint()` itself is *not* redefined. `SkPaint::reset()` survived as a symbol and
is documented as "equivalent to replacing SkPaint with the result of `SkPaint()`",
so zeroed storage plus `reset()` builds a paint with SurfaceFlinger's own
defaults and no layout assumption at all.

### Deliberately NOT kSrc

Stock does `if (blurAlpha == 1.0f) paint.setBlendMode(SkBlendMode::kSrc)` because
its shader fills the whole rect opaquely. Ours does not: the SDF leaves the
corners transparent, and kSrc would punch them through to black. The paint stays
at its default kSrcOver, and `blurAlpha` is folded into the `glassOpacity`
uniform instead of the paint's alpha.

### Skia types that cannot be used from outside Skia

`SkData` is allocated in place and declares a **private `operator delete`**, so
`sk_sp<SkData>`'s inline destructor cannot be instantiated in our translation
unit. The engine holds a raw `SkData*` and releases through the resolved
`SkNVRefCnt<SkData>::unref`. `sk_sp<SkShader>` is fine — `SkRefCntBase` has a
virtual destructor, so `delete this` goes through the vtable and emits no symbol.

`SkMatrix::Scale`/`Translate` are header-inline but delegate to `setScale`/
`setTranslate`, which are not. `SkMatrix::MakeAll` is inline all the way down and
is used instead; it leaves the type mask unknown and SurfaceFlinger's own
`postConcat` computes it, because the mask is a cache, not state.

## Measured on the A16 AVD with the engine live

### Cost

RenderEngine timing histogram, 8 recents-open/close cycles, full-screen
1080×2400 region, HIGH tier (16 dispersion taps):

| | mean | mode |
|---|---|---|
| stock | 4.82 ms | 4 ms |
| glass | 5.48 ms | 5 ms |

**+0.66 ms**, worst case (whole screen), on an emulator GPU. Re-measure on the
F1 before drawing conclusions, but it is not obviously unshippable.

### What stock A16 actually asks to blur

Instrumenting every distinct region geometry over volume / recents / shade:

```
draw #2 r=12 a=1.00 region=[0 0 1080 2400] blurRect=[0 0 1080 2400] radii=0.0 ctm=identity
```

The volume panel, the notification shade and the power menu produce **no blur
region at all** on this build — SystemUI draws them opaque. Plus a 128×128
`r=9`/`r=60` pair at SF startup, which is the blur pipeline warming itself up.

The recents backdrop does blur, but only **during the transition**, and it is
worth being precise about what it looks like, because two things there contradict
what the first pass suggested:

```
draw #106 r=80 region=[0 1 1079 2398] radii=23.2 ctm=[1.000 0.000 0.5 / 0.000 1.000 0.0]
draw #107 r=80 region=[0 0 1079 2399] radii=23.1 ctm=[1.000 0.000 0.4 / 0.000 1.000 0.2]
draw #108 r=80 region=[0 0 1080 2400] radii= 0.0 ctm=identity
```

- **Real regions do carry corner radii** (23.2 px ≈ 8.8 dp), so the corner-radius
  floor is a fallback for the settled full-screen case, not the normal path.
- **The canvas matrix is not always the identity.** Mid-animation it carries
  sub-pixel translation. Every region seen while the UI was *at rest* had an
  identity CTM, which is why the first pass concluded wrongly; `getTotalMatrix`
  earns its keep during animation, which is exactly when a wrong matrix would be
  most visible.

Also worth recording: **the only surface that holds a blur region indefinitely is
the harness's own `FLAG_BLUR_BEHIND` probe.** Recents, the power menu and the
shade were each checked by counting geometry-change log lines at 2.5 s and 5 s
after triggering them; only the probe still had one. Any A/B that needs a settled
frame has to use the probe.

Two consequences:

1. `persist.sys.glass.cornerRadiusDp` exists as a **floor** on the SkRRect's own
   radii, because a glass pane with no corner radius reads as a grey box.
2. "Glassify all system blurs" has very little to act on for stock AOSP. The
   value is in M3 — adding blur regions to notifications and the volume panel —
   and on ROMs that already enable more of them.

### Finding: `refractionDepth` does not survive a full-screen region

`direction = normalize(normal + depthEffect * radial + swirl * lobeDir)`, and
`radial` is the normalized vector from the region's centre. On a small pane that
is a mild dome. On a 1080×2400 region the radial term swings the refraction
direction 20–45° off the edge normal along most of the vertical edges, so
horizontal backdrop lines get dragged **vertically** and refract into arcs
instead of compressing straight.

Isolated by sweeping one term at a time against a fixed backdrop:

| refraction | dispersion | rim |
|---|---|---|
| 0 | 0 | straight lines, no displacement |
| 0 | on | straight lines with chromatic fringes — dispersion is correct |
| on | 0 | **arcs** |
| on, `refractionDepth=0` | 0 | straight lines, compressed toward the edge — correct |

So the port is faithful and dispersion is right; `refractionDepth=1` is simply
calibrated for pane-shaped regions. M2 should scale it down as the region grows,
or zero it for full-screen ones.

### The tooling bug this exposed

The property refresh originally rescanned for not-yet-existing properties every
240 draw calls. A region drawn a handful of times per experiment never reaches
the counter, so **a property set for the first time silently did nothing** — the
first `refractionDepth` measurement was invalid because of it. Rescan is now
time-based (500 ms), which bounds both cost and latency independent of frame
rate. Setting a property to the **empty string** resets it to its built-in
default, since Android properties cannot be deleted.

## The interior defaults do not transfer from upstream

Upstream's `frost = 0.36` and `saturation = 1.8` make the compositor path look
murky, and the reason is structural rather than a matter of taste.

Upstream owns the **whole** effect: it applies its own blur and its own frost as
one design. On the `drawBlurRegion` path the blur already exists and its radius
was chosen by whoever requested the region, so layering upstream's dark veil and
1.8× saturation on top re-does work that is already done — and does it to the
entire region.

With `blurMix = 1` the interior is exactly the compositor's Kawase output, and
the tone chain that follows is:

```
saturation 1.8  ->  mix(luma, color, 1.8)              // 80% further from grey
frost 0.36      ->  color*0.64 + (0.029,0.029,0.032)   // a dark veil
```

Measured on one interior crop (150–930 × 380–1180, away from every rim), on the
persistent probe:

| frost / saturation | interior luma vs stock | mean saturation |
|---|---|---|
| stock blur | — | 0.611 |
| 0.36 / 1.8 *(upstream)* | **−26.2 %** | 0.752 |
| 0.20 / 1.35 | −14.6 % | 0.686 |
| **0.12 / 1.15 (shipped)** | **−8.8 %** | 0.644 |
| 0 / 1.0 | −0.0 % | 0.611 |

`0 / 1.0` reproducing stock's luminance and saturation to three decimals is the
same identity proof as the bit-exact interior, arrived at from the other end.

**The engine ships 0.12 / 1.15.** That keeps the pane reading as a surface rather
than a hole while leaving the app's own blur recognisable, and lets the rim —
refraction, dispersion, highlight — carry the glass. This is the **only** number
that does not transfer verbatim from the harness; `harness/.../Params.java` keeps
0.36 / 1.8, which is correct there because the harness owns its own blur.

### Bug found alongside it: frostColor ignores the theme

The shader documents `frostColor.rgb` as *the system background colour*, but the
engine ships `(0.08, 0.08, 0.09)` unconditionally — a **dark-theme** value. On
light-mode content it tints toward dark grey no matter what the theme is.
SurfaceFlinger has no view of the framework theme, so something that does know
has to set `persist.sys.glass.frostR/G/B`. Subtle at 0.12, wrong at any value.

## Android 14 vs Android 16: what each build kept out-of-line

Re-running the host resolver against a real A14 binary (redroid, 7.9 MB, 19,691
FUNC symbols) after the draw path was finished turned up a gap and then a much
better architecture. Both targets now pass.

| entry point | A14 | A16 |
|---|---|---|
| `BlurFilter::drawBlurRegion` | 1471 B | 2660 B |
| `SkRuntimeEffect::makeShader` | ✓ *(fallback mangling)* | ✓ |
| `SkRuntimeEffect::findUniform` / `findChild` / `uniformSize` | ✓ | ✓ |
| `SkImage::makeShader` | ✓ | ✓ |
| `SkData::MakeZeroInitialized` | ✓ | ✓ |
| `sk_sp<SkData>::~sk_sp` | ✓ | ✓ |
| `SkNVRefCnt<SkData>::unref` | **inlined away** | ✓ |
| `SkMatrix::postConcat` / `invertNonIdentity` | ✓ | ✓ |
| `SkPaint::reset` / `~SkPaint` | ✓ | ✓ |
| `SkCanvas::drawRect` | ✓ | ✓ |
| **`SkCanvas::getTotalMatrix`** | **✓ present** | **inlined away** |
| **`SkPaint::setShader`** | **✓ present** | **inlined away** |
| builder surface | ✓ *(as `SkRuntimeShaderBuilder`)* | ✓ |

The `SkSpan<ChildPtr const>` → `SkSpan<ChildPtr>` fallback firing on A14 is the
candidate-list mechanism doing exactly the job it was built for.

### A14 is *less* inlined than A16, which inverts the risk

The two members the engine defines itself — because A16 has no symbol for them —
are **both real functions on A14**. So the honest thing is to prefer the target's
copy and treat our own definition as the fallback, not the default:

```
23062 symbols; builder complete; blob release via sk_sp dtor;
getTotalMatrix OUR DEFINITION; setShader OUR DEFINITION      <- A16
```

That line is printed at install so the first question about a misbehaving target
("which path did it take?") is already answered. On A14 both read `resolved`, and
the engine then depends on **no private object layout at all** for the canvas
matrix or the paint's shader — only on signatures.

This is why the strategy enum grew two more meanings, each with different
semantics and each earned by a measurement rather than a guess:

| strategy | requirement | why |
|---|---|---|
| `kCore` | all must resolve | the draw calls them directly |
| `kDataRelease` | **at least one** of the group | both are implicit template instantiations, so whether either survives is a per-build accident |
| `kOptional` | none required | the engine can implement them, but would rather not |
| `kBuilder` | none required | resolved for information only, never called |

Signature drift is now fatal only for `kCore` and `kDataRelease`. Refusing to
install because an *unused* informational symbol changed shape was theatre.

### sizeof(SkPaint) is 80 on both, with identical offsets

Read out of A14's `SkPaint::SkPaint()` (a real symbol there) and A16's inlined
copy inside `drawBlurRegion`:

```
zero 0x00..0x3B          six sk_sp members + fColor4f.rgb
0x3c  fColor4f.fA = 1.0f
0x40  fWidth      = 0
0x44  fMiterLimit = 4.0f    (movq $0x40800000)
0x48  fBitfields  = 0       (recent Skia keeps the blend mode in fBlender)
```

76 bytes of fields, 80 after padding, on **both**. The engine's pinned headers
agree (`engine believes sizeof(SkPaint)=80 align=8`).

That matters more than it looks. On A14 the engine never reads or writes an
SkPaint field — the target constructs it (`reset`), configures it (`setShader`),
consumes it (`drawRect`) and destroys it (`~SkPaint`). The engine only supplies
**memory**, so the sole requirement is that its buffer is no smaller than the
target's SkPaint. `ScopedPaint` therefore carries 48 bytes of headroom: an
undersized buffer would let `reset()` write past a stack frame inside
SurfaceFlinger, which is the worst failure mode on offer here, and the headroom
costs nothing worth counting.

### Regression check after the rework

Identical to the numbers M1 landed with, to the pixel:

| | before | after |
|---|---|---|
| deep interior, tone neutralised | maxdiff 0 | **maxdiff 0** |
| full frame changed / maxdiff | 261,839 / 215 | **261,839 / 215** |

## Fixing the arcs: paneness

The dome term (`direction = normal + refractionDepth * radial + …`) is what bends
straight backdrop lines into arcs on a full-screen region. `refractionDepth` is
now scaled by how pane-like the region is:

```
t        = clamp(paneSpanDp * density / max(w, h), 0, 1)
paneness = t^4
```

| `max(w,h)` | paneness at 480 dp | effect |
|---|---|---|
| ≤ 1260 px (≤ 480 dp) | **exactly 1** | untouched |
| 1500 px | 0.50 | reduced |
| 2400 px (full screen) | **0.076** | dome effectively off |

The clamp is what makes a steep curve safe. Any region whose longest side is at
or under the threshold gets `t == 1`, and 1⁴ is still 1, so pane-sized regions
are unaffected *by arithmetic* rather than by measurement. That lets the falloff
be as aggressive as the full-screen case needs. 480 dp sits well above a volume
panel or a notification row and well below a full-screen scrim.

Fourth power was chosen by measurement, not taste: squaring only reached 0.28 on
a 2400 px region, which still visibly arcs.

| depth on a 1080×2400 region | rim |
|---|---|
| 1.0 (raw upstream) | pronounced arcs sweeping across the band |
| 0.28 (squared) | reduced, still arcing |
| **0.076 (⁴)** | **straight lines with edge compression, dispersion intact** |

Regression after the change, with the effective parameters read back from the
engine's own log rather than assumed:

```
frost=0.00 sat=1.00 refr=0 disp=0 paneness=0.076 depth=0.076
deep interior : changed=0 maxdiff=0
full frame    : changed=261839 maxdiff=215
```

Unchanged from M1, to the pixel.

## Measurement hygiene, learned twice

Two separate wrong conclusions this session came from the same root cause: **a
screenshot taken before a property landed looks exactly like a property that had
no effect.**

1. The property rescan was originally driven by a draw-call counter. A region
   drawn a handful of times per experiment never reached it, so a property set
   for the *first* time silently did nothing. Now time-based (500 ms).
2. A batch of captures reported that `paneSpanDp` had broken the interior's
   bit-exactness. It had not — one capture in the pair predated a `setprop`.
   Chasing it produced a long and entirely fictional theory about `depthEffect`
   leaking into the interior.

`direction` is used at exactly two places in the shader, both inside the lens
branch and both scaled by `refractionAmount` / `dispersionAmount`. With those at
zero, `refractionDepth` **provably cannot** change a pixel — which is what the
re-run showed once the captures were honest.

The durable fix is that every geometry-change log line now carries the parameters
the shader actually received:

```
draw #12 r=80 region=[0 0 1080 2400] radii=0.0 ctm=[...]
  | frost=0.12 sat=1.15 refr=158 disp=16 paneness=0.076 depth=0.076
```

and the line is emitted when those parameters change, not only when the geometry
does. Any A/B whose two log lines report the same parameters is measuring
nothing, and that is now visible instead of inferred.

## The rim ignored the blur radius the app asked for

Measured with mean |Laplacian| — how much fine detail survives in a patch — on a
full-screen region at radius 80, noise disabled so the metric reads content
rather than grain:

| patch | stock | glass (before) | ratio |
|---|---|---|---|
| rim, 0–52 px | 0.650 | **5.762** | **8.9×** |
| rim, 52–104 px | 0.894 | 0.860 | 1.0× |
| interior | 0.769 | 0.742 | 1.0× |

The band carrying 9× the detail of the blurred content around it is exactly
`refractionScale.x` wide, and everything beyond it matches stock. The cause is
that the lens reads `sharpContent` at every branch, so the refraction band never
sees the blur the compositor produced.

Two defects, one fix. The second is subtler: the frost handover was
cross-dissolving `lens` (at the **displaced** position) against `fr` (at the
**undisplaced** position), so the transition blended two geometrically different
images.

`lensBlur` folds the blurred child back into the lens **at the position the lens
actually read from**, which fixes both:

```glsl
if (lensBlur > 0.0) {
    float4 lb = sampleBlurred(lensPos);
    lens  = mix(lens,  lb.rgb, lensBlur);
    lensA = mix(lensA, lb.a,   lensBlur);
}
```

One extra texture read, taken only inside the rim band and only when the value is
above zero.

| lensBlur | rim HF | vs stock |
|---|---|---|
| 0 | 5.840 | 9.0× |
| 0.5 | 3.342 | 5.1× |
| 0.8 | 1.786 | 2.8× |
| 1.0 | 0.865 | **1.3×** |

### It adapts to the radius rather than being pinned

`blurRadius` is an argument to `drawBlurRegion` that the engine previously
discarded. A **negative** `lensBlur` (the default) derives it instead:

```
lensBlur = clamp(blurRadius / lensBlurRef, 0, 1)      lensBlurRef = 80
```

An app asking for radius 80 wants what is behind it hidden, and revealing sharp
pixels in a 52 px band around the edge works directly against that. An app asking
for radius 12 wants a light veil, and a crisp refractive lens there costs it
nothing. Any value in [0, 1] pins it explicitly.

Verified by reading the engine's own log rather than inferring:

| setting | region | reported |
|---|---|---|
| default, `lensBlurRef` 80 | r=80 | `lensBlur=1.00` |
| `lensBlurRef` 160 | r=80 | `lensBlur=0.50` |
| explicit `lensBlur` 0.25 | r=80 | `lensBlur=0.25` |

Result at r=80: rim **8.9× → 1.3×**, interior untouched at 0.742.

### A sharper identity proof fell out of it

With displacement off and `lensBlur` at 1, the lens reduces to the blurred child
at the undisplaced position — which is exactly what stock draws. The full-frame
difference against stock collapsed from **261,839 px / maxdiff 215** to
**4,175 px / maxdiff 34**, and the residual is the SDF's own ~2 px antialiased
border (region perimeter ≈ 6,960 px). Stock draws a hard rect; this shader
feathers its own edge. Everything else is an exact identity.

### Measurement environment, the third trap

A stuck Quick Settings panel — left open by an earlier `cmd statusbar
expand-notifications` and not dismissed by `collapse` or HOME — covered the
harness for three consecutive measurements. The numbers looked plausible enough
to reason about: a "settled" blur radius of 22 instead of 80, `lensBlur 0` and
`lensBlur auto` producing byte-identical output, and a stock reference with a rim
HF of 0.003. Restarting SystemUI fixed it.

Captures are now asserted rather than trusted. Each one checks that a
`draw #` line exists for it and that the frame's standard deviation is above a
floor, and says `<-- FLAT` when it is not:

```
R2_off   r=22 lensBlur=0.00   stddev=3.3  <-- FLAT, environment is wrong
R3_off   r=80 lensBlur=0.00   stddev=53.4
```

Two other things worth keeping: the harness's blur radius **animates**, so a
capture taken too early reads a transient radius rather than the settled one; and
`noise` (default 0.05) dominates any high-frequency metric, so it must be zeroed
before measuring sharpness — it swamped this measurement twice.

## lensBlurRef was 6.7x too high, because Kawase saturates

Reported from a device: the sharp band was still obvious on the notification
shade after `lensBlur` shipped. It was not reproducible on the A16 AVD — the
shade produces **no blur region at all** there, confirmed by a zero draw-call
count across `cmd statusbar expand-notifications` and a 1.5 s manual drag. So the
diagnosis had to come from the mechanism rather than the screen.

The mechanism: `lensBlur = clamp(radius / lensBlurRef, 0, 1)` rises linearly,
but **the compositor's blur does not**. Sweeping the radius a window actually
requests, with the engine bypassed so only stock blur is measured:

| requested radius | backdrop HF | blur strength |
|---|---|---|
| 4 | 6.232 | 0% |
| 8 | 2.382 | 62% |
| **12** | **0.618** | **90%** |
| 16 | 0.608 | 90% |
| 22 | 0.605 | 90% |
| 32 | 0.598 | 90% |
| 48 | 0.590 | 91% |

Step to step: −61.8%, −74.1%, then **−1.5%, −0.5%, −1.1%, −1.4%**. The blur is
done at radius 12 and every larger request buys nothing, which is the `radius/6`
pass-count rule running out of passes.

`lensBlurRef` was 80 — the radius a background-blur *window* asks for. That put
the crossover 6.7x beyond where the blur actually finishes, so across the entire
useful range the backdrop was fully blurred while the rim was mostly sharp:

| requested radius 22 | rim HF | vs stock |
|---|---|---|
| stock blur | 0.452 | 1.0x |
| `lensBlurRef` 80 (before) | 4.330 | **9.6x** |
| **`lensBlurRef` 12 (measured)** | 0.688 | **1.5x** |

At radius 22 the old default gave `lensBlur=0.28` against a backdrop that was
already 90% blurred. Legible text with visible chromatic fringing sat in a 52 px
band around everything the system blurred.

**`lensBlurRef` now defaults to 12**, the measured saturation point, so the rim
reaches full blur exactly when the compositor's does.

The earlier note that "an app asking for radius 12 wants a light veil, and a
crisp lens there costs it nothing" was wrong, and wrong in a specific way worth
keeping: radius 12 is not a light veil, it is *maximum* blur. The reasoning
treated the requested radius as a proxy for blur strength when the two stop
tracking almost immediately.

### Caveat carried forward

Saturation at 12 was measured on this build's Kawase path on an emulator GPU. The
`radius/6` rule is in the shader source, but the pass cap is a per-build
constant. Re-run the sweep on the F1 before trusting 12 there — the sweep is now
one command, since `probeBlurRadius` is a harness parameter.

## Injection: why it is a DT_NEEDED entry and not LD_PRELOAD

`LD_PRELOAD` would need no ELF patching, no on-device tooling and no 8 MB copy.
It does not work, and the reason is structural rather than incidental.

Appending `setenv LD_PRELOAD /system/lib64/libglassengine.so` to
`/system/etc/init/surfaceflinger.rc` and rebooting: the device boots, the edit
survives, and the engine never loads. `/proc/<sf>/environ` has no `LD_PRELOAD`
in it at all — not ignored, *absent*. Because:

```
/proc/<surfaceflinger>/auxv:  AT_SECURE = 1
```

init starts surfaceflinger with `capabilities SYS_NICE`, so the kernel marks the
process as secure-execution, and bionic's linker both ignores `LD_PRELOAD` and
strips it from the environment. Nothing in the module's reach changes that
short of removing the capability, which would break the compositor's scheduling.

So injection is a `DT_NEEDED` entry added to a **copy** of the binary, which the
module mounts over the original. The ordinary linker then loads the engine at
process start: no ptrace, no startup race, no SELinux rules, and it survives
every restart of the compositor for free.

### The mount lands before the compositor starts

From the device's own `init.rc`, not from assumption:

```
on late-init
    trigger post-fs-data      <- module scripts, then the magic mount
    ...
    trigger boot
on boot                       <- line 1104
    class_start core          <- line 1215, starts surfaceflinger
```

`class_start core` sits inside `on boot`, and `post-fs-data` is triggered before
it. So an overlay written by `post-fs-data.sh` is mounted before
`/system/bin/surfaceflinger` is ever executed.

### patchelf runs on the device

The module cannot ship a pre-patched binary — every ROM has its own — so the
patcher has to run on the device. patchelf has no dependencies beyond a C++17
compiler, so its single translation unit cross-compiles directly with the NDK,
skipping autotools:

| ABI | size |
|---|---|
| arm64-v8a | 843,072 |
| x86_64 | 828,264 |

Verified on the emulator against a real `surfaceflinger`: `--add-needed`
succeeds, and `--print-needed` reports `libglassengine.so` first.

## The module, exercised on a real device

KernelSU is not installed on the AVD, so the magic mount itself is untested. Every
script was run against the real device with `MODDIR` pointed at a staged copy:

| path | result |
|---|---|
| preflight on a real `surfaceflinger` | `OK`, exit 0 |
| preflight on `/system/bin/logcat` | lists 15 missing entry points, exit 1 |
| overlay from a pristine source | built, **exactly one** `DT_NEEDED` |
| contexts on the overlay | `surfaceflinger_exec` / `system_lib_file` |
| second boot, binary unchanged | `overlay is current`, copy skipped |
| boot that never completed | overlay **removed**, module disabled |
| boot after that | stays disabled until the marker is cleared |
| marker cleared, boot completes | `service.sh` disarms, overlay kept |

### A duplicate DT_NEEDED, caught by testing against a hostile input

The first run was made against a `/system/bin/surfaceflinger` that was already
patched by the development installer, and it added a **second**
`libglassengine.so` entry. Production would not hit that, since the source is
pristine — but "patch an already-patched binary" compounds every time it happens
and grows the file on each pass.

The script now checks the source before adding, and verifies afterwards that
there is **exactly one** entry rather than at least one. A missing entry boots
fine and silently does nothing, which is the hardest failure to notice; a
duplicate is the signature of the script having run against its own output.

## The bootloop guard moved out of the engine

The engine's in-process guard cannot catch the failure that matters most. When
the injected library is unloadable the dynamic linker rejects surfaceflinger
*before any engine code runs* — which is exactly what the `libc++_shared.so`
bootloop was — so nothing inside the engine is ever in a position to disarm
anything.

The module's guard has no such blind spot: `post-fs-data.sh` writes the marker
before the compositor starts and can refuse to build the overlay at all, and
`service.sh` clears it only once `sys.boot_completed` lands. Neither depends on
the engine running.

This also dissolves the `sepolicy.rule` that was previously needed. The engine
could not write `/data/adb/glass` from the `surfaceflinger` domain; the module's
scripts run as root at post-fs-data and simply can. When the module is present it
drops a `/data/adb/glass/module` marker and the engine's own guard stands down,
so there are never two markers meaning the same thing. The in-engine guard
remains only for the development install, which patches `/system` directly and
has no scripts.

## frostColor now follows the theme, from outside the compositor

SurfaceFlinger has no view of the framework's theme — it is several layers below
anything that knows what Material You is doing — so the value has to arrive from
outside, and the property channel the engine already listens on is the obvious
route.

`cmd uimode night` is the signal, not `settings get secure ui_night_mode`: the
setting can be `null` or `auto`, while the command reports the state actually in
effect after schedules and overrides resolve.

| | `cmd uimode night` | `settings get secure ui_night_mode` |
|---|---|---|
| default | `no` | `null` |
| after `night yes` | `yes` | `2` |
| after `night no` | `no` | `1` |

`glass-theme.sh` polls it every 15 s and writes `frostR/G/B` only when the answer
changes, so the engine's own re-read stays idle too. Measured end to end, through
the property, the engine and the shader:

| theme | frostRGB | glass interior luma |
|---|---|---|
| light | 0.95 / 0.95 / 0.97 | 95.6 |
| dark | 0.08 / 0.08 / 0.09 | 73.9 |

**+29.4%**. Set `persist.sys.glass.follow_theme` to 0 to manage the colour by
hand.

## Which regions to glassify: no opt-out needed

The open question was whether a full-screen scrim should be treated as a pane at
all. It resolved by the effect degrading gracefully rather than by adding a
switch, and each of the three mechanisms was earned by a measurement:

- **paneness** takes the dome term to 0.076 on a full-screen region, so long
  edges refract along their own normal.
- **auto lensBlur** reaches 1 at radius 12, so the rim honours whatever blur the
  app asked for instead of revealing sharp content.
- **frost 0.12 / saturation 1.15** leave the app's own blur recognisable.

What is left on a full-screen scrim is a light frost, a slight colour lift and a
subtle refracting edge — unobtrusive, and correct for something that is a scrim
rather than a pane. A hard size cutoff would buy nothing that these three do not
already, and would add a knob whose right value nobody could justify.

There is also a positive reason **not** to skip small regions. SurfaceFlinger
warms its blur pipeline at startup with a 128×128 region at radius 9 and 60,
which is what triggers the engine's lazy shader compile — at boot, off the
critical path, rather than during the first user-visible animation. Skipping
small regions would move a ~300 ms SkSL compile into a transition.

## Android 16 QPR2 changes drawBlurRegion

Found on a Poco F1 running Project Infinity-X 3.12 and initially written up here
as a vendor patch. **That was wrong.** Unpacking LineageOS 23.2 for alioth — a
different device, a different maintainer — and pulling its `surfaceflinger` out
of the OTA payload shows the same thing:

| build | QPR | arch | `drawBlurRegion` argument encoding |
|---|---|---|---|
| AVD, BE2A.250530 (AOSP) | QPR0 | x86_64 | `…SkRRect`**`jf`**`RK6SkRect…` |
| redroid, Android 14 | — | x86_64 | `…SkRRect`**`jf`**`RK6SkRect…` |
| Poco F1, Infinity-X 3.12 | QPR2 | arm64 | `…SkRRect`**`jff`**`RK6SkRect…` |
| Poco F3, LineageOS 23.2 | QPR2 | arm64 | `…SkRRect`**`jff`**`RK6SkRect…` |
| **Pixel 9 Pro, stock Google** | **QPR2** | arm64 | `…SkRRect`**`jff`**`RK6SkRect…` |

The last row settles it. `google/caiman/caiman:16/BP4A.260105.004.E1/…:user/release-keys`
is Google's own signed release, pulled straight out of the factory image, and it
carries the same shape as the two LineageOS-family ROMs. So this is **AOSP
evolution at QPR2**, not something a vendor added, and the shape the engine was
originally written against is now the minority one — the QPR0 AVD is the outlier
among current devices, not the reference.

The Pixel build agrees on everything else too: `KawaseDualFilterV2` present, and
`invertNonIdentity`, `getTotalMatrix` and `setShader` all inlined away. Every
QPR2 characteristic found on the Poco F1 is stock Android.

The same split shows up twice more:

- **`KawaseDualFilterV2`.** All three QPR2 builds carry
  `RenderEngine::BlurAlgorithm::KawaseDualFilter` and `...V2` enum strings. The
  QPR0 AVD carries **neither**. So the blur algorithm changed at QPR2 too, which
  matters because `lensBlurRef = 12` was measured against the QPR0 filter and
  cannot be assumed to transfer.
- **`SkMatrix::invertNonIdentity` is inlined away** on all three QPR2 builds and
  present on both older ones.

Whether `blurScale` is what produces the "overextended edge" look people
associate with QPR2 blur is a reasonable guess — scaling the sampled content
about the region's centre would pull in content from further inside and hide a
dual-filter edge artifact — but it is a guess, not a measurement. The engine now
logs the value so it can be settled by observation.

### What the extra float is

Read out of the F1's compiled code at 0x232538:

| register | role | evidence |
|---|---|---|
| `s0` (1st float) | a scale | builds `SkMatrix::Scale(s,s)`, postConcats it, then offsets by `(1-s)*size*0.5` — a scale about the region's centre. `if (s == 1.0)` skips the whole block, so 1.0 is the default. |
| `s1` (2nd float) | **`blurAlpha`** | `min(x, 1.0)` then `max(x, 0.0)`, stored into the paint block. That is `setAlphaf` exactly. |

Worth being precise about the danger, because the obvious reading is wrong: on
**AArch64 and x86-64 SysV, floats live in their own register file**, so the extra
float does *not* shift the integer arguments — the canvas, rect and image
pointers all stay put. What would actually have broken is subtler: reading
`blurAlpha` from the wrong float, and calling the original with one float where
it expects two, leaving the callee to read a stale register.

The engine now carries both shapes as a `kHookTarget` group — alternatives with
*exactly one* expected to resolve, since the argument lists genuinely differ and
each needs its own replacement function. `blurScale != 1.0` is a shape the glass
would silently get wrong, so it is handed back to stock.

### SkMatrix::invertNonIdentity

It was `kCore`, so every QPR2 build was unusable over one arithmetic helper. It is now `kOptional` with a general adjugate/determinant 3×3
inverse in the engine — a dozen lines, handles perspective, and gives up nothing
by not calling Skia's.

### Where the three targets now stand

| | hook shape | `getTotalMatrix` / `setShader` | `invertNonIdentity` | blob release |
|---|---|---|---|---|
| AVD A16 x86_64 | `(radius, alpha)` | inlined → ours | resolved | both |
| redroid A14 x86_64 | `(radius, alpha)` | **resolved** | resolved | `sk_sp` dtor |
| **F1 Infinity-X A16 QPR2 arm64** | **`(radius, scale, alpha)`** | inlined → ours | **inlined → ours** | both |
| **F3 LineageOS 23.2 QPR2 arm64** | **`(radius, scale, alpha)`** | inlined → ours | **inlined → ours** | both |

All four pass, and no two of the first three take the same path through the
bridge — which is the point of the strategy groups. The two QPR2 builds take an
identical path, as they should.

### Verified on the F1, without installing anything

| step | result |
|---|---|
| `glasscheck` against its `surfaceflinger` | `OK`, 26,143 symbols, scaled shape identified |
| arm64 `patchelf` on the real 9.6 MB binary | patched, **exactly one** `DT_NEEDED` |
| SELinux contexts on the overlay | `surfaceflinger_exec` / `system_lib_file` |

Two things noted for later:

- **`persist.sys.sf.disable_blurs = 1`.** Blurs are currently switched off on
  this device, so a successful install would have nothing to act on until that
  is 0.
- **`RenderEngine chosen blur algorithm: KawaseDualFilterV2`** — the QPR2
  algorithm, not the one `lensBlurRef = 12` was measured against. The sweep needs
  re-running on a QPR2 device.

## KernelSU module mounting was dead on the test device

The module installed cleanly on the Poco F1 (KernelSU-Next 3.3.0), the preflight
passed, the reboot succeeded and the guard disarmed itself — and the engine was
still not loaded.

The overlay was built correctly:

```
/data/adb/modules/stockglass/system/bin/surfaceflinger   9,858,609   (patched)
/system/bin/surfaceflinger                               9,644,200   (original)
```

`post-fs-data.sh` logged `overlay built`, `boot_pending` was cleared by
`service.sh`, and surfaceflinger's own `/proc/<pid>/mountinfo` contained **zero**
module mounts — in the same mount namespace as the shell, so this was not
KernelSU's per-process namespacing hiding it.

The tempting conclusion was an ordering bug on our side: the module arrives in
`modules_update`, gets swapped in at boot, and our `system/` directory is created
by `post-fs-data.sh` — so if ksud built its mount plan before running the
scripts, a first-boot install would be missed. **That was not it.** The other
module installed on the device is not mounted either:

```
/data/adb/modules/ViPER4AndroidFX/system/etc/audio_effects.xml  7649  660b3a8d…
/system/etc/audio_effects.xml                                   7544  1a85369a…
```

Different size, different hash, both modules enabled, no `skip_mount`. Module
system-overlay mounting simply does not work on that device's kernel — which
also explains why its ViPER4Android had never worked.

**Nothing here is evidence about the module.** Everything short of the mount is
verified on real hardware: the preflight correctly identified the QPR2 hook
shape, arm64 patchelf produced exactly one `DT_NEEDED` on the real 9.6 MB binary,
the contexts were right, the boot completed and the guard armed and disarmed on
schedule. The mount is the one link still untested, and it needs a device whose
KernelSU actually performs it.

Worth keeping anyway: building the overlay in `customize.sh` at install time,
rather than in `post-fs-data.sh` on first boot, would remove the ordering
question entirely and cost nothing.

**Explained, 2026-11-17, on alioth (KernelSU 3.3.0, ksud `3.3.0-131-g8cd02474`).**
The same thing happened: installed, booted, guard disarmed, `overlay is
current`, and `/system/bin/surfaceflinger` still the original 9,805,336 bytes
with no `libglassengine.so` in `/system/lib64`. KernelSU 3.0 moved module
mounting out of `ksud` into a separately installed *metamodule* (meta-overlayfs
is the reference one). With none installed, modules are installed but not
mounted. `/data/adb/metamodule` did not exist on the phone, and `ksud` carries
the strings that say so (`No metamodule found`, `metamount.sh`). The F1's
symptom, where no module of any kind was mounted, fits the same cause. Since
0.4 the installer, the boot log, `glassctl` and the WebUI all report it instead
of failing silently.

A second install bug surfaced on the same device and is fixed: KernelSU's unzip
does not keep exec bits, and `customize.sh` ran `build-overlay.sh`, which
requires an executable `patchelf`, before `set_perm` had made it one. The first
install aborted with `no patchelf for arm64-v8a`.

## Android 17

Two targets, both brought up without the phone:

- **crDroid 13.1 for alioth** (Poco F3, arm64), `ro.build.version.release=17`,
  security patch 2026-11-01. `surfaceflinger` pulled from the OTA with
  `engine/test/ota_extract.py` (the system image is ext4, so `debugfs` dumps it).
- **The A17 AVD** (`android17-headless`, x86_64, `CE2A.260420.019` userdebug),
  where the engine actually ran.

### Resolution

| | crDroid 13.1 A17 arm64 | A17 AVD x86_64 |
|---|---|---|
| recovered FUNC symbols | 26,232 | 26,088 |
| hook shape | `(radius, scale, alpha)` @ 0x295240, 2680 B | `(radius, scale, alpha)` @ 0x2bd210, 3018 B |
| `getTotalMatrix` / `setShader` | inlined → ours | inlined → ours |
| `invertNonIdentity` | inlined → ours | inlined → ours |
| blob release | both | both |
| `engine/test/run.sh` | PASS | PASS |

Identical path to the A16 QPR2 builds. Nothing in the symbol table needed to
change for A17.

### Layouts: the A16 pin is valid for A17

`engine/test/layout/layout_check.sh` compiles every layout fact the engine uses
(SkPaint, SkCanvas::fMCRec, MCRec::fMatrix, SkRRect, SkData, SkString,
SkRuntimeEffect::Result/Uniform/Child, SkSamplingOptions) against both header
trees, for both ABIs. Pinned `android16-release` vs `android17-release`
@ `508606366e71`: **every row identical**.

The one source change on that surface is `SkData`: `const void* fPtr; size_t
fSize;` became `SkSpan<std::byte> fSpan;`. SkSpan is `{pointer, length}`, so the
data pointer is still at +24 and `writable_data()` from the A16 headers reads the
right word.

Cross-checked against crDroid's own compiled `drawBlurRegion`:

```
29536c:  ldr  x8, [x19, #0xc40]      fMCRec
295374:  ldp  s0, s1, [x8, #0x18]    fMatrix: fMat[0], fMat[1]
...      +0x24 +0x28 +0x2c +0x34 +0x48 +0x4c +0x54
29537c:  str  w9 (=0x80), ...        kUnknown type mask
```

The nine reads are exactly `SkM44::asM33`, which is what the engine's own
`getTotalMatrix` compiles to.

### Live on the A17 AVD

```
hook target: drawBlurRegion(radius, scale, alpha)
26088 symbols; builder complete; blob release via sk_sp dtor; getTotalMatrix OUR DEFINITION; setShader OUR DEFINITION
hook installed in 92 ms
mode: glass, preset regular, 0 properties overridden
SkSL compiled by SurfaceFlinger's Skia (22596 chars)
shader validated: 33 uniforms + 2 children resolved, blob 212 bytes, density 2.625 px/dp
```

The A17 AVD also chooses `KawaseDualFilterV2`.

## Glass is a mode, and stock mode is stock

`persist.sys.glass.mode` defaults to stock. In stock mode the replacement calls
the original with its own arguments before anything else of ours runs, and the
shader is not compiled.

Measured on the A17 AVD, blur-behind probe, three captures each:

| comparison | changed px | maxdiff |
|---|---|---|
| stock mode vs stock mode | 0 | 0 |
| stock mode vs hook not installed (`enabled=0`) | 10 | 2 |

The 10 pixels are the gesture-nav pill at the bottom edge
(432–649 × 2396–2404), mid-fade after the app launch in one capture and not the
other. Every other pixel is identical. `SkSL compiled` never appeared in the log
until glass was first selected.

## zoomScale: the A16 QPR2 float is honoured, not handed back

AOSP `android16-qpr2-release` and `android17-release` name the extra float
`zoomScale`. Stock applies it to both children:

```
blurred: Scale(4) → postConcat Translate(blurRect.topLeft)
         → postScale(z) → postTranslate(w(1-z)/2, h(1-z)/2) → postConcat inverse(ctm)
sharp:   inverse(ctm).preTranslate(w(1-z)/2, h(1-z)/2).preScale(z)
```

The engine previously handed every region with `zoom != 1` back to stock. On A17
that covers **the notification shade**:

```
r=1  zoom=1.000      r=2  zoom=0.999      r=34 zoom=0.975      r=65 zoom=0.952
```

so on Android 17 the shade would never have been glass. `blurredTransform` and
`sharpTransform` now reproduce both matrices. With every glass term neutralised
(interior = blurred child), the open shade at `zoom=0.952` against stock:
**397 of 2,344,000 px differ, all of them UI** (the clock ticking 12:51→12:52,
"2m"→"3m", one signal-bar icon). The blur itself is identical.

## Bug fixed: the sharp child was offset by blurRect's origin

`input` is a snapshot of the **whole surface**, at the device origin, while
`blurredImage` covers only `blurRect`. Stock's crossfade maps `input` with the
inverse canvas matrix only. The engine used the blurred child's recipe at scale 1,
which includes `Translate(blurRect.fLeft, blurRect.fTop)`, so the lens sampled
content shifted by the region's position.

Every earlier measurement used a full-screen region with `blurRect` at (0, 0),
which is why it never showed. A pane does not:

```
draw r=80 region=[0 0 864 808] blurRect=[108 808 972 1616] radii=56.0 ctm=[1 0 108 / 0 1 808]
```

With every glass term at zero the shader shows only the sharp child, which must
match the unblurred backdrop. Pane interior, 532,000 px:

| engine | px off by > 2 | maxdiff | mean |
|---|---|---|---|
| v0.2 | 531,849 | 247 | 55.76 |
| fixed | 1,550 | 4 | 0.02 |

On a real device this would have shown as a refracting rim imaging content from
the wrong place on every panel and dialog not at the top-left of the screen,
most visibly at low radii where `lensBlur` leaves the rim sharp.

## The glass fades in the way stock's blur does

Stock cross-fades `input` into the blurred image by `radius / mMaxCrossFadeRadius`
while the radius is below it: 10 for Kawase and KawaseDualFilterV2 (the
`BlurFilter` default), 0 for Gaussian. A shade pull starts at radius 1. The glass
used to arrive at full strength on that first frame.

Every term is now scaled by `fade = clamp(radius / crossFadeRadius, 0, 1)`:
refraction, dispersion, highlight, light, grain, frost and tint alpha, blurMix and
lensBlur, with saturation as `1 + (s - 1) · fade`. At fade 0 the shader reduces to
the sharp snapshot at the undisplaced position, which is what stock draws at
radius 0.

`getMaxCrossFadeRadius()` is inlined away on both A17 builds, and reading the
member out of `this` would mean depending on a private RenderEngine layout that no
header pins, so `crossFadeRadius` is a parameter defaulting to 10.

Pane probe, regular preset, against the unblurred backdrop:

| | region | px off by > 2 | maxdiff | mean |
|---|---|---|---|---|
| fade 0 | whole pane, rim included | 1,785 / 684,800 | 5 | 0.02 |
| fade 0.5 | inside the rim | 401,545 / 511,872 | 170 | 11.34 |

## Presets, measured

Pane probe, deep interior (170 px in from every edge), against stock:

| preset | changed | maxdiff |
|---|---|---|
| `clear` | **0 / 245,232** | **0** |
| `regular` | 244,811 | 18 |
| `frosted` | all | 35 |
| `prism` | all | 21 |

`clear` is bit-exact inside, so it is literally Android's blur with glass at the
edge. The other three were judged by eye on the probe, not calibrated.

## A pane-shaped probe needs a translucent theme

`Window.setBackgroundBlurRadius` produced no region at all with
`Theme_Material_Dialog_NoActionBar` plus `PixelFormat.TRANSLUCENT`. With
`Theme_Translucent_NoTitleBar` (`windowIsTranslucent=true`) and a rounded
`GradientDrawable` background (28 dp corners) it produces an off-origin region
with real corner radii (`radii=56.0` in the draw log).
`probePane=1` in the harness params selects it; it is the only way found to get a
settled region whose `blurRect` does not start at (0, 0).

## Measurement trap: `stop`/`start` does not reset boot_completed

`adb shell stop; adb shell start` restarts the framework but leaves
`sys.boot_completed=1`, so waiting on it returns at once and the next capture is
of a half-started system ("Activity class does not exist"). One v0.2
before/after capture had to be thrown away for exactly this. Wait for
`pm path <pkg>` to answer and `init.svc.bootanim` to read `stopped` instead.

## Bug fixed: arm64 returns non-trivial types through x8

Every Skia call that returns an `sk_sp` or an `SkRuntimeEffect::Result` returns
it indirectly, because those types are non-trivial for the purposes of calls.
The bridge had modelled that as an explicit first parameter, the return slot.
That is the x86-64 convention, where the hidden pointer goes in `rdi` like a
first argument, so every AVD run was correct. AArch64 is different: the
indirect-result pointer goes in **x8**, and the arguments start at x0 as if
there were no result at all.

On arm64 that would have shifted every argument of `MakeForShader`,
`SkImage::makeShader`, `SkData::MakeZeroInitialized` and
`SkRuntimeEffect::makeShader` by one register on the first glass frame. It was
caught by disassembly, before any arm64 load:

For `SkImage::makeShader(tx, ty, sampling, matrix)`:

| | x8 | x0 | next |
|---|---|---|---|
| before the fix | whatever was left there (the function pointer) | the return slot | x1 the image, w2/w3 the tile modes |
| after the fix | the return slot | the image | w1/w2 the tile modes |
| crDroid's own caller, `0x295400` | the return slot | the image | w1/w2 the tile modes |

The fix declares each of these functions as *returning* `Returned<N>`, a type
of the right size whose destructor is user-provided, and is therefore returned
indirectly by the platform's own rule. The compiler then puts the slot wherever
the target ABI says, and C++17's guaranteed elision constructs the result in
place. `SkCanvas::getTotalMatrix` returns a trivially copyable `SkMatrix` and is
now declared as exactly that.

## Sampling `input` is safe on Ganesh, and only on Ganesh

The glass samples `input`, the sharp snapshot of the surface, while drawing
into that same surface. RenderEngine sometimes takes it with
`makeTemporaryImage()` rather than `makeImageSnapshot()`. On Ganesh,
`SkSurface_Base::onMakeTemporaryImage` is a tail call to `makeImageSnapshot`,
which is copy-on-write: the first draw into the surface copies the image out
from under it. Only Graphite overrides it with a genuinely temporary image,
which would alias the surface from the second blur layer of a frame onward. The
engine stands down when `debug.renderengine.graphite` is `true` or the backend
property names Graphite.

The same copy-on-write is what makes the lighter backdrop blur safe (next
sections): A17's `generateTemporaryImage` warns that its result is valid only
until the next call, and KawaseDualFilterV2 returns
`surfaces[0]->makeTemporaryImage()` from preallocated buffers, so on Ganesh the
image it returned keeps its pixels when the filter draws into those buffers
again.

## Android 17 on alioth: what is a blur region

Poco F3, crDroid 13.1 (Android 17), Adreno 650, read-only over wireless adb.
`dumpsys SurfaceFlinger` gives counts per layer. `dumpsys SurfaceFlinger
--proto` would give geometry, but this build writes no `blur_regions` into it,
so the regions were told apart by changing what is on screen.

| on screen | layer | backgroundBlurRadius | blurRegions |
|---|---|---|---|
| shade, 2 notifications | `VRI-NotificationShade`, 1080×2400 | 77, scale 0.95 | 2 |
| shade, a 3rd notification posted | same | 77 | **3** |
| Quick Settings fully expanded | same | 77 | 3 (cards scrolled away, still regions) |
| volume panel | `VRI-VolumeDialog`, 396×2325 at (684, 75) | none | 2 |

- The shade is **one full-screen background blur**, and every notification card
  is **its own blur region in the same layer**. One card, one region.
- **Quick Settings tiles are not blur regions.** They are translucent views with
  no blur of their own, so the compositor never sees them.
- The volume panel has no window blur; its two regions are the ringer pill and
  the slider panel.
- Blur algorithm: `KawaseDualFilterV2`.

The A17 AVD differs: its shade blurs at 89 and carries no per-card regions.

`glasscheck` on alioth's own `surfaceflinger`, as root:

```
26232 function symbols recovered
hook     BlurFilter::drawBlurRegion (scaled)
hook     KawaseBlurFilter::generateTemporaryImage (optional)
hook     GaussianBlurFilter::generateTemporaryImage (optional)
hook     KawaseBlurDualFilterV2::generateTemporaryImage (optional)
OK    every required entry point resolved
```

## Backdrops and panes, on the A17 AVD

A region whose on-screen rect covers at least 90% of the display in both
directions is a backdrop; anything else is a pane. The engine logs what it
decided:

```
draw #381 backdrop/tone r=80 (blurred at a lower radius) ... device=[0 0 1080 2400] of 1080x2400
draw #427 pane r=80 ... device=[108 800 972 1600] of 1080x2400 ... radii=73.5
draw #537 backdrop/tone r=69 ... zoom=0.961 ...                     (the real shade, mid-animation)
draw #608 pane r=40 ... device=[54 912 1026 1201] ... over backdrop
```

| check | result |
|---|---|
| blur-behind probe, glass mode with `backdrop=stock`, against stock mode | identical, empty diff |
| real shade, the same comparison | max 3 levels on 8,471 px, all in the QS and notification UI (capture timing), none in the blur |
| `tone` at `backdropBlur 0.35` | the backdrop regenerated at radius 28 instead of 80; monotone, veiled |

`probePane=2` in the harness builds the alioth shape on the emulator: one
full-screen window with a background blur, holding two cards that are
`BackgroundBlurDrawable` blur regions at half the radius (needs
`settings put global hidden_api_policy 1`). Both cards log `over backdrop` and,
under a toned backdrop, come out monotone with it.

The copy-on-write claim above was tested directly. With the backdrop
regenerated nearly sharp (`backdropBlur 0.06`) and the cards set to show only
what is behind them (`blurMix 0`, `lensBlur 0`, `tintA 0`), the backdrop's
stripes run straight through both cards. Each card's own radius-40 blur was
generated **after** the backdrop's regenerated one, on the same
KawaseDualFilterV2, so the cards were reading an image the filter had since
drawn over, and it still held the backdrop's pixels.

The `clear` preset measured in "Presets, measured" above is `edge` from 0.4 on.

## First arm64 load: SELinux refused the hook, and gum crashed on it

alioth, crDroid 13.1 (A17), KernelSU 3.3.0 with meta-overlayfs. The engine
loaded for the first time on arm64, resolved 26,232 symbols, found the scaled
`drawBlurRegion` at `0x295240`, and SurfaceFlinger died before `main()`:

```
avc: denied { execmem } for comm="surfaceflinger" ... tclass=process permissive=0
F libc: Fatal signal 11 (SIGSEGV), code 1 (SEGV_MAPERR), fault addr 0x0 (surfaceflinger)
backtrace: #00..#08 libglassengine.so  #09 linker64 soinfo::call_constructors()
```

frida-gum's trampoline allocation was refused and gum dereferenced null inside
the engine's constructor. SurfaceFlinger restarted every ~5 s, the boot never
completed, and the next boot's guard disabled the module, as designed. Every AVD
used so far runs SELinux **permissive** (`getenforce` on glass17: `Permissive`),
which is why no emulator run ever showed it.

Two fixes, both proven on the phone **without restarting its compositor**, using
`engine/tools/hookprobe` labelled `surfaceflinger_exec` and run with
`runcon u:r:surfaceflinger:s0`. It runs the engine's new pre-hook check, then a
real `gum_interceptor_replace` on one of its own functions:

| policy | result |
|---|---|
| stock | `refused: execmem: anonymous RWX mmap denied`, exit 1, no crash |
| + `allow surfaceflinger surfaceflinger process execmem` (live, `ksud sepolicy patch`) | `refused: execmod: mprotect(RX) of a modified file page denied` |
| + `allow surfaceflinger surfaceflinger_exec file execmod` | `HOOK WORKS`: replaced, original through the trampoline, reverted, 0 denials |

1. `glass::canPatchCode()` (`engine/src/exec_probe.cpp`) asks both questions on
   throwaway mappings before gum is touched: an anonymous page, and a private
   mapping of the process's own executable, which carries the label the real
   patch is checked against. If either is refused, the engine logs which and
   does not hook. A device where the rules did not apply boots stock instead of
   crash-looping.
2. The module ships both rules in `sepolicy.rule`, which KernelSU applies at
   boot whether or not a metamodule is installed.

Also found on the device: `service.sh` started `glass-theme.sh` only if it was
executable, and `set_perm_recursive` leaves every file 0644, so theme following
had never run on a real install. It is started through `sh` now.

## First glass frame on arm64: a compiler-changed calling convention

**2026-11-17, alioth, crDroid 13.1 (A17), v0.4.0.** With the SELinux rules in
place the engine loaded and stock mode was correct, shade included. The moment
glass was switched on and the shade opened, SurfaceFlinger died; it then died
every ~5 s for as long as the saved mode said glass, the screen black. A manual
reboot hung on the boot logo (the first frames of the boot drew a blur region
within 2 s of SurfaceFlinger starting), and only the boot guard on the reboot
after that recovered the phone. 44 tombstones, all the same:

```
signal 11 (SIGSEGV), code 2 (SEGV_ACCERR), fault addr 0x...540 (write)   [RenderEngine thread]
#00 SkString::SkString(SkString&&)+4
#01 SkRuntimeEffect::MakeForShader(SkString)+36
#02..#04 libglassengine.so   (glass::skia::compile, from the first glass draw)
#05 <anonymous>               (the gum trampoline)
```

The fault address was the same page offset every time, inside the engine's
read-only SkSL text (`x8` held the bytes `assOpaci`, from `glassOpacity`). The
SkString move constructor was writing into the shader source.

**Cause.** `MakeForShader(SkString)` is the header-inline wrapper
`return MakeForShader(std::move(sksl), Options{})`. Its out-of-line copy in this
binary does not use the AArch64 convention for a by-value result:

```
6922f0  sub  sp, sp, #0x60 ...
692300  mov  x19, x0          ; x0 = RESULT address, not the SkString
692308  ldr  x8, [x20, #0x28] ; x8 overwritten (stack guard) — never read
692310  add  x0, sp, #0x8
692314  bl   SkString(SkString&&)   ; src = x1, the SkString
...
69232c  mov  x8, x19          ; forwards the result address in x8 to the real one
692330  bl   MakeForShader(SkString, const Options&)
```

Standard AArch64 passes the result address in x8 and the SkString in x0; this
copy takes the result in x0 and the string in x1. The compiler may do that to a
function it can see every caller of. The engine called it the standard way
(x0 = string, x8 = result), so it moved a "string" out of whatever x1 held — the
`const char*` the SkString had just been built from.

**Why no emulator showed it.** On x86-64 the result address goes in rdi, which
is also where a plain first argument goes. The changed and the standard
conventions are the same machine code there.

**Fix (v0.4.1).**

1. Compile through `MakeForShader(SkString, const Options&)`, the real
   definition, with `Options{}` built from the pinned header (40 bytes, all
   zero, as the wrapper itself builds it). It takes the result in x8
   (`mov x20, x8` at +28).
2. Before calling any function that returns by value, the engine reads its first
   instructions (`engine/src/abi_check.cpp`): it must read x8 by an exact,
   recognised encoding before anything could write it and before any branch.
   Otherwise the function is treated as absent; a core one makes the engine
   stand down. The installer's `glasscheck` asks the same question of the file.
   On alioth:

   | function | result register | evidence |
   |---|---|---|
   | `MakeForShader(SkString)` | NOT x8 | x8 overwritten at +24 |
   | `MakeForShader(SkString, const Options&)` | x8 | `mov x20, x8` at +28 |
   | `SkImage::makeShader` | x8 | `mov x19, x8` at +36 |
   | `SkData::MakeZeroInitialized` | x8 | at +16 |
   | `SkRuntimeEffect::makeShader` | x8 | at +28 |
   | `*BlurFilter*::generateTemporaryImage` (3) | x8 | at +36 / +48 / +64 |

   Every other bound function's argument registers were read off the
   disassembly too (SkString ctor/dtor, findUniform/findChild, uniformSize,
   SkMatrix::postConcat, SkPaint reset/dtor, SkCanvas::drawRect, both blob
   releases): all standard. `engine/test/abi_test.cpp` runs the file check on
   the host.
3. **Crash guard** (`engine/src/crash_guard.cpp`). While drawing glass the
   engine holds `sys.glass.active=1`. SurfaceFlinger does not restart in normal
   operation, so a new one that finds it set knows the last one died drawing
   glass: it saves `persist.sys.glass.mode=stock`, notes the time in
   `sys.glass.tripped`, and draws Android's blur. One crash, not a loop, and no
   hung boot. The surfaceflinger domain may set `system_prop` on stock policy
   (`engine/tools/propprobe` in that domain: `sys.glass.probe` ALLOWED,
   `debug.glass.probe` refused); `sepolicy.rule` grants it too.
4. `post-fs-data.sh` sets `sys.glass.module=1`, so the engine no longer looks
   for its marker under `/data/adb` (which enforcing SELinux hides from it,
   producing a string of denials at every boot), and `sys.glass.engine` reports
   `hooked` / `glass` / `no-glass` / `not-hooked` for `glassctl status`, since
   the phone's 256 KiB log buffer had already rotated past the engine's boot
   lines a minute after boot.

`engine/tools/out/arm64-v8a/glasscheck` on the phone now prints
`note SkRuntimeEffect::MakeForShader: result not taken in x8 (x8 overwritten at
+24 (f9401688) before any read); the engine compiles through the Options form`
and `OK`.

### v0.4.1 on alioth: glass draws

Installed over the stood-down v0.4.0 (`/data/adb/glass/disabled` removed), one
reboot. `sys.glass.engine=hooked`, `sys.glass.module=1`, mode stock. Switched to
glass with the shade open:

```
mode: glass, preset clear, backdrop tone, 3 properties overridden
SkSL compiled by SurfaceFlinger's Skia (22596 chars)              <- 16 ms
shader validated: 33 uniforms + 2 children resolved, blob 212 bytes, density 2.750 px/dp
draw #788 backdrop/tone r=13 (blurred at a lower radius) ... device=[0 0 1080 2400]
draw #792 pane r=56 ... device=[36 646 1044 765] ... radii=54.0 over backdrop
```

`sys.glass.engine=glass`, `sys.glass.active=1`, SurfaceFlinger's pid unchanged
through the whole session: shade, notification cards, the volume panel and the
keyboard, 316 engine log lines, no fatal signal, no tombstone.

- **Shade.** Toned backdrop at r=13 instead of 56; each card a pane over it.
  crDroid's SystemUI paints its own near-opaque card backgrounds on top of the
  blur region, so the cards look almost unchanged. Making them read as glass
  needs the card background alpha lowered app-side.
- **Volume panel.** Clearly glass: the wallpaper and the icons beneath refract
  through it under a bright rim. It is TWO blur regions in one layer, a rounded
  cap (radii 67.5) and a square body (radii 0) that overlap; stock blurs them
  identically, glass gives each its own edge, so a seam shows below the cap.
  Merging touching regions of one layer into one shape would fix it.
- **Keyboard** (patched Gboard, `dev.jason.com.google.android.inputmethod.latin`,
  frosted-glass patch). Its blur is `Window.setBackgroundBlurRadius` plus
  `ViewRootImpl.createBackgroundBlurDrawable`, i.e. SurfaceFlinger blur regions:
  `InputMethod` layer `blurRegions.size()=2`, key area [0 1447 1080 2206] and the
  strip below [0 2206 1080 2314], r=120. Both drawn as panes with no change to
  either side; content above bends into the keyboard's top edge.

Remaining noise: the engine's own boot-guard disarm thread still stats
`/data/adb` and unlinks under `/data/local/tmp` when the module owns the guard
(two denials per boot, harmless); it should not start when
`sys.glass.module=1`.

## v0.4.2 / v0.4.3 on alioth: colour, the volume panel seam, the update path

**Desaturation.** `clear` shipped `backdropSaturation 0`: the expanded shade
turned the wallpaper grey, and every card over it inherited that (panes over a
backdrop multiply saturations). Its pane tint, a 31% blend toward
(0.13, 0.10, 0.13), also takes 31% of the colour out of whatever the pane
transmits. Measured on the phone, mean chroma (max−min of RGB):

| where | stock | before | `backdropSaturation` 1.0 | 1.3 |
|---|---|---|---|---|
| expanded shade, below the tiles | 0.071 | 0.045 | 0.067 | 0.074 |

| where | stock | `saturation` 1.0 | 1.3 |
|---|---|---|---|
| whole volume panel | 0.119 | 0.100 | 0.123 |

`clear` now sets both to 1.3: the shade keeps its colour with a slight lift,
and panes get back what their dimming tint takes (0.69 × 1.3 ≈ 0.9).

**The seam.** The volume panel's two regions are merged into one outline
(`mergeNeighbours`): same x-extent within a pixel, overlapping in y, same blur
radius. The draw log marks both `merged`; the capture shows one continuous rim
from the rounded cap to the foot, where 0.4.1 had a fold below the bell. During
the slide-in the two regions move in layer-local coordinates as well, so the
merge leans on same-frame neighbours there.

**The engine's own boot guard** no longer starts its disarm thread when
`sys.glass.module=1`: `dmesg` after the 0.4.3 boot holds 83 avc lines and none
from surfaceflinger (each earlier boot had `getattr /data/adb`,
`search /data/local/tmp` and an `ioctl` on its maps).

**The update path (0.4.2 → fixed in 0.4.3).** 0.4.2 was installed over a running
0.4.1, so `customize.sh` stamped `/system/bin/surfaceflinger` as it saw it: our
own mounted overlay (9985265 bytes). At boot, before anything is mounted,
`post-fs-data.sh` sees the ROM's file (9805336) and stood the module down as if
after an OTA. The built overlay itself was right (`build-overlay.sh` noticed
the DT_NEEDED already present and copied the patched binary as-is). Now, when
the SurfaceFlinger the installer sees already carries our DT_NEEDED, it carries
over the installed copy's stamp, or leaves none for the next boot to record
from the real file. 0.4.3 was installed over the stood-down 0.4.2 (ROM's file
visible), so the "over a running copy" branch itself has not run on a device
yet; the next update will be the first.

0.4.3 booted with glass already on: `sys.glass.engine=glass` by the time adb
connected, no tombstone, no trip.

## v0.4.4: `softenDp`, a blur on the see-through view

`clear` showed what is behind its panes pin-sharp (blurMix 0.3: 70% sharp
child). Real clear glass, and the desktop reference (radius 2 at half
resolution), soften it slightly. New setting `softenDp`, an Android blur radius
in dp (σ = 0.57735 r + 0.5, RenderEffect's conversion), turned into a
`softenRadius` uniform: the plain see-through reads (`sIn`, `sBase`, the LOW
tier's lens read) go through `sampleSoft()`, a 3×3 Gaussian at one-sigma
spacing. The dispersion taps stay single reads, so the 9× cost lands only where
one read was taken before. 0 is exactly the old single read. `clear` uses 2.

On alioth, 0.4.4 installed over a running 0.4.3: the first real run of the
update-path fix (`- Updating over a running copy; kept the stamp of the ROM's
compositor`, stamp `9805336 1230768000` carried over), booted with glass on,
shader compiled and validated (`sys.glass.engine=glass`), no tombstone, no
surfaceflinger avc. Volume panel interior, mean absolute neighbour difference
(grey levels), softenDp 0 → 2: lower body 11.25 → 9.91, beside the slider
10.25 → 8.11. The refracted wave streaks inside the panel soften; the edge and
the icons drawn over it are untouched.

## The companion on the A17 AVD: tiles and folders as regions

glass17 (A17 AVD: SystemUIGoogle, the Pixel launcher), engine in glass mode with
`clear`, SELinux enforcing. The companion (ARCHITECTURE.md, "The companion") was
loaded by `companion/dev_attach.sh`, its JVMTI dev loader, because the AVD has
no LSPosed. Nothing was installed on alioth; the only contact was one read-only
`getprop`. Evidence is in `companion/verify/`.

Both hooks resolve and install, and the drawable is reachable from an app:

```
hooked com.android.systemui.qs.panels.ui.compose.infinitegrid.TileKt.TileExpandable (LLLLLLLLI)
Quick Settings tiles hooked: TileKt.TileExpandable (9 parameters)
BackgroundBlurDrawable available; framework default radius 263 px
hooked com.android.launcher3.folder.PreviewBackground.drawBackground (L)
```

**Regions on the shade layer** (`companion/tools/gate_matrix.sh`):

| state | `VRI-NotificationShade` blur regions |
|---|---|
| glass, Quick Settings fully open | **14**: 10 tiles on page 1, 4 on the off-screen page 2 (x 1080–2076) |
| glass, notification shade (QQS row) | 4 |
| `mode=stock`, `app.qs=0`, or engine reporting `no-glass` | **0**, the same as with no companion |
| engine reporting `hooked`, or empty (accepted on a debuggable build) | 14 |
| QS held open, then `mode=stock` by `setprop` alone | 14: nothing redraws, so nothing re-reads |
| then `service call activity 1599295570` | 0; after `mode=glass` and another call, 14 |

The shade keeps its own background blur (`backgroundBlurRadius=89`) and the
tiles add regions to the same layer, as notification cards do on alioth. The
engine draws each tile as a pane over the toned backdrop, at the tile's rect
(a large tile at device `[42 800 530 989]`), with the tile's own corners
(63 for an active tile, 94.5 for a pill-shaped one) and a `blurRect` of the
whole display.

**Measurement trap: count one layer.** The first counts read the first
`blurRegions.size()` within 16 lines after `VRI-NotificationShade`. With no
shade regions that line is absent, and the count came from the next layer (the
launcher's folder), a phantom 1 in every "off" state. `shade_count.sh` keys on
the layer name.

**The punch made tiles dark.** With the framework's transparent `SRC` paint
(`app.punch=1`) the tiles turned dark grey in the light theme. Overlaying their
rects on the home screen (`punch_scrim.png`) shows why: each tile showed the
raw wallpaper, because the paint erased the shade's light scrim under it. With
no punch (the default for tiles; `app.punch=0` forces it) the scrim stays, and
the tiles read as clear glass with legible labels (`qs_matrix.png`: stock,
default, `fill 0`, `punch=1`).

**Frame-synced.** At `animator_duration_scale 10`, the expansion shows no glass
outside a tile at any point (`qs_expansion.png`). The regions follow the tiles'
squish. Region radius 263, 89 or 40 looks the same.

**A switch reaches every tile.** Tiles read a Compose snapshot state
(`ParcelableSnapshotMutableIntState`, present in both builds) at draw time. The
companion writes it when the switches change, so Compose redraws every tile,
not only the ones SystemUI was redrawing anyway. With QS held open, a `setprop`
to stock left the 14 regions and the translucent fills, which stock
SurfaceFlinger then drew as plain blur. After the `SYSPROPS_TRANSACTION` call,
every tile was stock again: 0 regions, opaque fills (`qs_switch_push.png`).

**Folders.** One region on the launcher's layer (`blurRegions.size()=1`;
engine: `pane r=263 device=[78 1291 236 1449] radii=79.0`), none in stock mode
(`folder_stock_vs_glass.png`). Hidden-API reflection worked in the Pixel
launcher with `hidden_api_policy` untouched. With the home screen idle, a
`setprop` to stock left the region in place. Leaving and returning home, or the
`SYSPROPS_TRANSACTION` call, cleared it within 2 s, and the call brought it back
after `mode=glass`.

**Cost** (`companion/tools/cost.sh`, `verify/cost.txt`): TimeStats
`averageRenderEngineTiming` per client-composited frame, 6 QS open/close cycles,
two passes in opposite orders. SwiftShader, so compare the rows only.

| | pass 1 | pass 2 | frames, pass 1 / 2 |
|---|---|---|---|
| stock | 36.3 ms | 46.3 ms | 263 / 238 |
| glass, tiles off | 103.1 | 110.6 | 179 / 165 |
| glass, 14 tile regions at 263 px | 135.2 | 128.1 | 167 / 160 |
| the same at 89 px, the shade's radius | 126.6 | 127.0 | 165 / 161 |

**The engine's report.** On alioth (user build) `sys.glass.engine=glass` and
`sys.glass.module=1`. The AVD's `libglassengine.so` predates the report (no
`sys.glass.engine` string in it), so the property is empty there, which is why
the companion accepts empty on debuggable builds only.

**The dev loader on A17's ART**, for anyone redoing this:

- `cmd activity attach-agent` on a process that is not debuggable throws inside
  it and kills it ("Can't attach agent, process is not debuggable").
- `persist.debug.dalvik.vm.jdwp.enabled=1` makes every app debuggable on a
  userdebug build, but zygote reads it once, so it takes one
  `setprop ctl.restart zygote`.
- ART splits the agent argument at the first `=`. An agent in an app's own
  `lib/` dir (`/data/app/~~…==/…`) fails to load, so the agent goes in
  `/system/lib64` as `system_lib_file`, which app domains may map with SELinux
  enforcing.
- Capabilities: breakpoints, local variable access and frame pop are there;
  class redefinition and forced early return are not. So hooks are entry
  breakpoints that replace arguments; results cannot be replaced.
- `SetLocalObject` works on R8-processed dex with no local variable tables,
  because ART types each slot from the verifier.
- A hook sees only code that runs after the attach. SystemUI composes QQS at
  start, so the script makes it recompose with a night-mode round trip.

### v0.4.5: the rim softened too

`softenDp` in 0.4.4 left the dispersion taps as single sharp reads, and on a
narrow pane that band is most of the pane: the volume panel is ~49 dp wide and
`dispersionHeightDp` is 20 from each side. The user saw the rim stay crisp while
the middle softened. Each dispersion tap now also steps ACROSS the displacement,
cycling through (−1.2, −0.4, 0.4, 1.2) × 1.118σ (standard deviation σ); the taps
already walk ALONG it, so each channel's average is blurred in 2D at the same
σ as `sampleSoft`, still one read per tap. Zero offset when `softenRadius` is 0.

On alioth (installed over a running 0.4.4, stamp carried over again): booted
with glass on, shader compiled and validated (`sys.glass.engine=glass`), no
tombstone, no surfaceflinger avc. The rim itself is NOT yet measured: the
phone was in use, so no volume-panel capture was taken.

## The companion: themed app icons on the A17 AVD

glass17 as in "The companion on the A17 AVD" above: engine in glass mode, the
companion loaded by `companion/dev_attach.sh launcher`, nothing installed on
alioth. The AVD's Pixel launcher had themed icons off. They were switched on for
this with `content update --uri
content://com.google.android.apps.nexuslauncher.grid_control/icon_themed --bind
boolean_value:b:true` (`false` undoes it). crDroid's "Minimal" icon style was
read from its ThemePicker source (`ThemePickerIconStyle.MONOCHROME` sets the
same preference), not run. Evidence is in `companion/verify/`.

```
hooked com.android.launcher3.icons.FastBitmapDrawable.draw (L)
hooked com.android.launcher3.icons.mono.ThemedIconDelegate.drawContent (LLLLL)
launcher icons hooked: FastBitmapDrawable.draw, ThemedIconDelegate.drawContent (visible area 0.92)
```

The theme's icon shape measured as a 173 px path with corner radii of 86.2–86.5,
which is the circle it is. Each plate turns into translucent glass with the
engine's rim, and the glyph is unchanged (`icons_stock_vs_glass.png`).

**Regions on the launcher's layer** (`companion/tools/launcher_count.sh`,
`verify/icons_counts.txt`):

| state | regions |
|---|---|
| home: the folder, Photos, and the dock's Phone, Messages, Chrome | **5** |
| the dock's fourth slot, an unpinned prediction | none: it paints its own plate and ring |
| app drawer open | 0 |
| app drawer closed | 5 |
| a folder open | 4: the closed folder's icon is hidden |
| `mode=stock`, then glass again (each with `SYSPROPS_TRANSACTION`) | 0, then 5 |
| `app.icons=0` | 1: the folder |

**A change made while drawing waits for the next frame.** Turning icons back on
by push gave translucent plates and no regions. The window's aggregator copies
its regions in a pre-draw listener, before the draw that changed them, and an
idle home screen draws no next frame. A region that changes, or is hidden, now
asks for one.

**Ghost discs through the app drawer.** With the drawer open, every icon's glass
stayed under the drawer's translucent panel as a faint disc
(`drawer_ghosts_before.png`), and folders did the same. A once-a-second log of
each icon's ancestors (`app.debug=1`) showed every one VISIBLE at alpha 1 with
the drawer open: A17's launcher slides the drawer over the workspace without
fading it. Launcher regions now fade with the drawer's position
(`AllAppsTransitionController.mProgress`) as well as with their ancestors' alpha
and visibility. With the drawer open there are 0 regions and no discs
(`drawer_ghosts_after.png`).

**A launcher race showed through the plate.** After one re-attach, every dock
slot in glass mode showed a ring, a lighter plate and a second, fainter glyph,
for example a Gmail "M" behind the phone handset
(`dock_stacked_prediction.png`, top row). Stock was clean. `dumpsys activity
top` listed 7 hotseat children: 4 `PredictedAppIcon`s, 3 of them in the same
cells as the pinned Phone, Messages and Chrome. The Pixel launcher does this by
itself on some cold starts: three plain restarts with nothing attached gave 7, 4
and 4 children, and one of four attaches came up stacked. The pinned icon draws
later and its opaque plate covers the prediction exactly, so stock never shows
it. Launcher regions now keep the framework's punch, so the plate opens onto the
glass and not onto what the launcher drew first. QS tiles still do not punch,
because of the scrim (above). On a stacked dock after the change, Phone,
Messages and Chrome show clean glass, with 5 regions, then 0 in stock and 5 in
glass again (bottom row).

**The dev loader only sees icons that redraw.** A view keeps the display list
it recorded before the attach, so straight after one, only the icons that had
redrawn had glass (1 region of 5). `dev_attach.sh launcher` now turns "show
layout bounds" on and off (`debug.layout` plus `SYSPROPS_TRANSACTION`), which
makes every window invalidate every view, and the attach then shows all 5. Under
LSPosed the hooks are in place before the first draw.

**QS after these changes.** SystemUI was re-attached with the same build, and
`gate_matrix.sh` gave the same 12 rows as before (14, 4, 0, …). The alpha
tracking and the per-surface punch left the tiles as they were.

Not checked: app-launch and overview transitions with icon glass, and what the
icon regions cost.

## The companion: the notification shade, AVD and alioth (read-only)

**What alioth already has** (crDroid 13.1, StockGlass v0.4.5, glass on; read
over adb, nothing changed): `Settings.Secure notification_row_transparency=1`
and `notification_row_transparency_lockscreen=1`, so every shade card is its own
blur region and the engine draws it as a pane (the parent session logged one at
`[36 415 1044 577]`, radius 63). Over each pane lie the card's fill,
`system_surface_effect_1_dark` = `#8a121317` (`cmd overlay lookup`), and the
shade's scrims: `ScrimController` `mDefaultScrimAlpha=0.7254902` (the shade
panel colour's alpha) and, from crDroid's source, a notifications scrim of
alpha 127/255. crDroid's `shade_color_style` (Settings → UI → Shade style) is
unset, which is AOSP. A pinned heads-up has no region: in crDroid's SystemUI,
`ExpandableNotificationRow.usesTransparentBackground()` is false whenever
`mustStayOnScreen()`. LSPosed is not installed on alioth (Zygisk Next 1.5.3 is),
so the card and heads-up hooks have not run there.

**The scrim cut on the A17 AVD** (glass17; `dev_attach.sh systemui`). The
hooks resolve on Google's build too:

```
hooked com.android.systemui.statusbar.phone.ScrimState$9.setShadePanelColor (I)
hooked com.android.systemui.statusbar.phone.ScrimState$9.setNotificationScrimColor (I)
hooked com.android.systemui.statusbar.phone.ScrimState$5.setShadePanelColor (I)
hooked com.android.systemui.statusbar.phone.ScrimState$5.setNotificationScrimColor (I)
hooked com.android.systemui.statusbar.phone.ScrimController.updateThemeColors ()
no per-card blur in this SystemUI (NoSuchMethodException); notification cards and heads-ups left alone
```

With the notification shade held open (`ScrimController` dump):

| | behind scrim | notifications scrim |
|---|---|---|
| glass (`app.scrim` 0.5) | 0.365, tint `0x5dbbbecc` | 0.251, tint `0x40dae2ff` |
| `mode=stock` + `SYSPROPS_TRANSACTION` | 0.725, tint `0xb9bbbecc` | 0.498, tint `0x7fdae2ff` |
| `mode=glass` + `SYSPROPS_TRANSACTION` | 0.365 | 0.251 |

The stock values are the same as alioth's. A switch changes an open shade at
once (`updateThemeColors` then `applyAndDispatchState`), and the blurred home
screen reads through the lighter scrim (`shade_scrim_avd.png`). The AVD's shade
has no per-card regions, so the card fill and heads-up parts wait for LSPosed
on alioth.

## v0.4.6: Basic mode, regular glass, Liquid Material, tints from the region

**What changed in the shader.** Two blends from the reference
(projects/uikit-even-how-datamine, FINDINGS.md 4 and 5.1), both the identity at
their defaults, so every existing preset draws as before:

- the blur fill: `face = L * max(see, blur) + D * min(see, blur) + (1-L-D) * see`
  before the existing `mix(face, blur, blurMix)`;
- the tone after saturation: an optional luma limiter
  (`s = 1 - k * luma`, `mix(s*luma, s*c, 1 + 0.3*(1-s))`), then
  `c * toneScale + toneBias`, then a multiply by `filterColor`.

The reference's colour matrix decomposes exactly as `scale * saturation(c) +
bias` with Rec. 709 luma weights: light regular's off-diagonals
(-0.0970, -0.3262, -0.0329) are `0.504 * (1 - 1.905) * (0.2126, 0.7152, 0.0722)`
to four places.

**glass16 (A16 QPR0, x86_64, SwiftShader), harness probe pane r=80.** The shader
compiled and validated through SurfaceFlinger's Skia ("40 uniforms + 2 children
resolved, blob 256 bytes"). Interior crop [300 1000 780 1400], dark theme
(default frost colour), mean over the crop:

| | luma | chroma | rgb |
|---|---|---|---|
| Android blur | 77.9 | 101.9 | (156, 55, 79) |
| basic 0 (= `clear`) | 59.3 | 92.1 | (130, 38, 62) |
| basic 0.5, Regular | 98.4 | 120.9 | (191, 71, 100) |
| basic 1, Frosted | 91.7 | 102.8 | (170, 68, 93) |
| Regular + Material 100% (accent #69769b) | 73.0 | 73.8 | (125, 54, 111) |
| Clear + Material 100% | 44.5 | 53.4 | (84, 31, 64) |

Dark Regular comes out lighter than the blur, as the reference's +0.125 lift
over a dark backdrop makes it. In the light theme (frost colour 0.95) Regular
is a milky white pane and Frosted whiter still. Material at full strength takes
about a quarter of the light out in the accent's hue; the slider is there to
use less.

**Not run anywhere yet:** Blur intensity regenerates a pane's blur through the
generateTemporaryImage hook, which only exists on Android 17 builds. glass16
has none (`sys.glass.regen=0`, the WebUI says so). The call is the one the
toned backdrop has made on alioth since 0.4.0, now also for panes and cached
per layer. Also not seen: a region tint from a real app (`tint_test` covers
the encoding: every byte round-trips at radii up to 8191 px, and 3900 ordinary
radii, whole dp at twelve densities plus quarter and eighth pixels, never
decode as one).

## The companion: home screen widgets on the A17 AVD

glass17 as above (`companion/dev_attach.sh launcher`, engine in glass mode,
nothing installed on alioth). The test widgets were the Pixel image's Gemini
widget on page 1 and Google Calendar's "Calendar month view" (4×4,
`com.google.android.calendar/.widgetmonth.MonthViewWidgetProvider`) added on
page 2. That is the widget the user has on alioth. Evidence and the full
decision log are in `companion/verify/` (`widget_*.png`, `widget_counts.txt`).

```
launcher widgets hooked: CellLayout.dispatchDraw
wallpaper dark by Android's colour hints
```

**Regions on the launcher's layer:**

| state | page 1 (Gemini, folder, icons, dock) | page 2 (Calendar, dock) |
|---|---|---|
| glass | 6 | 4 |
| `mode=stock`, then glass again (each with `SYSPROPS_TRANSACTION`) | 0, then 6 | 0, then 4 |
| `app.widgets=0` | 5 | |
| app drawer open, then closed | 0, then 6 | |

Each widget's plate turns into translucent glass with the engine's rim at the
widget's own corners, and its text and buttons are unchanged. The overlay
draws after the widget, so the region does not punch.

**Light theme lost the Calendar's text.** At `app.fill` 0.3, the Calendar's
dark text sat on a dark grey pane: its light plate kept 30%, and the rest was
the AVD's dark default wallpaper (`widget_calendar_light_theme.png`, middle).
In the dark theme the same widget has light text on a dark plate and read well
(`widget_calendar_dark.png`). So a widget whose text would be lost keeps
`app.contrast` of its plate (ARCHITECTURE.md). With `app.debug=1` the
launcher logs each decision:

| theme | wallpaper (Android's hint) | Calendar plate | kept |
|---|---|---|---|
| light | default, dark | light (`GradientDrawable`) | 0.65 |
| dark | default, dark | dark (`GradientDrawable`) | 0.3 |
| dark | a pale blue test image, light | dark | 0.65 |
| dark | cleared back to the default, dark | dark | 0.3 |

The Gemini widget's `ColorDrawable` plate followed the same rule. The
wallpaper was set through the Pixel wallpaper app's preview: its
`READ_MEDIA_IMAGES` was granted for this and revoked after, and the image was
set on the home screen only. The listener logged `wallpaper now light` as it
was applied, and after
`service call wallpaper 17` (`clearWallpaper`, run as the shell user)
`wallpaper now dark`. Both decisions changed without a launcher restart.
`app.contrast=0` then unset, each with `SYSPROPS_TRANSACTION`, switched an idle
home screen to 0.3 and back to 0.65. In both light-text and dark-text cases
0.65 reads clearly and still shows glass (`widget_calendar_light_theme.png`,
`widget_calendar_light_wallpaper.png`).

**A widget re-renders through its loading layout.** On a theme flip or a new
wallpaper, the host shows the widget's loading layout for about a second.
That layout's plate is a `StateListDrawable`. A copy of a selector has no state
and drew nothing, so the tone was unread. The read now takes the selector's
current child. That child is null in the loading layout, so the system theme
decides until the real plate arrives; both times, the decision was the one the
plate then gave.

Not checked: crDroid's Launcher3 at run time (AOSP's has
`RoundedCornerEnforcement.findBackground`; the module carries a copy for a
launcher without it), widgets drawn as one bitmap (an `ImageView` plate: handled
in code, none on the AVD), what widget regions cost, and alioth.

## The companion: tinted glass for Quick Settings tiles that are on (A17 AVD, engine 0.4.6)

glass17 with the v0.4.6 engine dev-installed (`engine/install_dev.sh`) and the
framework restarted. Under SELinux enforcing, 0.4.6 refuses to hook ("execmod …
denied … Not hooking", `sys.glass.engine=not-hooked`), because a dev install has
no `sepolicy.rule`. The companion then drew nothing, as its opt-in requires. The
user set glass17 permissive for this run, so nothing here says anything about
SELinux. Then `sys.glass.engine=glass`, `sys.glass.regen=1`, and the companion
was attached with `dev_attach.sh systemui`. Log lines and the sweep are in
`companion/verify/` (`qs_tint_log.txt`, `qs_tint_sweep.png`).

**The tint arrives intact.** The engine decoded the tile's corner radii:

```
draw #195 pane r=263 tint=#4e5e8b@0.50 a=1.00 … device=[297 1221 530 1410]
```

That is Auto-rotate, the one small tile that was on, in its own fill colour
(light theme). The dark theme gave `tint=#bac5ee@0.50`. Every `app.tint` value
gave 14 regions with QS open, the same as untinted. Off tiles log no tint. So do
the dual-target large tiles (Wi-Fi, Bluetooth, Mobile data): on Google's build
their body stays neutral and "on" shows only in the icon square, which is not a
region.

**Strength.** With `app.tint` > 0 the tile's own fill keeps `app.fill` (0.3),
and the glass carries the colour (`qs_tint_sweep.png`, both themes, 0 / 0.5 /
0.65 / 0.8). In the light theme the on-tile is dark with a white icon, which
reads at every strength. In the dark theme the on-tile is light with a dark
icon over a dark shade. At 0.5, the parent session's suggested middle, the
glass went mid-grey and the icon nearly vanished; 0.65 was marginal, and 0.8
reads like the old look with the glass's rim and filtered light. The default is
0.8. `app.tint=0` gives the old look back (the fill keeps `app.accent` 0.75, no
tint). An engine without tints ignores the request, and the on-tile then keeps
only 30% of its colour, so `app.tint=0` belongs with an older engine.

**Blur intensity's pane re-blur on A17** (the parent session's check, its first
run on Android 17). With QS open, `setprop persist.sys.glass.fillBlurScale 1.5`
gave 62 draw lines with ` (blurred at N)`. 58 of them were tile panes,
`pane r=263 (blurred at 300)`: 263 × 1.5 is 394, which `kMaxRegenRadius` caps at
300. There was no `no blur at` warning. `sys.glass.tripped` stayed empty,
SurfaceFlinger kept its pid (25139), and the engine still reported `glass`. One
line carries both changes, `pane r=263 (blurred at 300) tint=#4e5e8b@0.80`.
`fillBlurScale` was unset afterwards.

Not checked: crDroid's tiles (whether their body turns to the accent colour),
the icon squares of dual-target tiles, and alioth.

**On alioth (crDroid 13.1, A17), installed 2026-11-17 17:50.** The installer
updated over the running copy and kept the ROM compositor's stamp; after the
reboot `sys.glass.engine=hooked` and `sys.glass.regen=1`, so the
generateTemporaryImage hooks that Blur intensity needs are in place on this
build. glass-theme.sh read the palette at boot (`accent -> #5c7a96`, from
`cmd overlay lookup`). Glass itself has not drawn a 0.4.6 frame yet: the saved
mode was `stock` before the install.

**The companion under Vector 2.2 (JingMatrix, API 102) on alioth.** Enabled in
Vector's manager with System UI and `com.android.launcher3` in scope
(`/data/adb/lspd/cli modules ls`, `cli scope ls dev.stockglass.companion`).
After System UI and the launcher restarted, every hook resolved on crDroid:

```
launcher folders hooked: PreviewBackground.drawBackground
launcher icons hooked: FastBitmapDrawable.draw, ThemedIconDelegate.drawContent (visible area 0.92)
Quick Settings tiles hooked: TileKt.TileExpandable (10 parameters)
notification cards hooked: BackgroundBlurDrawable.setColor
launcher widgets hooked: CellLayout.dispatchDraw
shade scrim hooked: 4 ScrimState setters, ScrimController.updateThemeColors
heads-ups hooked: ExpandableNotificationRow.usesTransparentBackground (applies under LSPosed; ...)
engine reports "hooked"
```

Nothing it draws has been seen yet, for the same reason: mode `stock`.

## v0.4.7: the trailing smear on moving panes

The user reported "Trailing blur sometimes" on alioth (0.4.6). The cause is
`mergeNeighbours`, the code that outlines the volume panel's cap and body as
one pane (v0.4.2 above).

**Old rule.** It remembered every pane drawn in the last 120 ms and merged any
of them that shared this pane's cross extent within a pixel and overlapped it
along the other axis. A pane moving straight along one axis meets exactly that
in its own earlier positions. This covers cards scrolling, a heads-up sliding
in, icons on a page swipe and tiles while Quick Settings expands. Its outline
became the union of where it had been. Each pane still drew only its own
rect, so the end it moved away from lost its rounded corners and rim, and the
shader's edge refraction there: a squared-off smear trailing the motion.

**New rule** (`engine/src/region_merge.h`). Each pane gets a slot: its place
among the panes drawn from the same snapshot (this layer, this frame). That
place stays the same from frame to frame, so it identifies the pane across
frames. A pane borrows a remembered region from an earlier frame only when
both of these hold:
- the region comes later in the draw order, so it has not been drawn yet this
  frame;
- the pane itself stands where it stood in an earlier frame (same slot, same
  rect within a pixel).

Regions drawn earlier in the same frame merge as before. The memory holds 64
regions instead of 32, so the cap still finds last frame's body behind a home
screen's worth of panes.

**`engine/test/merge_test.cpp`** runs the merge code itself on the host:

```
g++ -std=c++17 -O2 -Wall -Wextra -I engine/src engine/test/merge_test.cpp -o /tmp/merge_test && /tmp/merge_test
PASS: merge rules
```

The same test run against the header with the new rule's one line removed
(the old behaviour) fails where the smear was:

| scenario | frames failing, old rule |
|---|---|
| a pill moving down 30 px a frame | 11 of 11 |
| two cards scrolling 50 px a frame, 8 px apart | 11 of 11 |
| five icons on a page swipe, 40 px a frame | 55 of 55 |
| volume panel sliding in (cap and body move together) | 5 + 4 |

The volume panel standing still still merges into one outline from its second
frame on; its growing body still merges; the body still draws only what the
cap did not.

**On glass17** (A17 AVD, made permissive by the user; companion attached, Pixel
launcher with icons and widgets on two pages). The same swipes were run under
both engines. The table counts the draw lines that say `merged`:

| scenario | 0.4.6 | 0.4.8 |
|---|---|---|
| fast page swipes, page 1 ↔ 2, 3× | 26 of 79 | 0 of 87 |
| slow page swipes | 67 of 131 | 0 of 144, then 0 of 203 |
| QS expand and collapse, 2× | 0 of 96 | 0 of 48 |

On 0.4.6 the moving folder, Photos and both widgets merged. In mid-swipe
screenshots Photos' disc was a capsule stretched back along its path and the
widgets' glass ran on behind them. On 0.4.8 the discs stay round and the
corners rounded at both ends. SurfaceFlinger's pid never changed and nothing
tripped. Not covered there: heads-ups and the drawer's icons have no regions
on Google's builds, and the AVD's volume panel has none, so the
same-frame merge still rests on the host test.

**On alioth** (0.4.8, 2026-11-18): a full-width pane sliding in from the top
edge and back out (a heads-up, by its size and path) drew 132 lines, none of
them `merged`.

Still open: in 1 of 5 mid-swipe screenshots on 0.4.8, every moving control's
glass sat about 190 px (about one frame of that swipe) behind its control, in
its right shape. Holding the page still showed it aligned. Blur regions reach
SurfaceFlinger in a transaction merged into a given frame's
(`mergeWithNextTransaction`), so a frame the window misses would show them a
frame late. Whether that happens on alioth, or only on the AVD's slow
composition, is not known.

## The companion: fades, sliders, the text toolbar, screenshots, search boxes and the drawer (A17 AVD, engine 0.4.6)

The user's report from alioth, item by item. Everything below ran on the A17
AVD (glass17) under the dev loader, with the Pixel launcher; nothing on crDroid
or under LSPosed yet.

**1. Glass under tiles that Quick Settings has faded out.** Reproduced with the
previous build: with the shade held at 700 px (`tools/qs_hold.sh`), rows 3 and 4,
which only full QS shows, had glass and no tile, one of them tinted. QS fades
them with Compose layer alpha (`QuickSettingsContent` from 0.43 of the
expansion), a RenderNode property that never reaches a region. The tiles now
multiply in every layer's alpha from their draw node out (ARCHITECTURE.md).
After: no glass at 700 px; it fades in with the tiles at 1350 and 1550 px and is
whole at 1800 (`verify/qs_fade_with_tiles.png`); full QS still has 14 regions.

**3. Trailing blur, companion side.** The window's blur aggregator copied the
regions before the companion's per-frame sync, so an alpha change went to
SurfaceFlinger a frame late. Its listener is now moved behind the sync; logcat
shows `blur aggregator of <window>'s window moved behind the companion's
per-frame sync` for the shade, the launcher, the toolbar's and the screenshot
UI's windows. Frame timing cannot be measured on SwiftShader. The engine part
is v0.4.7 above.

**4. Glass out of step on the launcher.** Not reproduced. `tools/region_dump.sh`
(`app.debug=1`, then a property broadcast) logs every region with the rect hwui
last reported and where its control is; a checker flags regions with glass whose
rect is not on their control. Before this round's changes: launch from page 2
and HOME twice, overview then gesture home, gesture home, the long-press menu,
folder open and close, and the drawer showed 0 such regions, and off-screen
pages held empty rects. The launcher research found that in overview and quick
switch the workspace stays VISIBLE at about 0.9 scale with its pages at alpha
0; the sync's ancestor alpha takes those regions out (0 regions with glass in
overview). With this round's build: home, app launch and HOME, overview and
HOME, drawer and HOME, gesture home: 7 regions with glass each time on the home
screen, 0 off their control. To catch it on the phone:
`setprop persist.sys.glass.app.debug 1`, then
`service call activity 1599295570` while the stale glass is showing; the
launcher (`com.android.launcher3`) logs one `dump` line per region.

**5. Text selection toolbar.** SystemUI's `RemoteSelectionToolbar` renders it
through a SurfaceControlViewHost in a transparent popup of the app (Messages
here), so no app needs to be in scope. The SCVH layer has
`blurRegions.size()=1`, and the engine logs a pane at the toolbar's rect. The
toolbar's elevation shadow showed through the translucent fill and made it darker;
`ViewGlass.shadowless()` turns it off while glass is on
(`verify/text_toolbar.png`, stock above, glass below).

Over a white page (249,249,255) the glass reads (193,192,202) at `app.fill`
0.3; the stock toolbar is (225,226,237). With `app.fill=0` it is
(180,178,186), and the companion draws nothing there. It stays (180,178,186) with
`blurMix` 1 instead of the clear preset's 0.3, with `frostR/G/B` 0.98/0.98/1.0,
and at region radius 40, 120 and 263 px. The app's popup is a transparent
window without a shadow. The screenshot bar over the dock's light search pill
dims about as much (below). So a pane over light content comes out at about
0.72 of it. That is the preset, not a fault: glass17 runs `preset=clear`, and
clear (like Basic at its Clear end) lays a 31% blend toward (0.13, 0.10, 0.13)
over every pane (v0.4.2 above), so 0.69 × 249 + 0.31 × 30 ≈ 181, whatever
`blurMix`, frost colour or radius. At Regular a pane over white stays at about
0.98 in a light theme; in a dark theme the luma limiter brings it to about 0.72
on purpose, as the reference's dark Regular does.

**6. Sliders, screenshot bar, search boxes.**

- Brightness slider: full QS has 15 regions (14 tiles and the track; 14 with
  `app.sliders=0`). The inactive track shows the glass and its rim, and the
  active track keeps 75% (`verify/qs_brightness_slider.png`, stock above,
  glass below).
- Screenshot action bar: the pill turns to glass with the rim, and the dock's
  search pill refracts through it (`verify/screenshot_actions.png`). The
  `ScreenshotUI` layer has one blur region while shown, and no layer is left
  after dismissal.
- Dock search bar (Pixel: Google's widget in the Hotseat): its pill is two
  full-width `GradientDrawable` images, not a background. Light plate over a
  dark wallpaper keeps `app.contrast` 0.65 (`verify/dock_search_bar.png`).
- Drawer search pill: (220,221,231) stock, (184,188,202) glass
  (`verify/drawer_search_box.png`). While search is focused the Pixel launcher
  hides the pill (alpha 0), and the region is hidden with it; scrolling the list
  brings the idle pill back with its glass.
- Drawer icons: the Pixel launcher does not theme them, so the path was run with
  a temporary build that treated the open folder's (themed) icons as drawer
  icons: their regions came up at alpha 255 on the plates. The fade for icons
  partly scrolled under the search box was not exercised.

Not run: crDroid's `QsbLayout`, `AppsSearchContainerLayout`, volume slider and
themed drawer icons; the `:screenshot` process under LSPosed; whether crDroid's
build has `system_selection_toolbar_enabled` on (its SystemUI has the same
toolbar classes).

## The companion: the lockscreen clock (A17 AVD, engine 0.4.8)

glass17 with engine v0.4.8 (`preset=clear`), the companion loaded into SystemUI
by `dev_attach.sh`, and the AVD's default wallpaper (SystemUI's
`ImageWallpaper`: dark, with a light and blue shape across the top). The AVD
had no lockscreen; it was set to swipe for the test
(`locksettings set-disabled false`) and back afterwards.

- **Hooks.** `lockscreen clock hooked: 2`: `DigitalClockTextView.onLayout` and
  `FlexClockViewGroup.onLayout`. crDroid's `ClockStyle` is not in this build.
- **Capture.** `clock backdrop 270x600 for 1080x2400`, 25–80 ms on the
  background thread: once the lockscreen had settled, and again after each
  wake.
- **Large clock** (`FlexClockViewGroup` 526×795, digits 395 px: refraction
  height 178 px, band 36 px). Over the dark part of the wallpaper the digits are
  grey glass with a rim along their upper left edges
  (`verify/clock_large_stock_glass.png`, stock left).
- **Small clock** (`FlexClockTextView` 463×299, digits 163 px), shown here when
  the system's silent developer notification is allowed on the lockscreen. It
  sits over the wallpaper's light area, and the blue edge bends inside the 2
  and the 4; the colon reads as two drops (`verify/clock_small_stock_glass.png`,
  stock above; `verify/clock_small_glass_zoom.png`).
- **Switches.** `app.clock=0`, and `persist.sys.glass.mode stock` with
  `app.clock` unset, gave the stock clock at once. `app.clock=plain` gave
  translucent white with the bevel; unset again, the glass came back with its
  fade-in (`verify/clock_modes.png`: stock, glass, plain, glass).
- **Moving.** The switch from the small clock to the large one, at animator
  scale 10, kept glass in every frame, with no flash or stale frame
  (`verify/clock_size_transition.png`). The clock never crossed the blue edge
  while it moved, so alignment in motion was not seen over detail; at rest the
  edge inside the small clock's glyphs continues the edge outside them.
- **Doze.** The AVD has no AOD: the screen is black while asleep. Right after
  waking the clock is the stock doze text on black, and the glass is back, on a
  fresh capture, within the 3 s screenshot. The crossfade through a real doze
  transition was not seen.
- **Preview.** Wallpaper & style's lockscreen preview, which SystemUI draws,
  shows the stock clock.
- **Cost.** SystemUI's `dumpsys gfxinfo` over five charger plug-ins (each plays
  the clock's charge animation, which redraws the glyphs every frame): stock
  709 frames, 50th/90th percentile 65/150 ms; glass 722 frames, 65/150 ms. The
  emulator's GL is the bottleneck either way, so this shows only that the glass
  adds nothing measurable here.
- **Health.** SurfaceFlinger's pid stayed 15325 throughout, `sys.glass.tripped`
  stayed empty, and SystemUI did not crash.

Not run: crDroid's `ClockStyle` styles and the flex clock on crDroid (the
classes, methods and fields used match the decompiled crDroid 13.1 SystemUI);
the capture on crDroid's framework (the same A17 `ScreenCaptureInternal` and
`IWindowManager.captureDisplay` are expected; if either is missing, the clock
gets plain glass and one log line); a live wallpaper; the doze crossfade; cost
on alioth's GPU.

## The companion: clock faces from plugins (A17 AVD, engine 0.4.8)

The user's custom face on alioth (Inflate) stayed stock: it comes from a clock
plugin, which the first clock build did not reach. glass17 as above, with
crDroid 13.1's six clock plugins (`SystemUIClocks-*.apk`, taken from its
`system_ext.img`) installed as ordinary apps. SystemUI loads a clock plugin
only if it holds `com.android.systemui.permission.PLUGIN`, a signature
permission that crDroid-signed APKs never get on the AVD. So the test used
`companion/out/devtest/stockglass-companion-devtest.apk`
(`tools/devtest_plugins.sh`): the module's own `classes.dex`, byte for byte,
plus a test-only hook that has that one check ask about SystemUI instead, for
`com.android.systemui.clocks.*` only. The plugins were reinstalled after the
attach, so SystemUI loaded them, and each face was picked with
`settings put secure lock_screen_custom_clock_face '{"clockId":"…"}'`. The
plugins drew on the AVD as on the phone: Inflate's bubbly overlapping digits,
each in its own colour with a thick outline.

- **Hooks.** `lockscreen clock hooked: 3`, the third being
  `ClockEventController.setClock`.
- **Large clocks** (`companion/verify/clock_faces_stock_glass.png`, stock
  above, final build):
  - Inflate: `InflateClockViewGroup` 632×835, digits 387 px.
  - NumOverlap: `NumberOverlapClockViewGroup` 784×999, 395 px.
  - Growth: `CustomClockViewGroup` 588×1010, 493 px.
  - Calligraphy: `CustomClockViewGroup` 561×1010, 483 px.
  - Metro: `MetroClockViewGroup` 616×1048, 367 px, from its Lottie digit views.
  - BigNum, the analog face, stays stock, as intended.

  Each digital face is glass that keeps its digits' colours and outlines:
  NumOverlap keeps its cut gaps, Metro its two-tone stripes. The rim is lit
  along the upper left edges.
- **Small clocks** (`companion/verify/clock_faces_small.png`, final build):
  Inflate 424×299 (157 px), NumOverlap 527×299 (168 px), Growth 368×299
  (175 px), Calligraphy 344×299 (168 px), Metro 399×299 (158 px). They sit over
  the wallpaper's light area, and the blue edge bends inside Inflate's digits.
- **Fixed while testing:**
  - Growth's and Calligraphy's small clocks, each a single text view, were
    missed by the search for digit views.
  - Metro's small clock stayed stock while its first hour digit was empty
    (2:30): its digit height came from that view, now from the tallest one.
  - Inflate's small clock measured 0 px wide until the next minute's
    re-measure, so it stayed stock for up to a minute, with its date drawn
    under it. With one `requestLayout` it was 424×299 and glass within a
    second, its date beside it.
- **Tint** (Inflate, `companion/verify/clock_inflate_tint.png`):
  - Clear glass (tint 0) made the four digits one grey shape.
  - Multiplying by the face's colours gave dark, muddy glass.
  - Tinting after the light dimmed the rim.
  - Tinting before it, as shipped, kept the rim as crisp as on clear glass
    (`companion/verify/clock_inflate_rim_zoom.png`: stock, before, after).
- **Switches** (`companion/verify/clock_inflate_switches.png`): every change
  took effect at once, without a restart.
  - Default (tint 0.5): glass.
  - `app.clock=0`, and engine mode stock: stock.
  - `app.clock=plain`: translucent glass in the face's colours.
  - `app.clocktint=0`: clear grey glass.
  - `app.clocktint=1`: the face's own colours with the glass rim.
  - Unset again: glass.
- **Moving.** Inflate's switch from the large clock to the small one, at
  animator scale 10: glass in every frame captured
  (`companion/verify/clock_inflate_transition.png`).
- **Wake.** After power off and on, the glass was back on a fresh capture
  within 3 s.
- **Back to the flex clock.** Picking the default clock again gave its grey
  glass, as before.
- **Preview.** Wallpaper & style's lockscreen preview showed Inflate stock.
- **Cost.** SystemUI's `dumpsys gfxinfo` over five charger plug-ins, on Inflate:
  - Stock 354 frames, 50th/90th percentile 81/150 ms; glass 371 frames,
    93/150 ms.
  - In the other order: glass 400 frames, 85/150 ms; stock 356 frames,
    97/150 ms.

  No difference beyond run-to-run noise on the emulator.
- **Health.** SurfaceFlinger's pid stayed 15325 throughout,
  `sys.glass.tripped` stayed empty, SystemUI did not crash, and the companion
  logged no warnings.

**On alioth** (crDroid 13.1, module 0.4.11, companion c07f7f1f, 2026-11-18):
Vector loaded the companion into SystemUI and the launcher, and it logged no
warnings. The user saw Inflate drawn as glass on the lockscreen at the default
tint (0.5). Their verdict was that the puffy, overlapping digits make even glass
feel unglassy, and they switched to Calligraphy.

Not run:
- On alioth, anything beyond that look: the other faces, the cost, and the
  hooks' log lines (the phone's log had rotated before they were read).
- The doze crossfade (the AVD has no AOD).
- A live wallpaper.
- Cost on alioth's GPU. Each redraw of a face runs two shader passes and four
  blends over one blur; the flex clock runs one pass.

## Updating over a running copy (alioth, 0.4.9 over 0.4.8; fixed in 0.4.10)

Installing 0.4.9 over a running 0.4.8 on alioth (KernelSU 3.3.0,
meta-overlayfs 1.3.1), with glass on, killed SurfaceFlinger while KernelSU was
installing the module (2026-11-18 01:05). The crash guard did its job: the next
SurfaceFlinger found `sys.glass.active=1` and saved
`persist.sys.glass.mode=stock`, so the screen came back with Android's blur.
Glass stayed off after the user's reboot until `glassctl glass`.

**Tombstone** (`tombstone_17`):
- Where: the `RenderEngine` thread, SIGSEGV, SEGV_MAPERR. The pc equals the
  fault address, 0x28f6b0, and frame #01 is libglassengine.so at 0x1fb528.
- 0x28f6b0 is the link-time address of the library's `.plt` header (section
  [16]).
- The file stores that same value in its `.got.plt` slots (file offset 0x29e198
  on). At load, the linker overwrites those slots with resolved addresses.
- So a call landing on 0x28f6b0 means that page had been read back from the
  file.

**Cause.** meta-overlayfs's `metainstall.sh` copies the module's `system/` with
`cp -af` into `/data/adb/metamodule/mnt/<id>/`. That is the live lower directory
of the `/system` overlay
(`lowerdir=/data/adb/modules/meta-overlayfs/mnt/stockglass/system:/system`).
- cp opens an existing file with `O_TRUNC` and rewrites it in place.
- Truncating a file unmaps its pages from every process. That includes the
  private copy-on-write pages the dynamic linker wrote relocations into: the
  kernel's `truncate_pagecache` unmaps with `even_cows`.
- SurfaceFlinger's next fault on the engine's `.got.plt` page read the file's
  original contents, so its next call through the PLT jumped to 0x28f6b0.
- Identical bytes would not have saved it: the relocated copies are gone either
  way.
- The patched `surfaceflinger` binary escapes this only because it is running.
  The kernel refuses to open a running executable for writing, so `cp -f`
  unlinks it and creates a new file instead.

**Reproduced.** A small test library holds a relocated function pointer and
calls `getpid` through its PLT. A program called it about 10,000 times a second
while the copy ran:

| | host (x86_64, Linux 7.0) | alioth (kernel 4.19, in KernelSU's busybox shell) |
|---|---|---|
| `cp -af` over the mapped file | SIGSEGV, SIGSEGV, SIGBUS (3 of 3) | SIGSEGV |
| rename a new copy over it first, then the same `cp -af` | survived (3 of 3) | survived (10,636 calls) |

**Fix** (`module/customize.sh`, 0.4.10). Before the metamodule's copy, each file
of the new `system/` that already exists in the live tree is replaced in three
steps:
1. It is copied next to the live file.
2. It gets its SELinux label: busybox `chcon --reference`, or toybox
   `stat -c %C` where `--reference` is missing.
3. It is renamed over the live file.

The running processes keep the old file until the reboot, and the metamodule's
cp then rewrites a file nothing maps. On alioth the swapped files came out with
`surfaceflinger_exec` and `system_lib_file`.

The fix runs in the installer of the version being installed. So installing
0.4.10 or later over any running copy is covered, while installing 0.4.9 or
older over a running copy still is not.

**On alioth** (0.4.10 over a running 0.4.9, glass on, 02:07): the real update.
- SurfaceFlinger kept its pid (1628) and stayed mapped to the old library
  (inode 114694), while the path moved to the new file (114704). The new file is
  labelled `system_lib_file`, and the binary `surfaceflinger_exec`.
- The engine drew 7 frames within seconds of the copy.
- No tombstone, nothing tripped, and glass stayed on.

**Again** (0.4.11 over 0.4.10, 04:44 the same night, glass on): SurfaceFlinger
kept pid 1628 while the library's path moved to a new file (inode 114704 to
114703). No tombstone, and nothing tripped.

Also seen in that install's output: `chcon: invalid context: 0644`, twice. It is
meta-overlayfs's own `set_perm "$MNT_DIR" 0 0 0755 0644`, which passes a mode
where `set_perm` takes a label, and it is harmless.
