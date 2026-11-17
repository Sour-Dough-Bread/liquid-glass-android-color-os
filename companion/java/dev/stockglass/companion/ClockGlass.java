package dev.stockglass.companion;

import android.app.WallpaperManager;
import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapShader;
import android.graphics.BlendMode;
import android.graphics.Color;
import android.graphics.Matrix;
import android.graphics.PorterDuff;
import android.graphics.PorterDuffColorFilter;
import android.graphics.Rect;
import android.graphics.RenderEffect;
import android.graphics.RuntimeShader;
import android.graphics.Shader;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.os.SystemClock;
import android.text.TextPaint;
import android.view.SurfaceControl;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewParent;
import android.view.ViewTreeObserver;
import android.view.WindowManager;
import android.widget.TextView;
import de.robv.android.xposed.XC_MethodHook;
import de.robv.android.xposed.XposedBridge;
import java.lang.ref.WeakReference;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.WeakHashMap;

/**
 * The lockscreen clock in Liquid Glass, after the clock in Kyant0's
 * AndroidLiquidGlass catalog (LockScreenContent.kt and SdfShader.kt,
 * Apache-2.0). This reimplements that demo's look and parameters; no code
 * is copied from it.
 *
 * The engine cannot do this. A blur region is a rounded rectangle, and
 * SurfaceFlinger never sees the glyphs. So the clock draws its own glass, in
 * SystemUI, as the demo does in its app: a RenderEffect on the clock's view
 * turns whatever the view draws into the glass's shape, so font weight, size
 * and digit animations are followed for free.
 *
 *   1. Shape. The demo reads a signed distance field from a prebaked
 *      texture. Here it is rebuilt every frame from the glyphs: they are
 *      blurred (a Gaussian of sigma s), and under a straight edge the blurred
 *      coverage is Phi(d / s), d being the depth below the edge, so the
 *      logistic approximation of Phi gives d back in closed form. The
 *      coverage's gradient gives the edge's outward normal n.
 *   2. Glass. The backdrop is sampled at p - k * h * n, where the refraction
 *      k = circleMap(1 - d / band) is 1 at the edge and 0 from depth `band`
 *      inwards, and h is the refraction height. Then the demo's look: a 25%
 *      white fill, colour controls (brightness -0.1, contrast 0.75,
 *      saturation 1.5) and a bevel lit from 45 degrees.
 *   3. DST_IN with the clock's own drawing keeps the glass to the glyphs,
 *      with their antialiased edges.
 *
 * Sizes follow the digits' height H, measured with the clock's own paint,
 * in the demo's proportions: band 0.09 H, refraction height 0.45 H, sigma
 * band / 2.
 *
 * The backdrop is the screen without SystemUI's shade window, which holds
 * the clock: IWindowManager.captureDisplay with that window's layer left out,
 * at a quarter of the display's resolution (the demo blurs its backdrop by
 * 2 dp anyway, and the bilinear upscale does about that). Both A17 SystemUI
 * builds hold READ_FRAME_BUFFER. A capture is taken once the clock has sat
 * on the lockscreen, shown, not dozing and at a steady alpha, for SETTLE_MS,
 * and again after every doze (the launcher may have scrolled the wallpaper)
 * and every wallpaper change. It is never taken while dozing, or while the
 * keyguard comes or goes, when it would show the app behind. Every frame the backdrop's
 * matrix maps the screen into the clock's own coordinates, so the glass stays
 * on the wallpaper while the clock moves (the large and small clock, the
 * shade pulled down over the lockscreen).
 *
 * Plain glass, a translucent white with the same bevel and no refraction,
 * stands in when there is no backdrop to refract: the capture failed, or
 * the wallpaper is live (it moves; a capture would not). The clock stays
 * stock while dozing (crossfading through the doze transition), while the
 * engine is off or app.clock is 0, outside the shade window (the wallpaper
 * picker's preview), and until the first capture has come back.
 *
 * Hooked views:
 *   - DigitalClockTextView: the flex clock's text, both A17 builds. The large
 *     clock is a FlexClockViewGroup that draws its four digit views itself,
 *     so it is the group that gets the effect; the small clock is one text
 *     view on its own.
 *   - crDroid's ClockStyle (its own clock styles): each TextClock whose
 *     digits are at least MIN_DIGIT_DP tall, i.e. the time, not the date.
 *   - Clock faces from plugins, taken at ClockEventController.setClock (see
 *     faces). crDroid ships Pixel's: Inflate, NumOverlap, Growth,
 *     Calligraphy, Metro and BigNum. Each face's view group draws all of its
 *     digits itself, so the group gets the effect, as the flex clock's does,
 *     and overlapping digits make one piece of glass. Analog faces (BigNum)
 *     stay stock. The faces are colourful, and plain glass would make all
 *     four digits one grey shape, so a face's own drawing tints its glass:
 *     glass * (1 - t) + face * t, t being app.clocktint (default 0.5), with
 *     the bevel's light added after, at full strength. A face's dozeFraction
 *     drives the doze crossfade, and its digit height comes from its text
 *     paint (or, for Metro's Lottie digits, LOTTIE_DIGIT of its tallest
 *     digit view).
 *
 * Main thread, like the views, except the capture's binder call.
 */
