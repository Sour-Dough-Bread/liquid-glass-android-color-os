// Host test for the resolver, the judging rules and the install-time cache.
//
// Runs against real surfaceflinger binaries pulled out of firmware, on the
// host, where a parsing bug is an assertion instead of a soft reboot. It goes
// through the same sf::resolve() and sf::judge() as glasscheck and the engine,
// so a PASS here is the decision the device will make.
//
//   ./resolve_test <surfaceflinger-binary> [expected-drawBlurRegion-hex]
//
// Besides resolving, it writes the install-time cache for the binary, reads it
// back the way the engine would (against the file instead of memory), and
// checks that a tampered cache is refused.

#include "../src/elf_syms.h"
#include "../src/sf_cache.h"
#include "../src/sf_resolve.h"
#include "../src/sf_symbols.h"

#include <unistd.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
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

std::string slurp(const std::string& path) {
    std::ifstream f(path);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

void spit(const std::string& path, const std::string& body) {
    std::ofstream f(path, std::ios::trunc);
    f << body;
}

bool sameEntries(const glass::sf::Resolution& a, const glass::sf::Resolution& b) {
    for (size_t i = 0; i < glass::sf::kCount; ++i) {
        if (a.entry[i].sym.value != b.entry[i].sym.value) return false;
        if (a.entry[i].sym.size != b.entry[i].sym.size) return false;
        if (a.entry[i].sym.module != b.entry[i].sym.module) return false;
    }
    return a.shape == b.shape && a.ok == b.ok;
}

// A binary pulled from a device sits with the libraries it was pulled with:
// the Skia library (sf_resolve.h, kSkiaLibraries) is looked for next to it
// and in a lib64/ beside it, the way the device's linker finds it in
// /system/lib64. Absolute, as the cache records it.
std::vector<std::string> pulledLibraryDirs(const std::string& path) {
    char* real = realpath(path.c_str(), nullptr);
    std::string dir = real != nullptr ? real : path;
    free(real);
    const size_t slash = dir.rfind('/');
    dir = slash == std::string::npos ? "." : dir.substr(0, slash);
    return {dir, dir + "/lib64"};
}

bool load(glass::SymbolResolver* r, const std::string& path, std::vector<std::string>* report) {
    if (!r->loadFromFile(path, inflateViaXz, nullptr)) return false;
    *report = glass::sf::addSkiaLibraries(r, pulledLibraryDirs(path));
    return true;
}

void* probeUser(const glass::SymbolResolver& r) {
    return const_cast<std::vector<std::string>*>(&r.modules());
}

}  // namespace

