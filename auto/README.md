# Android Auto Glass

Liquid Glass on the car screen, as LSPosed modules for the phone. These are
prototypes. They have run on my Poco F3 and in an emulator, nowhere else.

## Why the engine can't do it alone

Android Auto doesn't hand SurfaceFlinger one car screen. It gives every car
window a private virtual display of its own (the rail, the dashboard, Maps,
each app screen), then composites those displays itself and encodes the result
as video for the head unit. A blur region in one car window sees only what is
below it on that window's display, and that is usually nothing.

Two places still work:

- **Google Maps.** The map is a SurfaceView below Maps' car window, on the
  same display. A blur region over a Maps button gets the engine's real glass
  over the live map.
- **Media screens.** Android Auto draws these itself, artwork included. Here
  the module draws the glass inside Android Auto's own renderer, with the
  engine's shader (`shader/build/glass_lean.sksl`, run as an AGSL
  RuntimeShader), so it looks like the engine's glass but samples the screen's
  own content.

## The modules

| directory | package | LSPosed scope | what it glasses |
|---|---|---|---|
| `maps/` | `dev.stockglass.auto.maps` | Google Maps (`com.google.android.apps.maps`) | Maps' car controls |
| `lists/` | `dev.stockglass.auto.lists` | Android Auto (`com.google.android.projection.gearhead`) | media card, now playing, browse |
| `media/` | `dev.stockglass.auto.media` | Android Auto | the media card only |

`lists/` contains all of `media/`. Enable one of the two, not both. `media/`
stays for its test app (`media/testmedia`, a media app with generated artwork
for the emulator, where Spotify isn't signed in).

### Google Maps

The module finds Maps' car window and puts a blur region under every floating
control with an opaque rounded plate: the map buttons, the Search pill and
its panel with suggestions, the speedometer, the navigation turn card with
its "Then" chip, the ETA panel and the route preview. Each plate keeps 30% of
its fill, so its text still reads. Coloured and dark plates (the turn card)
keep 60%.

Getting the fill translucent takes three tries, in order, because Maps draws
its plates three ways: a colour filter (CardView), the drawable's alpha
(Material shapes), and an alpha layer around the whole drawable (the
speedometer, a LayerDrawable that ignores both). The module draws each plate
into a 9x9 bitmap to check which one worked. A plate whose drawable Maps
replaces more than ten times in two seconds stays stock.

### Media

- **Dashboard card.** Glass under the play, previous and next buttons, over
  the album art. It follows track changes.
- **Full now playing.** Every control (play pill, previous, next, shuffle,
  save, connect, repeat, Jam, queue) gets glass over the blurred artwork
  behind it.
- **Browse, CarPlay style.** On a media app's browse root, the tab bar
  becomes a centred glass capsule floating over the content. The content runs
  the full height of the window and pages under the capsule, and the glass
  follows it live. Albums and playlists open with a header Android Auto draws
  in Compose. Those screens stay stock.

## Install

None of this is in the release zip, and CI doesn't build it. Build the APKs
yourself:

```bash
cd auto/maps && ./build.sh     # out/stockglass-auto-maps.apk
cd auto/lists && ./build.sh    # out/stockglass-auto-lists.apk
```

`lists/` and `media/` need `shader/build/glass_lean.sksl`; run
`shader/gen_glass_sksl.py` first if it isn't there. The build signs with
`~/.android/debug.keystore` unless `MODULE_KEYSTORE` (or `COMPANION_KEYSTORE`
for maps) says otherwise.

Install the APKs, enable them in LSPosed (or Vector), scope them as in the
table above, and force-stop Google Maps and Android Auto. The Stock Glass
module has to be installed with glass on (`glassctl glass`): Maps needs the
engine to draw its regions, and both modules follow the engine's switch.

## Switches

All are system properties. The modules check them every half second while
they draw, and `service call activity 1599295570` makes Maps re-read them at
once.

| property | effect |
|---|---|
| `persist.sys.glass.auto.maps=0` | Maps' controls stay stock |
| `persist.sys.glass.auto.fill` | fill kept on a Maps plate, 0 to 1 (default 0.3) |
| `persist.sys.glass.auto.plates=cards` | only CardView plates, as in the first prototype |
| `persist.sys.glass.auto.dump=<token>` | each new token writes Maps' car view tree to `/data/data/com.google.android.apps.maps/cache/stockglass-tree-<display>.txt` |
| `persist.sys.glass.auto.debug=1` | log every Maps plate and region change |
| `persist.sys.glass.auto.lists=0` | the media glass (`lists/`) stays off |
| `persist.sys.glass.auto.media=0` | the same for `media/` |
| `persist.sys.glass.auto.browse=0` | the browse screen stays stock, the players keep their glass |
| `persist.sys.glass.auto.browse.debug=1` | log the browse list's layout |

## What I tested

Android Auto 18.0, Google Maps and Spotify, shown on Google's Desktop Head
Unit 2.0:

- On my Poco F3 (crDroid, Vector): Maps' buttons, Search panel and
  speedometer; Spotify's dashboard card, full now playing and browse. I saw
  the rows sit under the capsule after a page-down but never caught a frame in
  the middle of a scroll.
- On an Android 17 emulator: the turn card, ETA panel and route preview in
  navigation, and the browse glass following a scroll, with the test media
  app.

Nothing ran in a real car.

## Known gaps

- Heads-up notifications are a car window of their own, so glass there needs
  a hook in Android Auto's compositor (the `:car` process). Not built.
- The engine has one dp scale, taken from the phone's density. On a 160 dpi
  car screen the Maps glass draws its rim bands at the phone's scale, two to
  three times too wide.
- The H.264 stream to the head unit softens the rims.
- The media glass finds the artwork and the buttons by how Android Auto 18.0
  draws them (bitmaps and round rects). If an update draws them as paths, it
  stays stock.
- If Android Auto drew the card before the hooks loaded, the card glass can
  wait for the next track change.
- Android 17 blurs at most ten regions, yet on Maps' car display all 26 of
  mine blurred. I don't know why.
- `dev_attach.sh` (loads a build into a running app without LSPosed) only
  targets the lab emulator, and `media/tools/card_shot.sh` needs the lab in
  `captures/`, which isn't in the repository.