final class ClockGlass {
    private static final String TEXT = "com.android.systemui.customization.clocks.view.DigitalClockTextView";
    private static final String GROUP = "com.android.systemui.shared.clocks.view.FlexClockViewGroup";
    /** crDroid's own clock styles, absent elsewhere. */
    private static final String STYLE = "com.android.systemui.clocks.ClockStyle";
    /** Where SystemUI takes each new clock, whichever provider made it. */
    private static final String EVENTS = "com.android.keyguard.ClockEventController";
    private static final String CLOCKS = "com.android.systemui.plugins.keyguard.ui.clocks.";
    private static final int TYPE_NOTIFICATION_SHADE = 2040;

    /** The flex clock, a crDroid clock style, a clock face from a plugin. */
    private static final int FLEX = 0, STYLED = 1, FACE = 2;

    /** The demo's proportions, in digit heights (see the class comment). */
    private static final float BAND = 0.09f, HEIGHT = 0.45f;
    /** crDroid styles and plugin faces: smaller text (a date line) stays stock. */
    private static final float MIN_DIGIT_DP = 24f;
    /** A Lottie digit's glyph height, in its view's height (Metro). */
    private static final float LOTTIE_DIGIT = 0.7f;
    /** How long the clock must sit still on the lockscreen before a capture. */
    private static final long SETTLE_MS = 400;
    /** Glass appearing where the clock was stock fades in over this. */
    private static final long INTRO_MS = 250;
    private static final float CAPTURE_SCALE = 0.25f;

    private static final int STOCK = 0, GLASS = 1, PLAIN = 2;

    private static final String AGSL = ""
            + "uniform shader content;\n"   // the glyphs, Gaussian-blurred by sigma
            + "uniform shader backdrop;\n"  // the screen behind the clock, in its own coordinates
            + "uniform float sigma;\n"
            + "uniform float band;\n"
            + "uniform float height;\n"
            + "uniform float2 light;\n"
            + "uniform float plain;\n"
            // 0: the glass, lit; 1: the glass unlit; 2: the light alone, as white (see build)
            + "uniform float pass;\n"
            + "float circleMap(float x) { return 1.0 - sqrt(1.0 - x * x); }\n"
            + "half4 main(float2 p) {\n"
            + "    float b = content.eval(p).a;\n"
            + "    if (b < 0.004) { return half4(0.0); }\n"
            // depth below the edge: invert Phi(d / sigma), logistic approximation
            + "    float c = clamp(b, 0.004, 0.996);\n"
            + "    float depth = max(0.0, sigma * log(c / (1.0 - c)) / 1.702);\n"
            // outward normal: down the blurred coverage's gradient
            + "    float e = max(1.0, 0.5 * sigma);\n"
            + "    float2 g = float2(content.eval(p + float2(e, 0.0)).a - content.eval(p - float2(e, 0.0)).a,\n"
            + "                      content.eval(p + float2(0.0, e)).a - content.eval(p - float2(0.0, e)).a);\n"
            + "    float gl = length(g);\n"
            + "    float2 n = gl > 0.0001 ? -g / gl : float2(0.0);\n"
            + "    float k = circleMap(1.0 - min(1.0, depth / band));\n"
            + "    float lit = clamp(dot(n, light), 0.0, 1.0);\n"
            + "    float rim = clamp(dot(n, -light), 0.0, 1.0) * smoothstep(1.0, 0.0, abs(k - 0.25) * 6.0);\n"
            + "    float gain = (1.0 + 0.5 * k * lit) * (1.0 + 0.5 * rim);\n"
            + "    if (pass > 1.5) {\n"
            // as a screen blend, what multiplying mid-grey glass by gain would add
            + "        float w = clamp(0.8 * (gain - 1.0), 0.0, 1.0);\n"
            + "        return half4(w, w, w, w);\n"
            + "    }\n"
            + "    if (pass > 0.5) { gain = 1.0; }\n"
            + "    if (plain > 0.5) {\n"
            + "        float a = clamp(0.3 * gain, 0.0, 1.0);\n"
            + "        return half4(a, a, a, a);\n"
            + "    }\n"
            + "    float3 col = backdrop.eval(p - k * height * n).rgb;\n"
            + "    col = mix(col, float3(1.0), 0.25);\n"
            + "    float l = dot(col, float3(0.213, 0.715, 0.072));\n"
            + "    col = mix(float3(l), col, 1.5);\n"
            + "    col = (col - 0.5) * 0.75 + 0.4;\n"
            + "    col = clamp(col, 0.0, 1.0) * gain;\n"
            + "    return half4(half3(col), 1.0);\n"
            + "}\n";

