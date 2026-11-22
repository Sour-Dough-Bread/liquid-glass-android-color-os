package dev.stockglass.auto.maps;

import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.ColorMatrix;
import android.graphics.ColorMatrixColorFilter;
import android.graphics.Outline;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.graphics.drawable.Drawable;
import android.view.View;
import android.view.ViewOutlineProvider;
import android.view.ViewTreeObserver;
import java.lang.ref.WeakReference;
import java.util.WeakHashMap;

/**
 * Glass under a control whose background is its plate: one of Maps' floating
 * map buttons or its search pill. Adapted from the companion's ViewGlass,
 * with one difference that matters for Maps: the plate is a CardView's
 * RoundRectDrawable, whose setAlpha() does nothing (androidx leaves it empty).
 * So the plate is made translucent with a colour filter that scales its alpha,
 * which every Drawable honours through the public setColorFilter(). Icons and
 * text are children of the card and are not touched.
 *
 * The region is a Drawable in the control's ViewOverlay, drawn at the view's
 * outline (rect and corner radius), so it follows the control through layout
 * changes and animation with no bookkeeping here (see Region). Main thread.
 */
final class PlateGlass extends Drawable {
    private static final WeakHashMap<View, PlateGlass> all = new WeakHashMap<>();
    private static boolean listening;

    private final WeakReference<View> view;
    final String what;
    final Region region = new Region();
    private Drawable background;
    /** The filter this put on the background, and the fill it was made for. */
    private ColorMatrixColorFilter ours;
    private float oursFor = -1f;
    private int measuredW = -1, measuredH = -1, natural = 255;
    private boolean strong;
    /** Corner radii {TL, TR, BR, BL} measured from the drawn plate, for a plate with a path outline. */
    private float[] corners;
    private static final float STRONG_FILL = 0.6f;
    private int alphaWas = 255;
    private boolean shadowCleared;
    private int ambientShadow, spotShadow;
    private final Rect rect = new Rect(), tmp = new Rect();
    /** Test only (auto.probe): extra tiny regions, drawn by the first plate attached. */
    private static PlateGlass prober;
    private final java.util.ArrayList<Region> probes = new java.util.ArrayList<>();
    private final Outline outline = new Outline();
    private final ViewTreeObserver.OnPreDrawListener preDraw = () -> {
        View v = PlateGlass.this.view.get();
        if (v != null) applyFill(v);
        return true;
    };
    private final View.OnAttachStateChangeListener attach = new View.OnAttachStateChangeListener() {
        @Override
        public void onViewAttachedToWindow(View v) {
            v.getViewTreeObserver().addOnPreDrawListener(preDraw);
        }

        @Override
        public void onViewDetachedFromWindow(View v) {
            v.getViewTreeObserver().removeOnPreDrawListener(preDraw);
        }
    };

    /** A lining: only its fill is made translucent; the plate it lines draws the glass. */
    private final boolean lining;

    private PlateGlass(View v, String what, boolean lining) {
        view = new WeakReference<>(v);
        this.what = what;
        this.lining = lining;
        region.ownedBy(v);
    }

    static boolean has(View v) {
        return all.containsKey(v);
    }

    /** Whether v is a plate (not a lining) that is opaque at its current size, so has glass. */
    static boolean covers(View v) {
        PlateGlass g = all.get(v);
        if (g == null || g.lining) return false;
        if (v.getWidth() != g.measuredW || v.getHeight() != g.measuredH) g.applyFill(v);
        return g.natural >= 0x80;
    }

    /** Gives {@code v} glass, once; later calls return the same one. */
    static PlateGlass attach(View v, String what) {
        return attach(v, what, false);
    }

    static PlateGlass attachLining(View v, String what) {
        return attach(v, what, true);
    }

