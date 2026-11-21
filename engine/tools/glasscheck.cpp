// glasscheck — on-device preflight for the KernelSU module.
//
// Resolves every entry point the engine needs out of the target's own
// MiniDebugInfo and reports whether this ROM's Skia is one the engine can drive.
// The module runs it during installation and refuses to install if it fails, so
// an incompatible ROM costs an error message rather than a boot.
//
// This is the same resolver the engine uses at load time, and the same table.
// Running it here does not make the load-time check redundant — that one still
// has to pass — it just moves the discovery to a point where the cost of being
// wrong is a declined install.
//
//   glasscheck /system/bin/surfaceflinger
//     exit 0  every required entry point resolved
//     exit 1  something required is missing, or a signature drifted
//     exit 2  bad usage / unreadable file
//
//   glasscheck --params    the engine's parameter tables and the live
//                          persist.sys.glass.* state, as one line of JSON
//   glasscheck --presets   preset names and summaries, one per line
//
// The last two exist so the WebUI and glassctl never keep their own copy of the
// presets or the defaults: they are compiled from the same glass_params.cpp the
// engine is, so what the UI shows is what the compositor will draw.

#include "../src/abi_check.h"
#include "../src/blur_scale.h"
#include "../src/elf_syms.h"
#include "../src/glass_params.h"
#include "../src/sf_cache.h"
#include "../src/sf_resolve.h"
#include "../src/sf_symbols.h"
#include "../src/xz_inflate.h"

#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(__aarch64__)
#include <sys/auxv.h>
#ifndef HWCAP2_BTI
#define HWCAP2_BTI (1 << 17)
#endif
#endif

namespace {

// The same question the engine asks before it hooks (glass_engine.cpp,
// cpuHasBti): whether this CPU enforces BTI on guarded pages.
bool cpuHasBti() {
#if defined(__aarch64__)
    return (getauxval(AT_HWCAP2) & HWCAP2_BTI) != 0;
#else
    return false;
#endif
}

// Parameter names and summaries are compile-time constants with no quotes or
// backslashes in them, so they need no escaping; a %g float is valid JSON as
// long as it is finite, and every table value is.
void printNumber(float v) {
    if (v != v || v == 1.0f / 0.0f || v == -1.0f / 0.0f) {
        printf("null");
    } else {
        printf("%.6g", static_cast<double>(v));
    }
}

int dumpParams() {
    glass::paramsInit();
    const size_t active = glass::paramsPresetIndex();
    printf("{\"mode\":\"%s\",\"preset\":\"%s\",\"defaultPreset\":\"%s\",\"presets\":[",
           glass::paramsMode() == glass::Mode::kGlass ? "glass" : "stock",
           glass::presetAt(active).name, glass::presetAt(glass::defaultPresetIndex()).name);
    for (size_t i = 0; i < glass::presetCount(); ++i) {
        const glass::Preset& p = glass::presetAt(i);
        printf("%s{\"name\":\"%s\",\"summary\":\"%s\",\"values\":{", i ? "," : "", p.name,
               p.summary);
        for (size_t k = 0; k < p.count; ++k) {
            printf("%s\"%s\":", k ? "," : "", glass::paramName(p.entries[k].id));
            printNumber(p.entries[k].value);
        }
        printf("}}");
    }
    printf("],\"params\":[");
    for (size_t i = 0; i < glass::P_count; ++i) {
        const auto id = static_cast<glass::ParamId>(i);
        printf("%s{\"name\":\"%s\",\"default\":", i ? "," : "", glass::paramName(id));
        printNumber(glass::paramDefault(id));
        printf(",\"effective\":");
        printNumber(glass::param(id));
        printf(",\"overridden\":%s", glass::paramOverridden(id) ? "true" : "false");
        if (const char* const* names = glass::paramChoices(id)) {
            printf(",\"choices\":[");
            for (int k = 0; names[k] != nullptr; ++k) printf("%s\"%s\"", k ? "," : "", names[k]);
            printf("]");
        }
        printf("}");
    }
    printf("]}\n");
    return 0;
}

int listPresets() {
    for (size_t i = 0; i < glass::presetCount(); ++i) {
        const glass::Preset& p = glass::presetAt(i);
        printf("%s\t%s\n", p.name, p.summary);
    }
    return 0;
}

}  // namespace

