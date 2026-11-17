package dev.stockglass.companion;

import android.app.WallpaperColors;
import android.app.WallpaperManager;
import android.appwidget.AppWidgetHostView;
import android.appwidget.AppWidgetProviderInfo;
import android.content.Context;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.ColorFilter;
import android.graphics.Outline;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewOutlineProvider;
import android.widget.ImageView;
import de.robv.android.xposed.XC_MethodHook;
import de.robv.android.xposed.XposedBridge;
import java.lang.ref.WeakReference;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;
import java.util.WeakHashMap;

/**
 * App widgets on the home screen.
 *
 * A widget is a RemoteViews tree inside a LauncherAppWidgetHostView, and its
 * plate is one view's background in that tree: the launcher's
 * RoundedCornerEnforcement.findBackground picks it (the view with id
 * android:id/background, else the first view that draws something). Glass mode
 * multiplies the plate's alpha by app.fill, as for icons and folders, which
 * leaves the widget's text and icons as they are, and puts a region at the plate's rounded rect: the
 * launcher's own (mEnforcedRectangle, mEnforcedCornerRadius) when it rounds
 * the widget itself, else the plate view's outline, else the system widget
 * radius.
 *
 * A widget's text is drawn for its own plate, so with less plate it can be lost
 * in the glass: dark text on a light plate over a dark wallpaper, light text
 * on a dark plate over anything but a dark one. Such a widget keeps
 * app.contrast of its plate instead (see keep()).
 *
 * Neither launcher's host view declares a draw method, so the region is drawn
 * by a Drawable in the host's ViewOverlay. The overlay draws with the host's
 * canvas, after the widget, so the region follows the widget through page
 * scrolls, the spring-loaded shrink and reorder animations. It does not punch:
 * drawn after the content, a punch would erase it. A before-hook on
 * CellLayout.dispatchDraw gives each widget on a page its overlay when the
 * page draws, and a pre-draw check redraws the overlay when the widget's
 * content, plate or plate position changes (a RemoteViews update can replace
 * the plate without the launcher redrawing anything else).
 *
 * Only widgets in the workspace get glass, not the widget picker's previews or
 * a widget being dragged. The one exception is the Pixel launcher's dock
 * search bar (OseWidgetView), a widget host the Hotseat holds directly: it
 * gets glass the same way, under app.search instead of app.widgets.
 */
final class LauncherWidgets {
    private static final String HOST = "com.android.launcher3.widget.LauncherAppWidgetHostView";
    private static final String BASE = "com.android.launcher3.widget.BaseLauncherAppWidgetHostView";
    private static final String ENFORCEMENT = "com.android.launcher3.widget.RoundedCornerEnforcement";
    private static final String CELL = "com.android.launcher3.CellLayout";
    private static final String WORKSPACE = "com.android.launcher3.Workspace";
    private static final String HOTSEAT = "com.android.launcher3.Hotseat";

    private static boolean installed;
    private static Class<?> host, workspace, hotseat;
    private static Field enforced, enforcedRect, enforcedRadius;
    private static Method findBackground;
    private static float systemRadius = -1;
    private static final WeakHashMap<View, Glass> glasses = new WeakHashMap<>();
    private static final WeakHashMap<View, Boolean> watched = new WeakHashMap<>();
    /** A plate's alpha as the widget set it. Drawables for a background, views for an ImageView. */
    private static final WeakHashMap<Object, Integer> original = new WeakHashMap<>();
    /** A plate's tone, read before its alpha first changes. */
    private static final WeakHashMap<Object, Integer> tones = new WeakHashMap<>();
    private static final int DARK = -1, NEITHER = 0, LIGHT = 1, UNREAD = 2;
    /** The home screen wallpaper's tone. Main thread. */
    private static int wallpaper = UNREAD;

    private LauncherWidgets() {}