    private static final WeakHashMap<View, Glass> all = new WeakHashMap<>();
    private static Class<?> groupClass;
    private static Field doze, styleDozing, styleClocks;
    private static boolean installed, listening;
    private static RuntimeShader shader;
    private static Handler main;

    private ClockGlass() {}

    static synchronized void install(ClassLoader cl) {
        if (installed) return;
        installed = true;
        int hooked = 0;
        try {
            Class<?> text = Class.forName(TEXT, false, cl);
            doze = field(text, "dozeFraction");
            try {
                groupClass = Class.forName(GROUP, false, cl);
            } catch (Throwable t) {
                groupClass = null;
            }
            hooked += layoutHook(text);
            if (groupClass != null) hooked += layoutHook(groupClass);
        } catch (Throwable t) {
            L.w("no flex clock of the expected shape (" + t + "); the lockscreen clock stays stock");
        }
        hooked += styles(cl);
        hooked += faces(cl);
        L.i("lockscreen clock hooked: " + hooked);
    }

    /**
     * Clock faces from plugins: crDroid ships Pixel's (SystemUIClocks-*
     * in /system_ext/priv-app). Each plugin has its own class loader and its
     * own copy of the clock view classes, so the hooks above never see them.
     * ClockEventController.setClock is where SystemUI takes every new clock,
     * from any provider, with both faces' views made. The flex clock passes
     * here too, but its views are SystemUI's own and the hooks above have it.
     */
    private static int faces(ClassLoader cl) {
        Method set, large, small, view, config, id;
        try {
            Class<?> clock = Class.forName(CLOCKS + "ClockController", false, cl);
            Class<?> face = Class.forName(CLOCKS + "ClockFaceController", false, cl);
            set = Class.forName(EVENTS, false, cl).getDeclaredMethod("setClock", clock);
            large = clock.getMethod("getLargeClock");
            small = clock.getMethod("getSmallClock");
            config = clock.getMethod("getConfig");
            view = face.getMethod("getView");
            id = Class.forName(CLOCKS + "ClockConfig", false, cl).getMethod("getId");
        } catch (Throwable t) {
            L.w("no clock handover of the expected shape (" + t + "); clock faces from plugins stay stock");
            return 0;
        }
        // whatever loaded SystemUI's own classes: the flex clock's views are its
        final ClassLoader own = set.getDeclaringClass().getClassLoader();
        XposedBridge.hookMethod(set, new XC_MethodHook() {
            @Override
            protected void beforeHookedMethod(MethodHookParam param) {
                Object c = param.args[0];
                if (c == null) return;
                try {
                    String name = String.valueOf(id.invoke(config.invoke(c)));
                    if (name.startsWith("ANALOG")) return; // hands and dial, no glyphs
                    for (Method m : new Method[] {large, small}) {
                        Object v = view.invoke(m.invoke(c));
                        if (v instanceof View && v.getClass().getClassLoader() != own) {
                            Glass.of((View) v, FACE, null).name = name;
                        }
                    }
                } catch (Throwable e) {
                    L.once("clock-face", "clock face hook failed; that face stays stock", e);
                }
            }
        });
        return 1;
    }

    /** Layout runs before the first frame, so the effect is in place for it. */
    private static int layoutHook(Class<?> c) throws NoSuchMethodException {
        Method onLayout = c.getDeclaredMethod("onLayout", boolean.class, int.class, int.class, int.class, int.class);
        XposedBridge.hookMethod(onLayout, new XC_MethodHook() {
            @Override
            protected void beforeHookedMethod(MethodHookParam param) {
                try {
                    View v = (View) param.thisObject;
                    ViewParent p = v.getParent();
                    View clock = groupClass != null && groupClass.isInstance(p) ? (View) p : v;
                    Glass.of(clock, FLEX, null);
                } catch (Throwable e) {
                    L.once("clock-flex", "clock hook failed; the clock stays stock", e);
                }
            }
        });
        return 1;
    }

    /**
     * crDroid's clock styles. applyClockAlpha runs right after a style's
     * layout is inflated, and again on each doze change.
     */
    private static int styles(ClassLoader cl) {
        Method apply;
        try {
            Class<?> c = Class.forName(STYLE, false, cl);
            apply = c.getDeclaredMethod("applyClockAlpha");
            styleDozing = field(c, "isDozing");
            styleClocks = field(c, "textClocks");
        } catch (Throwable t) {
            return 0; // not crDroid
        }
        XposedBridge.hookMethod(apply, new XC_MethodHook() {
            @Override
            protected void beforeHookedMethod(MethodHookParam param) {
                try {
                    Object list = styleClocks.get(param.thisObject);
                    if (!(list instanceof List)) return;
                    for (Object o : new ArrayList<>((List<?>) list)) {
                        if (o instanceof TextView) Glass.of((TextView) o, STYLED, param.thisObject);
                    }
                } catch (Throwable e) {
                    L.once("clock-style", "clock style hook failed; that style stays stock", e);
                }
            }
        });
        return 1;
    }

