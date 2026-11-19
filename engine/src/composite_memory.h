// What the last composite did, layer by layer.
//
// drawBlurRegion is told about one region of one layer. Whether a LATER layer
// will blur what this one draws is not something it can see, and it matters: a
// pane with no backdrop of its own reads the sharp snapshot, which costs a copy
// of the framebuffer and the shader's finest reads, and under an open shade
// none of that sharpness is visible. So the engine remembers what the last
// composite did and expects this one to do the same.
//
// A composite is one RenderEngine::drawLayers: its blur layers arrive as a run
// of snapshots drawn into one canvas, a fraction of a millisecond apart.
// Another canvas (the next frame draws into another buffer, a screenshot into
// its own) or a pause starts the next. A layer is known by its place in that
// run, the output's size and its bounds on it.
//
// Being wrong is cheap one way and mild the other. A layer taken for uncovered
// is drawn in full, as always. One taken for covered when the backdrop above it
// has just gone (the frame after a shade vanishes without animating) shows, for
// that one frame, what is behind its glass blurred by a few pixels; the frame
// after is right again.
//
// The last TWO composites have to agree before anything is taken for covered.
// One would do for a display on its own, but outputs take turns: while a
// single app is recorded or cast at the display's size, a composite of that
// app alone (no shade) runs between every two of the display's (shade open),
// and each would take the other for what came before it, wrongly, on every
// frame. With two, neither output ever sees two composites that agree, and
// both are drawn in full for as long as they alternate.
//
// Header-only and free of Skia so that test/composite_test.cpp runs it on the
// host.
#pragma once

#include <algorithm>
#include <cstdint>

namespace glass {

struct LayerBounds {
    float l = 0.0f, t = 0.0f, r = 0.0f, b = 0.0f;
    bool operator==(const LayerBounds& o) const {
        return l == o.l && t == o.t && r == o.r && b == o.b;
    }
};

class CompositeMemory {
public:
    // Blur layers remembered per composite. A frame with more is not trusted.
    static constexpr int kMaxPasses = 6;
    // Two calls this far apart are not one drawLayers. Within one they are well
    // under a millisecond apart; two frames are at least a refresh period.
    static constexpr int64_t kSameCompositeNs = 3 * 1000 * 1000;
    // A composite older than this says nothing about the next.
    static constexpr int64_t kFreshNs = 250LL * 1000 * 1000;
    // The lightest blur above a pane that hides what the pane would have read.
    static constexpr uint32_t kCoverMinRadius = 16;
    // What a covered pane pre-blurs its view at: half the cover's radius (so
    // the cover's own blur grows by under 12%), within these.
    static constexpr uint32_t kSoftMinRadius = 8;
    static constexpr uint32_t kSoftMaxRadius = 24;

    // The layer this call belongs to: its place in the current composite, or -1
    // when the composite has more layers than are remembered. `canvas` is what
    // is being drawn into, `input_id` the layer's snapshot (unique per layer
    // per composite), `w` x `h` the output's size, `bounds` the layer's on it.
    int begin(const void* canvas, uint32_t input_id, float w, float h, const LayerBounds& bounds,
              int64_t now_ns) {
        Composite& c = now_;
        if (c.count != 0 && c.last_id == input_id) {
            c.last_ns = now_ns;
            return c.overflow ? -1 : c.count - 1;
        }
        const bool same = c.count != 0 && c.canvas == canvas &&
                          now_ns - c.last_ns < kSameCompositeNs;
        if (!same) {
            before2_ = before_;
            before_ = c;
            c = Composite{};
            c.canvas = canvas;
        }
        c.last_id = input_id;
        c.last_ns = now_ns;
        if (c.count >= kMaxPasses) {
            c.overflow = true;
            return -1;
        }
        c.pass[c.count] = Pass{w, h, bounds, 0};
        return c.count++;
    }

    // The layer at `pass` drew a backdrop over the whole output that shows only
    // blurred content (Android's, or a toned one), blurred at `radius`.
    void cover(int pass, uint32_t radius) {
        if (pass < 0 || pass >= now_.count || now_.overflow) return;
        now_.pass[pass].cover_radius = radius;
    }

    // The radius a pane of the layer at `pass` may pre-blur what it sees
    // through at, or 0 when nothing says a backdrop will blur it. Not 0 only
    // when each of the last two composites had the same layers up to this
    // one, in the same order, and a later layer that covered the output
    // heavily enough. The radius is from the more recent of the two.
    uint32_t coveredRadius(int pass) const {
        if (pass < 0 || pass >= now_.count || now_.overflow) return 0;
        const uint32_t older = coverAbove(before2_, before_.last_ns, pass);
        const uint32_t newer = coverAbove(before_, now_.last_ns, pass);
        if (older < kCoverMinRadius || newer < kCoverMinRadius) return 0;
        return std::clamp<uint32_t>(newer / 2, kSoftMinRadius, kSoftMaxRadius);
    }

private:
    struct Pass {
        float w = 0.0f, h = 0.0f;  // the output's size
        LayerBounds bounds;        // the layer's bounds on it
        uint32_t cover_radius = 0;
    };
    struct Composite {
        const void* canvas = nullptr;
        int count = 0;
        bool overflow = false;  // more layers than fit: nothing here is trusted
        uint32_t last_id = 0;   // the snapshot the last call was for
        int64_t last_ns = 0;
        Pass pass[kMaxPasses];
    };

    static bool sameLayer(const Pass& a, const Pass& b) {
        return a.w == b.w && a.h == b.h && a.bounds == b.bounds;
    }

    // The radius of the cover that `was` (a composite that ended no more than
    // kFreshNs before `then_ns`) drew above the layer now at `pass`, or 0:
    // none, or its layers up to there are not the current composite's.
    uint32_t coverAbove(const Composite& was, int64_t then_ns, int pass) const {
        if (was.overflow || pass >= was.count || then_ns - was.last_ns > kFreshNs) return 0;
        for (int k = 0; k <= pass; ++k) {
            if (!sameLayer(was.pass[k], now_.pass[k])) return 0;
        }
        const Pass& me = now_.pass[pass];
        for (int j = pass + 1; j < was.count; ++j) {
            const Pass& above = was.pass[j];
            if (above.cover_radius == 0 || above.w != me.w || above.h != me.h) continue;
            return above.cover_radius;
        }
        return 0;
    }

    Composite now_, before_, before2_;
};

}  // namespace glass
