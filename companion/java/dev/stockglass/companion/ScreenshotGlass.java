package dev.stockglass.companion;

import android.view.View;
import de.robv.android.xposed.XC_MethodHook;
import de.robv.android.xposed.XposedBridge;
import java.lang.reflect.Field;
import java.lang.reflect.Method;

/**
 * The screenshot UI's action bar: the pill behind the Edit, Share and other
 * chips under a fresh screenshot. Both A17 builds draw it with Views, in
 * SystemUI's separate process com.android.systemui:screenshot, in a window
 * (TYPE_SCREENSHOT) that is added for each screenshot and removed when it is
 * dismissed.
 *
 * The pill is ScreenshotShelfView.actionsContainerBackground
 * (@id/actions_container_background), a FrameLayout with a rounded
 * surface-bright background and 4 dp of elevation. Each screenshot's
 * entrance animation waits for a pre-draw listener,
 * ScreenshotShelfViewProxy$prepareEntranceAnimation$1, made anew for each
 * one; a before-hook on its onPreDraw gives the pill glass (ViewGlass) before
 * its first frame. The pill is still GONE then: the overlay draws when it is
 * shown, and the region follows the view's alpha and slide through the
 * entrance and dismissal.
 */
final class ScreenshotGlass {
    private static final String P = "com.android.systemui.screenshot.";
    private static boolean installed;

    private ScreenshotGlass() {}

    static synchronized void install(ClassLoader cl) {
        if (installed) return;
        installed = true;
        final Field proxy, view, background;
        Method onPreDraw;
        try {
            Class<?> listener = Class.forName(P + "ScreenshotShelfViewProxy$prepareEntranceAnimation$1", false, cl);
            onPreDraw = listener.getDeclaredMethod("onPreDraw");
            proxy = field(listener, "this$0");
            view = field(Class.forName(P + "ScreenshotShelfViewProxy", false, cl), "view");
            // ScreenshotShelfView, in screenshot.ui in both builds
            background = field(view.getType(), "actionsContainerBackground");
        } catch (Throwable t) {
            L.w("no screenshot shelf of the expected shape (" + t + "); its action bar stays stock");
            return;
        }
        XposedBridge.hookMethod(onPreDraw, new XC_MethodHook() {
            @Override
            protected void beforeHookedMethod(MethodHookParam param) {
                try {
                    Region.reap();
                    Object p = proxy.get(param.thisObject);
                    Object shelf = p == null ? null : view.get(p);
                    Object v = shelf == null ? null : background.get(shelf);
                    if (v instanceof View) {
                        ViewGlass.attach((View) v, Gate::screenshot, x -> Gate.fill(), "screenshot actions")
                                .shadowless();
                    }
                } catch (Throwable e) {
                    L.once("screenshot", "screenshot hook failed; its action bar stays stock", e);
                }
            }
        });
        L.i("screenshot action bar hooked: ScreenshotShelfViewProxy entrance");
    }

    private static Field field(Class<?> c, String name) throws NoSuchFieldException {
        Field f = c.getDeclaredField(name);
        f.setAccessible(true);
        return f;
    }
}
