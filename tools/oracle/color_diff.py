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
"""`CPAL` and `COLR`, this library against fontTools.

documentation/design.md sections 7.6 and 14. For every font that has either
table - the committed color fixtures, and any color font the oracle image holds -
`font-color` prints what this library reads (every palette, every color, every
label, and the layers of every glyph that has any) and fontTools' own decompiler
is asked the same, in the fonttools image. The two JSON answers must be equal.

**The population is the fixtures.** The image's corpus holds no font with `COLR`
or `CPAL`: this script scans it for them and reports how many it found, so that
the day a font appears it is compared without an edit, and the report says the
number of real fonts compared rather than leaving it to be assumed.

Usage:
    color_diff.py [--quiet] [font...]
"""

import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import corpus
import oracle_env

ROOT = oracle_env.ROOT
DRIVER = os.path.join(ROOT, "build", "linux", "release", "apps", "examples",
                      "font-color")
FIXTURES = os.path.join(ROOT, "tests", "data", "fonts")
SCRATCH = os.path.join(ROOT, "build", "oracle", "color")


def has_color_tables(path):
    """Whether the sfnt directory names COLR or CPAL, read here, not by either side."""
    try:
        with open(path, "rb") as handle:
            head = handle.read(12)
            count = int.from_bytes(head[4:6], "big")
            tags = {handle.read(16)[:4] for _ in range(count)}
    except (OSError, ValueError):
        return False
    return b"COLR" in tags or b"CPAL" in tags


def main(argv):
    quiet = "--quiet" in argv
    rest = [a for a in argv[1:] if not a.startswith("--")]
    if rest:
        fonts = [os.path.abspath(a) for a in rest]
        real = 0
    else:
        corpus_fonts = [f for f in corpus.fonts("sfnt") if has_color_tables(f)]
        real = len(corpus_fonts)
        fixtures = [os.path.join(FIXTURES, n) for n in sorted(os.listdir(FIXTURES))
                    if n.endswith((".ttf", ".otf")) and has_color_tables(
                        os.path.join(FIXTURES, n))]
        fonts = fixtures + corpus_fonts
    os.makedirs(SCRATCH, exist_ok=True)
    out = os.path.join(SCRATCH, "fonttools.json")
    argv_ref = oracle_env.command("fonttools", [
        "python3", os.path.join(ROOT, "tools", "oracle", "fonttools_color.py"),
        out] + fonts, scratch=SCRATCH)
    finished = subprocess.run(argv_ref, capture_output=True, text=True)
    if finished.returncode != 0:
        raise oracle_env.OracleUnavailable(
            "fonttools_color failed: %s" % finished.stderr.strip()[-300:])
    with open(out, encoding="utf-8") as handle:
        want = json.load(handle)
    failed = 0
    for font in fonts:
        ours = subprocess.run([DRIVER, font], capture_output=True, text=True)
        try:
            got = json.loads(ours.stdout)
        except ValueError:
            got = {"error": ours.stderr.strip()[:200] or "no answer"}
        ref = want[font]
        if got != ref:
            failed += 1
            print("DIFF %s" % os.path.relpath(font, ROOT))
            for key in sorted(set(got) | set(ref)):
                if got.get(key) != ref.get(key):
                    print("     %s: %s vs fontTools %s" % (
                        key, json.dumps(got.get(key))[:160],
                        json.dumps(ref.get(key))[:160]))
    glyphs = sum(len((want[f].get("layers") or {})) for f in fonts)
    print("fonts compared %d (%d from the image's corpus), %d color glyphs, "
          "%d differing" % (len(fonts), real, glyphs, failed))
    return 1 if failed or not fonts else 0


if __name__ == "__main__":
    try:
        oracle_env.provenance(["fonttools"])
        sys.exit(main(sys.argv))
    except oracle_env.OracleUnavailable as why:
        sys.stderr.write("SKIPPED color_diff: %s\n" % why)
        sys.exit(1 if os.environ.get("GHOTI_ORACLE_REQUIRED") == "1" else 0)
