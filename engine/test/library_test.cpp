// Symbols from a library the binary loads (SymbolResolver::addLibrary,
// sf::addSkiaLibraries) on hand-made ELF files.
//
// ColorOS 17's surfaceflinger imports all of Skia from libhwui.so, and the
// resolver takes those entry points from the library's .dynsym. The real pair
// is checked by run.sh against binaries pulled from the phone; this covers
// the rules no real pair exercises: an import is never a definition, the
// binary's own definition wins, a library it does not load is refused, a
// library's local symbols are not exported, and nothing RenderEngine hooks
// is ever taken from a library.

#include "../src/elf_syms.h"
#include "../src/sf_resolve.h"
#include "../src/sf_symbols.h"

#include <elf.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool cond, const char* what) {
    printf("  [%s] %s\n", cond ? " ok " : "FAIL", what);
    if (!cond) ++failures;
}

struct Fn {
    std::string name;
    uint64_t value;      // 0: an import (SHN_UNDEF)
    bool local = false;  // STB_LOCAL instead of STB_GLOBAL
};

// An ELF64 file with sections only: .dynstr, .dynsym, .dynamic, .shstrtab.
// Enough for ElfView and SymbolResolver, which read sections and never
// segments. `dynamic_size_override` makes the .dynamic section claim more
// bytes than the file has.
std::string writeElf(const std::string& path, const std::vector<Fn>& fns,
                     const std::vector<std::string>& needed,
                     uint64_t dynamic_size_override = 0) {
    std::string dynstr(1, '\0');
    auto addStr = [&dynstr](const std::string& s) {
        const uint32_t at = static_cast<uint32_t>(dynstr.size());
        dynstr += s;
        dynstr.push_back('\0');
        return at;
    };

    std::vector<Elf64_Sym> syms(1);  // the null symbol
    for (const Fn& f : fns) {
        Elf64_Sym s{};
        s.st_name = addStr(f.name);
        s.st_info = ELF64_ST_INFO(f.local ? STB_LOCAL : STB_GLOBAL, STT_FUNC);
        s.st_shndx = f.value == 0 ? SHN_UNDEF : 1;  // any defined section will do
        s.st_value = f.value;
        s.st_size = f.value == 0 ? 0 : 64;
        syms.push_back(s);
    }
    std::vector<Elf64_Dyn> dyn;
    for (const std::string& n : needed) {
        Elf64_Dyn d{};
        d.d_tag = DT_NEEDED;
        d.d_un.d_val = addStr(n);
        dyn.push_back(d);
    }
    dyn.push_back(Elf64_Dyn{DT_NULL, {0}});

    const std::string shstr = std::string("\0.dynstr\0.dynsym\0.dynamic\0.shstrtab\0", 36);
    std::vector<uint8_t> f(sizeof(Elf64_Ehdr), 0);
    auto append = [&f](const void* p, size_t n) {
        while (f.size() % 8 != 0) f.push_back(0);
        const size_t at = f.size();
        f.insert(f.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n);
        return at;
    };
    const size_t at_dynstr = append(dynstr.data(), dynstr.size());
    const size_t at_dynsym = append(syms.data(), syms.size() * sizeof(Elf64_Sym));
    const size_t at_dynamic = append(dyn.data(), dyn.size() * sizeof(Elf64_Dyn));
    const size_t at_shstr = append(shstr.data(), shstr.size());

    Elf64_Shdr sh[5] = {};
    sh[1] = Elf64_Shdr{1, SHT_STRTAB, SHF_ALLOC, 0, at_dynstr, dynstr.size(), 0, 0, 1, 0};
    sh[2] = Elf64_Shdr{9, SHT_DYNSYM, SHF_ALLOC, 0, at_dynsym, syms.size() * sizeof(Elf64_Sym),
                       1, 1, 8, sizeof(Elf64_Sym)};
    sh[3] = Elf64_Shdr{17, SHT_DYNAMIC, SHF_ALLOC | SHF_WRITE, 0, at_dynamic,
                       dynamic_size_override != 0 ? dynamic_size_override
                                                  : dyn.size() * sizeof(Elf64_Dyn),
                       1, 0, 8, sizeof(Elf64_Dyn)};
    sh[4] = Elf64_Shdr{26, SHT_STRTAB, 0, 0, at_shstr, shstr.size(), 0, 0, 1, 0};
    const size_t at_sh = append(sh, sizeof(sh));

    Elf64_Ehdr eh{};
    memcpy(eh.e_ident, ELFMAG, SELFMAG);
    eh.e_ident[EI_CLASS] = ELFCLASS64;
    eh.e_ident[EI_DATA] = ELFDATA2LSB;
    eh.e_ident[EI_VERSION] = EV_CURRENT;
    eh.e_type = ET_DYN;
    eh.e_machine = EM_X86_64;
    eh.e_version = EV_CURRENT;
    eh.e_ehsize = sizeof(Elf64_Ehdr);
    eh.e_shoff = at_sh;
    eh.e_shentsize = sizeof(Elf64_Shdr);
    eh.e_shnum = 5;
    eh.e_shstrndx = 4;
    memcpy(f.data(), &eh, sizeof(eh));

    FILE* out = fopen(path.c_str(), "wb");
    if (out != nullptr) {
        fwrite(f.data(), 1, f.size(), out);
        fclose(out);
    }
    return path;
}

const char* mangling(glass::sf::Index i) { return glass::sf::kEntryPoints[i].manglings[0]; }

}  // namespace