    private static PlateGlass attach(View v, String what, boolean lining) {
        PlateGlass g = all.get(v);
        if (g != null) return g;
        g = new PlateGlass(v, what, lining);
        all.put(v, g);
        g.setBounds(0, 0, Math.max(1, v.getWidth()), Math.max(1, v.getHeight()));
        v.getOverlay().add(g);
        v.addOnAttachStateChangeListener(g.attach);
        v.getViewTreeObserver().addOnPreDrawListener(g.preDraw);
        v.invalidate();
        if (!listening) {
            listening = true;
            Gate.onChange(PlateGlass::redrawAll);
        }
        if (prober == null && !lining) prober = g;
        L.i("glass on " + what + " (" + v.getClass().getName() + " " + v.getWidth() + "x" + v.getHeight() + ")");
        return g;
    }

    static void redrawAll() {
        for (PlateGlass g : all.values().toArray(new PlateGlass[0])) {
            View v = g.view.get();
            if (v == null) continue;
            g.setBounds(0, 0, Math.max(1, v.getWidth()), Math.max(1, v.getHeight()));
            g.invalidateSelf();
            v.invalidate();
        }
    }

    /** Takes every plate back to stock and drops its region: a newer build is taking over. */
    static void detachAll() {
        for (java.util.Map.Entry<View, PlateGlass> e : all.entrySet().toArray(new java.util.Map.Entry[0])) {
            View v = e.getKey();
            PlateGlass g = e.getValue();
            if (v == null || g == null) continue;
            g.restore();
            g.region.release();
            for (Region p : g.probes) p.release();
            v.getOverlay().remove(g);
            v.removeOnAttachStateChangeListener(g.attach);
            v.getViewTreeObserver().removeOnPreDrawListener(g.preDraw);
            v.invalidate();
        }
        all.clear();
    }

    @Override
    public void draw(Canvas canvas) {
        View v = view.get();
        if (v == null) return;
        try {
            if (lining) {
                applyFill(v);
                region.hide();
                return;
            }
            if (!canvas.isHardwareAccelerated() || !applyFill(v)) {
                region.hide();
                for (Region p : probes) p.hide();
                return;
            }
            drawGlass(v, canvas);
        } catch (Throwable e) {
            L.once("plate:" + what, "cannot draw the " + what + "'s glass; it stays stock", e);
            restore();
            region.hide();
        }
    }

    /** Makes the plate translucent, or puts it back if glass is off. Returns whether it has glass. */
    private boolean applyFill(View v) {
        try {
            // The plate: the background, seen through our own wrapper (WRAP)
            // and through framework wrappers (Material's card wraps its shape
            // in an InsetDrawable it may make anew; the shape inside lasts).
            Drawable outer = v.getBackground();
            if (outer instanceof AlphaPlate) outer = ((AlphaPlate) outer).inner;
            Drawable bg = unwrap(outer);
            if (bg != background) {
                restore();
                background = bg;
                measuredW = measuredH = -1;
                churn();
            }
            if (bg == null || unstable || !Gate.maps()) {
                restore();
                return false;
            }
            // A plate can be clear at one size and opaque at another (Maps'
            // search panel: a clear frame round the Search pill, a white sheet
            // once it shows suggestions). Its own opacity is measured, with
            // ours taken off, whenever its size changes; a clear one has no glass.
            int w = v.getWidth(), h = v.getHeight();
            if (w != measuredW || h != measuredH) {
                restore();
                int c = centrePixel(outer, w, h);
                natural = android.graphics.Color.alpha(c);
                strong = strong(c);
                corners = MapsCar.isTurnCard(v) ? corners(outer, w, h) : null;
                measuredW = w;
                measuredH = h;
            }
            if (natural < 0x80) {
                restore();
                return false;
            }
            // A coloured or dark plate (the green turn card) carries white
            // text: it keeps more of its colour so the text stays readable.
            float k = strong ? Math.max(Gate.fill(), STRONG_FILL) : Gate.fill();
            if (k != oursFor) {
                restore();
                ColorMatrix m = new ColorMatrix();
                m.setScale(1f, 1f, 1f, k);
                ours = new ColorMatrixColorFilter(m);
                oursFor = k;
            }
            // What is on is tracked here, not asked: RoundRectDrawable does not
            // report the filter it was given, and setting it again every frame
            // would invalidate the card every frame.
            if (mode == NONE && !choose(v, outer, bg, k)) return false;
            clearShadow(v);
            return true;
        } catch (Throwable e) {
            L.once("plate-fill:" + what, "cannot set the " + what + "'s fill; it stays stock", e);
            return false;
        }
    }

