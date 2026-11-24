package dev.stockglass.companion;

import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.drawable.Drawable;
import android.view.View;
import de.robv.android.xposed.XC_MethodHook;
import de.robv.android.xposed.XposedBridge;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.Map;
import java.util.WeakHashMap;

/**
 * Themed icons on ColorOS 17's OplusLauncher: glass under the glyph, as
 * LauncherIcons does on Launcher3 (which has ThemedIconDelegate instead).
 *
 * ThemedIconDrawable.drawInternal(canvas, rect) draws the icon's plate (a
 * shape bitmap through mBgPaint, tinted with the theme's background colour)
 * and then the monochrome glyph through mMonoPaint; OplusThemedIconDrawable
 * wraps that in a saveLayer. A before-hook on the Oplus override, outside
 * its layer, puts a region at the icon's rect and draws the plate at
 * app.fill of the icon's alpha; the after-hook puts the paint back. Only
 * themed icons have a plate to thin: full-colour icons are left alone.
 *
 * Home screen and dock follow app.icons, the drawer app.drawer.
 */
final class OplusIcons {
    /** ColorOS's icon squircle, as a rounded rect: corner over size. */
    private static final float CORNER = 0.30f;
    private static boolean installed;
    private static Field bgPaint;
    private static final WeakHashMap<Drawable, Region> regions = new WeakHashMap<>();
    private static final WeakHashMap<Drawable, View> hosts = new WeakHashMap<>();
    private static final WeakHashMap<Paint, Integer> thinned = new WeakHashMap<>();

    private OplusIcons() {}

    /** Returns whether this launcher's themed icons are ColorOS's (hooked here). */
    static synchronized boolean install(ClassLoader cl) {
        if (installed) return bgPaint != null;
        installed = true;
        Method draw;
        try {
            Class<?> base = Class.forName("com.android.launcher3.icons.ThemedIconDrawable", false, cl);
            Class<?> oplus = Class.forName("com.android.launcher3.icons.OplusThemedIconDrawable", false, cl);
            draw = oplus.getDeclaredMethod("drawInternal", Canvas.class, Rect.class);
            bgPaint = base.getDeclaredField("mBgPaint");
            bgPaint.setAccessible(true);
        } catch (Throwable t) {
            bgPaint = null;
            return false; // not OplusLauncher
        }
        XposedBridge.hookMethod(draw, new XC_MethodHook() {
            @Override
            protected void beforeHookedMethod(MethodHookParam param) {
                try {
                    Region.reap();
                    before((Drawable) param.thisObject, (Canvas) param.args[0], (Rect) param.args[1]);
                } catch (Throwable e) {
                    L.once("icon", "themed icon hook failed; icons stay stock", e);
                }
            }

            @Override
            protected void afterHookedMethod(MethodHookParam param) {
                try {
                    Paint p = (Paint) bgPaint.get(param.thisObject);
                    Integer a = thinned.remove(p);
                    if (a != null) p.setAlpha(a);
                } catch (Throwable e) {
                    L.once("icon-after", "cannot restore a themed icon's plate", e);
                }
            }
        });
        Gate.onChange(OplusIcons::gateChanged);
        probe(cl);
        baked(cl);
        L.i("launcher themed icons hooked: OplusThemedIconDrawable.drawInternal");
        return true;
    }

    // ---------------------------------------------------------------- baked
    //
    // ColorOS's own themed icons (Settings, Themed icons) are not
    // ThemedIconDrawables: its theme engine bakes plate and glyph into one
    // bitmap, drawn by com.oplus.icons.OplusFastBitmapDrawable through
    // FastBitmapDrawable.mPaint like any full-colour icon. The plate is the
    // theme's background colour (ThemedIconDrawable.getColors: background,
    // foreground). A bitmap whose border ring is that colour is a themed
    // icon; for it, a colour filter keeps RGB and maps luminance to alpha so
    // that the plate comes out at app.fill and the glyph opaque, and a region
    // goes under the icon. Checked once per bitmap.

