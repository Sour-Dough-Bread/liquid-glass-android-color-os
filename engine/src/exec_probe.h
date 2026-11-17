// Can this process patch code? Asked before frida-gum is touched.
//
// Installing a hook needs two things from SELinux that an Android domain is
// normally denied:
//
//   execmem   executable anonymous memory, for the trampolines gum builds
//   execmod   making a written-to (copy-on-write) page of a file mapping
//             executable again, which is what patching a function's first
//             instructions in the binary's text amounts to
//
// On a real device in enforcing mode surfaceflinger has neither, and gum does
// not fail gracefully without them: on alioth (crDroid A17) the allocation was
// denied and gum dereferenced null inside the engine's constructor, before
// SurfaceFlinger's main(), on every start. The emulators run permissive, which
// is why nothing there ever showed it. The module grants both to surfaceflinger
// in its sepolicy.rule; this check is what keeps a device where that rule did
// not apply from crash-looping instead of booting stock.
//
// Both probes work on throwaway mappings: an anonymous page, and a private
// mapping of this process's own executable (the file whose label the real
// patch is checked against). Nothing the process runs is ever touched.
#pragma once

#include <string>

namespace glass {

// True when both are allowed. Otherwise false, with `why` naming what was
// denied and by which call.
bool canPatchCode(std::string* why);

}  // namespace glass
