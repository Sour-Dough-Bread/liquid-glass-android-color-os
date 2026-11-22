package dev.stockglass.auto.lists.glass;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Matrix;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.RecordingCanvas;
import android.graphics.RenderEffect;
import android.graphics.RenderNode;
import android.graphics.Shader;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;
import android.view.Display;
import android.view.View;
import android.view.inspector.WindowInspector;
import de.robv.android.xposed.XC_MethodHook.MethodHookParam;
import java.lang.ref.WeakReference;
import java.lang.reflect.Member;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Map;
import java.util.WeakHashMap;
import java.util.function.BiConsumer;

/**
 * Liquid Glass under the buttons of Android Auto's media screens: the
 * dashboard media card (as auto/media does) and the full-screen now-playing
 * screen of the media template, whose artwork lies deeper.
 *
 * This is auto/media's CardGlass, generalised. There the artwork's layer is
 * the direct child of the layer the buttons are drawn in, and the two are
 * recorded nested. The now-playing screen (Android Auto 18.0, Compose)
 * records, in its 770x480 content layer P:
 *
 *   node  placeholder (a flat rect; fades out once the art is in)
 *   node  backdrop (alpha fades in) > node (600x480, scaled to fill)
 *         > node with a RenderEffect (the blur) > node (600x480, clipped)
 *         > drawBitmap 600x600 artwork, cropped
 *   rect  scrim over all of P (0xbf242629)
 *   rrect + icon node per control: the play/pause pill (filled), back,
 *         previous, next and the custom actions (transparent circles), the
 *         Queue pill (transparent fill plus a STROKE outline)
 *
 * and Compose re-records a changed layer on its own, outside its parent, so
 * the stack of layers being recorded does not show the art's ancestry. So:
 *
 *   - RecordingCanvas.drawRenderNode (before-hook) keeps a map child -> parent,
 *     with the parent's canvas matrix, for every layer drawn anywhere.
 *   - RenderNode.setRenderEffect and RenderEffect.createBlurEffect note which
 *     layers blur their content, and by how much.
 *   - drawBitmap: a colour bitmap that fills a layer of at least 160 px each
 *     way is artwork (as before).
 *   - drawRect: a translucent rect over a whole layer is that layer's scrim.
 *   - drawRoundRect (FILL only) of a pill or circle in a layer P: walk up from
 *     each artwork's layer through the parent map; if P is reached, compose
 *     the transforms on the way (each node's position, property matrix and
 *     its parent's canvas matrix) into artwork px -> P, and if the button is
 *     inside the artwork's area there, the slot (a RenderNode of ours running
 *     the engine's lean shader) is drawn into P before the button, as in
 *     auto/media. A blur met on the way is applied to the backdrop, and P's
 *     scrim (or the art layer's own) is laid over it.
 */
public final class CardGlass {
    static final String TAG = "SGAutoLists";
    /** A layer this big each way, filled by a colour bitmap, is artwork. */
    private static final int MIN_ART_PX = 160;
    /** How far up from an artwork's layer a button's layer may be. */
    private static final int MAX_DEPTH = 10;

    private static Context ctx;
    private static String process;
    private static Handler main;
    private static HandlerThread worker;
    private static Handler work;
    private static volatile boolean running;
    private static BiConsumer<Member, String> hooks;
    private static boolean traceHooked;

    private static final ThreadLocal<ArrayList<RenderNode>> stack = ThreadLocal.withInitial(ArrayList::new);
    /** An artwork's own layer -> the artwork. */
    private static final WeakHashMap<RenderNode, Art> arts = new WeakHashMap<>();
    /** child layer -> where it was last drawn. */
    private static final WeakHashMap<RenderNode, Edge> edges = new WeakHashMap<>();
    /** layer -> blur radius of its RenderEffect, in its own px. */
    private static final WeakHashMap<RenderNode, Float> effects = new WeakHashMap<>();
    /** RenderEffect -> its blur radius (from createBlurEffect). */
    private static final WeakHashMap<RenderEffect, Float> blurOf = new WeakHashMap<>();
    private static final ThreadLocal<float[]> pendingBlur = new ThreadLocal<>();
    /** layer -> the colour of a translucent rect over all of it. */
    private static final WeakHashMap<RenderNode, Integer> scrims = new WeakHashMap<>();
    /** A button's layer -> its buttons' slots, by place and size. */
    private static final WeakHashMap<RenderNode, Map<String, Slot>> slots = new WeakHashMap<>();
    private static final Map<Integer, Paint> fills = new HashMap<>();

