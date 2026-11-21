#!/usr/bin/env python3
"""Transitional: cut the shared drawing code out of the module's sources.

The native build (rom/make_patch.sh) compiles the module's drawing code as it
is, against SurfaceFlinger's own Skia. That code lives in
engine/src/glass_engine.cpp next to everything only the module needs: the
inline hook, symbol resolution, the boot guard, install(). Until engine/src is
split (captures/sf-corpus/reports/rom_patch.md, "The engine/src split"), this
script does the split on a copy:

  glass_draw.cpp  glass_engine.cpp's declarations, verbatim, less the module's
                  own: its install section whole, and before it the hooking,
                  the boot guard and the gum replacements by name (below); plus
                  rom/split/glass_draw_api.inc
  sk_shared.cpp   sk_bridge.cpp less the two Skia members the module defines
                  for itself and less compile(), which a native build has its own of

A declaration it does not know is kept, so new drawing code flows through; new
module code outside the install section then fails the compile-check instead
of slipping in. A declaration
it must drop or must keep and cannot find is an error. Once engine/src carries
glass_draw.cpp and sk_shared.cpp itself, make_patch.sh copies those and this
script goes.

usage: split_engine.py <engine/src> <out-dir>
"""

import hashlib
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent

# The module's own: hooking, resolution, the boot guard, install. By name.
DROP = {
    # config the drawing code does not use
    "enginePath", "exists", "stateDir",
    # bootloop guard (the module's post-fs-data.sh or a dev install)
    "Guard", "g_module_guard", "guardPaths", "guardShouldRun", "guardDisarmWhenBooted",
    # the hook: frida, the resolved symbols, the originals
    "DrawBlurRegionFn", "DrawBlurRegionScaledFn", "DrawBlurRegionScaledKindFn",
    "g_interceptor", "g_original", "g_original_scaled", "g_original_scaled_kind",
    "g_syms", "g_bias",
    # gum replacements, one per hook shape and filter
    "generateHooks", "drawGlass", "drawGlassScaled", "drawGlassScaledKind",
}
# Everything from this rule to the end of the namespace is install(): the
# module's alone, whatever it grows.
INSTALL_RULE = r"^// -{10,} install\n"
# Dropped if present; a newer engine may not have them any more.
DROP_OPTIONAL = {"g_interceptor", "g_bias", "g_original_scaled_kind",
                 "DrawBlurRegionScaledKindFn", "generateHooks"}

# What glass_draw_api.inc calls into. Must be kept and must exist.
REQUIRED = {
    "propInt", "engineStatus", "g_sk", "GenerateFn", "kFilterCount", "g_generate",
    "generateHook", "g_last_generate", "g_made", "g_backdrop", "monotonicNs", "g_stats",
    "statsFlush", "deferredBlurSeen", "effectiveRadius", "drawGlassCore",
    "drawWithAndroidsBlur",
}

# Module-only headers. Everything else glass_engine.cpp includes is kept.
DROP_INCLUDES = {"<frida-gum.h>", '"abi_check.h"', '"elf_syms.h"', '"exec_probe.h"',
                 '"sf_cache.h"', '"sf_resolve.h"', '"sf_symbols.h"', '"xz_inflate.h"'}

# One-for-one replacements: (old, new, expected count).
REPLACE = [
    ("constexpr size_t kFilterCount = glass::sf::kGenerateFilterCount;",
     "constexpr size_t kFilterCount = glass::draw::kGenerateSlots;", 1),
]

# Strings that must not survive into glass_draw.cpp.
FORBIDDEN = ["gum_", "Gum", "glass::sf::", "glass::Symbol", "frida"]


def die(msg):
    sys.exit(f"split_engine: {msg}")


def skip_space_and_comments(t, i):
    """Index of the next character that is not whitespace or inside a comment."""
    n = len(t)
    while i < n:
        if t[i].isspace():
            i += 1
        elif t.startswith("//", i):
            j = t.find("\n", i)
            i = n if j < 0 else j + 1
        elif t.startswith("/*", i):
            j = t.find("*/", i + 2)
            if j < 0:
                die("unterminated block comment")
            i = j + 2
        else:
            break
    return i


def skip_literal(t, i):
    """At a quote: the index just past the literal (string, char or raw string)."""
    if t[i] == '"' and i > 0 and t[i - 1] == "R" and (i < 2 or not (t[i - 2].isalnum() or t[i - 2] == "_")):
        m = re.compile(r'"([^(\s]*)\(').match(t, i)
        if not m:
            die("odd raw string")
        end = t.find(")" + m.group(1) + '"', m.end())
        if end < 0:
            die("unterminated raw string")
        return end + len(m.group(1)) + 2
    q = t[i]
    i += 1
    while i < len(t):
        if t[i] == "\\":
            i += 2
            continue
        if t[i] == q:
            return i + 1
        if t[i] == "\n":
            die(f"newline inside a literal near: {t[max(0, i - 60):i]!r}")
        i += 1
    die("unterminated literal")


