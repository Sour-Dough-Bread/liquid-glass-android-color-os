// Host test for region_merge.h: pieces of one surface still merge, and a pane
// that moves never merges with where it was (the trailing smear).
//
//   g++ -std=c++17 -O2 -Wall -Wextra -I engine/src engine/test/merge_test.cpp -o /tmp/merge_test && /tmp/merge_test
#include "region_merge.h"

#include <cstdio>

using glass::MergeRect;
using glass::MergeShape;
using glass::RegionMerger;

namespace {

int failures = 0;

void check(bool ok, const char* what, int frame) {
    if (!ok) {
        ++failures;
        printf("FAIL %s (frame %d)\n", what, frame);
    }
}

bool same(const MergeRect& a, const MergeRect& b) {
    return a.l == b.l && a.t == b.t && a.r == b.r && a.b == b.b;
}

MergeShape shape(float l, float t, float r, float b, float ul, float ur, float lr, float ll) {
    MergeShape s;
    s.rect = {l, t, r, b};
    const float radii[4] = {ul, ur, lr, ll};
    for (int i = 0; i < 4; ++i) s.rx[i] = s.ry[i] = radii[i];
    return s;
}

struct Result {
    bool any;
    MergeShape shape;
    MergeRect draw;
    int merged;
};

Result draw(RegionMerger& m, const MergeShape& s, uint32_t input, int slot, int64_t ms) {
    Result r{};
    r.any = m.merge(s, input, 100, slot, ms, &r.shape, &r.draw, &r.merged);
    return r;
}

// crDroid's volume panel: a rounded cap over a square-topped body, 68 px of overlap.
const MergeShape kCap = shape(900, 600, 1040, 760, 70, 70, 0, 0);
MergeShape body(float bottom) { return shape(900, 692, 1040, bottom, 0, 0, 70, 70); }

}  // namespace

int main() {
    // 1. The volume panel, standing still. The first frame the cap has not
    //    met the body; from the second on both are one outline, and the body
    //    draws only what the cap did not.
    {
        RegionMerger m;
        uint32_t input = 1000;
        for (int f = 0; f < 6; ++f, ++input) {
            const int64_t ms = 1000 + f * 8;
            const Result cap = draw(m, kCap, input, 0, ms);
            const Result b = draw(m, body(1400), input, 1, ms);
            check(cap.any && same(cap.draw, kCap.rect), "cap draws its own rect", f);
            if (f == 0) {
                check(cap.merged == 0, "cap alone on the first frame", f);
            } else {
                check(cap.merged == 1, "cap merged with last frame's body", f);
                check(same(cap.shape.rect, MergeRect{900, 600, 1040, 1400}), "cap outline is the union", f);
                check(cap.shape.rx[0] == 70 && cap.shape.rx[2] == 70, "union corners from cap and body", f);
            }
            check(b.merged == 1, "body merged with this frame's cap", f);
            check(same(b.draw, MergeRect{900, 760, 1040, 1400}), "body skips what the cap drew", f);
        }
    }

    // 2. A pill moving down 30 px a frame (a heads-up sliding in). Before the
    //    slot rule its positions from the last 120 ms merged into its outline.
    {
        RegionMerger m;
        for (int f = 0; f < 12; ++f) {
            const MergeShape s = shape(100, 500 + 30 * f, 300, 620 + 30 * f, 60, 60, 60, 60);
            const Result r = draw(m, s, 2000 + f, 0, 1000 + f * 8);
            check(r.merged == 0 && same(r.shape.rect, s.rect), "moving pill keeps its own outline", f);
        }
    }

    // 3. A row of five icons on a page swipe, 40 px a frame to the left.
    {
        RegionMerger m;
        for (int f = 0; f < 12; ++f) {
            for (int i = 0; i < 5; ++i) {
                const float l = 60 + 210 * i - 40 * f;
                const MergeShape s = shape(l, 1200, l + 150, 1350, 40, 40, 40, 40);
                const Result r = draw(m, s, 3000 + f, i, 1000 + f * 8);
                check(r.merged == 0 && same(r.shape.rect, s.rect), "swiped icon keeps its own outline", f);
            }
        }
    }

    // 4. Two cards scrolling down 50 px a frame, 8 px apart: the upper one
    //    must not merge with where the lower one was.
    {
        RegionMerger m;
        for (int f = 0; f < 12; ++f) {
            const float y = 300 + 50 * f;
            const MergeShape a = shape(40, y, 1040, y + 200, 48, 48, 48, 48);
            const MergeShape b = shape(40, y + 208, 1040, y + 408, 48, 48, 48, 48);
            const Result ra = draw(m, a, 4000 + f, 0, 1000 + f * 8);
            const Result rb = draw(m, b, 4000 + f, 1, 1000 + f * 8);
            check(ra.merged == 0 && rb.merged == 0, "scrolling cards stay apart", f);
        }
    }

    // 5. The volume panel's body growing 20 px a frame under a cap that stays:
    //    still one outline, and the body never draws under the cap.
    {
        RegionMerger m;
        for (int f = 0; f < 8; ++f) {
            const int64_t ms = 1000 + f * 8;
            const Result cap = draw(m, kCap, 5000 + f, 0, ms);
            const Result b = draw(m, body(1000 + 20 * f), 5000 + f, 1, ms);
            if (f > 0) check(cap.merged >= 1, "cap merged with the growing body", f);
            check(same(cap.draw, kCap.rect), "cap draws its own rect", f);
            check(b.merged >= 1 && b.draw.t == 760, "body starts under the cap", f);
        }
    }

    // 6. The same panel over a busy home screen: 30 panes from another layer
    //    are drawn before it every frame, and the cap still finds the body.
    {
        RegionMerger m;
        for (int f = 0; f < 4; ++f) {
            const int64_t ms = 1000 + f * 8;
            for (int i = 0; i < 30; ++i) {
                const float l = 60 + 200 * (i % 5), t = 300 + 180 * (i / 5);
                draw(m, shape(l, t, l + 150, t + 150, 40, 40, 40, 40), 6000 + 2 * f, i, ms);
            }
            const Result cap = draw(m, kCap, 6001 + 2 * f, 0, ms);
            draw(m, body(1400), 6001 + 2 * f, 1, ms);
            if (f > 0) check(cap.merged == 1, "cap finds the body behind 30 other panes", f);
        }
    }

    // 7. The volume panel sliding in from the right edge inside its window,
    //    cap and body together: drawn as separate panes while moving (no
    //    smear), one outline again once it stops.
    {
        RegionMerger m;
        int f = 0;
        for (; f < 6; ++f) {
            const float dx = 120 - 24 * f;
            const int64_t ms = 1000 + f * 8;
            MergeShape cap = kCap, b = body(1400);
            cap.rect.l += dx; cap.rect.r += dx;
            b.rect.l += dx; b.rect.r += dx;
            const Result rc = draw(m, cap, 7000 + f, 0, ms);
            const Result rb = draw(m, b, 7000 + f, 1, ms);
            check(rc.merged == 0, "sliding cap borrows nothing from earlier frames", f);
            check(rb.merged == 1, "sliding body still meets this frame's cap", f);
        }
        for (; f < 9; ++f) {
            const int64_t ms = 1000 + f * 8;
            const Result rc = draw(m, kCap, 7000 + f, 0, ms);
            draw(m, body(1400), 7000 + f, 1, ms);
            if (f > 6) check(rc.merged == 1, "stopped cap merges again", f);
        }
    }

    printf("%s: merge rules\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