    private static synchronized Handler main() {
        if (main == null) main = new Handler(Looper.getMainLooper());
        return main;
    }

    private static RuntimeShader shader() {
        if (shader == null) shader = new RuntimeShader(AGSL);
        return shader;
    }

    /** Makes every clock rebuild its effect at its next frame, and asks for that frame. */
    static void refreshAll() {
        for (View v : new ArrayList<>(all.keySet())) {
            Glass g = v == null ? null : all.get(v);
            if (g == null) continue;
            g.builtMode = -1;
            v.invalidate();
        }
    }

    private static void listen(Context ctx) {
        if (listening) return;
        listening = true;
        Gate.onChange(ClockGlass::refreshAll);
        try {
            WallpaperManager.getInstance(ctx).addOnColorsChangedListener((colors, which) -> {
                Backdrop.stale = true;
                refreshAll();
            }, main());
        } catch (Throwable t) {
            L.once("clock-wallpaper", "no wallpaper change callback; the clock's backdrop is"
                    + " refreshed after each doze only", t);
        }
    }

    private static Field field(Class<?> c, String name) throws NoSuchFieldException {
        Field f = c.getDeclaredField(name);
        f.setAccessible(true);
        return f;
    }

    /** Per class: its dozeFraction field, or Boolean.FALSE for none. */
    private static final WeakHashMap<Class<?>, Object> dozeFields = new WeakHashMap<>();

    /** A float dozeFraction declared by {@code c} or a superclass, or null. */
    private static Field dozeField(Class<?> c) {
        Object f = dozeFields.get(c);
        if (f == null) {
            f = Boolean.FALSE;
            for (Class<?> k = c; k != null && k != View.class; k = k.getSuperclass()) {
                try {
                    Field d = k.getDeclaredField("dozeFraction");
                    if (d.getType() == float.class) {
                        d.setAccessible(true);
                        f = d;
                        break;
                    }
                } catch (NoSuchFieldException ignored) {
                    // keep looking up
                }
            }
            dozeFields.put(c, f);
        }
        return f instanceof Field ? (Field) f : null;
    }

    /** One clock view's glass. */
    private static final class Glass implements ViewTreeObserver.OnPreDrawListener, View.OnAttachStateChangeListener {
        private final WeakReference<View> view;
        private final int kind;
        /** A face's clock id, for the log. */
        String name;
        private WeakReference<Object> owner = new WeakReference<>(null);
        private WeakReference<View> digits = new WeakReference<>(null);
        private WeakReference<ViewTreeObserver> vto = new WeakReference<>(null);
        private int shade = -1;
        private boolean applied, logged, nudged;
        private long introStart;
        /** What the effect on the view was built for; builtMode -1 forces a rebuild. */
        int builtMode = -1;
        private int builtGen = -1;
        private float builtFade = -1f, builtDigit = -1f;
        private final float[] built = new float[9], now = new float[9];
        private final Matrix toScreen = new Matrix(), fromScreen = new Matrix(), local = new Matrix();
        private final int[] loc = new int[2], inWindow = new int[2];
        private final Rect bounds = new Rect();
        private float digitFor = -1f, digit;

        private Glass(View v, int kind) {
            view = new WeakReference<>(v);
            this.kind = kind;
        }

        static Glass of(View v, int kind, Object owner) {
            Glass g = all.get(v);
            if (g == null) {
                g = new Glass(v, kind);
                all.put(v, g);
                v.addOnAttachStateChangeListener(g);
                if (v.isAttachedToWindow()) g.watch(v);
                listen(v.getContext());
            }
            if (owner != null) g.owner = new WeakReference<>(owner);
            return g;
        }

        private void watch(View v) {
            ViewTreeObserver o = v.getViewTreeObserver();
            ViewTreeObserver old = vto.get();
            if (old == o) return;
            if (old != null && old.isAlive()) old.removeOnPreDrawListener(this);
            o.addOnPreDrawListener(this);
            vto = new WeakReference<>(o);
            shade = -1;
        }

        @Override
        public void onViewAttachedToWindow(View v) {
            watch(v);
        }

        @Override
        public void onViewDetachedFromWindow(View v) {
            ViewTreeObserver old = vto.get();
            if (old != null && old.isAlive()) old.removeOnPreDrawListener(this);
            vto = new WeakReference<>(null);
        }

        @Override
        public boolean onPreDraw() {
            View v = view.get();
            if (v == null) return true;
            try {
                update(v);
            } catch (Throwable t) {
                L.once("clock-draw", "clock glass failed; the clock stays stock", t);
                clear(v);
            }
            return true;
        }

