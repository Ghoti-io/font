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
"""The public MATH reader against fontTools, on every corpus font that has a MATH table.

`font-math` prints every fact the reader can give for a face (the 56 constants, the connector
overlap, and per glyph the italics correction, top accent, extended shape, the four kerns, and
the vertical and horizontal variants and assemblies). `fonttools_math_read.py` prints the same
lines from fontTools in the pinned image. The two outputs must be identical, line for line.
Default location only; device tables are not applied by fontTools' decoding, so a font whose
values carry them is compared on the numbers they carry, as the reader gives them unvaried.

Usage: math_read_diff.py [--font PATH]... [-v]
"""

import difflib
import glob
import os
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import corpus
import oracle_env

ROOT = oracle_env.ROOT
DRIVER = os.path.join(ROOT, "build", "linux", "release", "apps", "examples", "font-math")
READ = os.path.join(ROOT, "tools", "oracle", "fonttools_math_read.py")


def has_math(path):
    try:
        with open(path, "rb") as h:
            head = h.read(12)
            if head[:4] == b"ttcf":
                return False
            n = struct.unpack(">H", head[4:6])[0]
            return any(h.read(16)[:4] == b"MATH" for _ in range(n))
    except (OSError, struct.error):
        return False


def main(argv):
    extra = [argv[i + 1] for i, a in enumerate(argv) if a == "--font"]
    fixtures = sorted(glob.glob(os.path.join(ROOT, "tests", "data", "fonts", "math*.ttf")))
    fonts = [f for f in corpus.fonts("sfnt") if has_math(f)] + fixtures + extra
    print(oracle_env.provenance(["fonttools"]))
    if not fonts:
        print("check-oracle-math: FAILED, no font with a MATH table")
        return 1
    differ = 0
    lines = 0
    for font in fonts:
        ours = subprocess.run([DRIVER, font], capture_output=True, text=True)
        theirs = subprocess.run(oracle_env.command("fonttools", ["python3", READ, font]),
                capture_output=True, text=True)
        if ours.returncode or theirs.returncode:
            differ += 1
            print("%s: font-math exit %d, fontTools exit %d\n%s%s" % (font, ours.returncode,
                  theirs.returncode, ours.stderr[-300:], theirs.stderr[-500:]))
            continue
        lines += len(ours.stdout.splitlines())
        if ours.stdout != theirs.stdout:
            differ += 1
            diff = list(difflib.unified_diff(theirs.stdout.splitlines(), ours.stdout.splitlines(),
                    "fontTools", "here", lineterm="", n=0))
            print("%s: %d lines differ\n  %s" % (font, len(diff), "\n  ".join(diff[:12])))
        elif "-v" in argv:
            print("ok", font, len(ours.stdout.splitlines()))
    print("math reader: %d fonts, %d facts compared, %d differ" % (len(fonts), lines, differ))
    return 1 if differ else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
