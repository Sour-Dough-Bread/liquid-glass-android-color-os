package dev.stockglass.companion;

import android.view.View;
import de.robv.android.xposed.XC_MethodHook;
import de.robv.android.xposed.XposedBridge;
import java.lang.reflect.Field;
import java.lang.reflect.Method;

/**
 * The launcher's search boxes: the app drawer's "Search apps" pill and, on
 * crDroid, the search bar in the dock. Each is a view whose own background
 * is the pill, so each gets ViewGlass: a region at the pill and the
 * background at app.fill of its alpha.
 *
 *   drawer, crDroid   AppsSearchContainerLayout (an EditText). Every layout
 *                     pass sets a new PaintDrawable background
 *                     (setUpBackground, from onLayout); ViewGlass picks up
 *                     each new one.
 *   drawer, Pixel     UniversalSearchInputView. Its pill fades to 0 while
 *                     search is open, and the glass fades with it.
 *   dock, crDroid     QsbLayout: an accent ring (@id/outer) around the pill
 *                     (@id/inner) and a Gemini circle in the pill's colour.
 *                     The ring gets the region and all three get the fill; with
 *                     the ring turned off, the pill gets the region.
 *
 * The Pixel launcher's dock search bar is an app widget (OseWidgetView, from
 * the search app's RemoteViews); LauncherWidgets handles it, under app.search.
 *
 * Hooks run at layout or measure, before the first frame, and attach once per
 * view. The drawer's box fades with the drawer through its ancestors; the
 * dock's bar is covered by the drawer, so it fades as the drawer opens, as
 * the home screen's icons do.
 */
final class LauncherSearch {
    private static boolean installed;

    private LauncherSearch() {}

    static synchronized void install(ClassLoader cl) {
        if (installed) return;
        installed = true;
        int hooked = 0;
        hooked += drawerBox(cl, "com.android.launcher3.allapps.search.AppsSearchContainerLayout");
        hooked += drawerBox(cl, "com.google.android.apps.nexuslauncher.allapps.UniversalSearchInputView");
        hooked += dockBar(cl);
        L.i("launcher search boxes hooked: " + hooked);
    }

    private static int drawerBox(ClassLoader cl, String name) {
        Method onLayout;
        try {
            onLayout = Class.forName(name, false, cl).getDeclaredMethod("onLayout", boolean.class, int.class,
                    int.class, int.class, int.class);
        } catch (Throwable t) {
            return 0; // the other launcher's
        }
        XposedBridge.hookMethod(onLayout, new XC_MethodHook() {
            @Override
            protected void beforeHookedMethod(MethodHookParam param) {
                try {
                    Region.reap();
                    ViewGlass.attach((View) param.thisObject, Gate::search, x -> Gate.fill(), "drawer search");
                } catch (Throwable e) {
                    L.once("search-drawer", "drawer search hook failed; the box stays stock", e);
                }
            }
        });
        return 1;
    }

    private static int dockBar(ClassLoader cl) {
        final Field outer, inner, gemini;
        Method onMeasure;
        try {
            Class<?> c = Class.forName("com.android.launcher3.qsb.QsbLayout", false, cl);
            onMeasure = c.getDeclaredMethod("onMeasure", int.class, int.class);
            outer = field(c, "outer");
            inner = field(c, "inner");
            gemini = field(c, "geminiIcon");
        } catch (Throwable t) {
            return 0; // not crDroid's launcher
        }
        XposedBridge.hookMethod(onMeasure, new XC_MethodHook() {
            @Override
            protected void beforeHookedMethod(MethodHookParam param) {
                try {
                    Region.reap();
                    View ring = (View) outer.get(param.thisObject);
                    View pill = (View) inner.get(param.thisObject);
                    View circle = (View) gemini.get(param.thisObject);
                    View glass = ring != null ? ring : pill;
                    if (glass == null) return;
                    ViewGlass.attach(glass, Gate::search, x -> Gate.fill(), "dock search")
                            .fadeWith(Drawer.INSTANCE);
                    if (ring != null && pill != null) {
                        ViewGlass.attach(pill, Gate::search, x -> Gate.fill(), "dock search pill").fillOnly();
                    }
                    if (circle != null) {
                        ViewGlass.attach(circle, Gate::search, x -> Gate.fill(), "dock search circle").fillOnly();
                    }
                } catch (Throwable e) {
                    L.once("search-dock", "dock search hook failed; the bar stays stock", e);
                }
            }
        });
        return 1;
    }

    private static Field field(Class<?> c, String name) throws NoSuchFieldException {
        Field f = c.getDeclaredField(name);
        f.setAccessible(true);
        return f;
    }
}
