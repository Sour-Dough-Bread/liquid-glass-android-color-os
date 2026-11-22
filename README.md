# Liquid Glass for Android

Apple's Liquid Glass, drawn by Android's own compositor. A root module hooks
one function in SurfaceFlinger, `BlurFilter::drawBlurRegion`, and draws
refracting glass wherever the system already blurs something: notification
cards, the volume panel, blurred dialogs and, if you want, the shade behind
them. Nothing is themed and no app is patched. Switch it off and Android's blur
comes back, pixel for pixel.

An optional LSPosed companion makes glass of surfaces that have no blur of
their own:
- Quick Settings tiles;
- the launcher's icons, folders, widgets and search box;
- heads-up notifications;
- sliders, the text selection toolbar, toasts and the screenshot UI;
- the lockscreen clock.

There's a write-up of how it works on my site:
[Liquid Glass inside Android's compositor](https://capthnds.me/blog/liquid-glass-android/).

## See it in action

<p align="center">
  <a href="https://bnlg.capthnds.me/vclPImkJ3Q7a3FKgli/screen-20261122-013006-1795282158046.mp4">
    <img src="docs/images/demo.webp" width="45%" alt="Play the screen recording: a glass calendar widget, app icons, dock and search bar over a swirling live wallpaper" />
  </a>
</p>

The picture opens a 48-second screen recording from my phone, with an
animated shader as the live wallpaper. The wallpaper changes every frame,
so SurfaceFlinger redraws all the glass above it every frame too. It goes
from the home screen's widgets to the app drawer and Gboard, then the
shade, Quick Settings, the module's WebUI in recents and the volume panel.

## Screenshots

From a Poco F3 on crDroid 13.1, Android 17.

<p>
  <img src="docs/images/home.webp" width="49%" alt="The home screen, with the calendar and music widgets, app icons, dock and search bar drawn as glass" />
  <img src="docs/images/lockscreen.webp" width="49%" alt="The lockscreen, with the clock's digits drawn as glass" />
</p>
<p>
  <img src="docs/images/heads-up.webp" width="99%" alt="A heads-up notification as a glass card" />
</p>
<p>
  <img src="docs/images/volume-keyboard.webp" width="99%" alt="The volume panel's sliders and Gboard's keys as glass" />
</p>
<p align="center">
  <img src="docs/images/quick-settings.webp" width="60%" alt="Quick Settings with glass tiles" />
</p>
<p>
  <img src="docs/images/toolbar-screenshot.webp" width="99%" alt="The text selection toolbar and the screenshot action bar as glass" />
</p>

## Download

| Build | Where |
|---|---|
| Module | [The latest release](https://github.com/capthndsme/liquid-glass-android/releases/latest), with its sha256 |
| [Companion APK](https://github.com/capthndsme/liquid-glass-android/releases/latest/download/stockglass-companion.apk) | the same release; its notes give the sha256 |

GitHub Actions builds every push to this repository and publishes it under
[Releases](https://github.com/capthndsme/liquid-glass-android/releases). The
latest release is the newest numbered version. A build between two versions is
a pre-release named like `v0.4.20-3-g1a2b3c4`, and the module in it reports
that name. Every release after v0.4.21 has the companion APK too, built by the
same workflow. All companion builds, 0.1 included, are signed with one key, so
each installs over the one before.

It's experimental and needs root on Android 14 to 17. The installer checks the
ROM's SurfaceFlinger first and refuses to install if the engine can't drive it.
If a boot ever fails with the module installed, it disables itself on the next
boot. Support for ROMs other than the two I run is newer still: see
[Other ROMs](#other-roms-experimental).

1. Flash the module zip in KernelSU, Magisk or APatch, and reboot. KernelSU 3
   also needs a metamodule such as meta-overlayfs.
2. Turn glass on with the module's Action button, its WebUI or
   `sh /data/adb/modules/stockglass/glassctl glass`. Glass is off until you do.
3. For the companion, install the APK and enable it in LSPosed with System UI
   and your launcher in its scope. Then reboot.

[module/README.md](module/README.md) and
[companion/README.md](companion/README.md) cover the settings and each
surface's requirements. For example, app icons get glass only when they are
themed.

## Other ROMs (experimental)

I wrote Stock Glass on two phones, a Poco F3 on crDroid and a Poco F1 on
AxionOS. After I posted it, people on other ROMs reported "Flash failed". So
for v0.4.21 I collected the SurfaceFlinger from 66 firmware builds and taught
the engine what they do differently:

- **Evolution X 12.x, and crDroid 13.x builds made after 2026-11-11.** Their
  SurfaceFlinger has a Liquid Glass of its own, and its blur function takes
  one more argument, a material. v0.4.20 refused to install there. The engine
  now hooks that form too, and in glass mode it draws Evolution X's glass
  regions in its own look; `glassctl set romMaterials 1` leaves them to
  Evolution X. Both ROMs have blurred notification cards only on the lock
  screen since 2026-11-15, and the companion brings their glass back in the
  shade.
- **Android 15 builds where Skia's uniform lookups were compiled away:**
  Pixel's AP3A and BP1A, LineageOS 22.2, Nothing OS 3.2 on MediaTek and
  /e/OS on Android 15. The engine now lays out the shader's uniforms itself.
- **Builds with branch protection (BTI),** 22 of the 66, among them Pixel 8
  and later from Android 17 QPR1. The Pixel 8 and 8 Pro render with Graphite
  from that release on, and there the glass stands down and Android's blur
  stays.
- **Lunaris and Infinity-X,** which scale the blurred image differently. The
  engine reads the scale from each build when it starts.

The installer's check passes on all 66: Pixel (19 builds), LineageOS (16),
Evolution X and crDroid (9), Nothing OS (8), Motorola (5), and Infinity-X,
PixelOS, Lunaris, AxionOS, /e/OS and a TrebleDroid GSI (9). That checks the
binaries on a computer. None of the new code has run on a phone yet, mine
included, so expect bugs. The safety nets are the ones the module always had: the
installer refuses a SurfaceFlinger it can't drive, a failed boot disables the
module, and a SurfaceFlinger crash with glass on brings Android's blur back.

Some builds have nothing to draw on. Where the vendor turns window blur off
(the moto g 2025, some Nothing OS and LineageOS 22.2 builds), there is no blur
for the glass to replace. Samsung's and OnePlus's firmware weren't part of the
survey.

If you try it on one of these ROMs, please
[open an issue](https://github.com/capthndsme/liquid-glass-android/issues)
with your ROM, your phone and what the installer printed.

ROM developers can build the glass into SurfaceFlinger instead, with no root
and no hook: [rom/](rom/README.md) makes a `frameworks/native` patch for AOSP,
LineageOS, Evolution X and crDroid trees. The patches compile, but no ROM
with them has booted yet.
[docs/VERIFIED.md](docs/VERIFIED.md#many-surfaceflingers-the-firmware-survey-2026-11-21-and-22)
has the survey in full.

## What's here

| Path | What it is |
|---|---|
| `module/` | The KernelSU / Magisk / APatch module: the installer, boot scripts, `glassctl` and the WebUI. |
| `engine/` | `libglassengine.so`, which loads into SurfaceFlinger. It resolves Skia's symbols from the ROM's own binary, hooks the blur and draws the glass. |
| `shader/` | Generates the glass shader (SkSL) that the engine embeds. |
| `companion/` | The LSPosed module. |
| `harness/` | A test app that makes blur regions on demand. |
| `docs/` | [ARCHITECTURE.md](docs/ARCHITECTURE.md) explains the design. [VERIFIED.md](docs/VERIFIED.md) records what was tested where, with numbers. |

## Building

You need Android NDK 29.0.14206865 and Python 3. Set `ANDROID_NDK` if the NDK
isn't at `$HOME/Android/Sdk/ndk/29.0.14206865`. The companion also needs the
Android SDK (build-tools 36.0.0, platform android-33) and a JDK.

```sh
engine/vendor/fetch_skia_headers.sh
engine/vendor/fetch_xz.sh
engine/vendor/fetch_frida_gum.sh
engine/test/host.sh  # the tests that need no device
module/pack.sh       # module/out/stockglass-v<version>.zip
companion/build.sh   # companion/out/stockglass-companion.apk
```

The fetch scripts take pinned commits and checked archives, so a build made
elsewhere compiles the same third-party code. The release workflow
(`.github/workflows/release.yml`) runs these same scripts.

## Credits

- The glass maths comes from Kyant's
  [AndroidLiquidGlass](https://github.com/Kyant0/AndroidLiquidGlass)
  (Apache-2.0), by way of the Android port in
  [expo-liquid-glass-ui](https://github.com/capthndsme/expo-liquid-glass-ui).
  `shader/gen_glass_sksl.py` marks the derived portions.
- These are fetched at build time, not kept in this repo:
  - [Frida Gum](https://github.com/frida/frida-gum), for the hook;
  - [XZ Embedded](https://tukaani.org/xz/embedded.html), to read the ROM's
    compressed debug info;
  - [patchelf](https://github.com/NixOS/patchelf);
  - Skia's headers.