    static synchronized void install(ClassLoader cl) {
        if (installed) return;
        installed = true;
        Method dispatchDraw;
        try {
            host = Class.forName(HOST, false, cl);
            workspace = Class.forName(WORKSPACE, false, cl);
            try {
                hotseat = Class.forName(HOTSEAT, false, cl);
            } catch (ClassNotFoundException e) {
                hotseat = null;
            }
            dispatchDraw = Class.forName(CELL, false, cl).getDeclaredMethod("dispatchDraw", Canvas.class);
            Class<?> base = Class.forName(BASE, false, cl);
            enforced = field(base, "mIsCornerRadiusEnforced");
            enforcedRect = field(base, "mEnforcedRectangle");
            enforcedRadius = field(base, "mEnforcedCornerRadius");
            try {
                findBackground = Class.forName(ENFORCEMENT, false, cl).getMethod("findBackground", View.class);
            } catch (ReflectiveOperationException e) {
                findBackground = null; // the copy below does the same
            }
        } catch (Throwable t) {
            L.e("this launcher's widget hosts are not the expected shape; widgets left alone", t);
            return;
        }
        XposedBridge.hookMethod(dispatchDraw, new XC_MethodHook() {
            @Override
            protected void beforeHookedMethod(MethodHookParam param) {
                try {
                    Region.reap();
                    scan((ViewGroup) param.thisObject);
                } catch (Throwable e) {
                    L.once("widget-scan", "widget hook failed; widgets stay stock", e);
                }
            }
        });
        Gate.onChange(LauncherWidgets::gateChanged);
        L.i("launcher widgets hooked: CellLayout.dispatchDraw"
                + (findBackground == null ? " (own background lookup)" : ""));
    }

    /**
     * Gives every widget host on this page (CellLayout → ShortcutAndWidgetContainer) its overlay,
     * and the dock's search bar, which the Pixel launcher's Hotseat (a CellLayout) holds directly.
     */
    private static void scan(ViewGroup cell) {
        for (int i = 0; i < cell.getChildCount(); i++) {
            View c = cell.getChildAt(i);
            if (host.isInstance(c)) {
                if (!glasses.containsKey(c)) attach(c);
                continue;
            }
            if (!(c instanceof ViewGroup)) continue;
            ViewGroup items = (ViewGroup) c;
            for (int j = 0; j < items.getChildCount(); j++) {
                View item = items.getChildAt(j);
                if (host.isInstance(item) && !glasses.containsKey(item)) attach(item);
            }
        }
    }

    private static void attach(View h) {
        if (Gate.debug()) L.i("widget glass attached to " + nameOf(h) + " in " + h.getParent().getClass().getSimpleName());
        Glass g = new Glass(h);
        glasses.put(h, g);
        h.getOverlay().add(g);
        watch(h.getRootView());
        g.redraw();
    }

    /** One pre-draw listener per window: redraw an overlay whose widget changed under it. */
    private static void watch(View root) {
        if (watched.containsKey(root)) return;
        watched.put(root, Boolean.TRUE);
        final WeakReference<View> ref = new WeakReference<>(root);
        root.getViewTreeObserver().addOnPreDrawListener(() -> {
            View r = ref.get();
            if (r == null) return true;
            for (Glass g : glasses.values().toArray(new Glass[0])) {
                try {
                    g.check(r);
                } catch (Throwable e) {
                    L.once("widget-check", "cannot check a widget for changes", e);
                }
            }
            return true;
        });
    }

    /** Main thread. Every overlay redraws, which applies or restores its widget's plate. */
    private static void gateChanged() {
        for (Glass g : glasses.values().toArray(new Glass[0])) g.redraw();
    }

    /** The dock's search bar: a widget host the Hotseat holds itself (Pixel launcher). */
    private static boolean searchBar(View v) {
        return hotseat != null && hotseat.isInstance(v.getParent());
    }

    /** Whether this host gets glass where it is: a widget on the workspace, or the dock's search bar. */
    private static boolean placed(View v) {
        return searchBar(v) ? Gate.search() : onWorkspace(v) && Gate.widgets();
    }

    private static boolean onWorkspace(View v) {
        for (Object p = v.getParent(); p instanceof View; p = ((View) p).getParent()) {
            if (workspace.isInstance(p)) return true;
        }
        return false;
    }

    private static View findPlateView(View h) throws Exception {
        if (findBackground != null) return (View) findBackground.invoke(null, h);
        // RoundedCornerEnforcement.findBackground, for a launcher without it.
        View byId = findById(h, android.R.id.background);
        if (byId != null) return byId;
        if (h instanceof ViewGroup && ((ViewGroup) h).getChildCount() > 0) {
            return firstDrawing(((ViewGroup) h).getChildAt(0));
        }
        return h;
    }