    private static Field fastPaint, fastBitmap;
    /** The launcher's context, from the first icon with a host view. */
    private static android.content.Context context;
    private static Class<?> oplusFast;
    private static Method colors;
    private static final WeakHashMap<android.graphics.Bitmap, float[]> plates = new WeakHashMap<>();
    private static final float[] NOT_THEMED = new float[0];
    private static final WeakHashMap<Paint, Boolean> swapped = new WeakHashMap<>();
    private static android.graphics.ColorMatrixColorFilter filter;
    private static float filterFill = -1, filterPlate = -1, filterGlyph = -1;

    private static void baked(ClassLoader cl) {
        Method draw;
        try {
            Class<?> fast = Class.forName("com.android.launcher3.icons.FastBitmapDrawable", false, cl);
            Class<?> oplus = Class.forName("com.oplus.icons.OplusFastBitmapDrawable", false, cl);
            draw = oplus.getDeclaredMethod("drawInternal", Canvas.class, Rect.class);
            fastPaint = fast.getDeclaredField("mPaint");
            fastPaint.setAccessible(true);
            fastBitmap = fast.getDeclaredField("mBitmap");
            fastBitmap.setAccessible(true);
            colors = Class.forName("com.android.launcher3.icons.ThemedIconDrawable", false, cl)
                    .getMethod("getColors", android.content.Context.class);
            oplusFast = oplus;
            preview(fast.getDeclaredMethod("drawInternal", Canvas.class, Rect.class));
        } catch (Throwable t) {
            L.once("icon-baked", "ColorOS's baked themed icons are not the expected shape; they stay stock", t);
            return;
        }
        XposedBridge.hookMethod(draw, new XC_MethodHook() {
            @Override
            protected void beforeHookedMethod(MethodHookParam param) {
                try {
                    Region.reap();
                    bakedBefore((Drawable) param.thisObject, (Canvas) param.args[0], (Rect) param.args[1]);
                } catch (Throwable e) {
                    L.once("icon-baked-draw", "themed icon hook failed; icons stay stock", e);
                }
            }

            @Override
            protected void afterHookedMethod(MethodHookParam param) {
                try {
                    Paint p = (Paint) fastPaint.get(param.thisObject);
                    if (swapped.remove(p) != null) p.setColorFilter(null);
                } catch (Throwable e) {
                    L.once("icon-baked-after", "cannot restore a themed icon's paint", e);
                }
            }
        });
    }

    /**
     * Folder previews: the big folders draw their icons as plain
     * FastBitmapDrawables with no host view, inside the folder's own glass.
     * No region for those (glass in glass); their plate only gets the same
     * filter, so it is a tint over the folder's glass. app.folders.
     */
    private static void preview(Method base) {
        XposedBridge.hookMethod(base, new XC_MethodHook() {
            @Override
            protected void beforeHookedMethod(MethodHookParam param) {
                Object d = param.thisObject;
                if (oplusFast.isInstance(d) || context == null) return; // the Oplus hook's
                try {
                    if (!(((Drawable) d).getCallback() instanceof View)) previewFilter((Drawable) d);
                } catch (Throwable e) {
                    L.once("icon-preview", "folder preview icon hook failed; they stay stock", e);
                }
            }

            @Override
            protected void afterHookedMethod(MethodHookParam param) {
                if (oplusFast.isInstance(param.thisObject)) return;
                try {
                    Paint p = (Paint) fastPaint.get(param.thisObject);
                    if (swapped.remove(p) != null) p.setColorFilter(null);
                } catch (Throwable e) {
                    L.once("icon-preview-after", "cannot restore a folder preview icon's paint", e);
                }
            }
        });
    }

    /**
     * The launcher's application. Big folders draw their icons once into a
     * cached snapshot, often before any icon with a host view has been drawn,
     * so the context cannot wait for one.
     */
    private static android.content.Context application() {
        try {
            return (android.content.Context) Class.forName("android.app.ActivityThread")
                    .getMethod("currentApplication").invoke(null);
        } catch (Throwable e) {
            return null;
        }
    }

