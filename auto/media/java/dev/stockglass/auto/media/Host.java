package dev.stockglass.auto.media;

import android.app.Application;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import dalvik.system.PathClassLoader;
import de.robv.android.xposed.XC_MethodHook;
import de.robv.android.xposed.XposedBridge;
import java.lang.reflect.Member;
import java.lang.reflect.Method;
import java.util.HashMap;
import java.util.Map;
import java.util.function.BiConsumer;

/**
 * Loads the glass ({@code card.CardGlass}) and owns its hooks.
 *
 * Under LSPosed this is a thin pass-through. Under the dev loader on the AVD
 * it also lets a new build replace the running one without restarting
 * Android Auto (which would drop the car's projection): the dev loader can
 * attach only once per process and can never remove a hook, so every hook is
 * registered here, once, with a callback that forwards to whichever build of
 * {@code card.CardGlass} is current. A build is loaded from its APK by a
 * class loader of its own, so its classes are new each time.
 *
 *   adb shell am broadcast -a dev.stockglass.auto.media.RELOAD [--es apk <path>]
 *   adb shell am broadcast -a dev.stockglass.auto.media.CMD --es cmd <command>
 *
 * Both receivers exist under the dev loader only.
 */
public final class Host {
    static final String TAG = "SGAutoMedia";
    private static final String IMPL = "dev.stockglass.auto.media.card.CardGlass";
    private static final String RELOAD = "dev.stockglass.auto.media.RELOAD";
    private static final String CMD = "dev.stockglass.auto.media.CMD";
    private static final String PKG = "dev.stockglass.auto.media";

    private static final Handler main = new Handler(Looper.getMainLooper());
    private static final Map<Member, String> hooked = new HashMap<>();
    private static ClassLoader app;
    private static String process;
    private static boolean dev;
    /** The current build's entry points; null while none is running. */
    private static volatile Method before;
    private static Method stop, command;
    private static int generation;

    private Host() {}

    static void start(ClassLoader appLoader, String processName) {
        app = appLoader;
        process = processName;
        try {
            Class.forName("dev.stockglass.devloader.Loader", false, Host.class.getClassLoader());
            dev = true;
        } catch (Throwable t) {
            dev = false;
        }
        main.post(Host::whenReady);
    }

    /** Under LSPosed this runs before Application.onCreate; wait for the application. */
    private static int waits;

    private static void whenReady() {
        Application a = application();
        if (waits++ % 25 == 0) Log.i(TAG, "whenReady #" + waits + " in " + process + ": application " + a);
        if (a == null) {
            main.postDelayed(Host::whenReady, 200);
            return;
        }
        if (dev) {
            listen(a);
            load(a, apkPath(a, null));
        } else {
            load(a, null);
        }
    }

    private static Application application() {
        try {
            return (Application) Class.forName("android.app.ActivityThread")
                    .getMethod("currentApplication").invoke(null);
        } catch (Throwable t) {
            return null;
        }
    }

    private static String apkPath(Context c, String override) {
        if (override != null) return override;
        try {
            return c.getPackageManager().getApplicationInfo(PKG, 0).sourceDir;
        } catch (Throwable t) {
            Log.w(TAG, "module package not visible; using the loaded build", t);
            return null;
        }
    }

    /** Stops the current build, if any, and starts the one in {@code apk} (null: this one). */
    private static void load(Context c, String apk) {
        if (stop != null) {
            try {
                stop.invoke(null);
            } catch (Throwable t) {
                Log.e(TAG, "stop failed", t);
            }
        }
        before = null;
        stop = command = null;
        try {
            ClassLoader cl;
            if (apk == null) {
                cl = Host.class.getClassLoader();
            } else {
                // Parent: whatever holds the Xposed API (the dev loader's own
                // loader), so the build's own classes all come from its APK.
                cl = new PathClassLoader(apk, Host.class.getClassLoader().getParent());
            }
            Class<?> impl = Class.forName(IMPL, true, cl);
            Method startM = impl.getMethod("start", Context.class, ClassLoader.class, BiConsumer.class, String.class);
            Method b = impl.getMethod("before", String.class, Object.class);
            stop = impl.getMethod("stop");
            command = impl.getMethod("command", String.class);
            generation++;
            BiConsumer<Member, String> hooker = Host::hook;
            startM.invoke(null, c, app, hooker, process);
            before = b;
            Log.i(TAG, "build " + generation + " running in " + process + (apk != null ? " from " + apk : ""));
        } catch (Throwable t) {
            Log.e(TAG, "could not start " + IMPL + (apk != null ? " from " + apk : ""), t);
        }
    }

    /** Called by a build: hook {@code m} once for good, forwarding to the current build as {@code key}. */
    private static synchronized void hook(Member m, String key) {
        String had = hooked.get(m);
        if (had != null) {
            if (!had.equals(key)) Log.w(TAG, m + " is already hooked as " + had + ", not " + key);
            return;
        }
        hooked.put(m, key);
        XposedBridge.hookMethod(m, new XC_MethodHook() {
            @Override
            protected void beforeHookedMethod(MethodHookParam p) {
                Method b = before;
                if (b == null) return;
                try {
                    b.invoke(null, key, p);
                } catch (Throwable t) {
                    Log.e(TAG, "hook " + key + " threw", t);
                }
            }
        });
    }

    private static void listen(Application a) {
        BroadcastReceiver r = new BroadcastReceiver() {
            @Override
            public void onReceive(Context c, Intent i) {
                if (RELOAD.equals(i.getAction())) {
                    load(a, apkPath(a, i.getStringExtra("apk")));
                } else if (CMD.equals(i.getAction()) && command != null) {
                    try {
                        Object out = command.invoke(null, i.getStringExtra("cmd"));
                        Log.i(TAG, "cmd " + i.getStringExtra("cmd") + ": " + out);
                    } catch (Throwable t) {
                        Log.e(TAG, "cmd failed", t);
                    }
                }
            }
        };
        IntentFilter f = new IntentFilter();
        f.addAction(RELOAD);
        f.addAction(CMD);
        a.registerReceiver(r, f, null, main, Context.RECEIVER_EXPORTED);
        Log.i(TAG, "dev loader: listening for " + RELOAD + " and " + CMD + " in " + process);
    }
}
