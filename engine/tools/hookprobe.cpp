// hookprobe — can surfaceflinger's SELinux domain install a frida-gum hook?
//
// Run it IN that domain, without touching the real compositor:
//
//   adb push out/arm64-v8a/hookprobe /data/local/tmp/
//   adb shell su -c 'chcon u:object_r:surfaceflinger_exec:s0 /data/local/tmp/hookprobe'
//   adb shell su -c 'runcon u:r:surfaceflinger:s0 /data/local/tmp/hookprobe'
//   adb logcat -d -s HookProbe
//
// Labelled surfaceflinger_exec, so execmod is checked against the same type the
// real patch is. It reports what glass::canPatchCode() decides, then — only if
// that says yes — does what the engine does: gum_interceptor_replace on one of
// its own functions, calls it, calls the original through the trampoline, and
// reverts. Output goes to logcat because the surfaceflinger domain cannot write
// to the adb shell's terminal. Exit status 0 = hook works, 1 = refused (cleanly),
// anything else = it crashed, which is exactly what the check exists to prevent.

#include <android/log.h>
#include <frida-gum.h>

#include <string>

#include "../src/exec_probe.h"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "HookProbe", __VA_ARGS__)

extern "C" __attribute__((noinline, used)) int probeTarget(int x) {
    // Enough body that the prologue gum overwrites is real code.
    volatile int acc = x;
    for (int i = 0; i < 3; ++i) acc = acc * 3 + 1;
    return acc;
}

static int (*g_original)(int) = nullptr;

static int replacement(int x) { return g_original(x) + 1000000; }

int main() {
    std::string why;
    if (!glass::canPatchCode(&why)) {
        LOG("refused: %s", why.c_str());
        return 1;
    }
    LOG("execmem and execmod allowed");

    const int before = probeTarget(5);
    gum_init_embedded();
    GumInterceptor* in = gum_interceptor_obtain();
    gum_interceptor_begin_transaction(in);
    const GumReplaceReturn r =
        gum_interceptor_replace(in, reinterpret_cast<void*>(&probeTarget),
                                reinterpret_cast<void*>(&replacement),
                                reinterpret_cast<gpointer*>(&g_original), nullptr);
    gum_interceptor_end_transaction(in);
    if (r != GUM_REPLACE_OK || g_original == nullptr) {
        LOG("gum_interceptor_replace failed: %d", static_cast<int>(r));
        return 2;
    }
    const int hooked = probeTarget(5);
    const int through = g_original(5);
    gum_interceptor_revert(in, reinterpret_cast<void*>(&probeTarget));
    const int after = probeTarget(5);
    const bool ok = hooked == before + 1000000 && through == before && after == before;
    LOG("before=%d hooked=%d original-through-trampoline=%d after-revert=%d -> %s", before, hooked,
        through, after, ok ? "HOOK WORKS" : "WRONG RESULTS");
    return ok ? 0 : 3;
}
