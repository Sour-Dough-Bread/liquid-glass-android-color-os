// ELF64 symbol recovery from a stripped Android system binary.
//
// /system/bin/surfaceflinger is stripped: .dynsym carries ~1690 entries and not
// one RenderEngine symbol, so dlsym can never find BlurFilter::drawBlurRegion.
// What it does carry is .gnu_debugdata — MiniDebugInfo — an LZMA-compressed ELF
// holding a full .symtab. On Android 16 that is 22,669 FUNC symbols with sizes.
//
// Everything here parses a file we do not control, inside SurfaceFlinger, where
// a segfault is a soft reboot. Every offset is bounds-checked against the image
// size before use, and no parse failure is fatal — callers get false/nullopt and
// fall through to stock behaviour.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace glass {

// Which table a symbol came out of. Reported, never decided on: a name is a
// name wherever it was found.
enum class SymbolOrigin : uint8_t {
    kUnknown = 0,
    kSymtab,         // the binary's own .symtab (unstripped or partly stripped)
    kDynsym,         // .dynsym, the exported symbols
    kMiniDebugInfo,  // the .symtab inside .gnu_debugdata
};
const char* symbolOriginName(SymbolOrigin o);

struct Symbol {
    uint64_t value = 0;   // link-time address; add the module load bias for runtime
    uint64_t size = 0;
    SymbolOrigin origin = SymbolOrigin::kUnknown;
    bool valid() const { return value != 0; }
};

// Read-only view over an ELF64 image already in memory. Does not own the bytes.
class ElfView {
public:
    ElfView() = default;

    // Validates the header and section table. Returns false on anything
    // malformed, truncated, or not ELF64-little-endian.
    bool parse(const uint8_t* data, size_t size);

    // Returns pointer+length of a section's contents, or {nullptr, 0}.
    // For SHT_NOBITS sections this returns {nullptr, 0} rather than a range
    // that has no backing bytes.
    const uint8_t* section(const char* name, size_t* out_size) const;

    // Walks a symbol table section (".symtab" or ".dynsym"), inserting every
    // STT_FUNC entry with a non-empty name into `out`. Later duplicates are
    // ignored, matching first-wins lookup.
    bool collectFunctions(const char* symtab_name, SymbolOrigin origin,
                          std::unordered_map<std::string, Symbol>* out) const;

    bool ok() const { return ehdr_ != nullptr; }

private:
    const uint8_t* base_ = nullptr;
    size_t size_ = 0;
    const void* ehdr_ = nullptr;
    const void* shdrs_ = nullptr;
    uint16_t shnum_ = 0;
    const char* shstr_ = nullptr;
    size_t shstr_size_ = 0;

    // Bounds-checked accessor for the i-th section header.
    const void* shdr(uint16_t i) const;
    const char* sectionName(const void* sh) const;
};

// Signature of an xz decompressor. Injected rather than hard-wired so the host
// test can shell out to /usr/bin/xz while the device build links the vendored
// decoder — the ELF parsing is then identical in both, which is the half that
// actually has bugs in it.
using InflateFn = bool (*)(const uint8_t* in, size_t in_size,
                           std::vector<uint8_t>* out, void* user);

// Function symbols recovered from a binary's MiniDebugInfo.
class SymbolResolver {
public:
    // Reads `path`, locates .gnu_debugdata, decompresses it via `inflate`, and
    // indexes the embedded ELF's .symtab. Falls back to indexing the outer
    // binary's own .symtab and .dynsym as well, so an unstripped or partially
    // stripped build still resolves.
    bool loadFromFile(const std::string& path, InflateFn inflate, void* user);

    // Exact match on the mangled name.
    Symbol find(const std::string& mangled) const;

    // Any symbol whose name starts with `prefix`. Used only to tell "the method
    // moved or changed signature" apart from "the method is gone", because the
    // mangled name encodes the parameter types — a prefix hit with no exact hit
    // means the ABI drifted and the hook must not be installed.
    std::vector<std::string> findByPrefix(const std::string& prefix) const;

    size_t count() const { return syms_.size(); }
    const std::string& error() const { return error_; }
    // Whether the image carried MiniDebugInfo at all, for reports: a build
    // without it is one that only an install-time scan can bring up.
    bool hadMiniDebugInfo() const { return had_mdi_; }

private:
    std::unordered_map<std::string, Symbol> syms_;
    std::string error_;
    bool had_mdi_ = false;
};

// The GNU build ID of an ELF file (NT_GNU_BUILD_ID in a PT_NOTE segment), as
// lowercase hex. Reads only the headers and the note segments, never the whole
// file, so it is cheap enough for SurfaceFlinger's start-up. Empty when the
// file has none or is not an ELF64 the parser accepts.
std::string readGnuBuildId(const std::string& path);

// The AArch64 features an ELF64 file was built with: the value of
// GNU_PROPERTY_AARCH64_FEATURE_1_AND in its PT_GNU_PROPERTY segment, the one
// place the kernel and bionic's linker look. With the BTI bit set, on a CPU
// that implements BTI, the linker maps the file's code guarded: an indirect
// branch into it must land on a `bti` or `paciasp`. Headers and that segment
// only, like readGnuBuildId. False, with *features 0, when there is none.
constexpr uint32_t kAarch64FeatureBti = 1u << 0;
constexpr uint32_t kAarch64FeaturePac = 1u << 1;
bool readAarch64Features(const std::string& path, uint32_t* features);

// The executable PT_LOAD segments of an ELF64 file: where the link-time
// addresses of code live, and at which file offsets. Headers only, like
// readGnuBuildId. Empty on any parse failure.
struct ExecSegment {
    uint64_t vaddr = 0;
    uint64_t filesz = 0;
    uint64_t offset = 0;
};
std::vector<ExecSegment> readExecSegments(const std::string& path);

// Whether [vaddr, vaddr + n) lies inside one executable segment. Anything
// outside one is never read: in memory that could be an unmapped page.
bool inExecSegment(const std::vector<ExecSegment>& segs, uint64_t vaddr, size_t n);

// Copies n bytes of code at a link-time address out of the file. False unless
// the whole range lies in one executable segment and the read is complete.
bool readCodeAt(const std::string& path, const std::vector<ExecSegment>& segs, uint64_t vaddr,
                size_t n, uint8_t* out);

// Load bias of a mapped module, from /proc/self/maps. Runtime address of a
// symbol is bias + Symbol::value.
//
// Returns 0 and sets *found=false if no mapping matches. Note 0 is also a legal
// bias for a non-PIE image, hence the separate flag.
uintptr_t moduleLoadBias(const char* path_substring, bool* found);

// Absolute path of the main executable, e.g. /system/bin/surfaceflinger.
std::string selfExePath();

}  // namespace glass