        private void update(View v) {
            float fade = fade(v);
            if (fade > 0f) Backdrop.stale = true;
            if (!wanted(v) || fade >= 1f) {
                clear(v);
                return;
            }
            if (!v.isShown()) return;
            if (kind == FACE && v.getWidth() == 0 && !nudged) {
                // Inflate's small clock measures itself from its digits'
                // positions before they are laid out, so its first width is 0
                // and its digits draw outside it, where an effect cannot reach
                // (its date is misplaced too). The next minute's re-measure
                // fixes both; this asks for it now, once.
                nudged = true;
                v.requestLayout();
                return;
            }
            if (fade == 0f && v.getWidth() > 0) Backdrop.want(v, this);
            int mode = Backdrop.mode();
            if (mode == STOCK) {
                clear(v); // until the first capture is back
                return;
            }
            if (!applied && builtMode != mode) introStart = SystemClock.uptimeMillis();
            float intro = Math.min(1f, (SystemClock.uptimeMillis() - introStart) / (float) INTRO_MS);
            if (intro < 1f) {
                fade = Math.max(fade, 1f - intro);
                v.postInvalidateOnAnimation();
            }
            float h = digit(v);
            if (h <= 0f) {
                clear(v);
                return;
            }
            toScreen(v);
            toScreen.getValues(now);
            if (applied && mode == builtMode && fade == builtFade && h == builtDigit
                    && Backdrop.generation == builtGen && Arrays.equals(now, built)) {
                return;
            }
            build(v, mode, fade, h);
            System.arraycopy(now, 0, built, 0, 9);
            builtMode = mode;
            builtFade = fade;
            builtDigit = h;
            builtGen = Backdrop.generation;
            if (!logged) {
                logged = true;
                L.i("clock glass on " + v.getClass().getSimpleName() + (name != null ? " (" + name + ")" : "")
                        + " " + v.getWidth() + "x" + v.getHeight() + ": digits " + Math.round(h) + " px, "
                        + (mode == GLASS ? "refracting" : "plain")
                        + (kind == FACE ? ", tint " + Gate.clockTint() : ""));
            }
        }

        private void build(View v, int mode, float fade, float h) {
            float band = Math.max(1.5f, BAND * h);
            float sigma = band / 2f;
            RuntimeShader s = shader();
            s.setFloatUniform("sigma", sigma);
            s.setFloatUniform("band", band);
            s.setFloatUniform("height", HEIGHT * h);
            s.setFloatUniform("light", 0.70710677f, 0.70710677f);
            s.setFloatUniform("plain", mode == PLAIN ? 1f : 0f);
            s.setFloatUniform("pass", 0f);
            BitmapShader b = mode == GLASS ? Backdrop.shader : null;
            if (b != null) {
                // bitmap -> screen -> this view
                toScreen.invert(fromScreen);
                local.set(Backdrop.scale);
                local.postConcat(fromScreen);
                b.setLocalMatrix(local);
            } else {
                b = Backdrop.dummy();
            }
            s.setInputShader("backdrop", b);
            // RenderEffect's blur takes a radius: sigma = 0.57735 r + 0.5
            float radius = Math.max(0.5f, (sigma - 0.5f) / 0.57735f);
            RenderEffect blurred = RenderEffect.createBlurEffect(radius, radius, Shader.TileMode.DECAL);
            float tint = kind == FACE ? Gate.clockTint() : 0f;
            RenderEffect glass;
            if (tint <= 0f) {
                glass = RenderEffect.createChainEffect(RenderEffect.createRuntimeShaderEffect(s, "content"), blurred);
            } else {
                // A face keeps its colours: they are mixed into the unlit glass,
                // glass * (1 - tint) + face * tint, and the light goes on top
                // of that (a screen blend), so the rim stays as bright as on
                // clear glass. The blur is computed once for both passes.
                s.setFloatUniform("pass", 1f);
                RenderEffect base = RenderEffect.createChainEffect(RenderEffect.createRuntimeShaderEffect(s, "content"), blurred);
                s.setFloatUniform("pass", 2f);
                RenderEffect light = RenderEffect.createChainEffect(RenderEffect.createRuntimeShaderEffect(s, "content"), blurred);
                s.setFloatUniform("pass", 0f);
                RenderEffect mixed = RenderEffect.createBlendModeEffect(
                        RenderEffect.createColorFilterEffect(keep(1f - tint), base),
                        RenderEffect.createColorFilterEffect(keep(tint), RenderEffect.createOffsetEffect(0f, 0f)),
                        BlendMode.PLUS);
                glass = RenderEffect.createBlendModeEffect(mixed, light, BlendMode.SCREEN);
            }
            RenderEffect fx = RenderEffect.createBlendModeEffect(glass, RenderEffect.createOffsetEffect(0f, 0f),
                    BlendMode.DST_IN);
            if (fade > 0f) {
                // crossfade with the clock as drawn: glass * (1 - fade) + clock * fade
                fx = RenderEffect.createBlendModeEffect(
                        RenderEffect.createColorFilterEffect(keep(1f - fade), fx),
                        RenderEffect.createColorFilterEffect(keep(fade), RenderEffect.createOffsetEffect(0f, 0f)),
                        BlendMode.PLUS);
            }
            v.setRenderEffect(fx);
            applied = true;
        }

