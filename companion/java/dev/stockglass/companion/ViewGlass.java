package dev.stockglass.companion;

import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.Outline;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.Drawable;
import android.view.View;
import android.view.ViewOutlineProvider;
import android.view.ViewTreeObserver;
import java.lang.ref.WeakReference;
import java.util.WeakHashMap;

/**
 * Glass under a view whose own background is the control's plate: a popup's
 * container, a search box, a panel. The region is drawn from a Drawable in
 * the view's ViewOverlay, at the view's bounds (narrowed to its outline) with
 * the outline's corner radius, and the background is drawn at a fraction of
 * its own alpha, so the glass shows through it. The overlay draws with the
 * view's display list, so the region follows the view through scrolling,
 * animation and its window; the content stays as it is.
 *
 * The host may set the background's alpha itself (a search box hides its
 * pill while search is open). Each time it does, that alpha becomes the
 * background's own, the fraction is taken of it, and the glass follows it:
 * at the highest alpha seen it is at full strength, at 0 it is gone. The
 * overlay draws after the background, so the alpha is applied in a pre-draw
 * listener, after the host's animations have run and before the background
 * is drawn; otherwise every frame of a host's fade would show its own alpha.
 *
 * Main thread, like the views.
 */
final class ViewGlass extends Drawable {
    /** Whether this kind of control gets glass now (a Gate switch). */
    interface Switch {
        boolean on();
    }

    /** How much of the background's alpha to keep, 0..1. */
    interface Keep {
        float of(View v);
    }

    private static final WeakHashMap<View, ViewGlass> all = new WeakHashMap<>();
    private static boolean listening;

    private final WeakReference<View> view;
    private final Switch on;
    private final Keep keep;
    private final String what;
    private boolean shadowless, fillOnly;
    private int ambientShadow, spotShadow;
    private boolean shadowCleared;
    final Region region = new Region();
    private Drawable background;
    /** The background's alpha as the host set it, -1 before it is read. */
    private int originalAlpha = -1;
    /** The alpha this gave the background, as the background reports it back. */
    private int setTo = -1;
    /** The highest alpha the host has given this background: the glass's full strength. */
    private int peak;
    private final Rect rect = new Rect(), tmp = new Rect();
    private final Outline outline = new Outline();
    private final ViewTreeObserver.OnPreDrawListener preDraw = () -> {
        View v = ViewGlass.this.view.get();
        if (v != null) applyAlpha(v);
        return true;
    };

    private ViewGlass(View v, Switch on, Keep keep, String what) {
        view = new WeakReference<>(v);
        this.on = on;
        this.keep = keep;
        this.what = what;
        region.ownedBy(v);
    }

    /** Gives {@code v} glass, once; later calls return the same one. */
    static ViewGlass attach(View v, Switch on, Keep keep, String what) {
        ViewGlass g = all.get(v);
        if (g != null) return g;
        g = new ViewGlass(v, on, keep, what);
        all.put(v, g);
        g.setBounds(0, 0, Math.max(1, v.getWidth()), Math.max(1, v.getHeight()));
        v.getOverlay().add(g);
        // A window's tree observer is the window's: a view added to another
        // window (a screenshot UI shown again) needs the listener there too.
        final ViewGlass glass = g;
        v.addOnAttachStateChangeListener(new View.OnAttachStateChangeListener() {
            @Override
            public void onViewAttachedToWindow(View view) {
                view.getViewTreeObserver().addOnPreDrawListener(glass.preDraw);
            }

            @Override
            public void onViewDetachedFromWindow(View view) {
                view.getViewTreeObserver().removeOnPreDrawListener(glass.preDraw);
            }
        });
        v.getViewTreeObserver().addOnPreDrawListener(g.preDraw);
        v.invalidate();
        if (!listening) {
            listening = true;
            Gate.onChange(ViewGlass::gateChanged);
        }
        if (Gate.debug()) L.i(what + ": glass attached to " + v.getClass().getSimpleName());
        return g;
    }

    /**
     * Also hides the view's elevation shadow while it has glass. A shadow is
     * drawn under the whole outline, where an opaque background hides it; a
     * translucent one lets it through, and the glass came out darker than the
     * background it replaced. The engine draws the pane's own edge.
     */
    ViewGlass shadowless() {
        shadowless = true;
        return this;
    }

    /**
     * Only the background's alpha, no region of its own: a piece of a control
     * that sits on another view's glass (the pill inside a search bar's ring).
     */
    ViewGlass fillOnly() {
        fillOnly = true;
        region.hide();
        return this;
    }

