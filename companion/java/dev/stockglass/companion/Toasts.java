package dev.stockglass.companion;

import android.content.res.ColorStateList;
import android.content.res.Configuration;
import android.graphics.Color;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.view.View;
import android.widget.TextView;
import de.robv.android.xposed.XC_MethodHook;
import de.robv.android.xposed.XposedBridge;
import java.lang.reflect.Method;

/**
 * Text toasts. Since Android 12, SystemUI draws every app's text toast
 * itself: ToastUI builds a SystemUIToast (SystemUI's text_toast layout, a
 * LinearLayout whose background is the framework's toast_frame, a rounded
 * surface-coloured plate with 2 dp of elevation, holding the app's icon and
 * the text) and hands its view to a new android.widget.ToastPresenter, whose
 * show() adds it to the window manager as a window of its own (TYPE_TOAST).
 * hide() removes the window when the toast ends.
 *
 * A before-hook on ToastPresenter.show gives the plate glass (ViewGlass)
 * before the window is added: a region in the toast's own window, from that
 * window's ViewRootImpl, and the plate at a fraction of its alpha. The region
 * follows the view's alpha and scale through SystemUIToast's entrance and
 * exit animations, and goes with the window: SurfaceFlinger has no layer to
 * draw it on once hide() has removed it.
 *
 * Only SystemUI's own text toasts get glass: views that hold SystemUI's
 * toast text (R.id.text). A ToastPlugin's view and a custom-view toast are
 * left alone; custom-view toasts are deprecated and are drawn in the posting
 * app's process, which the companion is not loaded into.
 *
 * The plate's share follows the widgets' rule (LauncherWidgets.keep):
 * app.fill, or app.contrast where the text would be hard to read. A toast
 * floats over whatever app is open, which SystemUI cannot see, so the
 * backdrop is taken to be in the system theme, as apps usually are. A plate
 * in the theme's own tone (A17's toast_frame follows the theme) keeps
 * app.fill; a light plate in a dark theme or a dark one in a light theme
 * (a ROM's fixed toast style) keeps app.contrast.
 */
final class Toasts {
    private static final String PRESENTER = "android.widget.ToastPresenter";
    private static boolean installed;
    private static int textId = -1;

    private Toasts() {}

    static synchronized void install(ClassLoader cl) {
        if (installed) return;
        installed = true;
        Method show = null;
        try {
            // show(View, IBinder token, IBinder windowToken, int duration, int gravity,
            //      int x, int y, float hMargin, float vMargin, callback[, boolean removeAnimations]);
            // the shorter overload calls the longer one.
            for (Method m : Class.forName(PRESENTER, false, cl).getDeclaredMethods()) {
                Class<?>[] p = m.getParameterTypes();
                if (m.getName().equals("show") && p.length > 0 && p[0] == View.class
                        && (show == null || p.length > show.getParameterCount())) {
                    show = m;
                }
            }
            if (show == null) throw new NoSuchMethodException(PRESENTER + ".show(View, ...)");
        } catch (Throwable t) {
            L.w("no toast presenter (" + t + "); toasts stay stock");
            return;
        }
        XposedBridge.hookMethod(show, new XC_MethodHook() {
            @Override
            protected void beforeHookedMethod(MethodHookParam param) {
                try {
                    Region.reap();
                    Object v = param.args[0];
                    if (v instanceof View && isTextToast((View) v)) glass((View) v);
                } catch (Throwable e) {
                    L.once("toast", "toast hook failed; toasts stay stock", e);
                }
            }
        });
        L.i("toasts hooked: ToastPresenter.show");
    }

    private static boolean isTextToast(View v) {
        if (textId < 0) {
            textId = v.getResources().getIdentifier("text", "id", "com.android.systemui");
        }
        return textId != 0 && v.findViewById(textId) instanceof TextView;
    }

    private static void glass(View v) {
        Drawable bg = v.getBackground();
        int plate = tone(bg);
        boolean night = (v.getResources().getConfiguration().uiMode & Configuration.UI_MODE_NIGHT_MASK)
                == Configuration.UI_MODE_NIGHT_YES;
        // A light plate carries dark text, lost over a dark app; a dark plate
        // carries light text, lost over a light one. A plate that cannot be
        // read is taken to be in the theme's tone.
        boolean hard = plate == LIGHT ? night : plate == DARK && !night;
        if (Gate.debug()) {
            L.i("toast: " + (plate == LIGHT ? "light" : plate == DARK ? "dark" : "unread") + " plate ("
                    + (bg == null ? "none" : bg.getClass().getSimpleName()) + ") in a "
                    + (night ? "dark" : "light") + " theme: keeps app." + (hard ? "contrast" : "fill"));
        }
        ViewGlass.attach(v, Gate::toast,
                x -> hard ? Math.max(Gate.fill(), Gate.contrast()) : Gate.fill(), "toast")
                .shadowless();
    }

    private static final int NEITHER = 0, LIGHT = 1, DARK = 2;

    /** The plate's colour as LIGHT or DARK, NEITHER if it has none that can be read. */
    private static int tone(Drawable bg) {
        int c;
        if (bg instanceof GradientDrawable) {
            ColorStateList csl = ((GradientDrawable) bg).getColor();
            if (csl == null) return NEITHER;
            c = csl.getColorForState(bg.getState(), csl.getDefaultColor());
        } else if (bg instanceof ColorDrawable) {
            c = ((ColorDrawable) bg).getColor();
        } else {
            return NEITHER;
        }
        if (Color.alpha(c) == 0) return NEITHER;
        // 0.18 is where black and white text have the same contrast (LauncherWidgets).
        return Color.luminance(c) > 0.18f ? LIGHT : DARK;
    }
}
