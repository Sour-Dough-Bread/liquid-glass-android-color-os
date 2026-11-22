package dev.stockglass.auto.maps;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Outline;
import android.graphics.Rect;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewOutlineProvider;
import android.widget.TextView;
import java.io.File;
import java.io.FileWriter;
import java.util.WeakHashMap;

/**
 * Debug: writes a car window's visible views to Maps' cache dir, one line per
 * view (class, id, bounds in the window, background, outline, elevation, text),
 * so a control that is not an androidx CardView can be found on a phone, where
 * the window manager's view server is not available. Asked for by a new value
 * of persist.sys.glass.auto.dump; read it with
 *   su -c cat /data/data/com.google.android.apps.maps/cache/stockglass-tree-<display>.txt
 */
final class TreeDump {
    private static final WeakHashMap<View, String> done = new WeakHashMap<>();

    private TreeDump() {}

    static void maybe(View root) {
        String token = Gate.dump();
        if (token.isEmpty() || token.equals(done.get(root))) return;
        done.put(root, token);
        StringBuilder b = new StringBuilder();
        int display = root.getDisplay() == null ? -1 : root.getDisplay().getDisplayId();
        b.append("# car window ").append(root.getWidth()).append('x').append(root.getHeight())
                .append(" on display ").append(display)
                .append(root.getDisplay() == null ? "" : " \"" + root.getDisplay().getName() + "\"")
                .append(", density ").append(root.getResources().getDisplayMetrics().densityDpi)
                .append(" dpi, dump ").append(token).append('\n');
        int[] count = {0};
        walk(root, 0, b, count);
        try {
            File dir = root.getContext().getApplicationContext().getCacheDir();
            File f = new File(dir, "stockglass-tree-" + display + ".txt");
            try (FileWriter w = new FileWriter(f)) {
                w.write(b.toString());
            }
            L.i("dump " + token + ": " + count[0] + " views of the car window on display " + display + " in " + f);
        } catch (Throwable t) {
            L.e("cannot write the dump", t);
        }
    }

    private static void walk(View v, int depth, StringBuilder b, int[] count) {
        if (v.getVisibility() != View.VISIBLE || v.getWidth() <= 0 || v.getHeight() <= 0) return;
        ++count[0];
        for (int i = 0; i < depth; ++i) b.append("  ");
        b.append(name(v));
        if (v.getId() != View.NO_ID) {
            String id;
            try {
                id = v.getResources().getResourceEntryName(v.getId());
            } catch (Throwable t) {
                id = "0x" + Integer.toHexString(v.getId());
            }
            b.append(" id/").append(id);
        }
        int[] at = new int[2];
        v.getLocationInWindow(at);
        b.append(" [").append(at[0]).append(',').append(at[1]).append(' ')
                .append(v.getWidth()).append('x').append(v.getHeight()).append(']');
        if (v.getAlpha() < 1f) b.append(" alpha=").append(v.getAlpha());
        if (v.getElevation() != 0f || v.getTranslationZ() != 0f) {
            b.append(" elev=").append(v.getElevation() + v.getTranslationZ());
        }
        if (!v.willNotDraw()) b.append(" draws");
        if (PlateGlass.has(v)) b.append(" GLASS");
        Drawable bg = v.getBackground();
        if (bg != null) b.append(" bg=").append(drawable(bg, v));
        Drawable fg = v.getForeground();
        if (fg != null) b.append(" fg=").append(fg.getClass().getName()).append(" centreAlpha=")
                .append(centreAlpha(fg, v.getWidth(), v.getHeight()));
        ViewOutlineProvider op = v.getOutlineProvider();
        if (op != null && (bg != null || v.getClipToOutline() || v.getElevation() != 0f)) {
            b.append(" outline=").append(provider(op));
            try {
                Outline o = new Outline();
                op.getOutline(v, o);
                Rect r = new Rect();
                if (o.getRect(r)) b.append(' ').append(r.toShortString()).append(" r=").append(o.getRadius());
                else if (!o.isEmpty()) b.append(" (path)");
                else b.append(" (empty)");
            } catch (Throwable t) {
                b.append(" (").append(t.getClass().getSimpleName()).append(')');
            }
            if (v.getClipToOutline()) b.append(" clip");
        }
        if (v instanceof TextView) {
            CharSequence t = ((TextView) v).getText();
            if (t != null && t.length() > 0) {
                b.append(" text=\"").append(t.length() > 40 ? t.subSequence(0, 40) + "..." : t).append('"');
            }
        }
        CharSequence cd = v.getContentDescription();
        if (cd != null && cd.length() > 0) b.append(" cd=\"").append(cd).append('"');
        b.append('\n');
        if (v instanceof ViewGroup) {
            ViewGroup g = (ViewGroup) v;
            for (int i = 0; i < g.getChildCount(); ++i) walk(g.getChildAt(i), depth + 1, b, count);
        }
    }