        private void clear(View v) {
            if (applied) {
                v.setRenderEffect(null);
                applied = false;
            }
            builtMode = -1;
        }

        /** Whether this clock gets glass at all (the backdrop aside). */
        private boolean wanted(View v) {
            if (!Gate.clock() || !inShade(v)) return false;
            return kind == FLEX || digit(v) >= MIN_DIGIT_DP * v.getResources().getDisplayMetrics().density;
        }

        private boolean inShade(View v) {
            if (shade < 0) {
                if (!v.isAttachedToWindow()) return false;
                ViewGroup.LayoutParams lp = v.getRootView().getLayoutParams();
                shade = lp instanceof WindowManager.LayoutParams
                        && ((WindowManager.LayoutParams) lp).type == TYPE_NOTIFICATION_SHADE ? 1 : 0;
            }
            return shade == 1;
        }

        /** 0 on the lockscreen, 1 dozing, in between through the transition. */
        float fade(View v) {
            try {
                if (kind == STYLED) {
                    Object o = owner.get();
                    return o != null && styleDozing.getBoolean(o) ? 1f : 0f;
                }
                if (kind == FACE) {
                    // the face's own view group has it, as its digit views do
                    Field f = dozeField(v.getClass());
                    return f == null ? 0f : Math.max(0f, Math.min(1f, f.getFloat(v)));
                }
                View t = text(v);
                return t == null ? 0f : Math.max(0f, Math.min(1f, doze.getFloat(t)));
            } catch (Throwable e) {
                return 0f;
            }
        }

        /**
         * A face's digit view, breadth first: its first TextView (the
         * plugins' DigitalClockTextView, measured by its paint), else its
         * tallest view with a dozeFraction of its own (Metro's Lottie digits,
         * measured by their height; the first may be an empty leading hour
         * digit). Null for a face with no digits.
         */
        private View digits(View v) {
            View d = digits.get();
            if (d != null && d.getParent() != null && (d instanceof TextView || d.getHeight() > 0)) return d;
            List<View> level = new ArrayList<>();
            level.add(v);
            for (int depth = 0; depth < 4 && !level.isEmpty(); depth++) {
                List<View> next = new ArrayList<>();
                View tallest = null;
                for (View c : level) {
                    // the face itself may be one text view (a single-layer
                    // face); a group's own dozeFraction does not count
                    if (c instanceof TextView) {
                        digits = new WeakReference<>(c);
                        return c;
                    }
                    if (c != v && dozeField(c.getClass()) != null
                            && (tallest == null || c.getHeight() > tallest.getHeight())) {
                        tallest = c;
                    }
                    if (c instanceof ViewGroup) {
                        ViewGroup g = (ViewGroup) c;
                        for (int i = 0; i < g.getChildCount(); i++) next.add(g.getChildAt(i));
                    }
                }
                if (tallest != null) {
                    if (tallest.getHeight() > 0) digits = new WeakReference<>(tallest);
                    return tallest;
                }
                level = next;
            }
            return null;
        }

        private View text(View v) {
            if (kind == FACE) return digits(v);
            if (v instanceof TextView) return v;
            if (v instanceof ViewGroup) {
                ViewGroup g = (ViewGroup) v;
                for (int i = 0; i < g.getChildCount(); i++) {
                    if (g.getChildAt(i) instanceof TextView) return g.getChildAt(i);
                }
            }
            return null;
        }

        /** The height of a digit in this clock's paint, px. */
        private float digit(View v) {
            View t = text(v);
            if (t != null && !(t instanceof TextView)) {
                return LOTTIE_DIGIT * t.getHeight(); // no paint to measure with
            }
            if (!(t instanceof TextView)) return 0f;
            TextPaint p = ((TextView) t).getPaint();
            float size = p.getTextSize();
            if (size != digitFor) {
                p.getTextBounds("0", 0, 1, bounds);
                digit = bounds.height();
                digitFor = size;
            }
            return digit;
        }

