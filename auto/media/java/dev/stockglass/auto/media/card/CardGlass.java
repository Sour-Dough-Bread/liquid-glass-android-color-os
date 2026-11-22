package dev.stockglass.auto.media.card;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Matrix;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.RecordingCanvas;
import android.graphics.RenderNode;
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
 * Liquid Glass under the buttons of Android Auto's dashboard media card.
 *
 * How the card draws (Android Auto 18.0, Jetpack Compose, obfuscated): a
 * card layer (a RenderNode the size of the card) draws, in this order, a
 * child layer with the artwork (a bitmap filling the layer, then a
 * translucent scrim rect over it), the app icon, title and artist, the
 * progress bar (its own layer), and then each button: its background as a
 * rounded rect (the play/pause pill filled with the artwork's palette colour,
 * previous and next transparent circles), then its icon in a layer of its
 * own (an ALPHA_8 bitmap tinted by a colour filter).
 *
 * So the glass goes between a button's background and its icon, and it can
 * be put there from the framework's side alone, with nothing of Android
 * Auto's obfuscated code named anywhere:
 *
 *   - RenderNode.beginRecording / endRecording (before-hooks) keep a stack
 *     of the layers being recorded on each thread.
 *   - RecordingCanvas.drawBitmap(Bitmap, Rect, Rect, Paint): a colour bitmap
 *     that fills a layer of at least 160 px each way is artwork; the layer
 *     under it on the stack is the card.
 *   - RecordingCanvas.drawRect(float x4, Paint): a translucent rect over the
 *     whole artwork layer is its scrim.
 *   - RecordingCanvas.drawRoundRect(float x6, Paint): in a card's layer, a
 *     pill or circle inside the artwork's area is a button's background.
 *     Before it is drawn, the hook draws a RenderNode of ours (the slot)
 *     into the card's display list at the same place, and makes a filled
 *     background translucent. The icon is drawn after, so it stays crisp.
 *
 * The slot holds the glass (Lens: the engine's own shader) refracting the
 * artwork (Backdrop), never the screen, so a button never refracts its own
 * fill or icon. The slot is ours: new artwork or new parameters re-record
 * the slot alone, and Compose's display lists keep pointing at it.
 */
public final class CardGlass {
    static final String TAG = "SGAutoMedia";
    /** A layer this big each way, filled by a colour bitmap, is artwork. */
    private static final int MIN_ART_PX = 160;

    private static Context ctx;
    private static String process;
    private static Handler main;
    private static HandlerThread worker;
    private static Handler work;
    private static volatile boolean running;

    private static final ThreadLocal<ArrayList<RenderNode>> stack = ThreadLocal.withInitial(ArrayList::new);
    /** The card's layer -> its artwork. */
    private static final WeakHashMap<RenderNode, Art> arts = new WeakHashMap<>();
    /** The artwork's own layer -> its artwork (for the scrim). */
    private static final WeakHashMap<RenderNode, Art> artLayers = new WeakHashMap<>();
    /** The card's layer -> its buttons' slots, by place and size. */
    private static final WeakHashMap<RenderNode, Map<String, Slot>> slots = new WeakHashMap<>();
    private static final Map<Integer, Paint> fills = new HashMap<>();

    static final Lens.Look look = new Lens.Look();
    private static int lookVersion;
    /** How much of the card's scrim the glass sees (Backdrop). */
    static float scrimThrough = 0.5f;
    /** Box radius of the backdrop's blur, in artwork px. */
    static float blurPx = 12f;
    /** Alpha kept on a filled button background (the play/pause pill). */
    static float fillKeep = 0.4f;
    /** How strongly a filled button's glass is tinted with its fill colour. */
    static float tintKeep = 0.5f;
    /** Dev override of the switches: null follows the properties. */
    private static Boolean forced;
    private static float dp = -1f;

    private static Method sysGet;
    private static long gateAt;
    private static boolean gate;
    private static long hookNs, hookCalls, slotDraws;

    private CardGlass() {}

    static final class Art {
        WeakReference<RenderNode> layer;
        Bitmap bitmap;
        final Rect src = new Rect();
        final Rect dst = new Rect();
        /** The art layer's canvas matrix when the bitmap was drawn. */
        final Matrix m = new Matrix();
        int scrim;
        Backdrop backdrop;
        boolean building;
    }

    static final class Slot {
        final RenderNode node = new RenderNode("StockGlassLens");
        /** Slot coordinates -> the card layer's. */
        final Matrix toCard = new Matrix();
        float w, h, radius;
        int fill;
        Art art;
        WeakReference<RenderNode> card;
        /** Where it last was, in the card's coordinates. */
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
        main = new Handler(Looper.getMainLooper());
        worker = new HandlerThread("SGAutoMedia-backdrop");
        worker.start();
        work = new Handler(worker.getLooper());
        String bad = Lens.check();
        if (bad != null) {
            Log.e(TAG, "the engine's shader does not compile here; the card stays stock: " + bad);
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
        } catch (Throwable t) {
            Log.e(TAG, "no RecordingCanvas of the expected shape; the card stays stock", t);
            return;
        }
        running = true;
        Log.i(TAG, "media card glass ready in " + proc);
    }

    public static void stop() {
        running = false;
        // Empty every slot, so nothing of this build is drawn once it is gone.
        for (Map<String, Slot> m : slots.values()) {
            for (Slot s : m.values()) {
                s.node.beginRecording(1, 1);
                s.node.endRecording();
            }
        }
        invalidateAll();
        if (worker != null) worker.quitSafely();
        Log.i(TAG, "media card glass stopped");
    }

    // --------------------------------------------------------------- hooks

    public static void before(String key, Object param) {
        if (!running) return;
        MethodHookParam p = (MethodHookParam) param;
        switch (key) {
            case "begin":
                stack.get().add((RenderNode) p.thisObject);
                return;
            case "end": {
                ArrayList<RenderNode> s = stack.get();
                for (int i = s.size() - 1; i >= 0; i--) {
                    if (s.get(i) == p.thisObject) {
                        while (s.size() > i) s.remove(s.size() - 1);
                        break;
                    }
                }
                return;
            }
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

    private static void onBitmap(MethodHookParam p) {
        Bitmap b = (Bitmap) p.args[0];
        Rect dst = (Rect) p.args[2];
        if (b == null || dst == null || b.getConfig() == Bitmap.Config.ALPHA_8) return;
        RenderNode layer = top(0), card = top(1);
        if (layer == null || card == null) return;
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
        Art a = arts.get(card);
        if (a == null) {
            a = new Art();
            arts.put(card, a);
        }
        a.layer = new WeakReference<>(layer);
        artLayers.put(layer, a);
        // Buttons drawn in the artwork's own layer (no layer of its own for
        // the artwork) find it too.
        arts.put(layer, a);
        a.bitmap = b;
        Rect src = (Rect) p.args[1];
        if (src != null) a.src.set(src);
        else a.src.set(0, 0, b.getWidth(), b.getHeight());
        a.dst.set(dst);
        a.m.set(m);
        a.scrim = 0;
        final Art art = a;
        // after this recording: by then the scrim, if any, has been drawn too
        main.post(() -> refreshBackdrop(art));
    }

    private static void onRect(MethodHookParam p) {
        RenderNode layer = top(0);
        if (layer == null) return;
        Art a = artLayers.get(layer);
        if (a == null) return;
        Paint paint = (Paint) p.args[4];
        if (paint == null || paint.getShader() != null) return;
        int alpha = Color.alpha(paint.getColor());
        if (alpha == 0 || alpha == 255) return;
        float l = (float) p.args[0], t = (float) p.args[1], r = (float) p.args[2], b = (float) p.args[3];
        if (r - l < layer.getWidth() * 0.9f || b - t < layer.getHeight() * 0.9f) return;
        a.scrim = paint.getColor();
    }

    private static void onRoundRect(MethodHookParam p) {
        RenderNode card = top(0);
        if (card == null) return;
        Art a = arts.get(card);
        if (a == null || a.bitmap == null) return;
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
        RectF artRect = artOn(a, card);
        if (artRect == null || !artRect.contains(onCard)) return;
        if (onCard.width() * onCard.height() > 0.25f * artRect.width() * artRect.height()) return;
        if (!gate()) return;
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
                Log.i(TAG, "button at " + key + " r=" + rx + " on a card with " + a.bitmap.getWidth() + "x"
                        + a.bitmap.getHeight() + " artwork");
            }
            mine.put(key, s);
        }
        s.onCard.set(onCard);
        Paint paint = (Paint) p.args[6];
        int fill = paint == null ? 0 : paint.getColor();
        s.toCard.set(m);
        s.toCard.preTranslate(l, t);
        s.w = w;
        s.h = h;
        s.radius = rx;
        s.fill = fill;
        s.art = a;
        if (s.card == null || s.card.get() != card) s.card = new WeakReference<>(card);
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

    /** Where the artwork's bitmap lands in the coordinates of {@code card}. */
    private static RectF artOn(Art a, RenderNode card) {
        Matrix bm = bitmapTo(a, card);
        if (bm == null) return null;
        RectF r = new RectF(a.src);
        bm.mapRect(r);
        return r;
    }

    /**
     * Artwork pixels -> {@code card}'s coordinates: src -> dst, the art
     * layer's canvas matrix, and, when the card is the layer's parent, the
     * art layer's place and transform in it.
     */
    private static Matrix bitmapTo(Art a, RenderNode card) {
        RenderNode layer = a.layer == null ? null : a.layer.get();
        if (layer == null || a.src.isEmpty()) return null;
        Matrix bm = new Matrix();
        bm.setRectToRect(new RectF(a.src), new RectF(a.dst), Matrix.ScaleToFit.FILL);
        bm.postConcat(a.m);
        if (card != layer) {
            Matrix nm = new Matrix();
            layer.getMatrix(nm);
            bm.postConcat(nm);
            bm.postTranslate(layer.getLeft(), layer.getTop());
        }
        return bm;
    }

    // ---------------------------------------------------------------- glass

    private static void record(Slot s) {
        Art a = s.art;
        Backdrop bd = a == null ? null : a.backdrop;
        if (bd != null && !bd.matches(a.bitmap, a.scrim, scrimThrough)) bd = null;
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
                Matrix bm = s.card == null ? null : bitmapTo(a, s.card.get());
                Matrix inv = new Matrix();
                if (bm != null && bm.invert(inv)) {
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
        if (a.backdrop != null && a.backdrop.matches(b, a.scrim, scrimThrough)) return;
        a.building = true;
        final int scrim = a.scrim;
        final float through = scrimThrough, blur = blurPx;
        work.post(() -> {
            Backdrop made = null;
            try {
                made = Backdrop.make(b, scrim, through, blur);
            } catch (Throwable t) {
                once("backdrop", t);
            }
            final Backdrop done = made;
            main.post(() -> {
                a.building = false;
                if (done == null) return;
                a.backdrop = done;
                Log.i(TAG, "backdrop ready: " + b.getWidth() + "x" + b.getHeight() + String.format(" scrim %08x", scrim));
                rerecord();
                // the artwork or scrim may have changed while this one was made
                if (!done.matches(a.bitmap, a.scrim, scrimThrough)) refreshBackdrop(a);
            });
        });
    }

    private static void rerecord() {
        for (Map<String, Slot> m : slots.values()) {
            for (Slot s : m.values()) record(s);
        }
        invalidateAll();
    }

    /** Any frame of a window syncs our slots; the card's own display list is not re-recorded. */
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

    /** The engine's switch (persist.sys.glass.mode glass or on) and this module's own (persist.sys.glass.auto.media). */
    private static boolean gate() {
        if (forced != null) return forced;
        long now = SystemClock.uptimeMillis();
        if (now - gateAt < 500 && gateAt != 0) return gate;
        gateAt = now;
        String mode = prop("persist.sys.glass.mode", "");
        String own = prop("persist.sys.glass.auto.media", "1");
        gate = ("glass".equals(mode) || "on".equals(mode)) && !"0".equals(own);
        return gate;
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
                return "forced off (button fills come back when the card next redraws)";
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
                    case "blur": blurPx = v; break;
                    case "fill": fillKeep = v; break;
                    case "tint": tintKeep = v; break;
                    default: ok = look.set(a[1], v);
                }
                if (!ok) return "no such key " + a[1];
                lookVersion++;
                for (Art art : arts.values()) refreshBackdrop(art);
                rerecord();
                return a[1] + "=" + v;
            }
            case "state": {
                StringBuilder sb = new StringBuilder();
                sb.append("running=").append(running).append(" gate=").append(gate()).append(" dp=").append(dp)
                        .append(" through=").append(scrimThrough).append(" blur=").append(blurPx)
                        .append(" fill=").append(fillKeep).append(" tint=").append(tintKeep).append('\n');
                sb.append("look: ").append(look.dump()).append('\n');
                for (Map.Entry<RenderNode, Art> e : arts.entrySet()) {
                    Art art = e.getValue();
                    sb.append("card ").append(e.getKey().getWidth()).append('x').append(e.getKey().getHeight())
                            .append(" art ").append(art.bitmap == null ? "-" : art.bitmap.getWidth() + "x" + art.bitmap.getHeight())
                            .append(" on card ").append(artOn(art, e.getKey()))
                            .append(String.format(" scrim %08x", art.scrim))
                            .append(" backdrop ").append(art.backdrop != null).append('\n');
                    Map<String, Slot> m = slots.get(e.getKey());
                    if (m != null) {
                        for (Map.Entry<String, Slot> se : m.entrySet()) {
                            sb.append("  slot ").append(se.getKey()).append(String.format(" fill %08x", se.getValue().fill))
                                    .append(" recorded ").append(se.getValue().recorded != null).append('\n');
                        }
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
