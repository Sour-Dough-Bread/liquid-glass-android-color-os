package dev.stockglass.harness;

import android.app.Activity;
import android.app.Dialog;
import android.content.Context;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.BitmapShader;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.LinearGradient;
import android.graphics.Matrix;
import android.graphics.Paint;
import android.graphics.PixelFormat;
import android.graphics.Shader;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.Drawable;
import android.os.Bundle;
import android.util.Log;
import android.view.Choreographer;
import android.view.Gravity;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
import android.widget.FrameLayout;

import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;

/**
 * A fixed workload for measuring what the engine costs per composited frame.
 *
 * The activity draws a backdrop that scrolls by a few pixels every frame, so
 * SurfaceFlinger composes at the display's rate. Over it sits one translucent
 * window carrying the blur regions of a real scene, laid out from the region
 * maps measured on alioth (docs/VERIFIED.md): a full-screen background blur
 * with Quick Settings tiles and notification cards as regions in the same
 * layer, a home screen of icon-sized regions, a keyboard, a volume panel.
 * Nothing in that window changes, so every frame is the same composition and
 * TimeStats' RenderEngine average over a few seconds is a per-frame cost.
 *
 *   adb shell am start -n dev.stockglass.harness/.BenchActivity --es scene shade
 *
 * Extras:
 *   scene            shade | home | keyboard | panel | dialog | custom | none
 *   spec             custom: "left,top,width,height,corner,blur;..." in px
 *   animate          false freezes the backdrop at `phase` (for screenshots)
 *   redraw           true redraws the frozen backdrop every frame: the same
 *                    picture, composed at the display's rate (what the engine
 *                    does differently while frames keep coming can then be
 *                    compared pixel for pixel)
 *   phase            backdrop scroll offset in px when frozen
 *   speed            px per frame while animating (default 6)
 *   backdropRadius   the layer's background blur, px (shade: 77, as alioth)
 *   paneRadius       tiles and icons, px (default 263: the framework's 100 dp)
 *   cardRadius       notification cards, px (default 56, as alioth)
 *   tiles, cards     how many of each in the shade scene (default 5 and 3)
 *   move             true drifts the regions a little every frame
 *   fill             alpha (0-255) of each region's own plate (default 40)
 *
 * The regions are BackgroundBlurDrawables, as SystemUI's and the companion's
 * are: hidden API, so `settings put global hidden_api_policy 1` on an AVD.
 *
 * Every scene asks for at most ten blurs. Android 17 counts the blur requests
 * of layers whose owner is not privileged (CompositionEngine's Output.cpp,
 * kMaxBlurRequests = 10, b/399120953) and past ten draws none of them:
 * SystemUI and the recents launcher are exempt, this app is not. A scene with
 * eleven regions composes at full rate with no blur at all.
 */
public class BenchActivity extends Activity {
    private static final String TAG = "GlassBench";

    private float dp;
    private int width, height;
    private final List<View> panes = new ArrayList<>();
    private final List<int[]> paneSpecs = new ArrayList<>();
    private Dialog overlay;
    private boolean move;
    private int frame;

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        Intent in = getIntent();
        dp = getResources().getDisplayMetrics().density;
        width = getResources().getDisplayMetrics().widthPixels;
        height = getResources().getDisplayMetrics().heightPixels;

        boolean animate = flag(in, "animate", true);
        setContentView(new Backdrop(this, animate, flag(in, "redraw", false), num(in, "phase", 0),
                num(in, "speed", 6)));

