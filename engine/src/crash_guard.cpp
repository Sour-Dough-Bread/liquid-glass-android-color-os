#include "crash_guard.h"

#include <android/log.h>
#include <sys/system_properties.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>

#define LOG_TAG "StockGlass"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace glass {
namespace {

constexpr const char* kActive = "sys.glass.active";
constexpr const char* kTripped = "sys.glass.tripped";
constexpr const char* kMode = "persist.sys.glass.mode";

std::atomic<bool> g_armed{false};
std::atomic<bool> g_holding{false};
std::atomic<bool> g_warned{false};
const prop_info* g_mode_pi = nullptr;
uint32_t g_mode_serial = 0;

bool set(const char* name, const char* value) {
    return __system_property_set(name, value) == 0;
}

}  // namespace

bool crashGuardStart() {
    char v[PROP_VALUE_MAX] = {};
    __system_property_get(kActive, v);
    if (strcmp(v, "1") != 0) return false;

    // The previous SurfaceFlinger of this boot was drawing glass when it died.
    const bool mode_ok = set(kMode, "stock");
    char note[PROP_VALUE_MAX];
    snprintf(note, sizeof(note), "%lld", static_cast<long long>(time(nullptr)));
    set(kTripped, note);
    set(kActive, "0");

    g_mode_pi = __system_property_find(kMode);
    g_mode_serial = g_mode_pi != nullptr ? __system_property_serial(g_mode_pi) : 0;
    g_holding = true;
    LOGE("SurfaceFlinger restarted while Liquid Glass was on: the previous one died drawing it. "
         "Switched to Android's blur%s. The tombstone in /data/tombstones says where; "
         "`glassctl glass` turns it back on",
         mode_ok ? " (persist.sys.glass.mode=stock)"
                 : " for this run only — could not save persist.sys.glass.mode");
    return true;
}

bool crashGuardHolding() {
    if (!g_holding.load(std::memory_order_relaxed)) return false;
    // Released by anyone setting the mode again, which is a deliberate choice
    // made after the trip. Our own write above happened before the serial was
    // recorded, so it does not count.
    if (g_mode_pi == nullptr) {
        g_mode_pi = __system_property_find(kMode);
        if (g_mode_pi == nullptr) return true;
        g_holding = false;
        return false;
    }
    if (__system_property_serial(g_mode_pi) != g_mode_serial) {
        g_holding = false;
        return false;
    }
    return true;
}

void crashGuardArm() {
    if (g_armed.load(std::memory_order_relaxed)) return;
    g_armed = true;
    if (!set(kActive, "1") && !g_warned.exchange(true)) {
        LOGW("could not set %s; a crash in the glass path would loop instead of falling back",
             kActive);
    }
}

void crashGuardDisarm() {
    if (!g_armed.load(std::memory_order_relaxed)) return;
    g_armed = false;
    set(kActive, "0");
}

}  // namespace glass
