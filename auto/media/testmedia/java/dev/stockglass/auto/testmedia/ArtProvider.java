package dev.stockglass.auto.testmedia;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import java.io.File;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.List;

/**
 * Artwork for the browse tree, as PNGs behind content:// URIs (what Android
 * Auto asks media apps to use for item icons):
 *
 *   content://dev.stockglass.auto.testmedia.art/art/<seed>   colourful square artwork
 *   content://dev.stockglass.auto.testmedia.art/cat/<seed>   a white glyph on clear (category icons)
 *
 * Made once per seed into the cache directory. Exported with no permission:
 * this is a test app with nothing private in it.
 */
public final class ArtProvider extends ContentProvider {
    static final String AUTHORITY = "dev.stockglass.auto.testmedia.art";
    private static final int ART = 320, CAT = 128;

    @Override public boolean onCreate() { return true; }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        List<String> seg = uri.getPathSegments();
        if (seg.size() != 2) throw new FileNotFoundException(uri.toString());
        String kind = seg.get(0);
        int seed;
        try {
            seed = Integer.parseInt(seg.get(1));
        } catch (NumberFormatException e) {
            throw new FileNotFoundException(uri.toString());
        }
        File f = new File(getContext().getCacheDir(), kind + "-" + seed + ".png");
        synchronized (ArtProvider.class) {
            if (!f.exists()) {
                Bitmap b = "cat".equals(kind) ? glyph(seed) : Player.artwork(palette(seed), seed, ART);
                File tmp = new File(f.getPath() + ".tmp");
                try (FileOutputStream o = new FileOutputStream(tmp)) {
                    b.compress(Bitmap.CompressFormat.PNG, 100, o);
                } catch (IOException e) {
                    throw new FileNotFoundException(e.toString());
                }
                if (!tmp.renameTo(f)) throw new FileNotFoundException("rename " + f);
            }
        }
        return ParcelFileDescriptor.open(f, ParcelFileDescriptor.MODE_READ_ONLY);
    }

    /** Three saturated colours from a seed, a third of the hue circle apart with a twist. */
    static int[] palette(int seed) {
        float h = (seed * 47) % 360;
        return new int[] {
            Color.HSVToColor(new float[] {h, 0.85f, 1f}),
            Color.HSVToColor(new float[] {(h + 110) % 360, 0.9f, 0.95f}),
            Color.HSVToColor(new float[] {(h + 220) % 360, 0.8f, 1f}),
        };
    }

    /** A simple white shape on clear: circle, square, triangle, diamond, by seed. */
    private static Bitmap glyph(int seed) {
        Bitmap b = Bitmap.createBitmap(CAT, CAT, Bitmap.Config.ARGB_8888);
        Canvas c = new Canvas(b);
        Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        p.setColor(Color.WHITE);
        float m = CAT * 0.18f, s = CAT - 2 * m;
        Path path = new Path();
        switch (seed % 4) {
            case 0:
                c.drawCircle(CAT / 2f, CAT / 2f, s / 2f, p);
                break;
            case 1:
                c.drawRoundRect(m, m, m + s, m + s, s * 0.2f, s * 0.2f, p);
                break;
            case 2:
                path.moveTo(CAT / 2f, m);
                path.lineTo(m + s, m + s);
                path.lineTo(m, m + s);
                path.close();
                c.drawPath(path, p);
                break;
            default:
                path.moveTo(CAT / 2f, m);
                path.lineTo(m + s, CAT / 2f);
                path.lineTo(CAT / 2f, m + s);
                path.lineTo(m, CAT / 2f);
                path.close();
                c.drawPath(path, p);
                break;
        }
        return b;
    }

    @Override public String getType(Uri uri) { return "image/png"; }
    @Override public Cursor query(Uri u, String[] p, String s, String[] a, String o) { return null; }
    @Override public Uri insert(Uri u, ContentValues v) { return null; }
    @Override public int delete(Uri u, String s, String[] a) { return 0; }
    @Override public int update(Uri u, ContentValues v, String s, String[] a) { return 0; }
}
