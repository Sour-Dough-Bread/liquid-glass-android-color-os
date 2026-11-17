package dev.stockglass.companion;

import android.graphics.Canvas;
import android.view.View;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.HashMap;
import java.util.Map;

/**
 * Reflection into the Jetpack Compose that SystemUI bundles. Nothing here is
 * compiled against Compose: the host ships its own R8-processed copy, so names
 * are looked up at run time in the host's class loader, and every lookup was
 * checked against both A17 builds in hand (the AVD's SystemUIGoogle and
 * crDroid 13.1's SystemUI). Kotlin lambdas are made with java.lang.reflect.Proxy
 * on the host's own kotlin.jvm.functions interfaces.
 */
final class ComposeBridge {
    private final ClassLoader cl;
    private final Class<?> function0, function1, densityClass;
    final Method invoke0;
    private final Object unit;
    private final Method drawBehind;
    private final Method colorBox, red, green, blue;
    private final Field colorValue;
    private final Object localView;
    private final Class<?> compositionLocal;
    private final Field internalCanvas;
    private final Constructor<?> intState;
    private final Method intGet, intSet;
    private final Map<Class<?>, Method[]> scopeMethods = new HashMap<>();
    private final Map<Class<?>, Method> canvasGetters = new HashMap<>();
    private final Map<Class<?>, Method> outlineMethods = new HashMap<>();
    private final Map<Class<?>, Method> consumeMethods = new HashMap<>();

    ComposeBridge(ClassLoader cl) throws ReflectiveOperationException {
        this.cl = cl;
        function0 = load("kotlin.jvm.functions.Function0");
        function1 = load("kotlin.jvm.functions.Function1");
        invoke0 = function0.getMethod("invoke");
        unit = load("kotlin.Unit").getField("INSTANCE").get(null);
        drawBehind = load("androidx.compose.ui.draw.DrawModifierKt").getMethod("drawBehind",
                load("androidx.compose.ui.Modifier"), function1);
        Class<?> color = load("androidx.compose.ui.graphics.Color");
        colorBox = color.getMethod("box-impl", long.class);
        colorValue = color.getDeclaredField("value");
        colorValue.setAccessible(true);
        red = color.getMethod("getRed-impl", long.class);
        green = color.getMethod("getGreen-impl", long.class);
        blue = color.getMethod("getBlue-impl", long.class);
        Field lv = load("androidx.compose.ui.platform.AndroidCompositionLocals_androidKt")
                .getDeclaredField("LocalView");
        lv.setAccessible(true);
        localView = lv.get(null);
        compositionLocal = load("androidx.compose.runtime.CompositionLocal");
        internalCanvas = load("androidx.compose.ui.graphics.AndroidCanvas")
                .getDeclaredField("internalCanvas");
        internalCanvas.setAccessible(true);
        densityClass = load("androidx.compose.ui.unit.Density");
        Constructor<?> ctor = null;
        Method get = null, set = null;
        try {
            // What mutableIntStateOf returns on Android, in both builds.
            Class<?> st = load("androidx.compose.runtime.ParcelableSnapshotMutableIntState");
            ctor = st.getConstructor(int.class);
            get = st.getMethod("getIntValue");
            set = st.getMethod("setIntValue", int.class);
        } catch (ReflectiveOperationException e) {
            L.once("int-state", "no snapshot int state in this Compose; a switch reaches each"
                    + " tile when SystemUI next redraws it", null);
        }
        intState = ctor;
        intGet = get;
        intSet = set;
    }

    private Class<?> load(String name) throws ClassNotFoundException {
        return Class.forName(name, false, cl);
    }

    // ---- lambdas ---------------------------------------------------------

    interface Fn0 {
        Object call() throws Throwable;
    }

    interface Fn1 {
        void call(Object arg) throws Throwable;
    }

    Object function0(final Fn0 body, final String name) {
        return proxy(function0, name, (p, m, a) -> body.call());
    }

    Object function1(final Fn1 body, final String name) {
        return proxy(function1, name, (p, m, a) -> {
            body.call(a[0]);
            return unit;
        });
    }

