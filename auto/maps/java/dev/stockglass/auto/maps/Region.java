package dev.stockglass.auto.maps;

import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.PorterDuff;
import android.graphics.PorterDuffXfermode;
import android.graphics.Rect;
import android.graphics.Xfermode;
import android.graphics.drawable.Drawable;
import android.util.ArraySet;
import android.util.LongSparseArray;
import android.view.Surface;
import android.view.View;
import android.view.ViewTreeObserver;
import java.lang.ref.Reference;
import java.lang.ref.ReferenceQueue;
import java.lang.ref.WeakReference;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.HashSet;
import java.util.Set;
import java.util.WeakHashMap;

/**
 * One blur region: a BackgroundBlurDrawable owned by one control.
 *
 * A BackgroundBlurDrawable does not blur anything itself. Drawing it records an
 * empty RenderNode at the control's bounds (and, as the framework sets it up,
 * clears those pixels with transparent SRC paint); RenderThread reports where that node lands on the
 * surface every frame, and the window's aggregator sends the rect, corner radii
 * and blur radius to SurfaceFlinger with the same frame's buffer. So the
 * region follows scrolling and animation exactly, and SurfaceFlinger blurs the
 * layers below the window there, which is what the engine's drawBlurRegion
 * turns into a glass pane.
 *
 * Main thread only, like the aggregator behind it.
 */
final class Region {
    private static Method getViewRootImpl, create, setBlurRadius, setCorner4, setCorner1, setXfermode;
    private static Field blurRadiusField, paintField;
    private static final ReferenceQueue<Object> dead = new ReferenceQueue<>();
    private static final Set<Owner> owners = new HashSet<>();
    private static final WeakHashMap<View, Window> watched = new WeakHashMap<>();

    private Drawable bbd;
    private Object root;
    private WeakReference<View> host;
    private boolean visible;
    private int punching = -1;
    private int alpha = 255;
    private float drawOpacity = 1f;
    private Fade fade;

    /** An extra opacity for a region, from state the view tree does not show. */
    interface Fade {
        float of(View host) throws Exception;
    }

    /** Multiplies this region's opacity by {@code f} every frame. Must not hold the host. */
    void fadeWith(Fade f) {
        fade = f;
    }

    private float fadeOf(View h) {
        if (fade == null) return 1f;
        try {
            return fade.of(h);
        } catch (Throwable e) {
            L.once("region-fade", "cannot read a region's fade; drawing it at full opacity", e);
            return 1f;
        }
    }
    private int defaultRadius;
    private int l, t, r, b, blur = -1;
    private float tl = -1, tr = -1, br = -1, bl = -1;
    /** The tint asked of the engine: 0xRRGGBB, and its strength as a byte (0 = none). */
    private int tintRgb, tintQ;

    /**
     * Ties this region to the lifetime of {@code owner}: once the owner is
     * collected, the drawable leaves the aggregator. Without this every tile
     * Compose ever disposed would keep its drawable registered, and the window
     * would keep running the per-frame blur callback for nothing.
     */
    void ownedBy(Object owner) {
        synchronized (owners) {
            owners.add(new Owner(owner, this));
        }
    }

    /** Release the regions of collected owners. Cheap; call from hooks and draws. */
    static void reap() {
        for (Reference<?> ref; (ref = dead.poll()) != null; ) {
            Owner o = (Owner) ref;
            synchronized (owners) {
                owners.remove(o);
            }
            o.region.release();
        }
    }

    /**
     * Draws the region into {@code canvas} at the given rect, in the canvas's
     * own coordinates, on behalf of {@code host} (any view in the same window).
     * Radii are per corner: top-left, top-right, bottom-right, bottom-left.
     * {@code punch} keeps the framework's behaviour of clearing what the window
     * drew under the control; without it the region only adds glass beneath.
     */
    private static float snap(float radius) {
        return Math.round(radius * 16f) / 16f;
    }

    /**
     * Asks the engine to draw this region as glass tinted {@code rgb}
     * (0xRRGGBB) at {@code strength} (0 = plain glass), from the next draw on.
     * The engine filters what the glass transmits by the colour and lays the
     * colour over it at that strength. Android's own blur ignores it.
     */
    void tint(int rgb, float strength) {
        tintRgb = rgb & 0xffffff;
        tintQ = Math.round(Math.max(0f, Math.min(1f, strength)) * 255f);
    }

    /** The engine's encoding (engine/src/region_tint.h): whole pixels + (2q + 1) / 512. */
    private static float corner(float radius, int q) {
        return (float) Math.floor(Math.max(0f, radius)) + (2 * q + 1) / 512f;
    }

