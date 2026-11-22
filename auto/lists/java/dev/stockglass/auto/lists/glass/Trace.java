package dev.stockglass.auto.lists.glass;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Matrix;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.RenderNode;
import android.os.Looper;
import android.util.Log;
import de.robv.android.xposed.XC_MethodHook.MethodHookParam;
import java.io.File;
import java.io.FileWriter;
import java.io.IOException;
import java.util.ArrayList;

/**
 * Draw-order trace (dev loader only): logs the next N recording calls made in
 * this process, with the stack of RenderNodes being recorded, to
 * cache/trace.txt (read it with root: adb shell cat
 * /data/data/com.google.android.projection.gearhead/cache/trace.txt).
 *
 * Each line: depth, op, node, and what was drawn in the coordinates of the
 * node being recorded (the canvas matrix applied). A "node" line is a child
 * RenderNode drawn into the current one, with its place in its parent.
 *
 * The extra hooks it needs (paths, circles, child nodes, render effects, text)
 * are registered on the first "trace" command only, since the dev loader
 * cannot remove a hook and each one is a breakpoint.
 */
final class Trace {
    private static int remaining;
    private static FileWriter out;
    private static int lines;
    private static String onlyThread;

    private Trace() {}

    static boolean armed() {
        return remaining > 0;
    }

    static synchronized String arm(File dir, int n, boolean mainOnly) {
        stop();
        try {
            out = new FileWriter(new File(dir, "trace.txt"));
        } catch (IOException e) {
            return "cannot write: " + e;
        }
        lines = 0;
        onlyThread = mainOnly ? Looper.getMainLooper().getThread().getName() : null;
        remaining = n;
        return "tracing the next " + n + " calls to " + new File(dir, "trace.txt");
    }

    static synchronized void stop() {
        remaining = 0;
        if (out != null) {
            try {
                out.close();
            } catch (IOException ignored) {
            }
            out = null;
            Log.i(CardGlass.TAG, "trace done: " + lines + " lines");
        }
    }

    private static String id(Object o) {
        return Integer.toHexString(System.identityHashCode(o));
    }

    private static String node(RenderNode n) {
        return n == null ? "-" : "#" + id(n) + "(" + n.getWidth() + "x" + n.getHeight() + ")";
    }

    private static String paint(Paint p) {
        if (p == null) return "paint=null";
        StringBuilder sb = new StringBuilder();
        sb.append(String.format("color=%08x", p.getColor()));
        if (p.getStyle() != Paint.Style.FILL) sb.append(" ").append(p.getStyle()).append(" w=").append(p.getStrokeWidth());
        if (p.getShader() != null) sb.append(" shader=").append(p.getShader().getClass().getSimpleName());
        if (p.getColorFilter() != null) sb.append(" cf=").append(p.getColorFilter().getClass().getSimpleName());
        if (p.getBlendMode() != null) sb.append(" blend=").append(p.getBlendMode());
        if (p.getAlpha() != 255) sb.append(" a=").append(p.getAlpha());
        return sb.toString();
    }

    private static String at(Canvas c, float l, float t, float r, float b) {
        Matrix m = new Matrix();
        c.getMatrix(m);
        RectF x = new RectF(l, t, r, b);
        m.mapRect(x);
        return String.format("[%.0f,%.0f %.0fx%.0f]", x.left, x.top, x.width(), x.height());
    }

    private static String bitmap(Bitmap b) {
        return b == null ? "bitmap=null" : "bitmap " + b.getWidth() + "x" + b.getHeight() + " " + b.getConfig() + " @" + id(b);
    }

    static synchronized void on(String key, MethodHookParam p, ArrayList<RenderNode> stack) {
        if (remaining <= 0 || out == null) return;
        if (onlyThread != null && !onlyThread.equals(Thread.currentThread().getName())) return;
        remaining--;
        String s;
        try {
            s = describe(key, p, stack);
        } catch (Throwable t) {
            s = key + " ? " + t;
        }
        if (s == null) return;
        try {
            out.write("  ".repeat(Math.max(0, stack.size() - 1)));
            out.write(s);
            out.write('\n');
            lines++;
        } catch (IOException ignored) {
        }
        if (remaining == 0) stop();
    }

