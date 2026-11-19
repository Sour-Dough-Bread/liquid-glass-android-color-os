package dev.stockglass.companion;

import android.app.Dialog;
import android.content.res.ColorStateList;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.ColorFilter;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.DrawableWrapper;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.LayerDrawable;
import android.graphics.drawable.StateListDrawable;
import android.os.SystemClock;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewTreeObserver;
import android.view.Window;
import android.view.WindowManager;
import de.robv.android.xposed.XC_MethodHook;
import de.robv.android.xposed.XposedBridge;
import java.lang.ref.WeakReference;
import java.util.WeakHashMap;

/**
 * SystemUI's own dialogs: the popups the Quick Settings tiles open (Internet,
 * Bluetooth, Do Not Disturb, Cast, data saver, media output, the active apps
 * list, the user switcher) and the confirmations they open in turn.
 *
 * Read from AxionOS's SystemUI (A17) and its framework.jar with jadx. These
 * dialogs draw no blur. Their plate is the window background of
 * Theme.SystemUI.Dialog: the framework's dialog_background_material, an
 * InsetDrawable (16 dp) around a rounded GradientDrawable (28 dp corners)
 * filled with materialColorSurfaceBright. A dialog shown without a launch
 * animation keeps it on its DecorView. One launched from a tile
 * (DialogTransitionAnimator.show) has it moved to a LaunchableFrameLayout
 * inside the window, tagged R.id.tag_dialog_background, with the window
 * itself made transparent and full-screen.
 *
 * An after-hook on Dialog.show finds that plate: the tagged view, else the
 * DecorView, provided its background holds a solid, rectangular
 * GradientDrawable. That leaves out the bottom sheets, the screen capture
 * UI, the volume dialogs and the power menu, which draw their own, and the
 * power menu's options dialog, which blurs behind itself.
 *
 * The plate's translucency goes into the GradientDrawable's colour, not its
 * alpha. The launch and exit morph (GhostedViewTransitionAnimatorController)
 * draws this same drawable, setting its alpha to 255 and then 0 every frame
 * and restoring the alpha it found at the end; ViewGlass's setAlpha would
 * fight that. A colour is left alone by it.
 *
 * Glass only while the plate is settled. During the morph the plate is
 * hidden (transition visibility INVISIBLE, drawn through a GhostView) and
 * the morph draws an opaque layer of the colour the animator read before
 * the dialog was shown, with nothing under it that could blur. So the plate
 * stays opaque while it is hidden, and when it shows the glass fades in
 * (FADE_MS) as the plate's colour fades to app.fill of its alpha. The exit
 * morph hides the plate again, which puts the opaque colour back before the
 * morph's first frame. The region is at the GradientDrawable's own bounds
 * and corner radii, not the view's outline: after a launch the drawable is
 * left with a radius array, whose outline is a path, and the outline would
 * then be the whole window.
 *
 * The plate can change under it: Internet and Bluetooth animate its bounds
 * for 500 ms after a layout change (the rect is read on every draw), media
 * output tints it at run time and a night-mode switch installs a new
 * drawable (both read as the host's new colour). The dim behind the dialog
 * (0.6) stays, so the glass blurs a dimmed shade.
 *
 * After-hook: the decor exists only once show() has created the dialog, and
 * the animator tags the plate before it calls show(). LSPosed only.
 */
final class DialogGlass extends Drawable {
    private static final long FADE_MS = 180;
    private static final WeakHashMap<View, DialogGlass> all = new WeakHashMap<>();
    private static boolean installed, listening;
    private static int tagId = -1;

    private final WeakReference<View> view;
    private final Region region = new Region();
    /** The plate view's background as last seen, and the GradientDrawable in it. */
    private Drawable background;
    private GradientDrawable plate;
    /** The plate's colour as the host set it, and the colour this gave it (null: none). */
    private ColorStateList original, setTo;
    /** When the plate was first seen settled, -1 while it is not; the fade, 0..1. */
    private long settledAt = -1;
    private float shown;
    private boolean shadowCleared;
    private int ambientShadow, spotShadow;
    private final ViewTreeObserver.OnPreDrawListener preDraw = () -> {
        View v = DialogGlass.this.view.get();
        if (v != null) apply(v);
        return true;
    };

    private DialogGlass(View v) {
        view = new WeakReference<>(v);
        region.ownedBy(v);
    }