    private static final int NONE = 0, FILTER = 1, ALPHA = 2, WRAP = 3;
    private static final String[] MODES = {"none", "colour filter", "alpha", "alpha layer"};
    private int mode = NONE;
    private AlphaPlate wrapper;
    private long churnSince;
    private int churns;
    private boolean unstable, told;

    /**
     * Makes the plate translucent by the first means it honours, each checked
     * by drawing it: a colour filter that scales alpha (androidx CardView's
     * plate, which ignores setAlpha), then its alpha (a Material shape, which
     * may ignore the filter), then, for a plain view whose background ignores
     * both (Maps' speedometer: a LayerDrawable with a layer of Maps' own), a
     * wrapper that draws it into an alpha layer. CardViews never get the
     * wrapper: they own their background.
     */
    private boolean choose(View v, Drawable outer, Drawable bg, float k) {
        ColorFilter host = bg.getColorFilter();
        int limit = Math.round(k * 255f + 24f);
        if (host == null || host instanceof ColorMatrixColorFilter) {
            bg.mutate();
            bg.setColorFilter(ours);
            mode = FILTER;
            if (centreAlpha(outer, v.getWidth(), v.getHeight()) <= limit) return chosen(v);
            bg.setColorFilter(null);
        }
        bg.mutate();
        alphaWas = bg.getAlpha();
        bg.setAlpha(Math.round(k * alphaWas));
        mode = ALPHA;
        if (centreAlpha(outer, v.getWidth(), v.getHeight()) <= limit) return chosen(v);
        bg.setAlpha(alphaWas);
        mode = NONE;
        if (MapsCar.isCard(v)) {
            L.once("plate-stock:" + what, "the " + what + "'s plate ignores both a colour filter and its alpha;"
                    + " it stays stock", null);
            return false;
        }
        wrapper = new AlphaPlate(outer, Math.round(k * 255f));
        v.setBackground(wrapper);
        mode = WRAP;
        return chosen(v);
    }

    private boolean chosen(View v) {
        if (!told) {
            told = true;
            L.i("the " + what + "'s plate is made translucent with " + MODES[mode]);
        }
        v.invalidate();
        return true;
    }

    /** A plate whose drawable keeps changing (more than 10 times in 2 s) is left stock. */
    private void churn() {
        long now = android.os.SystemClock.uptimeMillis();
        if (now - churnSince > 2000) {
            churnSince = now;
            churns = 0;
        }
        if (++churns > 10 && !unstable) {
            unstable = true;
            L.w("the " + what + "'s background keeps changing; it stays stock");
        }
    }

    private static Drawable unwrap(Drawable d) {
        for (int i = 0; i < 4 && d instanceof android.graphics.drawable.DrawableWrapper; ++i) {
            Drawable in = ((android.graphics.drawable.DrawableWrapper) d).getDrawable();
            if (in == null) break;
            d = in;
        }
        return d;
    }

    /**
     * A card's elevation shadow is drawn under its whole outline, where the
     * opaque plate hid it; through a translucent one it darkens the glass.
     * The engine draws the pane's own edge.
     */
    private void clearShadow(View v) {
        if (shadowCleared) return;
        ambientShadow = v.getOutlineAmbientShadowColor();
        spotShadow = v.getOutlineSpotShadowColor();
        v.setOutlineAmbientShadowColor(0);
        v.setOutlineSpotShadowColor(0);
        shadowCleared = true;
    }