    /** The plate filter alone, for an icon drawn inside a folder's own glass. */
    private static void previewFilter(Drawable d) throws Exception {
        if (Gate.debug()) {
            Object cb = d.getCallback();
            L.once("preview-cb:" + (cb == null ? "null" : cb.getClass().getName()), "icon without a host view, callback "
                    + (cb == null ? "null" : cb.getClass().getName()), null);
        }
        if (!Gate.folders()) return;
        if (context == null) context = application();
        if (context == null) return;
        float[] plate = plateOf(d, context);
        Paint paint = (Paint) fastPaint.get(d);
        if (Gate.debug()) {
            L.once("preview:" + d.getClass().getSimpleName() + plate.length + (paint.getColorFilter() != null),
                    "folder preview icon " + d.getClass().getSimpleName() + ": themed=" + (plate.length > 0)
                    + " filter=" + paint.getColorFilter() + " thread=" + Thread.currentThread().getName(), null);
        }
        if (plate.length == 0 || paint.getColorFilter() != null) return;
        swapped.put(paint, Boolean.TRUE);
        paint.setColorFilter(filterFor(plate[0], plate[1]));
    }

    private static void bakedBefore(Drawable d, Canvas canvas, Rect rect) throws Exception {
        View host = d.getCallback() instanceof View ? (View) d.getCallback() : null;
        if (host == null) {
            previewFilter(d); // a folder preview (callback: FolderPreviewDrawable)
            return;
        }
        Region region = regions.get(d);
        boolean ok = host != null && rect != null && !rect.isEmpty() && canvas.isHardwareAccelerated() && on(host);
        if (host != null) context = host.getContext().getApplicationContext();
        float[] plate = ok ? plateOf(d, host.getContext()) : NOT_THEMED;
        Paint paint = (Paint) fastPaint.get(d);
        // Pressed and disabled icons carry their own filter: leave those stock.
        if (plate.length == 0 || paint.getColorFilter() != null) {
            if (region != null) region.hide();
            return;
        }
        if (region == null) {
            region = new Region();
            region.ownedBy(d);
            if (!inDrawer(host)) region.fadeWith(Drawer.INSTANCE);
            regions.put(d, region);
        }
        hosts.put(d, host);
        float r = Math.min(rect.width(), rect.height()) * CORNER;
        region.draw(host, canvas, rect.left, rect.top, rect.right, rect.bottom, r, r, r, r,
                Gate.radius(), false, d.getAlpha() / 255f);
        swapped.put(paint, Boolean.TRUE);
        paint.setColorFilter(filterFor(plate[0], plate[1]));
    }

    /** {plate luminance, glyph luminance} for a themed bitmap, else NOT_THEMED; cached. */
    private static float[] plateOf(Drawable d, android.content.Context ctx) throws Exception {
        android.graphics.Bitmap b = (android.graphics.Bitmap) fastBitmap.get(d);
        if (b == null || b.isRecycled()) return NOT_THEMED;
        float[] known = plates.get(b);
        if (known != null) return known;
        float[] res = NOT_THEMED;
        try {
            int[] c = (int[]) colors.invoke(null, ctx);
            int bg = c[0], fg = c[1];
            android.graphics.Bitmap s = b.getConfig() == android.graphics.Bitmap.Config.HARDWARE
                    ? b.copy(android.graphics.Bitmap.Config.ARGB_8888, false) : b;
            int w = s.getWidth(), h = s.getHeight(), match = 0, seen = 0;
            // A ring just inside the icon's edge: plate on a themed icon.
            float[][] at = {{0.5f, 0.1f}, {0.5f, 0.9f}, {0.1f, 0.5f}, {0.9f, 0.5f},
                    {0.25f, 0.15f}, {0.75f, 0.15f}, {0.25f, 0.85f}, {0.75f, 0.85f}};
            for (float[] p : at) {
                int px = s.getPixel(Math.round(p[0] * (w - 1)), Math.round(p[1] * (h - 1)));
                if (android.graphics.Color.alpha(px) < 250) continue;
                seen++;
                if (close(px, bg)) match++;
            }
            if (s != b) s.recycle();
            if (seen >= 6 && match >= seen - 1) res = new float[] {luma(bg), luma(fg)};
            if (Gate.debug()) {
                L.once("icon-sample:" + Integer.toHexString(bg), "themed plate colour #"
                        + Integer.toHexString(bg) + ", glyph #" + Integer.toHexString(fg), null);
            }
        } catch (Throwable e) {
            L.once("icon-sample", "cannot read an icon's pixels; it stays stock", e);
        }
        plates.put(b, res);
        return res;
    }

