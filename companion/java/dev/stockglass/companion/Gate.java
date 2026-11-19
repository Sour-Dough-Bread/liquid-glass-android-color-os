package dev.stockglass.companion;

import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import java.lang.reflect.Method;
import java.util.List;
import java.util.concurrent.CopyOnWriteArrayList;

/**
 * Whether the companion draws anything, read from the same properties as the
 * engine so there is one switch: glass mode on means glass on the controls
 * too, and stock mode leaves them exactly as the ROM draws them.
 *
 *   persist.sys.glass.mode          glass|on, else stock (the engine's switch)
 *   persist.sys.glass.bypass=1      stock, as in the engine
 *   persist.sys.glass.enabled=0     the engine is not installed; nothing to draw for
 *   persist.sys.glass.app.qs=0      leave Quick Settings tiles alone
 *   persist.sys.glass.app.folders=0 leave launcher folder icons alone
 *   persist.sys.glass.app.icons=0   leave themed app icons alone
 *   persist.sys.glass.app.widgets=0 leave home screen widgets alone
 *   persist.sys.glass.app.shade=0   leave the shade's scrims and notification cards alone
 *   persist.sys.glass.app.headsup=0 leave heads-up notifications alone
 *   persist.sys.glass.app.sliders=0 leave Quick Settings' brightness and volume sliders alone
 *   persist.sys.glass.app.toolbar=0 leave the text selection toolbar (Copy, Paste) alone
 *   persist.sys.glass.app.screenshot=0 leave the screenshot UI's action bar alone
 *   persist.sys.glass.app.toast=0   leave toasts alone
 *   persist.sys.glass.app.drawer=0  leave the app drawer's icons alone
 *   persist.sys.glass.app.search=0  leave the launcher's search boxes alone
 *   persist.sys.glass.app.volume=0  leave the volume panel alone
 *   persist.sys.glass.app.dialogs=0 leave SystemUI's dialogs (Quick Settings popups, ...) alone
 *   persist.sys.glass.app.overview=0 leave the buttons in Overview (recents) alone
 *   persist.sys.glass.app.appdialogs=0 no blur behind apps' dialogs (system_server)
 *   persist.sys.glass.app.dialogdim an app dialog's dim is multiplied by this while it is
 *                                   blurred behind (default 0.5)
 *   persist.sys.glass.app.dialogradius blur behind an app dialog, in px; 0 = the framework's
 *                                   largest window blur (max_window_blur_radius)
 *   persist.sys.glass.app.clock=0   leave the lockscreen clock alone (ClockGlass); "plain":
 *                                   translucent glass with no refraction, as with a live wallpaper
 *   persist.sys.glass.app.clocktint how much of a plugin clock face's own colours shows in its
 *                                   glass (default 0.5; 0 = clear glass, as on the flex clock)
 *   persist.sys.glass.app.fill      alpha kept on a neutral fill (default 0.3)
 *   persist.sys.glass.app.accent    alpha kept on a coloured fill, e.g. an active tile (default 0.75)
 *   persist.sys.glass.app.tint      strength of the engine's tinted glass under a QS tile
 *                                   that is on, in the tile's own colour (default 0.6;
 *                                   0 = untinted glass and app.accent on the fill)
 *   persist.sys.glass.app.tilecolor alpha kept on the fill of a QS tile that is on, over
 *                                   its tinted glass (default 0.5)
 *   persist.sys.glass.app.contrast  alpha kept on a widget's plate when its text would be
 *                                   hard to read on the glass (default 0.65; see LauncherWidgets)
 *   persist.sys.glass.app.radius    blur radius of the regions in px; 0 = the framework default
 *   persist.sys.glass.app.scrim     the shade's scrim alpha is multiplied by this (default 0.5)
 *   persist.sys.glass.app.scrimcolor stock: the shade's two scrims in their own colours;
 *                                   black, white or tint: one colour over the whole shade
 *                                   (tint: the palette's accent, dark or light with the theme)
 *   persist.sys.glass.app.cards     a notification card's fill alpha is multiplied by this
 *                                   (default 0.6)
 *   persist.sys.glass.app.punch     1: every region clears what its window drew under
 *                                   the control, as SystemUI's own notification cards
 *                                   do; 0: none does. Unset, each surface picks: QS
 *                                   tiles keep it (the shade's scrim stays over the
 *                                   glass), launcher icons and folders clear it
 *                                   (nothing the launcher draws belongs under them,
 *                                   and a dock prediction can be left under a pinned
 *                                   icon after a cold start)
 *   persist.sys.glass.app.rtfollow=0 do not ask for a UI frame when a region moves on
 *                                   RenderThread alone (Region.followRenderThread)
 *   persist.sys.glass.app.debug=1   log each region's alpha changes, each widget's
 *                                   contrast decision and, once a second, each
 *                                   region's ancestors; on a property change broadcast,
 *                                   every region as it stands (tools/region_dump.sh).
 *                                   "rt": the same, after scaling each window's content
 *                                   to 0.9 and back on RenderThread alone (a test)
 *
 * The mode is persistent, so it still says glass on a boot where the engine is
 * not running: the module disabled, stood down by its boot guard, or removed.
 * A region then gets Android's own blur, not glass. So the companion also
 * needs the engine's own report for this boot, sys.glass.engine: "hooked"
 * (inside SurfaceFlinger) or "glass" (its shader compiled). "not-hooked" and
 * "no-glass" mean it cannot draw glass, and empty means it never ran. The
 * exception is empty on a debuggable build, where the engine is a dev install
 * (engine/install_dev.sh) that may predate the report, as the test AVD's
 * does, and the mode alone decides. alioth, a user build, reports "glass".
 *
 * Read at most every 500 ms, from the draw path. A control only sees a change
 * when it is drawn again, and the host does not redraw what it has no reason
 * to (an off-screen QS page kept its glass after a switch to stock). So a
 * refresh that finds anything changed runs the onChange listeners on the main
 * thread, and they make every control redraw.
 */
