package de.robv.android.xposed;

import android.util.Log;
import dev.stockglass.devloader.Loader;
import java.lang.reflect.Member;

public final class XposedBridge {
    private XposedBridge() {}

    public static XC_MethodHook.Unhook hookMethod(Member hookMethod, XC_MethodHook callback) {
        Loader.hook(hookMethod, callback);
        return callback.new Unhook(hookMethod);
    }

    public static void log(String text) {
        Log.i(Loader.TAG, text);
    }

    public static void log(Throwable t) {
        Log.e(Loader.TAG, Log.getStackTraceString(t));
    }
}