    private void restore() {
        View v = view.get();
        if (background != null) {
            if (mode == FILTER) background.setColorFilter(null);
            if (mode == ALPHA) background.setAlpha(alphaWas);
        }
        if (mode == WRAP && wrapper != null && v != null && v.getBackground() == wrapper) {
            v.setBackground(wrapper.inner);
        }
        wrapper = null;
        mode = NONE;
        if (shadowCleared && v != null) {
            v.setOutlineAmbientShadowColor(ambientShadow);
            v.setOutlineSpotShadowColor(spotShadow);
            shadowCleared = false;
        }
    }

    /** Draws a plate into an alpha layer: for a background that honours neither a filter nor its alpha. */
    static final class AlphaPlate extends Drawable implements Drawable.Callback {
        final Drawable inner;
        private final int alpha;

        AlphaPlate(Drawable inner, int alpha) {
            this.inner = inner;
            this.alpha = alpha;
            setBounds(inner.getBounds());
            inner.setCallback(this);
        }

        @Override
        public void draw(Canvas c) {
            Rect b = getBounds();
            int sc = c.saveLayerAlpha(b.left, b.top, b.right, b.bottom, alpha);
            inner.draw(c);
            c.restoreToCount(sc);
        }

        @Override
        protected void onBoundsChange(Rect b) {
            inner.setBounds(b);
        }

        @Override
        public boolean isStateful() {
            return inner.isStateful();
        }

        @Override
        protected boolean onStateChange(int[] state) {
            return inner.setState(state);
        }

        @Override
        public void jumpToCurrentState() {
            inner.jumpToCurrentState();
        }

        @Override
        public boolean setVisible(boolean visible, boolean restart) {
            inner.setVisible(visible, restart);
            return super.setVisible(visible, restart);
        }

        @Override
        public void getOutline(Outline o) {
            inner.getOutline(o);
        }

        @Override
        public boolean getPadding(Rect p) {
            return inner.getPadding(p);
        }

        @Override
        public int getIntrinsicWidth() {
            return inner.getIntrinsicWidth();
        }

        @Override
        public int getIntrinsicHeight() {
            return inner.getIntrinsicHeight();
        }

        @Override
        public void setHotspot(float x, float y) {
            inner.setHotspot(x, y);
        }

        @Override
        public void setAlpha(int a) {}

        @Override
        public void setColorFilter(ColorFilter f) {}

        @Override
        public int getOpacity() {
            return PixelFormat.TRANSLUCENT;
        }

        @Override
        public void invalidateDrawable(Drawable d) {
            invalidateSelf();
        }

        @Override
        public void scheduleDrawable(Drawable d, Runnable what, long when) {
            scheduleSelf(what, when);
        }

        @Override
        public void unscheduleDrawable(Drawable d, Runnable what) {
            unscheduleSelf(what);
        }
    }

    private void drawGlass(View v, Canvas canvas) {
        rect.set(0, 0, v.getWidth(), v.getHeight());
        float r = Math.min(rect.width(), rect.height()) / 2f;
        float tl = r, tr = r, br = r, bl = r;
        ViewOutlineProvider provider = v.getOutlineProvider();
        if (provider != null) {
            outline.setEmpty();
            provider.getOutline(v, outline);
            float radius = outline.getRadius();
            if (radius >= 0 && outline.getRect(tmp) && !tmp.isEmpty()) {
                rect.set(tmp);
                r = Math.min(radius, Math.min(rect.width(), rect.height()) / 2f);
                tl = tr = br = bl = r;
            } else if (corners != null) {
                float m = Math.min(rect.width(), rect.height()) / 2f;
                tl = Math.min(corners[0], m);
                tr = Math.min(corners[1], m);
                br = Math.min(corners[2], m);
                bl = Math.min(corners[3], m);
            }
        }
        if (rect.isEmpty()) {
            region.hide();
            return;
        }
        rect.offset(v.getScrollX(), v.getScrollY()); // see ViewGlass: the overlay draws scrolled
        region.draw(v, canvas, rect.left, rect.top, rect.right, rect.bottom, tl, tr, br, bl,
                Gate.radius(), false, 1f);
        drawProbes(v, canvas);
    }