    private static boolean close(int a, int b) {
        return Math.abs(android.graphics.Color.red(a) - android.graphics.Color.red(b)) < 20
                && Math.abs(android.graphics.Color.green(a) - android.graphics.Color.green(b)) < 20
                && Math.abs(android.graphics.Color.blue(a) - android.graphics.Color.blue(b)) < 20;
    }

    private static float luma(int c) {
        return 0.2126f * android.graphics.Color.red(c) + 0.7152f * android.graphics.Color.green(c)
                + 0.0722f * android.graphics.Color.blue(c);
    }

    /**
     * alpha' = A + s * luma - k: the plate's luminance gives app.fill, the
     * glyph's 255 (or more, clamped). Transparent pixels stay transparent.
     */
    private static android.graphics.ColorFilter filterFor(float plate, float glyph) {
        float fill = Gate.fill();
        if (filter != null && fill == filterFill && plate == filterPlate && glyph == filterGlyph) return filter;
        float span = glyph - plate;
        if (Math.abs(span) < 8f) span = span < 0 ? -8f : 8f;
        float s = (1f - fill) * 255f / span;
        float k = 255f + s * plate - fill * 255f;
        float[] m = {
            1, 0, 0, 0, 0,
            0, 1, 0, 0, 0,
            0, 0, 1, 0, 0,
            s * 0.2126f, s * 0.7152f, s * 0.0722f, 1, -k,
        };
        filter = new android.graphics.ColorMatrixColorFilter(m);
        filterFill = fill;
        filterPlate = plate;
        filterGlyph = glyph;
        return filter;
    }

    /** Debug (app.debug): which icon drawables this launcher draws, once per class. */
    private static void probe(ClassLoader cl) {
        try {
            Class<?> fast = Class.forName("com.android.launcher3.icons.FastBitmapDrawable", false, cl);
            Method draw = fast.getDeclaredMethod("draw", Canvas.class);
            XposedBridge.hookMethod(draw, new XC_MethodHook() {
                @Override
                protected void beforeHookedMethod(MethodHookParam param) {
                    if (!Gate.debug()) return;
                    Object d = param.thisObject;
                    Object cb = ((Drawable) d).getCallback();
                    L.once("icon-class:" + d.getClass().getName(), "icon drawable " + d.getClass().getName()
                            + " in " + (cb == null ? "nothing" : cb.getClass().getName()), null);
                }
            });
        } catch (Throwable t) {
            L.once("icon-probe", "cannot probe icon drawables", t);
        }
    }

    private static void gateChanged() {
        for (Map.Entry<Drawable, Region> e : regions.entrySet().toArray(new Map.Entry[0])) {
            View h = hosts.get(e.getKey());
            if (h == null || !on(h)) e.getValue().hide();
            if (h != null) h.invalidate();
        }
    }

    private static void before(Drawable d, Canvas canvas, Rect rect) throws Exception {
        View host = d.getCallback() instanceof View ? (View) d.getCallback() : null;
        Region region = regions.get(d);
        if (host == null || rect == null || rect.isEmpty() || !canvas.isHardwareAccelerated() || !on(host)) {
            if (region != null) region.hide();
            return;
        }
        if (region == null) {
            region = new Region();
            region.ownedBy(d);
            if (!inDrawer(host)) region.fadeWith(Drawer.INSTANCE);
            regions.put(d, region);
        }
        hosts.put(d, host);
        float r = Math.min(rect.width(), rect.height()) * CORNER;
        region.draw(host, canvas, rect.left, rect.top, rect.right, rect.bottom, r, r, r, r,
                Gate.radius(), false, d.getAlpha() / 255f);
        Paint p = (Paint) bgPaint.get(d);
        int a = p.getAlpha();
        thinned.put(p, a);
        p.setAlpha(Math.round(a * Gate.fill()));
    }

    private static boolean on(View host) {
        return inDrawer(host) ? Gate.drawer() : Gate.icons();
    }

    private static boolean inDrawer(View v) {
        for (Object p = v.getParent(); p instanceof View; p = ((View) p).getParent()) {
            if (p.getClass().getName().contains(".allapps.")) return true;
        }
        return false;
    }
}
