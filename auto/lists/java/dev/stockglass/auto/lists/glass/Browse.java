package dev.stockglass.auto.lists.glass;

import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.ColorFilter;
import android.graphics.Outline;
import android.graphics.PixelFormat;
import android.graphics.RecordingCanvas;
import android.graphics.Rect;
import android.graphics.RenderEffect;
import android.graphics.RenderNode;
import android.graphics.RuntimeShader;
import android.graphics.Shader;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.Drawable;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewOutlineProvider;
import android.view.ViewTreeObserver;
import android.view.WindowManager;
import android.widget.LinearLayout;
import de.robv.android.xposed.XC_MethodHook.MethodHookParam;
import java.lang.ref.WeakReference;
import java.lang.reflect.Field;
import java.lang.reflect.Member;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;
import java.util.WeakHashMap;
import java.util.function.BiConsumer;

/**
 * A CarPlay-like media browse: on the root screen of a media app in Android
 * Auto (Car App Library host, Android Auto 18.0), the tab header floats as a
 * glass capsule over content that runs the full height of the window and
 * scrolls under it.
 *
 * The screen (captures/2026-11-22-android-auto/lists/views-browse-home.txt):
 * a vertical LinearLayout holding id/header_view (MaterialTabHeaderView, 96 px:
 * the app's leading action and a Material TabLayout) above
 * id/content_container (the content's Compose, which hosts the list as a
 * PagedListView > AutoTunedRecyclerView, id/tuned_recycler_view, with
 * clipToPadding off). All Views with resource names, so nothing obfuscated
 * is named here.
 *
 *   - Layout (LayoutParams only, re-checked after every layout pass): the
 *     header gets a width that fits its tabs, centred, 8 px from the top; the
 *     content gets a negative top margin, so it starts at the top of the
 *     window and is as tall as it; the list's parent (MaxWidthLayout) gets a
 *     top padding with clipping off, so the first row starts below the capsule
 *     and the rest scroll under it (the list ignores a padding of its own,
 *     see Floated.apply). The header is
 *     raised (translationZ) so it draws, and takes touches, above the content.
 *   - Glass: the header's background is a LivePlate. It draws a RenderNode of
 *     its own that draws the content's RenderNode (the very display list the
 *     window draws, by reference, so no extra drawing on the UI thread) with a
 *     RenderEffect: a blur, then the engine's lean glass shader (GlassSource,
 *     the code SurfaceFlinger runs for a blur region) with both its backdrops
 *     fed from that blurred content. The header clips to its capsule outline;
 *     its tabs draw over the glass, crisp.
 *   - Live: a RenderNode drawn by reference does not damage its second
 *     place when it changes, so once per frame in which anything in this
 *     process was recorded, the backdrop is recorded again (two ops), which
 *     makes RenderThread redraw the glass from the content's new display list.
 *
 * Sub-screens (an album, a playlist) have a Compose header and no
 * id/header_view: they are left as they are, and the root screen's layout is
 * put back whenever its header is hidden.
 *
 * Switch: persist.sys.glass.auto.browse=0 leaves the browse screen stock.
 */
final class Browse {
    private static final String TAG = "SGAutoBrowse";
    private static final String GH = "com.google.android.projection.gearhead";
    /** The window's flat colour under the content (Android Auto 18.0 dark theme). */
    private static final int WINDOW_BG = 0xff242629;

    /** True while this class records its own RenderNodes: CardGlass ignores the hook calls. */
    static volatile boolean busy;

    private static Handler main;
    private static boolean started;
    private static final WeakHashMap<View, Boolean> roots = new WeakHashMap<>();
    private static final WeakHashMap<View, Floated> columns = new WeakHashMap<>();
    private static int idHeader, idContent, idList;
    private static Field renderNodeField;
    private static long frameSeen = -1;

    private Browse() {}

    static void start(BiConsumer<Member, String> hooker, Handler handler) {
        main = handler;
        try {
            Class<?> wmg = Class.forName("android.view.WindowManagerGlobal");
            for (Method m : wmg.getDeclaredMethods()) {
                Class<?>[] p = m.getParameterTypes();
                if (m.getName().equals("addView") && p.length >= 2 && p[0] == View.class) hooker.accept(m, "addview");
            }
        } catch (Throwable t) {
            Log.e(TAG, "cannot follow new windows; only windows open now get the floating tabs", t);
        }
        started = true;
        main.post(Browse::scanExisting);
        Log.i(TAG, "browse ready");
    }

