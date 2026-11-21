#include "elf_syms.h"

#include <elf.h>
#include <fcntl.h>
#include <sys/stat.h>
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
    // A regular file of a believable size only. An ifstream opens a directory
    // too, and its tellg is then a size no allocation survives: bad_alloc,
    // which inside SurfaceFlinger would be an abort.
    struct stat st {};
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        st.st_size > (512LL << 20)) {
        return false;
    }
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamsize n = f.tellg();
    if (n <= 0 || n != static_cast<std::streamsize>(st.st_size)) return false;
    out->resize(static_cast<size_t>(n));
    f.seekg(0);
    return static_cast<bool>(f.read(reinterpret_cast<char*>(out->data()), n));
}

}  // namespace

const char* symbolOriginName(SymbolOrigin o) {
    switch (o) {
        case SymbolOrigin::kSymtab: return ".symtab";
        case SymbolOrigin::kDynsym: return ".dynsym";
        case SymbolOrigin::kMiniDebugInfo: return "MiniDebugInfo";
        default: return "?";
    }
}

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

bool ElfView::collectFunctions(const char* symtab_name, SymbolOrigin origin,
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
        const uint8_t* sym_bytes = base_ + s->sh_offset;
        const size_t count = s->sh_size / sizeof(Elf64_Sym);

        for (size_t k = 0; k < count; ++k) {
            // Copied out: the symbol table inside MiniDebugInfo is often not
            // 8-byte aligned (in most builds surveyed), and a reference to a
            // misaligned Elf64_Sym is undefined behaviour even where the CPU
            // would not mind.
            Elf64_Sym sym;
            memcpy(&sym, sym_bytes + k * sizeof(Elf64_Sym), sizeof(sym));
            if (ELF64_ST_TYPE(sym.st_info) != STT_FUNC) continue;
            if (sym.st_value == 0) continue;
            if (sym.st_name == 0 || sym.st_name >= strtab_size) continue;

            const char* nm = strtab + sym.st_name;
            // Guard against an unterminated string table.
            const size_t max_len = strtab_size - sym.st_name;
            const size_t len = strnlen(nm, max_len);
            if (len == 0 || len == max_len) continue;

            out->emplace(std::string(nm, len), Symbol{sym.st_value, sym.st_size, origin});
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
    outer.collectFunctions(".symtab", SymbolOrigin::kSymtab, &syms_);
    outer.collectFunctions(".dynsym", SymbolOrigin::kDynsym, &syms_);

    size_t mdi_size = 0;
    const uint8_t* mdi = outer.section(".gnu_debugdata", &mdi_size);
    had_mdi_ = mdi != nullptr && mdi_size > 0;
    if (mdi != nullptr && mdi_size > 0 && inflate != nullptr) {
        std::vector<uint8_t> inner_bytes;
        if (inflate(mdi, mdi_size, &inner_bytes, user)) {
            ElfView inner;
            if (inner.parse(inner_bytes.data(), inner_bytes.size())) {
                // MiniDebugInfo keeps exactly the symbols the outer .dynsym
                // lacks, which is precisely the set we need.
                inner.collectFunctions(".symtab", SymbolOrigin::kMiniDebugInfo, &syms_);
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

// ------------------------------------------------------- build id, properties

namespace {

// Spelled out rather than taken from <elf.h>: older glibc and NDK headers lack
// some of them, and the values are fixed by the ABI.
constexpr uint32_t kPtGnuProperty = 0x6474e553;            // PT_GNU_PROPERTY
constexpr uint32_t kNtGnuPropertyType0 = 5;                // NT_GNU_PROPERTY_TYPE_0
constexpr uint32_t kAarch64Feature1And = 0xc0000000;       // GNU_PROPERTY_AARCH64_FEATURE_1_AND

// The program header table of an ELF64 little-endian file. Every read here is
// bounded: the table and a note segment are a few hundred bytes on every
// Android build, so anything larger is refused rather than allocated.
bool readProgramHeaders(int fd, std::vector<Elf64_Phdr>* ph) {
    Elf64_Ehdr eh{};
    if (pread(fd, &eh, sizeof(eh), 0) != static_cast<ssize_t>(sizeof(eh)) ||
        memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 || eh.e_ident[EI_CLASS] != ELFCLASS64 ||
        eh.e_ident[EI_DATA] != ELFDATA2LSB || eh.e_phentsize != sizeof(Elf64_Phdr) ||
        eh.e_phnum == 0 || eh.e_phnum > 64) {
        return false;
    }
    ph->resize(eh.e_phnum);
    const size_t bytes = ph->size() * sizeof(Elf64_Phdr);
    return pread(fd, ph->data(), bytes, static_cast<off_t>(eh.e_phoff)) ==
           static_cast<ssize_t>(bytes);
}

bool readSegment(int fd, const Elf64_Phdr& p, size_t limit, std::vector<uint8_t>* out) {
    if (p.p_filesz == 0 || p.p_filesz > limit) return false;
    out->resize(p.p_filesz);
    return pread(fd, out->data(), out->size(), static_cast<off_t>(p.p_offset)) ==
           static_cast<ssize_t>(out->size());
}

struct Note {
    uint32_t type;
    bool gnu;  // named "GNU"
    const uint8_t* desc;
    uint32_t descsz;
};

// Each note of a note segment, until fn returns true. An Elf64_Nhdr (namesz,
// descsz, type) is followed by the name and then the descriptor, each starting
// on the segment's alignment: 4 for most notes, 8 for an ELF64
// .note.gnu.property. For a "GNU" name both put the descriptor at +16.
template <typename Fn>
void forEachNote(const std::vector<uint8_t>& seg, size_t align, Fn fn) {
    const auto up = [align](size_t n) { return (n + align - 1) & ~(align - 1); };
    size_t at = 0;
    while (at <= seg.size() && seg.size() - at >= 12) {
        uint32_t namesz, descsz, type;
        memcpy(&namesz, &seg[at], 4);
        memcpy(&descsz, &seg[at + 4], 4);
        memcpy(&type, &seg[at + 8], 4);
        const size_t name = at + 12;
        if (namesz > seg.size() - name) return;
        const size_t desc = up(name + namesz);
        if (desc > seg.size() || descsz > seg.size() - desc) return;
        const Note n{type, namesz == 4 && memcmp(&seg[name], "GNU", 4) == 0, seg.data() + desc,
                     descsz};
        if (fn(n)) return;
        at = up(desc + descsz);
    }
}

}  // namespace

std::string readGnuBuildId(const std::string& path) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return {};
    std::string hex;
    std::vector<Elf64_Phdr> ph;
    if (readProgramHeaders(fd, &ph)) {
        for (const Elf64_Phdr& p : ph) {
            std::vector<uint8_t> seg;
            if (p.p_type != PT_NOTE || !readSegment(fd, p, 65536, &seg)) continue;
            forEachNote(seg, p.p_align == 8 ? 8 : 4, [&hex](const Note& n) {
                if (!n.gnu || n.type != NT_GNU_BUILD_ID || n.descsz == 0 || n.descsz > 64) {
                    return false;
                }
                static const char kHex[] = "0123456789abcdef";
                for (uint32_t i = 0; i < n.descsz; ++i) {
                    hex.push_back(kHex[n.desc[i] >> 4]);
                    hex.push_back(kHex[n.desc[i] & 15]);
                }
                return true;
            });
            if (!hex.empty()) break;
        }
    }
    close(fd);
    return hex;
}

bool readAarch64Features(const std::string& path, uint32_t* features) {
    *features = 0;
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    bool found = false;
    std::vector<Elf64_Phdr> ph;
    if (readProgramHeaders(fd, &ph)) {
        for (const Elf64_Phdr& p : ph) {
            // Only where the kernel and bionic look: PT_GNU_PROPERTY, never a
            // property note sitting in a PT_NOTE alone. 1 KiB is the kernel's
            // own limit for it.
            std::vector<uint8_t> seg;
            if (p.p_type != kPtGnuProperty) continue;
            if (!readSegment(fd, p, 1024, &seg)) break;
            forEachNote(seg, 8, [&](const Note& n) {
                if (!n.gnu || n.type != kNtGnuPropertyType0) return false;
                // Properties: pr_type, pr_datasz, then pr_data padded to 8.
                size_t at = 0;
                while (n.descsz - at >= 8) {
                    uint32_t type, datasz;
                    memcpy(&type, n.desc + at, 4);
                    memcpy(&datasz, n.desc + at + 4, 4);
                    at += 8;
                    if (datasz > n.descsz - at) break;
                    if (type == kAarch64Feature1And && datasz == 4) {
                        memcpy(features, n.desc + at, 4);
                        found = true;
                        break;
                    }
                    const size_t step = (static_cast<size_t>(datasz) + 7) & ~size_t{7};
                    if (step > n.descsz - at) break;
                    at += step;
                }
                return true;
            });
            break;
        }
    }
    close(fd);
    return found;
}

std::vector<ExecSegment> readExecSegments(const std::string& path) {
    std::vector<ExecSegment> out;
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return out;
    std::vector<Elf64_Phdr> ph;
    if (readProgramHeaders(fd, &ph)) {
        for (const Elf64_Phdr& p : ph) {
            // Readable as well as executable: the engine reads this code in
            // its own process, where an execute-only segment would fault.
            if (p.p_type == PT_LOAD && (p.p_flags & PF_X) != 0 && (p.p_flags & PF_R) != 0 &&
                p.p_filesz > 0) {
                out.push_back({p.p_vaddr, p.p_filesz, p.p_offset});
            }
        }
    }
    close(fd);
    return out;
}

bool inExecSegment(const std::vector<ExecSegment>& segs, uint64_t vaddr, size_t n) {
    for (const ExecSegment& s : segs) {
        if (vaddr >= s.vaddr && n <= s.filesz && vaddr - s.vaddr <= s.filesz - n) return true;
    }
    return false;
}

bool readCodeAt(const std::string& path, const std::vector<ExecSegment>& segs, uint64_t vaddr,
                size_t n, uint8_t* out) {
    for (const ExecSegment& s : segs) {
        if (!(vaddr >= s.vaddr && n <= s.filesz && vaddr - s.vaddr <= s.filesz - n)) continue;
        const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return false;
        const off_t off = static_cast<off_t>(s.offset + (vaddr - s.vaddr));
        const bool ok = pread(fd, out, n, off) == static_cast<ssize_t>(n);
        close(fd);
        return ok;
    }
    return false;
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