    static synchronized void install(ClassLoader cl) {
        if (installed) return;
        installed = true;
        try {
            XposedBridge.hookMethod(Dialog.class.getDeclaredMethod("show"), new XC_MethodHook() {
                @Override
                protected void afterHookedMethod(MethodHookParam param) {
                    if (param.hasThrowable()) return;
                    try {
                        Region.reap();
                        Window w = ((Dialog) param.thisObject).getWindow();
                        // A window that blurs behind itself (the power menu's
                        // options) is left as it was designed.
                        if (w == null || (w.getAttributes().flags
                                & WindowManager.LayoutParams.FLAG_BLUR_BEHIND) != 0) return;
                        View decor = w.peekDecorView();
                        View p = decor == null ? null : plateOf(decor);
                        if (p != null) attach(p);
                    } catch (Throwable e) {
                        L.once("dialogs", "dialog hook failed; SystemUI's dialogs stay stock", e);
                    }
                }
            });
        } catch (Throwable t) {
            L.w("no Dialog.show (" + t + "); SystemUI's dialogs stay stock");
            return;
        }
        L.i("dialogs hooked: Dialog.show");
    }

    /** The view that draws the dialog's rounded plate, or null if it has none. */
    private static View plateOf(View decor) {
        if (tagId < 0) {
            tagId = decor.getResources().getIdentifier("tag_dialog_background", "id", "com.android.systemui");
        }
        View tagged = tagId != 0 ? tagged(decor, tagId) : null;
        View p = tagged != null ? tagged : decor;
        GradientDrawable g = gradient(p.getBackground());
        if (g == null || g.getShape() != GradientDrawable.RECTANGLE) return null;
        ColorStateList c = g.getColor();
        return c != null && Color.alpha(c.getDefaultColor()) > 0 ? p : null;
    }

    private static View tagged(View v, int id) {
        if (v.getTag(id) != null) return v;
        if (v instanceof ViewGroup) {
            ViewGroup g = (ViewGroup) v;
            for (int i = 0; i < g.getChildCount(); i++) {
                View t = tagged(g.getChildAt(i), id);
                if (t != null) return t;
            }
        }
        return null;
    }

    /** The GradientDrawable a background draws, as SystemUI's own animator finds it. */
    private static GradientDrawable gradient(Drawable d) {
        if (d instanceof GradientDrawable) return (GradientDrawable) d;
        if (d instanceof DrawableWrapper) return gradient(((DrawableWrapper) d).getDrawable());
        if (d instanceof StateListDrawable) return gradient(d.getCurrent());
        if (d instanceof LayerDrawable) {
            LayerDrawable l = (LayerDrawable) d;
            for (int i = 0; i < l.getNumberOfLayers(); i++) {
                GradientDrawable g = gradient(l.getDrawable(i));
                if (g != null) return g;
            }
        }
        return null;
    }

    private static void attach(View v) {
        if (all.containsKey(v)) return;
        DialogGlass g = new DialogGlass(v);
        all.put(v, g);
        g.setBounds(0, 0, Math.max(1, v.getWidth()), Math.max(1, v.getHeight()));
        v.getOverlay().add(g);
        v.addOnAttachStateChangeListener(new View.OnAttachStateChangeListener() {
            @Override
            public void onViewAttachedToWindow(View view) {
                view.getViewTreeObserver().addOnPreDrawListener(g.preDraw);
            }

            @Override
            public void onViewDetachedFromWindow(View view) {
                view.getViewTreeObserver().removeOnPreDrawListener(g.preDraw);
                g.settledAt = -1; // shown again later: fade in again
                g.shown = 0f;
            }
        });
        if (v.isAttachedToWindow()) v.getViewTreeObserver().addOnPreDrawListener(g.preDraw);
        v.invalidate();
        if (!listening) {
            listening = true;
            Gate.onChange(DialogGlass::gateChanged);
        }
        if (Gate.debug()) L.i("dialog: glass attached to " + v.getClass().getSimpleName());
    }

    /** Main thread. */
    private static void gateChanged() {
        for (View v : all.keySet().toArray(new View[0])) {
            if (v != null) v.invalidate();
        }
    }