    static final Lens.Look look = new Lens.Look();
    private static int lookVersion;
    /** How much of the scrim the glass sees (Backdrop), over sharp artwork (the dashboard card). */
    static float scrimThrough = 0.5f;
    /**
     * The same over artwork the screen blurs (the now-playing backdrop):
     * there the scrim is most of what is seen, and glass that sees much less
     * of it than its surroundings reads as a candy-coloured button.
     */
    static float blurredThrough = 0.9f;
    /** Box radius of the backdrop's blur, in artwork px. */
    static float blurPx = 12f;
    /** Alpha kept on a filled button background (the play/pause pill). */
    static float fillKeep = 0.4f;
    /** How strongly a filled button's glass is tinted with its fill colour. */
    static float tintKeep = 0.5f;
    /** Multiplies the blur found on the way (1: as Android Auto blurs it). */
    static float effectScale = 1f;
    /** Dev override of the switches: null follows the properties. */
    private static Boolean forced;
    private static float dp = -1f;

    private static Method sysGet;
    private static long gateAt;
    private static boolean gate;
    private static long hookNs, hookCalls, slotDraws;

    private CardGlass() {}

    static final class Edge {
        WeakReference<RenderNode> parent;
        final Matrix m = new Matrix();
    }

    static final class Art {
        WeakReference<RenderNode> layer;
        Bitmap bitmap;
        final Rect src = new Rect();
        final Rect dst = new Rect();
        /** The art layer's canvas matrix when the bitmap was drawn. */
        final Matrix m = new Matrix();
        /** A scrim drawn over the artwork in its own layer (the dashboard card's). */
        int ownScrim;
        /** What the backdrop is made with: scrim, and pre-blur in artwork px. */
        int scrim;
        float preBlur;
        /** The blur found on the way to the buttons, before effectScale. */
        float rawBlur;
        float through = -1f;
        Backdrop backdrop;
        boolean building;
    }

    /** An artwork found under a button's layer. */
    static final class Found {
        Art art;
        final Matrix toLayer = new Matrix();
        final RectF area = new RectF();
        float preBlur;
    }

    static final class Slot {
        final RenderNode node = new RenderNode("StockGlassLens");
        /** Slot coordinates -> the button layer's. */
        final Matrix toCard = new Matrix();
        /** Artwork px -> the button layer's. */
        final Matrix artToCard = new Matrix();
        float w, h, radius;
        int fill;
        Art art;
        /** Where it last was, in the button layer's coordinates. */
        final RectF onCard = new RectF();
        Backdrop recorded;
        int recordedLook = -1;
        float recordedW, recordedH;
        int recordedFill;
    }

    // ------------------------------------------------------------ lifecycle

