// Verifies the vendored decoder against system xz on a real .gnu_debugdata blob.
#include "../src/xz_inflate.h"
#include <cstdio>
#include <fstream>
#include <vector>
int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <in.xz> <expected.bin>\n", argv[0]); return 2; }
    auto slurp = [](const char* p, std::vector<uint8_t>* v) {
        std::ifstream f(p, std::ios::binary | std::ios::ate);
        if (!f) return false;
        v->resize((size_t)f.tellg()); f.seekg(0);
        return (bool)f.read((char*)v->data(), (std::streamsize)v->size());
    };
    std::vector<uint8_t> in, expect, got;
    if (!slurp(argv[1], &in) || !slurp(argv[2], &expect)) { fprintf(stderr, "read failed\n"); return 1; }
    if (!glass::inflateXz(in.data(), in.size(), &got)) { fprintf(stderr, "inflate FAILED\n"); return 1; }
    printf("  in=%zu  out=%zu  expected=%zu\n", in.size(), got.size(), expect.size());
    if (got != expect) { fprintf(stderr, "  MISMATCH\n"); return 1; }
    printf("  byte-identical to system xz -- PASS\n");
    return 0;
}
