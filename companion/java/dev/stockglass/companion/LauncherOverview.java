package dev.stockglass.companion;

import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import de.robv.android.xposed.XC_MethodHook;
import de.robv.android.xposed.XposedBridge;
import java.lang.reflect.Method;

/**
 * The buttons in Overview (recents): the action row under the task cards and
 * the Clear all button at the end of the cards.
 *
 * Read from AxionOS's Launcher3 QuickStep (Lineage-based, A17) with jadx.
 * Recents runs in the launcher's own window there, in Views. The row is
 * com.android.quickstep.views.OverviewActionsView, a FrameLayout whose
 * LinearLayout @id/action_buttons holds plain Buttons: Screenshot, Split
 * (hidden on phones), Lens (off by default) and, from Lineage, Clear all;
 * a Save app pair Button sits beside it on large screens. Each Button
 * carries its own background from the theme: with cross-window blur on, a
 * RippleDrawable over a rounded GradientDrawable (28 dp, so a pill at the
 * 48 dp height) filled with the framework's translucent
 * customColorSurfaceEffect1, the "semi-transparent button"; without it, the
 * same shape in an opaque surface colour. The row fades through
 * MultiValueAlpha on the container, which a region follows as ancestor alpha.
 *
 * ClearAllButton is the Clear all that scrolls in after the last card. With
 * Lineage's row button on (the default) it stays at alpha 0, but on a large
 * screen or with that button off it is the one that shows.
 *
 * Each Button gets glass (ViewGlass) at its outline, and its background
 * keeps app.fill of its alpha. The overview scrim (50% black or white) is
 * drawn under the buttons in the same window, so it stays over the glass, as
 * the shade's scrim does over Quick Settings tiles: ViewGlass draws in the
 * view's overlay, after the label, and clearing the window there would erase
 * the label too.
 *
 * Before-hooks only (they also run under the dev loader):
 * OverviewActionsView.onFinishInflate, which runs once per inflation, with
 * the children already in place; ClearAllButton.onLayout. The lock pill
 * (Lineage's drag-to-lock indicator, a LinearLayout) is not a Button and is
 * left as it is.
 */
final class LauncherOverview {
    private static final String P = "com.android.quickstep.views.";
    private static boolean installed;

    private LauncherOverview() {}

    static synchronized void install(ClassLoader cl) {
        if (installed) return;
        installed = true;
        int hooked = 0;
        try {
            Method inflate = Class.forName(P + "OverviewActionsView", false, cl).getDeclaredMethod("onFinishInflate");
            XposedBridge.hookMethod(inflate, new XC_MethodHook() {
                @Override
                protected void beforeHookedMethod(MethodHookParam param) {
                    try {
                        Region.reap();
                        if (param.thisObject instanceof ViewGroup) glassButtons((ViewGroup) param.thisObject);
                    } catch (Throwable e) {
                        L.once("overview-row", "overview action row hook failed; its buttons stay stock", e);
                    }
                }
            });
            hooked++;
        } catch (Throwable t) {
            L.w("no OverviewActionsView.onFinishInflate (" + t + "); the overview action row stays stock");
        }
        try {
            Method layout = Class.forName(P + "ClearAllButton", false, cl)
                    .getDeclaredMethod("onLayout", boolean.class, int.class, int.class, int.class, int.class);
            XposedBridge.hookMethod(layout, new XC_MethodHook() {
                @Override
                protected void beforeHookedMethod(MethodHookParam param) {
                    try {
                        Region.reap();
                        if (param.thisObject instanceof View) glass((View) param.thisObject);
                    } catch (Throwable e) {
                        L.once("overview-clearall", "Clear all hook failed; it stays stock", e);
                    }
                }
            });
            hooked++;
        } catch (Throwable t) {
            L.w("no ClearAllButton.onLayout (" + t + "); the Clear all after the cards stays stock");
        }
        if (hooked > 0) L.i("overview buttons hooked: " + hooked);
    }

    /** Every Button under {@code g} that draws a background of its own. */
    private static void glassButtons(ViewGroup g) {
        for (int i = 0; i < g.getChildCount(); i++) {
            View c = g.getChildAt(i);
            if (c instanceof Button) {
                glass(c);
            } else if (c instanceof ViewGroup) {
                glassButtons((ViewGroup) c);
            }
        }
    }

    private static void glass(View v) {
        if (v.getBackground() == null) return;
        ViewGlass.attach(v, Gate::overview, x -> Gate.fill(), "overview button").shadowless();
    }
}
