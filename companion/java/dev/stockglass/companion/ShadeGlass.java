package dev.stockglass.companion;

import android.content.Context;
import android.graphics.Color;
import android.graphics.drawable.Drawable;
import android.view.View;
import de.robv.android.xposed.XC_MethodHook;
import de.robv.android.xposed.XposedBridge;
import java.lang.ref.WeakReference;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.LinkedHashSet;
import java.util.Set;
import java.util.WeakHashMap;

/**
 * The notification shade's cards and scrims, and heads-up notifications.
 *
 * A17 already makes each notification card a blur region when the user has
 * notification row transparency on (Settings.Secure
 * notification_row_transparency; on, on alioth):
 * NotificationBackgroundView.setBlurBackgroundEnabled(true) gives the card a
 * BackgroundBlurDrawable from its window, and the engine draws each one as a
 * pane. What hides that glass is paint: the card's fill, which the drawable
 * paints itself (setColor: the theme's surface_effect_1, #8a121317 in
 * alioth's dark theme), and under the card the shade's two scrims, behind (the
 * shade panel colour, alpha 0.725 on alioth) and notifications (alpha 0.5).
 * Together they cover about 94% of each pane. In glass mode:
 *
 *  - the card fill's alpha is multiplied by app.cards, in a before-hook on
 *    BackgroundBlurDrawable.setColor for drawables whose callback is a
 *    NotificationBackgroundView;
 *  - the shade's scrim alphas are multiplied by app.scrim, in before-hooks on
 *    the UNLOCKED and SHADE_LOCKED ScrimStates' setShadePanelColor and
 *    setNotificationScrimColor, which ScrimController.updateThemeColors calls
 *    with the theme's colours. The keyguard, bouncer and AOD scrims are not
 *    touched. The two scrims differ in colour, and the notifications scrim
 *    covers only the stack, so the shade is two-toned. app.scrimcolor black,
 *    white or tint makes it one colour: the behind scrim, which covers the
 *    whole shade, takes that colour at its own alpha, and the notifications
 *    scrim is cleared;
 *  - a heads-up keeps its card's region. ExpandableNotificationRow
 *    .usesTransparentBackground() is false for a row that must stay on screen
 *    (a pinned heads-up), so the row drops its blur drawable and paints an
 *    opaque colour over the app below. An after-hook returns true instead
 *    while glass is on, so the heads-up keeps the same region and translucent
 *    fill as in the shade, and the engine draws it over the app. LSPosed only:
 *    the dev loader cannot replace a result.
 *
 * A SystemUI without per-card blur (the A17 AVD's SystemUIGoogle has no
 * setBlurBackgroundEnabled) gets the scrim part only.
 */
final class ShadeGlass {
    private static final String P = "com.android.systemui.statusbar.";
    private static final String BACKGROUND = P + "notification.row.NotificationBackgroundView";
    private static final String ACTIVATABLE = P + "notification.row.ActivatableNotificationView";
    private static final String ROW = P + "notification.row.ExpandableNotificationRow";
    private static final String STATE = P + "phone.ScrimState";
    private static final String CONTROLLER = P + "phone.ScrimController";
    private static final String BLUR_DRAWABLE = "com.android.internal.graphics.drawable.BackgroundBlurDrawable";
    private static final String[] SHADE_STATES = {"UNLOCKED", "SHADE_LOCKED"};

    private static boolean installed;
    private static Class<?> background;
    private static Method setColor, updateThemeColors, applyState, updateIfNeeded;
    private static Field blurSupported, onKeyguard;
    private static WeakReference<Object> controller;
    /** Each card drawable's colour as SystemUI last set it, before the cut. */
    private static final WeakHashMap<Drawable, Integer> cards = new WeakHashMap<>();
    /** Rows whose transparency the hook has answered for; main thread. */
    private static final WeakHashMap<Object, Boolean> rows = new WeakHashMap<>();

    private ShadeGlass() {}

    static synchronized void install(ClassLoader cl) {
        if (installed) return;
        installed = true;
        boolean cardsHooked = installCards(cl);
        boolean scrimHooked = installScrim(cl);
        boolean headsUpHooked = cardsHooked && installHeadsUp(cl);
        if (cardsHooked || scrimHooked || headsUpHooked) Gate.onChange(ShadeGlass::gateChanged);
    }