def items_of(t, start, end):
    """Top-level declarations of t[start:end] as (lead, code) pairs.

    `lead` is the comments and blank lines in front of a declaration, `code`
    the declaration itself, up to and including its `;` or closing `}`.
    """
    out = []
    i = start
    while True:
        j = skip_space_and_comments(t, i)
        if j >= end:
            tail = t[i:end]
            return out, tail
        if t[j] == "#":
            die(f"preprocessor line at namespace scope: {t[j:t.find(chr(10), j)]!r}")
        k = j
        depth = 0
        while True:
            if k >= end:
                die(f"declaration runs off the end: {t[j:j + 80]!r}")
            c = t[k]
            if t.startswith("//", k) or t.startswith("/*", k):
                k = skip_space_and_comments(t, k)
                continue
            if c in "\"'":
                # A ' between digits is a digit separator, not a literal.
                if c == "'" and k > 0 and t[k - 1].isalnum() and t[k + 1].isalnum() and t[k - 1].isdigit():
                    k += 1
                    continue
                k = skip_literal(t, k)
                continue
            if c == "#" and t[k - 1] == "\n":
                # A directive inside a body (#define BIND ...): to its end,
                # continuation lines included.
                while True:
                    e = t.find("\n", k)
                    if e < 0 or t[e - 1] != "\\":
                        k = len(t) if e < 0 else e + 1
                        break
                    k = e + 1
                continue
            if c in "{([":
                depth += 1
            elif c in "})]":
                depth -= 1
                if depth < 0:
                    die(f"unbalanced {c!r} near {t[k - 80:k + 1]!r}")
                if depth == 0 and c == "}":
                    n = skip_space_and_comments(t, k + 1)
                    k = n + 1 if n < end and t[n] == ";" else k + 1
                    break
            elif c == ";" and depth == 0:
                k += 1
                break
            k += 1
        out.append((t[i:j], t[j:k]))
        i = k


def code_only(t):
    """t with comments and the insides of literals blanked out."""
    out = []
    i = 0
    n = len(t)
    while i < n:
        if t.startswith("//", i) or t.startswith("/*", i):
            j = skip_space_and_comments(t, i)
            out.append("\n" * t.count("\n", i, j))
            i = j
        elif t[i] in "\"'" and not (t[i] == "'" and i > 0 and t[i - 1].isdigit()):
            j = skip_literal(t, i)
            out.append('""')
            i = j
        else:
            out.append(t[i])
            i += 1
    return "".join(out)


def strip_template(s):
    if not s.startswith("template"):
        return s
    i = s.index("<")
    depth = 0
    for k in range(i, len(s)):
        if s[k] == "<":
            depth += 1
        elif s[k] == ">":
            depth -= 1
            if depth == 0:
                return s[k + 1:].lstrip()
    die(f"unbalanced template header: {s[:80]!r}")


def name_of(code):
    s = strip_template(code.strip())
    m = re.match(r"(?:(?:inline|static|constexpr|const|extern|thread_local)\s+)*"
                 r"(?:struct|class|union|enum(?:\s+class|\s+struct)?)\s+([A-Za-z_]\w*)", s)
    if m:
        return m.group(1)
    m = re.match(r"using\s+([A-Za-z_]\w*)\s*=", s)
    if m:
        return m.group(1)
    if re.match(r"static_assert\b", s):
        return "static_assert"
    angle = 0
    for k, c in enumerate(s):
        if c == "<":
            angle += 1
        elif c == ">":
            angle = max(0, angle - 1)
        elif angle == 0 and c in "(=;[{":
            m = re.search(r"([A-Za-z_]\w*)\s*$", s[:k])
            if not m:
                die(f"cannot name the declaration: {s[:80]!r}")
            return m.group(1)
    die(f"cannot name the declaration: {s[:80]!r}")


def digest(*paths):
    h = hashlib.sha256()
    for p in paths:
        h.update(p.read_bytes())
    return h.hexdigest()[:12]


