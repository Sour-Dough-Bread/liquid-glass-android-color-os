package dev.stockglass.auto.lists.glass;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Matrix;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Rect;
import android.graphics.RectF;
import android.os.Bundle;
import android.util.Log;
import android.view.Display;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.view.accessibility.AccessibilityNodeInfo;
import android.view.accessibility.AccessibilityNodeProvider;
import android.view.inspector.WindowInspector;
import java.io.File;
import java.io.FileOutputStream;
import java.util.List;

/**
 * Development probes (dev loader only, through Host's CMD broadcast): what
 * windows this process has, the view and accessibility trees of a window, and
 * every draw call a view makes when drawn into a software canvas.
 */
final class Probe {
    static final String TAG = "SGAutoLists";

    private Probe() {}

    static String windows() {
        StringBuilder sb = new StringBuilder();
        for (View root : WindowInspector.getGlobalWindowViews()) {
            Display d = root.getDisplay();
            CharSequence title = root.getLayoutParams() instanceof WindowManager.LayoutParams
                    ? ((WindowManager.LayoutParams) root.getLayoutParams()).getTitle() : "?";
            sb.append(root.getClass().getName()).append(" title=").append(title)
                    .append(" display=").append(d == null ? "-" : d.getDisplayId() + "/" + d.getName())
                    .append(" ").append(root.getWidth()).append('x').append(root.getHeight())
                    .append(" attached=").append(root.isAttachedToWindow()).append('\n');
        }
        log(sb.toString());
        return "windows logged";
    }

    /** The window whose title is {@code title}, else the first whose title contains it. */
    static View window(String title) {
        View near = null;
        for (View root : WindowInspector.getGlobalWindowViews()) {
            if (!(root.getLayoutParams() instanceof WindowManager.LayoutParams)) continue;
            CharSequence t = ((WindowManager.LayoutParams) root.getLayoutParams()).getTitle();
            if (t == null) continue;
            if (title.contentEquals(t)) return root;
            if (near == null && t.toString().contains(title)) near = root;
        }
        return near;
    }

    static String tree(View root) {
        StringBuilder sb = new StringBuilder();
        views(root, 0, sb);
        log(sb.toString());
        return "tree logged";
    }

    private static void views(View v, int depth, StringBuilder sb) {
        int[] loc = new int[2];
        v.getLocationInWindow(loc);
        sb.append("  ".repeat(depth)).append(v.getClass().getName());
        Class<?> c = v.getClass().getSuperclass();
        while (c != null && !c.getName().startsWith("android.")) c = c.getSuperclass();
        if (c != null && c != v.getClass()) sb.append(" (").append(c.getName()).append(')');
        sb.append(" @").append(loc[0]).append(',').append(loc[1]).append(' ')
                .append(v.getWidth()).append('x').append(v.getHeight())
                .append(" vis=").append(v.getVisibility()).append(" layer=").append(v.getLayerType())
                .append(" id=").append(v.getId());
        AccessibilityNodeProvider p = v.getAccessibilityNodeProvider();
        if (p != null) sb.append(" provider=").append(p.getClass().getName());
        sb.append('\n');
        if (p != null) nodes(p, depth + 1, sb);
        if (v instanceof ViewGroup) {
            ViewGroup g = (ViewGroup) v;
            for (int i = 0; i < g.getChildCount(); i++) views(g.getChildAt(i), depth + 1, sb);
        }
    }

    /** Compose's virtual nodes have small ids; ask for each in turn (no accessibility service needed). */
    private static void nodes(AccessibilityNodeProvider p, int depth, StringBuilder sb) {
        Rect r = new Rect();
        int misses = 0;
        for (int id = -1; id < 4000 && misses < 400; id++) {
            AccessibilityNodeInfo n;
            try {
                n = p.createAccessibilityNodeInfo(id);
            } catch (Throwable t) {
                n = null;
            }
            if (n == null) {
                misses++;
                continue;
            }
            misses = 0;
            n.getBoundsInScreen(r);
            sb.append("  ".repeat(depth)).append("#").append(id).append(' ').append(n.getClassName())
                    .append(" ").append(r.toShortString());
            if (n.getText() != null) sb.append(" text=\"").append(n.getText()).append('"');
            if (n.getContentDescription() != null) sb.append(" desc=\"").append(n.getContentDescription()).append('"');
            if (n.isClickable()) sb.append(" clickable");
            if (n.getViewIdResourceName() != null) sb.append(" res=").append(n.getViewIdResourceName());
            Bundle e = n.getExtras();
            if (e != null && !e.isEmpty()) sb.append(" extras=").append(e.keySet());
            sb.append(" actions=");
            for (AccessibilityNodeInfo.AccessibilityAction a : n.getActionList()) {
                sb.append(a.getId()).append(a.getLabel() != null ? "(" + a.getLabel() + ")" : "").append(',');
            }
            sb.append('\n');
        }
    }

