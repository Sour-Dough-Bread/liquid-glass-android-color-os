package dev.stockglass.companion;

import de.robv.android.xposed.XposedBridge;
import java.lang.reflect.Member;
import java.lang.reflect.Method;

/**
 * Asks LSPosed to run methods without their compiled code, so that a call
 * the compiler inlined into them reaches a hook again.
 *
 * A hook replaces a method's entry point. A caller that dex2oat or the JIT
 * compiled with the callee's body inlined never goes through that entry, so a
 * hook on a two-line static (Compose's background()) can simply never fire
 * from a hot caller. LSPosed's XposedBridge.deoptimizeMethod makes the
 * runtime drop a method's compiled code and interpret it, calls included.
 *
 * It is LSPosed's own addition, absent from the classic API this module is
 * compiled against and from the dev loader, so it is looked up at run time;
 * without it the hooks still work wherever nothing was inlined.
 */
final class Deopt {
    private static Method deoptimize;
    private static boolean looked;

    private Deopt() {}

    /** Every method declared by each class that exists; returns how many were deoptimized. */
    static int classes(ClassLoader cl, String... names) {
        Method d = method();
        if (d == null) return 0;
        int n = 0;
        for (String name : names) {
            try {
                for (Method m : Class.forName(name, false, cl).getDeclaredMethods()) {
                    if (one(d, m)) n++;
                }
            } catch (Throwable t) {
                // A class this build does not have; nothing of it to deoptimize.
            }
        }
        return n;
    }

    private static boolean one(Method d, Member m) {
        try {
            d.invoke(null, m);
            return true;
        } catch (Throwable t) {
            return false;
        }
    }

    private static synchronized Method method() {
        if (!looked) {
            looked = true;
            try {
                deoptimize = XposedBridge.class.getMethod("deoptimizeMethod", Member.class);
            } catch (Throwable t) {
                L.once("deopt", "no XposedBridge.deoptimizeMethod (not LSPosed?); a call the"
                        + " compiler inlined skips its hook", null);
            }
        }
        return deoptimize;
    }
}