    public static void start(Context c, ClassLoader app, BiConsumer<Member, String> hooker, String proc) {
        ctx = c;
        process = proc;
        hooks = hooker;
        main = new Handler(Looper.getMainLooper());
        worker = new HandlerThread("SGAutoLists-backdrop");
        worker.start();
        work = new Handler(worker.getLooper());
        String bad = Lens.check();
        if (bad != null) {
            Log.e(TAG, "the engine's shader does not compile here; media screens stay stock: " + bad);
            return;
        }
        try {
            sysGet = Class.forName("android.os.SystemProperties").getMethod("get", String.class, String.class);
        } catch (Throwable t) {
            sysGet = null;
        }
        try {
            // Public API only: RecordingCanvas's draw methods are the public
            // final ones of its hidden superclass BaseRecordingCanvas, which
            // getMethod returns without naming that class.
            Class<?> rc = RecordingCanvas.class;
            hooker.accept(RenderNode.class.getMethod("beginRecording", int.class, int.class), "begin");
            hooker.accept(RenderNode.class.getMethod("endRecording"), "end");
            hooker.accept(rc.getMethod("drawBitmap", Bitmap.class, Rect.class, Rect.class, Paint.class), "bitmap");
            hooker.accept(rc.getMethod("drawRect", float.class, float.class, float.class, float.class,
                    Paint.class), "rect");
            hooker.accept(rc.getMethod("drawRoundRect", float.class, float.class, float.class, float.class,
                    float.class, float.class, Paint.class), "rrect");
            hooker.accept(rc.getMethod("drawRenderNode", RenderNode.class), "node");
            hooker.accept(RenderNode.class.getMethod("setRenderEffect", RenderEffect.class), "effect");
            hooker.accept(RenderEffect.class.getMethod("createBlurEffect", float.class, float.class,
                    Shader.TileMode.class), "blur");
            hooker.accept(RenderEffect.class.getMethod("createBlurEffect", float.class, float.class,
                    RenderEffect.class, Shader.TileMode.class), "blur2");
        } catch (Throwable t) {
            Log.e(TAG, "no RecordingCanvas of the expected shape; media screens stay stock", t);
            return;
        }
        running = true;
        Log.i(TAG, "media glass ready in " + proc);
        Browse.start(hooker, main);
    }

    public static void stop() {
        running = false;
        Browse.stop();
        // Empty every slot, so nothing of this build is drawn once it is gone.
        for (Map<String, Slot> m : slots.values()) {
            for (Slot s : m.values()) {
                s.node.beginRecording(1, 1);
                s.node.endRecording();
            }
        }
        invalidateAll();
        if (worker != null) worker.quitSafely();
        Log.i(TAG, "media glass stopped");
    }

    // --------------------------------------------------------------- hooks

    public static void before(String key, Object param) {
        if (!running || Browse.busy) return;
        MethodHookParam p = (MethodHookParam) param;
        if ("addview".equals(key)) {
            Browse.onAddView(p);
            return;
        }
        if (Trace.armed() && !"begin".equals(key)) Trace.on(key, p, stack.get());
        switch (key) {
            case "begin":
                stack.get().add((RenderNode) p.thisObject);
                if (Trace.armed()) Trace.on(key, p, stack.get());
                return;
            case "end": {
                Browse.onRecorded((RenderNode) p.thisObject);
                ArrayList<RenderNode> s = stack.get();
                for (int i = s.size() - 1; i >= 0; i--) {
                    if (s.get(i) == p.thisObject) {
                        while (s.size() > i) s.remove(s.size() - 1);
                        break;
                    }
                }
                return;
            }
            case "path": case "circle": case "text": case "bitmapM": case "bitmapXY":
                return; // trace only
            default:
                break;
        }
        long t0 = SystemClock.elapsedRealtimeNanos();
        try {
            switch (key) {
                case "bitmap":
                    onBitmap(p);
                    break;
                case "rect":
                    onRect(p);
                    break;
                case "rrect":
                    onRoundRect(p);
                    break;
                case "node":
                    onNode(p);
                    break;
                case "effect":
                    onEffect(p);
                    break;
                case "blur":
                case "blur2":
                    pendingBlur.set(new float[] {(float) p.args[0], (float) p.args[1]});
                    break;
                default:
                    break;
            }
        } catch (Throwable t) {
            once("hook-" + key, t);
        } finally {
            hookNs += SystemClock.elapsedRealtimeNanos() - t0;
            hookCalls++;
        }
    }

    private static RenderNode top(int down) {
        ArrayList<RenderNode> s = stack.get();
        int i = s.size() - 1 - down;
        return i >= 0 ? s.get(i) : null;
    }

    /** A child layer drawn into the layer being recorded: remember where. */
    private static void onNode(MethodHookParam p) {
        RenderNode child = (RenderNode) p.args[0];
        RenderNode parent = top(0);
        if (child == null || parent == null || child == parent) return;
        Edge e = edges.get(child);
        if (e == null) {
            e = new Edge();
            edges.put(child, e);
        }
        if (e.parent == null || e.parent.get() != parent) e.parent = new WeakReference<>(parent);
        ((Canvas) p.thisObject).getMatrix(e.m);
    }

