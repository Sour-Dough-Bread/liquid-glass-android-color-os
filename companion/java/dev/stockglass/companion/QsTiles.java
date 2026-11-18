package dev.stockglass.companion;

import android.graphics.Canvas;
import android.graphics.Rect;
import android.view.View;
import de.robv.android.xposed.XC_MethodHook;
import de.robv.android.xposed.XposedBridge;
import java.lang.ref.WeakReference;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.WeakHashMap;

/**
 * Quick Settings tiles in SystemUI (Android 16 QPR+ / 17, Compose QS).
 *
 * There is no tile View any more: the tile is the composable
 * {@code TileKt.Tile}, and its background is painted by
 * {@code TileKt.TileExpandable(expandable, color: () -> Color, shape, ...,
 * modifier, ..., composer, changed)}, which hands {@code color} to the
 * Expandable's animatedBackground (read at draw time) and lays out inside
 * {@code modifier}. One before-hook on that static method does both jobs:
 *
 *   color     is wrapped, so the fill comes back with its alpha cut to
 *             app.fill (neutral) or app.accent (coloured, i.e. an active tile);
 *   modifier  gets a drawBehind that draws this tile's BackgroundBlurDrawable
 *             at the tile's bounds with the shape's corner radii, which makes
 *             the tile a blur region of the shade window.
 *
 * A tile that is on also asks the engine for glass tinted in its own colour
 * (Region.tint), at app.tint, and its fill keeps app.tilecolor of its alpha
 * over that glass: both partly, so the glass still shows through. How far a
 * tile is "on" follows its colour's chroma, so the tint and the fill fade in
 * and out with SystemUI's own colour animation.
 *
 * The tile's shape is read again on every draw: A17 animates a tile's
 * corners (round when off, squarer when on) inside one remembered
 * RoundedCornerShape whose corner size reads the animation when the outline
 * is made, so the same shape at the same size changes radii. Reading it in
 * this draw also has Compose redraw the region as the corners animate.
 *
 * While the shade opens or closes, the notification stack slides over the
 * tiles and QS leaves that rounded rect undrawn (setFancyClipping; the QS
 * root view clips it out in dispatchDraw). A region knows nothing of that
 * clip, so a tile the stack covers has its glass faded out (Clip).
 *
 * The drawBehind sits outside the tile's squish and clip, so it is drawn with
 * the size the squish layout reports, before the tile's own background.
 * Parameters are matched by type, not position: crDroid's TileExpandable has
 * an extra boolean that the AVD's does not.
 */
final class QsTiles {
    private static final String TILE_KT = "com.android.systemui.qs.panels.ui.compose.infinitegrid.TileKt";
    private static final String QS_FRAGMENT = "com.android.systemui.qs.composefragment.QSFragmentCompose";
    private static final float ACCENT_CHROMA = 0.15f;
    /** Chroma of the surface tones: a fill at or under it is off, untinted. */
    private static final float NEUTRAL_CHROMA = 0.05f;
    private static boolean installed;
    private static ComposeBridge compose;
    private static final WeakHashMap<Object, Tile> tiles = new WeakHashMap<>();
    /** Read by every tile's colour and glass; written when the Gate changes. */
    private static Object gateState;
    private static int generation;

    private QsTiles() {}

    static synchronized void install(ClassLoader cl) {
        if (installed) return;
        installed = true;
        Method target = null;
        try {
            for (Method m : Class.forName(TILE_KT, false, cl).getDeclaredMethods()) {
                if (m.getName().equals("TileExpandable")) {
                    target = m;
                    break;
                }
            }
        } catch (ClassNotFoundException e) {
            L.w("no Compose QS (" + TILE_KT + "); tiles left alone");
            return;
        }
        if (target == null) {
            L.w("TileKt.TileExpandable not found; tiles left alone");
            return;
        }
        Class<?>[] p = target.getParameterTypes();
        final int key = index(p, "com.android.systemui.animation.Expandable");
        final int color = index(p, "kotlin.jvm.functions.Function0");
        final int shape = index(p, "androidx.compose.ui.graphics.Shape");
        final int modifier = index(p, "androidx.compose.ui.Modifier");
        final int composer = index(p, "androidx.compose.runtime.Composer");
        if (key < 0 || color < 0 || shape < 0 || modifier < 0 || composer < 0) {
            L.w("TileExpandable has an unexpected signature " + target + "; tiles left alone");
            return;
        }
        try {
            compose = new ComposeBridge(cl);
            gateState = compose.newIntState();
        } catch (Throwable t) {
            L.e("SystemUI's Compose does not have what the tiles need; tiles left alone", t);
            return;
        }
        Gate.onChange(QsTiles::gateChanged);
        Clip.install(cl);
        XposedBridge.hookMethod(target, new XC_MethodHook() {
            @Override
            protected void beforeHookedMethod(MethodHookParam param) {
                try {
                    Region.reap();
                    Object k = param.args[key];
                    if (k == null) return;
                    Tile t = tiles.get(k);
                    if (t == null) {
                        View v = compose.localView(param.args[composer]);
                        if (v == null) return;
                        t = new Tile(v);
                        t.region.ownedBy(k);
                        tiles.put(k, t);
                    }
                    t.shape = param.args[shape];
                    if (param.args[color] != null) {
                        t.color = param.args[color];
                        param.args[color] = t.colorProxy;
                    }
                    param.args[modifier] = compose.drawBehind(param.args[modifier], t.drawProxy);
                } catch (Throwable e) {
                    L.once("tile-hook", "tile hook failed; this tile stays stock", e);
                }
            }
        });
        L.i("Quick Settings tiles hooked: " + target.getDeclaringClass().getSimpleName() + "."
                + target.getName() + " (" + p.length + " parameters)");
    }

