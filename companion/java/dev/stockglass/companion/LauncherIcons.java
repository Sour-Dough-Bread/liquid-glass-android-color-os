package dev.stockglass.companion;

import android.graphics.Canvas;
import android.graphics.Matrix;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.drawable.Drawable;
import android.view.View;
import de.robv.android.xposed.XC_MethodHook;
import de.robv.android.xposed.XposedBridge;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.WeakHashMap;

/**
 * Themed app icons on the home screen and dock: what the Pixel launcher calls
 * themed icons, and crDroid's ThemePicker calls the "Minimal" app icon style
 * (it sets the same launcher preference).
 *
 * A themed icon is not a cached bitmap with its background baked in, as a
 * full-colour icon is. ThemedIconDelegate.drawContent(info, shape, canvas,
 * bounds, paint) draws it at draw time: the shape's shadow layer with
 * {@code paint}, then {@code paint} again, clipped to the theme's icon shape,
 * which is the plate, then the tinted monochrome glyph with its own paint. So
 * a before-hook can draw this icon's region at the plate and hand drawContent
 * a translucent copy of {@code paint}: the plate and its shadow fade to
 * app.fill, and the glyph stays as it is.
 *
 * drawContent does not know which view it draws for, and a region needs the
 * view's window. FastBitmapDrawable.draw(Canvas), which calls it, does: its
 * callback is the BubbleTextView. A before-hook there notes the drawable and
 * canvas, and drawContent pairs with it only if both match. Icons whose
 * BubbleTextView displays on the workspace (the home screen and the dock) get
 * glass under app.icons, and those in the app drawer (its grid, its
 * predictions row and its search results) under app.drawer: crDroid themes
 * the drawer's icons when its drawer setting asks for themed icons; the
 * Pixel launcher does not. Folder previews, the open folder, drag views and
 * the picker's preview stay stock.
 *
 * The drawer's grid scrolls under its search box and off its bottom edge,
 * where the RecyclerView clips the icon; a region is not clipped. So a drawer
 * icon's glass fades by how much of its plate is still on screen, and the
 * home screen's fades as the drawer opens over it (Drawer).
 */
final class LauncherIcons {
    private static final String FBD = "com.android.launcher3.icons.FastBitmapDrawable";
    private static final String THEMED = "com.android.launcher3.icons.mono.ThemedIconDelegate";
    private static final String BTV = "com.android.launcher3.BubbleTextView";
    /** BubbleTextView.DISPLAY_WORKSPACE (attrs.xml iconDisplay="workspace"): home screen and dock. */
    private static final int DISPLAY_WORKSPACE = 0;
    /** DISPLAY_ALL_APPS, DISPLAY_PREDICTION_ROW, DISPLAY_SEARCH_RESULT: BubbleTextView.displayIsAppDrawer(). */
    private static final int DISPLAY_ALL_APPS = 1, DISPLAY_PREDICTION_ROW = 8, DISPLAY_SEARCH_RESULT = 9;

    private static boolean installed;
    private static Class<?> themed, btv, predicted;
    private static Field delegate, display, path, pathSize, pinned;
    private static Method iconBounds;
    private static float visibleFactor = 0.92f;
    private static final WeakHashMap<View, Icon> icons = new WeakHashMap<>();
    private static final WeakHashMap<Object, Shape> shapes = new WeakHashMap<>();
    /** Recorded straight into the display list, so one Paint serves every icon. */
    private static final Paint glassPaint = new Paint();
    private static Object pendingDrawable;
    private static Canvas pendingCanvas;

    private LauncherIcons() {}

