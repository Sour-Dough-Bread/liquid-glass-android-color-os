package dev.stockglass.companion;

import android.graphics.Canvas;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import de.robv.android.xposed.XC_MethodHook;
import de.robv.android.xposed.XposedBridge;
import java.lang.ref.WeakReference;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.WeakHashMap;

/**
 * The brightness slider in Quick Settings, and crDroid's volume slider beside
 * it (Android 17, Compose QS).
 *
 * Both are Material 3 Sliders whose track SystemUI draws through
 * SliderDefaults.Track-mnvyFg4(state, cornerDp, modifier, enabled, colors,
 * stopIndicator, tick, gap, insideCorner, composer, changed, default): two
 * rounded paths, the active part in the primary colour and the rest in a
 * surface tone, on a canvas the size of the track. One before-hook there,
 * as for the tiles:
 *
 *   modifier  gets a drawBehind that draws a region at the track's bounds
 *             with the track's outer corner radius;
 *   colors    comes back with the track colours' alpha cut, the active part
 *             to app.accent and the rest to app.fill (the thumb and ticks
 *             keep theirs).
 *
 * The volume dialog and the brightness dialog draw their tracks through the
 * same method in windows of their own; only a track in the notification
 * shade's window gets glass. Compose restarts Track with the arguments its
 * body received, ours, so a modifier or colours this hook made are
 * recognised and not wrapped or faded twice.
 */
final class Sliders {
    private static final String DEFAULTS = "androidx.compose.material3.SliderDefaults";
    private static final String COLORS = "androidx.compose.material3.SliderColors";
    private static final String STATE = "androidx.compose.material3.SliderState";
    /** WindowManager.LayoutParams.TYPE_NOTIFICATION_SHADE (hidden). */
    private static final int TYPE_NOTIFICATION_SHADE = 2040;
    /** SliderColors fields in constructor order; the track colours are 1, 3, 6 and 8. */
    private static final String[] COLOR_FIELDS = {
        "thumbColor", "activeTrackColor", "activeTickColor", "inactiveTrackColor", "inactiveTickColor",
        "disabledThumbColor", "disabledActiveTrackColor", "disabledActiveTickColor",
        "disabledInactiveTrackColor", "disabledInactiveTickColor",
    };

    private static boolean installed;
    private static ComposeBridge compose;
    private static Constructor<?> colorsCtor;
    private static final Field[] colorFields = new Field[COLOR_FIELDS.length];
    private static final WeakHashMap<Object, Slider> sliders = new WeakHashMap<>();
    private static Object gateState;
    private static int generation;

    private Sliders() {}

    static synchronized void install(ClassLoader cl) {
        if (installed) return;
        installed = true;
        Method target = null;
        try {
            for (Method m : Class.forName(DEFAULTS, false, cl).getDeclaredMethods()) {
                if (m.getName().equals("Track-mnvyFg4")) target = m;
            }
            if (target == null) throw new NoSuchMethodException(DEFAULTS + ".Track-mnvyFg4");
            Class<?> colors = Class.forName(COLORS, false, cl);
            Class<?>[] longs = new Class<?>[COLOR_FIELDS.length];
            java.util.Arrays.fill(longs, long.class);
            colorsCtor = colors.getConstructor(longs);
            for (int i = 0; i < COLOR_FIELDS.length; ++i) {
                colorFields[i] = colors.getDeclaredField(COLOR_FIELDS[i]);
                colorFields[i].setAccessible(true);
            }
        } catch (Throwable t) {
            L.w("no Material 3 slider track of the expected shape (" + t + "); sliders left alone");
            return;
        }
        Class<?>[] p = target.getParameterTypes();
        final int state = index(p, STATE);
        final int modifier = index(p, "androidx.compose.ui.Modifier");
        final int colors = index(p, COLORS);
        final int composer = index(p, "androidx.compose.runtime.Composer");
        final int corner = state >= 0 && state + 1 < p.length && p[state + 1] == float.class ? state + 1 : -1;
        if (state < 0 || modifier < 0 || colors < 0 || composer < 0) {
            L.w("SliderDefaults.Track has an unexpected signature " + target + "; sliders left alone");
            return;
        }
        try {
            compose = new ComposeBridge(cl);
            gateState = compose.newIntState();
        } catch (Throwable t) {
            L.e("SystemUI's Compose does not have what the sliders need; sliders left alone", t);
            return;
        }
        Gate.onChange(Sliders::gateChanged);
        XposedBridge.hookMethod(target, new XC_MethodHook() {
            @Override
            protected void beforeHookedMethod(MethodHookParam param) {
                try {
                    Region.reap();
                    Object st = param.args[state];
                    if (st == null) return;
                    Slider s = sliders.get(st);
                    if (s == null) {
                        View v = compose.localView(param.args[composer]);
                        if (v == null) return;
                        s = new Slider(v);
                        sliders.put(st, s);
                    }
                    // Read in the caller's composition, so a switch recomposes it.
                    compose.read(gateState);
                    int shade = s.shade();
                    if (shade == 0) return; // a dialog's slider
                    if (corner >= 0) s.cornerDp = (float) param.args[corner];
                    // Not attached yet, the window is unknown: stock colours until
                    // the first draw finds the shade and recomposes (Slider.draw).
                    Object c = param.args[colors];
                    if (c != null && shade == 1) param.args[colors] = s.colors(c);
                    Object m = param.args[modifier];
                    if (m != null) param.args[modifier] = s.modifier(m);
                } catch (Throwable e) {
                    L.once("slider-hook", "slider hook failed; this slider stays stock", e);
                }
            }
        });
        L.i("Quick Settings sliders hooked: " + target.getDeclaringClass().getSimpleName() + "."
                + target.getName() + (corner < 0 ? " (no corner argument)" : ""));
    }

