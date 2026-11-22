package dev.stockglass.auto.maps;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Rect;
import android.graphics.drawable.Drawable;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewTreeObserver;
import android.view.WindowManager;
import de.robv.android.xposed.XC_MethodHook;
import de.robv.android.xposed.XposedBridge;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;
import java.util.WeakHashMap;

/**
 * Google Maps' Android Auto screen. Maps draws its car UI in one window of its
 * own, a TYPE_PRIVATE_PRESENTATION window on its private virtual display, with
 * the map in a SurfaceView layer below that window. A blur region on a control
 * in that window therefore blurs the map, and the engine draws it as glass.
 *
 * Nothing here names an obfuscated class or member. The car window is found by
 * its window type, through the framework's WindowManagerGlobal (a before-hook
 * on addView for windows made later, and its list of roots for windows that
 * already exist when the module is attached late, as the dev loader does). The
 * controls are found by class: every androidx CardView in the car window whose
 * plate is opaque, and nothing inside one that already has glass. In
 * Maps 26.14 those are the settings, mute and compass buttons, the zoom
 * buttons and the Search pill; they are plain Views (CardView with an
 * AppCompatImageView), not Compose. Maps' own resource ids for the buttons are
 * checked as well, so a release that stops using CardView for them shows up in
 * logcat, and such a button still gets glass on the nearest ancestor that draws
 * a rounded plate.
 *
 * The scan runs after every layout pass of the car window (a few hundred
 * views; tens of microseconds), so controls that appear later (zoom, the
 * search panel, cards during navigation) are picked up when they are laid out.
 */
final class MapsCar {
    static final int TYPE_PRIVATE_PRESENTATION = 2030;
    /** View tag key on a car window's root: a Runnable that retires the build that set it. */
    private static final int TAG_KEY = 0x5a17ca70;
    /** Maps' resource ids for its floating controls, as of 26.14 (a check, see above). */
    private static final String[] ANCHOR_IDS = {
        "map_buttons_view_settings_button",
        "map_buttons_view_compass",
        "map_buttons_view_zoom_in",
        "main_suggested_destinations_search_box",
    };
    /** A plate bigger than this share of the car window is not a control (a panel). */
    private static final float MAX_AREA = 0.5f;
    /** A floating plate that is not a CardView: at least this big each way, and this rounded. */
    private static final float MIN_SIDE_DP = 24f, MIN_RADIUS_DP = 6f;

    private static final WeakHashMap<View, Watch> roots = new WeakHashMap<>();
    private static final Runnable retire = MapsCar::dispose;
    private static int[] anchorIds;

    private MapsCar() {}

    static void install(ClassLoader cl) {
        try {
            Class<?> wmg = Class.forName("android.view.WindowManagerGlobal");
            int hooked = 0;
            for (Method m : wmg.getDeclaredMethods()) {
                Class<?>[] p = m.getParameterTypes();
                if (!m.getName().equals("addView") || p.length < 2 || p[0] != View.class) continue;
                XposedBridge.hookMethod(m, new XC_MethodHook() {
                    @Override
                    protected void beforeHookedMethod(MethodHookParam param) {
                        try {
                            Object lp = param.args[1];
                            if (!(param.args[0] instanceof View) || !(lp instanceof WindowManager.LayoutParams)) return;
                            if (((WindowManager.LayoutParams) lp).type != TYPE_PRIVATE_PRESENTATION) return;
                            final View root = (View) param.args[0];
                            Gate.handler().post(() -> watch(root));
                        } catch (Throwable t) {
                            L.once("addview", "cannot follow a new car window", t);
                        }
                    }
                });
                ++hooked;
            }
            L.i("watching for car windows (" + hooked + " WindowManagerGlobal.addView hook"
                    + (hooked == 1 ? "" : "s") + ")");
        } catch (Throwable t) {
            L.e("cannot hook WindowManagerGlobal.addView; only car windows that exist now get glass", t);
        }
        Gate.handler().post(MapsCar::scanExisting);
    }