    static void stop() {
        started = false;
        for (Floated f : columns.values().toArray(new Floated[0])) {
            if (f != null) f.restore();
        }
        columns.clear();
        Log.i(TAG, "browse stopped; layouts put back");
    }

    /** Before-hook of WindowManagerGlobal.addView. */
    static void onAddView(MethodHookParam p) {
        if (!started || !(p.args[0] instanceof View)) return;
        final View root = (View) p.args[0];
        main.post(() -> watch(root));
    }

    /**
     * Before-hook of RenderNode.endRecording, any node, any thread: once per
     * UI frame, record the backdrops again so their glass follows the content.
     */
    static void onRecorded(RenderNode n) {
        if (!started || columns.isEmpty() || busy || Looper.myLooper() != Looper.getMainLooper()) return;
        // The animation clock is locked to the frame's time while a frame runs.
        long frame = android.view.animation.AnimationUtils.currentAnimationTimeMillis();
        if (frame == frameSeen) return;
        frameSeen = frame;
        for (Floated f : columns.values()) {
            if (f != null && f.plate != null) f.plate.recordBackdrop();
        }
    }

    private static void scanExisting() {
        try {
            Class<?> wmg = Class.forName("android.view.WindowManagerGlobal");
            Object g = wmg.getMethod("getInstance").invoke(null);
            Field fv = wmg.getDeclaredField("mViews");
            fv.setAccessible(true);
            List<View> views;
            synchronized (g) {
                @SuppressWarnings("unchecked") List<View> v = (List<View>) fv.get(g);
                views = new ArrayList<>(v);
            }
            for (View v : views) watch(v);
        } catch (Throwable t) {
            Log.e(TAG, "cannot list this process's windows", t);
        }
    }

    private static void watch(View root) {
        if (!started || root == null || roots.containsKey(root)) return;
        roots.put(root, Boolean.TRUE);
        root.getViewTreeObserver().addOnGlobalLayoutListener(() -> check(root));
        root.addOnAttachStateChangeListener(new View.OnAttachStateChangeListener() {
            @Override
            public void onViewAttachedToWindow(View v) {
                v.getViewTreeObserver().addOnGlobalLayoutListener(() -> check(v));
                check(v);
            }

            @Override
            public void onViewDetachedFromWindow(View v) {}
        });
        check(root);
    }

    private static boolean ids(View any) {
        if (idHeader != 0) return true;
        android.content.res.Resources r = any.getResources();
        idHeader = r.getIdentifier("header_view", "id", GH);
        idContent = r.getIdentifier("content_container", "id", GH);
        idList = r.getIdentifier("tuned_recycler_view", "id", GH);
        if (idHeader == 0 || idContent == 0) {
            Log.w(TAG, "Android Auto has no id/header_view or id/content_container; browse stays stock");
            idHeader = -1;
            return false;
        }
        return true;
    }

    private static boolean debug() {
        return "1".equals(CardGlass.propOf("persist.sys.glass.auto.browse.debug", "0"));
    }

    private static boolean enabled() {
        return CardGlass.gateOpen() && !"0".equals(CardGlass.propOf("persist.sys.glass.auto.browse", "1"));
    }

    /** After a layout pass of a window: float the header of a browse screen, or put it back. */
    private static void check(View root) {
        if (!started) return;
        try {
            if (!ids(root) || idHeader < 0) return;
            View header = root.findViewById(idHeader);
            Floated f = null;
            if (header != null && header.getParent() instanceof LinearLayout) {
                LinearLayout column = (LinearLayout) header.getParent();
                View content = column.findViewById(idContent);
                if (content != null && content.getParent() == column) {
                    f = columns.get(column);
                    if (f == null || f.header.get() != header || f.content.get() != content) {
                        if (f != null) f.restore();
                        f = new Floated(column, header, content);
                        columns.put(column, f);
                    }
                }
            }
            // every column of this window: float it, or put it back
            for (Floated c : columns.values().toArray(new Floated[0])) {
                if (c == null || c.column.get() == null || c.column.get().getRootView() != root) continue;
                View h = c.header.get();
                boolean want = enabled() && h != null && h.getVisibility() == View.VISIBLE && h.isAttachedToWindow();
                if (want) c.apply();
                else c.restore();
            }
        } catch (Throwable t) {
            Log.e(TAG, "browse layout failed; putting it back", t);
            for (Floated c : columns.values().toArray(new Floated[0])) if (c != null) c.restore();
        }
    }

