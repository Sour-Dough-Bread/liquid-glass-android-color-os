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

struct Symbol {
    uint64_t value = 0;   // link-time address; add the module load bias for runtime
    uint64_t size = 0;
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
    bool collectFunctions(const char* symtab_name,
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

private:
    std::unordered_map<std::string, Symbol> syms_;
    std::string error_;
};

// Load bias of a mapped module, from /proc/self/maps. Runtime address of a
// symbol is bias + Symbol::value.
//
// Returns 0 and sets *found=false if no mapping matches. Note 0 is also a legal
// bias for a non-PIE image, hence the separate flag.
uintptr_t moduleLoadBias(const char* path_substring, bool* found);

// Absolute path of the main executable, e.g. /system/bin/surfaceflinger.
std::string selfExePath();

}  // namespace glass