    private static boolean installCards(ClassLoader cl) {
        try {
            background = Class.forName(BACKGROUND, false, cl);
            background.getMethod("setBlurBackgroundEnabled", boolean.class);
            setColor = Class.forName(BLUR_DRAWABLE, false, cl).getMethod("setColor", int.class);
        } catch (Throwable t) {
            background = null;
            L.i("no per-card blur in this SystemUI (" + t.getClass().getSimpleName()
                    + "); notification cards and heads-ups left alone");
            return false;
        }
        XposedBridge.hookMethod(setColor, new XC_MethodHook() {
            @Override
            protected void beforeHookedMethod(MethodHookParam param) {
                try {
                    Drawable d = (Drawable) param.thisObject;
                    if (!background.isInstance(d.getCallback())) return;
                    int color = (Integer) param.args[0];
                    cards.put(d, color);
                    if (Gate.shade()) param.args[0] = scaleAlpha(color, Gate.cards());
                } catch (Throwable e) {
                    L.once("card", "card hook failed; notification cards stay stock", e);
                }
            }
        });
        L.i("notification cards hooked: BackgroundBlurDrawable.setColor");
        return true;
    }

    private static boolean installScrim(ClassLoader cl) {
        Set<Method> setters = new LinkedHashSet<>();
        try {
            Class<?> state = Class.forName(STATE, false, cl);
            Class<?> ctl = Class.forName(CONTROLLER, false, cl);
            updateThemeColors = ctl.getDeclaredMethod("updateThemeColors");
            updateThemeColors.setAccessible(true);
            applyState = ctl.getDeclaredMethod("applyAndDispatchState");
            applyState.setAccessible(true);
            for (String name : SHADE_STATES) {
                Object s = state.getField(name).get(null);
                setters.add(declared(s.getClass(), state, "setShadePanelColor"));
                setters.add(declared(s.getClass(), state, "setNotificationScrimColor"));
            }
        } catch (Throwable t) {
            L.e("this SystemUI's scrims are not the expected shape; the shade's scrim left alone", t);
            return false;
        }
        XC_MethodHook scale = new XC_MethodHook() {
            @Override
            protected void beforeHookedMethod(MethodHookParam param) {
                try {
                    // A setter declared on ScrimState itself serves every state.
                    if (!isShadeState(param.thisObject) || !Gate.shade()) return;
                    int color = scaleAlpha((Integer) param.args[0], Gate.scrim());
                    String one = Gate.scrimColor();
                    if (!one.equals("stock")) {
                        if (param.method.getName().equals("setNotificationScrimColor")) {
                            color &= 0x00ffffff;
                        } else {
                            color = (color & 0xff000000) | (oneColour(one, color) & 0x00ffffff);
                        }
                    }
                    param.args[0] = color;
                } catch (Throwable e) {
                    L.once("scrim", "scrim hook failed; the shade's scrim stays stock", e);
                }
            }
        };
        for (Method m : setters) XposedBridge.hookMethod(m, scale);
        XposedBridge.hookMethod(updateThemeColors, new XC_MethodHook() {
            @Override
            protected void beforeHookedMethod(MethodHookParam param) {
                Object was = controller == null ? null : controller.get();
                if (was != param.thisObject) controller = new WeakReference<>(param.thisObject);
            }
        });
        L.i("shade scrim hooked: " + setters.size() + " ScrimState setters,"
                + " ScrimController.updateThemeColors");
        return true;
    }

