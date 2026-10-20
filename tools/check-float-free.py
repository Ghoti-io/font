#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Font.
#
# Ghoti.io Font is free software: you can redistribute it and/or modify it
# under the terms of the GNU Lesser General Public License version 3 as
# published by the Free Software Foundation.
"""Fail if a GFNT_ header names a floating type (AD-5, CAP-10; story 14b).

This library is float-free: no `float` lies between a font's bytes and a pixel
(documentation/design.md section 8), and since story 14b it is the glyph path
provider of shape-scene, whose seam (glyph_provider.h) is float-free on the
same terms so that this library can include it. Every header given is
preprocessed alone by the compiler with the library's include flags, and every
token `float`, `double`, `_FloatN`, `_FloatNx`, `__float128`, `__fp16` and
`_DecimalN` (`long double` is a `double`) in a line that came from a header of
the suite (a path with a `ghoti.io/` directory, or under this library's
include/) is reported with its file and line. The C library's headers are not
the suite's and are not read: GCC's <stddef.h> names `long double` in
`max_align_t`. shape-scene's own gate (its tools/check-float-free.py) is the
same rule over its seam; this one is the same script's rule over every header
this library installs.

`--sources` adds the code from an outline to coverage or to a device path
(src/scene, src/raster, src/outline): each file is preprocessed the same way and
its own lines must name no floating type. The headers it includes are not
read there, since shape-geometry's device.h declares its `double` entry points
beside the integer ones this code calls.

**One exemption, stated and checked:** `shape.h`'s `float point_size` (the
shaping options' point size, which Apple's `trak` tracks by; tier 2, not on the
path from an outline to coverage or to a device path), and the two headers that
include it, `layout.h` and `provider.h`. It is listed below by its exact line
text, so another floating token anywhere, including in shape.h, still fails,
and so does the exemption once the line is gone (a stale exemption is a gate
that has stopped saying what it claims). It predates story 14b and is recorded
for a decision in planning/specs/spec-shape-m1/stories/14b-*.md.

The gate is seen to fail in every run: a planted header that includes scene.h
and declares a `double` must be refused, naming the planted file; a control that
includes only <stddef.h> and <stdint.h> must pass (so neither a gate that
rejects everything nor one that reads the C library passes); and no header at
all is a failure.

    tools/check-float-free.py --cc "$(CC)" --flags "$(INCLUDE)" header...
"""
import argparse
import os
import re
import shlex
import subprocess
import sys
import tempfile

TOKEN = re.compile(r"\b(float|double|_Float(?:16|32|64|128)x?|__float128|__fp16|_Decimal(?:32|64|128))\b")
MARKER = re.compile(r'^#\s*(\d+)\s+"([^"]*)"')

# (header basename, the preprocessed line with its whitespace collapsed, token).
EXEMPT = {
    ("shape.h", "float point_size;", "float"),
}


def suite_file(path, roots):
    norm = os.path.normpath(path)
    if "/ghoti.io/" in norm.replace(os.sep, "/") + "/":
        return True
    return any(norm.startswith(r) for r in roots)


def scan(cc, flags, header, roots, own=False):
    """Every floating token in the suite's lines of @header's preprocessed text: [(file, line, text, token)]."""
    with tempfile.TemporaryDirectory() as tmp:
        unit = os.path.join(tmp, "unit.c")
        with open(unit, "w") as f:
            f.write('#include "%s"\n' % os.path.abspath(header))
        cmd = shlex.split(cc) + ["-E", "-std=c17"] + shlex.split(flags) + [unit]
        run = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if run.returncode != 0:
            raise SystemExit("check-float-free: %s does not preprocess:\n%s" % (header, run.stderr))
    found = []
    current, line = "", 0
    for text in run.stdout.splitlines():
        m = MARKER.match(text)
        if m:
            line, current = int(m.group(1)), m.group(2)
            continue
        if (os.path.abspath(current) == os.path.abspath(header)) if own else suite_file(current, roots):
            for t in TOKEN.findall(text):
                found.append((current, line, " ".join(text.split()), t))
        line += 1
    return found


def exempt(hit):
    f, _, text, t = hit
    return (os.path.basename(f), text, t) in EXEMPT


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cc", default=os.environ.get("CC", "cc"))
    ap.add_argument("--flags", default="")
    ap.add_argument("--sources", nargs="*", default=[])
    ap.add_argument("headers", nargs="*")
    args = ap.parse_args()
    roots = [os.path.normpath(os.path.abspath("include"))]
    if not args.headers:
        sys.exit("check-float-free: no header given: a gate over nothing passes nothing")
    failures = 0
    used = set()
    for h in args.headers:
        found = scan(args.cc, args.flags, h, roots)
        bad = [x for x in found if not exempt(x)]
        for x in found:
            if exempt(x):
                used.add((os.path.basename(x[0]), x[2], x[3]))
        for f, n, text, t in bad:
            print("check-float-free: %s:%d: `%s` reaches %s (%s)" % (f, n, t, h, text), file=sys.stderr)
        failures += bool(bad)
    print("check-float-free: %d headers, none names a floating type but the exemption" % len(args.headers))
    for src in args.sources:
        for f, n, text, t in scan(args.cc, args.flags, src, roots, own=True):
            print("check-float-free: %s:%d: `%s` in the outline-to-pixels code (%s)" % (f, n, t, text),
                  file=sys.stderr)
            failures += 1
    print("check-float-free: %d source files, none names a floating type" % len(args.sources))
    for e in sorted(EXEMPT - used):
        print("check-float-free: the exemption %s `%s` matches nothing: remove it" % (e[0], e[1]), file=sys.stderr)
        failures += 1
    for e in sorted(used):
        print("check-float-free: exempt, and still there: %s `%s`" % (e[0], e[1]))
    # Seen to fail: a planted double in a suite header that includes scene.h; a control of the C library only.
    scene = os.path.join("include", "ghoti.io", "font", "scene.h")
    with tempfile.TemporaryDirectory() as tmp:
        planted_dir = os.path.join(tmp, "ghoti.io")
        os.makedirs(planted_dir)
        planted = os.path.join(planted_dir, "planted.h")
        with open(planted, "w") as f:
            f.write('#include "%s"\ntypedef double gfnt_planted_t;\n' % os.path.abspath(scene))
        hits = scan(args.cc, args.flags, planted, roots)
        if not any(f.endswith("planted.h") and t == "double" and not exempt((f, n, x, t))
                   for f, n, x, t in hits):
            print("check-float-free: the planted double was not refused, so this gate measures nothing", file=sys.stderr)
            failures += 1
        else:
            print("check-float-free: ok, the planted double is refused, naming ghoti.io/planted.h")
        control = os.path.join(tmp, "control.h")
        with open(control, "w") as f:
            f.write("#include <stddef.h>\n#include <stdint.h>\ntypedef size_t gfnt_control_t;\n")
        if scan(args.cc, args.flags, control, roots):
            print("check-float-free: the control (the C library alone) was refused, so the gate reads too much",
                  file=sys.stderr)
            failures += 1
        else:
            print("check-float-free: ok, the control passes (the C library's own headers are not read)")
    if failures:
        sys.exit("check-float-free: FAIL")


if __name__ == "__main__":
    main()
