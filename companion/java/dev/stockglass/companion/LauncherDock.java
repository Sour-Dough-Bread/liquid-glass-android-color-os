package dev.stockglass.companion;

import android.graphics.Canvas;
import android.graphics.Rect;
import android.graphics.drawable.Drawable;
import android.view.View;
import android.view.ViewGroup;
import de.robv.android.xposed.XC_MethodHook;
import de.robv.android.xposed.XposedBridge;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.WeakHashMap;

/**
 * A glass slab behind the dock's icons on ColorOS 17's OplusLauncher
 * (app.dock).
 *
 * OplusHotseat.onDraw draws an optional dock background itself,
 * mDockerBackgroundDrawable (ColorOS's frost, or a plain fill), translated to
 * the icon container (mShortcutsAndWidgets), with corners of the hotseat's
 * height times BACKGROUND_SMOOTH_CORNER_RADIUS_FACTOR. On a phone that
 * drawable is usually absent or at alpha 0, so the dock has no plate. A
 * before-hook on onDraw puts a region where that background goes, with its
 * radius, into the hotseat's own display list, so the slab follows the
 * hotseat through its translation and alpha animations and fades as the
 * drawer opens. If ColorOS's background is showing, it is drawn at app.fill
 * of its alpha over the glass, as every other plate is.
 */
final class LauncherDock {
    private static final float DEFAULT_FACTOR = 0.44f;
    private static boolean installed;
    private static final WeakHashMap<View, Region> regions = new WeakHashMap<>();
    private static final WeakHashMap<Drawable, Integer> dimmed = new WeakHashMap<>();
    private static Field container, background;
    private static float factor = DEFAULT_FACTOR;
    private static final Rect rect = new Rect();

    private LauncherDock() {}

    static synchronized void install(ClassLoader cl) {
        if (installed) return;
        installed = true;
        Method onDraw;
        try {
            Class<?> hotseat = Class.forName("com.android.launcher3.OplusHotseat", false, cl);
            onDraw = hotseat.getDeclaredMethod("onDraw", Canvas.class);
            container = findField(hotseat, "mShortcutsAndWidgets");
            background = findField(hotseat, "mDockerBackgroundDrawable");
            try {
                Field f = hotseat.getDeclaredField("BACKGROUND_SMOOTH_CORNER_RADIUS_FACTOR");
                f.setAccessible(true);
                factor = f.getFloat(null);
            } catch (Throwable e) {
                factor = DEFAULT_FACTOR;
            }
        } catch (Throwable t) {
            return; // not OplusLauncher: the dock keeps the launcher's own look
        }
        XposedBridge.hookMethod(onDraw, new XC_MethodHook() {
            @Override
            protected void beforeHookedMethod(MethodHookParam param) {
                try {
                    Region.reap();
                    before((ViewGroup) param.thisObject, (Canvas) param.args[0]);
                } catch (Throwable e) {
                    L.once("dock", "dock hook failed; the dock stays stock", e);
                }
            }

            @Override
            protected void afterHookedMethod(MethodHookParam param) {
                try {
                    after((ViewGroup) param.thisObject);
                } catch (Throwable e) {
                    L.once("dock-after", "cannot restore the dock background's alpha", e);
                }
            }
        });
        Gate.onChange(LauncherDock::gateChanged);
        L.i("launcher dock hooked: OplusHotseat.onDraw (radius factor " + factor + ")");
    }

    private static void gateChanged() {
        boolean on = Gate.dock();
        for (java.util.Map.Entry<View, Region> e : regions.entrySet().toArray(new java.util.Map.Entry[0])) {
            if (!on) e.getValue().hide();
            e.getKey().invalidate();
        }
    }

    private static void before(ViewGroup hotseat, Canvas canvas) throws Exception {
        Region region = regions.get(hotseat);
        if (region == null) {
            region = new Region();
            region.ownedBy(hotseat);
            region.fadeWith(Drawer.INSTANCE);
            regions.put(hotseat, region);
        }
        View c = (View) container.get(hotseat);
        if (!Gate.dock() || c == null || c.getWidth() <= 0 || c.getHeight() <= 0
                || !canvas.isHardwareAccelerated()) {
            region.hide();
            return;
        }
        Drawable bg = (Drawable) background.get(hotseat);
        if (bg != null && !bg.getBounds().isEmpty()) {
            rect.set(bg.getBounds());
            rect.offset(c.getLeft(), c.getTop());
        } else {
            rect.set(c.getLeft(), c.getTop(), c.getRight(), c.getBottom());
        }
        if (rect.isEmpty()) {
            region.hide();
            return;
        }
        float r = Math.min(hotseat.getMeasuredHeight() * factor, Math.min(rect.width(), rect.height()) / 2f);
        region.draw(hotseat, canvas, rect.left, rect.top, rect.right, rect.bottom, r, r, r, r,
                Gate.radius(), false);
        if (bg != null && bg.getAlpha() > 0) {
            int was = bg.getAlpha();
            dimmed.put(bg, was);
            quietAlpha(bg, Math.round(was * Gate.fill()));
        }
    }

    /** Puts ColorOS's own background back at the alpha its animations set. */
    private static void after(ViewGroup hotseat) throws Exception {
        Drawable bg = (Drawable) background.get(hotseat);
        if (bg == null) return;
        Integer was = dimmed.remove(bg);
        if (was != null) quietAlpha(bg, was);
    }

    /** setAlpha without the invalidate that would redraw the hotseat every frame. */
    private static void quietAlpha(Drawable d, int alpha) {
        Drawable.Callback cb = d.getCallback();
        d.setCallback(null);
        try {
            d.setAlpha(alpha);
        } finally {
            d.setCallback(cb);
        }
    }

    private static Field findField(Class<?> c, String name) throws NoSuchFieldException {
        for (Class<?> k = c; k != null; k = k.getSuperclass()) {
            try {
                Field f = k.getDeclaredField(name);
                f.setAccessible(true);
                return f;
            } catch (NoSuchFieldException e) {
                // the superclass may declare it
            }
        }
        throw new NoSuchFieldException(name);
    }
}
