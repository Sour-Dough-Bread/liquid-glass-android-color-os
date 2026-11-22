package dev.stockglass.auto.maps;

import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import java.lang.reflect.Method;
import java.util.List;
import java.util.concurrent.CopyOnWriteArrayList;

/**
 * Whether Maps' car controls get glass, read from the engine's own properties
 * (adapted from the companion's Gate, which has the full reasoning):
 *
 *   persist.sys.glass.mode           glass|on, else stock (the engine's switch)
 *   persist.sys.glass.bypass=1       stock, as in the engine
 *   persist.sys.glass.enabled=0      the engine is not installed
 *   sys.glass.engine                 hooked|glass this boot (empty passes on debuggable builds)
 *   persist.sys.glass.auto.maps=0    leave Maps' car controls stock
 *   persist.sys.glass.auto.fill      alpha kept on a control's plate (default 0.3)
 *   persist.sys.glass.auto.radius    region blur radius in px; 0 = the framework default
 *   persist.sys.glass.auto.debug=1   log every plate found and every region change
 *   persist.sys.glass.auto.plates=cards  only androidx CardView plates (the first prototype);
 *                                    default: any floating view with an opaque rounded plate
 *   persist.sys.glass.auto.dump=<t>  each new value <t>: write every car window's visible views
 *                                    (class, id, bounds, background, outline) to Maps' cache dir
 *   persist.sys.glass.auto.probe=N   test only: N extra 4 x 4 px regions inside the first
 *                                    plate, to find Android 17's limit on blur requests
 *   persist.sys.glass.auto.proberadii=1  test only: each probe asks for a blur radius of its own
 *   persist.sys.glass.app.rtfollow=0 as in the companion (Region.followRenderThread)
 *
 * Read at most every 500 ms from the draw path; a property change broadcast
 * (`service call activity 1599295570`) re-reads at once and redraws.
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
    private static boolean glass, maps, debug, rtFollow = true;
    private static float fill = 0.3f;
    private static int radius, probe;
    private static String dump = "";
    private static boolean anyPlate = true;
    private static boolean probeRadii;
    /** Set when a newer build of this module took over the process (MapsCar.dispose). */
    static volatile boolean retired;

    private Gate() {}

    static void onChange(Runnable r) {
        listeners.add(r);
    }

    static synchronized void watch() {
        if (watching) return;
        watching = true;
        try {
            Class.forName("android.os.SystemProperties").getMethod("addChangeCallback", Runnable.class)
                    .invoke(null, (Runnable) () -> {
                        if (!retired) handler().post(Gate::reread);
                    });
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
        if (debug) Region.dump("1");
    }

    static synchronized Handler handler() {
        if (main == null) main = new Handler(Looper.getMainLooper());
        return main;
    }

    /** Glass on Maps' car controls now. */
    static boolean maps() {
        refresh();
        return glass && maps && !retired;
    }

    static boolean debug() {
        refresh();
        return debug;
    }

    static boolean rtFollow() {
        refresh();
        return rtFollow;
    }

    static float fill() {
        refresh();
        return fill;
    }

    static int radius() {
        refresh();
        return radius;
    }

    static int probe() {
        refresh();
        return probe;
    }

    /** Whether views other than CardViews get glass when they draw an opaque rounded plate. */
    static boolean anyPlate() {
        refresh();
        return anyPlate;
    }

    /** The dump token: a new non-empty value asks for one dump of each car window. */
    static String dump() {
        refresh();
        return dump;
    }

    static boolean probeRadii() {
        refresh();
        return probeRadii;
    }

    private static synchronized void refresh() {
        long now = SystemClock.uptimeMillis();
        if (now < next) return;
        next = now + REFRESH_MS;
        String was = glass + " " + maps + " " + fill + " " + radius + " " + debug + " " + probe + " " + probeRadii
                + " " + dump + " " + anyPlate;
        String mode = prop("mode", "");
        glass = engineLive()
                && (mode.equals("glass") || mode.equals("on"))
                && !prop("bypass", "").equals("1")
                && !prop("enabled", "1").equals("0");
        maps = !prop("auto.maps", "1").equals("0");
        fill = clamp(num(prop("auto.fill", ""), 0.3f));
        radius = Math.max(0, Math.round(num(prop("auto.radius", ""), 0f)));
        probe = Math.max(0, Math.min(32, Math.round(num(prop("auto.probe", ""), 0f))));
        probeRadii = prop("auto.proberadii", "0").equals("1");
        String d = prop("auto.debug", "0");
        debug = !d.isEmpty() && !d.equals("0");
        rtFollow = !prop("app.rtfollow", "1").equals("0");
        dump = prop("auto.dump", "");
        anyPlate = !prop("auto.plates", "").equals("cards");
        String is = glass + " " + maps + " " + fill + " " + radius + " " + debug + " " + probe + " " + probeRadii
                + " " + dump + " " + anyPlate;
        if (primed && !is.equals(was)) {
            L.i("switches now glass=" + glass + " maps=" + maps + " fill=" + fill + " radius=" + radius
                    + " probe=" + probe);
            for (Runnable r : listeners) handler().post(r);
        }
        primed = true;
    }

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
                get = Class.forName("android.os.SystemProperties").getMethod("get", String.class, String.class);
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
