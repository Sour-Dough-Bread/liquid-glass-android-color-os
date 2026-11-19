// Host test for composite_memory.h: a layer is taken for covered only when the
// last two composites both had the same layers in the same order with a heavy
// enough backdrop above it, and outputs that take turns never fool each other.
//
//   g++ -std=c++17 -O2 -Wall -Wextra -I engine/src engine/test/composite_test.cpp -o /tmp/composite_test && /tmp/composite_test
#include "composite_memory.h"

#include <cstdio>

using glass::CompositeMemory;
using glass::LayerBounds;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        ++failures;
        printf("FAIL %s\n", what);
    }
}

constexpr int64_t kMs = 1000 * 1000;
constexpr LayerBounds kFull{0, 0, 1080, 2400};
constexpr LayerBounds kPanel{684, 75, 1080, 2400};

// Three buffers in rotation, as a display's are.
const void* buffer(int frame) {
    static const char kBuffers[3] = {0, 0, 0};
    return &kBuffers[frame % 3];
}
// Another output's buffers: a recording, a cast, a capture.
const void* otherBuffer(int frame) {
    static const char kBuffers[2] = {0, 0};
    return &kBuffers[frame % 2];
}

uint32_t next_id = 100;

// One frame of the common case: the launcher's glass icons, then the shade.
// Returns what the launcher's panes may pre-blur at.
uint32_t homeUnderShade(CompositeMemory& m, int frame, int64_t t, uint32_t shade_radius) {
    const int home = m.begin(buffer(frame), next_id++, 1080, 2400, kFull, t);
    const uint32_t soft = m.coveredRadius(home);
    const int shade = m.begin(buffer(frame), next_id++, 1080, 2400, kFull, t + kMs / 5);
    if (shade_radius != 0) m.cover(shade, shade_radius);
    return soft;
}

// The launcher alone, on another output of the same size.
uint32_t homeAlone(CompositeMemory& m, int frame, int64_t t) {
    return m.coveredRadius(m.begin(otherBuffer(frame), next_id++, 1080, 2400, kFull, t));
}

}  // namespace