    /** Car windows that were added before this module was loaded. */
    private static void scanExisting() {
        try {
            Class<?> wmg = Class.forName("android.view.WindowManagerGlobal");
            Object g = wmg.getMethod("getInstance").invoke(null);
            Field fv = wmg.getDeclaredField("mViews");
            Field fp = wmg.getDeclaredField("mParams");
            fv.setAccessible(true);
            fp.setAccessible(true);
            Object lock = null;
            try {
                Field fl = wmg.getDeclaredField("mLock");
                fl.setAccessible(true);
                lock = fl.get(g);
            } catch (NoSuchFieldException ignored) {
            }
            List<View> views = new ArrayList<>();
            List<WindowManager.LayoutParams> params = new ArrayList<>();
            synchronized (lock != null ? lock : g) {
                @SuppressWarnings("unchecked") List<View> v = (List<View>) fv.get(g);
                @SuppressWarnings("unchecked") List<WindowManager.LayoutParams> p =
                        (List<WindowManager.LayoutParams>) fp.get(g);
                views.addAll(v);
                params.addAll(p);
            }
            int n = 0;
            for (int i = 0; i < views.size() && i < params.size(); ++i) {
                if (params.get(i).type != TYPE_PRIVATE_PRESENTATION) continue;
                watch(views.get(i));
                ++n;
            }
            L.i(views.size() + " windows in this process, " + n + " of them car windows");
        } catch (Throwable t) {
            L.e("cannot list this process's windows; car windows made from now on still get glass", t);
        }
    }

    /** A car window's root: rescans after each layout pass. */
    private static final class Watch {
        final View root;
        final ViewTreeObserver.OnGlobalLayoutListener layout;
        final View.OnAttachStateChangeListener attach;

        Watch(View root) {
            this.root = root;
            layout = () -> scan(root);
            attach = new View.OnAttachStateChangeListener() {
                @Override
                public void onViewAttachedToWindow(View v) {
                    v.getViewTreeObserver().addOnGlobalLayoutListener(layout);
                    scan(v);
                }

                @Override
                public void onViewDetachedFromWindow(View v) {
                    v.getViewTreeObserver().removeOnGlobalLayoutListener(layout);
                }
            };
        }
    }

    static void watch(View root) {
        if (Gate.retired || root == null || roots.containsKey(root)) return;
        // A build loaded earlier into this process (dev loader) goes first.
        Object old = root.getTag(TAG_KEY);
        if (old instanceof Runnable && old != retire) {
            L.i("retiring the build loaded before this one");
            try {
                ((Runnable) old).run();
            } catch (Throwable t) {
                L.e("the earlier build did not retire cleanly", t);
            }
        }
        root.setTag(TAG_KEY, retire);
        Watch w = new Watch(root);
        if (roots.isEmpty() && !rescanning) {
            rescanning = true;
            Gate.onChange(MapsCar::rescanAll);
        }
        roots.put(root, w);
        root.addOnAttachStateChangeListener(w.attach);
        root.getViewTreeObserver().addOnGlobalLayoutListener(w.layout);
        L.i("car window " + root.getClass().getSimpleName() + "@" + Integer.toHexString(System.identityHashCode(root))
                + " " + root.getWidth() + "x" + root.getHeight() + " on display "
                + (root.getDisplay() == null ? "?" : root.getDisplay().getDisplayId() + " \""
                + root.getDisplay().getName() + "\"") + ", density "
                + root.getResources().getDisplayMetrics().densityDpi + " dpi");
        scan(root);
    }

    /** Takes this build out of the process: plates back to stock, regions gone, listeners off. */
    static void dispose() {
        Gate.retired = true;
        for (java.util.Map.Entry<View, Watch> e : roots.entrySet()) {
            View root = e.getKey();
            Watch w = e.getValue();
            if (root == null) continue;
            root.removeOnAttachStateChangeListener(w.attach);
            root.getViewTreeObserver().removeOnGlobalLayoutListener(w.layout);
            if (root.getTag(TAG_KEY) == retire) root.setTag(TAG_KEY, null);
        }
        roots.clear();
        PlateGlass.detachAll();
        L.i("retired");
    }

    private static boolean rescanning;

    /** A switch changed (a dump asked for, say): scan now rather than at the next layout pass. */
    private static void rescanAll() {
        for (View root : roots.keySet().toArray(new View[0])) {
            if (root != null && root.isAttachedToWindow()) scan(root);
        }
    }

    private static void scan(View root) {
        if (Gate.retired) return;
        try {
            int rootArea = Math.max(1, root.getWidth() * root.getHeight());
            walk(root, rootArea, null);
            checkAnchors(root, rootArea);
            TreeDump.maybe(root);
        } catch (Throwable t) {
            L.once("scan", "cannot scan the car window for controls", t);
        }
    }

    private static void walk(View v, int rootArea, View plate) {
        if (v.getVisibility() == View.GONE) return;
        if (PlateGlass.has(v)) {
            // A plate that is clear at this size (Maps' search panel when it is
            // only the frame round the Search pill) does not cover what is in it.
            if (plate == null && PlateGlass.covers(v)) plate = v;
        } else if (plate == null && isPlate(v, rootArea)) {
            PlateGlass.attach(v, describe(v));
            plate = v;
        } else if (plate != null && isLining(v, plate)) {
            PlateGlass.attachLining(v, describe(v) + " (lining)");
        }
        if (v instanceof ViewGroup) {
            ViewGroup g = (ViewGroup) v;
            for (int i = 0; i < g.getChildCount(); ++i) walk(g.getChildAt(i), rootArea, plate);
        }
    }

