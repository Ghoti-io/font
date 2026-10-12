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
"""The named instances of every variable corpus font against fontconfig's.

fontconfig (the pinned `pango` image carries it) lists a variable font once for the font and
once for each named instance that is not the default, at index (n + 1) << 16. This library's font-set
record lists the same instances (`font-instances`); here the two lists are compared on style, full name and
PostScript name.

One difference is understood and counted rather than hidden: where `fvar` gives an instance its own
PostScript name, fontconfig ignores it and spells `Prefix_900wght`, and this library reports the font's
own name. A mismatch passes only when fontconfig's spelling is that synthesised one.

Usage: instance_diff.py [-v]
"""

import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import corpus
import oracle_env

ROOT = oracle_env.ROOT
DRIVER = os.path.join(ROOT, "build", "linux", "release", "apps", "examples", "font-instances")
SYNTH = re.compile(r"^.+_-?[0-9.]+[A-Za-z ]{4}(_.*)?$")


def fc(path):
    argv = oracle_env.command("pango", ["fc-scan", "--format",
            "%{index}\t%{style}\t%{fullname}\t%{postscriptname}\n", path])
    out = subprocess.run(argv, capture_output=True, text=True, check=True).stdout
    rows = []
    for line in out.splitlines():
        index, style, full, ps = line.split("\t")
        if int(index) >= 65536:
            rows.append((style, full, ps))
    return rows


def main():
    verbose = "-v" in sys.argv
    fonts = [f for f in corpus.fonts("variable") if f.endswith((".ttf", ".otf"))]
    directories = sorted({os.path.dirname(f) for f in fonts})
    ours = {}
    out = subprocess.run([DRIVER] + directories, capture_output=True, text=True, check=True).stdout
    for line in out.splitlines():
        path, style, full, ps = line.split("\t")
        ours.setdefault(path, []).append((style, full, ps))
    total = differ = fvar_named = 0
    fonts_seen = 0
    for f in fonts:
        theirs = sorted(fc(f))
        mine = sorted(ours.get(f, []))
        fonts_seen += 1
        total += len(theirs)
        if len(theirs) != len(mine):
            differ += 1
            print("%s: %d instances here, %d in fontconfig" % (f, len(mine), len(theirs)))
            continue
        for a, b in zip(mine, theirs):
            if a[:2] != b[:2]:
                differ += 1
                print("%s: %r vs %r" % (f, a, b))
            elif a[2] != b[2]:
                if SYNTH.match(b[2]) and not SYNTH.match(a[2]):
                    fvar_named += 1
                else:
                    differ += 1
                    print("%s: PostScript %r vs %r" % (f, a[2], b[2]))
        if verbose:
            print("ok", f, len(theirs))
    print("%s: %d variable fonts, %d instances, %d differ (%d with an fvar PostScript name)"
          % (oracle_env.provenance(["pango"]),
             fonts_seen, total, differ, fvar_named))
    if total == 0:
        print("no instances seen: the gate has nothing to compare")
        return 1
    return 1 if differ else 0


if __name__ == "__main__":
    sys.exit(main())
