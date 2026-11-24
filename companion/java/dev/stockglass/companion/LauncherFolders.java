package dev.stockglass.companion;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.os.SystemClock;
import android.view.View;
import de.robv.android.xposed.XC_MethodHook;
import de.robv.android.xposed.XposedBridge;
import java.lang.ref.WeakReference;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.WeakHashMap;

/**
 * Folder icons in Launcher3 and the launchers built from it (crDroid's
 * Launcher3QuickStep, the Pixel launcher).
 *
 * A closed folder's background is not a view or a drawable: FolderIcon
 * dispatchDraw (or CellLayout, while a drop hovers over it) calls
 * {@code PreviewBackground.drawBackground(Canvas)}, which fills the folder
 * shape at (basePreviewOffsetX, basePreviewOffsetY) with radius
 * getScaledRadius() in {@code mBgColor}. A before-hook on drawBackground draws
 * this folder's blur region at that circle's bounds into the same canvas, then
 * lets the shape fill with mBgColor's alpha cut to app.fill. The canvas is the
 * FolderIcon's (or CellLayout's) own display list, so the region follows the
 * workspace scroll and the hover/accept scale animation.
 *
 * The launcher window sits directly over the wallpaper, so what the glass
 * refracts is the wallpaper. The icons around it are in the same layer and are
 * never inside a region of their own layer.
 */
final class LauncherFolders {
    private static final String PB = "com.android.launcher3.folder.PreviewBackground";
    private static final float GENERIC_SHAPE_RATIO = 0.6f;
    private static boolean installed;
    private static final WeakHashMap<Object, Folder> folders = new WeakHashMap<>();

    private static Field offX, offY, previewSize, bgColor, invalidateDelegate, drawingDelegate, context;
    private static Method scaledRadius, getShape;
    private static ClassLoader loader;

    private LauncherFolders() {}

    static synchronized void install(ClassLoader cl) {
        if (installed) return;
        installed = true;
        loader = cl;
        if (installOplus(cl)) return;
        Method target;
        try {
            Class<?> pb = Class.forName(PB, false, cl);
            target = pb.getDeclaredMethod("drawBackground", Canvas.class);
            offX = field(pb, "basePreviewOffsetX");
            offY = field(pb, "basePreviewOffsetY");
            previewSize = field(pb, "previewSize");
            bgColor = field(pb, "mBgColor");
            invalidateDelegate = field(pb, "mInvalidateDelegate");
            context = field(pb, "mContext");
            scaledRadius = pb.getDeclaredMethod("getScaledRadius");
            scaledRadius.setAccessible(true);
            try {
                drawingDelegate = field(pb, "mDrawingDelegate");
            } catch (NoSuchFieldException e) {
                drawingDelegate = null;
            }
            try {
                getShape = pb.getDeclaredMethod("getShape"); // crDroid; the Pixel launcher inlined it
                getShape.setAccessible(true);
            } catch (NoSuchMethodException e) {
                getShape = null;
            }
        } catch (Throwable t) {
            L.e("this launcher's PreviewBackground is not the expected shape; folders left alone", t);
            return;
        }
        XposedBridge.hookMethod(target, new XC_MethodHook() {
            @Override
            protected void beforeHookedMethod(MethodHookParam param) {
                try {
                    Region.reap();
                    draw(param.thisObject, (Canvas) param.args[0]);
                } catch (Throwable e) {
                    L.once("folder", "folder hook failed; folders stay stock", e);
                }
            }
        });
        Gate.onChange(LauncherFolders::gateChanged);
        L.i("launcher folders hooked: PreviewBackground.drawBackground");
    }

    /**
     * ColorOS 17's OplusLauncher. Its folder plate (small, 2x2 and the big 3x3
     * folders alike) is a child view of the folder icon: FolderRoundImageView,
     * an ImageView laid out at the plate's bounds by OplusPreviewBackground
     * .setBackground, whose drawable is ColorOS's own frost (a
     * BlurTransitionDrawable blurring in the launcher's process) clipped to a
     * smooth rounded square of mRadius. OplusPreviewBackground.drawBackground
     * draws the same plate straight into the icon only during the fall and
     * convert animations, so the view is where the plate is.
     *
     * A before-hook on its onDraw puts this folder's region at the view's
     * bounds with mRadius, then draws ColorOS's frost over it at app.fill of
     * its strength (as every other surface keeps app.fill of its plate), and
     * skips the original. With folder glass off the original runs untouched.
     */
    private static boolean installOplus(ClassLoader cl) {
        final Class<?> view;
        final Field radius;
        final Method onDraw;
        try {
            view = Class.forName("com.android.launcher3.folder.FolderRoundImageView", false, cl);
            onDraw = view.getDeclaredMethod("onDraw", Canvas.class);
            radius = field(view, "mRadius");
        } catch (Throwable t) {
            return false; // not OplusLauncher
        }
        XposedBridge.hookMethod(onDraw, new XC_MethodHook() {
            @Override
            protected void beforeHookedMethod(MethodHookParam param) {
                try {
                    Region.reap();
                    if (drawOplus((android.widget.ImageView) param.thisObject, (Canvas) param.args[0],
                            radius.getFloat(param.thisObject))) {
                        param.setResult(null);
                    }
                } catch (Throwable e) {
                    L.once("folder-oplus", "folder hook failed; folders stay stock", e);
                }
            }
        });
        Gate.onChange(LauncherFolders::gateChanged);
        L.i("launcher folders hooked: FolderRoundImageView.onDraw (OplusLauncher)");
        return true;
    }

