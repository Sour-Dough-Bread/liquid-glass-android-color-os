// If SurfaceFlinger dies with Liquid Glass on, it comes back stock.
//
// The module's boot guard (post-fs-data.sh) only catches a boot that never
// completes, and only on the boot after it. A crash in the glass path after
// boot is a different animal: persist.sys.glass.mode survives the crash, init
// restarts SurfaceFlinger, the new process reads "glass" and dies on its first
// glass frame too, every ~5 seconds, with a black screen. On alioth that is
// exactly what happened — and when the phone was then rebooted by hand, the
// first frames of the boot drew a blur region, so the boot never completed
// either, and only the boot guard on the reboot after that recovered it.
//
// The fix keeps one non-persistent property, sys.glass.active, set to 1 while
// this process is drawing glass and 0 when it stops. SurfaceFlinger does not
// restart in normal operation, so a new SurfaceFlinger that finds it still at 1
// knows the previous one died with glass on. It then switches the saved mode to
// stock, leaves a note in sys.glass.tripped for glassctl and the WebUI, and
// draws Android's blur. One crash, not a loop. A reboot clears the property,
// which is right: the saved mode is already stock by then.
//
// The surfaceflinger domain may set system_prop (sys.* and persist.sys.*) on
// stock policy, measured on alioth with engine/tools/propprobe; the module's
// sepolicy.rule grants it as well for builds that differ.
#pragma once

namespace glass {

// At load, before anything reads the mode. Returns true when it tripped.
bool crashGuardStart();

// True while a trip is still in force: until someone sets the mode property
// again, whatever it now says. Only matters if switching the saved mode to
// stock failed; otherwise the mode itself already says stock.
bool crashGuardHolding();

// Glass is about to be drawn / is no longer being drawn. Cheap when there is
// nothing to change; a property write only on a transition.
void crashGuardArm();
void crashGuardDisarm();

}  // namespace glass
