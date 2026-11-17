// Host test for region_tint.h: every encoded tint survives the trip through a
// float, and no corner radius computed the ordinary way reads as a tint.
//
//   g++ -std=c++17 -O2 -I engine/src engine/test/tint_test.cpp -o /tmp/tint_test && /tmp/tint_test
#include "region_tint.h"

#include <cstdio>
#include <initializer_list>

using glass::decodeRegionTint;
using glass::encodeRegionTintCorner;

int main() {
    int failures = 0;

    // Round trip: every byte, at radii from tiny to large, including a float
    // that went through a double and back as a parcel would not change it.
    const float radii[] = {0.0f, 1.0f, 12.0f, 54.7f, 63.0f, 120.0f, 999.9f, 8191.0f};
    for (float r : radii) {
        for (int q = 0; q < 256; ++q) {
            const int bytes[4] = {q, 255 - q, (q * 7) & 255, q == 0 ? 1 : q};
            float rx[4], ry[4];
            for (int i = 0; i < 4; ++i) rx[i] = ry[i] = encodeRegionTintCorner(r, bytes[i]);
            const glass::RegionTint t = decodeRegionTint(rx, ry);
            const float want[4] = {bytes[0] / 255.0f, bytes[1] / 255.0f, bytes[2] / 255.0f,
                                   bytes[3] / 255.0f};
            const float got[4] = {t.rgb[0], t.rgb[1], t.rgb[2], t.strength};
            bool ok = t.on;
            for (int i = 0; i < 4; ++i) ok = ok && got[i] == want[i];
            if (!ok) {
                if (++failures < 10) printf("FAIL round trip r=%g q=%d\n", r, q);
            }
        }
    }

    // Strength 0 decodes as untinted.
    {
        float rx[4], ry[4];
        for (int i = 0; i < 4; ++i) rx[i] = ry[i] = encodeRegionTintCorner(40.0f, i == 3 ? 0 : 200);
        if (decodeRegionTint(rx, ry).on) { ++failures; printf("FAIL strength 0 read as on\n"); }
    }

    // Ordinary radii: whole dp at every common density, plus half and quarter
    // pixels, are never a tint, all four corners equal or not.
    const float densities[] = {1.0f, 1.5f, 2.0f, 2.25f, 2.5f, 2.625f, 2.75f, 2.8125f, 3.0f,
                               3.25f, 3.5f, 4.0f};
    int ordinary = 0;
    for (float d : densities) {
        for (int dp = 0; dp <= 64; ++dp) {
            for (float extra : {0.0f, 0.25f, 0.5f, 0.75f, 0.125f}) {
                const float r = dp * d + extra;
                float rx[4] = {r, r, r, r}, ry[4] = {r, r, r, r};
                ++ordinary;
                if (decodeRegionTint(rx, ry).on) {
                    ++failures;
                    printf("FAIL ordinary radius %g (dp %d at %g) read as a tint\n", r, dp, d);
                }
            }
        }
    }

    // A corner whose x and y radii differ (an elliptical corner) is never a tint.
    {
        float rx[4], ry[4];
        for (int i = 0; i < 4; ++i) rx[i] = ry[i] = encodeRegionTintCorner(30.0f, 100);
        ry[2] += 1.0f;
        if (decodeRegionTint(rx, ry).on) { ++failures; printf("FAIL elliptical corner\n"); }
    }

    printf("%s: round trips at %zu radii x 256, %d ordinary radii checked\n",
           failures == 0 ? "PASS" : "FAILED", sizeof(radii) / sizeof(radii[0]), ordinary);
    return failures == 0 ? 0 : 1;
}
