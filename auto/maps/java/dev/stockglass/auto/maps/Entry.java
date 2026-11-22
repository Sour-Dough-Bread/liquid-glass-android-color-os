package dev.stockglass.auto.maps;

import de.robv.android.xposed.IXposedHookLoadPackage;
import de.robv.android.xposed.callbacks.XC_LoadPackage;

/**
 * Xposed entry point (assets/xposed_init). Google Maps only, and only its main
 * process: that is the process that owns the Android Auto car window (a
 * Presentation on Maps' own private virtual display).
 */
public final class Entry implements IXposedHookLoadPackage {
    static final String MAPS = "com.google.android.apps.maps";

    @Override
    public void handleLoadPackage(XC_LoadPackage.LoadPackageParam p) {
        if (!MAPS.equals(p.packageName)) return;
        if (p.processName != null && !p.processName.equals(p.packageName)) return;
        MapsCar.install(p.classLoader);
        Gate.watch();
    }
}