    private static Method scrollOffsetMethod;

    /** View.computeVerticalScrollOffset (protected; RecyclerView makes it public and exact enough). */
    static int scrollOffset(View v) {
        try {
            if (scrollOffsetMethod == null) {
                scrollOffsetMethod = View.class.getDeclaredMethod("computeVerticalScrollOffset");
                scrollOffsetMethod.setAccessible(true);
            }
            return (Integer) scrollOffsetMethod.invoke(v);
        } catch (Throwable t) {
            return -1;
        }
    }

    static RenderNode renderNodeOf(View v) {
        try {
            if (renderNodeField == null) {
                renderNodeField = View.class.getDeclaredField("mRenderNode");
                renderNodeField.setAccessible(true);
            }
            return (RenderNode) renderNodeField.get(v);
        } catch (Throwable t) {
            return null;
        }
    }

    /** One browse screen's header and content, floated or as they were. */
    private static final class Floated {
        final WeakReference<LinearLayout> column;
        final WeakReference<View> header, content;
        /** Per list frame: {its own top padding, ours, times Android Auto reset it}. */
        final WeakHashMap<View, int[]> paddedLists = new WeakHashMap<>();
        boolean toldNoList;
        String lastState;
        boolean applied;
        LivePlate plate;
        // as they were
        int hWidth, hGravity, hTop, cTop;
        Drawable hBackground;
        ViewOutlineProvider hOutline;
        boolean hClip;
        float hZ;
        int hAmbient, hSpot;

        Floated(LinearLayout column, View header, View content) {
            this.column = new WeakReference<>(column);
            this.header = new WeakReference<>(header);
            this.content = new WeakReference<>(content);
        }

        void apply() {
            LinearLayout col = column.get();
            View h = header.get(), c = content.get();
            if (col == null || h == null || c == null) return;
            float dp = h.getResources().getDisplayMetrics().density;
            int gap = Math.round(8 * dp);
            int hh = h.getHeight() > 0 ? h.getHeight() : Math.round(96 * dp);
            LinearLayout.LayoutParams hl = (LinearLayout.LayoutParams) h.getLayoutParams();
            LinearLayout.LayoutParams cl = (LinearLayout.LayoutParams) c.getLayoutParams();
            if (!applied) {
                hWidth = hl.width;
                hGravity = hl.gravity;
                hTop = hl.topMargin;
                cTop = cl.topMargin;
                hBackground = h.getBackground();
                hOutline = h.getOutlineProvider();
                hClip = h.getClipToOutline();
                hZ = h.getTranslationZ();
                hAmbient = h.getOutlineAmbientShadowColor();
                hSpot = h.getOutlineSpotShadowColor();
                plate = new LivePlate(h, c);
                h.setBackground(plate);
                h.setOutlineProvider(new ViewOutlineProvider() {
                    @Override
                    public void getOutline(View v, Outline o) {
                        o.setRoundRect(0, 0, v.getWidth(), v.getHeight(), v.getHeight() / 2f);
                    }
                });
                h.setClipToOutline(true);
                h.setOutlineAmbientShadowColor(Color.TRANSPARENT);
                h.setOutlineSpotShadowColor(Color.TRANSPARENT);
                h.setTranslationZ(1f);
                applied = true;
                Log.i(TAG, "floating the tabs of a browse screen (" + col.getWidth() + "x" + col.getHeight() + ")");
            }
            int width = pillWidth(h, col.getWidth(), dp);
            boolean relayout = false;
            if (hl.width != width || hl.gravity != Gravity.CENTER_HORIZONTAL || hl.topMargin != gap) {
                hl.width = width;
                hl.gravity = Gravity.CENTER_HORIZONTAL;
                hl.topMargin = gap;
                h.setLayoutParams(hl);
                relayout = true;
            }
            int ct = -(hh + gap);
            if (cl.topMargin != ct) {
                cl.topMargin = ct;
                c.setLayoutParams(cl);
                relayout = true;
            }
            // The list's own top padding does not hold: Android Auto's paged
            // list lays its first row out at its top whatever the padding, as
            // soon as new rows arrive. So the list stays as it is and its parent
            // (MaxWidthLayout) is padded: the list sits below the capsule, as
            // tall as stock (paging unchanged), and with clipping off on the way
            // up a row that moves above the list's top still draws, under the
            // capsule, until it has left the list.
            View list = idList != 0 ? c.findViewById(idList) : null;
            if (list == null || !(list.getParent() instanceof ViewGroup)) {
                if (!toldNoList) Log.i(TAG, "no id/tuned_recycler_view in this browse screen (yet)");
                toldNoList = true;
            } else {
                ViewGroup frame = (ViewGroup) list.getParent();
                int[] st = paddedLists.get(frame);
                int want = hh + gap;
                if (st == null || frame.getPaddingTop() != st[1]) {
                    if (st == null) st = new int[3];
                    else st[2]++;
                    if (st[2] <= 20) {
                        st[0] = frame.getPaddingTop();
                        st[1] = st[0] + want;
                        paddedLists.put(frame, st);
                        frame.setPadding(frame.getPaddingLeft(), st[1], frame.getPaddingRight(), frame.getPaddingBottom());
                        frame.setClipToPadding(false);
                        frame.setClipChildren(false);
                        if (list instanceof ViewGroup) {
                            ((ViewGroup) list).setClipToPadding(false);
                            ((ViewGroup) list).setClipChildren(false);
                        }
                        Log.i(TAG, frame.getClass().getSimpleName() + " top padding " + st[0] + " -> " + st[1]
                                + (st[2] > 0 ? " (Android Auto had reset it, " + st[2] + "x)" : ""));
                        relayout = true;
                    } else if (st[2] == 21) {
                        st[2]++;
                        Log.w(TAG, "Android Auto keeps resetting the list frame's padding; leaving it");
                    }
                }
                if (debug() && list instanceof ViewGroup) {
                    ViewGroup g = (ViewGroup) list;
                    String now = "rows=" + g.getChildCount() + " firstTop=" + (g.getChildCount() > 0 ? g.getChildAt(0).getTop() : -1)
                            + " listTop=" + list.getTop() + " offset=" + scrollOffset(list) + " canUp=" + list.canScrollVertically(-1);
                    if (!now.equals(lastState)) {
                        lastState = now;
                        Log.i(TAG, "list " + now);
                    }
                }
            }
            if (relayout) col.requestLayout();
        }

