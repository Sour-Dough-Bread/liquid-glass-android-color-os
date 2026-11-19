package dev.stockglass.companion;

import android.content.res.Resources;
import android.view.WindowManager;
import de.robv.android.xposed.XC_MethodHook;
import de.robv.android.xposed.XposedBridge;
import java.lang.reflect.Field;
import java.lang.reflect.Method;

/**
 * A blurred backdrop behind every app's dialogs, from system_server, so no app
 * has to be in the module's scope.
 *
 * Read from AxionOS's services.jar (A17) with jadx and baksmali. Blur behind a
 * window is a window attribute: FLAG_BLUR_BEHIND with a radius
 * (LayoutParams.setBlurBehindRadius), which WindowState.applyDims hands to the
 * same Dimmer layer that draws the dim, so the blur sits under the dialog and
 * fades in and out with the dim. Apps rarely ask for it. Here every dialog
 * window does: DisplayPolicy.adjustWindowParamsLw(WindowState, LayoutParams)
 * runs on each add and each relayout, before the params are stored (on
 * relayout: adjust, the three sanitize* calls, which do not touch blur, then
 * mAttrs.copyFrom), so an after-hook there sets the flag where the client's
 * next relayout cannot undo it.
 *
 * A dialog is a window with FLAG_DIM_BEHIND and a dim above 0 that belongs to
 * an activity (WindowState.mActivityRecord), either TYPE_APPLICATION (a
 * Dialog, AlertDialog, bottom sheet) or a base application window that does
 * not fill the screen (a dialog-themed activity, e.g. a permission prompt).
 * Popup menus, spinners and the IME are other types; SystemUI's own dialogs
 * have no activity. Gating on the dim also keeps step with SystemUI's dialog
 * animator, which clears the dim while it animates.
 *
 * A window that asks for blur behind itself (the power menu, an app's
 * windowBlurBehindEnabled) is left as it asked. Nothing is set while window
 * blur is off (battery saver, the developer setting, no blur support), and
 * while it is on the dim is lightened (app.dialogdim), as AOSP's power menu
 * does: 0.54 with blur, 0.88 without. A window keeps what it was given until
 * its next relayout, so switching glass off reaches open dialogs when they
 * next lay out.
 *
 * The blur covers the task, so it is a full-screen blur, which the engine
 * treats as a backdrop (stock, tone or glass, as the backdrop setting says).
 * The adjust hook runs under WindowManager's global lock: nothing here makes a
 * binder call.
 */
final class AppDialogs {
    private static boolean installed;
    private static Field activityRecord, wmService, blurController;
    private static Method blurEnabled;
    private static int frameworkRadius = -1;

    private AppDialogs() {}

    static synchronized void install(ClassLoader cl) {
        if (installed) return;
        installed = true;
        Method adjust;
        try {
            Class<?> windowState = Class.forName("com.android.server.wm.WindowState", false, cl);
            adjust = Class.forName("com.android.server.wm.DisplayPolicy", false, cl)
                    .getDeclaredMethod("adjustWindowParamsLw", windowState, WindowManager.LayoutParams.class);
            activityRecord = field(windowState, "mActivityRecord");
            wmService = field(windowState, "mWmService");
            blurController = field(wmService.getType(), "mBlurController");
            blurEnabled = blurController.getType().getMethod("getBlurEnabled");
        } catch (Throwable t) {
            L.w("no DisplayPolicy.adjustWindowParamsLw of the expected shape (" + t
                    + "); apps' dialogs keep no blur behind them");
            return;
        }
        XposedBridge.hookMethod(adjust, new XC_MethodHook() {
            @Override
            protected void afterHookedMethod(MethodHookParam param) {
                if (param.hasThrowable()) return;
                try {
                    Object win = param.args[0];
                    Object a = param.args[1];
                    if (win != null && a instanceof WindowManager.LayoutParams) {
                        adjust(win, (WindowManager.LayoutParams) a);
                    }
                } catch (Throwable e) {
                    L.once("app-dialogs", "dialog blur hook failed; dialogs keep no blur behind them", e);
                }
            }
        });
        L.i("apps' dialogs hooked: DisplayPolicy.adjustWindowParamsLw");
    }

    private static void adjust(Object win, WindowManager.LayoutParams a) throws ReflectiveOperationException {
        if ((a.flags & WindowManager.LayoutParams.FLAG_DIM_BEHIND) == 0 || !(a.dimAmount > 0f)) return;
        if ((a.flags & WindowManager.LayoutParams.FLAG_BLUR_BEHIND) != 0 && a.getBlurBehindRadius() > 0) {
            return; // the window asked for its own blur behind
        }
        boolean dialog = a.type == WindowManager.LayoutParams.TYPE_APPLICATION
                || (a.type == WindowManager.LayoutParams.TYPE_BASE_APPLICATION
                        && !(a.width == WindowManager.LayoutParams.MATCH_PARENT
                                && a.height == WindowManager.LayoutParams.MATCH_PARENT));
        if (!dialog || activityRecord.get(win) == null) return;
        if (!Gate.appDialogs() || !blurOn(win)) return;
        a.flags |= WindowManager.LayoutParams.FLAG_BLUR_BEHIND;
        a.setBlurBehindRadius(radius());
        a.dimAmount *= Gate.appDialogDim();
    }

    /** WindowManager's own verdict: blur supported, not turned off, no battery saver. */
    private static boolean blurOn(Object win) throws ReflectiveOperationException {
        Object wm = wmService.get(win);
        Object bc = wm == null ? null : blurController.get(wm);
        return bc != null && (boolean) blurEnabled.invoke(bc);
    }

    /** app.dialogradius in px, else the framework's own largest window blur. */
    private static int radius() {
        int r = Gate.appDialogRadius();
        if (r > 0) return r;
        if (frameworkRadius < 0) {
            int id = Resources.getSystem().getIdentifier("max_window_blur_radius", "dimen", "android");
            frameworkRadius = id != 0 ? Resources.getSystem().getDimensionPixelSize(id) : 150;
        }
        return frameworkRadius;
    }

    /** A declared field of {@code c} or of the nearest superclass that declares it. */
    private static Field field(Class<?> c, String name) throws NoSuchFieldException {
        for (Class<?> k = c; k != null; k = k.getSuperclass()) {
            try {
                Field f = k.getDeclaredField(name);
                f.setAccessible(true);
                return f;
            } catch (NoSuchFieldException e) {
                // keep looking up the hierarchy
            }
        }
        throw new NoSuchFieldException(c.getName() + "." + name);
    }
}