    private static boolean installHeadsUp(ClassLoader cl) {
        Method uses;
        try {
            Class<?> row = Class.forName(ROW, false, cl);
            Class<?> activatable = Class.forName(ACTIVATABLE, false, cl);
            uses = row.getDeclaredMethod("usesTransparentBackground");
            blurSupported = field(activatable, "mIsBlurSupported");
            onKeyguard = field(activatable, "mOnKeyguard");
            updateIfNeeded = activatable.getDeclaredMethod("updateIfNeeded");
            updateIfNeeded.setAccessible(true);
        } catch (Throwable t) {
            L.e("this SystemUI's notification rows are not the expected shape; heads-ups left alone", t);
            return false;
        }
        XposedBridge.hookMethod(uses, new XC_MethodHook() {
            @Override
            protected void afterHookedMethod(MethodHookParam param) {
                try {
                    Object row = param.thisObject;
                    rows.put(row, Boolean.TRUE);
                    if (Boolean.TRUE.equals(param.getResult()) || !Gate.headsUp()) return;
                    // Only where the user has row transparency on, and not on
                    // the keyguard, which has its own setting.
                    if (blurSupported.getBoolean(row) && !onKeyguard.getBoolean(row)) {
                        param.setResult(Boolean.TRUE);
                    }
                } catch (Throwable e) {
                    L.once("headsup", "heads-up hook failed; heads-ups stay stock", e);
                }
            }
        });
        L.i("heads-ups hooked: ExpandableNotificationRow.usesTransparentBackground"
                + " (applies under LSPosed; the dev loader cannot replace a result)");
        return true;
    }

    /**
     * Main thread. Rows re-evaluate their transparency (a heads-up gains or
     * drops its region), cards get their colour again through the hook, and
     * the scrim controller re-reads the theme's colours and applies its state,
     * so an open shade changes at once.
     */
    private static void gateChanged() {
        for (Object row : rows.keySet().toArray()) {
            try {
                updateIfNeeded.invoke(row);
            } catch (Throwable e) {
                L.once("row-update", "cannot refresh a notification row", e);
            }
        }
        for (Object k : cards.keySet().toArray()) {
            Drawable d = (Drawable) k;
            Integer color = cards.get(d);
            if (color == null) continue;
            try {
                setColor.invoke(d, color);
                Object cb = d.getCallback();
                if (cb instanceof View) ((View) cb).invalidate();
            } catch (Throwable e) {
                L.once("card-update", "cannot refresh a notification card", e);
            }
        }
        Object c = controller == null ? null : controller.get();
        if (c != null) {
            try {
                updateThemeColors.invoke(c);
                applyState.invoke(c);
            } catch (Throwable e) {
                L.once("scrim-update", "cannot refresh the shade's scrim", e);
            }
        }
    }

    private static boolean isShadeState(Object state) {
        String name = state instanceof Enum ? ((Enum<?>) state).name() : String.valueOf(state);
        for (String s : SHADE_STATES) {
            if (s.equals(name)) return true;
        }
        return false;
    }

    /**
     * The one colour of app.scrimcolor, as RGB. tint is the palette's accent
     * at tone 20 for a dark shade or tone 90 for a light one, judged by the
     * shade panel colour SystemUI passed ({@code stock}), so a ROM that keeps
     * its shade dark in a light theme stays dark.
     */
    private static int oneColour(String which, int stock) {
        if (which.equals("black")) return 0x000000;
        if (which.equals("white")) return 0xffffff;
        try {
            Context c = (Context) Class.forName("android.app.ActivityThread")
                    .getMethod("currentApplication").invoke(null);
            boolean dark = Color.luminance(stock | 0xff000000) < 0.5f;
            return c.getColor(dark ? android.R.color.system_accent1_800 : android.R.color.system_accent1_100);
        } catch (Throwable e) {
            L.once("scrim-tint", "cannot read the palette's accent; the shade keeps its own colour", e);
            return stock;
        }
    }

    static int scaleAlpha(int color, float k) {
        int a = Math.max(0, Math.min(255, Math.round((color >>> 24) * k)));
        return (color & 0x00ffffff) | (a << 24);
    }

    /** The most derived declaration of {@code name(int)} from {@code c} up to {@code top}. */
    private static Method declared(Class<?> c, Class<?> top, String name) throws NoSuchMethodException {
        for (Class<?> k = c; k != null; k = k.getSuperclass()) {
            try {
                Method m = k.getDeclaredMethod(name, int.class);
                m.setAccessible(true);
                return m;
            } catch (NoSuchMethodException e) {
                if (k == top) break;
            }
        }
        throw new NoSuchMethodException(c.getName() + "." + name + "(int)");
    }

    private static Field field(Class<?> c, String name) throws NoSuchFieldException {
        Field f = c.getDeclaredField(name);
        f.setAccessible(true);
        return f;
    }
}