    /** Main thread: recompose and redraw every slider with the new settings. */
    private static void gateChanged() {
        try {
            if (!Gate.sliders()) {
                for (Slider s : sliders.values().toArray(new Slider[0])) s.hideAll();
            }
            compose.write(gateState, ++generation);
        } catch (Throwable e) {
            L.once("slider-redraw", "cannot redraw the sliders after a switch", e);
        }
    }

    private static int index(Class<?>[] types, String name) {
        for (int i = 0; i < types.length; ++i) {
            if (types[i].getName().equals(name)) return i;
        }
        return -1;
    }

    /** Per-slider state, keyed weakly by its SliderState. */
    private static final class Slider {
        final WeakReference<View> view;
        final Object drawProxy;
        /** One region per drawn track: crDroid can show the same slider in QQS and in QS. */
        final WeakHashMap<Object, Region> regions = new WeakHashMap<>();
        Region fallback;
        volatile float cornerDp = 12f;
        /** 1 in the shade's window, 0 elsewhere, -1 not known yet (not attached). */
        private int shade = -1;
        private Object fromColors, madeColors, madeModifier;
        private String madeFor;
        private boolean recomposing;

        Slider(View view) {
            this.view = new WeakReference<>(view);
            drawProxy = compose.function1(this::draw, "StockGlass.sliderGlass");
        }

        int shade() {
            if (shade >= 0) return shade;
            View v = view.get();
            if (v == null || !v.isAttachedToWindow()) return -1;
            ViewGroup.LayoutParams lp = v.getRootView().getLayoutParams();
            shade = lp instanceof WindowManager.LayoutParams
                    && ((WindowManager.LayoutParams) lp).type == TYPE_NOTIFICATION_SHADE ? 1 : 0;
            return shade;
        }

        /** The track colours made translucent; recognises its own, so it never fades twice. */
        Object colors(Object c) throws ReflectiveOperationException {
            if (c == madeColors) c = fromColors;
            String key = Gate.sliders() + " " + Gate.fill() + " " + Gate.accent();
            if (c.equals(fromColors) && key.equals(madeFor) && madeColors != null) return madeColors;
            fromColors = c;
            madeFor = key;
            if (!Gate.sliders()) {
                madeColors = c;
                return c;
            }
            Object[] v = new Object[colorFields.length];
            for (int i = 0; i < v.length; ++i) {
                long col = colorFields[i].getLong(c);
                float keep = i == 1 || i == 6 ? Gate.accent() : i == 3 || i == 8 ? Gate.fill() : 1f;
                v[i] = keep == 1f ? col : ComposeBridge.withAlpha(col, ComposeBridge.alpha(col) * keep);
            }
            madeColors = colorsCtor.newInstance(v);
            return madeColors;
        }

        /** {@code m.drawBehind(glass)}, unless {@code m} is already ours. */
        Object modifier(Object m) throws ReflectiveOperationException {
            if (m == madeModifier) return m;
            madeModifier = compose.drawBehind(m, drawProxy);
            return madeModifier;
        }

        void hideAll() {
            for (Region r : regions.values().toArray(new Region[0])) r.hide();
            if (fallback != null) fallback.hide();
        }

        private Region regionFor(Object node) {
            if (node == null) {
                if (fallback == null) fallback = new Region();
                return fallback;
            }
            Region r = regions.get(node);
            if (r == null) {
                r = new Region();
                r.ownedBy(node);
                final WeakReference<Object> n = new WeakReference<>(node);
                // QS fades the slider in with its content as it expands.
                r.fadeWith(h -> compose.layerAlpha(n.get()));
                regions.put(node, r);
            }
            return r;
        }

        /** drawBehind body: the track's region at its current bounds. */
        void draw(Object drawScope) {
            Region r = null;
            try {
                Region.reap();
                compose.read(gateState);
                Object node = compose.drawNode(drawScope);
                r = regionFor(node);
                View v = view.get();
                if (!Gate.sliders() || v == null || shade() != 1) {
                    r.hide();
                    return;
                }
                if (fromColors == null && !recomposing) {
                    // Composed before it was attached: give it its colours now.
                    recomposing = true;
                    v.post(Sliders::gateChanged);
                }
                Canvas c = compose.canvas(drawScope);
                if (c == null) return;
                long size = compose.size(drawScope);
                float w = ComposeBridge.width(size), h = ComposeBridge.height(size);
                float radius = Math.min(cornerDp * v.getResources().getDisplayMetrics().density, h / 2f);
                r.draw(v, c, 0, 0, Math.round(w), Math.round(h), radius, radius, radius, radius,
                        Gate.radius(), Gate.punch(false));
            } catch (Throwable e) {
                L.once("slider-draw", "cannot draw a slider's glass region", e);
                if (r != null) r.hide();
            }
        }
    }
}