        void restore() {
            if (!applied) return;
            applied = false;
            View h = header.get(), c = content.get();
            if (h != null) {
                LinearLayout.LayoutParams hl = (LinearLayout.LayoutParams) h.getLayoutParams();
                hl.width = hWidth;
                hl.gravity = hGravity;
                hl.topMargin = hTop;
                h.setLayoutParams(hl);
                if (h.getBackground() == plate) h.setBackground(hBackground);
                h.setOutlineProvider(hOutline);
                h.setClipToOutline(hClip);
                h.setTranslationZ(hZ);
                h.setOutlineAmbientShadowColor(hAmbient);
                h.setOutlineSpotShadowColor(hSpot);
            }
            if (c != null) {
                LinearLayout.LayoutParams cl = (LinearLayout.LayoutParams) c.getLayoutParams();
                cl.topMargin = cTop;
                c.setLayoutParams(cl);
            }
            for (java.util.Map.Entry<View, int[]> e : paddedLists.entrySet()) {
                View frame = e.getKey();
                if (frame == null || frame.getPaddingTop() != e.getValue()[1]) continue;
                frame.setPadding(frame.getPaddingLeft(), e.getValue()[0], frame.getPaddingRight(), frame.getPaddingBottom());
            }
            paddedLists.clear();
            plate = null;
            LinearLayout col = column.get();
            if (col != null) col.requestLayout();
            Log.i(TAG, "browse tabs put back");
        }
    }

    /**
     * Wide enough for the header's leading action and its tabs at about 120 dp
     * each, at most the column less 88 dp a side (clear of the page arrows).
     */
    private static int pillWidth(View header, int columnWidth, float dp) {
        int tabs = 4;
        View tl = find(header, "com.google.android.material.tabs.TabLayout");
        if (tl instanceof ViewGroup && ((ViewGroup) tl).getChildCount() > 0
                && ((ViewGroup) tl).getChildAt(0) instanceof ViewGroup) {
            tabs = Math.max(1, ((ViewGroup) ((ViewGroup) tl).getChildAt(0)).getChildCount());
        }
        int want = Math.round((88 + tabs * 120 + 16) * dp);
        int max = columnWidth - Math.round(2 * 88 * dp);
        return Math.max(Math.round(200 * dp), Math.min(want, max > 0 ? max : want));
    }