int main() {
    char tmpl[] = "/tmp/glass_lib_XXXXXX";
    if (mkdtemp(tmpl) == nullptr) return 2;
    const std::string dir = tmpl;
    const std::string libdir = dir + "/lib64";
    mkdir(libdir.c_str(), 0755);

    const std::string kCtor = mangling(glass::sf::kSkStringCtor);
    const std::string kReset = mangling(glass::sf::kPaintReset);
    const std::string kHook = mangling(glass::sf::kDrawBlurRegionScaled);

    // The binary: defines one function of its own and SkPaint::reset, imports
    // SkString's constructor, and loads libhwui.so.
    const std::string exe = writeElf(dir + "/surfaceflinger",
                                     {{"own_fn", 0x1000}, {kReset, 0x2000}, {kCtor, 0}},
                                     {"libhwui.so", "libc.so"});
    // The library: exports both, a local helper, and a copy of the hook
    // target (no library's copy of RenderEngine is ever the one that runs).
    writeElf(libdir + "/libhwui.so",
             {{kCtor, 0x500}, {kReset, 0x600}, {"local_fn", 0x700, true}, {kHook, 0x800},
              {"imported_by_lib", 0}},
             {"libc.so"});
    // One the binary does not load.
    writeElf(libdir + "/libother.so", {{"other_fn", 0x900}}, {});

    printf("== a binary and a library it loads\n");
    glass::SymbolResolver r;
    check(r.loadFromFile(exe, nullptr, nullptr), "the binary loads");
    check(r.needed().size() == 2 && r.needed()[0] == "libhwui.so" && r.needed()[1] == "libc.so",
          "DT_NEEDED read in order");
    check(!r.find(kCtor).valid(), "an import is not a definition");
    check(r.modules().size() == 1, "one module before any library");

    check(r.addLibrary(libdir + "/libother.so") == 0, "a library the binary does not load is refused");
    check(r.modules().size() == 1, "a refused library adds no module");

    const std::vector<std::string> report =
        glass::sf::addSkiaLibraries(&r, {dir + "/nowhere", libdir});
    check(report.size() == 1 && report[0].find("Skia from") != std::string::npos,
          "addSkiaLibraries finds libhwui.so in the second directory");
    check(r.modules().size() == 2, "the library is module 1");
    const glass::Symbol ctor = r.find(kCtor);
    check(ctor.valid() && ctor.module == 1 && ctor.value == 0x500,
          "the import resolves to the library's export");
    const glass::Symbol reset = r.find(kReset);
    check(reset.valid() && reset.module == 0 && reset.value == 0x2000,
          "the binary's own definition wins over the library's");
    check(!r.find("local_fn").valid(), "a library's local symbol is not exported");
    check(!r.find("imported_by_lib").valid(), "a library's import is not a definition");
    check(r.addLibrary(libdir + "/libhwui.so") == 0 && r.modules().size() == 2,
          "adding the same library twice adds nothing");

    printf("== resolve: only Skia may come from a library\n");
    glass::sf::Resolution res;
    glass::sf::resolve(r, nullptr, nullptr, &res);
    check(res.modules.size() == 2, "the resolution names both modules");
    check(res.has(glass::sf::kSkStringCtor) && res.sym(glass::sf::kSkStringCtor).module == 1,
          "a Skia entry is taken from the library");
    check(!res.has(glass::sf::kDrawBlurRegionScaled),
          "the hook target is never taken from a library");
    check(!res.entry[glass::sf::kDrawBlurRegionScaled].drifted,
          "nor counted as drift");
    check(glass::sf::moduleName(res, 1) == "libhwui.so", "the module is reported by name");

    // A cache could claim a hook target in a library; judge() refuses it.
    glass::sf::Resolution forged = res;
    forged.entry[glass::sf::kDrawBlurRegionScaled].sym = glass::Symbol{0x800, 4096, {}};
    forged.entry[glass::sf::kDrawBlurRegionScaled].sym.module = 1;
    glass::sf::judge(&forged);
    check(!forged.ok && forged.shape == -1, "judge refuses a hook target in a library");
    bool named = false;
    for (const std::string& f : forged.fatal) {
        named = named || f.find("must be the binary's own") != std::string::npos;
    }
    check(named, "and says why");
    forged = res;
    forged.entry[glass::sf::kSkStringCtor].sym.module = 7;
    glass::sf::judge(&forged);
    check(!forged.ok, "judge refuses an entry from a module nobody named");

    printf("== no Skia library\n");
    {
        const std::string plain = writeElf(dir + "/plain", {{"own_fn", 0x1000}}, {"libc.so"});
        glass::SymbolResolver p;
        check(p.loadFromFile(plain, nullptr, nullptr), "a binary that loads no Skia library");
        check(glass::sf::addSkiaLibraries(&p, {libdir}).empty() && p.modules().size() == 1,
              "addSkiaLibraries adds nothing and reports nothing");
    }
    {
        const std::string lost = writeElf(dir + "/lost", {{"own_fn", 0x1000}}, {"libhwui.so"});
        glass::SymbolResolver p;
        p.loadFromFile(lost, nullptr, nullptr);
        const std::vector<std::string> rep = glass::sf::addSkiaLibraries(&p, {dir + "/nowhere"});
        check(rep.size() == 1 && rep[0].find("not found") != std::string::npos &&
                  p.modules().size() == 1,
              "a loaded library that is not on disk is reported, not guessed");
    }

    printf("== a malformed .dynamic\n");
    {
        const std::string bad = writeElf(dir + "/bad", {{"own_fn", 0x1000}}, {"libhwui.so"},
                                         1u << 30);
        glass::SymbolResolver p;
        check(p.loadFromFile(bad, nullptr, nullptr), "the symbols still load");
        check(p.needed().empty(), "a .dynamic past the end of the file names nothing");
    }

    const std::string cleanup = "rm -rf '" + dir + "'";
    if (system(cleanup.c_str()) != 0) fprintf(stderr, "could not remove %s\n", dir.c_str());
    printf("%s\n", failures == 0 ? "PASS" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
