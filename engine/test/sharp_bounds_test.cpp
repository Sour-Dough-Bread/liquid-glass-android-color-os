// Host test for sharp_bounds.h: a copy of part of the snapshot is offered only
// for a layer whose panes have stood still for two frames, covers what they
// read with room to move, and is never offered for most of the screen.
//
//   g++ -std=c++17 -O2 -Wall -Wextra -I engine/src engine/test/sharp_bounds_test.cpp -o /tmp/sharp_bounds_test && /tmp/sharp_bounds_test
#include "sharp_bounds.h"

#include <cstdio>

using glass::ReadRect;
using glass::SharpBounds;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        ++failures;
        printf("FAIL %s\n", what);
    }
}

constexpr int64_t kMs = 1000 * 1000;
constexpr float kW = 1080, kH = 2400;
constexpr ReadRect kIme{0, 1500, 1080, 2400};      // a keyboard's layer
constexpr ReadRect kFull{0, 0, 1080, 2400};        // the launcher's
uint32_t next_id = 10;

}  // namespace

int main() {
    // A keyboard: two panes that never move. No copy on the first two frames,
    // then one that holds both with the margin, clipped to the output.
    {
        SharpBounds s;
        const ReadRect keys{0, 1534, 1080, 2295}, strip{0, 2295, 1080, 2400};
        int64_t t = 1000 * kMs;
        ReadRect copy;
        for (int f = 0; f < 5; ++f, t += 16 * kMs) {
            const uint32_t id = next_id++;
            const bool first = s.note(kIme, kW, kH, id, keys, t, &copy);
            check(first, "the first pane of a frame is the first");
            if (f < 2) {
                check(copy.empty(), "no copy until two frames agree");
            } else {
                check(!copy.empty() && copy.contains(keys) && copy.contains(strip),
                      "the copy holds both panes");
                check(copy.t == 1534 - 32 && copy.b == 2400 && copy.l == 0 && copy.r == 1080,
                      "grown by the margin, clipped to the output");
                check(copy.area() <= SharpBounds::kMaxShare * kW * kH, "and under the share");
            }
            ReadRect none;
            check(!s.note(kIme, kW, kH, id, strip, t + kMs / 10, &none) && none.empty(),
                  "later panes of the frame get no copy of their own");
        }
    }

    // A panel sliding in: every frame's panes read outside the last frame's
    // by more than the margin, so no copy is offered while it moves; once two
    // frames in a row found it in the same place, one is. (The last frame of
    // the slide already stood where it stops, so that is the second still
    // frame.)
    {
        SharpBounds s;
        int64_t t = 1000 * kMs;
        ReadRect copy;
        float x = 1080;
        for (int f = 0; f < 8; ++f, t += 16 * kMs) {
            x -= 60;
            s.note(kFull, kW, kH, next_id++, ReadRect{x, 900, x + 168, 1700}, t, &copy);
            check(copy.empty(), "no copy for a pane that moves 60 px a frame");
        }
        int offered = 0;
        for (int f = 0; f < 4; ++f, t += 16 * kMs) {
            s.note(kFull, kW, kH, next_id++, ReadRect{x, 900, x + 168, 1700}, t, &copy);
            if (!copy.empty()) {
                ++offered;
                check(copy.contains(ReadRect{x, 900, x + 168, 1700}), "it holds the pane");
            }
        }
        check(offered == 3, "offered once two frames agree on where it is");
    }

    // A slow drift stays inside the margin: the copy keeps being offered and
    // keeps holding the pane.
    {
        SharpBounds s;
        int64_t t = 1000 * kMs;
        ReadRect copy;
        float y = 900;
        int offered = 0;
        for (int f = 0; f < 10; ++f, t += 16 * kMs, y += 10) {
            const ReadRect pane{800, y, 968, y + 400};
            s.note(kFull, kW, kH, next_id++, pane, t, &copy);
            if (!copy.empty()) {
                ++offered;
                check(copy.contains(pane), "a pane drifting 10 px a frame is inside its copy");
            }
        }
        check(offered == 8, "offered on every frame after the first two");
    }

    // A home screen of icons: together they read most of the output, and the
    // whole snapshot is the better source.
    {
        SharpBounds s;
        int64_t t = 1000 * kMs;
        ReadRect copy;
        for (int f = 0; f < 4; ++f, t += 16 * kMs) {
            const uint32_t id = next_id++;
            s.note(kFull, kW, kH, id, ReadRect{60, 300, 240, 480}, t, &copy);
            check(copy.empty(), "no copy when the panes span most of the output");
            ReadRect none;
            s.note(kFull, kW, kH, id, ReadRect{840, 2100, 1020, 2280}, t, &none);
        }
    }

    // Frames a long way apart are not consecutive: nothing is assumed.
    {
        SharpBounds s;
        int64_t t = 1000 * kMs;
        ReadRect copy;
        const ReadRect pane{800, 900, 968, 1300};
        for (int f = 0; f < 4; ++f, t += 400 * kMs) {
            s.note(kFull, kW, kH, next_id++, pane, t, &copy);
            check(copy.empty(), "frames 400 ms apart say nothing about each other");
        }
    }

    // Two layers are kept apart: the keyboard's panes do not widen the
    // launcher's copy.
    {
        SharpBounds s;
        int64_t t = 1000 * kMs;
        ReadRect a, b;
        const ReadRect icon{60, 300, 240, 480}, keys{0, 1534, 1080, 2295};
        for (int f = 0; f < 4; ++f, t += 16 * kMs) {
            s.note(kFull, kW, kH, next_id++, icon, t, &a);
            s.note(kIme, kW, kH, next_id++, keys, t + kMs / 5, &b);
        }
        check(!a.empty() && a.b < 600, "the launcher's copy is its icon's");
        check(!b.empty() && b.t > 1400, "the keyboard's copy is the keyboard's");
    }

    // A pane that reaches past the output's edge is clipped to it.
    {
        SharpBounds s;
        int64_t t = 1000 * kMs;
        ReadRect copy;
        for (int f = 0; f < 3; ++f, t += 16 * kMs) {
            s.note(kFull, kW, kH, next_id++, ReadRect{900, -40, 1200, 300}, t, &copy);
        }
        check(!copy.empty() && copy.l >= 0 && copy.t == 0 && copy.r == kW,
              "the copy stays on the output");
    }

    if (failures == 0) printf("sharp_bounds_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
