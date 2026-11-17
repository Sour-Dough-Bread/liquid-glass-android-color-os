package dev.stockglass.harness;

import android.util.Log;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.util.HashMap;
import java.util.Map;

/**
 * Live-reloadable shader parameters.
 *
 * Backed by /data/local/tmp/glass_params.txt, `key=value` per line, `#` comments.
 * The view stats the file every frame-ish and reloads on mtime change, so the
 * tuning loop is a single `adb push` with no rebuild, no reinstall, no restart.
 *
 * This is deliberately the same shape the SurfaceFlinger engine will use for its
 * `persist.sys.glass.*` props: the point is that calibration numbers arrived at
 * here transfer verbatim.
 *
 * Defaults are the `regular` variant of expo-liquid-glass-view (android-port),
 * GlassEnums.kt REGULAR_DEFAULTS. Lengths are in dp and are scaled by density at
 * upload time, exactly as GlassAppearance.resolve() does.
 */
public final class Params {

    public static final String PATH = "/data/local/tmp/glass_params.txt";
    private static final String TAG = "GlassHarness";

    private final Map<String, Float> values = new HashMap<>();
    /**
     * Every reload restarts from these. Without it the file is a sticky overlay:
     * a key you delete keeps its last value, so an experiment silently inherits
     * the previous one's settings and you measure the wrong thing.
     */
    private final Map<String, Float> defaults = new HashMap<>();
    private long loadedMtime = -1;

    public Params() {
        // ---- geometry (dp) ----
        put("cornerRadiusDp", 28f);
        // 2.0 = circular, which is what an SkRRect from a real BlurRegion means.
        // 3.3418 = Apple's continuous corner (upstream research/05). SkRRect has
        // nowhere to carry this, so on the SF path it is global config.
        put("cornerShape", 2f);

        // ---- refraction (REGULAR_DEFAULTS) ----
        put("refractionWidthDp", 20f);    // -> refractionScale.x
        put("refractionHeightDp", 20f);   // -> refractionScale.y
        put("refractionAmountDp", 60f);
        put("refractionDepth", 1f);       // -> depthEffect
        put("refractionSwirl", 0f);       // DEFAULT_REFRACTION_SWIRL
        put("curvePower", 1f);            // -> profilePower
        put("curveBias", 0f);             // -> profileBias

        // ---- dispersion ----
        put("dispersionAmountDp", 6f);
        put("dispersionHeightDp", 20f);   // dispersionReachPx defaults to refractionHeight
        // HIGH tier is one tap per PIXEL (upstream ShaderQuality.HIGH). MEDIUM is
        // one per point, i.e. this value = density. Must stay <= density or the
        // spread >= 2 invariant breaks and every edge gets a blue fringe.
        put("dispersionTapSpacingPx", 1f);
        put("dispersionQuadrant", 0f);    // DEFAULT_DISPERSION_QUADRANT

        // ---- the frost handover (new for the drawBlurRegion contract) ----
        // These two have no upstream iOS calibration behind them; they were
        // swept on the A16 AVD (docs/VERIFIED.md). Measured rim-band contrast
        // vs deep-interior contrast showed them cleanly orthogonal: blurFalloff
        // moves the rim only (interior sat at 4.18 for every value tested),
        // while blurMix moves both.
        //
        // 1 = the interior hands over entirely to the compositor's Kawase blur.
        // Anything less leaves ghosted backdrop structure that reads as a smudge
        // rather than as glass, and the compositor already chose that radius for
        // this region — honouring it is what makes glassify-all look native.
        put("blurMix", 1f);
        // 2.0 captures most of the available rim crispness (25.5 of ~29.3 max)
        // and returns fall off hard after: 0.5->2.0 gains 12.6, 2.0->4.0 gains
        // 3.8, and by 4.0 backdrop lines start bleeding into the side edges.
        put("blurFalloff", 2f);
        // How much of the blurred child the refraction rim keeps. Without it the
        // rim reads only the sharp child and so ignores the blur entirely —
        // measured at 9x the fine detail of the surrounding blurred content on
        // the compositor path.
        //
        // The engine does NOT take this value: it derives lensBlur from the blur
        // radius the app asked for (persist.sys.glass.lensBlur < 0 = auto), which
        // this harness has no equivalent of. 0.3 is what the engine would compute
        // for the harness's own blurRadiusPx of 24 against its reference of 80,
        // so the two agree on this backdrop.
        put("lensBlur", 0.3f);

        // ---- tone ----
        // NB: the SurfaceFlinger engine deliberately does NOT inherit these two.
        // Upstream applies its own blur AND its own frost as one design, which
        // is right here; on the compositor path the blur already exists and was
        // chosen by whoever requested the region, so these values double-count
        // it. The engine ships frost 0.12 / saturation 1.15 — see
        // engine/src/glass_params.cpp. Every other number transfers verbatim.
        put("frost", 0.36f);              // -> frostColor.a
        put("frostR", 0.08f);
        put("frostG", 0.08f);
        put("frostB", 0.09f);
        put("tintR", 0f);
        put("tintG", 0f);
        put("tintB", 0f);
        put("tintA", 0f);
        put("saturation", 1.8f);
        put("light", 0f);                 // -> lightIntensity
        put("noise", 0.05f);
        put("glassOpacity", 1f);

        // ---- highlight ----
        put("highlightIntensity", 0.25f);
        // 180 puts the lobe axis vertical: top and bottom edges lit, side rims
        // fading to zero at the midpoints (upstream research/04).
        put("highlightAngleDeg", 180f);
        put("highlightWidthDp", 0.75f);
        put("highlightFalloff", 1f);

        // ---- HDR ----
        // Exactly 1.0 on SDR, and every HDR line in the shader then reduces to
        // the pre-HDR build bit-for-bit.
        put("hdrHeadroom", 1f);

        // ---- press (phase 3; the shared-memory channel on the SF path) ----
        put("touchGlow", 0f);
        put("touchLens", 0f);

        // ---- regular-glass blend and tone (engine glass_params.cpp) ----
        put("softenDp", 0f);
        put("fillLighten", 0f);
        put("fillDarken", 0f);
        put("lumaLimit", 0f);
        put("toneScale", 1f);
        put("toneBias", 0f);

        // ---- harness-only ----
        put("backdrop", 0f);              // which procedural backdrop to draw
        put("blurRadiusPx", 24f);         // the Kawase stand-in on the 0.25x image
        // What the probe dialog asks SurfaceFlinger for. Sweeping this is how
        // the engine's lensBlurRef gets a measured value instead of a guess:
        // Kawase saturates, so blur strength plateaus while a linear lensBlur
        // keeps climbing, and the gap between them is a visible mismatch band.
        put("probeBlurRadius", 80f);
        // 0 = full-screen blur-behind (settled, always reachable); 1 = a rounded
        // pane from the window's own background blur, off the display origin.
        put("probePane", 0f);
        put("probeCornerDp", 28f);
        put("showSplit", 1f);             // draw a no-glass reference half
    }