    /**
     * An opaque background inside a plate that covers nearly all of it: the
     * plate's real face (Maps' arrival panel in navigation is a CardView
     * holding a FrameLayout with a white rounded GradientDrawable). It gets
     * the plate's fill, not a region of its own.
     */
    private static boolean isLining(View v, View plate) {
        if (v.getBackground() == null) return false;
        if (v.getWidth() < 0.9f * plate.getWidth() || v.getHeight() < 0.9f * plate.getHeight()) return false;
        return plateAlpha(v) >= 0x80;
    }

    /**
     * A control's plate, at most MAX_AREA of the window, opaque in its middle:
     * an androidx CardView, or (unless auto.plates=cards) any view that floats
     * over the map on a rounded plate of its own (the speedometer, cards
     * during navigation).
     */
    private static boolean isPlate(View v, int rootArea) {
        if (v.getBackground() == null) return false;
        if (v.getWidth() * v.getHeight() > MAX_AREA * rootArea) return false;
        if (!isCard(v) && !(Gate.anyPlate() && floatingRounded(v))) return false;
        return plateAlpha(v) >= 0x80;
    }

    private static int speedometerId = -1;

    /**
     * Maps' speedometer is a flat round plate (a LayerDrawable: its shadow is
     * painted in), with no elevation, holding id/speedometer. Named, so that
     * other flat round things (a speed limit sign) keep their look.
     */
    private static boolean holdsSpeedometer(View v) {
        if (speedometerId == -1) {
            speedometerId = v.getResources().getIdentifier("speedometer", "id", Entry.MAPS);
            if (speedometerId == 0) L.w("Maps has no id/speedometer any more");
        }
        return speedometerId != 0 && v instanceof ViewGroup && v.findViewById(speedometerId) != null;
    }

    /**
     * Maps' turn card during navigation (the step and the "Then" chip below
     * it): Views from Google's navigation library, by class name, which that
     * library ships unobfuscated. Their plates are a drawable of Maps' own with
     * a path outline (the corners where the two meet are square).
     */
    static boolean isTurnCard(View v) {
        return v.getClass().getName().startsWith("com.google.android.libraries.geo.navcore.turncard.");
    }

    private static final android.graphics.Outline outline = new android.graphics.Outline();
    private static final Rect outlineRect = new Rect();

    /**
     * Elevated or clipped to its outline, and that outline a rounded rect
     * (corner radius at least MIN_RADIUS_DP) at least MIN_SIDE_DP each way.
     * A background's outline needs the background's bounds, which a view that
     * was just laid out has not given it yet, so that one is read with the
     * view's size (as plateAlpha does).
     */
    private static boolean floatingRounded(View v) {
        float dp = v.getResources().getDisplayMetrics().density;
        int w = v.getWidth(), h = v.getHeight();
        if (Math.min(w, h) < MIN_SIDE_DP * dp) return false;
        boolean named = holdsSpeedometer(v) || isTurnCard(v);
        if (v.getElevation() + v.getTranslationZ() <= 0f && !v.getClipToOutline() && !named) return false;
        android.view.ViewOutlineProvider op = v.getOutlineProvider();
        if (op == null) return false;
        outline.setEmpty();
        Drawable bg = v.getBackground();
        if (op == android.view.ViewOutlineProvider.BACKGROUND) {
            Rect old = bg.copyBounds();
            if (old.width() != w || old.height() != h) bg.setBounds(0, 0, w, h);
            try {
                bg.getOutline(outline);
            } finally {
                if (!bg.getBounds().equals(old)) bg.setBounds(old);
            }
        } else {
            op.getOutline(v, outline);
        }
        // A path outline (corners rounded unevenly) only for plates known by
        // name; PlateGlass measures their corners from the drawn plate.
        if (!outline.getRect(outlineRect) || outlineRect.isEmpty()) return named && !outline.isEmpty();
        return outline.getRadius() >= MIN_RADIUS_DP * dp;
    }

    static boolean isCard(View v) {
        for (Class<?> c = v.getClass(); c != null && c != View.class; c = c.getSuperclass()) {
            String n = c.getName();
            if (n.equals("androidx.cardview.widget.CardView") || n.equals("android.support.v7.widget.CardView")) {
                return true;
            }
        }
        return false;
    }

    /** Per plate drawable: {alpha, width, height} when it was read. */
    private static final WeakHashMap<Drawable, int[]> plateAlphas = new WeakHashMap<>();

