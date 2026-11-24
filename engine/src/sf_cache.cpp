#include "sf_cache.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace glass {
namespace sf {
namespace {

// Format 2 added the libraries (lib lines, and a module field on each entry).
// A format 1 cache is refused like any stale one, and the engine resolves.
constexpr const char kMagic[] = "stockglass-symcache 2";
constexpr size_t kMaxLines = 1024;

std::string toHex(const uint8_t* p, size_t n) {
    static const char kHex[] = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s.push_back(kHex[p[i] >> 4]);
        s.push_back(kHex[p[i] & 15]);
    }
    return s;
}

int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool fromHex(const char* s, std::vector<uint8_t>* out) {
    out->clear();
    const size_t n = strlen(s);
    if (n == 0 || n % 2 != 0) return false;
    for (size_t i = 0; i < n; i += 2) {
        const int hi = hexNibble(s[i]), lo = hexNibble(s[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out->push_back(static_cast<uint8_t>(hi << 4 | lo));
    }
    return true;
}

// One file the cached addresses belong to: [0] the binary, then each library
// (Resolution::modules), with its executable segments read from disk.
struct ModuleInfo {
    std::string path;
    std::vector<ExecSegment> segs;
};

// Compares recorded code bytes with the code at a link-time address of one
// module, wherever the caller's copy of that code is (this process, or the
// files). Called only after every module checked out.
using CodeCheck = bool (*)(const std::vector<ModuleInfo>& modules, size_t module,
                           uint64_t vaddr, const std::vector<uint8_t>& want, void* user);

// Before a CodeCheck: lets the caller look at the modules (the process finds
// each one's load bias). False with *why refuses the cache.
using ModulesReady = bool (*)(const std::vector<ModuleInfo>& modules, void* user,
                              std::string* why);

bool readCommon(const std::string& cache_path, const std::string& exe_path, ModulesReady ready,
                CodeCheck check, void* user, Resolution* out, std::string* why) {
    FILE* f = fopen(cache_path.c_str(), "re");
    if (f == nullptr) {
        *why = "no cache at " + cache_path;
        return false;
    }
    std::vector<std::string> lines;
    char buf[1024];
    while (fgets(buf, sizeof(buf), f) != nullptr && lines.size() < kMaxLines) {
        size_t n = strlen(buf);
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';
        lines.emplace_back(buf);
    }
    fclose(f);
    if (lines.empty() || lines[0] != kMagic) {
        *why = "not a cache this engine reads";
        return false;
    }

    uint64_t table = 0;
    std::string build_id;
    bool have_table = false, have_end = false;
    std::vector<ModuleInfo> modules(1, ModuleInfo{exe_path, {}});
    std::vector<std::string> lib_ids(1);
    for (const std::string& l : lines) {
        if (l.compare(0, 6, "table ") == 0) {
            table = strtoull(l.c_str() + 6, nullptr, 16);
            have_table = true;
        } else if (l.compare(0, 9, "build-id ") == 0) {
            build_id = l.substr(9);
        } else if (l.compare(0, 4, "lib ") == 0) {
            // lib <module> <build id> <absolute path>, numbered 1 and up in order.
            unsigned m = 0;
            char id[130] = {0}, path[600] = {0};
            if (sscanf(l.c_str(), "lib %u %129s %599s", &m, id, path) != 3 ||
                m != modules.size() || m > 254 || path[0] != '/') {
                *why = "malformed library: " + l;
                return false;
            }
            modules.push_back(ModuleInfo{path, {}});
            lib_ids.emplace_back(id);
        } else if (l == "end") {
            have_end = true;
        }
    }
    if (!have_end) {
        *why = "cache is truncated";
        return false;
    }
    if (!have_table || table != tableHash()) {
        *why = "cache was written for another entry-point table (module updated?)";
        return false;
    }
    const std::string actual = readGnuBuildId(exe_path);
    if (build_id.empty() || actual.empty() || build_id != actual) {
        *why = "cache is for another binary (build id " + build_id + ", running " + actual + ")";
        return false;
    }
    // A library is the file it was at install: an OTA that changed libhwui.so
    // and not surfaceflinger still invalidates the cache.
    for (size_t m = 1; m < modules.size(); ++m) {
        const std::string on_disk = readGnuBuildId(modules[m].path);
        if (on_disk.empty() || on_disk != lib_ids[m]) {
            *why = "cache is for another " + modules[m].path + " (build id " + lib_ids[m] +
                   ", on disk " + on_disk + ")";
            return false;
        }
    }
    for (ModuleInfo& m : modules) {
        m.segs = readExecSegments(m.path);
        if (m.segs.empty()) {
            *why = "cannot read the executable segments of " + m.path;
            return false;
        }
    }
    if (ready != nullptr && !ready(modules, user, why)) return false;

    *out = Resolution{};
    for (const ModuleInfo& m : modules) out->modules.push_back(m.path);
    for (const std::string& l : lines) {
        if (l.size() < 2 || l[1] != ' ' || (l[0] != 'e' && l[0] != 'd')) continue;
        if (l[0] == 'd') {
            char name[900] = {0};
            unsigned idx = 0;
            if (sscanf(l.c_str(), "d %u %899s", &idx, name) == 2 && idx < kEntryPointCount) {
                out->entry[idx].drifted = true;
                out->entry[idx].drift_name = name;
            }
            continue;
        }
        unsigned idx = 0, module = 0;
        unsigned long long value = 0, size = 0;
        char source[16] = {0}, prologue[2 * kCachePrologue + 2] = {0};
        if (sscanf(l.c_str(), "e %u %u %llx %llx %15s %33s", &idx, &module, &value, &size,
                   source, prologue) != 6 ||
            idx >= kEntryPointCount || value == 0 || module >= modules.size()) {
            *why = "malformed entry: " + l;
            return false;
        }
        std::vector<uint8_t> want;
        if (!fromHex(prologue, &want) || want.size() > kCachePrologue ||
            (size != 0 && want.size() > size)) {
            *why = "malformed code bytes: " + l;
            return false;
        }
        if (!inExecSegment(modules[module].segs, value, want.size())) {
            *why = std::string("address outside the code of ") + modules[module].path + ": " +
                   kEntryPoints[idx].label;
            return false;
        }
        if (!check(modules, module, value, want, user)) {
            *why = std::string("code differs from the cache at ") + kEntryPoints[idx].label;
            return false;
        }
        Resolved& r = out->entry[idx];
        r.sym = Symbol{value, size, SymbolOrigin::kUnknown};
        r.sym.module = static_cast<uint8_t>(module);
        r.source = strcmp(source, "scan") == 0 ? Source::kScan : Source::kCache;
        r.drifted = false;
    }
    judge(out);
    return true;
}

// ---- against this process: the mapped code, at each module's load bias
struct MemoryCtx {
    uintptr_t exe_bias;
    std::vector<uintptr_t> biases;  // by module, filled by memoryReady
};

bool memoryReady(const std::vector<ModuleInfo>& modules, void* user, std::string* why) {
    auto* ctx = static_cast<MemoryCtx*>(user);
    ctx->biases.assign(1, ctx->exe_bias);
    for (size_t m = 1; m < modules.size(); ++m) {
        bool found = false;
        const uintptr_t b = moduleLoadBias(modules[m].path.c_str(), &found);
        if (!found) {
            *why = modules[m].path + " is not loaded in this process";
            return false;
        }
        ctx->biases.push_back(b);
    }
    return true;
}

bool memoryCheck(const std::vector<ModuleInfo>&, size_t module, uint64_t vaddr,
                 const std::vector<uint8_t>& want, void* user) {
    const auto* ctx = static_cast<const MemoryCtx*>(user);
    if (module >= ctx->biases.size()) return false;
    return memcmp(reinterpret_cast<const void*>(ctx->biases[module] + vaddr), want.data(),
                  want.size()) == 0;
}

// ---- against the files on disk
bool fileCheck(const std::vector<ModuleInfo>& modules, size_t module, uint64_t vaddr,
               const std::vector<uint8_t>& want, void*) {
    const ModuleInfo& m = modules[module];
    std::vector<uint8_t> have(want.size());
    return readCodeAt(m.path, m.segs, vaddr, have.size(), have.data()) && have == want;
}

}  // namespace

bool writeCache(const std::string& cache_path, const std::string& exe_path, const Resolution& r,
                std::string* error) {
    if (!r.ok) {
        *error = "the resolution was not judged ok; refusing to cache it";
        return false;
    }
    const std::string build_id = readGnuBuildId(exe_path);
    if (build_id.empty()) {
        *error = "the binary has no GNU build id to key a cache on";
        return false;
    }
    // [0] is the binary; the resolution's own list names the libraries.
    std::vector<ModuleInfo> modules(1, ModuleInfo{exe_path, {}});
    for (size_t m = 1; m < r.modules.size(); ++m) modules.push_back(ModuleInfo{r.modules[m], {}});
    std::string body = kMagic;
    char line[1200];
    snprintf(line, sizeof(line), "\ntable %016" PRIx64 "\nbuild-id %s\n", tableHash(),
             build_id.c_str());
    body += line;
    for (size_t m = 0; m < modules.size(); ++m) {
        modules[m].segs = readExecSegments(modules[m].path);
        if (modules[m].segs.empty()) {
            *error = "cannot read the executable segments of " + modules[m].path;
            return false;
        }
        if (m == 0) continue;
        const std::string id = readGnuBuildId(modules[m].path);
        if (id.empty() || modules[m].path.empty() || modules[m].path[0] != '/' ||
            modules[m].path.find(' ') != std::string::npos) {
            *error = modules[m].path + " cannot be keyed (no build id, or not an absolute path)";
            return false;
        }
        snprintf(line, sizeof(line), "lib %zu %s %s\n", m, id.c_str(), modules[m].path.c_str());
        body += line;
    }
    for (size_t i = 0; i < kEntryPointCount; ++i) {
        const Resolved& e = r.entry[i];
        if (e.drifted && !e.sym.valid()) {
            snprintf(line, sizeof(line), "d %zu %s\n", i, e.drift_name.c_str());
            body += line;
        }
        if (!e.sym.valid()) continue;
        if (e.sym.module >= modules.size()) {
            *error = std::string("no file for the module of ") + kEntryPoints[i].label;
            return false;
        }
        const ModuleInfo& mod = modules[e.sym.module];
        const size_t n = static_cast<size_t>(
            e.sym.size < kCachePrologue ? (e.sym.size == 0 ? 4 : e.sym.size) : kCachePrologue);
        uint8_t code[kCachePrologue];
        if (!readCodeAt(mod.path, mod.segs, e.sym.value, n, code)) {
            *error = std::string("cannot read the code of ") + kEntryPoints[i].label;
            return false;
        }
        snprintf(line, sizeof(line), "e %zu %u %" PRIx64 " %" PRIx64 " %s %s %s\n", i,
                 static_cast<unsigned>(e.sym.module), e.sym.value, e.sym.size,
                 e.source == Source::kScan ? "scan" : "symbols", toHex(code, n).c_str(),
                 kEntryPoints[i].label);
        body += line;
    }
    body += "end\n";

    const std::string tmp = cache_path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "we");
    if (f == nullptr) {
        *error = "cannot write " + tmp;
        return false;
    }
    const bool wrote = fwrite(body.data(), 1, body.size(), f) == body.size();
    const bool closed = fclose(f) == 0;
    if (!wrote || !closed || rename(tmp.c_str(), cache_path.c_str()) != 0) {
        unlink(tmp.c_str());
        *error = "cannot write " + cache_path;
        return false;
    }
    chmod(cache_path.c_str(), 0644);
    return true;
}

bool readCacheForProcess(const std::string& cache_path, const std::string& exe_path,
                         uintptr_t bias, Resolution* out, std::string* why) {
    MemoryCtx ctx{bias, {}};
    return readCommon(cache_path, exe_path, memoryReady, memoryCheck, &ctx, out, why);
}

bool readCacheForFile(const std::string& cache_path, const std::string& exe_path,
                      Resolution* out, std::string* why) {
    return readCommon(cache_path, exe_path, nullptr, fileCheck, nullptr, out, why);
}

std::string cachePathFor(const std::string& library_path) {
    const std::string suffix = ".so";
    if (library_path.size() > suffix.size() &&
        library_path.compare(library_path.size() - suffix.size(), suffix.size(), suffix) == 0) {
        return library_path.substr(0, library_path.size() - suffix.size()) + ".symcache";
    }
    return library_path + ".symcache";
}

}  // namespace sf
}  // namespace glass
