// ===================================================================== API
//
// glass_draw.h: what the module's hooks, or a ROM's GlassFilter, call. Each
// entry point is the body of one of the module's own functions, with the
// part that knows the hooked signature left to the host.
//
// PROPOSED (rom_patch.md, "The engine/src split"); appended to the extracted
// drawing code by rom/split/split_engine.py until engine/src has it.

namespace glass {
namespace draw {

namespace {

// generateHook<K> for a slot known only at runtime.
template <size_t... K>
skia::ReturnedSkSp generateIn(std::index_sequence<K...>, size_t slot, const void* filter,
                              void* context, const void* display, uint32_t radius,
                              void* input_sp, const SkRect* blur_rect) {
    static constexpr GenerateFn kHooks[] = {&::generateHook<K>...};
    return kHooks[slot](filter, context, display, radius, input_sp, blur_rect);
}

}  // namespace

// glassInit's test.
bool enabledAtBoot() { return ::propInt("persist.sys.glass.enabled", 1) != 0; }

void setStatus(const char* status) { ::engineStatus(status); }

// install()'s report.
void setRegen(bool available) { __system_property_set("sys.glass.regen", available ? "1" : "0"); }

skia::Bridge& bridge() { return ::g_sk; }

void setGenerator(size_t slot, GenerateFn fn) {
    if (slot < ::kFilterCount) ::g_generate[slot] = fn;
}

skia::ReturnedSkSp generate(size_t slot, const void* filter, void* context, const void* display,
                            uint32_t radius, void* input_sp, const SkRect* blur_rect) {
    // A slot with no generator has nothing to call: the filter was never
    // handed over. Not a call a host makes.
    if (slot >= ::kFilterCount || ::g_generate[slot] == nullptr) return skia::ReturnedSkSp(nullptr);
    return generateIn(std::make_index_sequence<::kFilterCount>{}, slot, filter, context, display,
                      radius, input_sp, blur_rect);
}

// drawGlassScaled, with the original behind a callback.
void blurRegion(SkCanvas* canvas, const SkRRect* region, uint32_t radius, float zoom,
                float alpha, const SkRect* blur_rect, void* blurred_sp, void* input_sp,
                OriginalFn original, void* cookie) {
    const int64_t t0 = glass::param(glass::P_stats) != 0.0f ? ::monotonicNs() : 0;
    ::deferredBlurSeen(blurred_sp, input_sp);
    if (!::drawGlassCore(canvas, region, radius, zoom, alpha, blur_rect, blurred_sp, input_sp) &&
        original != nullptr) {
        ::drawWithAndroidsBlur(blurred_sp, input_sp, blur_rect, radius, [&](void* blurred) {
            original(cookie, blurred, ::effectiveRadius(radius));
        });
    }
    if (t0 != 0) ::g_stats.hook_ns += ::monotonicNs() - t0;
    ::statsFlush();
}

// drawGlassScaledKind's kind > 0 branch.
void androidDraws(const SkRect* blur_rect, uint32_t radius, void* blurred_sp, void* input_sp,
                  OriginalFn original, void* cookie) {
    const int64_t t0 = glass::param(glass::P_stats) != 0.0f ? ::monotonicNs() : 0;
    ::deferredBlurSeen(blurred_sp, input_sp);
    if (original != nullptr) {
        ::drawWithAndroidsBlur(blurred_sp, input_sp, blur_rect, radius, [&](void* blurred) {
            original(cookie, blurred, radius);
        });
    }
    if (t0 != 0) ::g_stats.hook_ns += ::monotonicNs() - t0;
    ::statsFlush();
}

// The made blurs and the backdrop go back to Skia's cache, which is safe in
// any context. The copy of part of the snapshot (snapshotCopy) is not let go
// here: it has to be deleted under the context it was made in, and which one
// is current while a filter is torn down is not known. It is let go as
// before, when a frame in its own context moves on, or not at all if none
// comes: one texture, once, as the process ends.
void releaseAll() {
    for (auto& e : ::g_made) {
        SkSafeUnref(e.image);
        e = {};
    }
    SkSafeUnref(::g_backdrop.image);
    ::g_backdrop = {};
    ::g_last_generate.valid = false;
}

}  // namespace draw
}  // namespace glass
