package dev.stockglass.companion;

import android.graphics.Canvas;
import android.graphics.Path;
import android.graphics.Rect;
import android.graphics.drawable.Drawable;
import android.view.View;
import android.view.ViewParent;
import de.robv.android.xposed.XC_MethodHook;
import de.robv.android.xposed.XposedBridge;
import java.lang.reflect.Field;
import java.lang.reflect.Member;
import java.lang.reflect.Method;
import java.util.WeakHashMap;

/**
 * ColorOS 17's launcher frost, everywhere at once: folder plates, the app
 * drawer's category cards and pills, popups and cards. OplusLauncher draws
 * each of them with a BlurTransitionDrawable from its blur controller, either
 * a RenderEffectDrawable (a RenderNode with a backdrop RenderEffect: the blur
 * is done in the launcher's own process) or a LayerBlurDrawable. Each knows
 * its corner radius (mRadius) and is drawn by the view it decorates, in that
 * view's canvas at its own bounds.
 *
 * A before-hook on each draw puts a region at the drawable's bounds, with
 * its radius and its current alpha (mWholeAlpha, which ColorOS animates as
 * folders open and the drawer slides), into the same canvas, so SurfaceFlinger
 * draws glass exactly where the frost was; then the frost itself is drawn at
 * app.fill of its strength over it, through the original method inside a
 * layer, and the hooked call returns. With the surface's switch off the
 * original runs untouched.
 *
 * The switch follows the view that draws: folders (app.folders), the drawer
 * (app.drawer), else app.frost. The dock's background is left to
 * LauncherDock, which draws its own slab there.
 */
final class OplusFrost {
    private static final String PKG = "com.android.launcher3.uioverrides.states.blurdrawable.";
    private static boolean installed;
    private static final WeakHashMap<Drawable, Region> regions = new WeakHashMap<>();
    private static final WeakHashMap<Drawable, View> hosts = new WeakHashMap<>();
    private static Class<?> hotseat;
    private static Method wholeAlpha;
    private static final Rect rect = new Rect();
    private static final Path clip = new Path();
    /** Set while the original runs inside our layer, so it is not hooked again. */
    private static boolean inOriginal;

    private OplusFrost() {}

    static boolean installed() {
        return installed;
    }

    /** Whether {@code d}'s own draw is hooked here (a folder plate's frost). */
    static boolean handles(Drawable d) {
        return installed && d != null && d.getClass().getName().startsWith(PKG);
    }

    static synchronized void install(ClassLoader cl) {
        if (installed) return;
        int hooked = 0;
        try {
            Class<?> base = Class.forName(PKG + "BlurTransitionDrawable", false, cl);
            wholeAlpha = base.getDeclaredMethod("getMWholeAlpha");
            wholeAlpha.setAccessible(true);
            try {
                hotseat = Class.forName("com.android.launcher3.Hotseat", false, cl);
            } catch (ClassNotFoundException e) {
                hotseat = null;
            }
            hooked += hook(cl, "RenderEffectDrawable");
            hooked += hook(cl, "LayerBlurDrawable");
        } catch (Throwable t) {
            return; // not OplusLauncher
        }
        if (hooked == 0) return;
        installed = true;
        Gate.onChange(OplusFrost::gateChanged);
        L.i("launcher frost hooked: " + hooked + " BlurTransitionDrawable kinds");
    }

    private static int hook(ClassLoader cl, String name) {
        final Method draw;
        final Field radius, view;
        try {
            Class<?> c = Class.forName(PKG + name, false, cl);
            draw = c.getDeclaredMethod("draw", Canvas.class);
            radius = field(c, "mRadius");
            Field v;
            try {
                v = field(c, "view");
            } catch (NoSuchFieldException e) {
                v = null;
            }
            view = v;
        } catch (Throwable t) {
            L.once("frost-" + name, name + " is not the expected shape; that frost stays stock", t);
            return 0;
        }
        XposedBridge.hookMethod(draw, new XC_MethodHook() {
            @Override
            protected void beforeHookedMethod(MethodHookParam param) {
                if (inOriginal) return;
                try {
                    Region.reap();
                    Drawable d = (Drawable) param.thisObject;
                    if (glass(d, (Canvas) param.args[0], radius, view, param.method)) param.setResult(null);
                } catch (Throwable e) {
                    L.once("frost-draw", "frost hook failed; the launcher's frost stays stock", e);
                }
            }
        });
        return 1;
    }

