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

constexpr const char kMagic[] = "stockglass-symcache 1";
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

// Compares recorded code bytes with the code at a link-time address, wherever
// the caller's copy of that code is (this process, or the file).
using CodeCheck = bool (*)(uint64_t vaddr, const std::vector<uint8_t>& want, void* user);

bool readCommon(const std::string& cache_path, const std::string& exe_path, CodeCheck check,
                void* user, Resolution* out, std::string* why) {
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
    for (const std::string& l : lines) {
        if (l.compare(0, 6, "table ") == 0) {
            table = strtoull(l.c_str() + 6, nullptr, 16);
            have_table = true;
        } else if (l.compare(0, 9, "build-id ") == 0) {
            build_id = l.substr(9);
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
    const std::vector<ExecSegment> segs = readExecSegments(exe_path);
    if (segs.empty()) {
        *why = "cannot read the binary's executable segments";
        return false;
    }

    *out = Resolution{};
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
        unsigned idx = 0;
        unsigned long long value = 0, size = 0;
        char source[16] = {0}, prologue[2 * kCachePrologue + 2] = {0};
        if (sscanf(l.c_str(), "e %u %llx %llx %15s %33s", &idx, &value, &size, source,
                   prologue) != 5 ||
            idx >= kEntryPointCount || value == 0) {
            *why = "malformed entry: " + l;
            return false;
        }
        std::vector<uint8_t> want;
        if (!fromHex(prologue, &want) || want.size() > kCachePrologue ||
            (size != 0 && want.size() > size)) {
            *why = "malformed code bytes: " + l;
            return false;
        }
        if (!inExecSegment(segs, value, want.size())) {
            *why = std::string("address outside the binary's code: ") + kEntryPoints[idx].label;
            return false;
        }
        if (!check(value, want, user)) {
            *why = std::string("code differs from the cache at ") + kEntryPoints[idx].label;
            return false;
        }
        Resolved& r = out->entry[idx];
        r.sym = Symbol{value, size, SymbolOrigin::kUnknown};
        r.source = strcmp(source, "scan") == 0 ? Source::kScan : Source::kCache;
        r.drifted = false;
    }
    judge(out);
    return true;
}

bool memoryCheck(uint64_t vaddr, const std::vector<uint8_t>& want, void* user) {
    const uintptr_t bias = *static_cast<const uintptr_t*>(user);
    return memcmp(reinterpret_cast<const void*>(bias + vaddr), want.data(), want.size()) == 0;
}

struct FileCheckCtx {
    const std::string* path;
    const std::vector<ExecSegment>* segs;
};

bool fileCheck(uint64_t vaddr, const std::vector<uint8_t>& want, void* user) {
    const auto* ctx = static_cast<const FileCheckCtx*>(user);
    std::vector<uint8_t> have(want.size());
    return readCodeAt(*ctx->path, *ctx->segs, vaddr, have.size(), have.data()) && have == want;
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
    const std::vector<ExecSegment> segs = readExecSegments(exe_path);
    if (segs.empty()) {
        *error = "cannot read the binary's executable segments";
        return false;
    }

    std::string body = kMagic;
    char line[1200];
    snprintf(line, sizeof(line), "\ntable %016" PRIx64 "\nbuild-id %s\n", tableHash(),
             build_id.c_str());
    body += line;
    for (size_t i = 0; i < kEntryPointCount; ++i) {
        const Resolved& e = r.entry[i];
        if (e.drifted && !e.sym.valid()) {
            snprintf(line, sizeof(line), "d %zu %s\n", i, e.drift_name.c_str());
            body += line;
        }
        if (!e.sym.valid()) continue;
        const size_t n = static_cast<size_t>(
            e.sym.size < kCachePrologue ? (e.sym.size == 0 ? 4 : e.sym.size) : kCachePrologue);
        uint8_t code[kCachePrologue];
        if (!readCodeAt(exe_path, segs, e.sym.value, n, code)) {
            *error = std::string("cannot read the code of ") + kEntryPoints[i].label;
            return false;
        }
        snprintf(line, sizeof(line), "e %zu %" PRIx64 " %" PRIx64 " %s %s %s\n", i, e.sym.value,
                 e.sym.size, e.source == Source::kScan ? "scan" : "symbols",
                 toHex(code, n).c_str(), kEntryPoints[i].label);
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
    uintptr_t b = bias;
    return readCommon(cache_path, exe_path, memoryCheck, &b, out, why);
}

bool readCacheForFile(const std::string& cache_path, const std::string& exe_path,
                      Resolution* out, std::string* why) {
    const std::vector<ExecSegment> segs = readExecSegments(exe_path);
    FileCheckCtx ctx{&exe_path, &segs};
    return readCommon(cache_path, exe_path, fileCheck, &ctx, out, why);
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