    /**
     * Test only: N more regions of 4 x 4 px in a row along the plate's top
     * edge. Android 17 draws no blur for a layer once the output's blur
     * requests from unprivileged owners pass ten (Output.cpp kMaxBlurRequests),
     * and Maps is not privileged; this finds where that bites here.
     */
    private void drawProbes(View v, Canvas canvas) {
        int n = this == prober ? Gate.probe() : 0;
        while (probes.size() < n) {
            Region p = new Region();
            p.ownedBy(v);
            probes.add(p);
        }
        for (int i = 0; i < probes.size(); ++i) {
            Region p = probes.get(i);
            if (i >= n) {
                p.hide();
                continue;
            }
            int x = rect.left + 2 + 5 * i;
            p.draw(v, canvas, x, rect.top + 2, x + 4, rect.top + 6, 0, 0, 0, 0,
                    Gate.probeRadii() ? 20 + 2 * i : Gate.radius(), false, 1f);
        }
    }

    /** Alpha of the middle of {@code d} drawn at w x h, 0..255; 0 if it cannot be drawn. */
    static int centreAlpha(Drawable d, int w, int h) {
        return android.graphics.Color.alpha(centrePixel(d, w, h));
    }

    /** Saturated or dark: HSV saturation over 0.35, or luma under 0.45. */
    private static boolean strong(int c) {
        float[] hsv = new float[3];
        android.graphics.Color.colorToHSV(c, hsv);
        int r = android.graphics.Color.red(c), g = android.graphics.Color.green(c), b = android.graphics.Color.blue(c);
        float luma = (0.2126f * r + 0.7152f * g + 0.0722f * b) / 255f;
        return hsv[1] > 0.35f || luma < 0.45f;
    }

    /**
     * Each corner's radius, from where the drawn plate becomes opaque along the
     * corner's diagonal: a circular corner of radius r is clear for
     * r (1 - 1/sqrt 2) of it. Drawn at a quarter size; once per plate size.
     */
    private static float[] corners(Drawable d, int w, int h) {
        int s = 4, bw = Math.max(1, w / s), bh = Math.max(1, h / s);
        Rect old = d.copyBounds();
        try {
            android.graphics.Bitmap bm = android.graphics.Bitmap.createBitmap(bw, bh, android.graphics.Bitmap.Config.ARGB_8888);
            Canvas c = new Canvas(bm);
            c.scale((float) bw / w, (float) bh / h);
            if (old.width() != w || old.height() != h) d.setBounds(0, 0, w, h);
            d.draw(c);
            int max = Math.min(bw, bh) / 2;
            int[][] from = {{0, 0, 1, 1}, {bw - 1, 0, -1, 1}, {bw - 1, bh - 1, -1, -1}, {0, bh - 1, 1, -1}};
            float[] r = new float[4];
            for (int k = 0; k < 4; ++k) {
                int i = 0;
                while (i < max && android.graphics.Color.alpha(
                        bm.getPixel(from[k][0] + from[k][2] * i, from[k][1] + from[k][3] * i)) < 0x80) ++i;
                r[k] = i * s / 0.2929f;
            }
            bm.recycle();
            return r;
        } catch (Throwable t) {
            return null;
        } finally {
            if (!d.getBounds().equals(old)) d.setBounds(old);
        }
    }

    /** The middle pixel of {@code d} drawn at w x h (ARGB); 0 if it cannot be drawn. */
    static int centrePixel(Drawable d, int w, int h) {
        if (w <= 0 || h <= 0) return 0;
        Rect old = d.copyBounds();
        try {
            android.graphics.Bitmap bm = android.graphics.Bitmap.createBitmap(9, 9, android.graphics.Bitmap.Config.ARGB_8888);
            Canvas c = new Canvas(bm);
            c.scale(9f / w, 9f / h);
            if (old.width() != w || old.height() != h) d.setBounds(0, 0, w, h);
            d.draw(c);
            int a = bm.getPixel(4, 4);
            bm.recycle();
            return a;
        } catch (Throwable t) {
            return 0;
        } finally {
            if (!d.getBounds().equals(old)) d.setBounds(old);
        }
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
