#include "elf_syms.h"

#include <elf.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace glass {
namespace {

// Every read from the image goes through this. The file is untrusted input
// being parsed inside SurfaceFlinger; an out-of-range read is a soft reboot.
inline bool inRange(size_t base, size_t off, size_t len, size_t total) {
    if (off > total) return false;
    if (len > total - off) return false;
    (void)base;
    return true;
}

bool readWholeFile(const std::string& path, std::vector<uint8_t>* out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamsize n = f.tellg();
    if (n <= 0) return false;
    out->resize(static_cast<size_t>(n));
    f.seekg(0);
    return static_cast<bool>(f.read(reinterpret_cast<char*>(out->data()), n));
}

}  // namespace

// ---------------------------------------------------------------------- ElfView

const void* ElfView::shdr(uint16_t i) const {
    if (i >= shnum_ || shdrs_ == nullptr) return nullptr;
    return reinterpret_cast<const uint8_t*>(shdrs_) + static_cast<size_t>(i) * sizeof(Elf64_Shdr);
}

const char* ElfView::sectionName(const void* sh) const {
    if (sh == nullptr || shstr_ == nullptr) return nullptr;
    const auto* s = static_cast<const Elf64_Shdr*>(sh);
    if (s->sh_name >= shstr_size_) return nullptr;
    return shstr_ + s->sh_name;
}

bool ElfView::parse(const uint8_t* data, size_t size) {
    base_ = nullptr;
    ehdr_ = nullptr;
    shdrs_ = nullptr;
    shstr_ = nullptr;
    shnum_ = 0;

    if (data == nullptr || size < sizeof(Elf64_Ehdr)) return false;
    const auto* eh = reinterpret_cast<const Elf64_Ehdr*>(data);
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0) return false;
    if (eh->e_ident[EI_CLASS] != ELFCLASS64) return false;
    if (eh->e_ident[EI_DATA] != ELFDATA2LSB) return false;
    if (eh->e_shentsize != sizeof(Elf64_Shdr)) return false;
    if (eh->e_shnum == 0 || eh->e_shoff == 0) return false;

    const size_t sh_bytes = static_cast<size_t>(eh->e_shnum) * sizeof(Elf64_Shdr);
    if (!inRange(0, eh->e_shoff, sh_bytes, size)) return false;

    const auto* shs = reinterpret_cast<const Elf64_Shdr*>(data + eh->e_shoff);

    // Section-header string table, needed to resolve section names.
    if (eh->e_shstrndx >= eh->e_shnum) return false;
    const Elf64_Shdr& strsec = shs[eh->e_shstrndx];
    if (strsec.sh_type == SHT_NOBITS) return false;
    if (!inRange(0, strsec.sh_offset, strsec.sh_size, size)) return false;

    base_ = data;
    size_ = size;
    ehdr_ = eh;
    shdrs_ = shs;
    shnum_ = eh->e_shnum;
    shstr_ = reinterpret_cast<const char*>(data + strsec.sh_offset);
    shstr_size_ = strsec.sh_size;
    return true;
}

const uint8_t* ElfView::section(const char* name, size_t* out_size) const {
    if (out_size) *out_size = 0;
    if (!ok() || name == nullptr) return nullptr;

    for (uint16_t i = 0; i < shnum_; ++i) {
        const auto* s = static_cast<const Elf64_Shdr*>(shdr(i));
        if (s == nullptr) continue;
        const char* n = sectionName(s);
        if (n == nullptr || strcmp(n, name) != 0) continue;
        if (s->sh_type == SHT_NOBITS) return nullptr;
        if (!inRange(0, s->sh_offset, s->sh_size, size_)) return nullptr;
        if (out_size) *out_size = s->sh_size;
        return base_ + s->sh_offset;
    }
    return nullptr;
}

