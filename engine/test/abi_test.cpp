// Host test for abi_check: does each by-value-returning entry point take its
// result address in x8? Runs the installer's file-based check against a real
// arm64 surfaceflinger pulled off a device.
//
//   ./abi_test <surfaceflinger> [label=x8|notx8|undetermined ...]
//
// Prints every by-value entry point that resolves; each label=expectation
// given must match, or the exit status is 1.

#include "../src/abi_check.h"
#include "../src/elf_syms.h"
#include "../src/sf_symbols.h"

#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

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

const char* shortName(glass::ResultRegister r) {
    switch (r) {
        case glass::ResultRegister::kX8: return "x8";
        case glass::ResultRegister::kNotX8: return "notx8";
        default: return "undetermined";
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <surfaceflinger> [label=x8|notx8|undetermined ...]\n", argv[0]);
        return 2;
    }
    std::map<std::string, std::string> expect;
    for (int a = 2; a < argc; ++a) {
        const char* eq = strrchr(argv[a], '=');
        if (eq == nullptr) return 2;
        expect[std::string(argv[a], eq - argv[a])] = eq + 1;
    }

    glass::SymbolResolver r;
    if (!r.loadFromFile(argv[1], inflateViaXz, nullptr)) {
        printf("FAIL  %s\n", r.error().c_str());
        return 2;
    }
    int bad = 0;
    for (size_t i = 0; i < glass::sf::kEntryPointCount; ++i) {
        const glass::sf::EntryPoint& ep = glass::sf::kEntryPoints[i];
        if (!ep.result_in_memory) continue;
        glass::Symbol hit{};
        for (const char* const* m = ep.manglings; *m != nullptr; ++m) {
            hit = r.find(*m);
            if (hit.valid()) break;
        }
        if (!hit.valid()) {
            printf("  %-48s absent\n", ep.label);
            continue;
        }
        glass::ResultRegister rr{};
        std::string detail;
        int machine = 0;
        if (!glass::aarch64ResultRegisterInFile(argv[1], hit.value, hit.size, &rr, &detail,
                                                &machine)) {
            printf("  %-48s not checked (e_machine %d)\n", ep.label, machine);
            continue;
        }
        const auto e = expect.find(ep.label);
        const bool mismatch = e != expect.end() && e->second != shortName(rr);
        if (mismatch) ++bad;
        printf("  %-48s %-12s %s%s\n", ep.label, glass::resultRegisterName(rr), detail.c_str(),
               mismatch ? ("   <-- expected " + e->second).c_str() : "");
        if (e != expect.end()) expect.erase(e);
    }
    for (const auto& kv : expect) {
        printf("  %-48s expected %s but it did not resolve\n", kv.first.c_str(), kv.second.c_str());
        ++bad;
    }
    printf("%s\n", bad ? "FAIL" : "OK");
    return bad ? 1 : 0;
}
