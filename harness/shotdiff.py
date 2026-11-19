#!/usr/bin/env python3
"""shotdiff.py <a.png> <b.png> [out.png]: how two screenshots differ.

Prints the share of pixels that differ at all, by more than 2 and by more than
8 levels (of 255, the largest channel difference), the mean and the largest
difference, and where the differing pixels are. With a third argument it
writes the difference, amplified 8x, for looking at.
"""
import sys
from PIL import Image, ImageChops


def main():
    a = Image.open(sys.argv[1]).convert("RGB")
    b = Image.open(sys.argv[2]).convert("RGB")
    if a.size != b.size:
        print("sizes differ: %s vs %s" % (a.size, b.size))
        return 1
    d = ImageChops.difference(a, b)
    # Per pixel, the largest of the three channel differences.
    r, g, bl = d.split()
    m = ImageChops.lighter(ImageChops.lighter(r, g), bl)
    hist = m.histogram()
    n = a.size[0] * a.size[1]
    any_ = n - hist[0]
    gt2 = sum(hist[3:])
    gt8 = sum(hist[9:])
    mean = sum(i * c for i, c in enumerate(hist)) / n
    top = max(i for i, c in enumerate(hist) if c)
    box = m.point(lambda v: 255 if v > 2 else 0).getbbox()
    print("differ: %.3f%%  >2: %.3f%%  >8: %.3f%%  mean %.3f  max %d  where(>2): %s" % (
        100.0 * any_ / n, 100.0 * gt2 / n, 100.0 * gt8 / n, mean, top, box))
    if len(sys.argv) > 3:
        m.point(lambda v: min(255, v * 8)).save(sys.argv[3])
    return 0


if __name__ == "__main__":
    sys.exit(main())