    private static void gateChanged() {
        for (java.util.Map.Entry<Drawable, Region> e : regions.entrySet().toArray(new java.util.Map.Entry[0])) {
            View h = hosts.get(e.getKey());
            if (h == null || !on(h)) e.getValue().hide();
            if (h != null) h.invalidate();
        }
    }

    /** Draws the frost as glass; false leaves it to the original. */
    private static boolean glass(Drawable d, Canvas canvas, Field radius, Field viewField, Member original)
            throws Exception {
        View host = viewField != null ? (View) viewField.get(d) : null;
        if (host == null && d.getCallback() instanceof View) host = (View) d.getCallback();
        Region region = regions.get(d);
        if (host == null || inDock(host)) {
            if (region != null) region.hide();
            return false;
        }
        if (region == null) {
            region = new Region();
            region.ownedBy(d);
            if (!host.getClass().getName().contains(".allapps.")) region.fadeWith(Drawer.INSTANCE);
            regions.put(d, region);
        }
        hosts.put(d, host);
        rect.set(d.getBounds());
        if (!on(host) || rect.isEmpty() || !canvas.isHardwareAccelerated()) {
            region.hide();
            return false;
        }
        float r = radiusOf(d, radius);
        r = Math.max(0f, Math.min(r, Math.min(rect.width(), rect.height()) / 2f));
        float alpha = Math.max(0f, Math.min(1f, (float) wholeAlpha.invoke(d)));
        region.draw(host, canvas, rect.left, rect.top, rect.right, rect.bottom, r, r, r, r,
                Gate.radius(), false, alpha);
        int keep = Math.round(255 * Gate.fill());
        if (keep > 0) {
            int save = canvas.saveLayerAlpha(rect.left, rect.top, rect.right, rect.bottom, keep);
            clip.reset();
            clip.addRoundRect(rect.left, rect.top, rect.right, rect.bottom, r, r, Path.Direction.CW);
            canvas.clipPath(clip);
            inOriginal = true;
            try {
                XposedBridge.invokeOriginalMethod(original, d, new Object[] {canvas});
            } finally {
                inOriginal = false;
                canvas.restoreToCount(save);
            }
        }
        return true;
    }

    private static boolean on(View host) {
        String n = host.getClass().getName();
        if (n.contains(".folder.")) return Gate.folders();
        if (n.contains(".allapps.")) return Gate.drawer();
        return Gate.frost();
    }

    /** The dock and its icon container: LauncherDock's. */
    private static boolean inDock(View v) {
        if (hotseat == null) return false;
        if (hotseat.isInstance(v)) return true;
        ViewParent p = v.getParent();
        return p != null && hotseat.isInstance(p);
    }

    private static float radiusOf(Drawable d, Field radius) throws IllegalAccessException {
        Object v = radius.get(d);
        if (v instanceof Float) return (Float) v;
        if (v == null) return 0f;
        try {
            // RenderEffectDrawable keeps it in a DirtyValue<Float>: peek() reads it
            // without clearing the dirty flag the drawable acts on.
            Object f = v.getClass().getMethod("peek").invoke(v);
            return f instanceof Float ? (Float) f : 0f;
        } catch (ReflectiveOperationException e) {
            return 0f;
        }
    }

    private static Field field(Class<?> c, String name) throws NoSuchFieldException {
        Field f = c.getDeclaredField(name);
        f.setAccessible(true);
        return f;
    }
}
