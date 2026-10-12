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
"""Subset every corpus font that has a `MATH` table, and hold the table to fontTools.

documentation/design.md section 12. A subsetter that drops `MATH` makes a math font's subset
useless for formulas; one that keeps it has to keep exactly the entries of the glyphs it kept, and
the sizes and pieces those glyphs grow to. For each font with the table and each of several
samples of characters (a formula's worth, and seeded draws from the glyphs the table names) this
writes the subset twice, with the ids retained and renumbered, and asks fontTools -- run on
the same glyph set with its closure off -- for the table it would have written. The two trees
(constants, italics corrections, top accents, extended shapes, kerns, constructions and
assemblies, with every glyph an id in the original's numbering) must be the same.

Whether this library's glyph set is the one fontTools' own closure finds for the same characters
is a second question, counted as glyphs missed (fontTools keeps one this does not) and glyphs
extra (the over-approximation).

The `math*.ttf` fixtures (every part of the table; device tables and variation indices) are
included, as are any fonts named with --font.

Usage: math_subset_diff.py [--font PATH]... [--quiet]
"""

import glob
import json
import os
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import corpus
import oracle_env

ROOT = oracle_env.ROOT
SCRATCH = os.path.join(ROOT, "build", "oracle", "math-subset")
DRIVER = os.path.join(ROOT, "build", "linux", "release", "apps", "examples", "font-subset")
CHECK = os.path.join(ROOT, "tools", "oracle", "fonttools_math_check.py")


def has_math(path):
    """Whether the first face of an sfnt file lists a MATH table."""
    try:
        with open(path, "rb") as h:
            head = h.read(12)
            if head[:4] == b"ttcf":
                return False
            n = struct.unpack(">H", head[4:6])[0]
            return any(h.read(16)[:4] == b"MATH" for _ in range(n))
    except (OSError, struct.error):
        return False


def container(script, data, name):
    """Run a mode of the checker in the fonttools image; its JSON output."""
    src = os.path.join(SCRATCH, name + "-in.json")
    dst = os.path.join(SCRATCH, name + "-out.json")
    with open(src, "w", encoding="utf-8") as h:
        json.dump(data, h)
    argv = oracle_env.command("fonttools", ["python3", CHECK, script, src, dst], scratch=SCRATCH)
    run = subprocess.run(argv, capture_output=True, text=True)
    if run.returncode != 0:
        sys.stderr.write(run.stdout + run.stderr)
        raise SystemExit("the fontTools checker failed (%s)" % script)
    with open(dst, encoding="utf-8") as h:
        return json.load(h)


def main(argv):
    quiet = "--quiet" in argv
    extra = [argv[i + 1] for i, a in enumerate(argv) if a == "--font"]
    os.makedirs(SCRATCH, exist_ok=True)
    fixtures = sorted(glob.glob(os.path.join(ROOT, "tests", "data", "fonts", "math*.ttf")))
    fonts = [f for f in corpus.fonts("sfnt") if has_math(f)] + fixtures + extra
    print(oracle_env.provenance(["fonttools"]))
    if not fonts:
        print("check-oracle-subset-math: FAILED, no font with a MATH table in the corpus")
        return 1
    samples = container("pick", fonts, "pick")
    jobs = []
    declined = []
    refused = []
    for fi, font in enumerate(fonts):
        for si, cps in enumerate(samples.get(font, [])):
            base = os.path.join(SCRATCH, "s-%d-%d" % (fi, si))
            args = [DRIVER, "--unicodes", ",".join("%X" % c for c in cps)]
            run_a = subprocess.run(args + ["--retain-gids", font, base + "-retain.otf"],
                                   capture_output=True, text=True)
            run_b = subprocess.run(args + ["--map", base + ".map", font, base + "-ren.otf"],
                                   capture_output=True, text=True)
            bad = run_a if run_a.returncode else run_b if run_b.returncode else None
            if bad:
                why = bad.stderr.strip().splitlines()[0] if bad.stderr.strip() else "?"
                (declined if "nsupported" in why else refused).append((font, si, why))
                continue
            with open(base + ".map", encoding="utf-8") as h:
                gmap = [None if l.strip() == "-" else int(l) for l in h if l.strip()]
            jobs.append({"font": font, "sample": si, "cps": cps, "retain": base + "-retain.otf",
                         "renumber": base + "-ren.otf", "map": gmap})
    results = container("compare", jobs, "compare")
    differ = []
    errors = []
    missed = []
    kept = extra_glyphs = 0
    with_math = 0
    for r in results:
        name = os.path.basename(r["font"])
        if "error" in r:
            errors.append((name, r["sample"], r["error"]))
            continue
        if r["has_ours"]:
            with_math += 1
        for mode in ("retain", "renumber"):
            if r[mode]:
                differ.append((name, r["sample"], mode, r[mode]))
        if r["missed"]:
            missed.append((name, r["sample"], r["missed"]))
        kept += r["kept"]
        extra_glyphs += r["extra"]
    print("check-oracle-subset-math: %d fonts, %d subsets (%d keep a MATH table), %d differ, "
          "%d did not open, %d refused, %d declined"
          % (len(fonts), len(results), with_math, len(differ), len(errors), len(refused),
             len(declined)))
    print("  glyph sets: fontTools' own closure keeps a glyph this does not in %d subsets; "
          "this keeps %d more than it (of %d)" % (len(missed), extra_glyphs, kept))
    for name, si, mode, why in differ[:20 if quiet else 200]:
        print("  DIFFERENT %s sample %d (%s): %s" % (name, si, mode, why))
    for name, si, why in errors[:20]:
        print("  ERROR %s sample %d: %s" % (name, si, why))
    for font, si, why in refused[:20]:
        print("  REFUSED %s sample %d: %s" % (os.path.basename(font), si, why))
    for name, si, glyphs in missed[:20]:
        print("  MISSED %s sample %d: fontTools keeps %s" % (name, si, glyphs))
    for font, si, why in declined[:5]:
        print("  declined %s sample %d: %s" % (os.path.basename(font), si, why))
    ok = not differ and not errors and not refused and not missed and with_math
    if not ok:
        print("check-oracle-subset-math: FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