        /** This view's coordinates to the screen's, through every ancestor's transform. */
        private void toScreen(View v) {
            Matrix m = toScreen;
            m.reset();
            View cur = v;
            while (true) {
                Matrix own = cur.getMatrix();
                if (!own.isIdentity()) m.postConcat(own);
                m.postTranslate(cur.getLeft(), cur.getTop());
                ViewParent p = cur.getParent();
                if (!(p instanceof View)) break;
                View pv = (View) p;
                m.postTranslate(-pv.getScrollX(), -pv.getScrollY());
                cur = pv;
            }
            // the window's own position on the screen
            cur.getLocationOnScreen(loc);
            cur.getLocationInWindow(inWindow);
            m.postTranslate(loc[0] - inWindow[0], loc[1] - inWindow[1]);
        }

        /**
         * The view's alpha on the screen: its own times every ancestor's, 0
         * if any of them is not visible. Not 1 at rest everywhere (crDroid's
         * clock styles have an opacity setting), so a capture waits for it to
         * hold still rather than for 1.
         */
        float alpha(View v) {
            float a = 1f;
            for (View cur = v; cur != null; ) {
                if (cur.getVisibility() != View.VISIBLE) return 0f;
                a *= cur.getAlpha();
                ViewParent p = cur.getParent();
                cur = p instanceof View ? (View) p : null;
            }
            return a;
        }
    }

    /** Scales a premultiplied image by {@code a}. */
    private static PorterDuffColorFilter keep(float a) {
        int alpha = Math.max(0, Math.min(255, Math.round(a * 255f)));
        return new PorterDuffColorFilter(Color.argb(alpha, 0, 0, 0), PorterDuff.Mode.DST_IN);
    }

    /** The capture of the screen behind the clock (see the class comment). */
    private static final class Backdrop {
        static BitmapShader shader;
        /** The capture's pixels to the screen's. */
        static final Matrix scale = new Matrix();
        static int generation;
        static boolean stale = true;
        private static boolean failed, live, capturing, scheduled;
        /** The clock's alpha when the capture was asked for, then when it was taken. */
        private static float settledAlpha;
        private static int width, height;
        private static Handler bg;
        private static BitmapShader dummy;

        static int mode() {
            if (live || Gate.clockPlain()) return PLAIN;
            if (shader != null) return GLASS;
            return failed ? PLAIN : STOCK;
        }

        static BitmapShader dummy() {
            if (dummy == null) {
                Bitmap b = Bitmap.createBitmap(1, 1, Bitmap.Config.ARGB_8888);
                dummy = new BitmapShader(b, Shader.TileMode.CLAMP, Shader.TileMode.CLAMP);
            }
            return dummy;
        }

        /** The clock is on the lockscreen and not dozing; capture if there is reason to. */
        static void want(View v, Glass g) {
            if (Gate.clockPlain()) return; // nothing to refract
            View root = v.getRootView();
            if (shader != null && (root.getWidth() != width || root.getHeight() != height)) stale = true;
            if (shader == null && !failed && !live) stale = true; // e.g. app.clock was "plain"
            if (!stale || capturing || scheduled) return;
            schedule(v, g, 0);
        }

        private static void schedule(View v, Glass g, int tries) {
            scheduled = true;
            settledAlpha = g.alpha(v);
            final WeakReference<View> ref = new WeakReference<>(v);
            main().postDelayed(() -> {
                scheduled = false;
                View w = ref.get();
                if (w != null) start(w, g, tries);
            }, SETTLE_MS);
        }

        /**
         * On the lockscreen, not dozing, and not fading: the keyguard is not
         * coming or going (the capture would show the app behind it).
         */
        private static boolean ready(View v, Glass g) {
            float a = g.alpha(v);
            return v.isAttachedToWindow() && v.isShown() && g.fade(v) == 0f && a > 0.05f
                    && Math.abs(a - settledAlpha) < 0.002f && Gate.clock();
        }

        private static void start(View v, Glass g, int tries) {
            if (!stale || capturing) return;
            if (!ready(v, g)) {
                // Still settling (the keyguard fading in): try again, a few
                // times, as a lockscreen at rest draws no frame to ask with.
                if (tries < 8 && v.isAttachedToWindow() && v.isShown() && g.fade(v) == 0f) {
                    schedule(v, g, tries + 1);
                }
                return;
            }
            if (Gate.clockPlain()) return;
            if (liveWallpaper(v.getContext())) {
                if (!live) L.i("live wallpaper: the clock gets plain glass");
                live = true;
                stale = false;
                refreshAll();
                return;
            }
            SurfaceControl exclude = shadeLayer(v);
            if (exclude == null) {
                fail("the shade window has no layer", null);
                return;
            }
            final int display = v.getDisplay() != null ? v.getDisplay().getDisplayId() : 0;
            final Rect screen = v.getContext().getSystemService(WindowManager.class)
                    .getMaximumWindowMetrics().getBounds();
            final View root = v.getRootView();
            final int w = root.getWidth(), h = root.getHeight();
            final WeakReference<View> ref = new WeakReference<>(v);
            final long t0 = SystemClock.uptimeMillis();
            capturing = true;
            stale = false;
            bg().post(() -> {
                Bitmap shot = null;
                Throwable err = null;
                try {
                    shot = capture(display, exclude);
                } catch (Throwable t) {
                    err = t;
                }
                final Bitmap b = shot;
                final Throwable e = err;
                main().post(() -> done(b, e, screen, w, h, ref, g, t0));
            });
        }