def split_glass_engine(src, api):
    t = src.read_text()
    ns_open = re.search(r"^namespace \{\n", t, re.M)
    ns_close = re.search(r"^\}  // namespace\n", t, re.M)
    if not ns_open or not ns_close or ns_close.start() < ns_open.end():
        die("glass_engine.cpp: no top-level anonymous namespace")
    if len(re.findall(r"^namespace \{\n", t, re.M)) != 1:
        die("glass_engine.cpp: more than one top-level anonymous namespace")

    # The preamble: the includes and the log macros, less the module's headers.
    pre = t[:ns_open.start()]
    first_include = pre.find("#include")
    if first_include < 0:
        die("glass_engine.cpp: no includes")
    kept_pre = []
    dropped_inc = set()
    for line in pre[first_include:].splitlines(keepends=True):
        m = re.match(r"#include\s+(\S+)", line)
        if m and m.group(1) in DROP_INCLUDES:
            dropped_inc.add(m.group(1))
            continue
        kept_pre.append(line)
    if "<frida-gum.h>" not in dropped_inc:
        die("glass_engine.cpp no longer includes frida-gum.h: re-check the split")

    rule = list(re.finditer(INSTALL_RULE, t[:ns_close.start()], re.M))
    if len(rule) != 1 or rule[0].start() < ns_open.end():
        die(f"glass_engine.cpp: expected one install section rule, found {len(rule)}")
    cut = rule[0].start()
    install = t[cut:ns_close.start()]
    if not re.search(r"^bool install\(\)", install, re.M):
        die("glass_engine.cpp: install() is not in the install section")
    items, tail = items_of(t, ns_open.end(), cut)
    names = [name_of(code) for _, code in items]
    found = set(names)
    for n in sorted(DROP - DROP_OPTIONAL - found):
        die(f"glass_engine.cpp: no declaration named {n} to drop; re-check the split")
    for n in sorted(REQUIRED - found):
        die(f"glass_engine.cpp: no declaration named {n}, which glass_draw_api.inc needs")

    body = []
    # For the report only, so a loose regex: the functions defined there.
    install_names = re.findall(r"^(?:[\w:<>,*&]+[ \t]+)+[*&]*(\w+)\(", install, re.M)
    report = [f"  drop {', '.join(install_names)} (the install section)"]
    for (lead, code), n in zip(items, names):
        if n in DROP:
            report.append(f"  drop {n}")
            # Keep the blank line that separated it from what comes next.
            if body and not body[-1].endswith("\n\n"):
                body.append("\n")
            continue
        body.append(lead + code)
    body.append(tail)
    body = "".join(body)

    for old, new, count in REPLACE:
        if body.count(old) != count:
            die(f"glass_engine.cpp: expected {count} of {old!r}, found {body.count(old)}")
        body = body.replace(old, new)

    out = (
        "// GENERATED by rom/split/split_engine.py from engine/src/glass_engine.cpp\n"
        f"// (sha256 {digest(src)}): its drawing code, without the module's hooking,\n"
        "// symbol resolution, boot guard and install, and with the API that the\n"
        "// module's hooks or a ROM's GlassFilter drive it through (glass_draw.h).\n"
        "// Do not edit; edit engine/src/glass_engine.cpp.\n\n"
        + "".join(kept_pre).rstrip("\n")
        + '\n\n#include "glass_draw.h"\n\nnamespace {\n'
        + body
        + "}  // namespace\n\n"
        + api.read_text()
    )
    code = code_only(out)
    for f in FORBIDDEN:
        if f in code:
            line = next(l for l in code.splitlines() if f in l)
            die(f"glass_draw.cpp would still use {f!r}: {line.strip()!r}")
    kept = sum(1 for n in names if n not in DROP)
    return out, report, kept


def split_sk_bridge(src):
    t = src.read_text()
    rules = [m.start() for m in re.finditer(r"^// -{20,}\n", t, re.M)]
    if len(rules) != 2:
        die(f"sk_bridge.cpp: expected the two rules around the member definitions, found {len(rules)}")
    members = t[rules[0]:t.index("\n", rules[1]) + 1]
    for m in ("SkMatrix SkCanvas::getTotalMatrix() const", "void SkPaint::setShader("):
        if m not in members:
            die(f"sk_bridge.cpp: {m} is no longer between the rules")
    t = t[:rules[0]] + t[rules[0] + len(members):]

    sig = "SkRuntimeEffect* compile(const Bridge& b, const char* sksl, std::string* error) {"
    if t.count(sig) != 1:
        die("sk_bridge.cpp: compile() not found once")
    start = t.index(sig)
    depth = 0
    for k in range(start, len(t)):
        if t[k] == "{":
            depth += 1
        elif t[k] == "}":
            depth -= 1
            if depth == 0:
                end = k + 1
                break
    else:
        die("sk_bridge.cpp: compile() never closes")
    end = t.index("\n", end) + 1
    t = t[:start] + t[end:]
    for m in ("SkCanvas::getTotalMatrix() const {", "SkPaint::setShader(", "SkStringCtor"):
        if m in t:
            die(f"sk_bridge.cpp: {m} survived the split")
    return (
        "// GENERATED by rom/split/split_engine.py from engine/src/sk_bridge.cpp\n"
        f"// (sha256 {digest(src)}): the bridge's helpers that hold in any build,\n"
        "// less compile() (a native build has its own) and less the two Skia\n"
        "// members the module defines for itself (a native build links Skia's).\n"
        "// Do not edit; edit engine/src/sk_bridge.cpp.\n\n" + t.lstrip("\n")
    )


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__.strip().splitlines()[-1])
    src = pathlib.Path(sys.argv[1])
    out = pathlib.Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    draw, report, kept = split_glass_engine(src / "glass_engine.cpp", HERE / "glass_draw_api.inc")
    (out / "glass_draw.cpp").write_text(draw)
    (out / "glass_draw.h").write_text((HERE / "glass_draw.h").read_text())
    (out / "sk_shared.cpp").write_text(split_sk_bridge(src / "sk_bridge.cpp"))
    print(f"split_engine: glass_draw.cpp keeps {kept} declarations of glass_engine.cpp")
    for line in report:
        print(line)


if __name__ == "__main__":
    main()