    void draw(View host, Canvas canvas, int left, int top, int right, int bottom,
              float rTL, float rTR, float rBR, float rBL, int blurRadius, boolean punch) {
        draw(host, canvas, left, top, right, bottom, rTL, rTR, rBR, rBL, blurRadius, punch, 1f);
    }

    /**
     * As above, with the region's opacity: SurfaceFlinger blends the glass by
     * it, so a control that fades can fade its glass with it.
     */
    void draw(View host, Canvas canvas, int left, int top, int right, int bottom,
              float rTL, float rTR, float rBR, float rBL, int blurRadius, boolean punch,
              float opacity) {
        if (!canvas.isHardwareAccelerated()) return; // a software snapshot; no RenderThread
        if (right <= left || bottom <= top) {
            hide();
            return;
        }
        // The engine reads an odd multiple of 1/512 px in all four corner radii
        // as a tint request (engine/src/region_tint.h). A measured or animated
        // radius can land on one by chance, about once in a thousand values;
        // a multiple of 1/16 px never does. A tint is that request on purpose:
        // red, green, blue and strength in the four corners, each radius at
        // least a pixel inside half the shorter side, since Skia scales down
        // corners that do not fit and that would erase the fractions.
        if (tintQ > 0) {
            float fit = Math.min(right - left, bottom - top) / 2f - 1f;
            rTL = corner(Math.min(rTL, fit), (tintRgb >> 16) & 0xff);
            rTR = corner(Math.min(rTR, fit), (tintRgb >> 8) & 0xff);
            rBR = corner(Math.min(rBR, fit), tintRgb & 0xff);
            rBL = corner(Math.min(rBL, fit), tintQ);
        } else {
            rTL = snap(rTL);
            rTR = snap(rTR);
            rBR = snap(rBR);
            rBL = snap(rBL);
        }
        try {
            Object vri = viewRoot(host);
            if (vri == null) return;
            if (this.host == null || this.host.get() != host) this.host = new WeakReference<>(host);
            // The window's aggregator copies its regions in a pre-draw listener,
            // before the draw pass. Whatever changes here, during the draw, goes
            // to SurfaceFlinger with the NEXT frame, and a window that is not
            // animating (an idle home screen) has none. So a change asks for one.
            boolean changed = false;
            if (bbd == null || vri != root) {
                release();
                bbd = (Drawable) createMethod(vri).invoke(vri);
                root = vri;
                int id;
                synchronized (lastReported) {
                    id = ++nodeId;
                    lastReported.setEmpty();
                }
                followRenderThread(id);
                visible = true; // the aggregator registers it as soon as it is created
                defaultRadius = blurRadiusField(bbd).getInt(bbd);
                l = t = r = b = 0;
                blur = -1;
                tl = tr = br = bl = -1;
                punching = -1;
                alpha = 255; // a new drawable starts opaque
                changed = true;
            }
            if (Math.round(opacity * 255f) <= 0) {
                hide();
                return;
            }
            drawOpacity = Math.min(1f, opacity);
            int before = alpha;
            // The drawable records nothing (no path, no node) while its alpha
            // is 0, and only a node in the display list keeps a position. So it
            // is recorded visible, and applyAlpha() below then takes it out of
            // the window's regions if an ancestor hides the control.
            if (alpha == 0) {
                bbd.setAlpha(255);
                alpha = 255;
            }
            if (punching != (punch ? 1 : 0)) {
                setPunch(punch);
                punching = punch ? 1 : 0;
            }
            if (!visible) {
                bbd.setVisible(true, false);
                visible = true;
                // Its path is only rebuilt by setBounds/setCornerRadius, and a
                // rebuild while hidden leaves it empty, which draws nothing.
                l = t = r = b = 0;
                changed = true;
            }
            int want = blurRadius > 0 ? blurRadius : defaultRadius;
            if (want != blur) {
                setBlurRadius.invoke(bbd, want);
                blur = want;
                changed = true;
            }
            if (rTL != tl || rTR != tr || rBR != br || rBL != bl) {
                setCorners(rTL, rTR, rBR, rBL);
                tl = rTL;
                tr = rTR;
                br = rBR;
                bl = rBL;
                changed = true;
            }
            if (left != l || top != t || right != r || bottom != b) {
                bbd.setBounds(left, top, right, bottom);
                l = left;
                t = top;
                r = right;
                b = bottom;
            }
            bbd.draw(canvas);
            applyAlpha(ancestorAlpha(host) * fadeOf(host));
            if (changed || alpha != before) host.postInvalidateOnAnimation();
            window = watch(host);
        } catch (Throwable e) {
            L.once("region", "cannot create or draw a BackgroundBlurDrawable; no glass", e);
            release();
        }
    }