    /**
     * A layer gets a RenderEffect. Its blur radius is the one createBlurEffect
     * was last asked for on this thread when the effect is new (Compose makes
     * the effect just before it sets it, and keeps it).
     */
    private static void onEffect(MethodHookParam p) {
        RenderNode n = (RenderNode) p.thisObject;
        RenderEffect e = (RenderEffect) p.args[0];
        if (e == null) {
            effects.remove(n);
            return;
        }
        Float r = blurOf.get(e);
        if (r == null) {
            float[] pb = pendingBlur.get();
            if (pb == null) return;
            r = Math.max(pb[0], pb[1]);
            blurOf.put(e, r);
            pendingBlur.remove();
        }
        Float had = effects.put(n, r);
        if (had == null || !had.equals(r)) Log.i(TAG, "blur " + r + " px on a " + n.getWidth() + "x" + n.getHeight() + " layer");
    }

    private static void onBitmap(MethodHookParam p) {
        Bitmap b = (Bitmap) p.args[0];
        Rect dst = (Rect) p.args[2];
        if (b == null || dst == null || b.getConfig() == Bitmap.Config.ALPHA_8) return;
        RenderNode layer = top(0);
        if (layer == null) return;
        int lw = layer.getWidth(), lh = layer.getHeight();
        if (lw < MIN_ART_PX || lh < MIN_ART_PX) return;
        Canvas c = (Canvas) p.thisObject;
        Matrix m = new Matrix();
        c.getMatrix(m);
        RectF r = new RectF(dst);
        m.mapRect(r);
        // fills the layer (a cropped or exactly fitted artwork)
        if (r.left > 0.5f || r.top > 0.5f || r.right < lw - 0.5f || r.bottom < lh - 0.5f) return;
        if (!gate()) return;
        Art a = arts.get(layer);
        if (a == null) {
            a = new Art();
            arts.put(layer, a);
            Log.i(TAG, "artwork " + b.getWidth() + "x" + b.getHeight() + " in a " + lw + "x" + lh + " layer");
        }
        a.layer = new WeakReference<>(layer);
        if (a.bitmap != b) a.ownScrim = 0;
        a.bitmap = b;
        Rect src = (Rect) p.args[1];
        if (src != null) a.src.set(src);
        else a.src.set(0, 0, b.getWidth(), b.getHeight());
        a.dst.set(dst);
        a.m.set(m);
    }

    private static void onRect(MethodHookParam p) {
        RenderNode layer = top(0);
        if (layer == null) return;
        Paint paint = (Paint) p.args[4];
        if (paint == null || paint.getShader() != null || paint.getStyle() != Paint.Style.FILL) return;
        int alpha = Color.alpha(paint.getColor());
        if (alpha == 0 || alpha == 255) return;
        float l = (float) p.args[0], t = (float) p.args[1], r = (float) p.args[2], b = (float) p.args[3];
        if (r - l < layer.getWidth() * 0.9f || b - t < layer.getHeight() * 0.9f) return;
        scrims.put(layer, paint.getColor());
        Art a = arts.get(layer);
        if (a != null) a.ownScrim = paint.getColor();
    }