    private void put(String k, float v) { values.put(k, v); defaults.put(k, v); }

    public float f(String k) {
        Float v = values.get(k);
        if (v == null) {
            Log.w(TAG, "unknown param " + k);
            return 0f;
        }
        return v;
    }

    /** @return true when the file changed and values were reloaded. */
    public boolean reloadIfChanged() {
        File file = new File(PATH);
        if (!file.exists()) {
            if (loadedMtime != -1) { loadedMtime = -1; values.putAll(defaults); return true; }
            return false;
        }
        long mtime = file.lastModified();
        if (mtime == loadedMtime) return false;
        loadedMtime = mtime;

        values.putAll(defaults);
        int applied = 0;
        try (BufferedReader r = new BufferedReader(new FileReader(file))) {
            String line;
            while ((line = r.readLine()) != null) {
                line = line.trim();
                if (line.isEmpty() || line.startsWith("#")) continue;
                int eq = line.indexOf('=');
                if (eq <= 0) continue;
                String k = line.substring(0, eq).trim();
                String v = line.substring(eq + 1).trim();
                if (!values.containsKey(k)) {
                    Log.w(TAG, "ignoring unknown param: " + k);
                    continue;
                }
                try {
                    values.put(k, Float.parseFloat(v));
                    applied++;
                } catch (NumberFormatException e) {
                    Log.w(TAG, "bad value for " + k + ": " + v);
                }
            }
        } catch (Exception e) {
            Log.w(TAG, "could not read " + PATH, e);
        }
        Log.i(TAG, "reloaded " + applied + " params");
        return true;
    }
}
