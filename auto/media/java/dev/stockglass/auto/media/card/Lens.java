package dev.stockglass.auto.media.card;

import android.graphics.BitmapShader;
import android.graphics.Matrix;
import android.graphics.Paint;
import android.graphics.RuntimeShader;
import android.graphics.Shader;

/**
 * One piece of glass: the StockGlass engine's lean shader
 * (shader/build/glass_lean.sksl, the code SurfaceFlinger runs for a blur
 * region) as an AGSL RuntimeShader, given the uniforms the engine's
 * fillUniforms gives a pane with the default preset ("basic" at variant 0,
 * which is "clear"; engine/src/glass_params.cpp).
 *
 * The engine hands the shader two backdrops, the screen and its blur; here
 * they are the card's own artwork (see Backdrop), as bitmap shaders whose
 * local matrices put the artwork where the card draws it. A button never
 * refracts its own fill or icon: only the artwork is ever sampled.
 */
final class Lens {
    /**
     * The engine's default preset, "clear" (glass_params.cpp kClear over the
     * defaults), with the changes noted for a small button on a dark card.
     */
    static final class Look {
        float refractionWidthDp = 24f, refractionHeightDp = 24f, refractionAmountDp = 60f;
        float refractionDepth = 1f, paneSpanDp = 480f, curvePower = 1f, curveBias = 0f;
        float blurMix = 0.3f, blurFalloff = 2f, lensBlur = 0.3f, rimFrost = 1f;
        float frost = 0f, frostR = 0.08f, frostG = 0.08f, frostB = 0.09f;
        // The preset's tint is 0.31: a veil for glass over a bright, busy
        // home screen. The card already lays a dark scrim over its artwork,
        // so the glass keeps a little of that tint and lifts its face by
        // 0.1, as Core Animation's dark regular glass does (face matrix bias
        // 0.125, FINDINGS.md 5.1). Its rim light is a little stronger and
        // wider than the preset's 0.42 / 1 dp, which is lost on a 64 dp
        // button after the head unit's H.264.
        float tintR = 0.13f, tintG = 0.10f, tintB = 0.13f, tintA = 0.12f;
        float saturation = 1.3f, light = 0f, glassOpacity = 1f;
        float highlightIntensity = 0.7f, highlightAngleDeg = 180f, highlightWidthDp = 1.5f, highlightFalloff = 1.9f;
        float cornerShape = 2f;
        float fillLighten = 0f, fillDarken = 0f, lumaLimit = 0f, toneScale = 1f, toneBias = 0.1f;
        /**
         * 1: refraction sized to the button, as Core Animation sizes it
         * (uikit.even.how datamine, FINDINGS.md 5.1: with Hm half the smaller
         * side in points, the rim reads min(Hm, 60) inward and the
         * displacement dies out min(Hm / 2, 20) in). The engine's own
         * 60 / 24 dp is for panes the size of a notification card, and on a
         * 64 dp button it folds the whole backdrop over. 0: the engine's.
         */
        float sized = 1f;

        boolean set(String key, float v) {
            try {
                getClass().getDeclaredField(key).setFloat(this, v);
                return true;
            } catch (Throwable t) {
                return false;
            }
        }

        String dump() {
            StringBuilder sb = new StringBuilder();
            for (java.lang.reflect.Field f : getClass().getDeclaredFields()) {
                try {
                    sb.append(f.getName()).append('=').append(f.getFloat(this)).append(' ');
                } catch (Throwable ignored) {
                }
            }
            return sb.toString();
        }
    }

    private static RuntimeShader shared;
    private static Paint paint;

    private Lens() {}