    /**
     * The search bar's pill: the visible images, about as wide as the bar, that
     * its RemoteViews stack under the G, mic and Lens buttons.
     */
    private static List<View> searchPlates(View h) {
        List<View> out = new ArrayList<>();
        collectPills(h, h.getWidth(), h.getHeight(), out);
        return out;
    }

    private static void collectPills(View v, int w, int hgt, List<View> out) {
        if (v.getVisibility() != View.VISIBLE) return;
        if (v instanceof ImageView && ((ImageView) v).getDrawable() != null && v.getWidth() >= 0.9f * w
                && v.getHeight() >= 0.5f * hgt) {
            out.add(v);
        }
        if (v instanceof ViewGroup) {
            ViewGroup g = (ViewGroup) v;
            for (int i = 0; i < g.getChildCount(); i++) collectPills(g.getChildAt(i), w, hgt, out);
        }
    }

    private static void tree(View v, int depth, StringBuilder out) {
        out.append("\n  ");
        for (int i = 0; i < depth; i++) out.append("  ");
        out.append(v.getClass().getSimpleName()).append(' ').append(v.getWidth()).append('x').append(v.getHeight())
                .append(v.getVisibility() == View.VISIBLE ? "" : " hidden")
                .append(v.getBackground() == null ? "" : " bg=" + v.getBackground().getClass().getSimpleName())
                .append(v instanceof ImageView && ((ImageView) v).getDrawable() != null
                        ? " img=" + ((ImageView) v).getDrawable().getClass().getSimpleName() : "");
        if (v instanceof ViewGroup && depth < 6) {
            for (int i = 0; i < ((ViewGroup) v).getChildCount(); i++) tree(((ViewGroup) v).getChildAt(i), depth + 1, out);
        }
    }

    private static View findById(View v, int id) {
        if (v.getId() == id) return v;
        if (v instanceof ViewGroup) {
            ViewGroup g = (ViewGroup) v;
            for (int i = 0; i < g.getChildCount(); i++) {
                View f = findById(g.getChildAt(i), id);
                if (f != null) return f;
            }
        }
        return null;
    }

    private static View firstDrawing(View v) {
        if (v.getVisibility() != View.VISIBLE) return null;
        if (!v.willNotDraw() || v.getBackground() != null || v.getForeground() != null) return v;
        if (v instanceof ViewGroup) {
            ViewGroup g = (ViewGroup) v;
            for (int i = 0; i < g.getChildCount(); i++) {
                View f = firstDrawing(g.getChildAt(i));
                if (f != null) return f;
            }
        }
        return null;
    }

    /** What paints the plate: the view's background, else an ImageView's image. */
    private static Object plateOf(View v) {
        if (v == null || v.getVisibility() != View.VISIBLE) return null;
        Drawable bg = v.getBackground();
        if (bg != null) return bg;
        if (v instanceof ImageView && ((ImageView) v).getDrawable() != null) return v;
        return null;
    }

    private static int alphaOf(Object plate) {
        if (plate instanceof ImageView) return ((ImageView) plate).getImageAlpha();
        // ColorDrawable reports its colour's alpha, not the modulation setAlpha takes.
        if (plate instanceof ColorDrawable) return 255;
        return ((Drawable) plate).getAlpha();
    }

    private static void setAlphaOf(Object plate, int a) {
        if (plate instanceof ImageView) {
            ImageView iv = (ImageView) plate;
            if (iv.getImageAlpha() != a) iv.setImageAlpha(a);
        } else {
            Drawable d = ((Drawable) plate).mutate();
            if (d.getAlpha() != a || d instanceof ColorDrawable) d.setAlpha(a);
        }
    }