int main() {
    // Nothing is known on the first frame, and one frame is not enough; from
    // the third the launcher is covered, at half the shade's radius.
    {
        CompositeMemory m;
        int64_t t = 1000 * kMs;
        check(homeUnderShade(m, 0, t, 27) == 0, "the first frame is drawn in full");
        check(homeUnderShade(m, 1, t += 16 * kMs, 27) == 0, "one frame of cover is not enough");
        for (int f = 2; f < 6; ++f) {
            t += 16 * kMs;
            check(homeUnderShade(m, f, t, 27) == 13, "covered at half the cover's radius");
        }
    }

    // The radius follows the more recent cover, within its limits, and a light
    // cover (the shade just starting to open) covers nothing: both of the last
    // two have to be heavy enough.
    {
        CompositeMemory m;
        int64_t t = 1000 * kMs;
        homeUnderShade(m, 0, t, 15);
        check(homeUnderShade(m, 1, t += 16 * kMs, 16) == 0, "only one frame known");
        check(homeUnderShade(m, 2, t += 16 * kMs, 17) == 0, "15 then 16: the older is too light");
        check(homeUnderShade(m, 3, t += 16 * kMs, 77) == 8, "16 then 17: the smallest pre-blur");
        check(homeUnderShade(m, 4, t += 16 * kMs, 77) == 24, "17 then 77: capped at 24");
    }

    // The shade goes without animating: one frame is still taken for covered
    // (the documented lag), the next is not.
    {
        CompositeMemory m;
        int64_t t = 1000 * kMs;
        for (int f = 0; f < 3; ++f, t += 16 * kMs) homeUnderShade(m, f, t, 27);
        int home = m.begin(buffer(3), next_id++, 1080, 2400, kFull, t);
        check(m.coveredRadius(home) == 13, "the frame after the cover goes is still soft");
        t += 16 * kMs;
        home = m.begin(buffer(4), next_id++, 1080, 2400, kFull, t);
        check(m.coveredRadius(home) == 0, "and the one after is drawn in full");
    }

    // A shade whose blur is not a cover (fading in, or a glass backdrop): the
    // launcher stays sharp.
    {
        CompositeMemory m;
        int64_t t = 1000 * kMs;
        for (int f = 0; f < 4; ++f, t += 16 * kMs) {
            check(homeUnderShade(m, f, t, 0) == 0, "no cover, no pre-blur");
        }
    }

    // The cover has to be ABOVE: the shade itself, and a layer over it, are
    // never covered by it.
    {
        CompositeMemory m;
        int64_t t = 1000 * kMs;
        for (int f = 0; f < 4; ++f, t += 16 * kMs) {
            m.begin(buffer(f), next_id++, 1080, 2400, kFull, t);
            const int shade = m.begin(buffer(f), next_id++, 1080, 2400, kFull, t + kMs / 5);
            m.cover(shade, 27);
            const int volume = m.begin(buffer(f), next_id++, 1080, 2400, kPanel, t + kMs / 2);
            check(m.coveredRadius(shade) == 0, "the covering layer is not covered");
            check(m.coveredRadius(volume) == 0, "a layer above the cover is not covered");
        }
    }

    // A layer between the two that is not a cover is looked past.
    {
        CompositeMemory m;
        int64_t t = 1000 * kMs;
        uint32_t soft = 0;
        for (int f = 0; f < 4; ++f, t += 16 * kMs) {
            const int home = m.begin(buffer(f), next_id++, 1080, 2400, kFull, t);
            soft = m.coveredRadius(home);
            m.begin(buffer(f), next_id++, 1080, 2400, kPanel, t + kMs / 5);
            const int shade = m.begin(buffer(f), next_id++, 1080, 2400, kFull, t + kMs / 2);
            m.cover(shade, 40);
        }
        check(soft == 20, "the cover two layers up counts");
    }

    // The layers below changed. Before: launcher, another full-screen layer,
    // shade. Now a panel's layer comes first, so the launcher sits where the
    // other layer was, with the same size and bounds, and a cover is still
    // above that place in both earlier composites. Only the layers BELOW give
    // it away, and nothing is assumed.
    {
        CompositeMemory m;
        int64_t t = 1000 * kMs;
        for (int f = 0; f < 3; ++f, t += 16 * kMs) {
            const int home = m.begin(buffer(f), next_id++, 1080, 2400, kFull, t);
            const int other = m.begin(buffer(f), next_id++, 1080, 2400, kFull, t + kMs / 5);
            const int shade = m.begin(buffer(f), next_id++, 1080, 2400, kFull, t + kMs / 2);
            m.cover(shade, 27);
            if (f == 2) {
                check(m.coveredRadius(home) == 13 && m.coveredRadius(other) == 13,
                      "both layers under the shade are covered");
            }
        }
        m.begin(buffer(3), next_id++, 1080, 2400, kPanel, t);
        const int home = m.begin(buffer(3), next_id++, 1080, 2400, kFull, t + kMs / 5);
        check(home == 1 && m.coveredRadius(home) == 0, "a new layer below: places do not line up");
    }

    // Another output (a screenshot at half size) is not the display's frame.
    {
        CompositeMemory m;
        int64_t t = 1000 * kMs;
        for (int f = 0; f < 3; ++f, t += 16 * kMs) homeUnderShade(m, f, t, 27);
        const int home = m.begin(otherBuffer(0), next_id++, 540, 1200, LayerBounds{0, 0, 540, 1200},
                                 t - 12 * kMs);
        check(m.coveredRadius(home) == 0, "another output size is another frame");
    }

    // Two outputs of one size taking turns: the display with its shade open,
    // and a recording of the launcher alone. Each follows the other, so
    // neither ever sees two composites that agree, and both are drawn in
    // full on every frame.
    {
        CompositeMemory m;
        int64_t t = 1000 * kMs;
        for (int f = 0; f < 3; ++f, t += 16 * kMs) homeUnderShade(m, f, t, 27);
        for (int f = 3; f < 12; ++f, t += 16 * kMs) {
            const uint32_t alone = homeAlone(m, f, t - 8 * kMs);
            const uint32_t display = homeUnderShade(m, f, t, 27);
            if (f == 3) {
                // The recording's first frame follows two of the display's:
                // the one wrong guess a new output costs.
                check(alone == 13, "a new output's first frame is taken for covered");
            } else {
                check(alone == 0, "the recording is never taken for covered again");
            }
            check(display == 0, "nor the display, while they alternate");
        }
        // The recording stops. Its last frame is the older of the two for
        // one more display frame; after that the saving is back.
        check(homeUnderShade(m, 12, t, 27) == 0, "the recording is still the older of the two");
        check(homeUnderShade(m, 13, t += 16 * kMs, 27) == 13, "covered again");
    }

    // An old composite says nothing, as the newer or the older of the two.
    {
        CompositeMemory m;
        int64_t t = 1000 * kMs;
        homeUnderShade(m, 0, t, 27);
        homeUnderShade(m, 1, t += 16 * kMs, 27);
        check(homeUnderShade(m, 2, t += 300 * kMs, 27) == 0, "a composite 300 ms old is stale");
        check(homeUnderShade(m, 3, t += 16 * kMs, 27) == 0, "and so is the one before a gap");
        check(homeUnderShade(m, 4, t += 16 * kMs, 27) == 13, "two fresh ones again");
    }

    // One buffer for every frame (no rotation): the pause between frames is
    // what separates them.
    {
        CompositeMemory m;
        int64_t t = 1000 * kMs;
        static const char kOnly = 0;
        uint32_t soft = 0;
        for (int f = 0; f < 4; ++f, t += 8 * kMs) {
            const int home = m.begin(&kOnly, next_id++, 1080, 2400, kFull, t);
            check(home == 0, "a frame in the same buffer starts at the first place");
            soft = m.coveredRadius(home);
            const int shade = m.begin(&kOnly, next_id++, 1080, 2400, kFull, t + kMs / 5);
            m.cover(shade, 27);
        }
        check(soft == 13, "frames in one buffer are told apart by the pause");
    }

    // Another canvas starts a composite however soon it comes: a capture one
    // millisecond after the display's frame is not two more layers of it.
    {
        CompositeMemory m;
        const int64_t t = 1000 * kMs;
        check(m.begin(buffer(0), next_id++, 1080, 2400, kFull, t) == 0, "first layer");
        check(m.begin(buffer(0), next_id++, 1080, 2400, kFull, t + kMs / 5) == 1, "second layer");
        check(m.begin(otherBuffer(0), next_id++, 1080, 2400, kFull, t + kMs) == 0,
              "another canvas a millisecond later is a new composite");
    }

    // A frame cut in two by a long pause between its layers: the halves are
    // taken for two composites, and nothing is covered by mistake.
    {
        CompositeMemory m;
        int64_t t = 1000 * kMs;
        for (int f = 0; f < 3; ++f, t += 16 * kMs) homeUnderShade(m, f, t, 27);
        const int home = m.begin(buffer(3), next_id++, 1080, 2400, kFull, t);
        check(m.coveredRadius(home) == 13, "first half: as the last two frames");
        const int shade = m.begin(buffer(3), next_id++, 1080, 2400, kFull, t + 5 * kMs);
        m.cover(shade, 27);
        check(shade == 0 && m.coveredRadius(shade) == 0,
              "second half: the shade is no pane under a cover");
        t += 16 * kMs;
        check(homeUnderShade(m, 4, t, 27) == 0, "the frame after a cut frame is drawn in full");
        check(homeUnderShade(m, 5, t += 16 * kMs, 27) == 0, "and the next: the cut is the older");
        check(homeUnderShade(m, 6, t += 16 * kMs, 27) == 13, "then covered again");
    }

    // More blur layers than are remembered: such a frame is not trusted, even
    // for the places it does remember. The cover here sits at a place that
    // fits (the second), over a launcher at the first, in every frame.
    {
        CompositeMemory m;
        int64_t t = 1000 * kMs;
        for (int f = 0; f < 4; ++f, t += 16 * kMs) {
            for (int i = 0; i < CompositeMemory::kMaxPasses + 2; ++i) {
                const int p = m.begin(buffer(f), next_id++, 1080, 2400, kFull, t + i * (kMs / 10));
                check(i < CompositeMemory::kMaxPasses ? p == i : p == -1,
                      "places up to the limit, none past it");
                if (i == 1) m.cover(p, 27);
            }
            check(m.coveredRadius(0) == 0, "a frame with too many layers: nothing covered");
        }
        // The same layers without the extra ones are trusted again after two
        // frames that fit.
        check(homeUnderShade(m, 4, t, 27) == 0, "after an overflowing frame");
        check(homeUnderShade(m, 5, t += 16 * kMs, 27) == 0, "the overflow is still the older");
        check(homeUnderShade(m, 6, t += 16 * kMs, 27) == 13, "two frames that fit");
    }

    // The same snapshot again is the same layer, not a new one.
    {
        CompositeMemory m;
        const int64_t t = 1000 * kMs;
        const int a = m.begin(buffer(0), 7, 1080, 2400, kFull, t);
        const int b = m.begin(buffer(0), 7, 1080, 2400, kFull, t + kMs / 10);
        const int c = m.begin(buffer(0), 8, 1080, 2400, kFull, t + kMs / 5);
        check(a == 0 && b == 0 && c == 1, "a layer's regions share its place");
    }

    if (failures == 0) printf("composite_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
