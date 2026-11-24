package dev.stockglass.companion;

import android.graphics.RenderEffect;
import android.view.View;
import de.robv.android.xposed.XC_MethodHook;
import de.robv.android.xposed.XposedBridge;
import java.lang.reflect.Method;
import java.util.Map;
import java.util.WeakHashMap;

/**
 * ColorOS 17's COUI material frost as glass, in the processes this is
 * installed in (the launcher: the drawer's search pill, All/Categories tab
 * and colour-filter button).
 *
 * COUI views ask for frost through COUIMaterialBlurEffect, which ends in the
 * framework's com.oplus.view.OplusViewBackgroundRenderEffect
 * .setBackgroundRenderEffect(RenderEffect, View): a backdrop RenderEffect
 * (a blur and two blend colours) on the view's own RenderNode, drawn in the
 * app's process, clipped to the view's outline. Null takes it away.
 *
 * With glass on, a non-null effect is replaced by null (no frost) and the
 * view gets ViewGlass in plateless mode: a region at its outline, with the
 * outline's corners, that the engine draws as glass. A null from the app
 * takes the glass away too. Each view's last requested effect is kept, so
 * switching glass off gives the views their frost back.
 */
final class CouiGlass {
    private static final String CLASS = "com.oplus.view.OplusViewBackgroundRenderEffect";
    private static boolean installed;
    private static Method set;
    /** Each view's frost as the app last asked for it; null: none. */
    private static final WeakHashMap<View, RenderEffect> asked = new WeakHashMap<>();
    /** The views whose frost is replaced now. */
    private static final WeakHashMap<View, Boolean> replaced = new WeakHashMap<>();

    private CouiGlass() {}

    static synchronized void install(ClassLoader cl) {
        if (installed) return;
        installed = true;
        try {
            set = Class.forName(CLASS, false, cl).getDeclaredMethod("setBackgroundRenderEffect",
                    RenderEffect.class, View.class);
        } catch (Throwable t) {
            return; // not ColorOS
        }
        XposedBridge.hookMethod(set, new XC_MethodHook() {
            @Override
            protected void beforeHookedMethod(MethodHookParam param) {
                try {
                    View v = (View) param.args[1];
                    if (v == null) return;
                    RenderEffect e = (RenderEffect) param.args[0];
                    asked.put(v, e);
                    if (e == null) {
                        replaced.remove(v);
                        redraw(v);
                    } else if (on(v)) {
                        if (v.getWidth() <= 0) {
                            afterLayout(v); // the frost stays until the view's size is known
                        } else if (!backdrop(v)) {
                            param.args[0] = null;
                            glass(v);
                        }
                    }
                } catch (Throwable t) {
                    L.once("coui", "COUI frost hook failed; that frost stays stock", t);
                }
            }
        });
        Gate.onChange(CouiGlass::gateChanged);
        L.i("COUI frost hooked: OplusViewBackgroundRenderEffect.setBackgroundRenderEffect");
        installDrawerTab(cl);
    }

    private static final String SEGMENT = "com.coui.appcompat.segmentbutton.COUISegmentButtonLayout";
    private static final String TAB = "com.android.launcher3.allapps.OplusAllAppCategoryTabView";
    private static final WeakHashMap<View, Region> tabs = new WeakHashMap<>();