final class Gate {
    private static final String P = "persist.sys.glass.";
    private static final long REFRESH_MS = 500;
    private static final List<Runnable> listeners = new CopyOnWriteArrayList<>();

    private static Method get;
    private static Handler main;
    private static boolean primed, watching;
    private static long next;
    private static int debuggable = -1;
    private static String engineSeen;
    private static boolean glass, qs, folders, icons, widgets, shade, headsUp, sliders, toolbar, screenshot,
            toast, drawer, search, clock, clockPlain, volume, dialogs, overview, appDialogs, debug, rtFollow = true;
    private static String punch = "", scrimColor = "stock";
    private static String debugMode = "0";
    private static float fill = 0.3f, accent = 0.75f, tint = 0.6f, tileColor = 0.5f, contrast = 0.65f,
            scrim = 0.5f, cards = 0.6f, clockTint = 0.5f, appDialogDim = 0.5f;
    private static int radius, appDialogRadius;

    private Gate() {}

    /** Runs {@code r} on the main thread after any refresh that changed a setting. */
    static void onChange(Runnable r) {
        listeners.add(r);
    }

    /**
     * Also re-reads the properties when the system announces a property
     * change, so a host sitting idle (the launcher at home, nothing redrawing)
     * follows a switch at once rather than at its next redraw. setprop alone
     * announces nothing; `service call activity 1599295570`
     * (SYSPROPS_TRANSACTION) runs every app's change callbacks.
     */
    static synchronized void watch() {
        if (watching) return;
        watching = true;
        try {
            Class.forName("android.os.SystemProperties").getMethod("addChangeCallback", Runnable.class)
                    .invoke(null, (Runnable) () -> handler().post(Gate::reread));
        } catch (Throwable t) {
            L.once("prop-callback", "no property change callback; a switch applies when the"
                    + " control is next drawn", t);
        }
    }

    private static void reread() {
        synchronized (Gate.class) {
            next = 0;
        }
        refresh();
        if (debug) Region.dump(debugMode);
    }

    private static synchronized Handler handler() {
        if (main == null) main = new Handler(Looper.getMainLooper());
        return main;
    }

    static boolean qs() {
        refresh();
        return glass && qs;
    }

    static boolean folders() {
        refresh();
        return glass && folders;
    }