    static synchronized void install(ClassLoader cl) {
        if (installed) return;
        installed = true;
        Method draw, drawContent = null;
        try {
            Class<?> fbd = Class.forName(FBD, false, cl);
            themed = Class.forName(THEMED, false, cl);
            btv = Class.forName(BTV, false, cl);
            draw = fbd.getMethod("draw", Canvas.class);
            delegate = field(fbd, "delegate");
            display = field(btv, "mDisplay");
            try {
                iconBounds = btv.getMethod("getIconBounds", Rect.class);
            } catch (NoSuchMethodException e) {
                iconBounds = null; // the drawer's glass then fades by the whole view's clip
            }
            Class<?> shape = Class.forName("com.android.launcher3.icons.IconShape", false, cl);
            path = field(shape, "path");
            pathSize = field(shape, "pathSize");
            for (Method m : themed.getDeclaredMethods()) {
                if (m.getName().equals("drawContent") && m.getParameterCount() == 5) drawContent = m;
            }
            if (drawContent == null) throw new NoSuchMethodException(THEMED + ".drawContent");
            try {
                visibleFactor = field(Class.forName("com.android.launcher3.icons.IconNormalizer", false,
                        cl), "ICON_VISIBLE_AREA_FACTOR").getFloat(null);
            } catch (ReflectiveOperationException e) {
                L.once("icon-factor", "no IconNormalizer.ICON_VISIBLE_AREA_FACTOR; assuming "
                        + visibleFactor, null);
            }
            try {
                // A dock prediction draws its own plate and ring over the icon
                // until it is pinned; glass under that plate would never show.
                predicted = Class.forName("com.android.launcher3.views.PredictedAppIcon", false, cl);
                pinned = field(predicted, "mIsPinned");
            } catch (ReflectiveOperationException e) {
                predicted = null;
            }
        } catch (Throwable t) {
            L.e("this launcher has no themed icons of the expected shape; icons left alone", t);
            return;
        }
        XposedBridge.hookMethod(draw, new XC_MethodHook() {
            @Override
            protected void beforeHookedMethod(MethodHookParam param) {
                try {
                    Object d = delegate.get(param.thisObject);
                    boolean mine = d != null && themed.isInstance(d);
                    pendingDrawable = mine ? param.thisObject : null;
                    pendingCanvas = mine ? (Canvas) param.args[0] : null;
                } catch (Throwable e) {
                    pendingDrawable = null;
                    L.once("icon-draw", "icon hook failed; icons stay stock", e);
                }
            }
        });
        XposedBridge.hookMethod(drawContent, new XC_MethodHook() {
            @Override
            protected void beforeHookedMethod(MethodHookParam param) {
                Object drawable = pendingDrawable;
                Canvas canvas = pendingCanvas;
                pendingDrawable = null;
                pendingCanvas = null;
                try {
                    Region.reap();
                    if (drawable == null || canvas != param.args[2]
                            || delegate.get(drawable) != param.thisObject) {
                        return;
                    }
                    drawGlass((Drawable) drawable, param);
                } catch (Throwable e) {
                    L.once("icon-content", "icon hook failed; icons stay stock", e);
                }
            }
        });
        Gate.onChange(LauncherIcons::gateChanged);
        L.i("launcher icons hooked: FastBitmapDrawable.draw, ThemedIconDelegate.drawContent"
                + " (visible area " + visibleFactor + ")");
    }

    private static Field field(Class<?> c, String name) throws NoSuchFieldException {
        Field f = c.getDeclaredField(name);
        f.setAccessible(true);
        return f;
    }

    /** drawContent's arguments: (BitmapInfo, IconShape, Canvas, Rect bounds, Paint). */
    private static void drawGlass(Drawable drawable, XC_MethodHook.MethodHookParam p) throws Exception {
        Object cb = drawable.getCallback();
        if (!(cb instanceof View) || !btv.isInstance(cb)) return;
        int d = display.getInt(cb);
        boolean drawer = d == DISPLAY_ALL_APPS || d == DISPLAY_PREDICTION_ROW || d == DISPLAY_SEARCH_RESULT;
        if (d != DISPLAY_WORKSPACE && !drawer) return;
        View host = (View) cb;
        if (predicted != null && predicted.isInstance(host) && !pinned.getBoolean(host)) {
            Icon was = icons.get(host);
            if (was != null) was.region.hide();
            return;
        }
        Icon icon = icons.get(host);
        if (icon == null) {
            icon = new Icon(host);
            icon.region.ownedBy(host);
            icon.region.fadeWith(drawer ? LauncherIcons::onScreen : Drawer.INSTANCE);
            icons.put(host, icon);
        }
        Canvas canvas = (Canvas) p.args[2];
        Paint paint = (Paint) p.args[4];
        // A software canvas is a snapshot (a drag outline, a preview bitmap):
        // it has no RenderThread to place a region, so it gets the stock plate.
        if (!(drawer ? Gate.drawer() : Gate.icons()) || !canvas.isHardwareAccelerated() || paint == null
                || paint.getAlpha() == 0) {
            icon.region.hide();
            return;
        }
        Shape s = shape(p.args[1]);
        if (s == null) {
            icon.region.hide();
            return;
        }
        // drawContent maps the shape's path into bounds with translate(left, top),
        // scale(width / pathSize), then scale(visibleFactor) about the centre.
        Rect b = (Rect) p.args[3];
        float sx = b.width() / s.size, sy = b.height() / s.size, c = s.size / 2f;
        float f = visibleFactor;
        float left = b.left + sx * (c + f * (s.bounds.left - c));
        float right = b.left + sx * (c + f * (s.bounds.right - c));
        float top = b.top + sy * (c + f * (s.bounds.top - c));
        float bottom = b.top + sy * (c + f * (s.bounds.bottom - c));
        float k = f * Math.min(sx, sy);
        // Punched: the plate becomes a window onto the glass, not onto whatever
        // the launcher drew under it first. After a cold start the Pixel
        // launcher can leave a dock prediction (ring, plate and glyph) under a
        // pinned icon; the stock opaque plate hides it, a translucent one would not.
        icon.region.draw(host, canvas, Math.round(left), Math.round(top), Math.round(right),
                Math.round(bottom), s.radii[0] * k, s.radii[1] * k, s.radii[2] * k, s.radii[3] * k,
                Gate.radius(), Gate.punch(true), paint.getAlpha() / 255f);
        glassPaint.set(paint);
        glassPaint.setAlpha(Math.round(paint.getAlpha() * Gate.fill()));
        p.args[4] = glassPaint;
    }