        private static void done(Bitmap b, Throwable e, Rect screen, int w, int h, WeakReference<View> ref,
                Glass g, long t0) {
            capturing = false;
            live = false;
            if (b == null) {
                fail("no capture of the screen behind the clock", e);
                return;
            }
            View v = ref.get();
            if (v == null || !ready(v, g)) {
                // the lockscreen moved on while it was taken: it may show the app behind
                stale = true;
                return;
            }
            BitmapShader s = new BitmapShader(b, Shader.TileMode.CLAMP, Shader.TileMode.CLAMP);
            s.setFilterMode(BitmapShader.FILTER_MODE_LINEAR);
            shader = s;
            scale.setScale(screen.width() / (float) b.getWidth(), screen.height() / (float) b.getHeight());
            scale.postTranslate(screen.left, screen.top);
            width = w;
            height = h;
            failed = false;
            generation++;
            L.i("clock backdrop " + b.getWidth() + "x" + b.getHeight() + " for " + screen.width() + "x"
                    + screen.height() + " in " + (SystemClock.uptimeMillis() - t0) + " ms");
            refreshAll();
        }

        private static void fail(String why, Throwable e) {
            failed = true;
            L.once("clock-capture", why + "; the clock gets plain glass", e);
            refreshAll();
        }

        private static synchronized Handler bg() {
            if (bg == null) {
                HandlerThread t = new HandlerThread("StockGlassClock");
                t.start();
                bg = new Handler(t.getLooper());
            }
            return bg;
        }

        /** IWindowManager.captureDisplay, with {@code exclude} and its children left out. */
        private static Bitmap capture(int display, SurfaceControl exclude) throws Exception {
            Class<?> internal = Class.forName("android.window.ScreenCaptureInternal");
            Class<?> argsClass = Class.forName("android.window.ScreenCaptureInternal$CaptureArgs");
            Class<?> builder = Class.forName("android.window.ScreenCaptureInternal$CaptureArgs$Builder");
            Class<?> listenerClass = Class.forName("android.window.ScreenCaptureInternal$ScreenCaptureListener");
            Class<?> sync = Class.forName("android.window.ScreenCaptureInternal$SynchronousScreenCaptureListener");
            Object args = builder.getConstructor().newInstance();
            builder.getMethod("setFrameScale", float.class).invoke(args, CAPTURE_SCALE);
            builder.getMethod("setExcludeLayers", SurfaceControl[].class)
                    .invoke(args, (Object) new SurfaceControl[] {exclude});
            args = builder.getMethod("build").invoke(args);
            Object listener = internal.getMethod("createSyncCaptureListener").invoke(null);
            Object wm = Class.forName("android.view.WindowManagerGlobal").getMethod("getWindowManagerService")
                    .invoke(null);
            Class.forName("android.view.IWindowManager").getMethod("captureDisplay", int.class, argsClass,
                    listenerClass).invoke(wm, display, args, listener);
            Object buffer = sync.getMethod("getBuffer").invoke(listener);
            if (buffer == null) return null;
            return (Bitmap) buffer.getClass().getMethod("asBitmap").invoke(buffer);
        }

        /** The layer of the window {@code v} is in (the shade window). */
        private static SurfaceControl shadeLayer(View v) {
            try {
                Object root = View.class.getMethod("getViewRootImpl").invoke(v);
                if (root == null) return null;
                Object sc = root.getClass().getMethod("getSurfaceControl").invoke(root);
                return sc instanceof SurfaceControl && ((SurfaceControl) sc).isValid() ? (SurfaceControl) sc : null;
            } catch (Throwable t) {
                L.once("clock-layer", "cannot reach the shade window's layer", t);
                return null;
            }
        }

        /**
         * A live wallpaper moves, and a capture would not. SystemUI's own
         * wallpapers (ImageWallpaper, the colour gradient) are still images.
         */
        private static boolean liveWallpaper(Context ctx) {
            try {
                WallpaperManager wm = WallpaperManager.getInstance(ctx);
                Object info;
                try {
                    info = WallpaperManager.class.getMethod("getWallpaperInfo", int.class)
                            .invoke(wm, WallpaperManager.FLAG_LOCK);
                } catch (NoSuchMethodException e) {
                    info = wm.getWallpaperInfo();
                }
                if (info == null) return false;
                String pkg = ((android.app.WallpaperInfo) info).getPackageName();
                return !"com.android.systemui".equals(pkg);
            } catch (Throwable t) {
                return false;
            }
        }
    }
}
