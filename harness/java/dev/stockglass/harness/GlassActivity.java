package dev.stockglass.harness;

import android.app.Activity;
import android.app.Dialog;
import android.graphics.Color;
import android.graphics.drawable.ColorDrawable;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.util.Log;
import android.view.WindowManager;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;

public class GlassActivity extends Activity {

    private static final String TAG = "GlassHarness";

    /** Which generated tier to load. HIGH = 16 taps, one per pixel. */
    private static final String SHADER_ASSET = "glass_high.sksl";

    private final Params probeParams = new Params();

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        String sksl = readAsset(SHADER_ASSET);
        if (sksl == null) {
            Log.e(TAG, "could not read asset " + SHADER_ASSET);
            finish();
            return;
        }
        setContentView(new GlassView(this, sksl));
        showBlurProbe();
    }

    /**
     * A dialog window that asks SurfaceFlinger for a background blur.
     *
     * This exists because every blur region SystemUI produces is transient — the
     * shade animates its radius 0 -> 20 during expansion and then goes opaque, so
     * a screenshot taken afterwards can never contain one. A dialog holds its
     * blur for as long as it is up, which makes drawBlurRegion reachable from a
     * scripted screenshot.
     *
     * It is also the exact shape M1 needs: the blur samples the activity behind
     * it, which is the harness's own high-frequency backdrop, so the compositor
     * is doing real work over content we control.
     */
    private void showBlurProbe() {
        if (!getWindowManager().isCrossWindowBlurEnabled()) {
            Log.e(TAG, "cross-window blur is DISABLED; no blur regions will be produced");
            return;
        }
        probeParams.reloadIfChanged();
        if (probeParams.f("probePane") == 2f) {
            showShadeProbe();
            return;
        }
        boolean pane = probeParams.f("probePane") != 0f;
        // The window background blur is only honoured when the THEME says the
        // window is translucent (windowIsTranslucent); a translucent pixel
        // format alone produced no region on the A17 AVD. Blur-behind has no
        // such requirement and keeps the original dialog theme.
        Dialog d = new Dialog(this, pane ? android.R.style.Theme_Translucent_NoTitleBar
                                         : android.R.style.Theme_Material_Dialog_NoActionBar);
        d.setCancelable(false);
        View content = new View(this);
        d.setContentView(content);

        android.view.Window w = d.getWindow();
        w.setBackgroundDrawable(new ColorDrawable(Color.argb(0x30, 0, 0, 0)));
        w.setFormat(android.graphics.PixelFormat.TRANSLUCENT);

        // Two different blur types, both requested:
        //
        //  setBackgroundBlurRadius  frosts what shows THROUGH this window, and
        //                           only applies if the window is genuinely
        //                           translucent. A stock dialog theme is not,
        //                           which is why this alone produced no blur
        //                           region at all — dumpsys showed mBlurEnabled
        //                           true with no layer carrying a radius.
        //
        //  FLAG_BLUR_BEHIND +       blurs everything BEHIND the window and has
        //  setBlurBehindRadius      no translucency requirement, so it is the
        //                           reliable one for a scripted test.
        int probeRadius = Math.max(0, Math.round(probeParams.f("probeBlurRadius")));
        if (pane) {
            // A PANE-shaped region: the window's own background blur, which the
            // compositor clips to the background drawable's bounds and corner
            // radius. Unlike blur-behind, this region sits off the display
            // origin and carries real corner radii, which is the shape every
            // system panel and dialog produces — and the only way to exercise a
            // blurRect that does not start at (0, 0).
            float dp = getResources().getDisplayMetrics().density;
            android.graphics.drawable.GradientDrawable bg =
                new android.graphics.drawable.GradientDrawable();
            bg.setColor(Color.argb(0x08, 0, 0, 0));
            bg.setCornerRadius(probeParams.f("probeCornerDp") * dp);
            w.setBackgroundDrawable(bg);
            w.setBackgroundBlurRadius(probeRadius);
        } else {
            w.setBackgroundBlurRadius(probeRadius);
            w.addFlags(WindowManager.LayoutParams.FLAG_BLUR_BEHIND);
            WindowManager.LayoutParams lp = w.getAttributes();
            lp.setBlurBehindRadius(probeRadius);
            w.setAttributes(lp);
        }
        w.setDimAmount(0f);
        w.setGravity(Gravity.CENTER);
        w.setLayout(getResources().getDisplayMetrics().widthPixels * 4 / 5,
                    getResources().getDisplayMetrics().heightPixels / 3);
        d.show();
        Log.i(TAG, "blur probe dialog shown (" + (pane ? "pane" : "blur-behind")
                + "), requested radius " + probeRadius);
    }

    /**
     * probePane=2: a backdrop and panes in ONE layer, which is how the Android 17
     * shade arrives on a real phone (alioth, crDroid): the shade window carries a
     * full-screen background blur, and every notification card is a blur region
     * of that same window. The AVD's SystemUI blurs the shade but not the cards,
     * so this is the only way to reach "pane over backdrop" on an emulator.
     *
     * The cards use BackgroundBlurDrawable, which is what SystemUI uses. It is
     * hidden API, reached by reflection; on an AVD run
     * `adb shell settings put global hidden_api_policy 1` first.
     */
    private void showShadeProbe() {
        int radius = Math.max(0, Math.round(probeParams.f("probeBlurRadius")));
        float dp = getResources().getDisplayMetrics().density;
        Dialog d = new Dialog(this, android.R.style.Theme_Translucent_NoTitleBar);
        d.setCancelable(false);
        android.widget.FrameLayout root = new android.widget.FrameLayout(this);
        d.setContentView(root);
        android.view.Window w = d.getWindow();
        w.setBackgroundDrawable(new ColorDrawable(Color.argb(0x08, 0, 0, 0)));
        w.setFormat(android.graphics.PixelFormat.TRANSLUCENT);
        w.setBackgroundBlurRadius(radius);
        w.setDimAmount(0f);
        w.setLayout(WindowManager.LayoutParams.MATCH_PARENT, WindowManager.LayoutParams.MATCH_PARENT);

        int width = getResources().getDisplayMetrics().widthPixels;
        int height = getResources().getDisplayMetrics().heightPixels;
        final View[] cards = new View[2];
        for (int i = 0; i < cards.length; ++i) {
            View card = new View(this);
            android.widget.FrameLayout.LayoutParams lp = new android.widget.FrameLayout.LayoutParams(
                width * 9 / 10, Math.round(110 * dp), Gravity.CENTER_HORIZONTAL | Gravity.TOP);
            lp.topMargin = Math.round(height * (0.38f + 0.1f * i));
            root.addView(card, lp);
            cards[i] = card;
        }
        d.show();
        // The view root exists only once the window is attached.
        root.post(() -> {
            try {
                Object vri = View.class.getMethod("getViewRootImpl").invoke(root);
                java.lang.reflect.Method make =
                    vri.getClass().getMethod("createBackgroundBlurDrawable");
                for (View card : cards) {
                    Object bbd = make.invoke(vri);
                    bbd.getClass().getMethod("setBlurRadius", int.class).invoke(bbd, radius / 2);
                    bbd.getClass().getMethod("setCornerRadius", float.class)
                        .invoke(bbd, probeParams.f("probeCornerDp") * dp);
                    bbd.getClass().getMethod("setColor", int.class)
                        .invoke(bbd, Color.argb(0x10, 0xff, 0xff, 0xff));
                    card.setBackground((android.graphics.drawable.Drawable) bbd);
                }
                Log.i(TAG, "shade probe shown: backdrop radius " + radius + ", 2 cards at "
                        + (radius / 2));
            } catch (Exception e) {
                Log.e(TAG, "BackgroundBlurDrawable unavailable (hidden_api_policy?)", e);
            }
        });
    }

    private String readAsset(String name) {
        try (InputStream in = getAssets().open(name)) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buf = new byte[8192];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            return out.toString("UTF-8");
        } catch (Exception e) {
            Log.e(TAG, "asset read failed", e);
            return null;
        }
    }
}