    /**
     * The drawer's All/Categories switch (OplusAllAppCategoryTabView, a
     * COUISegmentButtonLayout) is not frost: its onDraw paints the pill into
     * mTmpRectF, radius half its height, with the paints the tab hands out
     * from getCustomBackgroundPaint (new ones on every call). A region goes
     * under the pill before onDraw; those paints keep app.fill of their alpha.
     */
    private static void installDrawerTab(ClassLoader cl) {
        final java.lang.reflect.Field rectField;
        final Class<?> tab;
        Method onDraw, paints;
        try {
            Class<?> seg = Class.forName(SEGMENT, false, cl);
            tab = Class.forName(TAB, false, cl);
            onDraw = seg.getDeclaredMethod("onDraw", android.graphics.Canvas.class);
            paints = tab.getDeclaredMethod("getCustomBackgroundPaint");
            rectField = seg.getDeclaredField("mTmpRectF");
            rectField.setAccessible(true);
        } catch (Throwable t) {
            return; // not OplusLauncher's drawer
        }
        XposedBridge.hookMethod(onDraw, new XC_MethodHook() {
            @Override
            protected void beforeHookedMethod(MethodHookParam param) {
                if (!tab.isInstance(param.thisObject)) return;
                try {
                    Region.reap();
                    View v = (View) param.thisObject;
                    Region r = tabs.get(v);
                    if (r == null) {
                        r = new Region();
                        r.ownedBy(v);
                        tabs.put(v, r);
                    }
                    android.graphics.Canvas c = (android.graphics.Canvas) param.args[0];
                    android.graphics.RectF f = (android.graphics.RectF) rectField.get(v);
                    if (!Gate.drawer() || f == null || f.isEmpty() || !c.isHardwareAccelerated()) {
                        r.hide();
                        return;
                    }
                    float rad = f.height() / 2f;
                    r.draw(v, c, Math.round(f.left), Math.round(f.top), Math.round(f.right),
                            Math.round(f.bottom), rad, rad, rad, rad, Gate.radius(), false);
                } catch (Throwable t) {
                    L.once("coui-tab", "drawer tab hook failed; the tab stays stock", t);
                }
            }
        });
        XposedBridge.hookMethod(paints, new XC_MethodHook() {
            @Override
            protected void afterHookedMethod(MethodHookParam param) {
                try {
                    if (!Gate.drawer()) return;
                    Object res = param.getResult();
                    if (!(res instanceof android.graphics.Paint[])) return;
                    for (android.graphics.Paint p : (android.graphics.Paint[]) res) {
                        if (p != null) p.setAlpha(Math.round(p.getAlpha() * Gate.fill()));
                    }
                } catch (Throwable t) {
                    L.once("coui-tab-paint", "cannot thin the drawer tab's fill", t);
                }
            }
        });
        Gate.onChange(() -> {
            for (Map.Entry<View, Region> e : tabs.entrySet().toArray(new Map.Entry[0])) {
                if (!Gate.drawer()) e.getValue().hide();
                e.getKey().invalidate();
            }
        });
        L.i("drawer tab hooked: COUISegmentButtonLayout.onDraw");
    }

    private static void glass(View v) {
        replaced.put(v, Boolean.TRUE);
        ViewGlass.attach(v, () -> wanted(v), x -> 0f, "COUI frost").plateless();
        redraw(v);
    }

    /**
     * A view about as wide as its window is a backdrop behind content (the
     * drawer's header fade), not a control's plate: its frost stays.
     */
    private static boolean backdrop(View v) {
        View root = v.getRootView();
        return root != null && root.getWidth() > 0 && v.getWidth() >= 0.9f * root.getWidth();
    }

    /** Decides once the view has been laid out, for frost asked before that. */
    private static void afterLayout(View v) {
        v.addOnLayoutChangeListener(new View.OnLayoutChangeListener() {
            @Override
            public void onLayoutChange(View view, int l, int t, int r, int b, int ol, int ot, int or, int ob) {
                if (r - l <= 0) return;
                view.removeOnLayoutChangeListener(this);
                try {
                    RenderEffect effect = asked.get(view);
                    if (effect == null || replaced.containsKey(view) || !on(view) || backdrop(view)) return;
                    XposedBridge.invokeOriginalMethod(set, null, new Object[] {null, view});
                    glass(view);
                } catch (Throwable e) {
                    L.once("coui-layout", "cannot give a COUI view glass after layout", e);
                }
            }
        });
    }

    private static boolean wanted(View v) {
        return replaced.containsKey(v) && on(v);
    }

    private static boolean on(View v) {
        return v.getClass().getName().contains(".allapps.") || inDrawer(v) ? Gate.drawer() : Gate.frost();
    }

    private static boolean inDrawer(View v) {
        for (Object p = v.getParent(); p instanceof View; p = ((View) p).getParent()) {
            if (p.getClass().getName().contains(".allapps.")) return true;
        }
        return false;
    }

    private static void redraw(View v) {
        v.invalidate();
    }

    /** Main thread. Gives each view the frost or the glass its switch now says. */
    private static void gateChanged() {
        for (Map.Entry<View, RenderEffect> e : asked.entrySet().toArray(new Map.Entry[0])) {
            View v = e.getKey();
            RenderEffect effect = e.getValue();
            if (effect == null) continue;
            try {
                if (on(v) && v.getWidth() > 0 && !backdrop(v)) {
                    if (!replaced.containsKey(v)) {
                        XposedBridge.invokeOriginalMethod(set, null, new Object[] {null, v});
                        glass(v);
                    }
                } else if (replaced.remove(v) != null) {
                    XposedBridge.invokeOriginalMethod(set, null, new Object[] {effect, v});
                    redraw(v);
                }
            } catch (Throwable t) {
                L.once("coui-switch", "cannot switch a COUI view between frost and glass", t);
            }
        }
    }
}
