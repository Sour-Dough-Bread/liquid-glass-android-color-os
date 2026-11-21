#!/usr/bin/env python3
"""Which BlurFilter this tree has, for GlassFilter (rom/make_patch.sh).

Reads libs/renderengine/skia/filters/BlurFilter.h and writes GlassFilterApi.h
beside it, saying which of the shapes GlassFilter knows the tree has:

  STOCKGLASS_GENERATE_TEMPORARY_IMAGE  1: generateTemporaryImage() (Android 17)
                                       0: generate() (Android 16 QPR2)
  STOCKGLASS_DRAW_MATERIAL             drawBlurRegion takes the region's material
                                       after the input image (Evolution X, crDroid)
  STOCKGLASS_SET_DISPLAY               the filter has setDisplay(namePlusId)
  STOCKGLASS_LENS_REACH                the filter has lensReach(region, material)

Then it reads SkiaRenderEngine.cpp: every member called on mBlurFilter has to
be one GlassFilter takes over, or getMaxCrossFadeRadius(), which GlassFilter
is built to answer as the filter it wraps. A fork that calls anything else
stops the generator here, rather than get a patch that cuts its own filter
off from that call.

Prints one line saying what it found.

usage: blur_api.py <frameworks/native root>
"""

import pathlib
import re
import sys

RE = "libs/renderengine"

# AOSP's drawBlurRegion, parameter types in order (android16-qpr2-release and
# android17-release alike).
DRAW_TYPES = ["SkCanvas*", "const SkRRect&", "const uint32_t", "const float", "const float",
              "const SkRect&", "sk_sp<SkImage>", "sk_sp<SkImage>"]
# What Evolution X and crDroid add after them.
MATERIAL_TYPE = "int32_t"


def die(msg):
    sys.exit(f"blur_api: {msg}")


def code_of(text):
    """`text` without its comments (BlurFilter.h's own name generate() in them)."""
    return re.sub(r"//[^\n]*|/\*.*?\*/", " ", text, flags=re.S)


def declaration(text, name):
    """The parameter list of the one declaration of `name` in BlurFilter, and
    what stands between its `)` and its `;`; None when there is none."""
    found = list(re.finditer(rf"\b{name}\(", text))
    if not found:
        return None
    if len(found) != 1:
        die(f"BlurFilter.h names {name}( {len(found)} times; expected one declaration")
    i = found[0].end()
    depth = 1
    for k in range(i, len(text)):
        if text[k] == "(":
            depth += 1
        elif text[k] == ")":
            depth -= 1
            if depth == 0:
                return text[i:k], text[k + 1:text.index(";", k)].strip()
    die(f"BlurFilter.h: {name}( never closes")


def param_types(params):
    """Each parameter's type, with its name and any default argument dropped."""
    out = []
    for p in re.split(r",(?![^<]*>)", params):
        p = " ".join(p.split("=")[0].split())
        if not p:
            continue
        m = re.match(r"(.*?[\s*&>])(\w+)$", p)
        if m and m.group(1).strip() not in ("", "const", "unsigned"):
            p = m.group(1)
        out.append(re.sub(r"\s+([*&])", r"\1", p.strip()))
    return out


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__.strip().splitlines()[-1])
    root = pathlib.Path(sys.argv[1])
    filters = root / RE / "skia/filters"
    h = code_of((filters / "BlurFilter.h").read_text())

    if declaration(h, "generateTemporaryImage") is not None:
        generate, what = 1, ["generateTemporaryImage() (Android 17)"]
    elif re.search(r"sk_sp<SkImage>\s+generate\(", h):
        generate, what = 0, ["generate() (Android 16 QPR2)"]
    else:
        die("BlurFilter.h has neither generateTemporaryImage() nor generate()")

    draw = declaration(h, "drawBlurRegion")
    if draw is None:
        die("BlurFilter.h has no drawBlurRegion")
    types = param_types(draw[0])
    if types == DRAW_TYPES:
        material = 0
    elif types == DRAW_TYPES + [MATERIAL_TYPE]:
        material = 1
        what.append("drawBlurRegion with a material")
    else:
        die(f"drawBlurRegion({', '.join(types)}) is a shape GlassFilter does not know")
    if draw[1]:
        die(f"drawBlurRegion is declared {draw[1]!r} after its parameters")

    set_display = declaration(h, "setDisplay")
    if set_display is not None:
        if param_types(set_display[0]) != ["const std::string&"] or set_display[1]:
            die("setDisplay() is a shape GlassFilter does not know")
        what.append("setDisplay()")
    lens_reach = declaration(h, "lensReach")
    if lens_reach is not None:
        if (param_types(lens_reach[0]) != ["const SkRRect&", "int32_t"] or
                lens_reach[1] != "const"):
            die("lensReach() is a shape GlassFilter does not know")
        what.append("lensReach()")

    handled = {"drawBlurRegion", "getMaxCrossFadeRadius"}
    handled |= ({"generateTemporaryImage", "preallocateBuffers", "areBuffersPreallocated"}
                if generate else {"generate"})
    if set_display is not None:
        handled.add("setDisplay")
    if lens_reach is not None:
        handled.add("lensReach")
    sre = code_of((root / RE / "skia/SkiaRenderEngine.cpp").read_text())
    called = set(re.findall(r"\bmBlurFilter->(\w+)\(", sre))
    if not called:
        die("SkiaRenderEngine.cpp calls nothing on mBlurFilter")
    unknown = sorted(called - handled)
    if unknown:
        die(f"SkiaRenderEngine.cpp calls mBlurFilter->{', '.join(unknown)}(), "
            "which GlassFilter does not pass on to the filter it wraps")

    (filters / "GlassFilterApi.h").write_text(
        "// Generated by rom/make_patch.sh (rom/tools/blur_api.py): the shape of this\n"
        "// tree's BlurFilter, which GlassFilter overrides.\n"
        + "".join(f"//   {w}\n" for w in what)
        + "#pragma once\n\n"
        f"#define STOCKGLASS_GENERATE_TEMPORARY_IMAGE {generate}\n"
        f"#define STOCKGLASS_DRAW_MATERIAL {material}\n"
        f"#define STOCKGLASS_SET_DISPLAY {int(set_display is not None)}\n"
        f"#define STOCKGLASS_LENS_REACH {int(lens_reach is not None)}\n")
    print("; ".join(what))


if __name__ == "__main__":
    main()