    /** Draws {@code v} into a logging software canvas; saves the picture as PNG in the cache dir. */
    static String draws(View v, File dir) {
        Bitmap b = Bitmap.createBitmap(Math.max(1, v.getWidth()), Math.max(1, v.getHeight()), Bitmap.Config.ARGB_8888);
        LogCanvas c = new LogCanvas(b);
        v.draw(c);
        try (java.io.FileWriter w = new java.io.FileWriter(new File(dir, "probe-draw.txt"))) {
            w.write(c.sb.toString());
        } catch (Throwable t) {
            Log.e(TAG, "txt", t);
        }
        File f = new File(dir, "probe-draw.png");
        try (FileOutputStream o = new FileOutputStream(f)) {
            b.compress(Bitmap.CompressFormat.PNG, 100, o);
        } catch (Throwable t) {
            Log.e(TAG, "png", t);
        }
        return "draws logged, " + f;
    }

    static void log(String s) {
        for (String line : s.split("\n")) Log.i(TAG, line);
    }

    /** Logs each draw with its device-space bounds. */
    static final class LogCanvas extends Canvas {
        final StringBuilder sb = new StringBuilder();
        private final Matrix m = new Matrix();
        private final RectF tmp = new RectF();

        LogCanvas(Bitmap b) {
            super(b);
        }

        private String at(float l, float t, float r, float bt) {
            getMatrix(m);
            tmp.set(l, t, r, bt);
            m.mapRect(tmp);
            return String.format("[%.0f,%.0f %.0fx%.0f]", tmp.left, tmp.top, tmp.width(), tmp.height());
        }

        private static String p(Paint p) {
            if (p == null) return "paint=null";
            return String.format("color=%08x style=%s shader=%s cf=%s blend=%s",
                    p.getColor(), p.getStyle(),
                    p.getShader() == null ? "-" : p.getShader().getClass().getSimpleName(),
                    p.getColorFilter() == null ? "-" : p.getColorFilter().getClass().getSimpleName(),
                    p.getBlendMode());
        }

        private static String bm(Bitmap b) {
            return "bitmap " + b.getWidth() + "x" + b.getHeight() + " " + b.getConfig() + " #" + System.identityHashCode(b);
        }

        private void add(String s) {
            sb.append("  ".repeat(Math.max(0, getSaveCount() - 1))).append(s).append('\n');
        }