    /** Also fades the glass by {@code f} (see Region.fadeWith). */
    ViewGlass fadeWith(Region.Fade f) {
        region.fadeWith(f);
        return this;
    }

    private void clearShadow(View v) {
        if (!shadowless || shadowCleared) return;
        ambientShadow = v.getOutlineAmbientShadowColor();
        spotShadow = v.getOutlineSpotShadowColor();
        v.setOutlineAmbientShadowColor(0);
        v.setOutlineSpotShadowColor(0);
        shadowCleared = true;
    }

    private void restoreShadow() {
        View v = view.get();
        if (!shadowCleared || v == null) return;
        v.setOutlineAmbientShadowColor(ambientShadow);
        v.setOutlineSpotShadowColor(spotShadow);
        shadowCleared = false;
    }

    /** Main thread. Redraws every view with glass so it picks up a switch. */
    private static void gateChanged() {
        for (ViewGlass g : all.values().toArray(new ViewGlass[0])) g.redraw();
    }

    void redraw() {
        View v = view.get();
        if (v == null) return;
        setBounds(0, 0, Math.max(1, v.getWidth()), Math.max(1, v.getHeight()));
        invalidateSelf();
        v.invalidate();
    }

    @Override
    public void draw(Canvas canvas) {
        View v = view.get();
        if (v == null) return;
        try {
            if (!canvas.isHardwareAccelerated() || !applyAlpha(v)) {
                region.hide();
                return;
            }
            if (!fillOnly) drawGlass(v, canvas);
        } catch (Throwable e) {
            L.once("view-glass:" + what, "cannot draw the " + what + "'s glass; it stays stock", e);
            restore();
            region.hide();
        }
    }

    /**
     * Gives the background its glass alpha, or puts it back if glass is off.
     * Returns whether the view has glass now. Idempotent; runs before each draw.
     */
    private boolean applyAlpha(View v) {
        try {
            Drawable bg = v.getBackground();
            if (bg != background) {
                restore();
                background = bg;
                peak = 0;
            }
            if (bg == null || !on.on()) {
                restore();
                return false;
            }
            // A ColorDrawable reports its colour's alpha, not the modulation
            // setAlpha takes, so it counts as 255 and is never "changed by
            // the host"; mutate() so a shared constant state is not touched.
            boolean color = bg instanceof ColorDrawable;
            int cur = bg.getAlpha();
            if (originalAlpha < 0) {
                bg.mutate();
                originalAlpha = color ? 255 : cur;
            } else if (!color && cur != setTo) {
                originalAlpha = cur; // the host has set its own alpha since
            }
            peak = Math.max(peak, originalAlpha);
            int want = Math.round(originalAlpha * Math.max(0f, Math.min(1f, keep.of(v))));
            if (cur != want || color) bg.setAlpha(want);
            setTo = bg.getAlpha();
            clearShadow(v);
            return true;
        } catch (Throwable e) {
            L.once("view-glass-alpha:" + what, "cannot set the " + what + "'s fill; it stays stock", e);
            return false;
        }
    }

    private void drawGlass(View v, Canvas canvas) {
        rect.set(0, 0, v.getWidth(), v.getHeight());
        float r = Math.min(rect.width(), rect.height()) / 2f;
        ViewOutlineProvider provider = v.getOutlineProvider();
        if (provider != null) {
            outline.setEmpty();
            provider.getOutline(v, outline);
            float radius = outline.getRadius();
            if (radius >= 0 && outline.getRect(tmp) && !tmp.isEmpty()) {
                rect.set(tmp);
                r = Math.min(radius, Math.min(rect.width(), rect.height()) / 2f);
            }
        }
        if (rect.isEmpty() || peak <= 0) {
            region.hide();
            return;
        }
        // The overlay draws in the view's scrolled space; the background (and
        // so the outline) does not, View.drawBackground undoes the scroll. A
        // single-line centred TextView, which every Button is, scrolls by about
        // half of TextView's 1 << 20 px line: Overview's buttons put their
        // glass 524,000 px to the left until this was undone here too.
        rect.offset(v.getScrollX(), v.getScrollY());
        region.draw(v, canvas, rect.left, rect.top, rect.right, rect.bottom, r, r, r, r,
                Gate.radius(), false, originalAlpha / (float) peak);
    }

    /** Puts the background back as the view had it. */
    private void restore() {
        if (background != null && originalAlpha >= 0) background.setAlpha(originalAlpha);
        originalAlpha = -1;
        setTo = -1;
        restoreShadow();
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