namespace {

// Strings in the JSON report are mangled names, labels and fixed messages; the
// escaping covers what could still appear in them.
void jsonString(const std::string& s) {
    putchar('"');
    for (const char c : s) {
        if (c == '"' || c == '\\') {
            printf("\\%c", c);
        } else if (static_cast<unsigned char>(c) < 0x20) {
            printf("\\u%04x", c);
        } else {
            putchar(c);
        }
    }
    putchar('"');
}

std::string prop(const char* name) {
#ifdef __ANDROID__
    char buf[PROP_VALUE_MAX] = {0};
    return __system_property_get(name, buf) > 0 ? std::string(buf) : std::string();
#else
    (void)name;
    return {};
#endif
}

void printJson(const char* path, const glass::sf::Resolution& res, const std::string& build_id) {
    printf("{\"path\":");
    jsonString(path);
    printf(",\"build_id\":");
    jsonString(build_id);
    printf(",\"symbols\":%zu,\"minidebuginfo\":%s,\"ok\":%s,\"shape\":", res.symbol_count,
           res.had_minidebuginfo ? "true" : "false", res.ok ? "true" : "false");
    if (res.shape >= 0) {
        jsonString(glass::sf::kHookShapes[res.shape].label);
    } else {
        printf("null");
    }
    printf(",\"fatal\":[");
    for (size_t i = 0; i < res.fatal.size(); ++i) {
        if (i) putchar(',');
        jsonString(res.fatal[i]);
    }
    printf("],\"notes\":[");
    for (size_t i = 0; i < res.notes.size(); ++i) {
        if (i) putchar(',');
        jsonString(res.notes[i]);
    }
    printf("],\"entries\":[");
    for (size_t i = 0; i < glass::sf::kEntryPointCount; ++i) {
        const glass::sf::EntryPoint& ep = glass::sf::kEntryPoints[i];
        const glass::sf::Resolved& e = res.entry[i];
        printf("%s{\"label\":", i ? "," : "");
        jsonString(ep.label);
        printf(",\"strategy\":\"%s\",\"resolved\":%s", glass::sf::strategyName(ep.strategy),
               e.sym.valid() ? "true" : "false");
        if (e.sym.valid()) {
            printf(",\"value\":\"0x%llx\",\"size\":%llu,\"source\":\"%s\"",
                   static_cast<unsigned long long>(e.sym.value),
                   static_cast<unsigned long long>(e.sym.size), glass::sf::sourceName(e.source));
        }
        if (e.drifted) {
            printf(",\"drift\":");
            jsonString(e.drift_name);
        }
        if (e.abi_rejected) {
            printf(",\"abi\":");
            jsonString(e.abi_detail);
        }
        putchar('}');
    }
    printf("]}\n");
}

int usage(const char* argv0) {
    fprintf(stderr,
            "usage: %s [--json] [--cache-out FILE] [surfaceflinger]\n"
            "       %s --verify-cache FILE [surfaceflinger]\n"
            "       %s --params | --presets\n",
            argv0, argv0, argv0);
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && strcmp(argv[1], "--params") == 0) return dumpParams();
    if (argc == 2 && strcmp(argv[1], "--presets") == 0) return listPresets();

