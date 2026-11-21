// The note readers (readGnuBuildId, readAarch64Features) on hand-made ELF
// files. Both run inside SurfaceFlinger on a file it maps, so a malformed note
// has to come back "none", never as a read past the segment.
//
// The corpus checks the good cases against readelf on real builds; this covers
// the shapes no build has: notes cut short, sizes that overflow, a property
// note outside PT_GNU_PROPERTY, a property list to walk.

#include "../src/elf_syms.h"

#include <elf.h>
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

void put32(std::vector<uint8_t>* b, uint32_t v) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    b->insert(b->end(), p, p + 4);
}

void pad(std::vector<uint8_t>* b, size_t align) {
    while (b->size() % align != 0) b->push_back(0);
}

// A note: namesz, descsz, type, name, descriptor, each padded to align.
std::vector<uint8_t> note(uint32_t type, const char* name, const std::vector<uint8_t>& desc,
                          size_t align, uint32_t namesz_override = 0,
                          uint32_t descsz_override = 0) {
    std::vector<uint8_t> b;
    const uint32_t namesz = static_cast<uint32_t>(strlen(name) + 1);
    put32(&b, namesz_override != 0 ? namesz_override : namesz);
    put32(&b, descsz_override != 0 ? descsz_override : static_cast<uint32_t>(desc.size()));
    put32(&b, type);
    b.insert(b.end(), name, name + namesz);
    pad(&b, align);
    b.insert(b.end(), desc.begin(), desc.end());
    pad(&b, align);
    return b;
}

// One GNU property: pr_type, pr_datasz, data padded to 8.
void property(std::vector<uint8_t>* desc, uint32_t type, const std::vector<uint8_t>& data,
              uint32_t datasz_override = 0) {
    put32(desc, type);
    put32(desc, datasz_override != 0 ? datasz_override : static_cast<uint32_t>(data.size()));
    desc->insert(desc->end(), data.begin(), data.end());
    pad(desc, 8);
}

std::vector<uint8_t> u32(uint32_t v) {
    std::vector<uint8_t> b;
    put32(&b, v);
    return b;
}

struct Segment {
    uint32_t type;
    uint64_t align;
    std::vector<uint8_t> bytes;
    uint64_t filesz_override = 0;
};

// An ELF64 header, a program header table, then each segment's bytes.
std::string writeElf(const char* tag, const std::vector<Segment>& segs, uint16_t phnum_override = 0) {
    std::vector<uint8_t> f(sizeof(Elf64_Ehdr) + segs.size() * sizeof(Elf64_Phdr), 0);
    Elf64_Ehdr eh{};
    memcpy(eh.e_ident, ELFMAG, SELFMAG);
    eh.e_ident[EI_CLASS] = ELFCLASS64;
    eh.e_ident[EI_DATA] = ELFDATA2LSB;
    eh.e_ident[EI_VERSION] = EV_CURRENT;
    eh.e_type = ET_DYN;
    eh.e_machine = EM_AARCH64;
    eh.e_version = EV_CURRENT;
    eh.e_phoff = sizeof(Elf64_Ehdr);
    eh.e_ehsize = sizeof(Elf64_Ehdr);
    eh.e_phentsize = sizeof(Elf64_Phdr);
    eh.e_phnum = phnum_override != 0 ? phnum_override : static_cast<uint16_t>(segs.size());
    memcpy(f.data(), &eh, sizeof(eh));
    for (size_t i = 0; i < segs.size(); ++i) {
        pad(&f, 8);
        Elf64_Phdr ph{};
        ph.p_type = segs[i].type;
        ph.p_offset = f.size();
        ph.p_vaddr = ph.p_paddr = f.size();
        ph.p_filesz = ph.p_memsz =
            segs[i].filesz_override != 0 ? segs[i].filesz_override : segs[i].bytes.size();
        ph.p_align = segs[i].align;
        ph.p_flags = PF_R;
        memcpy(f.data() + sizeof(Elf64_Ehdr) + i * sizeof(Elf64_Phdr), &ph, sizeof(ph));
        f.insert(f.end(), segs[i].bytes.begin(), segs[i].bytes.end());
    }
    char path[] = "/tmp/glass_note_XXXXXX";
    const int fd = mkstemp(path);
    if (fd < 0 || write(fd, f.data(), f.size()) != static_cast<ssize_t>(f.size())) {
        perror(tag);
        exit(2);
    }
    close(fd);
    return path;
}

constexpr uint32_t kPtGnuProperty = 0x6474e553;
constexpr uint32_t kFeature1And = 0xc0000000;
constexpr uint32_t kX86Feature1And = 0xc0000002;

// Most cases need one segment. Built step by step: GCC takes a nested brace
// list of temporaries here for a dangling pointer.
std::string oneSegment(const char* tag, uint32_t type, uint64_t align,
                       const std::vector<uint8_t>& bytes, uint16_t phnum_override = 0) {
    std::vector<Segment> segs(1);
    segs[0].type = type;
    segs[0].align = align;
    segs[0].bytes = bytes;
    return writeElf(tag, segs, phnum_override);
}

bool features(const std::string& path, uint32_t* out) {
    const bool r = glass::readAarch64Features(path, out);
    unlink(path.c_str());
    return r;
}

}  // namespace