    /**
     * Main thread. Every tile that has drawn read gateState, so writing it
     * makes Compose redraw them all, with the new fill and with or without
     * glass. Turning off also hides every region at once, in case a tile is
     * never redrawn (or this Compose had no int state to write).
     */
    private static void gateChanged() {
        try {
            if (!Gate.qs()) {
                for (Tile t : tiles.values().toArray(new Tile[0])) t.region.hide();
            }
            compose.write(gateState, ++generation);
        } catch (Throwable e) {
            L.once("tile-redraw", "cannot redraw the tiles after a switch", e);
        }
    }

    /**
     * The notification stack's rounded rect that QS clips out of its drawing
     * while the shade opens and closes. QSFragmentCompose.setFancyClipping
     * (left inset, top, right inset, bottom, radius, visible, full width)
     * passes it in the QS root view's coordinates, and the root's
     * dispatchDraw undoes its own translationY before clipping. Main thread.
     */
    private static final class Clip {
        private static WeakReference<View> root = new WeakReference<>(null);
        private static boolean on;
        private static int leftInset, top, rightInset, bottom;
        private static Method getView;
        private static final int[] xy = new int[2];

        static void install(ClassLoader cl) {
            Method m;
            try {
                m = Class.forName(QS_FRAGMENT, false, cl).getMethod("setFancyClipping", int.class, int.class,
                        int.class, int.class, int.class, boolean.class, boolean.class);
            } catch (Throwable t) {
                L.w("no QSFragmentCompose.setFancyClipping (" + t.getClass().getSimpleName()
                        + "); tile glass stays under the notifications as the shade opens and closes");
                return;
            }
            XposedBridge.hookMethod(m, new XC_MethodHook() {
                @Override
                protected void afterHookedMethod(MethodHookParam param) {
                    try {
                        Object[] a = param.args;
                        boolean fullWidth = (Boolean) a[6];
                        leftInset = fullWidth ? 0 : (Integer) a[0];
                        top = (Integer) a[1];
                        rightInset = fullWidth ? 0 : (Integer) a[2];
                        bottom = (Integer) a[3];
                        on = (Boolean) a[5];
                        if (getView == null) getView = param.thisObject.getClass().getMethod("getView");
                        Object v = getView.invoke(param.thisObject);
                        if (v instanceof View && root.get() != v) root = new WeakReference<>((View) v);
                    } catch (Throwable e) {
                        on = false;
                        L.once("qs-clip", "cannot read where the notifications clip QS; tile glass stays"
                                + " under them as the shade opens and closes", e);
                    }
                }
            });
            L.i("QS clip hooked: " + m.getDeclaringClass().getSimpleName() + ".setFancyClipping");
        }

        /** Whether the clipped-out rect covers {@code r} (window coordinates) by more than a pixel. */
        static boolean covers(Rect r) {
            View v = root.get();
            if (!on || bottom <= top || r.isEmpty() || v == null || !v.isAttachedToWindow()) return false;
            v.getLocationInWindow(xy);
            float y0 = xy[1] - v.getTranslationY();
            float cTop = y0 + top, cBottom = y0 + bottom;
            float cLeft = xy[0] - leftInset, cRight = xy[0] + v.getWidth() + rightInset;
            return r.bottom > cTop + 1 && r.top < cBottom - 1 && r.right > cLeft && r.left < cRight;
        }
    }

    /** 0 for a neutral fill, 1 for an accent, in between while one animates into the other. */
    private static float onness(float chroma) {
        return Math.max(0f, Math.min(1f, (chroma - NEUTRAL_CHROMA) / (ACCENT_CHROMA - NEUTRAL_CHROMA)));
    }