    /** The class, and for an obfuscated one the first framework or library class it extends. */
    private static String name(View v) {
        Class<?> c = v.getClass();
        String n = c.getName();
        if (n.indexOf('.') > 0 && !n.contains("$")) return shorten(n);
        for (Class<?> s = c.getSuperclass(); s != null; s = s.getSuperclass()) {
            String sn = s.getName();
            if (sn.startsWith("android.") || sn.startsWith("androidx.") || sn.startsWith("com.google.android.material.")) {
                return n + "<" + shorten(sn);
            }
        }
        return n;
    }

    private static String shorten(String n) {
        return n.replace("android.widget.", "").replace("android.view.", "")
                .replace("androidx.cardview.widget.", "").replace("android.support.v7.widget.", "v7.");
    }

    private static String provider(ViewOutlineProvider op) {
        if (op == ViewOutlineProvider.BACKGROUND) return "BACKGROUND";
        if (op == ViewOutlineProvider.BOUNDS) return "BOUNDS";
        if (op == ViewOutlineProvider.PADDED_BOUNDS) return "PADDED_BOUNDS";
        return op.getClass().getName();
    }

    private static String drawable(Drawable d, View v) {
        StringBuilder b = new StringBuilder(d.getClass().getName());
        for (Class<?> s = d.getClass().getSuperclass(); s != null && s != Drawable.class; s = s.getSuperclass()) {
            if (!s.getName().startsWith("android.graphics.drawable.")) continue;
            b.append('<').append(s.getSimpleName());
            break;
        }
        b.append(" a=").append(d.getAlpha());
        if (d instanceof ColorDrawable) {
            b.append(" #").append(Integer.toHexString(((ColorDrawable) d).getColor()));
        } else if (d instanceof GradientDrawable) {
            GradientDrawable g = (GradientDrawable) d;
            b.append(" shape=").append(g.getShape());
            if (g.getColor() != null) b.append(" #").append(Integer.toHexString(g.getColor().getDefaultColor()));
            try {
                b.append(" corner=").append(g.getCornerRadius());
            } catch (Throwable ignored) {
            }
        }
        b.append(" centreAlpha=").append(centreAlpha(d, v.getWidth(), v.getHeight()));
        if (d instanceof android.graphics.drawable.LayerDrawable) {
            android.graphics.drawable.LayerDrawable l = (android.graphics.drawable.LayerDrawable) d;
            b.append(" layers=[");
            for (int i = 0; i < l.getNumberOfLayers(); ++i) {
                Drawable c = l.getDrawable(i);
                if (i > 0) b.append(", ");
                if (c == null) {
                    b.append("null");
                    continue;
                }
                b.append(c.getClass().getName()).append(" a=").append(c.getAlpha())
                        .append(" centreAlpha=").append(centreAlpha(c, v.getWidth(), v.getHeight()));
            }
            b.append(']');
        }
        return b.toString();
    }

    private static int centreAlpha(Drawable d, int w, int h) {
        Rect old = d.copyBounds();
        try {
            Bitmap bm = Bitmap.createBitmap(9, 9, Bitmap.Config.ARGB_8888);
            Canvas c = new Canvas(bm);
            c.scale(9f / w, 9f / h);
            if (old.width() != w || old.height() != h) d.setBounds(0, 0, w, h);
            d.draw(c);
            int a = Color.alpha(bm.getPixel(4, 4));
            bm.recycle();
            return a;
        } catch (Throwable t) {
            return -1;
        } finally {
            if (!d.getBounds().equals(old)) d.setBounds(old);
        }
    }
}
