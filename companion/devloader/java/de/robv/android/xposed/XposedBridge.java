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

    /**
     * The original, without hooks. The dev loader never hooks a call made
     * from inside a callback (Loader.dispatch), so a plain call is it.
     */
    public static Object invokeOriginalMethod(Member method, Object thisObject, Object[] args)
            throws IllegalAccessException, java.lang.reflect.InvocationTargetException {
        java.lang.reflect.Method m = (java.lang.reflect.Method) method;
        m.setAccessible(true);
        return m.invoke(thisObject, args);
    }

    public static void log(String text) {
        Log.i(Loader.TAG, text);
    }

    public static void log(Throwable t) {
        Log.e(Loader.TAG, Log.getStackTraceString(t));
    }
}