// One line per binary, for engine/test/corpus.sh: the verdict, the shape, what
// the symbols held, and the first reason when it fails.
int brief(const std::string& path) {
    glass::SymbolResolver r;
    std::vector<std::string> libs;
    if (!load(&r, path, &libs)) {
        printf("FAIL  no symbols (%s)\n", r.error().c_str());
        return 1;
    }
    glass::sf::Resolution res;
    glass::sf::resolve(r, glass::sf::fileAbiProbe, probeUser(r), &res);
    std::string drift;
    for (size_t i = 0; i < glass::sf::kEntryPointCount; ++i) {
        if (!res.entry[i].drifted) continue;
        drift += drift.empty() ? "" : ",";
        drift += glass::sf::kEntryPoints[i].label;
    }
    printf("%s  %-36s syms=%zu mdi=%s regen=%s subset=%s%s%s%s%s\n", res.ok ? "PASS" : "FAIL",
           res.shape >= 0 ? glass::sf::kHookShapes[res.shape].label : "no hook shape",
           res.symbol_count, res.had_minidebuginfo ? "yes" : "no",
           res.canRegenerateBlur() ? "yes" : "no", res.canCopySubset() ? "yes" : "no",
           drift.empty() ? "" : " drift=", drift.c_str(), res.fatal.empty() ? "" : " | ",
           res.fatal.empty() ? "" : res.fatal[0].c_str());
    return res.ok ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc == 3 && strcmp(argv[1], "--brief") == 0) return brief(argv[2]);
    if (argc < 2) {
        fprintf(stderr, "usage: %s <surfaceflinger> [expected-hex-addr]\n"
                        "       %s --brief <surfaceflinger>\n", argv[0], argv[0]);
        return 2;
    }
    const std::string path = argv[1];

    glass::SymbolResolver r;
    printf("== %s\n", path.c_str());
    const std::string build_id = glass::readGnuBuildId(path);
    printf("  build id %s\n", build_id.empty() ? "(none)" : build_id.c_str());
    if (!r.loadFromFile(path, inflateViaXz, nullptr)) {
        fprintf(stderr, "  load failed: %s\n", r.error().c_str());
        printf("FAILURES\n\n");
        return 1;
    }
    printf("  %zu function symbols recovered%s\n", r.count(),
           r.hadMiniDebugInfo() ? " (MiniDebugInfo present)" : " (no MiniDebugInfo)");
    check(r.count() > 10000, "recovered a plausible symbol count (>10000)");
    for (const std::string& line : glass::sf::addSkiaLibraries(&r, pulledLibraryDirs(path))) {
        printf("  %s\n", line.c_str());
    }

    glass::sf::Resolution res;
    glass::sf::resolve(r, glass::sf::fileAbiProbe, probeUser(r), &res);
    for (size_t i = 0; i < glass::sf::kEntryPointCount; ++i) {
        printf("%s\n", glass::sf::describe(res, i).c_str());
    }
    for (const std::string& n : res.notes) printf("  note   %s\n", n.c_str());
    for (const std::string& f : res.fatal) printf("  FATAL  %s\n", f.c_str());
    check(res.ok, "the engine can drive this build (judge)");
    printf("  hook target: %s\n",
           res.shape >= 0 ? glass::sf::kHookShapes[res.shape].label : "NONE");
    check(!r.find("_ZN4this6symbol4doesNotExistEv").valid(), "missing symbol reports invalid");

    // run.sh passes an empty string when no address was given; that is "don't
    // check", not "expect 0".
    if (argc >= 3 && argv[2][0] != '\0' && res.shape >= 0) {
        const uint64_t expect = strtoull(argv[2], nullptr, 16);
        const uint64_t got = res.sym(glass::sf::kHookShapes[res.shape].entry).value;
        check(got == expect, "drawBlurRegion at expected 0x%llx (got 0x%llx)",
              (unsigned long long)expect, (unsigned long long)got);
    }

    // ---- the install-time cache, round trip and refusals
    if (res.ok && !build_id.empty()) {
        char tmpl[] = "/tmp/glass_cache_XXXXXX";
        const int fd = mkstemp(tmpl);
        if (fd >= 0) close(fd);
        const std::string cache = tmpl;
        std::string err;
        check(glass::sf::writeCache(cache, path, res, &err), "cache written%s%s",
              err.empty() ? "" : ": ", err.c_str());

        glass::sf::Resolution back;
        std::string why;
        const bool read_ok = glass::sf::readCacheForFile(cache, path, &back, &why);
        check(read_ok, "cache read back and verified against the file%s%s",
              why.empty() ? "" : ": ", why.c_str());
        if (read_ok) {
            glass::sf::probeAbi(&back, glass::sf::fileAbiProbe, &back.modules);
            glass::sf::judge(&back);
            check(sameEntries(res, back), "cached resolution equals the resolved one");
        }

        const std::string body = slurp(cache);
        // A changed code byte: the first entry's prologue, last hex digit.
        {
            std::string t = body;
            const size_t e = t.find("\ne ");
            size_t field = e;
            for (int k = 0; k < 7 && field != std::string::npos; ++k) field = t.find(' ', field + 1);
            if (field != std::string::npos) {
                char& c = t[field - 1];
                c = c == '0' ? '1' : '0';
                spit(cache, t);
                glass::sf::Resolution x;
                std::string w;
                // Read first: as a sibling argument, w.c_str() could be taken
                // before the call fills w.
                const bool read = glass::sf::readCacheForFile(cache, path, &x, &w);
                check(!read, "a cache with a changed code byte is refused (%s)", w.c_str());
            }
        }
        // Another binary's build id.
        {
            std::string t = body;
            const size_t b = t.find("build-id ");
            if (b != std::string::npos) {
                char& c = t[b + 9];
                c = c == 'a' ? 'b' : 'a';
                spit(cache, t);
                glass::sf::Resolution x;
                std::string w;
                check(!glass::sf::readCacheForFile(cache, path, &x, &w),
                      "a cache for another build id is refused");
            }
        }
        // Truncated.
        {
            spit(cache, body.substr(0, body.size() / 2));
            glass::sf::Resolution x;
            std::string w;
            check(!glass::sf::readCacheForFile(cache, path, &x, &w),
                  "a truncated cache is refused");
        }
        unlink(cache.c_str());
    }

    printf("%s\n\n", failures == 0 ? "PASS" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
