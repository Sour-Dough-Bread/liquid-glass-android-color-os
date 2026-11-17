package dev.stockglass.companion;

import android.content.Context;
import android.content.ContextWrapper;
import android.view.View;
import java.lang.reflect.Field;

/**
 * The launcher's app drawer, as a fade for regions on the home screen.
 *
 * Opening the drawer does not fade the workspace on A17's launchers: every
 * view from an icon up to the window stays VISIBLE at alpha 1 while the
 * drawer's panel slides over it. The panel is slightly translucent, so glass
 * left under the home screen's icons showed through it as faint discs. The
 * drawer's own position, AllAppsTransitionController.mProgress, runs from 1
 * (closed) to 0 (open); the regions use it as their opacity.
 */
final class Drawer implements Region.Fade {
    static final Drawer INSTANCE = new Drawer();

    private static Class<?> launcher;
    private static Field controller, progress;

    private Drawer() {}

    static synchronized void init(ClassLoader cl) {
        if (launcher != null) return;
        try {
            Class<?> l = Class.forName("com.android.launcher3.Launcher", false, cl);
            controller = l.getDeclaredField("mAllAppsController");
            controller.setAccessible(true);
            progress = controller.getType().getDeclaredField("mProgress");
            progress.setAccessible(true);
            launcher = l;
        } catch (Throwable t) {
            L.once("drawer", "cannot read the app drawer's position; home screen glass does not"
                    + " fade when it opens", t);
        }
    }

    @Override
    public float of(View host) throws IllegalAccessException {
        if (launcher == null) return 1f;
        Context c = host.getContext();
        for (int i = 0; c != null && i < 8; ++i) {
            if (launcher.isInstance(c)) {
                Object ctl = controller.get(c);
                return ctl == null ? 1f : Math.max(0f, Math.min(1f, progress.getFloat(ctl)));
            }
            c = c instanceof ContextWrapper ? ((ContextWrapper) c).getBaseContext() : null;
        }
        return 1f;
    }
}