    /** The artwork, if any, under {@code layer} at {@code inLayer} (a button's rect there). */
    private static Found find(RenderNode layer, RectF inLayer) {
        float cx = inLayer.centerX(), cy = inLayer.centerY();
        Matrix nm = new Matrix();
        for (Map.Entry<RenderNode, Art> e : arts.entrySet()) {
            Art a = e.getValue();
            RenderNode n = e.getKey();
            if (a.bitmap == null || a.src.isEmpty()) continue;
            Found f = new Found();
            f.toLayer.setRectToRect(new RectF(a.src), new RectF(a.dst), Matrix.ScaleToFit.FILL);
            f.toLayer.postConcat(a.m);
            // the artwork's visible area: the bitmap, cropped to its layer
            RectF area = new RectF(a.src);
            f.toLayer.mapRect(area);
            if (!area.intersect(0, 0, n.getWidth(), n.getHeight())) continue;
            float blur = 0f;
            int depth = 0;
            while (n != layer && depth++ < MAX_DEPTH) {
                Float r = effects.get(n);
                if (r != null && r > 0f) {
                    // the radius is in n's px; in artwork px it is that over the scale so far
                    float s = Math.max(1e-3f, f.toLayer.mapRadius(1f));
                    blur = Math.max(blur, r / s);
                }
                Edge ed = edges.get(n);
                RenderNode parent = ed == null || ed.parent == null ? null : ed.parent.get();
                if (parent == null) break;
                n.getMatrix(nm);
                nm.postTranslate(n.getLeft(), n.getTop());
                nm.postConcat(ed.m);
                f.toLayer.postConcat(nm);
                nm.mapRect(area);
                n = parent;
            }
            if (n != layer) continue;
            if (!area.contains(cx, cy)) continue;
            f.art = a;
            f.area.set(area);
            f.preBlur = blur;
            return f;
        }
        return null;
    }

    private static void onRoundRect(MethodHookParam p) {
        RenderNode card = top(0);
        if (card == null || arts.isEmpty()) return;
        Paint paint = (Paint) p.args[6];
        if (paint != null && paint.getStyle() != Paint.Style.FILL) return; // an outline, e.g. the Queue pill's
        float l = (float) p.args[0], t = (float) p.args[1], r = (float) p.args[2], b = (float) p.args[3];
        float rx = (float) p.args[4];
        float w = r - l, h = b - t;
        // a pill or a circle, button-sized
        if (Math.min(w, h) < 24f || rx < 0.45f * Math.min(w, h)) return;
        Canvas c = (Canvas) p.thisObject;
        Matrix m = new Matrix();
        c.getMatrix(m);
        RectF onCard = new RectF(l, t, r, b);
        m.mapRect(onCard);
        Found f = find(card, onCard);
        if (f == null) return;
        if (onCard.width() * onCard.height() > 0.25f * f.area.width() * f.area.height()) return;
        if (!gate()) return;
        Art a = f.art;
        int scrim = scrims.containsKey(card) ? scrims.get(card) : a.ownScrim;
        float pre = f.preBlur * effectScale;
        float through = f.preBlur > 0f ? blurredThrough : scrimThrough;
        a.rawBlur = f.preBlur;
        if (a.scrim != scrim || Math.abs(a.preBlur - pre) > 0.5f || a.through != through) {
            a.scrim = scrim;
            a.preBlur = pre;
            a.through = through;
            refreshBackdrop(a);
        } else if (a.backdrop == null || !a.backdrop.matches(a.bitmap, a.scrim, a.through, a.preBlur)) {
            refreshBackdrop(a);
        }
        Map<String, Slot> mine = slots.computeIfAbsent(card, k -> new HashMap<>());
        String key = Math.round(onCard.left) + "," + Math.round(onCard.top) + " " + Math.round(w) + "x" + Math.round(h);
        Slot s = mine.get(key);
        if (s == null) {
            // The same button with another size (the pill morphs between
            // pause and play): reuse its slot, so a morph neither piles up
            // slots nor drops the glass for a frame.
            String near = null;
            for (Map.Entry<String, Slot> e : mine.entrySet()) {
                if (overlap(e.getValue().onCard, onCard) > 0.5f) {
                    near = e.getKey();
                    break;
                }
            }
            if (near != null) {
                s = mine.remove(near);
            } else {
                s = new Slot();
                Log.i(TAG, "button at " + key + " r=" + rx + " over " + a.bitmap.getWidth() + "x"
                        + a.bitmap.getHeight() + " artwork in a " + card.getWidth() + "x" + card.getHeight()
                        + " layer, blur " + pre + String.format(" scrim %08x", scrim));
            }
            mine.put(key, s);
        }
        s.onCard.set(onCard);
        int fill = paint == null ? 0 : paint.getColor();
        s.toCard.set(m);
        s.toCard.preTranslate(l, t);
        s.artToCard.set(f.toLayer);
        s.w = w;
        s.h = h;
        s.radius = rx;
        s.fill = fill;
        s.art = a;
        s.node.setPosition(Math.round(l), Math.round(t), Math.round(l) + Math.round(w), Math.round(t) + Math.round(h));
        record(s);
        ((RecordingCanvas) c).drawRenderNode(s.node);
        slotDraws++;
        if (paint != null && Color.alpha(fill) > 0 && fillKeep < 1f) {
            p.args[6] = translucent(paint);
        }
    }

