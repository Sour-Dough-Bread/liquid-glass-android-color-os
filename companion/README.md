# Stock Glass Companion

An LSPosed module that turns Quick Settings tiles and the brightness slider,
the text selection toolbar (Cut, Copy, Paste), apps' text toasts, the
screenshot UI's action bar, SystemUI's own dialogs (the Quick Settings
popups), AxionOS's volume panel, launcher folder icons, themed app icons (home
screen, dock and app drawer), home screen widgets, the launcher's search boxes
and the buttons in Overview into blur regions, so that
the Stock Glass engine draws Liquid Glass under them. In the notification shade it uncovers the glass Android 17
already puts under each card (lighter scrims, a lighter card fill), and it lets
heads-up notifications keep that glass. The lockscreen clock is not a region:
SystemUI draws its glyphs as refracting glass itself (`ClockGlass`, after
Kyant0's AndroidLiquidGlass lockscreen demo), on the flex clock, crDroid's
clock styles and the digital clock faces from plugins (Inflate, NumOverlap,
Growth, Calligraphy, Metro). It draws nothing unless the engine is running and
in glass mode. From system_server it also puts a blurred backdrop behind
every app's dialogs (a window attribute, so no app needs to be in scope).

- Design, hooks, cost, opt-in: `../docs/ARCHITECTURE.md`, "The companion".
- What was verified, and how: `../docs/VERIFIED.md`, "The companion on the A17
  AVD" and the companion sections after it (themed icons, the shade, widgets,
  tinted tiles, then fades, sliders, the toolbar, screenshots, search boxes and
  the drawer, then the lockscreen clock and its plugin faces); screenshots and
  numbers in `verify/`.

Status: a prototype. It has run on the A17 AVD under the dev loader below, not
under LSPosed and not on a phone.

## Build

```sh
cd /home/captainhandsome/projects/stock-glass/companion
./build.sh
```

Needs the Android SDK (build-tools 36.0.0, platform android-33), NDK
29.0.14206865 and a JDK, the same as `harness/build.sh`. Output:
`out/stockglass-companion.apk`, signed with the debug key, and
`out/lib/<abi>/libglassagent.so` for the dev loader (not in the APK).

## With LSPosed

Install the APK and enable it in LSPosed with System UI and the launcher in its
scope (`com.android.launcher3` on crDroid, `com.google.android.apps.nexuslauncher`
on Pixel builds). Restart both, or reboot. It then follows the engine's switch.
No app needs to be in scope for the text selection toolbar or toasts: on
Android 17 SystemUI draws every app's toolbar and text toast. Toasts an app
draws with its own view (deprecated since Android 11) are drawn in the app's
process and stay stock. The screenshot UI runs in SystemUI's own
`com.android.systemui:screenshot` process, which the System UI scope covers.
The blur behind apps' dialogs needs System Framework (`android`) in the scope
as well, and a reboot; without it everything else still works.

New in 0.2: the Quick Settings popups (`DialogGlass`), AxionOS's volume panel
(`AxionVolume`), the Overview buttons (`LauncherOverview`) and the blur behind
apps' dialogs (`AppDialogs`). All four were seen working on a Poco F1
(AxionOS, A17), and all but the AxionOS panel on a Poco F3 (crDroid, A17).
Under System Framework scope the hook loads in system_server on both, with no
crashes. Vector added that scope by itself when 0.2 replaced 0.1 on the F1,
but not on the F3.

New in 0.2.2: notification cards keep their glass in the unlocked shade, and
heads-ups over apps keep theirs, on Evolution X and crDroid builds from
2026-11-15 on, which give a card a blur region only on the lock screen. It
has not run on such a build yet; on other ROMs it changes nothing.

App icons get glass only when they are themed: "Themed icons" in a Pixel
build's wallpaper and style settings, the "Minimal" app icon style in crDroid's
ThemePicker. Full-colour icons are left as they are. In the app drawer, only
launchers that theme the drawer's icons get glass there (crDroid does when its
drawer settings ask for themed icons; the Pixel launcher does not).

Widgets on the home screen keep their text and icons; only their background
turns to glass. Where that would make the text hard to read (dark text over a
dark wallpaper, light text over anything but a dark one), the widget keeps
`app.contrast` of its background instead of `app.fill`.

