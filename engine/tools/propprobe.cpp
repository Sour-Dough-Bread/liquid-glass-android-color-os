// propprobe — which properties may surfaceflinger's SELinux domain set?
//
// The engine's crash guard records "glass is on" in a property that outlives a
// SurfaceFlinger crash, so the restarted compositor can come up stock. That only
// works if the surfaceflinger domain may set it. Run it in that domain, the
// same way as hookprobe:
//
//   adb push out/arm64-v8a/propprobe /data/local/tmp/
//   adb shell su -c 'chcon u:object_r:surfaceflinger_exec:s0 /data/local/tmp/propprobe'
//   adb shell su -c 'runcon u:r:surfaceflinger:s0 /data/local/tmp/propprobe'
//   adb logcat -d -s PropProbe
//
// Only non-persistent names are written, so nothing survives a reboot.
#include <android/log.h>
#include <sys/system_properties.h>

#include <cstring>

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "PropProbe", __VA_ARGS__)

static bool tryName(const char* name) {
    const int rc = __system_property_set(name, "1");
    char value[PROP_VALUE_MAX] = {};
    __system_property_get(name, value);
    const bool ok = rc == 0 && strcmp(value, "1") == 0;
    LOG("%-24s set rc=%d read back '%s' -> %s", name, rc, value, ok ? "ALLOWED" : "refused");
    return ok;
}

int main() {
    char mode[PROP_VALUE_MAX] = {};
    __system_property_get("persist.sys.glass.mode", mode);
    LOG("can read persist.sys.glass.mode: '%s'", mode);
    const bool sys = tryName("sys.glass.probe");      // system_prop
    tryName("debug.glass.probe");                      // debug_prop
    return sys ? 0 : 1;
}