    /** Intersection over the smaller of the two areas. */
    private static float overlap(RectF x, RectF y) {
        RectF i = new RectF();
        if (!i.setIntersect(x, y)) return 0f;
        float small = Math.min(x.width() * x.height(), y.width() * y.height());
        return small <= 0f ? 0f : i.width() * i.height() / small;
    }

    private static Paint translucent(Paint paint) {
        int key = paint.getColor() ^ Float.floatToIntBits(fillKeep);
        Paint q = fills.get(key);
        if (q == null) {
            if (fills.size() > 64) fills.clear();
            q = new Paint(paint);
            q.setAlpha(Math.round(paint.getAlpha() * fillKeep));
            fills.put(key, q);
        }
        return q;
    }

    /** The trace's own hooks, once (see Trace). */
    private static void hookTrace() {
        if (traceHooked || hooks == null) return;
        traceHooked = true;
        try {
            Class<?> rc = RecordingCanvas.class;
            hooks.accept(rc.getMethod("drawPath", android.graphics.Path.class, Paint.class), "path");
            hooks.accept(rc.getMethod("drawCircle", float.class, float.class, float.class, Paint.class), "circle");
            hooks.accept(rc.getMethod("drawTextRun", CharSequence.class, int.class, int.class, int.class, int.class,
                    float.class, float.class, boolean.class, Paint.class), "text");
            hooks.accept(rc.getMethod("drawBitmap", Bitmap.class, Matrix.class, Paint.class), "bitmapM");
            hooks.accept(rc.getMethod("drawBitmap", Bitmap.class, float.class, float.class, Paint.class), "bitmapXY");
        } catch (Throwable t) {
            once("trace-hooks", t);
        }
    }

    // ---------------------------------------------------------------- glass

    private static void record(Slot s) {
        Art a = s.art;
        Backdrop bd = a == null ? null : a.backdrop;
        if (bd != null && !bd.matches(a.bitmap, a.scrim, a.through, a.preBlur)) bd = null;
        // hasDisplayList: when a slot drops out of every display list (the
        // card was recorded once without it, e.g. while the pill morphs
        // between pause and play), RenderThread destroys its display list,
        // and a slot drawn again later would be empty.
        if (s.recorded == bd && s.recordedLook == lookVersion && s.recordedW == s.w && s.recordedH == s.h
                && s.recordedFill == s.fill && bd != null && s.node.hasDisplayList()) {
            return;
        }
        boolean drew = false;
        RecordingCanvas c = s.node.beginRecording(Math.max(1, Math.round(s.w)), Math.max(1, Math.round(s.h)));
        try {
            if (bd != null && running && gate()) {
                Matrix inv = new Matrix();
                if (s.artToCard.invert(inv)) {
                    Matrix slotToArt = new Matrix(s.toCard);
                    slotToArt.postConcat(inv);
                    Lens.Look k = look;
                    if (Color.alpha(s.fill) > 0 && tintKeep > 0f) {
                        k = tinted(s.fill);
                    }
                    c.drawRect(0, 0, s.w, s.h, Lens.paint(k, s.w, s.h, s.radius, density(), bd, slotToArt));
                    drew = true;
                }
            }
        } finally {
            s.node.endRecording();
        }
        // An empty slot (switched off, no backdrop yet) is never up to date.
        s.recorded = drew ? bd : null;
        s.recordedLook = lookVersion;
        s.recordedW = s.w;
        s.recordedH = s.h;
        s.recordedFill = s.fill;
    }

