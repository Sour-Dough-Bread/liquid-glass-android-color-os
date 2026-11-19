package dev.stockglass.companion;

import android.graphics.Canvas;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import de.robv.android.xposed.XC_MethodHook;
import de.robv.android.xposed.XposedBridge;
import java.lang.ref.WeakReference;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.HashMap;
import java.util.Map;
import java.util.WeakHashMap;

/**
 * AxionOS's volume panel, which has no blur of its own.
 *
 * Read from AxionOS's SystemUI (Lineage-based A17) with jadx. With
 * Settings.Secure use_axion_volume_dialog on (the default) the volume panel is
 * com.android.systemui.axion.volume.AxionVolumeDialog, a ComponentDialog in a
 * window titled "AxionVolumeDialog" whose content is one ComposeView. Unlike
 * AOSP's VolumeDialog, which gives the ringer and every slider a
 * BackgroundBlurDrawable, nothing in it blurs, and no flag or property turns
 * that on: SurfaceFlinger had no region for it on a Poco F1, so the engine
 * had nothing to draw on.
 *
 * Every plate in it is painted in the theme's surfaceBright, which nothing
 * else in that dialog uses: the collapsed panel (56 dp wide, clipped to 32 dp
 * corners, then a RectangleShape background), the ringer button (a
 * FilledTonalIconButton: a Surface, 28 dp corners), the expanded panel (16 dp)
 * and its round button. All of them reach Compose's
 * BackgroundKt.background(Modifier, Color, Shape), the Surfaces through
 * SurfaceKt.surface.
 *
 * Two before-hooks:
 *
 *   MaterialTheme(colorScheme, ...)  in the volume dialog's window only, sets
 *       colorScheme.surfaceBright to app.fill of its alpha. The dialog builds a
 *       new scheme each composition (dynamicDark/LightColorScheme), and the
 *       colour flows from the theme to every plate, so no plate can come back
 *       opaque on a partial recomposition. That exact colour is the marker.
 *   background(modifier, color, shape)  for the marker colour only, puts a
 *       drawBehind before the background that draws a region at the plate's
 *       bounds. A panel's background is a RectangleShape inside clip(shape);
 *       the corners are then that clip's, read from the GraphicsLayerElement
 *       the clip added just before.
 *
 * The dialog fades and slides through a graphics layer, which the region
 * follows (ComposeBridge.layerAlpha). background() is two lines, so its
 * callers may have it inlined by the compiler; they are deoptimized
 * (Deopt) so the hook still sees the calls.
 */
final class AxionVolume {
    private static final String TITLE = "AxionVolumeDialog";
    private static final String COMPOSABLES = "com.android.systemui.axion.volume.ui.composable.";
    private static boolean installed;
    private static ComposeBridge compose;
    private static Field surfaceBright, combinedInner, layerClip, layerShape;
    private static Class<?> combinedModifier, graphicsLayerElement;
    /** Compose's RectangleShape singleton: a panel's background inside clip(shape). */
    private static Object rectangleShape;
    /** Read by the theme hook and every plate; written when the Gate changes. */
    private static Object gateState;
    private static int generation;

    /** The volume dialog's plate colour while glass is on, and the view it is drawn in. */
    private static volatile boolean marked;
    private static volatile long marker;
    private static WeakReference<View> host = new WeakReference<>(null);

    /** surfaceBright as each scheme had it, so turning glass off puts it back. */
    private static final Map<Object, Long> original = new WeakHashMap<>();
    /** Whether a ComposeView is in the volume dialog's window, once known. */
    private static final Map<View, Boolean> volumeViews = new WeakHashMap<>();
    /** One drawBehind per plate shape: Compose compares onDraw by identity. */
    private static final Map<Object, Plate> plates = new HashMap<>();

    private AxionVolume() {}