    private static int index(Class<?>[] types, String name) {
        for (int i = 0; i < types.length; ++i) {
            if (types[i].getName().equals(name)) return i;
        }
        return -1;
    }

    /** Per-tile state, keyed weakly by the tile's remembered Expandable. */
    private static final class Tile {
        final Region region = new Region();
        final WeakReference<View> view;
        final Object colorProxy;
        final Object drawProxy;
        volatile Object color;
        volatile Object shape;
        volatile float fillAlpha = 1f;
        private final Rect where = new Rect();
        /** The drawBehind's Modifier.Node: where the layers this tile is drawn in start. */
        private WeakReference<Object> node;

        Tile(View view) {
            this.view = new WeakReference<>(view);
            // Stable identities, so Compose sees the same arguments on every
            // recomposition and skips exactly as it would without the hook.
            colorProxy = compose.function0(this::fill, "StockGlass.tileColor");
            drawProxy = compose.function1(this::draw, "StockGlass.tileGlass");
            // QS fades tiles in and out with layer alpha (the rows that only the
            // full panel shows, as it expands and collapses), and a layer's alpha
            // is not something hwui gives the region: follow it every frame.
            // Under the notification stack, which QS does not draw (Clip), the
            // tile is gone too.
            region.fadeWith(h -> {
                WeakReference<Object> n = node;
                float a = compose.layerAlpha(n == null ? null : n.get());
                return a > 0f && Clip.covers(region.windowRect(where)) ? 0f : a;
            });
        }

        /** The tile's colour lambda, with the fill made translucent in glass mode. */
        Object fill() throws Throwable {
            Object c;
            try {
                c = compose.invoke0.invoke(color);
            } catch (InvocationTargetException e) {
                throw e.getCause(); // whatever the tile's own lambda threw, unchanged
            }
            try {
                compose.read(gateState);
                long v = compose.colorValue(c);
                float a = ComposeBridge.alpha(v);
                fillAlpha = a;
                if (!Gate.qs() || a == 0f) return c;
                // Tinted, a tile that is on has its colour in the glass as well
                // (draw()), and its own fill keeps app.tilecolor over it.
                float keep = Gate.tint() > 0f
                        ? Gate.fill() + (Gate.tileColor() - Gate.fill()) * onness(compose.chroma(v))
                        : compose.chroma(v) > ACCENT_CHROMA ? Gate.accent() : Gate.fill();
                return compose.box(ComposeBridge.withAlpha(v, a * keep));
            } catch (Throwable e) {
                L.once("tile-color", "cannot read a tile colour; drawing it unchanged", e);
                return c;
            }
        }

        /** drawBehind body: this tile's blur region at its current bounds. */
        void draw(Object drawScope) {
            try {
                Region.reap();
                compose.read(gateState);
                // A tile with no fill of its own (crDroid's QS panel styles draw
                // tiles that way) has nothing for glass to show through.
                if (!Gate.qs() || fillAlpha == 0f) {
                    region.hide();
                    return;
                }
                View v = view.get();
                Canvas c = v == null ? null : compose.canvas(drawScope);
                if (c == null) return;
                Object n = compose.drawNode(drawScope);
                if (n != null && (node == null || node.get() != n)) node = new WeakReference<>(n);
                long size = compose.size(drawScope);
                Object s = shape;
                // Every draw, not cached by shape and size: see the class comment.
                float[] radii = s == null ? new float[4] : compose.radii(s, size, drawScope);
                tint();
                region.draw(v, c, 0, 0, Math.round(ComposeBridge.width(size)),
                        Math.round(ComposeBridge.height(size)),
                        radii[0], radii[1], radii[2], radii[3], Gate.radius(), Gate.punch(false));
            } catch (Throwable e) {
                L.once("tile-draw", "cannot draw a tile's glass region", e);
                region.hide();
            }
        }

        /**
         * The region's tint from the tile's colour as it is now. This draw
         * runs before the tile's own background reads it, so it reads the
         * tile's lambda itself; reading it here also redraws the region while
         * the colour animates.
         */
        private void tint() {
            float strength = Gate.tint();
            Object fn = color;
            if (strength <= 0f || fn == null) {
                region.tint(0, 0f);
                return;
            }
            try {
                long v = compose.colorValue(compose.invoke0.invoke(fn));
                region.tint(compose.rgb(v), strength * onness(compose.chroma(v)));
            } catch (Throwable e) {
                L.once("tile-tint", "cannot read a tile colour for its tint; glass untinted", e);
                region.tint(0, 0f);
            }
        }
    }
}