    /** The look, tinted with a filled button's own colour (as the engine tints an active QS tile). */
    private static Lens.Look tinted(int fill) {
        Lens.Look k = new Lens.Look();
        for (java.lang.reflect.Field f : Lens.Look.class.getDeclaredFields()) {
            try {
                f.setFloat(k, f.getFloat(look));
            } catch (Throwable ignored) {
            }
        }
        k.tintR = Color.red(fill) / 255f;
        k.tintG = Color.green(fill) / 255f;
        k.tintB = Color.blue(fill) / 255f;
        k.tintA = tintKeep;
        return k;
    }

    private static void refreshBackdrop(Art a) {
        Bitmap b = a.bitmap;
        if (b == null || a.building) return;
        if (a.backdrop != null && a.backdrop.matches(b, a.scrim, a.through, a.preBlur)) return;
        a.building = true;
        final int scrim = a.scrim;
        final float through = a.through, blur = blurPx, pre = a.preBlur;
        work.post(() -> {
            Backdrop made = null;
            try {
                made = Backdrop.make(b, scrim, through, blur, pre);
            } catch (Throwable t) {
                once("backdrop", t);
            }
            final Backdrop done = made;
            main.post(() -> {
                a.building = false;
                if (done == null) return;
                a.backdrop = done;
                Log.i(TAG, "backdrop ready: " + b.getWidth() + "x" + b.getHeight() + String.format(" scrim %08x", scrim)
                        + " pre-blur " + pre);
                rerecord();
                // the artwork, scrim or blur may have changed while this one was made
                if (!done.matches(a.bitmap, a.scrim, a.through, a.preBlur)) refreshBackdrop(a);
            });
        });
    }

    private static void rerecord() {
        for (Map<String, Slot> m : slots.values()) {
            for (Slot s : m.values()) record(s);
        }
        invalidateAll();
    }

    /** Any frame of a window syncs our slots; the button layers' own display lists are not re-recorded. */
    private static void invalidateAll() {
        try {
            for (View root : WindowInspector.getGlobalWindowViews()) root.invalidate();
        } catch (Throwable t) {
            once("invalidate", t);
        }
    }

    /** The car display's density: the first window on a display other than the phone's. */
    private static float density() {
        if (dp > 0f) return dp;
        float d = -1f;
        try {
            for (View root : WindowInspector.getGlobalWindowViews()) {
                Display disp = root.getDisplay();
                if (disp != null && disp.getDisplayId() != Display.DEFAULT_DISPLAY) {
                    d = root.getResources().getDisplayMetrics().density;
                    break;
                }
            }
        } catch (Throwable ignored) {
        }
        dp = d > 0f ? d : 1f;
        Log.i(TAG, "car density " + dp);
        return dp;
    }

    // ------------------------------------------------------------ switches

    /** The engine's switch (persist.sys.glass.mode glass or on) and this module's own (persist.sys.glass.auto.lists). */
    private static boolean gate() {
        if (forced != null) return forced;
        long now = SystemClock.uptimeMillis();
        if (now - gateAt < 500 && gateAt != 0) return gate;
        gateAt = now;
        String mode = prop("persist.sys.glass.mode", "");
        String own = prop("persist.sys.glass.auto.lists", "1");
        gate = ("glass".equals(mode) || "on".equals(mode)) && !"0".equals(own);
        return gate;
    }

    /** For Browse: the media glass switch (engine mode glass, auto.lists not 0). */
    static boolean gateOpen() {
        return gate();
    }

    static String propOf(String k, String def) {
        return prop(k, def);
    }

    private static String prop(String k, String def) {
        try {
            return sysGet == null ? def : (String) sysGet.invoke(null, k, def);
        } catch (Throwable t) {
            return def;
        }
    }

    private static final java.util.Set<String> onceKeys = new java.util.HashSet<>();

    private static void once(String key, Throwable t) {
        synchronized (onceKeys) {
            if (!onceKeys.add(key)) return;
        }
        Log.e(TAG, key, t);
    }

    // ------------------------------------------------------- dev commands