    private Object proxy(Class<?> iface, String name, InvocationHandler invoke) {
        return Proxy.newProxyInstance(cl, new Class<?>[] {iface}, (p, m, a) -> {
            switch (m.getName()) {
                case "invoke":
                    return invoke.invoke(p, m, a);
                case "equals":
                    return p == a[0];
                case "hashCode":
                    return System.identityHashCode(p);
                case "toString":
                    return name;
                default:
                    throw new UnsupportedOperationException(m.getName());
            }
        });
    }

    // ---- composition -----------------------------------------------------

    /** {@code modifier.drawBehind(onDraw)}; equal across recompositions for the same onDraw. */
    Object drawBehind(Object modifier, Object onDraw) throws ReflectiveOperationException {
        return drawBehind.invoke(null, modifier, onDraw);
    }

    /** LocalView.current, read through the composer handed to a composable. */
    View localView(Object composer) throws ReflectiveOperationException {
        Method consume = consumeMethods.get(composer.getClass());
        if (consume == null) {
            consume = composer.getClass().getMethod("consume", compositionLocal);
            consumeMethods.put(composer.getClass(), consume);
        }
        return (View) consume.invoke(composer, localView);
    }

    // ---- state -----------------------------------------------------------

    /**
     * A snapshot int state, as mutableIntStateOf makes, or null if this Compose
     * has none. A draw lambda that reads it is redrawn by Compose whenever it
     * is written, on screen or not, which is how a switch reaches tiles that
     * SystemUI itself would not redraw (an off-screen page of the pager).
     */
    Object newIntState() throws ReflectiveOperationException {
        return intState == null ? null : intState.newInstance(0);
    }

    /** Reads {@code state}, so the draw or composition running now depends on it. */
    int read(Object state) throws ReflectiveOperationException {
        return state == null ? 0 : (int) intGet.invoke(state);
    }

    /** Main thread, outside a draw pass. */
    void write(Object state, int value) throws ReflectiveOperationException {
        if (state != null) intSet.invoke(state, value);
    }

    // ---- colours ---------------------------------------------------------

    long colorValue(Object boxed) throws IllegalAccessException {
        return colorValue.getLong(boxed);
    }

    Object box(long value) throws ReflectiveOperationException {
        return colorBox.invoke(null, value);
    }

    /** Alpha of a packed Compose colour (sRGB: 8 bits at 56; other spaces: 10 bits at 6). */
    static float alpha(long v) {
        if ((v & 0x3fL) == 0) return ((v >>> 56) & 0xff) / 255f;
        return ((v >>> 6) & 0x3ff) / 1023f;
    }

    static long withAlpha(long v, float a) {
        a = Math.max(0f, Math.min(1f, a));
        if ((v & 0x3fL) == 0) {
            long argb = v >>> 32;
            long na = Math.round(a * 255f);
            return ((argb & 0x00ffffffL) | (na << 24)) << 32;
        }
        long na = Math.round(a * 1023f);
        return (v & ~(0x3ffL << 6)) | (na << 6);
    }

    /** 0xRRGGBB of a packed Compose colour, its channels clamped to 0..1. */
    int rgb(long v) throws ReflectiveOperationException {
        return byteOf((float) red.invoke(null, v)) << 16
                | byteOf((float) green.invoke(null, v)) << 8
                | byteOf((float) blue.invoke(null, v));
    }

    private static int byteOf(float c) {
        return Math.round(Math.max(0f, Math.min(1f, c)) * 255f);
    }

    /** Max minus min channel: about 0 for greys and the surface tones, high for an accent. */
    float chroma(long v) throws ReflectiveOperationException {
        float r = (float) red.invoke(null, v);
        float g = (float) green.invoke(null, v);
        float b = (float) blue.invoke(null, v);
        return Math.max(r, Math.max(g, b)) - Math.min(r, Math.min(g, b));
    }

    // ---- drawing ---------------------------------------------------------

