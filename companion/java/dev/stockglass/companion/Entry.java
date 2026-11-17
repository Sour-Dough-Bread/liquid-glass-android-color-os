package dev.stockglass.companion;

import de.robv.android.xposed.IXposedHookLoadPackage;
import de.robv.android.xposed.callbacks.XC_LoadPackage;

/**
 * The companion's only Xposed entry point (assets/xposed_init).
 *
 * The compositor engine can only restyle blur regions that already exist. The
 * surfaces here never become blur regions on their own, so this module makes
 * them into regions: each one gets a BackgroundBlurDrawable from its window's
 * ViewRootImpl, the same object SystemUI uses for notification cards, and its
 * own fill becomes translucent so the glass the engine draws there shows.
 */
public final class Entry implements IXposedHookLoadPackage {
    private static final String SYSTEMUI = "com.android.systemui";
    /** SystemUI's process for the screenshot UI, in both A17 builds. */
    private static final String SCREENSHOT = SYSTEMUI + ":screenshot";

    @Override
    public void handleLoadPackage(XC_LoadPackage.LoadPackageParam p) {
        if (SYSTEMUI.equals(p.packageName) && SCREENSHOT.equals(p.processName)) {
            ScreenshotGlass.install(p.classLoader);
            Gate.watch();
            return;
        }
        // Otherwise only the package's own main process; other packages can
        // load this code (createPackageContext with CONTEXT_INCLUDE_CODE) and
        // SystemUI runs helper processes that draw no tiles.
        if (p.processName != null && !p.processName.equals(p.packageName)) return;
        switch (p.packageName) {
            case SYSTEMUI:
                QsTiles.install(p.classLoader);
                Sliders.install(p.classLoader);
                ShadeGlass.install(p.classLoader);
                TextToolbar.install(p.classLoader);
                ClockGlass.install(p.classLoader);
                Gate.watch();
                break;
            case "com.android.launcher3":
            case "com.google.android.apps.nexuslauncher":
                Drawer.init(p.classLoader);
                LauncherFolders.install(p.classLoader);
                LauncherIcons.install(p.classLoader);
                LauncherWidgets.install(p.classLoader);
                LauncherSearch.install(p.classLoader);
                Gate.watch();
                break;
            default:
                break;
        }
    }
}
