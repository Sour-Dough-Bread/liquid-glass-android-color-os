package dev.stockglass.companion;

import android.view.View;
import de.robv.android.xposed.XC_MethodHook;
import de.robv.android.xposed.XposedBridge;
import java.lang.reflect.Field;
import java.lang.reflect.Method;

/**
 * The text selection toolbar (Cut, Copy, Paste, Select all) on Android 17.
 *
 * Apps no longer draw it. With system_selection_toolbar_enabled (on in both
 * A17 builds in hand), an app's FloatingToolbar is a RemoteFloatingToolbarPopup:
 * a PopupWindow holding a SurfaceView, into which SystemUI's
 * SysUiSelectionToolbarRenderService draws the toolbar through a
 * SurfaceControlViewHost. So the toolbar of every app is a view tree in
 * SystemUI's process, where the companion already runs, and no app needs to be
 * in the module's scope.
 *
 * RemoteSelectionToolbar keeps the toolbar's rounded container (the
 * framework's floating_popup_container, tagged "floating_toolbar") in its
 * public field contentContainer, and show(ShowInfo) runs every time the
 * toolbar is shown or updated. A before-hook there gives the container glass
 * (ViewGlass): a region in the SurfaceControlViewHost's own window, which
 * SurfaceFlinger draws over the app below it.
 */
final class TextToolbar {
    private static final String TOOLBAR = "com.android.systemui.selectiontoolbar.app.ui.RemoteSelectionToolbar";
    private static boolean installed;

    private TextToolbar() {}

    static synchronized void install(ClassLoader cl) {
        if (installed) return;
        installed = true;
        final Field container;
        Method show = null;
        try {
            Class<?> c = Class.forName(TOOLBAR, false, cl);
            container = c.getField("contentContainer");
            for (Method m : c.getDeclaredMethods()) {
                if (m.getName().equals("show") && m.getParameterCount() == 1) show = m;
            }
            if (show == null) throw new NoSuchMethodException(TOOLBAR + ".show");
        } catch (Throwable t) {
            L.w("no SystemUI selection toolbar (" + t + "); apps draw their own, and their"
                    + " toolbar stays stock");
            return;
        }
        XposedBridge.hookMethod(show, new XC_MethodHook() {
            @Override
            protected void beforeHookedMethod(MethodHookParam param) {
                try {
                    Region.reap();
                    Object v = container.get(param.thisObject);
                    if (v instanceof View) {
                        ViewGlass.attach((View) v, Gate::toolbar, x -> Gate.fill(), "text toolbar")
                                .shadowless();
                    }
                } catch (Throwable e) {
                    L.once("toolbar", "text toolbar hook failed; the toolbar stays stock", e);
                }
            }
        });
        L.i("text selection toolbar hooked: RemoteSelectionToolbar.show");
    }
}
