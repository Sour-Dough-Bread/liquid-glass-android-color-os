#!/usr/bin/env python3
"""Fills libs/renderengine/skia/filters/glass/ for rom/make_patch.sh.

  - engine/src's drawing code as it is: glass_draw.{h,cpp}, sk_shared.cpp,
    glass_params.cpp, crash_guard.cpp, and every engine header any of it
    includes, found by following the includes, the generated shader among
    them. The module's own sources (glass_engine.cpp, sk_bridge.cpp, the
    symbol resolver) stay behind;
  - the glue in rom/glue/glass/ (the native Skia bridge, Android.bp).

Every source the Android.bp builds has to be there, and nothing else.

usage: stage_glass.py <engine/src> <dest>
"""

import pathlib
import re
import shutil
import sys

ROM = pathlib.Path(__file__).resolve().parent.parent
GLUE = ROM / "glue" / "glass"

# Taken from engine/src as they are.
SOURCES = ["glass_draw.h", "glass_draw.cpp", "sk_shared.cpp", "glass_params.cpp",
           "crash_guard.cpp"]


def die(msg):
    sys.exit(f"stage_glass: {msg}")


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__.strip().splitlines()[-1])
    src = pathlib.Path(sys.argv[1]).resolve()
    dest = pathlib.Path(sys.argv[2])
    dest.mkdir(parents=True, exist_ok=True)

    missing = [f for f in SOURCES if not (src / f).exists()]
    if missing:
        die(f"{src} has no {', '.join(missing)}")
    for f in SOURCES:
        shutil.copyfile(src / f, dest / f)
    for f in GLUE.iterdir():
        shutil.copyfile(f, dest / f.name)

    # Every engine header the staged files include, transitively.
    pending = [p for p in dest.rglob("*") if p.is_file() and p.suffix in (".h", ".cpp", ".inc")]
    seen = set()
    while pending:
        p = pending.pop()
        if p in seen:
            continue
        seen.add(p)
        for name in re.findall(r'^\s*#\s*include\s+"([^"]+)"', p.read_text(), re.M):
            target = (p.parent / name).resolve()
            if target.exists():
                pending.append(target)
                continue
            rel = (p.parent.resolve().relative_to(dest.resolve()) / name)
            origin = src / rel
            if origin.exists():
                (dest / rel).parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(origin, dest / rel)
                pending.append(dest / rel)
            # Anything else is Skia's or the system's, found on the include path.

    bp = (dest / "Android.bp").read_text()
    listed = set(re.findall(r'^\s*"([^"]+\.cpp)",', bp, re.M))
    present = {p.name for p in dest.glob("*.cpp")}
    if listed != present:
        die(f"Android.bp builds {sorted(listed)}, the directory has {sorted(present)}")
    for p in sorted(dest.rglob("*")):
        if p.is_file():
            print(f"  {p.relative_to(dest)}")


if __name__ == "__main__":
    main()
