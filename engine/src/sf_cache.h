// The install-time symbol cache: resolve once at install, verify at every boot.
//
// Finding the entry points costs the engine its biggest start-up item: reading
// the whole ~10 MB surfaceflinger and inflating its MiniDebugInfo, 318 ms on
// the Poco F1 before SurfaceFlinger's main(). glasscheck already does exactly
// that work at install, so it writes down what it found, and the engine checks
// the note instead of redoing it.
//
// The cache is also what makes builds WITHOUT symbols reachable: an address
// found at install by other means (sf_scan, Source::kScan) is handed to the
// engine the same way. The expensive and clever part runs in glasscheck, where
// a mistake costs a declined install and the host can test it; the engine
// only ever does the cheap, strict check below.
//
// Trust nothing in the file. It is accepted only when:
//   - its format line and the entry-point table hash match this engine
//     (sf_resolve.h tableHash: an older glasscheck's cache is ignored);
//   - its build ID equals the running binary's (.note.gnu.build-id);
//   - every entry's address lies inside an executable segment of the binary,
//     and its first bytes, recorded at install, equal the code at that address
//     in this process.
// Any failure means "no cache": the engine resolves from symbols as it always
// did, so a stale or damaged cache can cost time, never correctness.
//
// Where it lives: next to the engine library, named libglassengine.symcache.
// The module's overlay puts both in /system/lib64 with the system_lib_file
// label, which SurfaceFlinger may read; /data/adb is out of its reach.
//
// Format, one record per line:
//   stockglass-symcache 1
//   table <16 hex>
//   build-id <hex>
//   e <index> <value hex> <size hex> <symbols|scan> <prologue hex> <label...>
//   d <index> <drifted mangling>          (informational)
//   end
#pragma once

#include <cstdint>
#include <string>

#include "sf_resolve.h"

namespace glass {
namespace sf {

// How many code bytes are recorded per entry and compared at boot.
constexpr size_t kCachePrologue = 16;

// Writes the resolved entries of `r` (judged ok) for the binary at exe_path.
bool writeCache(const std::string& cache_path, const std::string& exe_path, const Resolution& r,
                std::string* error);

// Reads a cache and checks it against the binary running in this process,
// loaded at `bias`: the code compared is the mapped text. Fills `out` with the
// entries (Source::kCache, or kScan for scanned ones) and judges it; the caller
// should still run probeAbi(). False with *why when the cache is absent, stale
// or does not match.
bool readCacheForProcess(const std::string& cache_path, const std::string& exe_path,
                         uintptr_t bias, Resolution* out, std::string* why);

// The same check against the file on disk (host tests, glasscheck --verify).
bool readCacheForFile(const std::string& cache_path, const std::string& exe_path,
                      Resolution* out, std::string* why);

// The cache path next to a library path: "/x/libglassengine.so" ->
// "/x/libglassengine.symcache".
std::string cachePathFor(const std::string& library_path);

}  // namespace sf
}  // namespace glass