    static boolean icons() {
        refresh();
        return glass && icons;
    }

    static boolean widgets() {
        refresh();
        return glass && widgets;
    }

    static boolean shade() {
        refresh();
        return glass && shade;
    }

    static boolean headsUp() {
        refresh();
        return glass && headsUp;
    }

    static boolean sliders() {
        refresh();
        return glass && sliders;
    }

    static boolean toolbar() {
        refresh();
        return glass && toolbar;
    }

    static boolean screenshot() {
        refresh();
        return glass && screenshot;
    }

    static boolean toast() {
        refresh();
        return glass && toast;
    }

    static boolean drawer() {
        refresh();
        return glass && drawer;
    }

    static boolean search() {
        refresh();
        return glass && search;
    }

    static boolean volume() {
        refresh();
        return glass && volume;
    }

    static boolean dialogs() {
        refresh();
        return glass && dialogs;
    }

    static boolean overview() {
        refresh();
        return glass && overview;
    }

    static boolean appDialogs() {
        refresh();
        return glass && appDialogs;
    }

    /** An app dialog's dim is multiplied by this while it is blurred behind. */
    static float appDialogDim() {
        refresh();
        return appDialogDim;
    }

    /** Blur behind an app dialog in px; 0 for the framework's own radius. */
    static int appDialogRadius() {
        refresh();
        return appDialogRadius;
    }

    static boolean clock() {
        refresh();
        return glass && clock;
    }

    /** persist.sys.glass.app.clock=plain: the clock's glass refracts nothing. */
    static boolean clockPlain() {
        refresh();
        return clockPlain;
    }

    /** How much of a plugin clock face's colours its glass keeps, 0 to 1. */
    static float clockTint() {
        refresh();
        return clockTint;
    }

    /** persist.sys.glass.app.rtfollow=0: glass moved on RenderThread alone waits for a redraw. */
    static boolean rtFollow() {
        refresh();
        return rtFollow;
    }

    static float scrim() {
        refresh();
        return scrim;
    }

    static float cards() {
        refresh();
        return cards;
    }

    /** persist.sys.glass.app.debug=1: log region changes. */
    static boolean debug() {
        refresh();
        return debug;
    }

    static float fill() {
        refresh();
        return fill;
    }

    static float accent() {
        refresh();
        return accent;
    }

    static float contrast() {
        refresh();
        return contrast;
    }

    static float tint() {
        refresh();
        return tint;
    }

    /** Alpha kept on the fill of a QS tile that is on, over its tinted glass. */
    static float tileColor() {
        refresh();
        return tileColor;
    }

    /** "stock", "black", "white" or "tint" (see the class comment). */
    static String scrimColor() {
        refresh();
        return scrimColor;
    }

    static int radius() {
        refresh();
        return radius;
    }

    /** app.punch if it is set, else the surface's own choice (see the class comment). */
    static boolean punch(boolean byDefault) {
        refresh();
        return punch.equals("1") || (byDefault && !punch.equals("0"));
    }

