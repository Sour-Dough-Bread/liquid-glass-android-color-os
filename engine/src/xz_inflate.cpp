#include "xz_inflate.h"

extern "C" {
#include "xz.h"
}

namespace glass {
namespace {

// .gnu_debugdata on Android is a few hundred KB compressed expanding to a few
// MB. Start generously so the common case never reallocates, but still grow
// rather than assuming.
constexpr size_t kInitialOut = 4u << 20;
constexpr size_t kMaxOut = 256u << 20;   // refuse absurd expansion rather than OOM SurfaceFlinger
constexpr uint32_t kDictMax = 1u << 26;

bool g_crc_ready = false;

}  // namespace

bool inflateXz(const uint8_t* in, size_t in_size, std::vector<uint8_t>* out) {
    if (in == nullptr || in_size == 0 || out == nullptr) return false;

    if (!g_crc_ready) {
        xz_crc32_init();
        g_crc_ready = true;
    }

    // XZ_DYNALLOC rather than XZ_SINGLE: the decompressed size is not known up
    // front, and XZ_SINGLE requires the whole output buffer to be sized
    // correctly in advance.
    struct xz_dec* dec = xz_dec_init(XZ_DYNALLOC, kDictMax);
    if (dec == nullptr) return false;

    out->assign(kInitialOut, 0);

    struct xz_buf b {};
    b.in = in;
    b.in_pos = 0;
    b.in_size = in_size;
    b.out = out->data();
    b.out_pos = 0;
    b.out_size = out->size();

    enum xz_ret r = XZ_OK;
    for (;;) {
        r = xz_dec_run(dec, &b);
        if (r != XZ_OK) break;

        // All input was supplied up front, so XZ_OK can only mean the output
        // buffer filled up.
        if (b.out_pos < b.out_size) break;   // no progress possible; treat as failure below
        if (out->size() >= kMaxOut) { r = XZ_MEM_ERROR; break; }

        const size_t grown = out->size() * 2 > kMaxOut ? kMaxOut : out->size() * 2;
        out->resize(grown, 0);
        b.out = out->data();
        b.out_size = out->size();
    }

    xz_dec_end(dec);

    if (r != XZ_STREAM_END) {
        out->clear();
        return false;
    }
    out->resize(b.out_pos);
    return true;
}

bool inflateXzAdapter(const uint8_t* in, size_t in_size, std::vector<uint8_t>* out, void*) {
    return inflateXz(in, in_size, out);
}

}  // namespace glass
