package dev.stockglass.devloader;

import android.app.Application;
import android.util.Log;
import dalvik.system.PathClassLoader;
import de.robv.android.xposed.IXposedHookLoadPackage;
import de.robv.android.xposed.XC_MethodHook;
import de.robv.android.xposed.callbacks.XC_LoadPackage;
import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.lang.reflect.Constructor;
import java.lang.reflect.Member;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/**
 * Runs the companion's Xposed entry points in a process that has no Xposed
 * framework, for the userdebug test AVD only. libglassagent.so (a JVMTI agent,
 * attached with `cmd activity attach-agent`) loads this class from the APK's
 * assets/devloader.dex and calls {@link #attach}. Hooks are JVMTI breakpoints
 * at the first instruction of the hooked method; when one is hit the agent
 * reads the argument registers, calls {@link #dispatch}, and writes back any
 * argument a callback replaced.
 *
 * What this cannot do, by construction: run after-hooks, skip the original
 * method, or change its result. The companion uses before-hooks that replace
 * arguments and nothing else, so the same APK behaves the same here and under
 * LSPosed.
 */
public final class Loader {
    public static final String TAG = "StockGlassDev";

    private static final class Entry {
        final Member method;
        final int id;
        final List<XC_MethodHook> callbacks = new CopyOnWriteArrayList<>();

        Entry(Member method, int id) {
            this.method = method;
            this.id = id;
        }
    }

    private static final List<Entry> entries = new ArrayList<>();
    private static final ThreadLocal<Boolean> dispatching = new ThreadLocal<>();
    private static boolean warnedResult;

    private Loader() {}

    /** Implemented by the agent: sets the breakpoint, returns a hook id or -jvmtiError. */
    private static native int nativeHook(Member method, String shape, boolean isStatic);

    /** Called by the agent on the process's main thread, once. */
    static void attach(String apkPath) {
        try {
            Application app = (Application) Class.forName("android.app.ActivityThread")
                    .getMethod("currentApplication").invoke(null);
            ClassLoader moduleLoader = new PathClassLoader(apkPath, Loader.class.getClassLoader());
            XC_LoadPackage.LoadPackageParam p = new XC_LoadPackage.LoadPackageParam();
            p.packageName = app.getPackageName();
            p.processName = Application.getProcessName();
            p.classLoader = app.getClassLoader();
            p.appInfo = app.getApplicationInfo();
            p.isFirstApplication = true;
            Log.i(TAG, "attached to " + p.processName + " (" + p.packageName + "), module " + apkPath);
            for (String name : readInit(apkPath)) {
                Object mod = moduleLoader.loadClass(name).getDeclaredConstructor().newInstance();
                if (mod instanceof IXposedHookLoadPackage) {
                    ((IXposedHookLoadPackage) mod).handleLoadPackage(p);
                } else {
                    Log.w(TAG, name + " is not an IXposedHookLoadPackage; skipped");
                }
            }
        } catch (Throwable t) {
            Log.e(TAG, "attach failed", t);
        }
    }

    private static List<String> readInit(String apkPath) throws Exception {
        List<String> names = new ArrayList<>();
        try (ZipFile zip = new ZipFile(apkPath)) {
            ZipEntry e = zip.getEntry("assets/xposed_init");
            if (e == null) throw new IllegalStateException("no assets/xposed_init in " + apkPath);
            try (BufferedReader r = new BufferedReader(
                    new InputStreamReader(zip.getInputStream(e), StandardCharsets.UTF_8))) {
                for (String line; (line = r.readLine()) != null; ) {
                    line = line.trim();
                    if (!line.isEmpty() && !line.startsWith("#")) names.add(line);
                }
            }
        }
        return names;
    }

    /** Backs XposedBridge.hookMethod. */
    public static synchronized void hook(Member method, XC_MethodHook callback) {
        for (Entry e : entries) {
            if (e.method.equals(method)) {
                e.callbacks.add(callback);
                return;
            }
        }
        Class<?>[] params;
        if (method instanceof Method) {
            params = ((Method) method).getParameterTypes();
        } else if (method instanceof Constructor) {
            params = ((Constructor<?>) method).getParameterTypes();
        } else {
            Log.e(TAG, "cannot hook " + method);
            return;
        }
        StringBuilder shape = new StringBuilder();
        for (Class<?> c : params) shape.append(shapeOf(c));
        int id = nativeHook(method, shape.toString(), Modifier.isStatic(method.getModifiers()));
        if (id < 0) {
            Log.e(TAG, "breakpoint on " + method + " refused, jvmtiError " + (-id));
            return;
        }
        Entry e = new Entry(method, id);
        e.callbacks.add(callback);
        entries.add(e);
        Log.i(TAG, "hooked " + method.getDeclaringClass().getName() + "." + method.getName()
                + " (" + shape + ")");
    }

    private static char shapeOf(Class<?> c) {
        if (!c.isPrimitive()) return 'L';
        if (c == boolean.class) return 'Z';
        if (c == byte.class) return 'B';
        if (c == char.class) return 'C';
        if (c == short.class) return 'S';
        if (c == int.class) return 'I';
        if (c == long.class) return 'J';
        if (c == float.class) return 'F';
        return 'D';
    }

    /**
     * Called by the agent from the breakpoint, on the thread that hit it.
     * Returns the arguments to write back (a copy; the agent writes only the
     * ones that changed), or null to leave the call alone.
     */
    static Object[] dispatch(int id, Object self, Object[] args) {
        if (dispatching.get() != null) return null; // a callback calling a hooked method
        Entry entry = null;
        synchronized (Loader.class) {
            for (Entry e : entries) {
                if (e.id == id) {
                    entry = e;
                    break;
                }
            }
        }
        if (entry == null) return null;
        dispatching.set(Boolean.TRUE);
        try {
            XC_MethodHook.MethodHookParam p = new XC_MethodHook.MethodHookParam();
            p.method = entry.method;
            p.thisObject = self;
            p.args = args.clone();
            for (XC_MethodHook cb : entry.callbacks) {
                try {
                    cb.devCallBefore(p);
                } catch (Throwable t) {
                    Log.e(TAG, "before-hook of " + entry.method + " threw", t);
                }
            }
            if ((p.getResult() != null || p.hasThrowable()) && !warnedResult) {
                warnedResult = true;
                Log.w(TAG, "setResult/setThrowable ignored: the dev loader only replaces arguments");
            }
            return p.args;
        } finally {
            dispatching.remove();
        }
    }
}