    /**
     * The share of a widget's plate that stays: app.fill, or app.contrast when
     * the widget's text would be hard to read on the glass. A light plate
     * carries dark text, which is lost on a dark wallpaper; a dark plate
     * carries light text, which is lost on anything but a dark one, since white
     * text loses its contrast well before Android calls a wallpaper light. The
     * wallpaper's tone is Android's own (WallpaperColors hints: dark under a
     * mean luminance of 0.3, light over 0.7). A plate whose colour cannot be
     * read is taken to follow the system theme, as widgets usually do.
     */
    private static float keep(View h, Object plate) {
        Integer t = tones.get(plate);
        int tone = t != null && t != NEITHER ? t : night(h) ? DARK : LIGHT;
        boolean darkWallpaper = wallpaper(h.getContext()) == DARK;
        boolean hard = tone == LIGHT ? darkWallpaper : !darkWallpaper;
        float fill = Gate.fill();
        return hard ? Math.max(fill, Gate.contrast()) : fill;
    }

    private static boolean night(View v) {
        return (v.getResources().getConfiguration().uiMode & Configuration.UI_MODE_NIGHT_MASK)
                == Configuration.UI_MODE_NIGHT_YES;
    }

    /** Why the last plateTone() was NEITHER, for the debug log. Main thread. */
    private static String unread = "";

    /** LIGHT or DARK by the mean colour of a copy of the plate, NEITHER if it cannot be read. */
    private static int plateTone(Object plate) {
        Drawable d = plate instanceof ImageView ? ((ImageView) plate).getDrawable() : (Drawable) plate;
        // A selector's copy has no state, so it draws nothing: read the child it shows.
        if (d != null) d = d.getCurrent();
        Drawable.ConstantState cs = d == null ? null : d.getConstantState();
        unread = d == null ? "no drawable" : d.getClass().getSimpleName() + " has no constant state";
        if (cs == null) return NEITHER;
        Bitmap b = Bitmap.createBitmap(8, 8, Bitmap.Config.ARGB_8888);
        try {
            // A copy, so the widget's drawable keeps its bounds; it carries the
            // plate's tint, which a background colour set from RemoteViews is.
            Drawable copy = cs.newDrawable().mutate();
            copy.setBounds(0, 0, 8, 8);
            copy.draw(new Canvas(b));
            long r = 0, g = 0, bl = 0, w = 0;
            for (int y = 0; y < 8; y++) {
                for (int x = 0; x < 8; x++) {
                    int c = b.getPixel(x, y);
                    int a = Color.alpha(c);
                    r += (long) Color.red(c) * a;
                    g += (long) Color.green(c) * a;
                    bl += (long) Color.blue(c) * a;
                    w += a;
                }
            }
            unread = d.getClass().getSimpleName() + " drew nothing at 8x8";
            if (w == 0) return NEITHER;
            // 0.18 is where black and white text have the same contrast.
            return Color.luminance(Color.rgb((int) (r / w), (int) (g / w), (int) (bl / w))) > 0.18f
                    ? LIGHT : DARK;
        } catch (Throwable e) {
            unread = d.getClass().getSimpleName() + ": " + e; // a hardware bitmap, say
            return NEITHER;
        } finally {
            b.recycle();
        }
    }

    /** The home screen wallpaper's tone, followed as it changes. Main thread. */
    private static int wallpaper(Context c) {
        if (wallpaper != UNREAD) return wallpaper;
        wallpaper = NEITHER;
        try {
            WallpaperManager wm = WallpaperManager.getInstance(c.getApplicationContext());
            wm.addOnColorsChangedListener((colors, which) -> {
                if ((which & WallpaperManager.FLAG_SYSTEM) == 0) return;
                int t = wallpaperTone(colors);
                if (t == wallpaper) return;
                wallpaper = t;
                L.i("wallpaper now " + toneName(t));
                gateChanged();
            }, new Handler(Looper.getMainLooper()));
            wallpaper = wallpaperTone(wm.getWallpaperColors(WallpaperManager.FLAG_SYSTEM));
            L.i("wallpaper " + toneName(wallpaper) + " by Android's colour hints");
        } catch (Throwable t) {
            L.once("wallpaper", "cannot read the wallpaper's colours; widgets take it as not dark", t);
        }
        return wallpaper;
    }

    private static int wallpaperTone(WallpaperColors colors) {
        if (colors == null) return NEITHER;
        int hints = colors.getColorHints();
        if ((hints & WallpaperColors.HINT_SUPPORTS_DARK_THEME) != 0) return DARK;
        if ((hints & WallpaperColors.HINT_SUPPORTS_DARK_TEXT) != 0) return LIGHT;
        return NEITHER;
    }

