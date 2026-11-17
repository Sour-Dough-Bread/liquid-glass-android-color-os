// Host test for the MiniDebugInfo symbol resolver and the entry-point table.
//
// Runs against real surfaceflinger binaries pulled off devices, on the host,
// where a parsing bug is an assertion instead of a soft reboot.
//
//   ./resolve_test <surfaceflinger-binary> [expected-drawBlurRegion-hex]

#include "../src/elf_syms.h"
#include "../src/sf_symbols.h"

#include <unistd.h>

#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

// Host-side xz: shell out. The device build swaps in a vendored decoder; the
// ELF parsing under test is identical either way.
bool inflateViaXz(const uint8_t* in, size_t in_size, std::vector<uint8_t>* out, void*) {
    char in_path[] = "/tmp/glass_mdi_XXXXXX";
    int fd = mkstemp(in_path);
    if (fd < 0) return false;
    const bool wrote = write(fd, in, in_size) == static_cast<ssize_t>(in_size);
    close(fd);
    if (!wrote) { unlink(in_path); return false; }

    const std::string cmd = std::string("xz -d -c ") + in_path;
    FILE* p = popen(cmd.c_str(), "r");
    if (p == nullptr) { unlink(in_path); return false; }

    out->clear();
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), p)) > 0) out->insert(out->end(), buf, buf + n);
    const int rc = pclose(p);
    unlink(in_path);
    return rc == 0 && !out->empty();
}

int failures = 0;

void check(bool cond, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void check(bool cond, const char* fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    printf("  [%s] %s\n", cond ? " ok " : "FAIL", msg);
    if (!cond) ++failures;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <surfaceflinger> [expected-hex-addr]\n", argv[0]);
        return 2;
    }

    glass::SymbolResolver r;
    printf("== %s\n", argv[1]);
    if (!r.loadFromFile(argv[1], inflateViaXz, nullptr)) {
        fprintf(stderr, "  load failed: %s\n", r.error().c_str());
        return 1;
    }
    printf("  %zu function symbols recovered\n", r.count());
    check(r.count() > 10000, "recovered a plausible symbol count (>10000)");

    // Resolve every entry point through its candidate list, exactly as the
    // engine will at load time, and record which strategies came out whole.
    bool strategy_ok[glass::sf::kStrategyCount];
    for (bool& ok : strategy_ok) ok = true;
    bool resolved[glass::sf::kCount] = {false};

    for (size_t i = 0; i < glass::sf::kEntryPointCount; ++i) {
        const glass::sf::EntryPoint& ep = glass::sf::kEntryPoints[i];

        glass::Symbol hit;
        const char* which = nullptr;
        for (const char* const* m = ep.manglings; *m != nullptr; ++m) {
            const glass::Symbol s2 = r.find(*m);
            if (s2.valid()) { hit = s2; which = *m; break; }
        }

        resolved[i] = hit.valid();
        if (!hit.valid() && ep.strategy != glass::sf::kDataRelease &&
            ep.strategy != glass::sf::kHookTarget) {
            strategy_ok[ep.strategy] = false;
        }

        if (hit.valid()) {
            printf("  [ ok ] %-7s %-38s 0x%-8llx %5llu B%s\n",
                   glass::sf::strategyName(ep.strategy), ep.label,
                   (unsigned long long)hit.value, (unsigned long long)hit.size,
                   which == ep.manglings[0] ? "" : "  (fallback mangling)");
            continue;
        }

        // Nothing matched. Distinguish "inlined away" from "signature drifted":
        // the second is dangerous, the first merely rules a strategy out.
        std::vector<std::string> near;
        // A hook-target variant that did not resolve is the normal case — the
        // other shape did. Its prefix necessarily matches the shape that IS
        // present, so reporting that as drift would fire on every build.
        // "exactly one shape resolved" is the check that actually matters.
        if (ep.prefix != nullptr && ep.strategy != glass::sf::kHookTarget) {
            near = r.findByPrefix(ep.prefix);
        }
        if (!near.empty()) {
            check(false, "%-7s %-38s SIGNATURE DRIFT, %zu unvetted",
                  glass::sf::strategyName(ep.strategy), ep.label, near.size());
            for (const auto& n : near) printf("           %s\n", n.c_str());
        } else {
            printf("  [ -- ] %-7s %-38s inlined away\n",
                   glass::sf::strategyName(ep.strategy), ep.label);
        }
    }

    check(strategy_ok[glass::sf::kCore], "core entry points all resolved");
    // kDataRelease is a group of alternatives: one is enough, and which one
    // survives out-of-line differs per build (A16 has both, A14 only the sk_sp
    // destructor).
    check(resolved[glass::sf::kSkSpDataDtor] || resolved[glass::sf::kDataUnref],
          "a uniform-blob release path is available");
    // Exactly one shape of the hook target: two would mean we cannot tell which
    // one the compositor actually calls.
    const int shapes = (resolved[glass::sf::kDrawBlurRegion] ? 1 : 0) +
                       (resolved[glass::sf::kDrawBlurRegionScaled] ? 1 : 0);
    check(shapes == 1, "exactly one drawBlurRegion shape resolved (%d)", shapes);
    printf("  hook target: %s\n",
           resolved[glass::sf::kDrawBlurRegion] ? "drawBlurRegion(radius, alpha)"
               : (resolved[glass::sf::kDrawBlurRegionScaled]
                      ? "drawBlurRegion(radius, scale, alpha)" : "NONE"));
    printf("  SkRuntimeEffectBuilder surface: %s (unused; the draw goes through\n"
           "  SkRuntimeEffect::makeShader, which needs no builder layout)\n",
           strategy_ok[glass::sf::kBuilder] ? "complete" : "partly inlined");

    // The hook target must have a real body; a zero size means we would have an
    // address with nothing to patch.
    const size_t which = resolved[glass::sf::kDrawBlurRegion] ? glass::sf::kDrawBlurRegion
                                                              : glass::sf::kDrawBlurRegionScaled;
    const glass::Symbol dbr = r.find(glass::sf::kEntryPoints[which].manglings[0]);
    check(dbr.size > 64, "drawBlurRegion has a real body (not inlined)");
    check(r.findByPrefix(glass::sf::kEntryPoints[glass::sf::kDrawBlurRegion].prefix).size() == 1,
          "exactly one drawBlurRegion mangling present");
    check(!r.find("_ZN4this6symbol4doesNotExistEv").valid(), "missing symbol reports invalid");

    // run.sh passes an empty string when no address was given; that is "don't
    // check", not "expect 0".
    if (argc >= 3 && argv[2][0] != '\0') {
        const uint64_t expect = strtoull(argv[2], nullptr, 16);
        check(dbr.value == expect, "drawBlurRegion at expected 0x%llx",
              (unsigned long long)expect);
    }

    printf("%s\n\n", failures == 0 ? "PASS" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
