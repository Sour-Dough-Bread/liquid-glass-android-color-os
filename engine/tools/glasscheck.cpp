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
#include "../src/elf_syms.h"
#include "../src/glass_params.h"
#include "../src/sf_symbols.h"
#include "../src/xz_inflate.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

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

int main(int argc, char** argv) {
    if (argc == 2 && strcmp(argv[1], "--params") == 0) return dumpParams();
    if (argc == 2 && strcmp(argv[1], "--presets") == 0) return listPresets();

    const char* path = argc >= 2 ? argv[1] : "/system/bin/surfaceflinger";
    if (argc > 2) {
        fprintf(stderr, "usage: %s [surfaceflinger] | --params | --presets\n", argv[0]);
        return 2;
    }

    glass::SymbolResolver r;
    if (!r.loadFromFile(path, glass::inflateXzAdapter, nullptr)) {
        printf("FAIL  cannot read symbols from %s\n      %s\n", path, r.error().c_str());
        return 2;
    }
    printf("%s\n  %zu function symbols recovered\n", path, r.count());

    bool core_ok = true;
    bool release_ok = false;
    int shapes = 0;
    int drift = 0;
    int generators = 0;
    bool make_wrapper_bad = false;  // MakeForShader(SkString) fails the x8 check
    bool make_opts_ok = false;      // MakeForShader(SkString, const Options&) usable

    for (size_t i = 0; i < glass::sf::kEntryPointCount; ++i) {
        const glass::sf::EntryPoint& ep = glass::sf::kEntryPoints[i];
        glass::Symbol hit{};
        for (const char* const* m = ep.manglings; *m != nullptr; ++m) {
            hit = r.find(*m);
            if (hit.valid()) break;
        }

        if (hit.valid() && ep.result_in_memory) {
            // On arm64, a by-value result must come back the standard way (x8)
            // or the engine will not call the function: same test, same answer
            // as inside SurfaceFlinger. Not applicable on x86-64.
            glass::ResultRegister rr{};
            std::string detail;
            int machine = 0;
            if (glass::aarch64ResultRegisterInFile(path, hit.value, hit.size, &rr, &detail,
                                                   &machine) &&
                rr != glass::ResultRegister::kX8) {
                if (i == glass::sf::kMakeForShader) {
                    make_wrapper_bad = true;
                    printf("  note     %s: result not taken in x8 (%s); "
                           "the engine compiles through the Options form\n", ep.label,
                           detail.c_str());
                } else if (ep.strategy == glass::sf::kCore) {
                    core_ok = false;
                    printf("  ABI      %-38s result not taken in x8 (%s)\n", ep.label,
                           detail.c_str());
                } else {
                    printf("  note     %s: result not taken in x8 (%s); not used\n", ep.label,
                           detail.c_str());
                }
                continue;
            }
        }

        if (hit.valid()) {
            if (i == glass::sf::kMakeForShaderOpts) make_opts_ok = true;
            if (ep.strategy == glass::sf::kDataRelease) release_ok = true;
            if (ep.strategy == glass::sf::kHookTarget) {
                ++shapes;
                printf("  hook     %s\n", ep.label);
            }
            if (ep.strategy == glass::sf::kBlurGenerate) {
                ++generators;
                printf("  hook     %s (optional)\n", ep.label);
            }
            continue;
        }

        // A prefix hit with no exact hit means the signature changed shape.
        // That is only dangerous where the draw depends on it.
        const bool present_but_changed =
            ep.prefix != nullptr && !r.findByPrefix(ep.prefix).empty();
        const bool required = ep.strategy == glass::sf::kCore;

        if (required) {
            core_ok = false;
            printf("  MISSING  %-38s %s\n", ep.label,
                   present_but_changed ? "(signature drifted)" : "(inlined away)");
        } else if (present_but_changed && ep.strategy == glass::sf::kDataRelease) {
            ++drift;
            printf("  DRIFT    %-38s\n", ep.label);
        }
    }

    if (shapes != 1) {
        core_ok = false;
        printf("  %s  drawBlurRegion: expected exactly one known shape, found %d\n",
               shapes == 0 ? "MISSING " : "AMBIGUOUS", shapes);
    }
    if (generators == 0) {
        printf("  note     no generateTemporaryImage: a toned backdrop keeps the radius it asked for\n");
    }
    if (make_wrapper_bad && !make_opts_ok) {
        core_ok = false;
        printf("  ABI      no SkRuntimeEffect::MakeForShader the engine can call safely\n");
    }
    if (!release_ok) {
        core_ok = false;
        printf("  MISSING  no way to release the uniform blob\n");
    }

    if (!core_ok || drift != 0) {
        printf("FAIL  this build's Skia is not one the engine can drive.\n"
               "      Nothing was installed; the device is untouched.\n");
        return 1;
    }
    printf("OK    every required entry point resolved\n");
    return 0;
}
