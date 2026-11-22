package dev.stockglass.auto.media;

import de.robv.android.xposed.IXposedHookLoadPackage;
import de.robv.android.xposed.callbacks.XC_LoadPackage;

/**
 * The module's Xposed entry point (assets/xposed_init): Liquid Glass under
 * the buttons of Android Auto's dashboard media card.
 *
 * Android Auto (com.google.android.projection.gearhead) draws each car
 * window on a private virtual display and composites them itself, so the
 * StockGlass engine's blur regions never see the card's artwork. The glass
 * is drawn inside Android Auto's own renderer instead, in the process that
 * draws the car's windows (":projection" in 18.0).
 */
public final class Entry implements IXposedHookLoadPackage {
    static final String GEARHEAD = "com.google.android.projection.gearhead";

    @Override
    public void handleLoadPackage(XC_LoadPackage.LoadPackageParam p) {
        android.util.Log.i("SGAutoMedia", "handleLoadPackage " + p.packageName + " in " + p.processName
                + " (first " + p.isFirstApplication + ")");
        if (!GEARHEAD.equals(p.packageName)) return;
        // In 18.0 the car's windows (rail, dashboard, app screens) belong to
        // ":projection". The process name is not relied on: every Android Auto
        // process gets the hooks, and only one that has a media card window
        // ever finds one.
        Host.start(p.classLoader, p.processName);
    }
}