    /** DrawScope methods by run-time class: getSize, getDrawContext, getLayoutDirection. */
    private Method[] scope(Object drawScope) throws NoSuchMethodException {
        Class<?> c = drawScope.getClass();
        Method[] m = scopeMethods.get(c);
        if (m == null) {
            m = new Method[] {
                c.getMethod("getSize-NH-jbRc"),
                c.getMethod("getDrawContext"),
                c.getMethod("getLayoutDirection"),
            };
            scopeMethods.put(c, m);
        }
        return m;
    }

    /** DrawScope.size, packed: width float bits high, height low. */
    long size(Object drawScope) throws ReflectiveOperationException {
        return (long) scope(drawScope)[0].invoke(drawScope);
    }

    static float width(long size) {
        return Float.intBitsToFloat((int) (size >>> 32));
    }

    static float height(long size) {
        return Float.intBitsToFloat((int) size);
    }

    /** The android.graphics.Canvas a DrawScope is recording into, or null. */
    Canvas canvas(Object drawScope) throws ReflectiveOperationException {
        Object ctx = scope(drawScope)[1].invoke(drawScope);
        Method getCanvas = canvasGetters.get(ctx.getClass());
        if (getCanvas == null) {
            getCanvas = ctx.getClass().getMethod("getCanvas");
            canvasGetters.put(ctx.getClass(), getCanvas);
        }
        Object c = getCanvas.invoke(ctx);
        if (!internalCanvas.getDeclaringClass().isInstance(c)) return null;
        return (Canvas) internalCanvas.get(c);
    }

    /**
     * Corner radii of {@code shape} at {@code size}, as top-left, top-right,
     * bottom-right, bottom-left, through Shape.createOutline: what the tile's
     * own clip and background use. A shape whose outline is a free path has no
     * rounded-rect equivalent; it gets the largest radius that still fits.
     */
    float[] radii(Object shape, long size, Object drawScope) throws ReflectiveOperationException {
        Method create = outlineMethods.get(shape.getClass());
        if (create == null) {
            for (Method m : shape.getClass().getMethods()) {
                if (m.getName().startsWith("createOutline") && m.getParameterCount() == 3) {
                    create = m;
                    break;
                }
            }
            if (create == null) throw new NoSuchMethodException(shape.getClass() + ".createOutline");
            outlineMethods.put(shape.getClass(), create);
        }
        Method[] s = scope(drawScope);
        Object density = densityClass.isInstance(drawScope) ? drawScope
                : s[1].invoke(drawScope).getClass().getMethod("getDensity")
                        .invoke(s[1].invoke(drawScope));
        Object outline = create.invoke(shape, size, s[2].invoke(drawScope), density);
        String kind = outline.getClass().getName();
        if (kind.endsWith("Outline$Rounded")) {
            Field f = outline.getClass().getDeclaredField("roundRect");
            f.setAccessible(true);
            Object rr = f.get(outline);
            return new float[] {
                cornerX(rr, "topLeftCornerRadius"),
                cornerX(rr, "topRightCornerRadius"),
                cornerX(rr, "bottomRightCornerRadius"),
                cornerX(rr, "bottomLeftCornerRadius"),
            };
        }
        if (kind.endsWith("Outline$Rectangle")) return new float[4];
        float r = Math.min(width(size), height(size)) / 2f;
        L.once("outline:" + shape.getClass().getName(), "shape " + shape.getClass().getName()
                + " has a path outline; the glass uses a circle-ended pill", null);
        return new float[] {r, r, r, r};
    }

    // ---- layer alpha -----------------------------------------------------

    private Field drawNodeField, nodeCoordinator, wrappedBy, ownedLayer, lastLayerAlpha, explicitLayer,
            layerImpl, implAlpha;
    private int layersKnown; // 0 unread, 1 usable, -1 not in this Compose
    private final java.util.IdentityHashMap<Object, Float> alphaMemo = new java.util.IdentityHashMap<>();
    private final java.util.ArrayList<Object> alphaPath = new java.util.ArrayList<>();
    private int memoFrame = -1;