    private static View find(View v, String className) {
        for (Class<?> c = v.getClass(); c != null && c != View.class; c = c.getSuperclass()) {
            if (c.getName().equals(className)) return v;
        }
        if (v instanceof ViewGroup) {
            ViewGroup g = (ViewGroup) v;
            for (int i = 0; i < g.getChildCount(); ++i) {
                View f = find(g.getChildAt(i), className);
                if (f != null) return f;
            }
        }
        return null;
    }

    /**
     * The capsule's background: glass over the content below it, drawn from the
     * content's own RenderNode (see the class comment).
     */
    static final class LivePlate extends Drawable {
        private final WeakReference<View> header, content;
        private final RenderNode backdrop = new RenderNode("SGBrowseBackdrop");
        private RuntimeShader shader;
        private int effectW = -1, effectH = -1;
        final Lens.Look look = new Lens.Look();

        LivePlate(View header, View content) {
            this.header = new WeakReference<>(header);
            this.content = new WeakReference<>(content);
            // A large pane over a dark, busy list: lift the face a little more
            // than the card's buttons, as a CarPlay tab bar reads.
            look.tintR = 0.20f;
            look.tintG = 0.20f;
            look.tintB = 0.22f;
            look.tintA = 0.35f;
            look.toneBias = 0.06f;
            look.highlightIntensity = 0.8f;
        }

        @Override
        public void draw(Canvas canvas) {
            Rect b = getBounds();
            if (!(canvas instanceof RecordingCanvas) || b.isEmpty()) {
                canvas.drawColor(0xcc33363a);
                return;
            }
            if (!recordBackdrop()) {
                canvas.drawColor(0xcc33363a);
                return;
            }
            busy = true;
            try {
                ((RecordingCanvas) canvas).drawRenderNode(backdrop);
            } finally {
                busy = false;
            }
        }

        /** Records the backdrop: the window's colour and the content's RenderNode, under the capsule. */
        boolean recordBackdrop() {
            View h = header.get(), c = content.get();
            Rect b = getBounds();
            if (h == null || c == null || b.isEmpty()) return false;
            RenderNode cn = renderNodeOf(c);
            if (cn == null) return false;
            int w = b.width(), ht = b.height();
            busy = true;
            try {
                backdrop.setPosition(0, 0, w, ht);
                if (w != effectW || ht != effectH) {
                    backdrop.setRenderEffect(effect(w, ht, h.getResources().getDisplayMetrics().density));
                    effectW = w;
                    effectH = ht;
                }
                RecordingCanvas rc = backdrop.beginRecording(w, ht);
                try {
                    rc.drawColor(windowColour(h));
                    // The content's node draws at its own left/top in the
                    // column; the header is at its left/top in the same column.
                    rc.translate(-h.getLeft(), -h.getTop());
                    rc.drawRenderNode(cn);
                } finally {
                    backdrop.endRecording();
                }
            } finally {
                busy = false;
            }
            return true;
        }

        private RenderEffect effect(int w, int h, float dp) {
            if (shader == null) {
                // One backdrop only: the engine's shader takes the screen and its
                // blur; a RenderEffect gives a shader one input, so both read it.
                String src = GlassSource.LEAN
                        .replace("uniform shader sharpContent;", "")
                        .replace("sharpContent.eval(", "blurredContent.eval(");
                shader = new RuntimeShader(src);
            }
            Lens.uniforms(shader, look, w, h, h / 2f, dp);
            RenderEffect blur = RenderEffect.createBlurEffect(18 * dp, 18 * dp, Shader.TileMode.CLAMP);
            RenderEffect glass = RenderEffect.createRuntimeShaderEffect(shader, "blurredContent");
            return RenderEffect.createChainEffect(glass, blur);
        }

        private static int windowColour(View h) {
            Drawable d = h.getRootView().getBackground();
            if (d instanceof ColorDrawable && Color.alpha(((ColorDrawable) d).getColor()) == 255) {
                return ((ColorDrawable) d).getColor();
            }
            return WINDOW_BG;
        }

        @Override
        public void setAlpha(int alpha) {}

        @Override
        public void setColorFilter(ColorFilter f) {}

        @Override
        public int getOpacity() {
            return PixelFormat.TRANSLUCENT;
        }
    }
}
