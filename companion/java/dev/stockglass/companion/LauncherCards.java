package dev.stockglass.companion;

import android.graphics.Canvas;
import android.graphics.Rect;
import android.graphics.drawable.Drawable;
import android.view.View;
import android.view.ViewGroup;
import de.robv.android.xposed.XC_MethodHook;
import de.robv.android.xposed.XposedBridge;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.ArrayDeque;
import java.util.Map;
import java.util.WeakHashMap;

/**
 * ColorOS 17's workspace cards (launcher3.card.TitleCardView: the steps
 * card and other Quick App cards rendered by org.hapjs), under app.widgets.
 *
 * A card that asks for a blurred background gets a BlurTransitionDrawable
 * from CardBackgroundBlurManager, which OplusFrost already turns into glass.
 * The others paint their own opaque plate: a view inside the renderer whose
 * background fills the card. TitleCardView.dispatchDraw clips the card to
 * mCardBounds (LauncherCardView) with corners of getRadiusArr(). A before-hook
 * there puts a region at mCardBounds with that radius and keeps the plate's
 * background at app.fill of its alpha, as widgets do; with the switch off
 * the plate gets its alpha back.
 */
final class LauncherCards {
    private static boolean installed;
    private static Field bounds;
    private static Method radii;
    private static final WeakHashMap<View, Region> regions = new WeakHashMap<>();
    /** Each thinned plate background and the alpha it had. */
    private static final WeakHashMap<Drawable, Integer> plates = new WeakHashMap<>();
    private static final WeakHashMap<View, Drawable> plateOf = new WeakHashMap<>();
    private static final Rect rect = new Rect();

    private LauncherCards() {}

    static synchronized void install(ClassLoader cl) {
        if (installed) return;
        installed = true;
        Method dispatchDraw;
        try {
            Class<?> card;
            try {
                card = Class.forName("com.android.launcher3.card.TitleCardView", false, cl);
            } catch (ClassNotFoundException e) {
                return; // not OplusLauncher
            }
            dispatchDraw = card.getDeclaredMethod("dispatchDraw", Canvas.class);
            bounds = null;
            for (Class<?> k = card; k != null && bounds == null; k = k.getSuperclass()) {
                try {
                    bounds = k.getDeclaredField("mCardBounds"); // LauncherCardView's
                } catch (NoSuchFieldException e) {
                    // declared further up
                }
            }
            if (bounds == null) throw new NoSuchFieldException("mCardBounds");
            bounds.setAccessible(true);
            radii = card.getMethod("getRadiusArr");
        } catch (Throwable t) {
            L.e("this launcher's cards are not the expected shape; cards left alone", t);
            return;
        }
        XposedBridge.hookMethod(dispatchDraw, new XC_MethodHook() {
            @Override
            protected void beforeHookedMethod(MethodHookParam param) {
                try {
                    Region.reap();
                    draw((ViewGroup) param.thisObject, (Canvas) param.args[0]);
                } catch (Throwable e) {
                    L.once("card", "card hook failed; cards stay stock", e);
                }
            }
        });
        Gate.onChange(LauncherCards::gateChanged);
        L.i("launcher cards hooked: TitleCardView.dispatchDraw");
    }

    private static void gateChanged() {
        boolean on = Gate.widgets();
        for (Map.Entry<View, Region> e : regions.entrySet().toArray(new Map.Entry[0])) {
            if (!on) {
                e.getValue().hide();
                restore(e.getKey());
            }
            e.getKey().invalidate();
        }
    }

    private static void draw(ViewGroup card, Canvas canvas) throws Exception {
        Region region = regions.get(card);
        if (region == null) {
            region = new Region();
            region.ownedBy(card);
            region.fadeWith(Drawer.INSTANCE);
            regions.put(card, region);
        }
        rect.set((Rect) bounds.get(card));
        if (!Gate.widgets() || rect.isEmpty() || !canvas.isHardwareAccelerated()) {
            region.hide();
            restore(card);
            return;
        }
        Drawable plate = findPlate(card, rect.width(), rect.height());
        if (plate == null) {
            // Nothing opaque to thin (a frosted card is OplusFrost's): no glass here.
            region.hide();
            return;
        }
        Drawable was = plateOf.get(card);
        if (was != plate) {
            restore(card);
            plateOf.put(card, plate);
        }
        Integer original = plates.get(plate);
        if (original == null) {
            plate.mutate();
            original = plate.getAlpha();
            plates.put(plate, original);
        }
        int want = Math.round(original * Gate.fill());
        if (plate.getAlpha() != want) plate.setAlpha(want);
        float[] r = (float[]) radii.invoke(card);
        float corner = r != null && r.length > 0 ? r[0] : 0f;
        corner = Math.max(0f, Math.min(corner, Math.min(rect.width(), rect.height()) / 2f));
        region.draw(card, canvas, rect.left, rect.top, rect.right, rect.bottom, corner, corner, corner, corner,
                Gate.radius(), false);
    }

    /** The card's plate: the first background, breadth first, about as big as the card. */
    private static Drawable findPlate(ViewGroup card, int w, int h) {
        ArrayDeque<View> queue = new ArrayDeque<>();
        for (int i = 0; i < card.getChildCount(); i++) queue.add(card.getChildAt(i));
        int seen = 0;
        while (!queue.isEmpty() && seen++ < 64) {
            View v = queue.poll();
            if (v.getVisibility() != View.VISIBLE) continue;
            Drawable bg = v.getBackground();
            if (bg != null && v.getWidth() >= 0.9f * w && v.getHeight() >= 0.9f * h) return bg;
            if (v instanceof ViewGroup) {
                ViewGroup g = (ViewGroup) v;
                for (int i = 0; i < g.getChildCount(); i++) queue.add(g.getChildAt(i));
            }
        }
        return null;
    }

    private static void restore(View card) {
        Drawable d = plateOf.remove(card);
        if (d == null) return;
        Integer a = plates.remove(d);
        if (a != null) d.setAlpha(a);
    }
}
