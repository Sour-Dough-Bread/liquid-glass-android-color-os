package dev.stockglass.auto.media.card;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;

/**
 * What the glass refracts: the card's artwork, with as much of the card's
 * scrim over it as {@code through} says (1: the artwork exactly as the card
 * shows it around the button; 0: the artwork bare, so the glass is a window
 * through the scrim), and a blurred copy at a quarter of its size, which is
 * what the engine's Kawase pass gives its shader.
 *
 * Made once per artwork (and scrim), off the UI thread; drawing the glass
 * then only samples these two bitmaps.
 */
final class Backdrop {
    final Bitmap sharp, blurred;
    final float blurScale;
    final Bitmap source;
    final int scrim;
    final float through;

    private Backdrop(Bitmap source, int scrim, float through, Bitmap sharp, Bitmap blurred, float blurScale) {
        this.source = source;
        this.scrim = scrim;
        this.through = through;
        this.sharp = sharp;
        this.blurred = blurred;
        this.blurScale = blurScale;
    }

    boolean matches(Bitmap art, int scrimColor, float scrimThrough) {
        return source == art && scrim == scrimColor && through == scrimThrough;
    }

    /** blurPx: the blur's box radius in artwork pixels (three passes, roughly a Gaussian of 1.4 times that). */
    static Backdrop make(Bitmap art, int scrimColor, float scrimThrough, float blurPx) {
        Bitmap sharp = art.copy(Bitmap.Config.ARGB_8888, true); // also reads back a HARDWARE bitmap
        if (Color.alpha(scrimColor) > 0 && scrimThrough > 0f) {
            int a = Math.round(Color.alpha(scrimColor) * Math.min(1f, scrimThrough));
            new Canvas(sharp).drawColor(Color.argb(a, Color.red(scrimColor), Color.green(scrimColor), Color.blue(scrimColor)));
        }
        sharp.prepareToDraw();
        float scale = 0.25f;
        int bw = Math.max(1, Math.round(sharp.getWidth() * scale));
        int bh = Math.max(1, Math.round(sharp.getHeight() * scale));
        Bitmap small = Bitmap.createScaledBitmap(sharp, bw, bh, true);
        if (small == sharp) small = sharp.copy(Bitmap.Config.ARGB_8888, true);
        int r = Math.max(1, Math.round(blurPx * scale));
        int[] px = new int[bw * bh];
        small.getPixels(px, 0, bw, 0, 0, bw, bh);
        for (int pass = 0; pass < 3; pass++) {
            boxH(px, bw, bh, r);
            boxV(px, bw, bh, r);
        }
        Bitmap blurred = Bitmap.createBitmap(bw, bh, Bitmap.Config.ARGB_8888);
        blurred.setPixels(px, 0, bw, 0, 0, bw, bh);
        blurred.prepareToDraw();
        return new Backdrop(art, scrimColor, scrimThrough, sharp, blurred, scale);
    }

    private static void boxH(int[] px, int w, int h, int r) {
        int[] row = new int[w];
        int n = 2 * r + 1;
        for (int y = 0; y < h; y++) {
            int o = y * w;
            System.arraycopy(px, o, row, 0, w);
            int sa = 0, sr = 0, sg = 0, sb = 0;
            for (int i = -r; i <= r; i++) {
                int c = row[Math.max(0, Math.min(w - 1, i))];
                sa += c >>> 24; sr += (c >> 16) & 255; sg += (c >> 8) & 255; sb += c & 255;
            }
            for (int x = 0; x < w; x++) {
                px[o + x] = ((sa / n) << 24) | ((sr / n) << 16) | ((sg / n) << 8) | (sb / n);
                int out = row[Math.max(0, x - r)], in = row[Math.min(w - 1, x + r + 1)];
                sa += (in >>> 24) - (out >>> 24);
                sr += ((in >> 16) & 255) - ((out >> 16) & 255);
                sg += ((in >> 8) & 255) - ((out >> 8) & 255);
                sb += (in & 255) - (out & 255);
            }
        }
    }

    private static void boxV(int[] px, int w, int h, int r) {
        int[] col = new int[h];
        int n = 2 * r + 1;
        for (int x = 0; x < w; x++) {
            for (int y = 0; y < h; y++) col[y] = px[y * w + x];
            int sa = 0, sr = 0, sg = 0, sb = 0;
            for (int i = -r; i <= r; i++) {
                int c = col[Math.max(0, Math.min(h - 1, i))];
                sa += c >>> 24; sr += (c >> 16) & 255; sg += (c >> 8) & 255; sb += c & 255;
            }
            for (int y = 0; y < h; y++) {
                px[y * w + x] = ((sa / n) << 24) | ((sr / n) << 16) | ((sg / n) << 8) | (sb / n);
                int out = col[Math.max(0, y - r)], in = col[Math.min(h - 1, y + r + 1)];
                sa += (in >>> 24) - (out >>> 24);
                sr += ((in >> 16) & 255) - ((out >> 16) & 255);
                sg += ((in >> 8) & 255) - ((out >> 8) & 255);
                sb += (in & 255) - (out & 255);
            }
        }
    }
}