    // ---- RenderThread frames ----------------------------------------------

    private static Class<?> positionListener;
    private static Method addPositionListener, removePositionListener;
    private static Field renderNodeField, ownListenerField;
    private volatile Window window;
    /** RenderThread: the window's pre-draw count when this region last moved. */
    private int movedAt = -1;
    /**
     * Where hwui last put this region's node, in window coordinates, or empty
     * once it reported the node lost: written on RenderThread, in the order
     * the moves happen, before the aggregator queues them. sweep() holds the
     * aggregator to it. Guards itself and {@link #nodeId}.
     */
    private final Rect lastReported = new Rect();
    /** Which drawable's node lastReported follows: the listener of a replaced one is ignored. */
    private int nodeId;
    /** Main thread: lastReported is written before the aggregator hears of a move. */
    private boolean recorded;

    /**
     * hwui reports where the region's node is on every frame it renders, UI
     * thread or not, but the aggregator only sends regions to SurfaceFlinger
     * from a frame the UI thread drew: it registers its frame callback in a
     * pre-draw listener. A move made on RenderThread alone (a View.animate()
     * with no listeners, which hwui runs there) left the glass where the last
     * UI frame put it, until something else redrew the window: the content
     * moved and the glass stayed. So the region also listens to its node, and
     * a move on a frame no pre-draw started asks the window for a UI frame,
     * which sends the positions hwui has kept for it.
     *
     * The same listener keeps lastReported, for sweep().
     */
    private void followRenderThread(final int id) {
        recorded = false;
        try {
            if (addPositionListener == null) {
                positionListener = Class.forName("android.graphics.RenderNode$PositionUpdateListener");
                Class<?> renderNode = Class.forName("android.graphics.RenderNode");
                addPositionListener = renderNode.getMethod("addPositionUpdateListener", positionListener);
                renderNodeField = bbd.getClass().getDeclaredField("mRenderNode");
                renderNodeField.setAccessible(true);
                try {
                    removePositionListener = renderNode.getMethod("removePositionUpdateListener",
                            positionListener);
                    ownListenerField = bbd.getClass().getDeclaredField("mPositionUpdateListener");
                    ownListenerField.setAccessible(true);
                } catch (ReflectiveOperationException e) {
                    L.w("cannot reorder a region's position listeners (" + e + "); regions are not"
                            + " checked against hwui");
                }
            }
            final WeakReference<Region> self = new WeakReference<>(this);
            Object l = java.lang.reflect.Proxy.newProxyInstance(positionListener.getClassLoader(),
                    new Class<?>[] {positionListener}, (p, m, a) -> {
                        switch (m.getName()) {
                            case "positionChanged": {
                                Region r = self.get();
                                if (r != null) {
                                    r.movedOnRenderThread(id, (Long) a[0], (Integer) a[1], (Integer) a[2],
                                            (Integer) a[3], (Integer) a[4]);
                                }
                                return null;
                            }
                            case "positionLost": {
                                Region r = self.get();
                                if (r != null) r.movedOnRenderThread(id, (Long) a[0], 0, 0, 0, 0);
                                return null;
                            }
                            case "equals":
                                return p == a[0];
                            case "hashCode":
                                return System.identityHashCode(p);
                            case "toString":
                                return "StockGlass.followRenderThread";
                            default:
                                return null; // applyStretch
                        }
                    });
            Object node = renderNodeField.get(bbd);
            addPositionListener.invoke(node, l);
            // The node calls its listeners in the order they were added. With the
            // drawable's own behind this one, a move is in lastReported before
            // the aggregator has it, so sweep() never finds the aggregator ahead.
            Object own = ownListenerField == null ? null : ownListenerField.get(bbd);
            if (own != null) {
                removePositionListener.invoke(node, own);
                addPositionListener.invoke(node, own);
                recorded = true;
            }
        } catch (Throwable e) {
            L.once("region-rt", "cannot follow a region on RenderThread; glass moved by a"
                    + " RenderThread animation catches up at the next redraw", e);
        }
    }