int main() {
    std::vector<uint8_t> desc;
    uint32_t f = 0;

    puts("== readAarch64Features");
    desc.clear();
    property(&desc, kFeature1And, u32(3));
    check(features(oneSegment("bti", kPtGnuProperty, 8, note(5, "GNU", desc, 8)), &f) && f == 3,
          "BTI and PAC from a PT_GNU_PROPERTY note");

    desc.clear();
    property(&desc, kX86Feature1And, u32(1));
    property(&desc, kFeature1And, u32(1));
    check(features(oneSegment("walk", kPtGnuProperty, 8, note(5, "GNU", desc, 8)), &f) && f == 1,
          "found after another property in the list");

    desc.clear();
    property(&desc, 0xc0000001, {1, 2, 3, 4, 5, 6});  // 6 bytes, padded to 8
    property(&desc, kFeature1And, u32(2));
    check(features(oneSegment("odd", kPtGnuProperty, 8, note(5, "GNU", desc, 8)), &f) && f == 2,
          "a property of odd size is stepped over by its padded size");

    desc.clear();
    property(&desc, kFeature1And, u32(3));
    check(!features(oneSegment("ptnote", PT_NOTE, 8, note(5, "GNU", desc, 8)), &f) && f == 0,
          "the same note in a PT_NOTE alone does not count (the kernel and bionic ignore it)");

    check(!features(oneSegment("name", kPtGnuProperty, 8, note(5, "GNX", desc, 8)), &f),
          "a note not named GNU is not read");

    check(!features(oneSegment("type", kPtGnuProperty, 8, note(1, "GNU", desc, 8)), &f),
          "a note of another type is not read");

    check(!features(oneSegment("descsz", kPtGnuProperty, 8, note(5, "GNU", desc, 8, 0, 0xfffffff0)), &f),
          "a descriptor size past the segment is refused");

    check(!features(oneSegment("namesz", kPtGnuProperty, 8, note(5, "GNU", desc, 8, 0xfffffff0)), &f),
          "a name size past the segment is refused");

    desc.clear();
    property(&desc, kFeature1And, u32(3), 0xfffffff8);
    check(!features(oneSegment("datasz", kPtGnuProperty, 8, note(5, "GNU", desc, 8)), &f) && f == 0,
          "a property size past the descriptor is refused");

    desc.clear();
    property(&desc, kFeature1And, {3, 0, 0, 0, 0, 0, 0, 0});
    check(!features(oneSegment("size8", kPtGnuProperty, 8, note(5, "GNU", desc, 8)), &f),
          "a feature word that is not 4 bytes is not taken");

    desc.clear();
    property(&desc, kFeature1And, u32(3));
    {
        Segment s{kPtGnuProperty, 8, note(5, "GNU", desc, 8)};
        s.filesz_override = 4096;  // bigger than the kernel's 1 KiB limit
        const std::vector<Segment> segs = {s};
        check(!features(writeElf("big", segs), &f), "a property segment over 1 KiB is refused");
    }
    {
        std::vector<uint8_t> cut = note(5, "GNU", desc, 8);
        cut.resize(14);
        check(!features(oneSegment("cut", kPtGnuProperty, 8, cut), &f),
              "a note cut short inside its name is refused");
    }
    check(!features(oneSegment("phnum", kPtGnuProperty, 8, note(5, "GNU", desc, 8), 65), &f),
          "a program header count over 64 is refused");
    check(!glass::readAarch64Features("/nonexistent/surfaceflinger", &f), "a missing file is none");

    puts("== readGnuBuildId");
    const std::vector<uint8_t> id = {0xde, 0xad, 0xbe, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab,
                                     0xcd, 0xef, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
    desc.clear();
    property(&desc, kFeature1And, u32(3));
    for (size_t align : {size_t{4}, size_t{8}}) {
        std::vector<Segment> segs(2);
        segs[0] = {PT_NOTE, align, note(5, "GNU", desc, align)};
        segs[1] = {PT_NOTE, align, note(NT_GNU_BUILD_ID, "GNU", id, align)};
        const std::string p = writeElf("id", segs);
        const std::string hex = glass::readGnuBuildId(p);
        unlink(p.c_str());
        check(hex == "deadbeef0123456789abcdef0011223344556677",
              align == 4 ? "build id after another note, 4-aligned"
                         : "build id after another note, 8-aligned");
    }
    {
        std::vector<uint8_t> two = note(1, "Android", u32(36), 4);
        const std::vector<uint8_t> b = note(NT_GNU_BUILD_ID, "GNU", id, 4);
        two.insert(two.end(), b.begin(), b.end());
        const std::string p = oneSegment("two", PT_NOTE, 4, two);
        check(glass::readGnuBuildId(p) == "deadbeef0123456789abcdef0011223344556677",
              "build id as the second note of one segment");
        unlink(p.c_str());
    }
    {
        const std::string p =
            oneSegment("idcut", PT_NOTE, 4, note(NT_GNU_BUILD_ID, "GNU", id, 4, 0, 0x7ffffff0));
        check(glass::readGnuBuildId(p).empty(), "a build id whose size runs past the segment is none");
        unlink(p.c_str());
    }

    if (failures != 0) {
        printf("elf_note_test: %d FAILED\n", failures);
        return 1;
    }
    puts("elf_note_test: all passed");
    return 0;
}