    public static Object command(String cmd) {
        if (cmd == null) return "no cmd";
        String[] a = cmd.trim().split("\\s+");
        switch (a[0]) {
            case "windows":
                return Probe.windows();
            case "tree": {
                View w = Probe.window(a.length > 1 ? a[1] : "Dashboard");
                return w == null ? "no window" : Probe.tree(w);
            }
            case "draws": {
                View w = Probe.window(a.length > 1 ? a[1] : "Dashboard");
                return w == null ? "no window" : Probe.draws(w, ctx.getCacheDir());
            }
            case "trace": {
                hookTrace();
                int n = a.length > 1 ? Integer.parseInt(a[1]) : 2000;
                return Trace.arm(ctx.getCacheDir(), n, !(a.length > 2 && "all".equals(a[2])));
            }
            case "trace-stop":
                Trace.stop();
                return "trace stopped";
            case "reset":
                hookNs = hookCalls = slotDraws = 0;
                return "counters reset";
            case "on":
                forced = Boolean.TRUE;
                rerecord();
                return "forced on";
            case "off":
                forced = Boolean.FALSE;
                rerecord();
                return "forced off (button fills come back when the screen next redraws)";
            case "auto":
                forced = null;
                rerecord();
                return "following the properties";
            case "set": {
                if (a.length < 3) return "set <key> <value>: " + look.dump();
                float v = Float.parseFloat(a[2]);
                boolean ok = true;
                switch (a[1]) {
                    case "through": scrimThrough = v; break;
                    case "bthrough": blurredThrough = v; break;
                    case "blur": blurPx = v; break;
                    case "fill": fillKeep = v; break;
                    case "tint": tintKeep = v; break;
                    case "effect": effectScale = v; break;
                    default: ok = look.set(a[1], v);
                }
                if (!ok) return "no such key " + a[1];
                lookVersion++;
                for (Art art : arts.values()) {
                    art.preBlur = art.rawBlur * effectScale;
                    art.through = art.rawBlur > 0f ? blurredThrough : scrimThrough;
                    refreshBackdrop(art);
                }
                rerecord();
                return a[1] + "=" + v;
            }
            case "state": {
                StringBuilder sb = new StringBuilder();
                sb.append("running=").append(running).append(" gate=").append(gate()).append(" dp=").append(dp)
                        .append(" through=").append(scrimThrough).append(" bthrough=").append(blurredThrough)
                        .append(" blur=").append(blurPx)
                        .append(" fill=").append(fillKeep).append(" tint=").append(tintKeep)
                        .append(" effect=").append(effectScale).append('\n');
                sb.append("look: ").append(look.dump()).append('\n');
                sb.append("edges ").append(edges.size()).append(", blurred layers ").append(effects.size())
                        .append(", scrims ").append(scrims.size()).append('\n');
                for (Map.Entry<RenderNode, Art> e : arts.entrySet()) {
                    Art art = e.getValue();
                    sb.append("art layer ").append(e.getKey().getWidth()).append('x').append(e.getKey().getHeight())
                            .append(" bitmap ").append(art.bitmap == null ? "-" : art.bitmap.getWidth() + "x" + art.bitmap.getHeight())
                            .append(String.format(" scrim %08x", art.scrim)).append(" pre-blur ").append(art.preBlur)
                            .append(" backdrop ").append(art.backdrop != null).append('\n');
                }
                for (Map.Entry<RenderNode, Map<String, Slot>> e : slots.entrySet()) {
                    sb.append("button layer ").append(e.getKey().getWidth()).append('x').append(e.getKey().getHeight()).append('\n');
                    for (Map.Entry<String, Slot> se : e.getValue().entrySet()) {
                        sb.append("  slot ").append(se.getKey()).append(String.format(" fill %08x", se.getValue().fill))
                                .append(" recorded ").append(se.getValue().recorded != null).append('\n');
                    }
                }
                sb.append(String.format("hooks: %d calls, %.1f us each; %d slot draws",
                        hookCalls, hookCalls == 0 ? 0 : hookNs / 1000.0 / hookCalls, slotDraws));
                return sb.toString();
            }
            default:
                return "unknown " + cmd;
        }
    }
}