    /** RenderThread; a lost node reports an empty rect. */
    private void movedOnRenderThread(int id, long frameNumber, int left, int top, int right, int bottom) {
        synchronized (lastReported) {
            if (id != nodeId) return; // the node of a drawable this region has replaced
            lastReported.set(left, top, right, bottom);
        }
        Window w = window;
        if (w == null) return;
        long last = w.lastFrame;
        w.lastFrame = frameNumber;
        if (frameNumber < last && frameNumber > 0) {
            L.i("frame numbers restarted in " + w.name + ": " + last + " -> " + frameNumber);
        }
        int k = w.preDraws;
        if (k != movedAt) {
            movedAt = k; // the frame a UI draw started, which sends it
            return;
        }
        View rv = w.root.get();
        if (rv != null && Gate.rtFollow()) rv.postInvalidateOnAnimation(); // safe off the UI thread
    }

    /** A window with regions: its pre-draw count, read on RenderThread. */
    private static final class Window {
        final WeakReference<View> root;
        final String name;
        /** The tree observer the per-frame sync is listening on. */
        WeakReference<ViewTreeObserver> vto = new WeakReference<>(null);
        volatile int preDraws;
        /** Main thread: time in syncWindow since the last dump, and the count at that dump. */
        long syncNanos;
        int dumpedAt;
        /** RenderThread: the last frame number hwui reported a move on. */
        long lastFrame;
        /** Main thread: a sweep is posted, and the pre-draw count it last looked at. */
        boolean sweepPending;
        int sweptAt;
        final Runnable sweeper = this::sweepNow;

        Window(View root) {
            this.root = new WeakReference<>(root);
            name = root.getClass().getSimpleName() + "@" + Integer.toHexString(System.identityHashCode(root));
        }

        /** Posted by a frame: sweeps, and again later while the window keeps drawing. */
        private void sweepNow() {
            View rv = root.get();
            if (rv == null) {
                sweepPending = false;
                return;
            }
            sweep(rv, name);
            if (preDraws != sweptAt && rv.isAttachedToWindow()) {
                sweptAt = preDraws;
                rv.postDelayed(sweeper, SWEEP_MS);
            } else {
                sweepPending = false;
            }
        }
    }

    /**
     * A region is not part of the view tree, so an ancestor that fades or
     * hides the control (the launcher fading its workspace for the app drawer
     * or an app launch) does nothing to it: the glass would stay where the
     * control has gone. Every frame, before the window draws, this scales each
     * region by its control's own opacity and its ancestors' alpha and
     * visibility, and takes out the region of a control that has left the
     * window's view tree.
     *
     * The window's aggregator copies its regions for the frame in a pre-draw
     * listener of its own, which it adds when the window gets its first
     * region, so before this one. What this changed then reached
     * SurfaceFlinger a frame late, and not at all if the window stopped
     * drawing straight after (the last frame of a fade). So this listener is
     * moved ahead of the aggregator's; when the aggregator re-adds its own
     * (after the window had no regions for a while), it lands behind anyway.
     */
    private static Window watch(View host) {
        View rootView = host.getRootView();
        ViewTreeObserver vto = rootView.getViewTreeObserver();
        Window w = watched.get(rootView);
        // A tree observer belongs to the window, not the view: a root added to
        // a new window (the screenshot UI, shown again) starts with none of
        // the old one's listeners.
        if (w != null && w.vto.get() == vto) return w;
        if (w == null) {
            w = new Window(rootView);
            watched.put(rootView, w);
        }
        w.vto = new WeakReference<>(vto);
        final Window win = w;
        vto.addOnPreDrawListener(() -> {
            View rv = win.root.get();
            if (rv != null) {
                win.preDraws++;
                long start = System.nanoTime();
                syncWindow(rv);
                if (!win.sweepPending) {
                    win.sweepPending = true;
                    win.sweptAt = win.preDraws;
                    rv.postDelayed(win.sweeper, SWEEP_MS);
                }
                win.syncNanos += System.nanoTime() - start;
            }
            return true;
        });
        aggregatorLast(rootView);
        return win;
    }

    private static Field aggregatorField, aggregatorListener;

