package dev.stockglass.companion;

import android.util.Log;
import de.robv.android.xposed.XposedBridge;
import java.util.HashSet;
import java.util.Set;

/** Logging: logcat tag StockGlassApp, and the Xposed log for anything that goes wrong. */
final class L {
    static final String TAG = "StockGlassApp";
    private static final Set<String> once = new HashSet<>();

    private L() {}

    static void i(String msg) {
        Log.i(TAG, msg);
    }

    static void w(String msg) {
        Log.w(TAG, msg);
        XposedBridge.log("StockGlass: " + msg);
    }

    static void e(String msg, Throwable t) {
        Log.e(TAG, msg, t);
        XposedBridge.log("StockGlass: " + msg + ": " + t);
    }

    /** Errors on a per-frame path are reported once per key, never per frame. */
    static void once(String key, String msg, Throwable t) {
        synchronized (once) {
            if (!once.add(key)) return;
        }
        if (t == null) w(msg);
        else e(msg, t);
    }
}
