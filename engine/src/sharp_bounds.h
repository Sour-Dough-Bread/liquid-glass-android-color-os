// Which part of the snapshot a layer's panes read.
//
// A pane that shows what is behind it reads the snapshot of the output, and
// Skia will not sample a surface while drawing into it: the first draw that
// names the snapshot costs a copy of the whole framebuffer. The engine can copy
// just the part the panes read instead (snapshotCopy=bounds), but only BEFORE
// anything is drawn into the output, which is when the first pane of the layer
// arrives and the others are still to come. So it copies where the layer's
// panes were a frame ago, grown a little, and a pane that turns out to read
// outside the copy reads the snapshot itself (which then costs the full copy as
// well: correct, and dearer than not having tried).
//
// That makes it a bet, taken only when it looks safe: the panes of this layer
// stood still for the last two frames (a keyboard, a volume panel, a home
// screen at rest), and what they read is a small enough share of the output to
// be worth copying apart. A layer whose panes are moving (a page swipe, a
// panel sliding in) is left to the full copy until they settle.
//
// Header-only and free of Skia so that test/sharp_bounds_test.cpp runs it on
// the host.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace glass {

struct ReadRect {
    float l = 0.0f, t = 0.0f, r = 0.0f, b = 0.0f;

    bool empty() const { return !(r > l && b > t); }
    float area() const { return empty() ? 0.0f : (r - l) * (b - t); }
    bool contains(const ReadRect& o) const {
        return o.l >= l && o.t >= t && o.r <= r && o.b <= b;
    }
    ReadRect grown(float by) const { return {l - by, t - by, r + by, b + by}; }
    ReadRect within(float w, float h) const {
        return {std::max(l, 0.0f), std::max(t, 0.0f), std::min(r, w), std::min(b, h)};
    }
    void join(const ReadRect& o) {
        if (o.empty()) return;
        if (empty()) {
            *this = o;
            return;
        }
        l = std::min(l, o.l);
        t = std::min(t, o.t);
        r = std::max(r, o.r);
        b = std::max(b, o.b);
    }
    bool operator==(const ReadRect& o) const {
        return l == o.l && t == o.t && r == o.r && b == o.b;
    }
};

class SharpBounds {
public:
    // How far a pane may have moved since the last frame and still be inside
    // the copy, px.
    static constexpr float kMotionMargin = 32.0f;
    // Past this share of the output the copy is not worth making apart.
    static constexpr float kMaxShare = 0.6f;
    // Frames further apart than this are not consecutive.
    static constexpr int64_t kFreshNs = 250LL * 1000 * 1000;
    static constexpr int kLayers = 4;

    // Call once for every pane that reads the snapshot, in draw order. `key`
    // is the layer's bounds on the output (what tells layers apart), `w` x `h`
    // the output's size, `input_id` the layer's snapshot this frame, `reach`
    // what the pane reads on the output.
    //
    // For the FIRST such pane of a layer in a frame, `copy` is set to the rect
    // worth copying, or left empty when the snapshot itself should be read.
    // Returns whether this call was that first pane.
    bool note(const ReadRect& key, float w, float h, uint32_t input_id, const ReadRect& reach,
              int64_t now_ns, ReadRect* copy) {
        *copy = ReadRect{};
        Layer& e = layerFor(key, w, h, now_ns);
        const ReadRect mine = reach.within(w, h);
        const bool first = e.input_id != input_id;
        if (first) {
            const bool consecutive = e.input_id != 0 && now_ns - e.ns <= kFreshNs;
            e.before = consecutive ? e.last : ReadRect{};
            e.last = consecutive ? e.now : ReadRect{};
            e.now = ReadRect{};
            e.input_id = input_id;
            // Steady: last frame's panes read inside what the frame before's
            // did, give or take the margin, and so can this frame's be
            // expected to.
            if (!e.last.empty() && !e.before.empty() &&
                e.before.grown(kMotionMargin).contains(e.last)) {
                ReadRect u = e.last.grown(kMotionMargin);
                u.join(mine);
                u = u.within(w, h);
                if (!u.empty() && u.area() <= kMaxShare * w * h) *copy = u;
            }
        }
        e.ns = now_ns;
        e.now.join(mine);
        return first;
    }

private:
    struct Layer {
        ReadRect key;
        float w = 0.0f, h = 0.0f;
        uint32_t input_id = 0;
        int64_t ns = 0;
        ReadRect now, last, before;  // what the panes read: this frame, the last, the one before
    };

    Layer& layerFor(const ReadRect& key, float w, float h, int64_t now_ns) {
        Layer* oldest = &layers_[0];
        for (Layer& e : layers_) {
            if (e.input_id != 0 && e.key == key && e.w == w && e.h == h) return e;
            if (e.ns < oldest->ns) oldest = &e;
        }
        *oldest = Layer{};
        oldest->key = key;
        oldest->w = w;
        oldest->h = h;
        oldest->ns = now_ns;
        return *oldest;
    }

    Layer layers_[kLayers];
};

}  // namespace glass