    /** Moves the window's blur aggregator's pre-draw listener behind ours. */
    private static void aggregatorLast(View rootView) {
        try {
            Object vri = viewRoot(rootView);
            if (vri == null) return;
            if (aggregatorField == null) {
                aggregatorField = vri.getClass().getDeclaredField("mBlurRegionAggregator");
                aggregatorField.setAccessible(true);
            }
            Object aggregator = aggregatorField.get(vri);
            if (aggregator == null) return;
            if (aggregatorListener == null) {
                aggregatorListener = aggregator.getClass().getDeclaredField("mOnPreDrawListener");
                aggregatorListener.setAccessible(true);
            }
            ViewTreeObserver.OnPreDrawListener l =
                    (ViewTreeObserver.OnPreDrawListener) aggregatorListener.get(aggregator);
            if (l == null) return; // added later, after ours
            ViewTreeObserver vto = rootView.getViewTreeObserver();
            vto.removeOnPreDrawListener(l);
            vto.addOnPreDrawListener(l);
            L.i("blur aggregator of " + rootView.getClass().getSimpleName()
                    + "'s window moved behind the companion's per-frame sync");
        } catch (Throwable e) {
            L.once("region-order", "cannot order the blur aggregator after the companion; a fade"
                    + " reaches SurfaceFlinger a frame late", e);
        }
    }

    private static int frame;

    /** Counts the windows' pre-draw passes: anything cached per frame can key on it. */
    static int frame() {
        return frame;
    }

    private static void syncWindow(View rootView) {
        ++frame;
        Owner[] all;
        synchronized (owners) {
            all = owners.toArray(new Owner[0]);
        }
        Object vri;
        try {
            vri = viewRoot(rootView);
        } catch (Throwable e) {
            return;
        }
        replayOldSurface(vri);
        boolean changed = false;
        long now = android.os.SystemClock.uptimeMillis();
        boolean trace = Gate.debug() && now - lastTrace > 1000;
        if (trace) lastTrace = now;
        for (Owner o : all) {
            Region r = o.region;
            View h = r.host == null ? null : r.host.get();
            if (trace && h != null && r.root == vri) L.i("trace " + r.id() + " visible=" + r.visible
                    + " alpha=" + r.alpha + " at " + r.reported() + " host " + where(h) + " chain "
                    + chain(h));
            if (r.bbd == null || !r.visible || r.root != vri) continue;
            try {
                // A control that has left the tree is not drawn; hwui reports
                // its region lost only once its display list is swept.
                float a = h == null || h.getRootView() != rootView ? 0f : ancestorAlpha(h) * r.fadeOf(h);
                changed |= r.applyAlpha(a);
            } catch (Throwable e) {
                L.once("region-alpha", "cannot fade a region with its control", e);
            }
        }
        if (changed) rootView.postInvalidateOnAnimation();
    }

    private static Field rtUpdatesField, rtLockField, surfaceField;
    private static Method nextFrameNumber;
    private static boolean queueUnreadable, unswept;

    /**
     * hwui reports each move of a region's node with the frame number of the
     * window's surface, and the window's blur aggregator queues the moves by
     * that number and applies them in number order when it sends a frame to
     * SurfaceFlinger. It only sends frames while it has a region at an alpha
     * above 0, so through overview or an app launch, where the launcher fades
     * its pages and every region is at 0, the moves pile up unapplied. When
     * the window then stops, hwui reports every region lost with frame 0, and
     * a window that starts again draws into a new surface, which numbers its
     * frames from 1. The queue then sorts the old moves after everything the
     * new surface reports: the losses apply on the first new frame, and once
     * the new surface's count passes the old numbers, the old rects come back
     * for controls that are no longer there. That was a home screen page
     * drawn as glass over another one, at the scale overview had left it at,
     * until that page was drawn again.
     *
     * A move numbered past the surface's next frame can only be from an
     * earlier surface, so this applies those now, in order, before the
     * aggregator sends this frame. They happened before the losses and before
     * anything the new surface has reported, and are applied before those.
     * Every frame: looking only on a new surface's first frame left glass
     * behind in a chaos run (VERIFIED.md), and the check is a lock and a size.
     */
    private static void replayOldSurface(Object vri) {
        if (queueUnreadable || aggregatorField == null) return;
        try {
            Object aggregator = aggregatorField.get(vri);
            if (aggregator == null) return;
            if (rtUpdatesField == null) {
                Class<?> c = aggregator.getClass();
                rtUpdatesField = c.getDeclaredField("mFrameRtUpdates");
                rtUpdatesField.setAccessible(true);
                rtLockField = c.getDeclaredField("mRtLock");
                rtLockField.setAccessible(true);
                surfaceField = vri.getClass().getDeclaredField("mSurface");
                surfaceField.setAccessible(true);
                nextFrameNumber = Surface.class.getMethod("getNextFrameNumber");
            }
            LongSparseArray<?> queue = (LongSparseArray<?>) rtUpdatesField.get(aggregator);
            synchronized (rtLockField.get(aggregator)) {
                replayLocked(vri, queue);
            }
        } catch (Throwable e) {
            queueUnreadable = true;
            L.once("region-queue", "cannot read the blur aggregator's queue; after the window"
                    + " restarts, an old rect can come back until its control is drawn again", e);
        }
    }

