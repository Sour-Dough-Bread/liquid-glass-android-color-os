package dev.stockglass.harness;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapShader;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.LinearGradient;
import android.graphics.Matrix;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.RuntimeShader;
import android.graphics.Shader;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.View;

/**
 * Renders the SurfaceFlinger glass shader against a synthetic backdrop pair that
 * mimics BlurFilter::drawBlurRegion's inputs exactly.
 *
 * The point of this harness is that it is NOT an approximation of the compositor
 * path — it reproduces the contract:
 *
 *   sharpContent   full-resolution snapshot          local matrix = I
 *   blurredContent 0.25x blurred copy (kInputScale)  local matrix = Scale(4, 4)
 *
 * Both children are therefore evaluated at the SAME coordinate, which is the one
 * thing about this contract that is easy to get wrong and expensive to debug on
 * a device that soft-reboots when you do.
 *
 * The glass is drawn with drawRect(paneRect), NOT drawRoundRect: the shader owns
 * its own antialiasing via the SDF, and clipping to a round rect would feather
 * the edge twice.
 */
public class GlassView extends View {

    private static final String TAG = "GlassHarness";

    /** BlurFilter::kInputScale. */
    private static final float INPUT_SCALE = 0.25f;

    private final Params params = new Params();
    private final String skslSource;

    private RuntimeShader glass;
    private Bitmap sharpBitmap;
    private Bitmap blurredBitmap;
    private BitmapShader sharpShader;
    private BitmapShader blurredShader;

    private final Paint panePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Handler handler = new Handler(Looper.getMainLooper());
    private float lastBackdropKind = -1f;
    private float lastBlurRadius = -1f;

    public GlassView(Context ctx, String sksl) {
        super(ctx);
        this.skslSource = sksl;
        try {
            glass = new RuntimeShader(sksl);
            Log.i(TAG, "shader compiled OK (" + sksl.length() + " chars)");
        } catch (Throwable t) {
            // A compile failure here is the whole point of running this before
            // going anywhere near SurfaceFlinger, where the same mistake is a
            // soft reboot.
            Log.e(TAG, "SHADER COMPILE FAILED", t);
            glass = null;
        }
        startWatching();
    }

    private void startWatching() {
        handler.postDelayed(new Runnable() {
            @Override public void run() {
                if (params.reloadIfChanged()) invalidate();
                handler.postDelayed(this, 300);
            }
        }, 300);
    }

    @Override
    protected void onSizeChanged(int w, int h, int ow, int oh) {
        super.onSizeChanged(w, h, ow, oh);
        rebuildBackdrop(w, h);
    }

    // ------------------------------------------------------------- backdrop

    private void rebuildBackdrop(int w, int h) {
        if (w <= 0 || h <= 0) return;

        sharpBitmap = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
        drawBackdrop(new Canvas(sharpBitmap), w, h, (int) params.f("backdrop"));

        int bw = Math.max(1, Math.round(w * INPUT_SCALE));
        int bh = Math.max(1, Math.round(h * INPUT_SCALE));
        Bitmap small = Bitmap.createScaledBitmap(sharpBitmap, bw, bh, true);
        blurredBitmap = boxBlur(small, Math.max(1, Math.round(params.f("blurRadiusPx") * INPUT_SCALE)));
        if (blurredBitmap != small) small.recycle();

        sharpShader = new BitmapShader(sharpBitmap, Shader.TileMode.CLAMP, Shader.TileMode.CLAMP);
        blurredShader = new BitmapShader(blurredBitmap, Shader.TileMode.CLAMP, Shader.TileMode.CLAMP);

        // getShaderTransform(canvas, blurRect, kInverseInputScale). blurRect is
        // the whole view here, so the translate term is zero and only the scale
        // survives.
        Matrix m = new Matrix();
        m.setScale(1f / INPUT_SCALE, 1f / INPUT_SCALE);
        blurredShader.setLocalMatrix(m);

        lastBackdropKind = params.f("backdrop");
        lastBlurRadius = params.f("blurRadiusPx");
    }

    /**
     * Something with real high-frequency content: refraction and chromatic
     * dispersion are invisible over a smooth gradient, which is how a broken
     * dispersion walk ships looking fine.
     */
    private void drawBackdrop(Canvas c, int w, int h, int kind) {
        Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);

        p.setShader(new LinearGradient(0, 0, w, h,
                new int[]{0xFF1B2B5C, 0xFF7A2F86, 0xFFD4552B, 0xFF122A4A},
                null, Shader.TileMode.CLAMP));
        c.drawRect(0, 0, w, h, p);
        p.setShader(null);

        // Thin high-contrast bars: the single best test for chromatic fringing,
        // because a wrong tap walk turns them into a rainbow smear.
        p.setColor(Color.WHITE);
        for (int i = 0; i < 40; i++) {
            float y = h * 0.08f + i * (h * 0.02f);
            p.setStrokeWidth(1f + (i % 4));
            c.drawLine(0, y, w, y, p);
        }

