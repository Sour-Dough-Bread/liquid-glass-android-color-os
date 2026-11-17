package de.robv.android.xposed;

import java.lang.reflect.Member;

/**
 * Under the dev loader only {@link #beforeHookedMethod} runs, and only to read
 * and replace arguments: a JVMTI breakpoint at the first instruction can rewrite
 * the argument registers, but cannot skip the method or see what it returns.
 * The companion is written to need nothing more.
 */
public abstract class XC_MethodHook {
    public XC_MethodHook() {}

    public XC_MethodHook(int priority) {}

    protected void beforeHookedMethod(MethodHookParam param) throws Throwable {}

    protected void afterHookedMethod(MethodHookParam param) throws Throwable {}

    /** Dev loader entry; not part of the Xposed API and never called by the module. */
    public final void devCallBefore(MethodHookParam param) throws Throwable {
        beforeHookedMethod(param);
    }

    public static final class MethodHookParam {
        public Member method;
        public Object thisObject;
        public Object[] args;
        private Object result;
        private Throwable throwable;

        public Object getResult() { return result; }

        public void setResult(Object result) {
            this.result = result;
            this.throwable = null;
        }

        public Throwable getThrowable() { return throwable; }

        public boolean hasThrowable() { return throwable != null; }

        public void setThrowable(Throwable throwable) { this.throwable = throwable; }
    }

    public class Unhook {
        private final Member method;

        public Unhook(Member method) { this.method = method; }

        public Member getHookedMethod() { return method; }

        public XC_MethodHook getCallback() { return XC_MethodHook.this; }

        public void unhook() {}
    }
}