    /**
     * How opaque the card's plate is in its middle: the background drawn into
     * a 9 x 9 bitmap, alpha of the centre pixel; -1 before the view has a size.
     * Drawn rather than asked: CardView.getCardBackgroundColor() is renamed by
     * R8 in Maps, and Maps' Search pill is an opaque CardView inside a
     * transparent MaterialCardView (id/scene_card), which must not get the glass.
     * A view that was just laid out has not given its background its new
     * bounds yet (View.draw does that), so the read uses the view's size.
     */
    private static int plateAlpha(View v) {
        Drawable bg = v.getBackground();
        int w = v.getWidth(), h = v.getHeight();
        if (w <= 0 || h <= 0) return -1;
        int[] known = plateAlphas.get(bg);
        if (known != null && known[1] == w && known[2] == h) return known[0];
        int a;
        Rect old = bg.copyBounds();
        try {
            Bitmap bm = Bitmap.createBitmap(9, 9, Bitmap.Config.ARGB_8888);
            Canvas c = new Canvas(bm);
            c.scale(9f / w, 9f / h);
            if (old.width() != w || old.height() != h) bg.setBounds(0, 0, w, h);
            bg.draw(c);
            a = Color.alpha(bm.getPixel(4, 4));
            bm.recycle();
        } catch (Throwable t) {
            L.once("plate-alpha", "cannot draw a card's plate to read it; taken as opaque", t);
            a = 255;
        } finally {
            if (!bg.getBounds().equals(old)) bg.setBounds(old);
        }
        plateAlphas.put(bg, new int[] {a, w, h});
        if (Gate.debug() || a < 0x80) L.i("plate of " + describe(v) + " " + w + "x" + h + ": alpha " + a);
        return a;
    }

    /**
     * Each of Maps' known control ids that is on screen should sit in a plate
     * with glass. One that does not means the layout changed: it gets glass on
     * the nearest ancestor that draws a rounded background, and a line in
     * logcat says so once.
     */
    private static void checkAnchors(View root, int rootArea) {
        if (anchorIds == null) {
            anchorIds = new int[ANCHOR_IDS.length];
            for (int i = 0; i < ANCHOR_IDS.length; ++i) {
                anchorIds[i] = root.getResources().getIdentifier(ANCHOR_IDS[i], "id", Entry.MAPS);
                if (anchorIds[i] == 0) L.w("Maps has no id/" + ANCHOR_IDS[i] + " any more");
            }
        }
        for (int i = 0; i < anchorIds.length; ++i) {
            if (anchorIds[i] == 0) continue;
            View a = root.findViewById(anchorIds[i]);
            if (a == null || !a.isShown() || a.getWidth() == 0) continue;
            View plate = null;
            boolean covered = false;
            for (Object p = a; p instanceof View; p = ((View) p).getParent()) {
                View x = (View) p;
                if (PlateGlass.has(x)) {
                    covered = true;
                    break;
                }
                if (plate == null && x.getBackground() != null && x.getWidth() * x.getHeight() <= MAX_AREA * rootArea
                        && x.getClipToOutline() | x.getOutlineProvider() != null) {
                    plate = x;
                }
                if (x == root) break;
            }
            if (covered) continue;
            L.once("anchor:" + ANCHOR_IDS[i], "id/" + ANCHOR_IDS[i] + " is not inside a CardView with glass; "
                    + (plate == null ? "no plate found for it" : "using " + plate.getClass().getName()), null);
            if (plate != null) PlateGlass.attach(plate, ANCHOR_IDS[i] + " (fallback)");
        }
    }

    private static String describe(View v) {
        String id = "no id";
        if (v.getId() != View.NO_ID) {
            try {
                id = "id/" + v.getResources().getResourceEntryName(v.getId());
            } catch (Throwable t) {
                id = "id/0x" + Integer.toHexString(v.getId());
            }
        }
        // The first descendant with a known id or text says which control it is.
        String inner = innerName(v, 0);
        return v.getClass().getSimpleName() + " " + id + (inner == null ? "" : " [" + inner + "]");
    }

    private static String innerName(View v, int depth) {
        if (depth > 6) return null;
        if (depth > 0 && v.getId() != View.NO_ID) {
            try {
                return v.getResources().getResourceEntryName(v.getId());
            } catch (Throwable ignored) {
            }
        }
        CharSequence cd = v.getContentDescription();
        if (cd != null && cd.length() > 0) return "\"" + cd + "\"";
        if (v instanceof ViewGroup) {
            ViewGroup g = (ViewGroup) v;
            for (int i = 0; i < g.getChildCount(); ++i) {
                String s = innerName(g.getChildAt(i), depth + 1);
                if (s != null) return s;
            }
        }
        return null;
    }
}