        // Saturated blocks: expose per-channel displacement directly.
        int[] blocks = {0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFF00, 0xFF00FFFF, 0xFFFF00FF};
        float bw = w / 6f;
        for (int i = 0; i < blocks.length; i++) {
            p.setColor(blocks[i]);
            c.drawRect(i * bw, h * 0.62f, (i + 1) * bw, h * 0.72f, p);
        }

        // Text: the eye is brutally good at spotting wrong refraction on glyphs.
        p.setColor(Color.WHITE);
        p.setTextSize(w * 0.075f);
        p.setFakeBoldText(true);
        c.drawText("STOCK GLASS", w * 0.06f, h * 0.30f, p);
        p.setTextSize(w * 0.045f);
        p.setFakeBoldText(false);
        c.drawText("drawBlurRegion harness", w * 0.06f, h * 0.36f, p);

        // A dense checker to stress the sampler at the crop boundary.
        p.setColor(0xFF101010);
        int cell = Math.max(6, w / 60);
        for (int y = (int) (h * 0.78f); y < h * 0.94f; y += cell) {
            for (int x = 0; x < w; x += cell) {
                if (((x / cell) + (y / cell)) % 2 == 0) c.drawRect(x, y, x + cell, y + cell, p);
            }
        }
    }

    /** Three box passes ~= Gaussian; stands in for the compositor's Kawase output. */
    private static Bitmap boxBlur(Bitmap src, int radius) {
        if (radius < 1) return src;
        int w = src.getWidth(), h = src.getHeight();
        int[] px = new int[w * h];
        src.getPixels(px, 0, w, 0, 0, w, h);
        int[] tmp = new int[w * h];
        for (int pass = 0; pass < 3; pass++) {
            boxPass(px, tmp, w, h, radius);   // rows -> tmp (transposed)
            boxPass(tmp, px, h, w, radius);   // rows -> px  (transposed back)
        }
        Bitmap out = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
        out.setPixels(px, 0, w, 0, 0, w, h);
        return out;
    }

    /** One horizontal box pass, writing transposed so the same code does both axes. */
    private static void boxPass(int[] in, int[] out, int w, int h, int r) {
        for (int y = 0; y < h; y++) {
            int row = y * w;
            int a = 0, rr = 0, gg = 0, bb = 0, n = 0;
            for (int x = -r; x <= r; x++) {
                int c = in[row + clamp(x, 0, w - 1)];
                a += (c >>> 24); rr += (c >> 16) & 0xFF; gg += (c >> 8) & 0xFF; bb += c & 0xFF; n++;
            }
            for (int x = 0; x < w; x++) {
                out[x * h + y] = ((a / n) << 24) | ((rr / n) << 16) | ((gg / n) << 8) | (bb / n);
                int add = in[row + clamp(x + r + 1, 0, w - 1)];
                int sub = in[row + clamp(x - r, 0, w - 1)];
                a += (add >>> 24) - (sub >>> 24);
                rr += ((add >> 16) & 0xFF) - ((sub >> 16) & 0xFF);
                gg += ((add >> 8) & 0xFF) - ((sub >> 8) & 0xFF);
                bb += (add & 0xFF) - (sub & 0xFF);
            }
        }
    }

    private static int clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

    // ----------------------------------------------------------------- draw

    @Override
    protected void onDraw(Canvas canvas) {
        int w = getWidth(), h = getHeight();
        if (w == 0 || h == 0) return;

        if (sharpBitmap == null
                || params.f("backdrop") != lastBackdropKind
                || params.f("blurRadiusPx") != lastBlurRadius) {
            rebuildBackdrop(w, h);
        }

        // The backdrop itself. On the SF path this is everything composited
        // below the blur region; here we draw it so the glass has something to
        // sit over and the refraction is visible against known content.
        canvas.drawBitmap(sharpBitmap, 0, 0, null);

        if (glass == null) return;

        float density = getResources().getDisplayMetrics().density;

        // Three panes shaped like the real first targets: a volume panel, a
        // notification row, and a QS tile.
        drawPane(canvas, new RectF(w * 0.08f, h * 0.10f, w * 0.92f, h * 0.24f), density);
        drawPane(canvas, new RectF(w * 0.08f, h * 0.40f, w * 0.92f, h * 0.52f), density);
        drawPane(canvas, new RectF(w * 0.12f, h * 0.62f, w * 0.52f, h * 0.80f), density);
    }

    private void drawPane(Canvas canvas, RectF rect, float density) {
        float dp = density;
        float radius = params.f("cornerRadiusDp") * dp;
        radius = Math.min(radius, Math.min(rect.width(), rect.height()) * 0.5f);

        // ---- geometry: exactly what the SF caller derives from effectRegion ----
        glass.setFloatUniform("size", rect.width(), rect.height());
        glass.setFloatUniform("offset", -rect.left, -rect.top);
        glass.setFloatUniform("crop", 0f, 0f, (float) getWidth(), (float) getHeight());
        // packed (BL, BR, TR, TL), matching cornerParam's quadrant select
        glass.setFloatUniform("cornerExtents", radius, radius, radius, radius);
        float shape = params.f("cornerShape");
        glass.setFloatUniform("cornerShapes", shape, shape, shape, shape);
        glass.setFloatUniform("unitScale", dp);

        // ---- refraction ----
        glass.setFloatUniform("refractionScale",
                params.f("refractionWidthDp") * dp, params.f("refractionHeightDp") * dp);
        glass.setFloatUniform("refractionAmount", params.f("refractionAmountDp") * dp);
        glass.setFloatUniform("refractionSwirl", params.f("refractionSwirl"));
        glass.setFloatUniform("depthEffect", params.f("refractionDepth"));
        glass.setFloatUniform("profilePower", params.f("curvePower"));
        glass.setFloatUniform("profileBias", params.f("curveBias"));

        // ---- dispersion ----
        glass.setFloatUniform("dispersionHeight", params.f("dispersionHeightDp") * dp);
        glass.setFloatUniform("dispersionAmount", params.f("dispersionAmountDp") * dp);
        glass.setFloatUniform("dispersionTapSpacing", params.f("dispersionTapSpacingPx"));
        glass.setFloatUniform("dispersionQuadrant", params.f("dispersionQuadrant"));

        // ---- frost handover ----
        glass.setFloatUniform("blurMix", params.f("blurMix"));
        glass.setFloatUniform("blurFalloff", params.f("blurFalloff"));
        glass.setFloatUniform("lensBlur", params.f("lensBlur"));

        // ---- tone ----
        glass.setFloatUniform("tintColor",
                params.f("tintR"), params.f("tintG"), params.f("tintB"), params.f("tintA"));
        glass.setFloatUniform("frostColor",
                params.f("frostR"), params.f("frostG"), params.f("frostB"), params.f("frost"));
        glass.setFloatUniform("saturation", params.f("saturation"));
        glass.setFloatUniform("lightIntensity", params.f("light"));
        glass.setFloatUniform("noiseAmount", params.f("noise"));
        glass.setFloatUniform("glassOpacity", params.f("glassOpacity"));

        // ---- highlight ----
        double angle = Math.toRadians(params.f("highlightAngleDeg"));
        glass.setFloatUniform("highlightIntensity", params.f("highlightIntensity"));
        glass.setFloatUniform("highlightDir", (float) Math.cos(angle), (float) Math.sin(angle));
        glass.setFloatUniform("highlightWidth", params.f("highlightWidthDp") * dp);
        glass.setFloatUniform("highlightFalloff", Math.max(0.01f, params.f("highlightFalloff")));

        glass.setFloatUniform("hdrHeadroom", Math.max(1f, params.f("hdrHeadroom")));

        // ---- press (phase 3 on the SF path) ----
        glass.setFloatUniform("touchPos", rect.width() * 0.5f, rect.height() * 0.5f);
        glass.setFloatUniform("touchGlow", params.f("touchGlow"));
        glass.setFloatUniform("touchLens", params.f("touchLens"));

        // ---- the regular-glass blend and tone (identity unless set) ----
        // Uniforms a RuntimeShader is never given read as zero, and a zero
        // toneScale or filterColor would draw the pane black.
        glass.setFloatUniform("softenRadius", params.f("softenDp") * dp);
        // The sharp bitmap is the see-through child here, read at full detail.
        glass.setFloatUniform("softenFast", 0f);
        glass.setFloatUniform("rimFrost", 0f);
        // How far in the edge reaches, as the engine computes it (fillUniforms).
        glass.setFloatUniform("deepBand", Math.max(Math.max(params.f("refractionWidthDp"),
                params.f("refractionHeightDp")), Math.max(params.f("highlightWidthDp"), 7f)) * dp
                + 0.5f);
        glass.setFloatUniform("fillLighten", params.f("fillLighten"));
        glass.setFloatUniform("fillDarken", params.f("fillDarken"));
        glass.setFloatUniform("lumaLimit", params.f("lumaLimit"));
        glass.setFloatUniform("toneScale", params.f("toneScale"));
        glass.setFloatUniform("toneBias", params.f("toneBias"), params.f("toneBias"),
                params.f("toneBias"));
        glass.setFloatUniform("filterColor", 1f, 1f, 1f);

        glass.setInputShader("sharpContent", sharpShader);
        glass.setInputShader("blurredContent", blurredShader);

        panePaint.setShader(glass);
        // drawRect, not drawRoundRect: the SDF owns the shape and its AA.
        canvas.drawRect(rect, panePaint);
    }
}