    static synchronized void install(ClassLoader cl) {
        if (installed) return;
        installed = true;
        try {
            Class.forName("com.android.systemui.axion.volume.AxionVolumeDialog", false, cl);
        } catch (Throwable t) {
            return; // not AxionOS; its own volume dialog blurs, or is another ROM's
        }
        Method theme = null, background = null;
        int scheme = -1, composer = -1;
        try {
            Class<?> colorScheme = Class.forName("androidx.compose.material3.ColorScheme", false, cl);
            Class<?> composerClass = Class.forName("androidx.compose.runtime.Composer", false, cl);
            for (Method m : Class.forName("androidx.compose.material3.MaterialThemeKt", false, cl)
                    .getDeclaredMethods()) {
                if (!m.getName().equals("MaterialTheme")) continue;
                Class<?>[] p = m.getParameterTypes();
                int s = indexOf(p, colorScheme), c = indexOf(p, composerClass);
                // The overload the dialog calls: (ColorScheme, Shapes, Typography,
                // content, Composer, changed, default); it calls the one with a
                // MotionScheme, which needs nothing more.
                if (s == 0 && c >= 0 && p.length == 7 && p[p.length - 1] == int.class && theme == null) {
                    theme = m;
                    scheme = s;
                    composer = c;
                }
            }
            Class<?> modifier = Class.forName("androidx.compose.ui.Modifier", false, cl);
            Class<?> shape = Class.forName("androidx.compose.ui.graphics.Shape", false, cl);
            for (Method m : Class.forName("androidx.compose.foundation.BackgroundKt", false, cl)
                    .getDeclaredMethods()) {
                Class<?>[] p = m.getParameterTypes();
                if (m.getName().startsWith("background-") && p.length == 3 && p[0] == modifier
                        && p[1] == long.class && p[2] == shape) {
                    background = m;
                }
            }
            surfaceBright = colorScheme.getDeclaredField("surfaceBright");
            surfaceBright.setAccessible(true);
            combinedModifier = Class.forName("androidx.compose.ui.CombinedModifier", false, cl);
            combinedInner = combinedModifier.getDeclaredField("inner");
            combinedInner.setAccessible(true);
            graphicsLayerElement = Class.forName("androidx.compose.ui.graphics.GraphicsLayerElement", false, cl);
            layerClip = graphicsLayerElement.getDeclaredField("clip");
            layerClip.setAccessible(true);
            layerShape = graphicsLayerElement.getDeclaredField("shape");
            layerShape.setAccessible(true);
            rectangleShape = Class.forName("androidx.compose.ui.graphics.RectangleShapeKt", false, cl)
                    .getField("RectangleShape").get(null);
            if (theme == null || background == null) {
                throw new NoSuchMethodException((theme == null ? "MaterialTheme(ColorScheme, ...)" : "")
                        + (background == null ? " background(Modifier, Color, Shape)" : ""));
            }
            compose = new ComposeBridge(cl);
            gateState = compose.newIntState();
        } catch (Throwable t) {
            L.w("AxionOS volume panel, but not the Compose it was read against (" + t
                    + "); it stays stock");
            return;
        }
        final int schemeArg = scheme, composerArg = composer;
        XposedBridge.hookMethod(theme, new XC_MethodHook() {
            @Override
            protected void beforeHookedMethod(MethodHookParam param) {
                try {
                    Object s = param.args[schemeArg];
                    if (s == null) return;
                    View v = compose.localView(param.args[composerArg]);
                    if (v == null || !inVolumeDialog(v)) return;
                    themed(s, v);
                } catch (Throwable e) {
                    L.once("volume-theme", "volume panel theme hook failed; it stays stock", e);
                }
            }
        });
        XposedBridge.hookMethod(background, new XC_MethodHook() {
            @Override
            protected void beforeHookedMethod(MethodHookParam param) {
                if (!marked || (long) param.args[1] != marker) return;
                try {
                    Object shape = plateShape(param.args[0], param.args[2]);
                    Plate p;
                    synchronized (plates) {
                        p = plates.get(shape);
                        if (p == null) {
                            p = new Plate(shape);
                            plates.put(shape, p);
                        }
                    }
                    param.args[0] = compose.drawBehind(param.args[0], p.drawProxy);
                } catch (Throwable e) {
                    L.once("volume-plate", "volume panel plate hook failed; it stays stock", e);
                }
            }
        });
        Gate.onChange(AxionVolume::gateChanged);
        int deopt = Deopt.classes(cl, COMPOSABLES + "VolumeDialogContentKt", COMPOSABLES + "RingerControlKt",
                "androidx.compose.material3.SurfaceKt");
        L.i("AxionOS volume panel hooked: MaterialTheme + " + background.getName()
                + (deopt > 0 ? " (" + deopt + " callers deoptimized)" : ""));
    }