    private static String toneName(Integer t) {
        return t == null ? "unread" : t == DARK ? "dark" : t == LIGHT ? "light" : "neither dark nor light";
    }

    private static String nameOf(View h) {
        AppWidgetProviderInfo info = h instanceof AppWidgetHostView
                ? ((AppWidgetHostView) h).getAppWidgetInfo() : null;
        return info == null ? "a widget" : info.provider.flattenToShortString();
    }

    private static Field field(Class<?> c, String name) throws NoSuchFieldException {
        Field f = c.getDeclaredField(name);
        f.setAccessible(true);
        return f;
    }

    private static float systemRadius() {
        if (systemRadius < 0) {
            Resources r = Resources.getSystem();
            int id = r.getIdentifier("system_app_widget_background_radius", "dimen", "android");
            systemRadius = id == 0 ? 0f : r.getDimension(id);
        }
        return systemRadius;
    }

    /** The region and plate state of one widget, drawn from its host's overlay. */
    private static final class Glass extends Drawable {
        final WeakReference<View> host;
        final Region region = new Region();
        WeakReference<View> plateView;
        Object plate;
        /** A search bar's other full-width images (the pill is two in Google's bar), faded with the plate. */
        final List<View> extras = new ArrayList<>();
        View lastChild;
        boolean lastOnWorkspace, active;
        float lastKeep = -1;
        /** The region's rect, and the plate view's bounds it came from, in host coordinates. */
        final Rect rect = new Rect(), bounds = new Rect();
        private final Rect tmp = new Rect(), seen = new Rect();
        private final Outline outline = new Outline();

        Glass(View h) {
            host = new WeakReference<>(h);
            region.ownedBy(h);
            region.fadeWith(Drawer.INSTANCE);
        }

        @Override
        public void draw(Canvas canvas) {
            View h = host.get();
            if (h == null) return;
            try {
                drawGlass(h, canvas);
            } catch (Throwable e) {
                L.once("widget-draw", "cannot draw a widget's glass; it stays stock", e);
                restore();
                region.hide();
                active = false;
            }
        }

        private void drawGlass(View h, Canvas canvas) throws Exception {
            lastChild = h instanceof ViewGroup && ((ViewGroup) h).getChildCount() > 0
                    ? ((ViewGroup) h).getChildAt(0) : null;
            lastOnWorkspace = placed(h);
            boolean bar = searchBar(h);
            List<View> pills = bar && lastOnWorkspace ? searchPlates(h) : null;
            View bg = !lastOnWorkspace || !canvas.isHardwareAccelerated() ? null
                    : bar ? (pills.isEmpty() ? null : pills.get(0)) : findPlateView(h);
            Object p = plateOf(bg);
            if (p == null && lastOnWorkspace && Gate.debug()) {
                StringBuilder t = new StringBuilder();
                tree(h, 0, t);
                L.once("no-plate:" + nameOf(h), nameOf(h) + ": no plate found (" + (bg == null ? "no background view"
                        : bg.getClass().getSimpleName() + " draws nothing") + "); it stays stock:" + t, null);
            }
            if (p != plate) restore();
            if (p == null) {
                plate = null;
                plateView = null;
                region.hide();
                active = false;
                return;
            }
            plate = p;
            plateView = new WeakReference<>(bg);
            Integer was = original.get(p);
            boolean seen = was != null;
            if (!seen) {
                was = alphaOf(p);
                original.put(p, was);
                if (!tones.containsKey(p)) tones.put(p, plateTone(p));
            }
            float keep = keep(h, p);
            if ((keep != lastKeep || !seen) && Gate.debug()) {
                Integer t = tones.get(p);
                L.i(nameOf(h) + ": plate " + toneName(t) + " ("
                        + (t != null && t == NEITHER ? unread : p.getClass().getSimpleName())
                        + "), wallpaper " + toneName(wallpaper) + ", keeps " + keep + " of the plate");
            }
            lastKeep = keep;
            setAlphaOf(p, Math.round(was * keep));
            if (pills != null) {
                for (int i = 1; i < pills.size(); i++) {
                    View x = pills.get(i);
                    Integer xw = original.get(x);
                    if (xw == null) {
                        xw = alphaOf(x);
                        original.put(x, xw);
                        extras.add(x);
                    }
                    setAlphaOf(x, Math.round(xw * keep));
                }
            }

            offsetIn(h, bg, bounds);
            float r;
            if (bar) {
                rect.set(bounds);
                r = pillRadius(bg, rect);
            } else if (enforced.getBoolean(h)) {
                rect.set((Rect) enforcedRect.get(h));
                r = enforcedRadius.getFloat(h);
            } else {
                rect.set(bounds);
                r = outlineRadius(bg, rect);
            }
            if (rect.isEmpty()) {
                region.hide();
                active = false;
                return;
            }
            region.draw(h, canvas, rect.left, rect.top, rect.right, rect.bottom, r, r, r, r,
                    Gate.radius(), false, 1f);
            active = true;
        }