    private static String describe(String key, MethodHookParam p, ArrayList<RenderNode> stack) {
        RenderNode top = stack.isEmpty() ? null : stack.get(stack.size() - 1);
        Object[] a = p.args;
        switch (key) {
            case "begin": {
                RenderNode n = (RenderNode) p.thisObject;
                return "begin " + node(n) + " rec " + a[0] + "x" + a[1] + " [" + Thread.currentThread().getName() + "]";
            }
            case "end":
                return "end " + node((RenderNode) p.thisObject);
            case "bitmap": {
                Canvas c = (Canvas) p.thisObject;
                Rect src = (Rect) a[1], dst = (Rect) a[2];
                return "bitmap " + bitmap((Bitmap) a[0]) + " src=" + (src == null ? "-" : src.toShortString()) + " dst="
                        + (dst == null ? "-" : at(c, dst.left, dst.top, dst.right, dst.bottom)) + " " + paint((Paint) a[3]);
            }
            case "bitmapM": {
                Matrix m = (Matrix) a[1];
                return "bitmapM " + bitmap((Bitmap) a[0]) + " m=" + m + " " + paint((Paint) a[2]);
            }
            case "bitmapXY": {
                Canvas c = (Canvas) p.thisObject;
                Bitmap b = (Bitmap) a[0];
                float x = (float) a[1], y = (float) a[2];
                return "bitmapXY " + bitmap(b) + " at " + at(c, x, y, x + (b == null ? 0 : b.getWidth()), y + (b == null ? 0 : b.getHeight()))
                        + " " + paint((Paint) a[3]);
            }
            case "rect": {
                Canvas c = (Canvas) p.thisObject;
                return "rect " + at(c, (float) a[0], (float) a[1], (float) a[2], (float) a[3]) + " " + paint((Paint) a[4]);
            }
            case "rrect": {
                Canvas c = (Canvas) p.thisObject;
                return "rrect " + at(c, (float) a[0], (float) a[1], (float) a[2], (float) a[3]) + " r=" + a[4] + " "
                        + paint((Paint) a[6]);
            }
            case "path": {
                Canvas c = (Canvas) p.thisObject;
                RectF bb = new RectF();
                ((Path) a[0]).computeBounds(bb, true);
                return "path " + at(c, bb.left, bb.top, bb.right, bb.bottom) + " " + paint((Paint) a[1]);
            }
            case "circle": {
                Canvas c = (Canvas) p.thisObject;
                float cx = (float) a[0], cy = (float) a[1], r = (float) a[2];
                return "circle " + at(c, cx - r, cy - r, cx + r, cy + r) + " " + paint((Paint) a[3]);
            }
            case "text": {
                Canvas c = (Canvas) p.thisObject;
                CharSequence t = (CharSequence) a[0];
                int s = (int) a[1], e = (int) a[2];
                float x = (float) a[5], y = (float) a[6];
                return "text \"" + t.subSequence(s, e) + "\" at " + at(c, x, y, x, y) + " " + paint((Paint) a[8]);
            }
            case "node": {
                RenderNode n = (RenderNode) a[0];
                Canvas c = (Canvas) p.thisObject;
                StringBuilder sb = new StringBuilder("node ").append(node(n)).append(" at ")
                        .append(at(c, n.getLeft(), n.getTop(), n.getRight(), n.getBottom()));
                if (n.getAlpha() != 1f) sb.append(" alpha=").append(n.getAlpha());
                if (n.getScaleX() != 1f || n.getScaleY() != 1f) {
                    sb.append(" scale=").append(n.getScaleX()).append(',').append(n.getScaleY())
                            .append(" pivot=").append(n.getPivotX()).append(',').append(n.getPivotY());
                }
                if (n.getTranslationX() != 0f || n.getTranslationY() != 0f) {
                    sb.append(" tr=").append(n.getTranslationX()).append(',').append(n.getTranslationY());
                }
                if (n.getElevation() != 0f) sb.append(" elev=").append(n.getElevation());
                if (n.getClipToOutline()) sb.append(" clipOutline");
                if (n.getClipToBounds()) sb.append(" clipBounds");
                if (n.hasShadow()) sb.append(" shadow");
                if (n.getUseCompositingLayer()) sb.append(" layer");
                return sb.toString();
            }
            case "effect":
                return "effect on " + node((RenderNode) p.thisObject) + ": " + a[0];
            default:
                return key;
        }
    }
}
