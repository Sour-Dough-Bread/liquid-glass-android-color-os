// One pane from overlapping regions.
//
// A window may describe one surface as several blur regions. crDroid's volume
// panel is two in one layer: a rounded cap and a square-cornered body that
// overlap by ~68 px. Stock blurs both with the same image, so the join is
// invisible; glass gives each its own edge, and the body's top edge refracts
// right across the panel under the cap.
//
// So regions that overlap along one axis and share the other axis's extent
// exactly (within a pixel), at the same blur radius, are drawn as ONE outline:
// the union, its corners taken from the regions at either end. drawBlurRegion
// sees one region at a time, so the others come from a short memory of what
// was drawn, in layer-local coordinates, which stay put while the layer itself
// slides. Each region still draws only its own rect, less any part a neighbour
// already drew this frame, so no pixel is drawn twice and none is left out.
//
// Which remembered regions count:
//
// - Any drawn earlier in this frame (the same snapshot, so the same uniqueID).
// - From an earlier frame (the last ~120 ms), only regions that come AFTER
//   this one in the layer's draw order, so have not been drawn yet this
//   frame. That is what the memory is for: the cap is drawn before the body
//   and would not know about it otherwise.
// - And only while this region stands where it stood in an earlier frame.
//
// A region's place in the draw order (its slot) is what identifies it from
// frame to frame. Without that, a pane moving straight along one axis (cards
// scrolling, a heads-up sliding in, icons on a page swipe, tiles as Quick
// Settings expands) met its own positions from the last 120 ms. They share its
// cross extent and overlap it, so they were merged as neighbours. Its outline
// then ran back along its path, and the end it moved away from lost its
// corners and rim: a squared-off smear that trailed every slide. A moving pane
// now borrows nothing from earlier frames. Neighbours drawn this frame still
// merge with it, and the first frame a surface appears the region drawn first
// draws alone, as before.
//
// Header-only and free of Skia so that test/merge_test.cpp runs it on the host.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace glass {

struct MergeRect {
    float l = 0.0f, t = 0.0f, r = 0.0f, b = 0.0f;
};