    /** Draws the plate as glass; false leaves it to the original onDraw. */
    private static boolean drawOplus(android.widget.ImageView v, Canvas canvas, float r) {
        Folder f = folders.get(v);
        if (f == null) {
            f = new Folder();
            f.region.ownedBy(v);
            f.region.fadeWith(Drawer.INSTANCE);
            f.host = new WeakReference<>(v);
            folders.put(v, f);
        }
        android.graphics.drawable.Drawable d = v.getDrawable();
        if (OplusFrost.handles(d)) { // its own draw is hooked there
            f.region.hide();
            return false;
        }
        int w = v.getWidth(), h = v.getHeight();
        if (!Gate.folders() || d == null || w <= 0 || h <= 0 || !canvas.isHardwareAccelerated()) {
            f.region.hide();
            return false;
        }
        float corner = Math.max(0f, Math.min(r, Math.min(w, h) / 2f));
        f.region.draw(v, canvas, 0, 0, w, h, corner, corner, corner, corner,
                Gate.radius(), Gate.punch(true));
        int keep = Math.round(255 * Gate.fill());
        if (keep > 0) {
            d.setVisible(true, false);
            d.setBounds(0, 0, w, h);
            int save = canvas.saveLayerAlpha(0, 0, w, h, keep);
            android.graphics.Path clip = new android.graphics.Path();
            clip.addRoundRect(0, 0, w, h, corner, corner, android.graphics.Path.Direction.CW);
            canvas.clipPath(clip);
            d.draw(canvas);
            canvas.restoreToCount(save);
        }
        return true;
    }

    /**
     * Main thread. Redraws every folder icon so it picks up the change; a
     * folder on another workspace page redraws when it is next shown. Turning
     * off also hides every region at once.
     */
    private static void gateChanged() {
        boolean on = Gate.folders();
        for (Folder f : folders.values().toArray(new Folder[0])) {
            if (!on) f.region.hide();
            View h = f.host == null ? null : f.host.get();
            if (h != null) h.invalidate();
        }
    }

    private static Field field(Class<?> c, String name) throws NoSuchFieldException {
        Field f = c.getDeclaredField(name);
        f.setAccessible(true);
        return f;
    }

    private static void draw(Object bg, Canvas canvas) throws Exception {
        Folder f = folders.get(bg);
        if (f == null) {
            f = new Folder();
            f.region.ownedBy(bg);
            f.region.fadeWith(Drawer.INSTANCE);
            folders.put(bg, f);
        }
        View host = (View) invalidateDelegate.get(bg);
        if (host == null && drawingDelegate != null) host = (View) drawingDelegate.get(bg);
        if (host != null && (f.host == null || f.host.get() != host)) f.host = new WeakReference<>(host);
        int color = bgColor.getInt(bg);
        if (!Gate.folders()) {
            if (f.known && color == f.glassColor) bgColor.setInt(bg, f.stockColor);
            f.known = false;
            f.region.hide();
            return;
        }
        // mBgColor comes from the theme and changes with it; anything that is
        // not the colour we wrote is the theme's new colour.
        if (!f.known || color != f.glassColor) {
            f.stockColor = color;
            f.known = true;
        }
        f.glassColor = (f.stockColor & 0x00ffffff)
                | (Math.round(Color.alpha(f.stockColor) * Gate.fill()) << 24);
        bgColor.setInt(bg, f.glassColor);

        if (host == null) return;
        int r = (int) scaledRadius.invoke(bg);
        int grow = r - previewSize.getInt(bg) / 2;
        int x = offX.getInt(bg) - grow;
        int y = offY.getInt(bg) - grow;
        float corner = r * f.ratio(bg);
        f.region.draw(host, canvas, x, y, x + 2 * r, y + 2 * r, corner, corner, corner, corner,
                Gate.radius(), Gate.punch(true));
    }

    private static final class Folder {
        final Region region = new Region();
        WeakReference<View> host;
        boolean known;
        int stockColor, glassColor;
        private float ratio = 1f;
        private long ratioAt = -1;

        /** Corner radius over half-size for the theme's folder shape; re-read once a second. */
        float ratio(Object bg) {
            long now = SystemClock.uptimeMillis();
            if (ratioAt >= 0 && now - ratioAt < 1000) return ratio;
            ratioAt = now;
            Object shape = null;
            try {
                if (getShape != null) {
                    shape = getShape.invoke(bg);
                } else {
                    // Pixel launcher: ThemeManager.INSTANCE.get(ctx).iconState.folderShape
                    Class<?> tm = Class.forName("com.android.launcher3.graphics.ThemeManager",
                            false, loader);
                    Object holder = tm.getField("INSTANCE").get(null);
                    Object manager = holder.getClass().getMethod("get", Context.class)
                            .invoke(holder, context.get(bg));
                    Object state = field(tm, "iconState").get(manager);
                    shape = field(state.getClass(), "folderShape").get(state);
                }
            } catch (Throwable e) {
                L.once("folder-shape", "cannot read the folder shape; using a circle", e);
            }
            if (shape == null) return ratio = 1f;
            String name = shape.getClass().getName();
            if (name.endsWith("$Circle")) return ratio = 1f;
            try {
                return ratio = field(shape.getClass(), "radiusRatio").getFloat(shape);
            } catch (Throwable e) {
                L.once("folder-shape:" + name, "folder shape " + name
                        + " is a path; its glass is a rounded square", null);
                return ratio = GENERIC_SHAPE_RATIO;
            }
        }
    }
}
