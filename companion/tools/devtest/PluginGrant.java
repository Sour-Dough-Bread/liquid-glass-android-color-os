package dev.stockglass.devtest;

import de.robv.android.xposed.IXposedHookLoadPackage;
import de.robv.android.xposed.XC_MethodHook;
import de.robv.android.xposed.XposedBridge;
import de.robv.android.xposed.callbacks.XC_LoadPackage;
import java.lang.reflect.Method;

/**
 * Test only, for the userdebug AVD; never in the module (build.sh compiles
 * java/ alone). SystemUI loads a clock plugin only if the plugin holds
 * com.android.systemui.permission.PLUGIN, a signature permission, so the
 * crDroid-signed SystemUIClocks-* APKs never get it on the AVD. This asks
 * PackageManager about SystemUI itself instead, for those packages only.
 * The dev loader can replace arguments but not results, hence the swap.
 */
public final class PluginGrant implements IXposedHookLoadPackage {
    private static final String PERMISSION = "com.android.systemui.permission.PLUGIN";
    private static final String CLOCKS = "com.android.systemui.clocks.";

    @Override
    public void handleLoadPackage(XC_LoadPackage.LoadPackageParam p) throws Throwable {
        if (!"com.android.systemui".equals(p.processName)) return;
        Method m = Class.forName("android.app.ApplicationPackageManager")
                .getDeclaredMethod("checkPermission", String.class, String.class);
        XposedBridge.hookMethod(m, new XC_MethodHook() {
            @Override
            protected void beforeHookedMethod(MethodHookParam param) {
                Object pkg = param.args[1];
                if (PERMISSION.equals(param.args[0]) && pkg instanceof String && ((String) pkg).startsWith(CLOCKS)) {
                    param.args[1] = "com.android.systemui";
                }
            }
        });
        android.util.Log.i("StockGlassDev", "devtest: clock plugins pass the PLUGIN permission check");
    }
}