    /**
     * Gives the plate its glass colour for this frame, or puts its own back.
     * Runs before each draw of the window; idempotent.
     */
    private void apply(View v) {
        try {
            Drawable bg = v.getBackground();
            if (bg != background) {
                restore();
                background = bg;
                plate = null;
                original = null;
            }
            if (bg == null || !Gate.dialogs()) {
                restore();
                unsettle();
                return;
            }
            if (plate == null) {
                // Its own state, so the colour does not reach every dialog
                // drawn from the same resource (and Resources' cache).
                bg.mutate();
                plate = gradient(bg);
                if (plate == null || plate.getShape() != GradientDrawable.RECTANGLE) {
                    plate = null;
                    return;
                }
            }
            ColorStateList cur = plate.getColor();
            if (cur == null) {
                setTo = null;
                unsettle();
                return;
            }
            if (cur != setTo) {
                original = cur; // first sight, or the host set a colour of its own since
                setTo = null;
            }
            if (v.getVisibility() != View.VISIBLE || v.getWidth() <= 0 || v.getHeight() <= 0) {
                restore(); // a morph is drawing it: opaque, as the morph's own layer is
                unsettle();
                return;
            }
            long now = SystemClock.uptimeMillis();
            if (settledAt < 0) settledAt = now;
            shown = Math.min(1f, (now - settledAt) / (float) FADE_MS);
            float keep = 1f - (1f - Math.max(0f, Math.min(1f, Gate.fill()))) * shown;
            int a = Math.round(Color.alpha(original.getDefaultColor()) * keep);
            if (setTo == null || Color.alpha(setTo.getDefaultColor()) != a) {
                ColorStateList want = original.withAlpha(a);
                plate.setColor(want);
                setTo = plate.getColor();
            }
            clearShadow(v);
            if (shown < 1f) v.postInvalidateOnAnimation();
        } catch (Throwable e) {
            L.once("dialog-fill", "cannot set a dialog's fill; it stays stock", e);
            restore();
            unsettle();
        }
    }

    private void unsettle() {
        settledAt = -1;
        shown = 0f;
    }

    /** Puts the plate's own colour and shadow back. */
    private void restore() {
        if (plate != null && setTo != null && original != null && plate.getColor() == setTo) {
            plate.setColor(original);
        }
        setTo = null;
        View v = view.get();
        if (shadowCleared && v != null) {
            v.setOutlineAmbientShadowColor(ambientShadow);
            v.setOutlineSpotShadowColor(spotShadow);
        }
        shadowCleared = false;
    }

    /**
     * A framework-themed dialog (an AlertDialog.Builder confirmation) may keep
     * its elevation; the shadow under a translucent plate darkens the glass.
     */
    private void clearShadow(View v) {
        if (shadowCleared) return;
        ambientShadow = v.getOutlineAmbientShadowColor();
        spotShadow = v.getOutlineSpotShadowColor();
        v.setOutlineAmbientShadowColor(0);
        v.setOutlineSpotShadowColor(0);
        shadowCleared = true;
    }

    @Override
    public void draw(Canvas canvas) {
        View v = view.get();
        if (v == null) return;
        try {
            GradientDrawable p = plate;
            // Through a GhostView the plate is drawn while hidden: no glass there.
            if (!canvas.isHardwareAccelerated() || p == null || setTo == null || shown <= 0f
                    || v.getVisibility() != View.VISIBLE) {
                region.hide();
                return;
            }
            Rect b = p.getBounds();
            if (b.isEmpty()) {
                region.hide();
                return;
            }
            float max = Math.min(b.width(), b.height()) / 2f;
            float tl, tr, br, bl;
            float[] r = p.getCornerRadii();
            if (r != null && r.length >= 8) {
                tl = r[0];
                tr = r[2];
                br = r[4];
                bl = r[6];
            } else {
                tl = tr = br = bl = p.getCornerRadius();
            }
            // The plate is a background, drawn with the view's scroll undone;
            // this overlay draws in the scrolled space (see ViewGlass.drawGlass).
            int sx = v.getScrollX(), sy = v.getScrollY();
            region.draw(v, canvas, b.left + sx, b.top + sy, b.right + sx, b.bottom + sy,
                    Math.min(tl, max), Math.min(tr, max), Math.min(br, max), Math.min(bl, max),
                    Gate.radius(), false, shown);
        } catch (Throwable e) {
            L.once("dialog-glass", "cannot draw a dialog's glass; it stays stock", e);
            restore();
            region.hide();
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