    /** replayOldSurface, holding the aggregator's lock. */
    private static void replayLocked(Object vri, LongSparseArray<?> queue) throws Exception {
        int n = queue.size();
        if (n == 0) return;
        long newest = queue.keyAt(n - 1);
        if (newest <= 1) return;
        Surface surface = (Surface) surfaceField.get(vri);
        if (surface == null || !surface.isValid()) return;
        long next = (Long) nextFrameNumber.invoke(surface);
        if (newest <= next) return;
        int first = n;
        while (first > 0 && queue.keyAt(first - 1) > next) --first;
        int moves = 0;
        for (int i = first; i < n; ++i) {
            ArraySet<?> frameMoves = (ArraySet<?>) queue.valueAt(i);
            for (int j = 0; j < frameMoves.size(); ++j) {
                ((Runnable) frameMoves.valueAt(j)).run();
                ++moves;
            }
        }
        long oldest = queue.keyAt(first);
        for (int i = n - 1; i >= first; --i) queue.removeAt(i);
        L.i("applied " + moves + " region moves from frames " + oldest + "-" + newest
                + " of an earlier surface; the window's next frame is " + next);
    }

    /** How long after a frame a window's regions are swept, and how often while it draws. */
    private static final long SWEEP_MS = 300;

    /**
     * hwui's word against the aggregator's. Once the aggregator has applied
     * every move it has queued, each region's rect should be where hwui last
     * reported its node, or empty if hwui last reported it lost
     * (lastReported, which sees the moves in the order they happened, not in
     * frame number order). One that is not got its moves applied out of
     * order, whatever the cause, and SurfaceFlinger would keep its glass
     * where its control no longer is: an off-screen page's over the page on
     * screen, or a page's own at another scale. hwui only reports a node
     * again when it moves, so nothing else would correct it. This puts such
     * rects back and has the next frame send them.
     *
     * Runs SWEEP_MS after a window draws, and every SWEEP_MS while it keeps
     * drawing; moves still in the queue are left for the next sweep.
     */
    private static void sweep(View rootView, String name) {
        if (unswept || queueUnreadable || rtUpdatesField == null) return;
        Object vri;
        try {
            vri = viewRoot(rootView);
        } catch (Throwable e) {
            return;
        }
        if (vri == null) return;
        Owner[] all;
        synchronized (owners) {
            all = owners.toArray(new Owner[0]);
        }
        int fixed = 0, shown = 0, lost = 0;
        try {
            Object aggregator = aggregatorField.get(vri);
            if (aggregator == null) return;
            @SuppressWarnings("unchecked")
            LongSparseArray<Object> queue = (LongSparseArray<Object>) rtUpdatesField.get(aggregator);
            synchronized (rtLockField.get(aggregator)) {
                replayLocked(vri, queue);
                if (queue.size() > 0) return;
                for (Owner o : all) {
                    Region r = o.region;
                    if (r.root != vri || r.bbd == null || !r.recorded) continue;
                    Rect rect = r.aggregatorRect();
                    synchronized (r.lastReported) {
                        if (rect.equals(r.lastReported)) continue;
                        rect.set(r.lastReported);
                    }
                    ++fixed;
                    if (r.visible && r.alpha > 0) ++shown; // in what SurfaceFlinger has
                    if (rect.isEmpty()) ++lost;
                }
                // An empty frame of moves: the aggregator sends its regions with the next frame.
                if (shown > 0) queue.put(0, new ArraySet<Runnable>());
            }
        } catch (Throwable e) {
            unswept = true;
            L.once("region-sweep", "cannot check regions against hwui; a rect applied out of order"
                    + " stays until its control moves", e);
            return;
        }
        if (fixed == 0) return;
        if (shown > 0) rootView.postInvalidateOnAnimation();
        L.i("put " + fixed + " region rect" + (fixed == 1 ? "" : "s") + " in " + name
                + " back where hwui last reported " + (fixed == 1 ? "its node" : "their nodes") + " ("
                + shown + " with glass, " + lost + " lost)");
    }

    private static long lastTrace;
    private static Field rectField;
    private static boolean shrunk;