    /** Main thread. Redraws every icon so it picks up the change. */
    private static void gateChanged() {
        boolean on = Gate.icons() && Gate.drawer();
        for (Icon i : icons.values().toArray(new Icon[0])) {
            if (!on) i.region.hide(); // the next draw puts back what is still on
            View v = i.host.get();
            if (v != null) v.invalidate();
        }
    }

    private static final Rect shown = new Rect(), plate = new Rect();

    /**
     * A drawer icon's fade: the part of its plate (BubbleTextView.getIconBounds)
     * that its parents do not clip, which falls to 0 as the grid scrolls it
     * under the search box or off the bottom.
     */
    private static float onScreen(View host) throws Exception {
        if (!host.getLocalVisibleRect(shown)) return 0f;
        if (iconBounds != null) iconBounds.invoke(host, plate);
        else plate.set(0, 0, host.getWidth(), host.getHeight());
        if (plate.isEmpty()) return 1f;
        float w = Math.max(0, Math.min(plate.right, shown.right) - Math.max(plate.left, shown.left));
        float h = Math.max(0, Math.min(plate.bottom, shown.bottom) - Math.max(plate.top, shown.top));
        return w / plate.width() * (h / plate.height());
    }

    private static Shape shape(Object iconShape) throws IllegalAccessException {
        if (iconShape == null) return null;
        Shape s = shapes.get(iconShape);
        if (s == null) {
            Path p = (Path) path.get(iconShape);
            int size = pathSize.getInt(iconShape);
            s = p == null || size <= 0 ? Shape.NONE : Shape.measure(p, size);
            shapes.put(iconShape, s);
            if (s != Shape.NONE) {
                L.i("icon shape " + size + " px: corners " + s.radii[0] + ", " + s.radii[1] + ", "
                        + s.radii[2] + ", " + s.radii[3] + " in " + s.bounds.toShortString());
            }
        }
        return s == Shape.NONE ? null : s;
    }

    private static final class Icon {
        final Region region = new Region();
        final java.lang.ref.WeakReference<View> host;

        Icon(View host) {
            this.host = new java.lang.ref.WeakReference<>(host);
        }
    }

    /**
     * The theme's icon shape as a rounded rect: its bounds, and per corner
     * (top-left, top-right, bottom-right, bottom-left) the radius of the circle
     * that meets the path where the corner's diagonal does. Exact for a circle
     * or a rounded square; for a squircle or a cookie it is the rounded rect
     * closest at the corners, which is where the two can differ most.
     */
    private static final class Shape {
        static final Shape NONE = new Shape(0);
        private static final int OVERSAMPLE = 4;
        final float size;
        final RectF bounds = new RectF();
        final float[] radii = new float[4];

        private Shape(float size) {
            this.size = size;
        }

        static Shape measure(Path path, int size) {
            Shape s = new Shape(size);
            path.computeBounds(s.bounds, true);
            Path big = new Path(path);
            Matrix m = new Matrix();
            m.setScale(OVERSAMPLE, OVERSAMPLE);
            big.transform(m);
            android.graphics.Region mask = new android.graphics.Region();
            mask.setPath(big, new android.graphics.Region(
                    (int) Math.floor(s.bounds.left * OVERSAMPLE) - 1,
                    (int) Math.floor(s.bounds.top * OVERSAMPLE) - 1,
                    (int) Math.ceil(s.bounds.right * OVERSAMPLE) + 1,
                    (int) Math.ceil(s.bounds.bottom * OVERSAMPLE) + 1));
            float half = Math.min(s.bounds.width(), s.bounds.height()) / 2f;
            float[][] corners = {
                {s.bounds.left, s.bounds.top, 1, 1},
                {s.bounds.right, s.bounds.top, -1, 1},
                {s.bounds.right, s.bounds.bottom, -1, -1},
                {s.bounds.left, s.bounds.bottom, 1, -1},
            };
            double inset = 1 - Math.sqrt(0.5); // where a corner circle meets its diagonal
            for (int i = 0; i < 4; ++i) {
                float r = half;
                for (float t = 0; t <= half; t += 1f / OVERSAMPLE) {
                    int x = Math.round((corners[i][0] + corners[i][2] * t) * OVERSAMPLE);
                    int y = Math.round((corners[i][1] + corners[i][3] * t) * OVERSAMPLE);
                    if (mask.contains(x, y)) {
                        r = (float) Math.min(half, t / inset);
                        break;
                    }
                }
                s.radii[i] = r;
            }
            return s;
        }
    }
}