Notification cards and heads-ups get glass only where Android 17's notification
row transparency is on (`Settings.Secure` `notification_row_transparency`; it is
on on alioth): that is what makes each card a blur region. Heads-ups need
LSPosed; the dev loader below cannot do that part. crDroid and Evolution X
builds from 2026-11-15 on keep that region for cards on the lock screen only;
in glass mode the companion gives cards in the shade and heads-ups their region
back, again under LSPosed only. That part has not run on such a build yet. crDroid's own Settings → UI →
Shade style → Glass also lightens the shade's scrim, with or without the
companion.

## Switches

Read at most every 500 ms, while a control draws.

| property | default | |
|---|---|---|
| `persist.sys.glass.mode` | | the engine's switch; `glass` or `on` turns the companion on too |
| `persist.sys.glass.app.qs` | 1 | 0 leaves Quick Settings tiles stock |
| `persist.sys.glass.app.folders` | 1 | 0 leaves folder icons stock |
| `persist.sys.glass.app.icons` | 1 | 0 leaves themed app icons stock |
| `persist.sys.glass.app.widgets` | 1 | 0 leaves home screen widgets stock |
| `persist.sys.glass.app.shade` | 1 | 0 leaves the shade's scrims and notification cards stock |
| `persist.sys.glass.app.headsup` | 1 | 0 leaves heads-up notifications stock |
| `persist.sys.glass.app.sliders` | 1 | 0 leaves Quick Settings' brightness slider (and crDroid's volume slider) stock |
| `persist.sys.glass.app.toolbar` | 1 | 0 leaves the text selection toolbar stock |
| `persist.sys.glass.app.screenshot` | 1 | 0 leaves the screenshot UI's action bar stock |
| `persist.sys.glass.app.toast` | 1 | 0 leaves toasts stock |
| `persist.sys.glass.app.drawer` | 1 | 0 leaves the app drawer's themed icons stock |
| `persist.sys.glass.app.search` | 1 | 0 leaves the drawer's and the dock's search boxes stock |
| `persist.sys.glass.app.volume` | 1 | 0 leaves AxionOS's volume panel stock (the stock volume dialog has blur of its own) |
| `persist.sys.glass.app.dialogs` | 1 | 0 leaves SystemUI's dialogs stock: the Quick Settings popups (Internet, Bluetooth, Do Not Disturb, Cast, …) and their confirmations |
| `persist.sys.glass.app.overview` | 1 | 0 leaves the buttons in Overview (Screenshot, Clear all, …) stock |
| `persist.sys.glass.app.appdialogs` | 1 | 0: no blur behind apps' dialogs (needs System Framework in scope) |
| `persist.sys.glass.app.dialogdim` | 0.5 | an app dialog's dim is multiplied by this while it has blur behind it (AOSP's power menu uses 0.54 with blur, 0.88 without) |
| `persist.sys.glass.app.dialogradius` | 0 | the blur radius behind apps' dialogs in px; 0 is the framework's largest window blur |
| `persist.sys.glass.app.clock` | 1 | 0 leaves the lockscreen clock stock; `plain` gives it translucent glass that refracts nothing (what a live wallpaper gets) |
| `persist.sys.glass.app.clocktint` | 0.5 | how much of a plugin clock face's own colours (Inflate, NumOverlap, …) shows in its glass: 0 is the flex clock's clear glass, 1 the face as drawn with the glass rim on top |
| `persist.sys.glass.app.fill` | 0.3 | alpha kept on a neutral fill |
| `persist.sys.glass.app.accent` | 0.75 | alpha kept on a coloured fill (an active tile) when `app.tint` is 0 |
| `persist.sys.glass.app.tint` | 0.6 | strength of the tinted glass the engine (v0.4.6 or later) draws under a Quick Settings tile that is on, in the tile's own colour; the tile's fill then keeps `app.tilecolor`. 0: untinted glass and `app.accent`. An older engine ignores the tint, so set 0 there |
| `persist.sys.glass.app.tilecolor` | 0.5 | alpha kept on the fill of a Quick Settings tile that is on, over its tinted glass. Together with `app.tint` it decides how see-through an active tile is; much less of both loses a dark-theme tile's dark icon over the dark shade |
| `persist.sys.glass.app.contrast` | 0.65 | alpha kept on a widget's background when its text would be hard to read on glass; 0 turns that off |
| `persist.sys.glass.app.radius` | 0 | region blur radius in px; 0 is the framework's 100 dp |
| `persist.sys.glass.app.scrim` | 0.5 | the shade's scrim alpha (behind and notifications) is multiplied by this |
| `persist.sys.glass.app.scrimcolor` | stock | `stock`: the shade's two scrims in their own colours, a panel colour behind everything and another under the notifications. `black`, `white` or `tint` (the palette's accent, tone 20 in a dark shade, 90 in a light one): one colour over the whole shade, at the behind scrim's alpha |
| `persist.sys.glass.app.cards` | 0.6 | a notification card's fill alpha is multiplied by this |
| `persist.sys.glass.app.punch` | per surface | 1: every region clears what its window drew under the control, as the framework does; 0: none does. Unset, launcher regions clear it and QS tiles keep it (the shade's scrim) |
| `persist.sys.glass.app.rtfollow` | 1 | 0: a region moved on RenderThread alone waits for the window's next UI frame |
| `persist.sys.glass.app.debug` | 0 | 1 logs each region's alpha changes, each widget's contrast decision and, once a second, each region's ancestors; on a property broadcast, every region as it stands (`tools/region_dump.sh`). `rt`: the same, after scaling each window's content to 0.9 and back with a RenderThread animation (a test) |

It also needs the engine to report this boot: `sys.glass.engine` must be `hooked`
or `glass`. Empty passes only on debuggable builds.

A `setprop` reaches each control when it is next drawn. To apply it at once, also
send `SYSPROPS_TRANSACTION`, which runs every app's property change callbacks:

```sh
adb shell service call activity 1599295570
```

## On the AVD, without LSPosed

```sh
cd /home/captainhandsome/projects/stock-glass/companion
ANDROID_SERIAL=emulator-5564 ./dev_attach.sh systemui
```

```sh
cd /home/captainhandsome/projects/stock-glass/companion
ANDROID_SERIAL=emulator-5564 ./dev_attach.sh launcher
```

The screenshot UI's process starts with the first screenshot. Take one, then:

```sh
cd /home/captainhandsome/projects/stock-glass/companion
ANDROID_SERIAL=emulator-5564 ./dev_attach.sh com.android.systemui:screenshot
```

`dev_attach.sh` refuses any build that is not debuggable, so it never touches
the phone. It turns on JDWP for every app (one zygote restart, the first time),
installs the APK, pushes the agent to `/system/lib64` (the AVD must run with
`-writable-system`), restarts the target, and attaches. The script's header
explains each step.

The dev loader (`devloader/`) is a JVMTI agent (`agent.cpp`) plus an
implementation of the parts of the Xposed API the module uses
(`devloader/java`). The module is compiled against those classes but never
contains them, so the same `classes.dex` runs under LSPosed. Limits:

- A hook is a breakpoint at method entry. `beforeHookedMethod` can read and
  replace arguments; `setResult` is ignored (with one warning), and
  `afterHookedMethod` is never called.
- At most 32 hooks, and 48 argument slots per method.
- A hook sees only calls made after the attach. The script makes SystemUI
  recompose Quick Settings with a night-mode round trip, and makes the launcher
  redraw every view by turning "show layout bounds" on and off.
- A process that is not debuggable crashes on attach, so the script checks
  `adb jdwp` first.

The lockscreen clock needs a lockscreen, which the AVD does not have by
default. A swipe lockscreen is enough:

```sh
adb -s emulator-5564 shell locksettings set-disabled false
```

Then press power twice. The clock is laid out again when the keyguard shows,
which is when the hook first sees it. `locksettings set-disabled true` turns
the lockscreen off again.

Clock faces from plugins need the plugins, and the AVD's SystemUI refuses
crDroid's: it loads a clock plugin only if the plugin holds
`com.android.systemui.permission.PLUGIN`, a signature permission. So the test
uses a variant of the module with one more, test-only hook
(`tools/devtest/PluginGrant.java`) that has that check ask about SystemUI
itself, for `com.android.systemui.clocks.*` only. It is never in the module
(`build.sh` compiles `java/` alone), and the variant's `classes.dex` is the
module's own. With crDroid's `SystemUIClocks-*.apk` (from its
`system_ext.img`, `/priv-app`) at hand:

```sh
cd /home/captainhandsome/projects/stock-glass/companion
./build.sh && ./tools/devtest_plugins.sh
APK=out/devtest/stockglass-companion-devtest.apk ANDROID_SERIAL=emulator-5564 ./dev_attach.sh systemui
```

Then install (or reinstall) the plugin APKs, which makes SystemUI load them
through the hooked check, and pick a face:

```sh
adb -s emulator-5564 shell settings put secure lock_screen_custom_clock_face '{"clockId":"DIGITAL_CLOCK_INFLATE"}'
```

The ids are `DIGITAL_CLOCK_INFLATE`, `_NUMBEROVERLAP`, `_GROWTH`,
`_CALLIGRAPHY`, `_METRO` and `ANALOG_CLOCK_BIGNUM`; deleting the setting goes
back to the flex clock.

## Measuring

`tools/` (each takes `ANDROID_SERIAL`, default `emulator-5564`):

| script | |
|---|---|
| `shade_count.sh` | blur regions on the shade's layer, 0 when it has none |
| `launcher_count.sh` | blur regions on the home screen's layer, 0 when it has none |
| `qsshot.sh <out.png> [min]` | open Quick Settings, wait for `min` regions, take a screenshot |
| `gate_matrix.sh` | the shade's region count for every switch |
| `cost.sh <label> [prop=value…]` | RenderEngine time per frame over 6 QS open/close cycles (TimeStats) |
| `qs_hold.sh <out.png> <to_y> [from_y]` | drag Quick Settings open to `to_y`, hold, take a screenshot |
| `region_dump.sh [process]` | every region of a process as it stands (`app.debug=1` and a property broadcast); default the Pixel launcher, `com.android.launcher3` on crDroid |
| `chaos.sh [trials] [seed] [outdir]` | stale launcher glass hunt: random home screen actions (page flings, home and overview gestures mid-animation, app launches, the drawer, screen off, Wallpaper & style), then the region dump and the launcher layer's `blurRegions.size()` after each trial and again after HOME; counts regions with glass whose control is off screen (stale), whose rect is off their control (misplaced), and SurfaceFlinger regions beyond those shown (excess). The same seed replays the same actions |
| `stale_repro.sh [out-prefix]` | the shortest sequence found that leaves home screen glass behind (overview, back into the app, HOME, page swipes), then stale and shown regions, the launcher layer's count, and how often the companion replayed moves from an earlier surface or its sweep put rects back |
| `perf_home.sh <label> [swipes]` | what the home screen's glass costs on a fixed workload (stress page, drawer, page swipes): the per-frame sync's µs per pre-draw, median UI and RenderThread times, frame time percentiles, SurfaceFlinger's RenderEngine time, its log lines, live regions |
| `toast_check.sh [out-prefix]` | a real text toast (Settings' Build number, tapped four times): SystemUI's toast regions and the toast layer's `blurRegions.size()` while it shows and after it hides |

## Layout

```
AndroidManifest.xml   Xposed metadata only: no activity, service or permission
assets/xposed_init    the entry class
java/…/Entry.java            which process gets which hooks
java/…/QsTiles.java          SystemUI: TileKt.TileExpandable
java/…/Sliders.java          SystemUI: SliderDefaults.Track (brightness, crDroid's volume)
java/…/TextToolbar.java      SystemUI: RemoteSelectionToolbar.show (every app's text toolbar)
java/…/Toasts.java           SystemUI: ToastPresenter.show (every app's text toast)
java/…/ScreenshotGlass.java  SystemUI :screenshot: the action bar
java/…/ClockGlass.java       SystemUI: the lockscreen clock (flex clock, crDroid's clock styles, plugin faces); a RenderEffect, not a region
java/…/LauncherFolders.java  Launcher3: PreviewBackground.drawBackground
java/…/ShadeGlass.java       SystemUI: shade scrims, notification cards, heads-ups
java/…/LauncherIcons.java    Launcher3: themed icons on the home screen, dock and drawer (FastBitmapDrawable, ThemedIconDelegate)
java/…/LauncherWidgets.java  Launcher3: home screen widgets (CellLayout.dispatchDraw, one overlay per widget), Pixel's dock search widget
java/…/LauncherSearch.java   Launcher3: the drawer's search box, crDroid's dock search bar
java/…/ViewGlass.java        glass under a view whose background is the control's plate
java/…/Drawer.java           Launcher3: the app drawer's position, as a fade
java/…/Region.java           one BackgroundBlurDrawable per control
java/…/ComposeBridge.java    reflection into SystemUI's Compose
java/…/Gate.java             the switches
java/…/L.java                logging, tag StockGlassApp
devloader/            JVMTI agent and Xposed API stand-in (development only)
tools/                measuring scripts
tools/devtest/        test-only hooks for the AVD, never in the module (devtest_plugins.sh builds the variant)
verify/               screenshots and numbers from the AVD
```