    /**
     * Test for followRenderThread(): scales every window's content to 0.9, or
     * back to 1, with a View.animate() that has no listeners, which hwui runs
     * on RenderThread alone. Without the follow, the glass stays where it was.
     */
    private static void renderThreadTest() {
        shrunk = !shrunk;
        for (View rv : watched.keySet().toArray(new View[0])) {
            if (rv == null || !(rv instanceof android.view.ViewGroup)) continue;
            View content = ((android.view.ViewGroup) rv).getChildAt(0);
            if (content == null) continue;
            content.animate().scaleX(shrunk ? 0.9f : 1f).scaleY(shrunk ? 0.9f : 1f).setDuration(4000)
                    .start();
            L.i("rt test: " + content.getClass().getSimpleName() + " to " + (shrunk ? 0.9f : 1f)
                    + " on RenderThread");
        }
    }

    /**
     * Debug (app.debug=1, on a property change broadcast): logs every live
     * region as it stands, without drawing a frame, so a stale one can be
     * caught where it sits.
     */
    static void dump(String mode) {
        if (mode.equals("rt")) renderThreadTest();
        Owner[] all;
        synchronized (owners) {
            all = owners.toArray(new Owner[0]);
        }
        int live = 0;
        for (Owner o : all) {
            Region r = o.region;
            if (r.bbd == null) continue;
            ++live;
            View h = r.host == null ? null : r.host.get();
            L.i("dump " + r.id() + " visible=" + r.visible + " alpha=" + r.alpha + " at " + r.reported()
                    + (h == null ? " host gone" : " " + h.getClass().getSimpleName() + " host "
                    + where(h)));
        }
        StringBuilder w = new StringBuilder();
        for (java.util.Map.Entry<View, Window> e : watched.entrySet()) {
            if (e.getKey() == null) continue;
            Window win = e.getValue();
            int n = win.preDraws - win.dumpedAt;
            w.append(' ').append(e.getKey().getClass().getSimpleName()).append('=').append(win.preDraws);
            if (n > 0) {
                w.append(String.format(java.util.Locale.ROOT, " (%d since the last dump, %.1f us each)", n,
                        win.syncNanos / 1000.0 / n));
            }
            win.syncNanos = 0;
            win.dumpedAt = win.preDraws;
        }
        L.i("dump: " + live + " regions; UI pre-draws per window:" + w);
    }

    private String id() {
        return Integer.toHexString(System.identityHashCode(this));
    }

    /**
     * Debug: where the host is in its window now, whether any of it is on
     * screen, and which window that is ("detached" once the window is gone,
     * where the region can no longer reach SurfaceFlinger).
     */
    private static String where(View h) {
        int[] xy = new int[2];
        h.getLocationInWindow(xy);
        android.graphics.Rect vis = new android.graphics.Rect();
        boolean attached = h.isAttachedToWindow();
        boolean shown = attached && h.getGlobalVisibleRect(vis);
        View root = h.getRootView();
        return xy[0] + "," + xy[1] + " " + h.getWidth() + "x" + h.getHeight() + (shown ? " shown" : " off")
                + " in " + root.getClass().getSimpleName() + "@"
                + Integer.toHexString(System.identityHashCode(root)) + (attached ? "" : " detached");
    }

    /** The rect the aggregator sends for this region (window coordinates), as it has applied hwui's moves. */
    private Rect aggregatorRect() throws Exception {
        if (rectField == null) {
            rectField = bbd.getClass().getDeclaredField("mRect");
            rectField.setAccessible(true);
        }
        return (Rect) rectField.get(bbd);
    }

    /** Debug: the aggregator's rect, and hwui's last word on the node if they differ. */
    private String reported() {
        String s;
        try {
            s = String.valueOf(aggregatorRect());
        } catch (Throwable e) {
            s = "?";
        }
        synchronized (lastReported) {
            if (recorded && !s.equals(String.valueOf(lastReported))) s += " (hwui: " + lastReported + ")";
        }
        return s;
    }

    private static String chain(View v) {
        StringBuilder sb = new StringBuilder();
        for (Object p = v; p instanceof View; p = ((View) p).getParent()) {
            View x = (View) p;
            sb.append(x.getClass().getSimpleName()).append(':').append(x.getVisibility() == View.VISIBLE
                    ? "" : "hidden:").append(x.getAlpha()).append('/').append(x.getTransitionAlpha())
                    .append(' ');
        }
        return sb.toString();
    }