    const char* path = "/system/bin/surfaceflinger";
    const char* cache_out = nullptr;
    const char* verify_cache = nullptr;
    bool json = false;
    bool have_path = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--json") == 0) {
            json = true;
        } else if (strcmp(argv[i], "--cache-out") == 0 && i + 1 < argc) {
            cache_out = argv[++i];
        } else if (strcmp(argv[i], "--verify-cache") == 0 && i + 1 < argc) {
            verify_cache = argv[++i];
        } else if (argv[i][0] != '-' && !have_path) {
            path = argv[i];
            have_path = true;
        } else {
            return usage(argv[0]);
        }
    }

    const std::string build_id = glass::readGnuBuildId(path);

    // Checking a cache against the file is what the engine does against
    // memory at every start; this lets the installer and the tests see the
    // same verdict.
    if (verify_cache != nullptr) {
        glass::sf::Resolution res;
        std::string why;
        if (!glass::sf::readCacheForFile(verify_cache, path, &res, &why)) {
            printf("FAIL  cache not usable: %s\n", why.c_str());
            return 1;
        }
        glass::sf::probeAbi(&res, glass::sf::fileAbiProbe, const_cast<char*>(path));
        glass::sf::judge(&res);
        printf("%s  cache %s for %s (build id %s)\n", res.ok ? "OK  " : "FAIL",
               res.ok ? "verified" : "does not add up", path, build_id.c_str());
        return res.ok ? 0 : 1;
    }

    glass::SymbolResolver r;
    if (!r.loadFromFile(path, glass::inflateXzAdapter, nullptr)) {
        if (json) {
            printf("{\"path\":");
            jsonString(path);
            printf(",\"build_id\":");
            jsonString(build_id);
            printf(",\"ok\":false,\"fatal\":[");
            jsonString("cannot read symbols: " + r.error());
            printf("]}\n");
        } else {
            printf("FAIL  cannot read symbols from %s\n      %s\n", path, r.error().c_str());
            printf("      This build carries no symbol table the engine can use.\n");
        }
        return 2;
    }

    glass::sf::Resolution res;
    glass::sf::resolve(r, glass::sf::fileAbiProbe, const_cast<char*>(path), &res);

    if (json) {
        printJson(path, res, build_id);
    } else {
        const std::string fp = prop("ro.build.fingerprint");
        if (!fp.empty()) printf("%s\n", fp.c_str());
        printf("%s\n  build id %s\n  %zu function symbols recovered%s\n", path,
               build_id.empty() ? "(none)" : build_id.c_str(), r.count(),
               res.had_minidebuginfo ? " from MiniDebugInfo" : " (no MiniDebugInfo)");
        if (res.shape >= 0) {
            const glass::Symbol& t = res.sym(glass::sf::kHookShapes[res.shape].entry);
            printf("  hook     %s at 0x%llx, %llu bytes\n", glass::sf::kHookShapes[res.shape].label,
                   static_cast<unsigned long long>(t.value),
                   static_cast<unsigned long long>(t.size));
            // What the engine will read at boot, before it hooks (blur_scale.h).
            const glass::BlurScale scale = glass::probeBlurScaleInFile(path, t);
            printf("  scale    %s\n", scale.how.c_str());
        }
        printf("  lighter backdrop blur: %s\n",
               res.canRegenerateBlur() ? "available" : "not on this build");
        uint32_t features = 0;
        if (glass::readAarch64Features(path, &features) &&
            (features & glass::kAarch64FeatureBti) != 0) {
            printf("  branch protection: BTI-built%s\n",
                   cpuHasBti() ? ", and this CPU enforces it" : "; this CPU does not enforce it");
        }
        for (const std::string& n : res.notes) printf("  note     %s\n", n.c_str());
        for (const std::string& f : res.fatal) printf("  MISSING  %s\n", f.c_str());
    }

    if (!res.ok) {
        if (!json) {
            printf("FAIL  this build's Skia is not one the engine can drive.\n"
                   "      Nothing was installed; the device is untouched.\n");
        }
        return 1;
    }

    if (cache_out != nullptr) {
        std::string err;
        if (!glass::sf::writeCache(cache_out, path, res, &err)) {
            // Not fatal: the engine resolves from symbols without a cache.
            if (!json) printf("  note     no symbol cache written: %s\n", err.c_str());
        } else if (!json) {
            printf("  symbol cache written: %s\n", cache_out);
        }
    }
    if (!json) printf("OK    every required entry point resolved\n");
    return 0;
}