// A round rect as drawBlurRegion hands it over: the rect, and each corner's x
// and y radius in SkRRect's corner order (upper-left, upper-right, lower-right,
// lower-left).
struct MergeShape {
    MergeRect rect;
    float rx[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float ry[4] = {0.0f, 0.0f, 0.0f, 0.0f};
};

namespace merge_detail {

inline bool within1(float a, float b) { return std::fabs(a - b) <= 1.0f; }

// Along `v` (vertical, else horizontal): start and end on that axis.
inline float lo(const MergeRect& r, bool v) { return v ? r.t : r.l; }
inline float hi(const MergeRect& r, bool v) { return v ? r.b : r.r; }

inline bool sameCross(const MergeRect& a, const MergeRect& b, bool v) {
    return v ? within1(a.l, b.l) && within1(a.r, b.r) : within1(a.t, b.t) && within1(a.b, b.b);
}
inline bool overlapsAlong(const MergeRect& a, const MergeRect& b, bool v) {
    return std::min(hi(a, v), hi(b, v)) - std::max(lo(a, v), lo(b, v)) > 1.0f;
}
inline bool sameRect(const MergeRect& a, const MergeRect& b) {
    return within1(a.l, b.l) && within1(a.t, b.t) && within1(a.r, b.r) && within1(a.b, b.b);
}

}  // namespace merge_detail

class RegionMerger {
public:
    // Fills `shape` (what the shader outlines) and `draw` (what this call
    // covers). Returns false when neighbours drawn earlier this frame already
    // covered all of this region. `merged` is how many regions were merged
    // into this one's outline.
    //
    // `input_id` identifies the snapshot (this layer, this frame), `slot` this
    // region's place among the panes drawn from it, `now_ms` a monotonic clock.
    bool merge(const MergeShape& self, uint32_t input_id, uint32_t radius, int slot,
               int64_t now_ms, MergeShape* shape, MergeRect* draw, int* merged) {
        using namespace merge_detail;
        const MergeRect r = self.rect;
        *shape = self;
        *draw = r;
        *merged = 0;

        // Does this region stand where it stood in an earlier frame?
        bool still = false;
        for (const Memo& m : memo_) {
            if (m.input_id != input_id && m.slot == slot && now_ms - m.ms <= kWindowMs &&
                sameRect(m.shape.rect, r)) {
                still = true;
                break;
            }
        }

        struct Member {
            MergeShape shape;
            bool this_frame;
        };
        constexpr int kMax = 8;
        Member members[kMax];
        int n = 0;
        members[n++] = {self, true};

        // Which axis, if any: decided by the first neighbour that fits.
        int axis = -1;  // 1 = vertical, 0 = horizontal
        for (bool grew = true; grew && n < kMax;) {
            grew = false;
            for (const Memo& m : memo_) {
                if (n >= kMax) break;
                if (now_ms - m.ms > kWindowMs || m.radius != radius) continue;
                const bool this_frame = m.input_id == input_id;
                // From an earlier frame: only a region still to be drawn this
                // frame, and only while this one is not moving (see above).
                if (!this_frame && (!still || m.slot <= slot)) continue;
                const MergeRect& c = m.shape.rect;
                bool known = false;
                for (int i = 0; i < n; ++i) {
                    if (sameRect(members[i].shape.rect, c)) {
                        if (this_frame) members[i].this_frame = true;
                        known = true;
                        break;
                    }
                }
                if (known) continue;
                for (int v = 1; v >= 0 && !known; --v) {
                    if (axis != -1 && axis != v) continue;
                    if (!sameCross(r, c, v != 0)) continue;
                    for (int i = 0; i < n; ++i) {
                        if (overlapsAlong(members[i].shape.rect, c, v != 0)) {
                            axis = v;
                            members[n++] = {m.shape, this_frame};
                            grew = known = true;
                            break;
                        }
                    }
                }
            }
        }
        // The memory gets this region either way, for the regions drawn after it.
        memo_[next_] = {self, input_id, radius, slot, now_ms};
        next_ = (next_ + 1) % kSize;
        if (n == 1) return true;

        const bool v = axis == 1;
        int first = 0, last = 0;
        for (int i = 1; i < n; ++i) {
            if (lo(members[i].shape.rect, v) < lo(members[first].shape.rect, v)) first = i;
            if (hi(members[i].shape.rect, v) > hi(members[last].shape.rect, v)) last = i;
        }
        const MergeRect& a = members[first].shape.rect;
        const MergeRect& b = members[last].shape.rect;
        MergeShape u;
        u.rect = v ? MergeRect{r.l, a.t, r.r, b.b} : MergeRect{a.l, r.t, b.r, r.b};
        // UL, UR, LR, LL. Vertical: the top pair from the first, the bottom
        // pair from the last. Horizontal: the left pair from the first, the
        // right pair from the last.
        const MergeShape& f = members[first].shape;
        const MergeShape& l = members[last].shape;
        const int from_first[2] = {0, v ? 1 : 3};
        const int from_last[2] = {2, v ? 3 : 1};
        for (int k = 0; k < 2; ++k) {
            u.rx[from_first[k]] = f.rx[from_first[k]];
            u.ry[from_first[k]] = f.ry[from_first[k]];
            u.rx[from_last[k]] = l.rx[from_last[k]];
            u.ry[from_last[k]] = l.ry[from_last[k]];
        }
        *shape = u;
        *merged = n - 1;

        // Skip what neighbours drawn earlier this frame already covered: a
        // neighbour over this region's start moves the start past it, one
        // over its end pulls the end back. One strictly inside is left alone
        // (this region draws over it, which is the stock behaviour too).
        float s0 = lo(r, v), s1 = hi(r, v);
        for (int i = 1; i < n; ++i) {
            if (!members[i].this_frame) continue;
            const MergeRect& m = members[i].shape.rect;
            if (lo(m, v) <= s0 && hi(m, v) > s0) {
                s0 = std::min(hi(m, v), s1);
            } else if (lo(m, v) < s1 && hi(m, v) >= s1) {
                s1 = std::max(lo(m, v), s0);
            }
        }
        if (s1 - s0 < 0.5f) return false;
        *draw = v ? MergeRect{r.l, s0, r.r, s1} : MergeRect{s0, r.t, s1, r.b};
        return true;
    }

private:
    struct Memo {
        MergeShape shape;
        uint32_t input_id = 0;
        uint32_t radius = 0;
        int slot = -1;
        int64_t ms = -1000000;  // never recent
    };
    // A home screen draws ~20 panes a frame and the shade over it as many
    // again; the cap has to still find last frame's body behind all of them.
    static constexpr int kSize = 64;
    static constexpr int64_t kWindowMs = 120;
    Memo memo_[kSize];
    int next_ = 0;
};

}  // namespace glass