    /** The volume dialog's scheme: plates translucent in glass mode, as the ROM had them otherwise. */
    private static void themed(Object scheme, View v) throws ReflectiveOperationException {
        compose.read(gateState);
        Long orig;
        synchronized (original) {
            orig = original.get(scheme);
            if (orig == null) {
                orig = surfaceBright.getLong(scheme);
                original.put(scheme, orig);
            }
        }
        if (!Gate.volume()) {
            surfaceBright.setLong(scheme, orig);
            marked = false;
            return;
        }
        long m = ComposeBridge.withAlpha(orig, ComposeBridge.alpha(orig) * Gate.fill());
        surfaceBright.setLong(scheme, m);
        marker = m;
        marked = true;
        if (host.get() != v) host = new WeakReference<>(v);
    }

    /**
     * The shape the plate is drawn in: its background's own, or, for a
     * RectangleShape background inside clip(shape), that clip's shape, which
     * clip() added to the chain just before the background.
     */
    private static Object plateShape(Object modifier, Object shape) throws IllegalAccessException {
        if (shape != rectangleShape) return shape;
        Object inner = combinedModifier.isInstance(modifier) ? combinedInner.get(modifier) : modifier;
        if (graphicsLayerElement.isInstance(inner) && layerClip.getBoolean(inner)) {
            Object clip = layerShape.get(inner);
            if (clip != null) return clip;
        }
        return shape;
    }

    private static boolean inVolumeDialog(View v) {
        Boolean known = volumeViews.get(v);
        if (known != null) return known;
        View root = v.getRootView();
        ViewGroup.LayoutParams lp = root == null ? null : root.getLayoutParams();
        if (!(lp instanceof WindowManager.LayoutParams)) return false; // not in a window yet; ask again
        CharSequence title = ((WindowManager.LayoutParams) lp).getTitle();
        boolean is = title != null && TITLE.contentEquals(title);
        volumeViews.put(v, is);
        return is;
    }

    /** Main thread. Recomposes the theme (and so every plate) after a switch; hides glass when off. */
    private static void gateChanged() {
        try {
            if (!Gate.volume()) {
                synchronized (plates) {
                    for (Plate p : plates.values()) p.hideAll();
                }
            }
            compose.write(gateState, ++generation);
        } catch (Throwable e) {
            L.once("volume-redraw", "cannot redraw the volume panel after a switch", e);
        }
    }

    private static int indexOf(Class<?>[] types, Class<?> type) {
        for (int i = 0; i < types.length; i++) {
            if (types[i] == type) return i;
        }
        return -1;
    }

    /**
     * The glass for every plate drawn in one shape. The drawBehind is shared,
     * so Compose sees the same modifier on every recomposition; each node it
     * runs for (one per plate on screen) has a region of its own.
     */
    private static final class Plate {
        final Object shape;
        final Object drawProxy;
        /** By node, weakly: a node that left the composition releases its region (ownedBy). */
        private final Map<Object, Region> regions = new WeakHashMap<>();
        private Region fallback;

        Plate(Object shape) {
            this.shape = shape;
            drawProxy = compose.function1(this::draw, "StockGlass.volumePlate");
        }

        void hideAll() {
            for (Region r : regions.values()) r.hide();
            if (fallback != null) fallback.hide();
        }

        private Region regionFor(Object node) {
            if (node == null) {
                if (fallback == null) fallback = new Region();
                return fallback;
            }
            Region r = regions.get(node);
            if (r == null) {
                r = new Region();
                r.ownedBy(node);
                final WeakReference<Object> n = new WeakReference<>(node);
                r.fadeWith(h -> compose.layerAlpha(n.get()));
                regions.put(node, r);
            }
            return r;
        }

        void draw(Object drawScope) {
            Region r = null;
            try {
                Region.reap();
                compose.read(gateState);
                Object node = compose.drawNode(drawScope);
                r = regionFor(node);
                View v = host.get();
                Canvas c = v == null ? null : compose.canvas(drawScope);
                if (!Gate.volume() || c == null) {
                    r.hide();
                    return;
                }
                long size = compose.size(drawScope);
                float[] radii = compose.radii(shape, size, drawScope);
                r.draw(v, c, 0, 0, Math.round(ComposeBridge.width(size)), Math.round(ComposeBridge.height(size)),
                        radii[0], radii[1], radii[2], radii[3], Gate.radius(), false);
            } catch (Throwable e) {
                L.once("volume-draw", "cannot draw a volume panel plate's glass", e);
                if (r != null) r.hide();
            }
        }
    }
}