    /**
     * Compose fades with graphics layers (graphicsLayer { alpha }, Modifier.alpha,
     * AnimatedVisibility): the alpha is a property of the layer's RenderNode,
     * applied when hwui draws it. A region is a node of its own that only
     * reports where it is, so a layer's alpha never reaches it; the glass would
     * stay at full strength through a fade. These are the fields Compose itself
     * reads for NodeCoordinator.isTransparent(), public in both A17 builds.
     */
    private boolean layers() {
        if (layersKnown != 0) return layersKnown > 0;
        try {
            Class<?> scope = load("androidx.compose.ui.node.LayoutNodeDrawScope");
            Class<?> coordinator = load("androidx.compose.ui.node.NodeCoordinator");
            Class<?> layer = load("androidx.compose.ui.graphics.layer.GraphicsLayer");
            drawNodeField = scope.getField("drawNode");
            nodeCoordinator = load("androidx.compose.ui.Modifier$Node").getField("coordinator");
            wrappedBy = coordinator.getField("wrappedBy");
            ownedLayer = coordinator.getField("layer");
            lastLayerAlpha = coordinator.getField("lastLayerAlpha");
            explicitLayer = coordinator.getField("explicitLayer");
            layerImpl = layer.getField("impl");
            implAlpha = layerImpl.getType().getField("alpha");
            layersKnown = 1;
        } catch (Throwable t) {
            layersKnown = -1;
            L.once("layer-alpha", "this Compose does not expose its layers' alpha; glass under a"
                    + " control does not fade with it", t);
        }
        return layersKnown > 0;
    }

    /** The Modifier.Node a draw lambda is running for, or null. */
    Object drawNode(Object drawScope) {
        if (!layers() || !drawNodeField.getDeclaringClass().isInstance(drawScope)) return null;
        try {
            return drawNodeField.get(drawScope);
        } catch (IllegalAccessException e) {
            return null;
        }
    }

    /**
     * The opacity Compose draws {@code node} with: the product of the alpha of
     * every layer from the node's coordinator out to the root, which is how
     * hwui composes them. Main thread. Coordinators are shared by every control
     * in a subtree, so each is read once per frame (Region.frame()).
     */
    float layerAlpha(Object node) {
        if (node == null || !layers()) return 1f;
        try {
            int f = Region.frame();
            if (f != memoFrame) {
                alphaMemo.clear();
                memoFrame = f;
            }
            alphaPath.clear();
            float outer = 1f;
            for (Object c = nodeCoordinator.get(node); c != null; c = wrappedBy.get(c)) {
                Float known = alphaMemo.get(c);
                if (known != null) {
                    outer = known;
                    break;
                }
                alphaPath.add(c);
                if (alphaPath.size() > 4096) break; // a cycle would be a bug in Compose; stay finite
            }
            for (int i = alphaPath.size() - 1; i >= 0; --i) {
                Object c = alphaPath.get(i);
                outer *= ownAlpha(c);
                alphaMemo.put(c, outer);
            }
            alphaPath.clear();
            return outer;
        } catch (Throwable t) {
            L.once("layer-alpha-read", "cannot read a Compose layer's alpha; glass drawn at full"
                    + " opacity", t);
            return 1f;
        }
    }

    /** A coordinator's own layer alpha; 1 without a layer (lastLayerAlpha is stale then). */
    private float ownAlpha(Object coordinator) throws IllegalAccessException {
        if (ownedLayer.get(coordinator) == null) return 1f;
        Object explicit = explicitLayer.get(coordinator);
        // An explicit GraphicsLayer is set up by its owner, not by a layer block,
        // so lastLayerAlpha never hears of it.
        float a = explicit != null ? implAlpha.getFloat(layerImpl.get(explicit))
                : lastLayerAlpha.getFloat(coordinator);
        return Math.max(0f, Math.min(1f, a));
    }

    private static float cornerX(Object roundRect, String field) throws ReflectiveOperationException {
        Field f = roundRect.getClass().getDeclaredField(field);
        f.setAccessible(true);
        long packed = f.getLong(roundRect); // CornerRadius: x high, y low
        return Float.intBitsToFloat((int) (packed >>> 32));
    }
}
