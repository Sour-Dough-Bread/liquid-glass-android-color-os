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
- sliders, the text-selection toolbar and the screenshot UI;
- the lockscreen clock.

Status: experimental, root only, Android 14 to 17. The installer checks the
ROM's SurfaceFlinger first and refuses to install if the engine can't drive it.
It has been tested on a Poco F3 (crDroid 13.1, Android 17, KernelSU 3 with
meta-overlayfs) and on Android 16 and 17 emulators.

## What's here

| Path | What it is |
|---|---|
| `module/` | The KernelSU / Magisk / APatch module: the installer, boot scripts, `glassctl` and the WebUI. Start with [module/README.md](module/README.md). |
| `engine/` | `libglassengine.so`, which loads into SurfaceFlinger. It resolves Skia's symbols from the ROM's own binary, hooks the blur and draws the glass. |
| `shader/` | Generates the glass shader (SkSL) that the engine embeds. |
| `companion/` | The LSPosed module. See [companion/README.md](companion/README.md). |
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
module/pack.sh       # module/out/stockglass-v<version>.zip
companion/build.sh   # companion/out/stockglass-companion.apk
```

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