        @Override public void drawBitmap(Bitmap b, Rect src, Rect dst, Paint p) {
            add("drawBitmap " + bm(b) + " src=" + (src == null ? "-" : src.toShortString()) + " dst=" + at(dst.left, dst.top, dst.right, dst.bottom) + " " + p(p));
            super.drawBitmap(b, src, dst, p);
        }
        @Override public void drawBitmap(Bitmap b, Rect src, RectF dst, Paint p) {
            add("drawBitmapF " + bm(b) + " src=" + (src == null ? "-" : src.toShortString()) + " dst=" + at(dst.left, dst.top, dst.right, dst.bottom) + " " + p(p));
            super.drawBitmap(b, src, dst, p);
        }
        @Override public void drawBitmap(Bitmap b, float x, float y, Paint p) {
            add("drawBitmapXY " + bm(b) + " at " + at(x, y, x + b.getWidth(), y + b.getHeight()) + " " + p(p));
            super.drawBitmap(b, x, y, p);
        }
        @Override public void drawBitmap(Bitmap b, Matrix mx, Paint p) {
            add("drawBitmapM " + bm(b) + " " + mx + " " + p(p));
            super.drawBitmap(b, mx, p);
        }
        @Override public void drawRect(RectF r, Paint p) {
            add("drawRect " + at(r.left, r.top, r.right, r.bottom) + " " + p(p));
            super.drawRect(r, p);
        }
        @Override public void drawRect(float l, float t, float r, float b, Paint p) {
            add("drawRect " + at(l, t, r, b) + " " + p(p));
            super.drawRect(l, t, r, b, p);
        }
        @Override public void drawRoundRect(RectF r, float rx, float ry, Paint p) {
            add("drawRoundRect " + at(r.left, r.top, r.right, r.bottom) + " r=" + rx + " " + p(p));
            super.drawRoundRect(r, rx, ry, p);
        }
        @Override public void drawRoundRect(float l, float t, float r, float b, float rx, float ry, Paint p) {
            add("drawRoundRect " + at(l, t, r, b) + " r=" + rx + " " + p(p));
            super.drawRoundRect(l, t, r, b, rx, ry, p);
        }
        @Override public void drawPath(Path path, Paint p) {
            RectF bb = new RectF();
            path.computeBounds(bb, true);
            add("drawPath " + at(bb.left, bb.top, bb.right, bb.bottom) + " " + p(p));
            super.drawPath(path, p);
        }
        @Override public void drawCircle(float cx, float cy, float r, Paint p) {
            add("drawCircle " + at(cx - r, cy - r, cx + r, cy + r) + " " + p(p));
            super.drawCircle(cx, cy, r, p);
        }
        @Override public void drawOval(float l, float t, float r, float b, Paint p) {
            add("drawOval " + at(l, t, r, b) + " " + p(p));
            super.drawOval(l, t, r, b, p);
        }
        @Override public void drawLine(float x0, float y0, float x1, float y1, Paint p) {
            add("drawLine " + at(Math.min(x0, x1), Math.min(y0, y1), Math.max(x0, x1), Math.max(y0, y1)) + " w=" + p.getStrokeWidth() + " cap=" + p.getStrokeCap() + " " + p(p));
            super.drawLine(x0, y0, x1, y1, p);
        }
        @Override public void drawPaint(Paint p) {
            add("drawPaint " + p(p));
            super.drawPaint(p);
        }
        @Override public void drawColor(int color) {
            add(String.format("drawColor %08x", color));
            super.drawColor(color);
        }
        @Override public void drawText(char[] t, int i, int n, float x, float y, Paint p) {
            add("drawText \"" + new String(t, i, n) + "\" at " + at(x, y, x, y) + " " + p(p));
            super.drawText(t, i, n, x, y, p);
        }
        @Override public void drawText(String t, float x, float y, Paint p) {
            add("drawText \"" + t + "\" at " + at(x, y, x, y) + " " + p(p));
            super.drawText(t, x, y, p);
        }
        @Override public void drawText(CharSequence t, int s, int e, float x, float y, Paint p) {
            add("drawText \"" + t.subSequence(s, e) + "\" at " + at(x, y, x, y) + " " + p(p));
            super.drawText(t, s, e, x, y, p);
        }
        @Override public void drawTextRun(char[] t, int i, int n, int ci, int cn, float x, float y, boolean rtl, Paint p) {
            add("drawTextRun \"" + new String(t, i, n) + "\" at " + at(x, y, x, y) + " " + p(p));
            super.drawTextRun(t, i, n, ci, cn, x, y, rtl, p);
        }
        @Override public void drawTextRun(CharSequence t, int s, int e, int cs, int ce, float x, float y, boolean rtl, Paint p) {
            add("drawTextRun \"" + t.subSequence(s, e) + "\" at " + at(x, y, x, y) + " " + p(p));
            super.drawTextRun(t, s, e, cs, ce, x, y, rtl, p);
        }
        @Override public int saveLayer(RectF b, Paint p) {
            add("saveLayer " + (b == null ? "-" : at(b.left, b.top, b.right, b.bottom)) + " " + p(p));
            return super.saveLayer(b, p);
        }
        @Override public int saveLayer(float l, float t, float r, float b, Paint p) {
            add("saveLayer " + at(l, t, r, b) + " " + p(p));
            return super.saveLayer(l, t, r, b, p);
        }
        @Override public int saveLayerAlpha(RectF b, int a) {
            add("saveLayerAlpha " + a);
            return super.saveLayerAlpha(b, a);
        }
        @Override public int saveLayerAlpha(float l, float t, float r, float b, int a) {
            add("saveLayerAlpha " + at(l, t, r, b) + " " + a);
            return super.saveLayerAlpha(l, t, r, b, a);
        }
        @Override public boolean clipRect(float l, float t, float r, float b) {
            add("clipRect " + at(l, t, r, b));
            return super.clipRect(l, t, r, b);
        }
        @Override public boolean clipPath(Path path) {
            RectF bb = new RectF();
            path.computeBounds(bb, true);
            add("clipPath " + at(bb.left, bb.top, bb.right, bb.bottom));
            return super.clipPath(path);
        }
        @Override public boolean clipOutPath(Path path) {
            add("clipOutPath");
            return super.clipOutPath(path);
        }
    }

    static String list(List<?> l) {
        return String.valueOf(l);
    }
}