    private static synchronized void refresh() {
        long now = SystemClock.uptimeMillis();
        if (now < next) return;
        next = now + REFRESH_MS;
        String was = glass + " " + qs + " " + folders + " " + icons + " " + widgets + " " + shade + " " + headsUp
                + " " + sliders + " " + toolbar + " " + screenshot + " " + toast + " " + drawer + " " + search + " " + clock
                + " " + volume + " " + dialogs + " " + overview + " " + appDialogs
                + " " + clockPlain + " " + clockTint + " " + fill + " " + accent + " " + tint + " " + tileColor + " " + contrast
                + " " + scrim + " " + scrimColor + " " + cards + " " + radius + " " + punch;
        String mode = prop("mode", "");
        glass = engineLive()
                && (mode.equals("glass") || mode.equals("on"))
                && !prop("bypass", "").equals("1")
                && !prop("enabled", "1").equals("0");
        qs = !prop("app.qs", "1").equals("0");
        folders = !prop("app.folders", "1").equals("0");
        icons = !prop("app.icons", "1").equals("0");
        widgets = !prop("app.widgets", "1").equals("0");
        shade = !prop("app.shade", "1").equals("0");
        headsUp = !prop("app.headsup", "1").equals("0");
        sliders = !prop("app.sliders", "1").equals("0");
        toolbar = !prop("app.toolbar", "1").equals("0");
        screenshot = !prop("app.screenshot", "1").equals("0");
        toast = !prop("app.toast", "1").equals("0");
        drawer = !prop("app.drawer", "1").equals("0");
        search = !prop("app.search", "1").equals("0");
        volume = !prop("app.volume", "1").equals("0");
        dialogs = !prop("app.dialogs", "1").equals("0");
        overview = !prop("app.overview", "1").equals("0");
        appDialogs = !prop("app.appdialogs", "1").equals("0");
        appDialogDim = clamp(num(prop("app.dialogdim", ""), 0.5f));
        appDialogRadius = Math.max(0, Math.round(num(prop("app.dialogradius", ""), 0f)));
        String clockMode = prop("app.clock", "1");
        clock = !clockMode.equals("0");
        clockPlain = clockMode.equals("plain");
        clockTint = clamp(num(prop("app.clocktint", ""), 0.5f));
        fill = clamp(num(prop("app.fill", ""), 0.3f));
        accent = clamp(num(prop("app.accent", ""), 0.75f));
        tint = clamp(num(prop("app.tint", ""), 0.6f));
        tileColor = clamp(num(prop("app.tilecolor", ""), 0.5f));
        contrast = clamp(num(prop("app.contrast", ""), 0.65f));
        scrim = clamp(num(prop("app.scrim", ""), 0.5f));
        String sc = prop("app.scrimcolor", "stock");
        scrimColor = sc.equals("black") || sc.equals("white") || sc.equals("tint") ? sc : "stock";
        cards = clamp(num(prop("app.cards", ""), 0.6f));
        radius = Math.max(0, Math.round(num(prop("app.radius", ""), 0f)));
        punch = prop("app.punch", "");
        rtFollow = !prop("app.rtfollow", "1").equals("0");
        debugMode = prop("app.debug", "0");
        debug = !debugMode.isEmpty() && !debugMode.equals("0");
        String is = glass + " " + qs + " " + folders + " " + icons + " " + widgets + " " + shade + " " + headsUp
                + " " + sliders + " " + toolbar + " " + screenshot + " " + toast + " " + drawer + " " + search + " " + clock
                + " " + volume + " " + dialogs + " " + overview + " " + appDialogs
                + " " + clockPlain + " " + clockTint + " " + fill + " " + accent + " " + tint + " " + tileColor + " " + contrast
                + " " + scrim + " " + scrimColor + " " + cards + " " + radius + " " + punch;
        if (primed && !is.equals(was)) {
            for (Runnable r : listeners) handler().post(r);
        }
        primed = true;
    }

    /** Whether the engine is drawing glass this boot (see the class comment). */
    private static boolean engineLive() {
        String engine = sys("sys.glass.engine", "");
        if (debuggable < 0) debuggable = sys("ro.debuggable", "0").equals("1") ? 1 : 0;
        boolean live = engine.equals("hooked") || engine.equals("glass")
                || (engine.isEmpty() && debuggable == 1);
        if (!engine.equals(engineSeen)) {
            engineSeen = engine;
            L.i("engine reports \"" + engine + "\"" + (live ? "" : "; controls stay stock"));
        }
        return live;
    }

    private static String prop(String name, String def) {
        return sys(P + name, def);
    }

    private static String sys(String key, String def) {
        try {
            if (get == null) {
                get = Class.forName("android.os.SystemProperties")
                        .getMethod("get", String.class, String.class);
            }
            return (String) get.invoke(null, key, def);
        } catch (Throwable t) {
            L.once("prop", "cannot read system properties; staying stock", t);
            return def;
        }
    }

    private static float num(String s, float def) {
        if (s == null || s.isEmpty()) return def;
        try {
            return Float.parseFloat(s);
        } catch (NumberFormatException e) {
            return def;
        }
    }

    private static float clamp(float v) {
        return Math.max(0f, Math.min(1f, v));
    }
}
