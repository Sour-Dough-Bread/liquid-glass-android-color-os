#!/usr/bin/env python3
"""The three edits rom/make_patch.sh makes to existing frameworks/native files.

  libs/renderengine/skia/filters/BlurFilter.h
      drawBlurRegion becomes virtual, so GlassFilter can take it over.
  libs/renderengine/skia/SkiaRenderEngine.cpp
      includes GlassFilter.h, and wraps the filter right after the constructor
      picks it (the `switch (blurAlgorithm)`).
  libs/renderengine/Android.bp
      GlassFilter.cpp joins librenderengine_skia_sources, and librenderengine
      takes librenderengine_glass whole.

Each edit is found by what the code says, not by line number, and has to be
found exactly once; anything else stops the generator. Already-applied edits
are left alone, so a tree that carries an older version of the patch, or a
fork that made drawBlurRegion virtual itself, still works.

usage: edit_tree.py <frameworks/native root>
"""

import pathlib
import re
import sys

RE = "libs/renderengine"


def die(msg):
    sys.exit(f"edit_tree: {msg}")


def once(pattern, text, what, flags=re.M):
    found = list(re.finditer(pattern, text, flags))
    if len(found) != 1:
        die(f"{what}: expected exactly one match for {pattern!r}, found {len(found)}")
    return found[0]


def matching_brace(text, open_index):
    depth = 0
    for k in range(open_index, len(text)):
        if text[k] == "{":
            depth += 1
        elif text[k] == "}":
            depth -= 1
            if depth == 0:
                return k
    die("unbalanced braces")


def edit_blur_filter_h(root, log):
    p = root / RE / "skia/filters/BlurFilter.h"
    t = p.read_text()
    if re.search(r"^\s*virtual\s+void\s+drawBlurRegion\(", t, re.M):
        log.append(f"{p.relative_to(root)}: drawBlurRegion is already virtual")
        return
    m = once(r"^(\s*)void drawBlurRegion\(", t, "BlurFilter.h")
    t = t[:m.start()] + m.group(1) + "virtual void drawBlurRegion(" + t[m.end():]
    p.write_text(t)
    log.append(f"{p.relative_to(root)}: drawBlurRegion made virtual")


def edit_skia_render_engine(root, log):
    p = root / RE / "skia/SkiaRenderEngine.cpp"
    t = p.read_text()
    if "GlassFilter::wrap(" in t:
        log.append(f"{p.relative_to(root)}: already wraps the filter")
        return

    # The include, in its sorted place among the filters' own.
    includes = list(re.finditer(r'^#include "filters/([^"]+)"\n', t, re.M))
    if not includes:
        die('SkiaRenderEngine.cpp: no #include "filters/..." to place GlassFilter.h among')
    after = [m for m in includes if m.group(1) > "GlassFilter.h"]
    at = after[0].start() if after else includes[-1].end()
    t = t[:at] + '#include "filters/GlassFilter.h"\n' + t[at:]

    # The wrap, after the switch that picks the filter. Inside the
    # constructor: the only place mBlurFilter is chosen.
    sw = once(r"^(\s*)switch \(blurAlgorithm\) \{", t, "SkiaRenderEngine.cpp")
    close = matching_brace(t, t.index("{", sw.start()))
    body = t[sw.start():close]
    if "mBlurFilter = " not in body:
        die("SkiaRenderEngine.cpp: the blurAlgorithm switch no longer assigns mBlurFilter")
    eol = t.index("\n", close) + 1
    indent = sw.group(1)
    wrap = (
        f"\n{indent}// StockGlass: Liquid Glass over the blur regions while persist.sys.glass.mode=glass,\n"
        f"{indent}// Android's blur otherwise (filters/GlassFilter.h).\n"
        f"{indent}mBlurFilter = GlassFilter::wrap(mBlurFilter, mRuntimeEffectManager);\n"
    )
    t = t[:eol] + wrap + t[eol:]
    p.write_text(t)
    log.append(f"{p.relative_to(root)}: includes GlassFilter.h, wraps the filter after the switch")


def edit_android_bp(root, log):
    p = root / RE / "Android.bp"
    t = p.read_text()
    if "librenderengine_glass" in t:
        log.append(f"{p.relative_to(root)}: already builds librenderengine_glass")
        return

    # GlassFilter.cpp, in its sorted place in librenderengine_skia_sources.
    fg = once(r'^filegroup \{\n\s*name: "librenderengine_skia_sources",', t, "Android.bp")
    close = matching_brace(t, t.index("{", fg.start()))
    block = t[fg.start():close]
    srcs = list(re.finditer(r'^(\s*)"skia/filters/([^"]+)",\n', block, re.M))
    if not srcs:
        die("Android.bp: no skia/filters/ sources in librenderengine_skia_sources")
    after = [m for m in srcs if m.group(2) > "GlassFilter.cpp"]
    m = after[0] if after else srcs[-1]
    at = fg.start() + (m.start() if after else m.end())
    t = t[:at] + f'{m.group(1)}"skia/filters/GlassFilter.cpp",\n' + t[at:]

    # librenderengine takes librenderengine_glass whole.
    lib = once(r'^cc_library_static \{\n(\s*)name: "librenderengine",\n', t, "Android.bp")
    close = matching_brace(t, t.index("{", lib.start()))
    block = t[lib.start():close]
    indent = lib.group(1)
    w = re.search(r"^(\s*)whole_static_libs: \[", block, re.M)
    if w:
        at = lib.start() + w.end()
        t = t[:at] + f'\n{indent}{indent}"librenderengine_glass",' + t[at:]
    else:
        # Last, before the module's closing brace.
        at = t.rindex("\n", lib.start(), close) + 1
        t = t[:at] + f'{indent}whole_static_libs: ["librenderengine_glass"],\n' + t[at:]
    p.write_text(t)
    log.append(f"{p.relative_to(root)}: GlassFilter.cpp in librenderengine_skia_sources, "
               "librenderengine_glass in librenderengine")


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__.strip().splitlines()[-1])
    root = pathlib.Path(sys.argv[1])
    log = []
    edit_blur_filter_h(root, log)
    edit_skia_render_engine(root, log)
    edit_android_bp(root, log)
    for line in log:
        print(f"edit_tree: {line}")


if __name__ == "__main__":
    main()