    /** The control's opacity times its ancestors'; 0 if any of them is not VISIBLE. */
    private static float ancestorAlpha(View v) {
        float a = 1f;
        for (Object p = v; p instanceof View; p = ((View) p).getParent()) {
            View x = (View) p;
            if (x.getVisibility() != View.VISIBLE) return 0f;
            a *= x.getAlpha() * x.getTransitionAlpha();
            if (a <= 0f) return 0f;
        }
        return Math.min(1f, a);
    }

    /** Alpha 0 takes it out of the window's regions; its node stays recorded. */
    private boolean applyAlpha(float ancestors) {
        int a = Math.max(0, Math.min(255, Math.round(drawOpacity * ancestors * 255f)));
        if (a == alpha) return false;
        bbd.setAlpha(a);
        alpha = a;
        if (Gate.debug()) {
            View h = host == null ? null : host.get();
            L.i("region " + Integer.toHexString(System.identityHashCode(this)) + " alpha " + a
                    + " (" + (h == null ? "?" : h.getClass().getSimpleName()) + ", ancestors "
                    + ancestors + ")");
        }
        return true;
    }

    /**
     * Where hwui last put this region's node, in window coordinates, copied
     * into {@code out}: empty before the first report, after a loss, and when
     * the region cannot follow RenderThread. A frame behind at most.
     */
    Rect windowRect(Rect out) {
        synchronized (lastReported) {
            out.set(lastReported);
        }
        return out;
    }

    /** Takes the region out of the window's blur regions until the next draw. */
    void hide() {
        if (bbd != null && visible) {
            bbd.setVisible(false, false);
            visible = false;
            View h = host == null ? null : host.get();
            if (h != null) h.postInvalidateOnAnimation(); // see draw(): the removal needs a frame
        }
    }

    void release() {
        hide();
        bbd = null;
        root = null;
    }

    private void setCorners(float rTL, float rTR, float rBR, float rBL) throws Exception {
        if (setCorner4 == null && setCorner1 == null) {
            Class<?> c = bbd.getClass();
            try {
                // (TL, TR, BL, BR) on every version that has it.
                setCorner4 = c.getMethod("setCornerRadius", float.class, float.class, float.class,
                        float.class);
            } catch (NoSuchMethodException e) {
                setCorner1 = c.getMethod("setCornerRadius", float.class);
            }
        }
        if (setCorner4 != null) setCorner4.invoke(bbd, rTL, rTR, rBL, rBR);
        // One radius for all four cannot carry a tint, and an encoded one would
        // read as a grey tint: snapped, it carries none.
        else setCorner1.invoke(bbd, snap(Math.min(Math.min(rTL, rTR), Math.min(rBR, rBL))));
    }

    /**
     * The framework draws the drawable's shape with transparent SRC paint,
     * which erases whatever the window drew there first. Under a QS tile that
     * is the shade's scrim, so the tile would show the unveiled home screen,
     * darker than everything around it in a light theme. Without the punch the
     * paint is transparent SRC_OVER: nothing is drawn, and the node still
     * reports its position.
     */
    private void setPunch(boolean punch) throws Exception {
        Xfermode mode = punch ? new PorterDuffXfermode(PorterDuff.Mode.SRC) : null;
        if (setXfermode == null && paintField == null) {
            try {
                setXfermode = bbd.getClass().getMethod("setXfermode", Xfermode.class);
            } catch (NoSuchMethodException e) {
                paintField = bbd.getClass().getDeclaredField("mPaint");
                paintField.setAccessible(true);
            }
        }
        if (setXfermode != null) setXfermode.invoke(bbd, mode);
        else ((Paint) paintField.get(bbd)).setXfermode(mode);
    }

    private static Object viewRoot(View v) throws Exception {
        if (getViewRootImpl == null) getViewRootImpl = View.class.getMethod("getViewRootImpl");
        return getViewRootImpl.invoke(v);
    }

    private static Method createMethod(Object vri) throws Exception {
        if (create == null) {
            create = vri.getClass().getMethod("createBackgroundBlurDrawable");
        }
        return create;
    }

    private static Field blurRadiusField(Drawable d) throws Exception {
        if (blurRadiusField == null) {
            setBlurRadius = d.getClass().getMethod("setBlurRadius", int.class);
            blurRadiusField = d.getClass().getDeclaredField("mBlurRadius");
            blurRadiusField.setAccessible(true);
            L.i("BackgroundBlurDrawable available; framework default radius "
                    + blurRadiusField.getInt(d) + " px");
        }
        return blurRadiusField;
    }

    private static final class Owner extends WeakReference<Object> {
        final Region region;

        Owner(Object owner, Region region) {
            super(owner, dead);
            this.region = region;
        }
    }
}