        /** {@code v}'s bounds in {@code h}'s coordinates. */
        private void offsetIn(View h, View v, Rect out) {
            out.set(0, 0, v.getWidth(), v.getHeight());
            for (View c = v; c != h && c.getParent() instanceof View; c = (View) c.getParent()) {
                out.offset(c.getLeft(), c.getTop());
            }
        }

        /** The plate's own rounding, which also narrows {@code r} to the outline's rect. */
        private float outlineRadius(View bg, Rect r) {
            ViewOutlineProvider provider = bg.getOutlineProvider();
            if (provider != null) {
                outline.setEmpty();
                provider.getOutline(bg, outline);
                float radius = outline.getRadius();
                if (radius >= 0 && outline.getRect(tmp) && !tmp.isEmpty()) {
                    r.set(r.left + tmp.left, r.top + tmp.top, r.left + tmp.right, r.top + tmp.bottom);
                    return radius;
                }
            }
            return systemRadius();
        }

        /** Puts the plate back as the widget set it. */
        /** A search bar's pill: its GradientDrawable's own corners, at most half its height. */
        private float pillRadius(View bg, Rect r) {
            float half = r.height() / 2f;
            Drawable d = bg instanceof ImageView ? ((ImageView) bg).getDrawable() : bg.getBackground();
            if (d instanceof GradientDrawable) {
                GradientDrawable g = (GradientDrawable) d;
                float[] radii = g.getCornerRadii();
                float c = radii != null && radii.length > 0 ? radii[0] : g.getCornerRadius();
                if (c > 0) return Math.min(c, half);
            }
            return half;
        }

        private void restore() {
            for (View x : extras) {
                Integer xw = original.remove(x);
                if (xw != null) setAlphaOf(x, xw);
            }
            extras.clear();
            if (plate == null) return;
            Integer was = original.remove(plate);
            if (was != null) setAlphaOf(plate, was);
            plate = null;
        }

        /** Pre-draw: redraw if the widget changed since the overlay last drew. */
        void check(View root) {
            View h = host.get();
            if (h == null || !h.isAttachedToWindow() || h.getRootView() != root) return;
            View c0 = h instanceof ViewGroup && ((ViewGroup) h).getChildCount() > 0
                    ? ((ViewGroup) h).getChildAt(0) : null;
            boolean dirty = c0 != lastChild || placed(h) != lastOnWorkspace;
            if (!dirty && active) {
                View bg = plateView == null ? null : plateView.get();
                if (bg == null || plateOf(bg) != plate) {
                    dirty = true;
                } else {
                    offsetIn(h, bg, seen);
                    dirty = !seen.equals(bounds);
                }
            }
            if (dirty) redraw();
        }

        /**
         * Invalidates the overlay. The overlay invalidates a drawable's bounds,
         * and an empty rect reaches no parent, so they are the host's first.
         */
        void redraw() {
            View h = host.get();
            if (h != null) setBounds(0, 0, Math.max(1, h.getWidth()), Math.max(1, h.getHeight()));
            invalidateSelf();
        }

        @Override
        public void setAlpha(int alpha) {}

        @Override
        public void setColorFilter(ColorFilter colorFilter) {}

        @Override
        public int getOpacity() {
            return PixelFormat.TRANSLUCENT;
        }
    }
}
