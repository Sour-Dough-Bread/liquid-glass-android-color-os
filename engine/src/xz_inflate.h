// xz decompression for .gnu_debugdata.
//
// Android ships no xz/xzcat/unxz (verified on an A16 image — toybox has
// neither), so the engine carries a minimal XZ Embedded decoder. See
// vendor/fetch_xz.sh for what is vendored and why.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace glass {

// Decompresses a complete xz stream. Returns false on any malformed input
// rather than aborting: this parses a system file inside SurfaceFlinger, so
// every failure has to be survivable.
bool inflateXz(const uint8_t* in, size_t in_size, std::vector<uint8_t>* out);

// Matches InflateFn in elf_syms.h so it can be handed to SymbolResolver.
bool inflateXzAdapter(const uint8_t* in, size_t in_size, std::vector<uint8_t>* out, void* user);

}  // namespace glass