bool ElfView::collectFunctions(const char* symtab_name,
                               std::unordered_map<std::string, Symbol>* out) const {
    if (!ok() || out == nullptr) return false;

    for (uint16_t i = 0; i < shnum_; ++i) {
        const auto* s = static_cast<const Elf64_Shdr*>(shdr(i));
        if (s == nullptr) continue;
        const char* n = sectionName(s);
        if (n == nullptr || strcmp(n, symtab_name) != 0) continue;
        if (s->sh_type == SHT_NOBITS || s->sh_entsize != sizeof(Elf64_Sym)) continue;
        if (!inRange(0, s->sh_offset, s->sh_size, size_)) continue;

        // sh_link names the string table this symbol table uses. Two symbol
        // tables in one image can point at different string tables, so this
        // must be followed rather than assuming ".strtab".
        if (s->sh_link >= shnum_) continue;
        const auto* strs = static_cast<const Elf64_Shdr*>(shdr(static_cast<uint16_t>(s->sh_link)));
        if (strs == nullptr || strs->sh_type == SHT_NOBITS) continue;
        if (!inRange(0, strs->sh_offset, strs->sh_size, size_)) continue;

        const char* strtab = reinterpret_cast<const char*>(base_ + strs->sh_offset);
        const size_t strtab_size = strs->sh_size;
        const auto* syms = reinterpret_cast<const Elf64_Sym*>(base_ + s->sh_offset);
        const size_t count = s->sh_size / sizeof(Elf64_Sym);

        for (size_t k = 0; k < count; ++k) {
            const Elf64_Sym& sym = syms[k];
            if (ELF64_ST_TYPE(sym.st_info) != STT_FUNC) continue;
            if (sym.st_value == 0) continue;
            if (sym.st_name == 0 || sym.st_name >= strtab_size) continue;

            const char* nm = strtab + sym.st_name;
            // Guard against an unterminated string table.
            const size_t max_len = strtab_size - sym.st_name;
            const size_t len = strnlen(nm, max_len);
            if (len == 0 || len == max_len) continue;

            out->emplace(std::string(nm, len), Symbol{sym.st_value, sym.st_size});
        }
        return true;
    }
    return false;
}

// --------------------------------------------------------------- SymbolResolver

bool SymbolResolver::loadFromFile(const std::string& path, InflateFn inflate, void* user) {
    syms_.clear();
    error_.clear();

    std::vector<uint8_t> image;
    if (!readWholeFile(path, &image)) {
        error_ = "cannot read " + path;
        return false;
    }

    ElfView outer;
    if (!outer.parse(image.data(), image.size())) {
        error_ = "not a valid ELF64: " + path;
        return false;
    }

    // An unstripped or partially stripped build resolves without MiniDebugInfo.
    outer.collectFunctions(".symtab", &syms_);
    outer.collectFunctions(".dynsym", &syms_);

    size_t mdi_size = 0;
    const uint8_t* mdi = outer.section(".gnu_debugdata", &mdi_size);
    if (mdi != nullptr && mdi_size > 0 && inflate != nullptr) {
        std::vector<uint8_t> inner_bytes;
        if (inflate(mdi, mdi_size, &inner_bytes, user)) {
            ElfView inner;
            if (inner.parse(inner_bytes.data(), inner_bytes.size())) {
                // MiniDebugInfo keeps exactly the symbols the outer .dynsym
                // lacks, which is precisely the set we need.
                inner.collectFunctions(".symtab", &syms_);
            } else {
                error_ = "decompressed .gnu_debugdata is not ELF64";
            }
        } else {
            error_ = "failed to decompress .gnu_debugdata";
        }
    } else if (mdi == nullptr) {
        // Not fatal: some builds ship symbols directly.
        if (syms_.empty()) error_ = "no .gnu_debugdata and no usable symbol table";
    }

    return !syms_.empty();
}

Symbol SymbolResolver::find(const std::string& mangled) const {
    auto it = syms_.find(mangled);
    return it == syms_.end() ? Symbol{} : it->second;
}

std::vector<std::string> SymbolResolver::findByPrefix(const std::string& prefix) const {
    std::vector<std::string> hits;
    for (const auto& [name, sym] : syms_) {
        if (name.size() >= prefix.size() && name.compare(0, prefix.size(), prefix) == 0) {
            hits.push_back(name);
        }
    }
    return hits;
}

// ----------------------------------------------------------------- process info

uintptr_t moduleLoadBias(const char* path_substring, bool* found) {
    if (found) *found = false;
    if (path_substring == nullptr) return 0;

    std::ifstream maps("/proc/self/maps");
    if (!maps) return 0;

    std::string line;
    while (std::getline(maps, line)) {
        if (line.find(path_substring) == std::string::npos) continue;

        // The bias is the load address of the *first* mapping of the module,
        // which for a PIE with a zero-based first PT_LOAD is that address. Later
        // mappings of the same file have non-zero file offsets, so require
        // offset 0 to avoid mistaking a middle segment for the base.
        unsigned long long start = 0, end = 0, offset = 0;
        char perms[8] = {0};
        if (sscanf(line.c_str(), "%llx-%llx %7s %llx", &start, &end, perms, &offset) != 4) continue;
        if (offset != 0) continue;

        if (found) *found = true;
        return static_cast<uintptr_t>(start);
    }
    return 0;
}

std::string selfExePath() {
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    buf[n] = '\0';
    return std::string(buf);
}

}  // namespace glass