        String scene = in.getStringExtra("scene");
        if (scene == null) scene = "shade";
        move = flag(in, "move", false);
        if (!scene.equals("none")) showScene(scene, in);
    }

    @Override
    protected void onDestroy() {
        if (overlay != null) overlay.dismiss();
        super.onDestroy();
    }

    private static boolean flag(Intent in, String key, boolean def) {
        String s = in.getStringExtra(key);
        if (s != null) return s.equals("true") || s.equals("1");
        return in.getBooleanExtra(key, def);
    }

    private static int num(Intent in, String key, int def) {
        String s = in.getStringExtra(key);
        if (s != null) {
            try {
                return Integer.parseInt(s.trim());
            } catch (NumberFormatException e) {
                return def;
            }
        }
        return in.getIntExtra(key, def);
    }

    private int px(float dps) {
        return Math.round(dps * dp);
    }

    /** Queues one region: rect in px, corner radius in px, blur radius in px. */
    private void pane(int left, int top, int w, int h, int corner, int blur) {
        paneSpecs.add(new int[] {left, top, w, h, corner, blur});
    }

    private void showScene(String scene, Intent in) {
        int paneRadius = num(in, "paneRadius", 263);
        int cardRadius = num(in, "cardRadius", 56);
        int backdropRadius = 0;
        boolean blurBehind = false;
        int margin = px(16);
        int gap = px(8);

        switch (scene) {
            case "shade": {
                backdropRadius = num(in, "backdropRadius", 77);
                int tiles = num(in, "tiles", 5);
                int cards = num(in, "cards", 3);
                int y = px(96);
                // The brightness slider: one pill across the panel.
                pane(margin, y, width - 2 * margin, px(48), px(24), paneRadius);
                y += px(48) + gap;
                // Large tiles, two to a row.
                int tw = (width - 2 * margin - gap) / 2;
                int th = px(72);
                for (int i = 0; i < tiles; ++i) {
                    int col = i % 2;
                    pane(margin + col * (tw + gap), y, tw, th, px(24), paneRadius);
                    if (col == 1 || i == tiles - 1) y += th + gap;
                }
                y += px(16);
                // Notification cards, each its own region at the cards' radius.
                for (int i = 0; i < cards; ++i) {
                    pane(margin, y, width - 2 * margin, px(88), px(28), cardRadius);
                    y += px(88) + px(4);
                }
                break;
            }
            case "home": {
                // Themed icons, a widget and the dock's search pill: ten regions.
                int cell = (width - 2 * margin) / 4;
                int icon = px(56);
                int y = px(72);
                pane(margin, y, width - 2 * margin, px(150), px(28), paneRadius);
                y += px(150) + px(24);
                int icons = num(in, "tiles", 8);
                for (int i = 0; i < icons; ++i) {
                    int col = i % 4;
                    pane(margin + col * cell + (cell - icon) / 2, y, icon, icon, icon / 2, paneRadius);
                    if (col == 3) y += px(92);
                }
                pane(margin, height - px(110), width - 2 * margin, px(52), px(26), paneRadius);
                break;
            }
            case "keyboard": {
                // The patched Gboard on alioth: the key area and the strip below it, r=120.
                int kb = num(in, "paneRadius", 120);
                int top = height - px(330);
                pane(0, top, width, px(290), 0, kb);
                pane(0, top + px(290), width, px(40), 0, kb);
                break;
            }
            case "panel": {
                // A volume panel: a rounded cap over a square-topped body, overlapping.
                int pw = px(64);
                int left = width - pw - px(12);
                int top = height / 2 - px(180);
                int panel = num(in, "paneRadius", 80);
                pane(left, top, pw, px(64), pw / 2, panel);
                pane(left, top + px(32), pw, px(300), pw / 2, panel);
                break;
            }
            case "custom": {
                // --es spec "left,top,width,height,corner,blur;..." in px: for
                // finding what a cost scales with (area, edge length, count).
                String spec = in.getStringExtra("spec");
                backdropRadius = num(in, "backdropRadius", 0);
                if (spec != null) {
                    for (String one : spec.split(";")) {
                        String[] f = one.trim().split(",");
                        if (f.length != 6) continue;
                        try {
                            pane(Integer.parseInt(f[0].trim()), Integer.parseInt(f[1].trim()),
                                    Integer.parseInt(f[2].trim()), Integer.parseInt(f[3].trim()),
                                    Integer.parseInt(f[4].trim()), Integer.parseInt(f[5].trim()));
                        } catch (NumberFormatException e) {
                            Log.e(TAG, "bad pane in spec: " + one);
                        }
                    }
                }
                break;
            }
            case "dialog":
                blurBehind = true;
                backdropRadius = num(in, "backdropRadius", 150);
                break;
            default:
                Log.e(TAG, "unknown scene " + scene);
                return;
        }

        Dialog d = new Dialog(this, android.R.style.Theme_Translucent_NoTitleBar);
        d.setCancelable(false);
        FrameLayout root = new FrameLayout(this);
        d.setContentView(root);
        Window w = d.getWindow();
        w.setBackgroundDrawable(new ColorDrawable(Color.argb(0x08, 0, 0, 0)));
        w.setFormat(PixelFormat.TRANSLUCENT);
        w.setDimAmount(0f);
        int flags = num(in, "flags", 0);
        if (flags != 0) w.addFlags(flags);
        if (blurBehind) {
            w.addFlags(WindowManager.LayoutParams.FLAG_BLUR_BEHIND);
            WindowManager.LayoutParams lp = w.getAttributes();
            lp.setBlurBehindRadius(backdropRadius);
            w.setAttributes(lp);
            w.setLayout(width * 4 / 5, height / 3);
            w.setGravity(Gravity.CENTER);
        } else {
            // The backdrop is set on the layer once the window has a surface
            // (below), as SystemUI sets the shade's. Window.setBackgroundBlurRadius
            // would make it one more blur REGION of the window, and RenderEngine
            // draws a layer's regions grouped by radius in whatever order the
            // window's set of drawables lists them: launch to launch the
            // "backdrop" then lands before the panes or over them.
            w.setLayout(WindowManager.LayoutParams.MATCH_PARENT, WindowManager.LayoutParams.MATCH_PARENT);
        }

        final int fill = num(in, "fill", 40);
        for (int[] s : paneSpecs) {
            View v = new View(this);
            FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(s[2], s[3], Gravity.LEFT | Gravity.TOP);
            lp.leftMargin = s[0];
            lp.topMargin = s[1];
            root.addView(v, lp);
            panes.add(v);
        }
        d.show();
        overlay = d;

        final int backdrop = backdropRadius;
        final boolean layerBlur = !blurBehind && backdropRadius > 0;
        final String name = scene;
        // The view root exists only once the window is attached.
        root.post(() -> {
            try {
                Object vri = View.class.getMethod("getViewRootImpl").invoke(root);
                if (layerBlur) {
                    // SurfaceControl.Transaction.setBackgroundBlurRadius, as
                    // SystemUI's BlurUtils.applyBlur does for the shade.
                    Object sc = vri.getClass().getMethod("getSurfaceControl").invoke(vri);
                    android.view.SurfaceControl.Transaction t = new android.view.SurfaceControl.Transaction();
                    android.view.SurfaceControl.Transaction.class
                            .getMethod("setBackgroundBlurRadius", android.view.SurfaceControl.class, int.class)
                            .invoke(t, sc, backdrop);
                    t.apply();
                    t.close();
                }
                Method make = vri.getClass().getMethod("createBackgroundBlurDrawable");
                for (int i = 0; i < panes.size(); ++i) {
                    int[] s = paneSpecs.get(i);
                    Object bbd = make.invoke(vri);
                    bbd.getClass().getMethod("setBlurRadius", int.class).invoke(bbd, s[5]);
                    bbd.getClass().getMethod("setCornerRadius", float.class).invoke(bbd, (float) s[4]);
                    bbd.getClass().getMethod("setColor", int.class)
                            .invoke(bbd, Color.argb(fill, 0xff, 0xff, 0xff));
                    panes.get(i).setBackground((Drawable) bbd);
                }
                Log.i(TAG, "scene " + name + ": backdrop radius " + backdrop + ", " + panes.size()
                        + " regions");
            } catch (Exception e) {
                Log.e(TAG, "BackgroundBlurDrawable unavailable (hidden_api_policy?)", e);
            }
            if (move) drift();
        });
    }

    /** Moves every region a few pixels each frame, as a scrolling list does. */
    private void drift() {
        Choreographer.getInstance().postFrameCallback(new Choreographer.FrameCallback() {
            @Override
            public void doFrame(long nanos) {
                if (isDestroyed()) return;
                ++frame;
                float y = (float) Math.sin(frame * 0.05) * px(24);
                for (View v : panes) v.setTranslationY(y);
                Choreographer.getInstance().postFrameCallback(this);
            }
        });
    }

    /** High-frequency content that scrolls: the compositor has new pixels every frame. */
    private static final class Backdrop extends View {
        private final boolean animate;
        private final boolean redraw;
        private final int speed;
        private int phase;
        private Bitmap bitmap;
        private BitmapShader shader;
        private final Matrix matrix = new Matrix();
        private final Paint paint = new Paint();

        Backdrop(Context c, boolean animate, boolean redraw, int phase, int speed) {
            super(c);
            this.animate = animate;
            this.redraw = redraw;
            this.phase = phase;
            this.speed = speed;
        }

        @Override
        protected void onSizeChanged(int w, int h, int ow, int oh) {
            if (w <= 0 || h <= 0) return;
            bitmap = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
            paint(new Canvas(bitmap), w, h);
            shader = new BitmapShader(bitmap, Shader.TileMode.REPEAT, Shader.TileMode.REPEAT);
            paint.setShader(shader);
        }

        private static void paint(Canvas c, int w, int h) {
            Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
            // The gradient meets itself at the tile's top and bottom, so the
            // scroll has no seam.
            p.setShader(new LinearGradient(0, 0, 0, h,
                    new int[] {0xFF1B2B5C, 0xFF7A2F86, 0xFFD4552B, 0xFF2E7D6B, 0xFF1B2B5C},
                    null, Shader.TileMode.CLAMP));
            c.drawRect(0, 0, w, h, p);
            p.setShader(null);

            p.setColor(Color.WHITE);
            for (int i = 0; i < 36; i++) {
                float y = h * 0.05f + i * (h * 0.012f);
                p.setStrokeWidth(1f + (i % 4));
                c.drawLine(0, y, w, y, p);
            }
            int[] blocks = {0xFFFF3B30, 0xFF34C759, 0xFF007AFF, 0xFFFFCC00, 0xFF5AC8FA, 0xFFAF52DE};
            float bw = w / 6f;
            for (int row = 0; row < 3; row++) {
                for (int i = 0; i < blocks.length; i++) {
                    p.setColor(blocks[(i + row * 2) % blocks.length]);
                    float top = h * (0.52f + row * 0.13f);
                    c.drawRoundRect(i * bw + 8, top, (i + 1) * bw - 8, top + h * 0.07f, 24, 24, p);
                }
            }
            p.setColor(Color.WHITE);
            p.setFakeBoldText(true);
            p.setTextSize(w * 0.075f);
            for (int i = 0; i < 6; i++) {
                c.drawText("STOCK GLASS " + i, w * 0.06f, h * (0.12f + i * 0.16f), p);
            }
            p.setFakeBoldText(false);
            p.setTextSize(w * 0.04f);
            for (int i = 0; i < 12; i++) {
                c.drawText("The quick brown fox jumps over the lazy dog", w * 0.04f,
                        h * (0.17f + i * 0.08f), p);
            }
            p.setColor(0xFF101010);
            int cell = Math.max(6, w / 60);
            for (int y = (int) (h * 0.90f); y < h * 0.98f; y += cell) {
                for (int x = 0; x < w; x += cell) {
                    if (((x / cell) + (y / cell)) % 2 == 0) c.drawRect(x, y, x + cell, y + cell, p);
                }
            }
        }

        @Override
        protected void onDraw(Canvas canvas) {
            if (shader == null) return;
            matrix.setTranslate(0, -phase);
            shader.setLocalMatrix(matrix);
            canvas.drawRect(0, 0, getWidth(), getHeight(), paint);
            if (animate) {
                phase = (phase + speed) % Math.max(1, getHeight());
                postInvalidateOnAnimation();
            } else if (redraw) {
                postInvalidateOnAnimation();
            }
        }
    }
}