    /**
     * A paint that draws glass over (0, 0, w, h) in a slot's own coordinates.
     * Main thread only; draw with it before the next call.
     * {@code toBitmap} maps slot coordinates to artwork pixels; the blurred
     * backdrop is {@code blurScale} times smaller than the artwork.
     */
    static Paint paint(Look k, float w, float h, float radius, float dp, Backdrop b, Matrix slotToArt) {
        // One compiled shader for every slot: a display list keeps the shader
        // instance it was recorded with, and setting a uniform afterwards
        // makes a new instance for the next draw, so the next slot's
        // uniforms never reach an earlier slot's recording. Compiling it per
        // slot cost milliseconds on the UI thread while the pill morphs.
        if (shared == null) shared = new RuntimeShader(GlassSource.LEAN);
        RuntimeShader s = shared;
        Matrix local = new Matrix();
        // A shader's local matrix maps shader space (artwork px) to the slot.
        if (!slotToArt.invert(local)) local.reset();
        BitmapShader sharp = new BitmapShader(b.sharp, Shader.TileMode.CLAMP, Shader.TileMode.CLAMP);
        sharp.setLocalMatrix(local);
        Matrix blurLocal = new Matrix(local);
        blurLocal.preScale(1f / b.blurScale, 1f / b.blurScale);
        BitmapShader blurred = new BitmapShader(b.blurred, Shader.TileMode.CLAMP, Shader.TileMode.CLAMP);
        blurred.setLocalMatrix(blurLocal);
        // Bilinear: the blurred backdrop is a quarter size, as the engine's Kawase output is.
        blurred.setFilterMode(BitmapShader.FILTER_MODE_LINEAR);
        sharp.setFilterMode(BitmapShader.FILTER_MODE_LINEAR);
        s.setInputShader("sharpContent", sharp);
        s.setInputShader("blurredContent", blurred);

        float cap = Math.min(w, h) * 0.5f;
        float r = Math.min(cap, Math.max(radius, 0f));
        s.setFloatUniform("size", w, h);
        s.setFloatUniform("offset", 0f, 0f);
        s.setFloatUniform("crop", -1e6f, -1e6f, 1e6f, 1e6f);
        s.setFloatUniform("cornerExtents", r, r, r, r);
        s.setFloatUniform("cornerShapes", k.cornerShape, k.cornerShape, k.cornerShape, k.cornerShape);
        s.setFloatUniform("unitScale", dp);
        float rw = k.refractionWidthDp * dp, rh = k.refractionHeightDp * dp, ra = k.refractionAmountDp * dp;
        if (k.sized > 0.5f) {
            float hm = Math.min(w, h) * 0.5f / dp;
            ra = Math.min(hm, 60f) * dp;
            rw = rh = Math.min(hm * 0.5f, 20f) * dp;
        }
        s.setFloatUniform("refractionScale", rw, rh);
        s.setFloatUniform("refractionAmount", ra);
        s.setFloatUniform("refractionSwirl", 0f);
        s.setFloatUniform("depthEffect", k.refractionDepth * paneness(k, w, h, dp));
        s.setFloatUniform("profilePower", k.curvePower);
        s.setFloatUniform("profileBias", k.curveBias);
        s.setFloatUniform("dispersionHeight", 20f * dp);
        s.setFloatUniform("dispersionAmount", 0f);
        s.setFloatUniform("dispersionTapSpacing", 1f);
        s.setFloatUniform("dispersionQuadrant", 0f);
        s.setFloatUniform("blurMix", k.blurMix);
        s.setFloatUniform("blurFalloff", k.blurFalloff);
        s.setFloatUniform("lensBlur", k.lensBlur);
        s.setFloatUniform("rimFrost", k.rimFrost);
        s.setFloatUniform("tintColor", k.tintR, k.tintG, k.tintB, k.tintA);
        s.setFloatUniform("frostColor", k.frostR, k.frostG, k.frostB, k.frost);
        s.setFloatUniform("saturation", k.saturation);
        s.setFloatUniform("fillLighten", k.fillLighten);
        s.setFloatUniform("fillDarken", k.fillDarken);
        s.setFloatUniform("lumaLimit", k.lumaLimit);
        s.setFloatUniform("toneScale", k.toneScale);
        s.setFloatUniform("toneBias", k.toneBias, k.toneBias, k.toneBias);
        s.setFloatUniform("filterColor", 1f, 1f, 1f);
        s.setFloatUniform("lightIntensity", k.light);
        s.setFloatUniform("noiseAmount", 0f);
        s.setFloatUniform("glassOpacity", k.glassOpacity);
        double a = Math.toRadians(k.highlightAngleDeg);
        s.setFloatUniform("highlightIntensity", k.highlightIntensity);
        s.setFloatUniform("highlightDir", (float) Math.cos(a), (float) Math.sin(a));
        s.setFloatUniform("highlightWidth", k.highlightWidthDp * dp);
        s.setFloatUniform("highlightFalloff", Math.max(0.01f, k.highlightFalloff));
        s.setFloatUniform("hdrHeadroom", 1f);
        s.setFloatUniform("touchPos", w * 0.5f, h * 0.5f);
        s.setFloatUniform("touchGlow", 0f);
        s.setFloatUniform("touchLens", 0f);
        s.setFloatUniform("softenRadius", 0f);
        s.setFloatUniform("softenFast", 0f);
        s.setFloatUniform("deepBand", Math.max(Math.max(rw, rh),
                Math.max(Math.max(k.highlightWidthDp * dp, 7f * dp), 1f)) + 0.5f);
        if (paint == null) paint = new Paint();
        paint.setShader(s);
        return paint;
    }

    /** glass_draw.cpp panenessFor: depth effect fades out on panes wider than paneSpanDp. */
    private static float paneness(Look k, float w, float h, float dp) {
        float span = k.paneSpanDp * dp;
        if (span <= 0f) return 1f;
        float t = Math.max(0f, Math.min(1f, span / Math.max(Math.max(w, h), 1f)));
        return t * t * t * t;
    }

    /** Compiles the shader once, to fail early (and loudly) on a device whose AGSL refuses it. */
    static String check() {
        try {
            new RuntimeShader(GlassSource.LEAN);
            return null;
        } catch (Throwable t) {
            return String.valueOf(t.getMessage());
        }
    }
}
