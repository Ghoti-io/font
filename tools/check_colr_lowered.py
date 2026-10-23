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
"""Every color glyph of the real fonts lowers, and the lowered lists hash to what is committed (L12 b and c).

documentation/design.md section 8.8. For every font with a `COLR` table - the committed
fixtures, the image's real color fonts and the pinned real fonts of `colr_real_fonts.py`
(Noto Color Emoji's COLR version 1 build, Nabla) - `font-colr-scene --summary` lowers every color
glyph and prints what was refused and a hash of the whole font's lowered lists (a fold of each
glyph's number, outcome, list content hash and item count). Two committed files are the
expectation:

  * `tests/data/golden/colr-refusals.txt`: every refusal that is expected, one line a glyph -
    `<font> <instance> <glyph> <unbounded|no-record|limit> <paint format>` - the glyphs a font states
    no extent for (a solid or gradient no glyph bounds in a glyph with no clip box) and the ones
    that name themselves. The gate fails on a refusal not on the list **and** when the list changes
    (a glyph on it that now lowers): the list is the denominator of "every glyph lowers", so it
    moves only by being regenerated, deliberately.
  * `tests/data/golden/colr-lowered.hash`: `<font> <instance> <color glyphs> <lowered> <hash>`. The hash
    is integers and content hashes, so it is the same on every target the lowering is; the value
    committed here is measured on x86-64, and the other targets are checked by the cross-target
    gates against it.

The gate carries its own control: after the comparison, one committed line is changed in memory and
fed back through the same comparison, which must report a difference or the gate fails (a `==` that
has rotted into comparing a file with itself would report every font clean).

Usage:
    check_colr_lowered.py            compare
    check_colr_lowered.py --update   regenerate both files (deliberate: the diff belongs in a commit that says why)
"""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "oracle"))
import colr_real_fonts
import colr_scene_diff
import corpus
import oracle_env

ROOT = oracle_env.ROOT
DRIVER = colr_scene_diff.DRIVER
GOLDEN = os.path.join(ROOT, "tests", "data", "golden")
REFUSALS = os.path.join(GOLDEN, "colr-refusals.txt")
HASHES = os.path.join(GOLDEN, "colr-lowered.hash")
REASONS = {"1": "unbounded", "2": "no-record"}
# A variable font is also lowered at one instance (normalised 2.14 coordinates): the paints move with it.
INSTANCE = ("half", ["8192", "-8192"])


def fonts():
    fixtures = [os.path.join(colr_scene_diff.FIXTURES, n) for n in sorted(os.listdir(colr_scene_diff.FIXTURES))
                if n.endswith((".ttf", ".otf")) and colr_scene_diff.has_colr(os.path.join(colr_scene_diff.FIXTURES, n))]
    image = [f for f in corpus.fonts("sfnt") if colr_scene_diff.has_colr(f)]
    return fixtures + image + colr_real_fonts.ensure()


def summarize(font, name, coords):
    run = subprocess.run([DRIVER, font, "--summary"] + coords, capture_output=True, text=True)
    refusals = []
    summary = None
    for line in run.stdout.splitlines():
        parts = line.split()
        if parts[:1] == ["refused"]:
            refusals.append((int(parts[2 - 1]), REASONS.get(parts[2], parts[2]), parts[3]))
        elif parts[:1] == ["limit"]:
            refusals.append((int(parts[1]), "limit", "0"))
        elif parts[:1] == ["summary"]:
            summary = dict(p.split("=") for p in parts[1:])
    if summary is None:
        raise RuntimeError("%s: no summary (%s)" % (os.path.basename(font), run.stderr.strip()[:200]))
    return refusals, summary


def measure():
    refusal_lines = []
    hash_lines = []
    for font in fonts():
        base = os.path.basename(font)
        instances = [("default", [])]
        if colr_scene_diff.has_fvar(font):
            instances.append(INSTANCE)
        for name, coords in instances:
            refusals, s = summarize(font, name, coords)
            for gid, reason, fmt in sorted(refusals):
                refusal_lines.append("%s %s %d %s %s" % (base, name, gid, reason, fmt))
            hash_lines.append("%s %s %s %s %s" % (base, name, s["glyphs"], s["lowered"], s["hash"]))
    return refusal_lines, hash_lines


def read(path):
    with open(path, encoding="utf-8") as handle:
        return [l.rstrip("\n") for l in handle if l.strip() and not l.startswith("#")]


HEADER_REFUSALS = ("# Every refusal the lowering is expected to make, one line a glyph:\n"
                   "# <font> <instance> <glyph> <unbounded|no-record|limit> <paint format>\n"
                   "# See tools/check_colr_lowered.py; regenerate with --update.\n")
HEADER_HASHES = ("# The whole-font hash of every real color font's lowered lists (x86-64):\n"
                 "# <font> <instance> <color glyphs> <lowered> <hash>\n"
                 "# See tools/check_colr_lowered.py; regenerate with --update.\n")


def compare(want, got, what):
    """The differences between the committed lines and the measured ones, as readable strings."""
    out = []
    for line in sorted(set(want) - set(got)):
        out.append("%s: committed but not measured: %s" % (what, line))
    for line in sorted(set(got) - set(want)):
        out.append("%s: measured but not committed: %s" % (what, line))
    return out


def main(argv):
    try:
        refusal_lines, hash_lines = measure()
    except (colr_real_fonts.FetchFailed, oracle_env.OracleUnavailable) as why:
        sys.stderr.write("SKIPPED check_colr_lowered: %s\n" % why)
        return 1 if os.environ.get("GHOTI_ORACLE_REQUIRED") == "1" else 0
    if "--update" in argv:
        with open(REFUSALS, "w", encoding="utf-8") as out:
            out.write(HEADER_REFUSALS + "".join(l + "\n" for l in refusal_lines))
        with open(HASHES, "w", encoding="utf-8") as out:
            out.write(HEADER_HASHES + "".join(l + "\n" for l in hash_lines))
        print("check_colr_lowered: wrote %d refusals and %d font hashes" % (len(refusal_lines), len(hash_lines)))
        return 0
    want_refusals = read(REFUSALS)
    want_hashes = read(HASHES)
    problems = compare(want_refusals, refusal_lines, "refusals") + compare(want_hashes, hash_lines, "hashes")
    # The control: one committed line changed in memory must be seen by the same comparison.
    if want_hashes:
        mutated = [want_hashes[0][:-1] + ("0" if want_hashes[0][-1] != "0" else "1")] + want_hashes[1:]
        if not compare(mutated, hash_lines, "control"):
            problems.append("the comparison is blind: a changed hash was not seen")
    if want_refusals and not compare(want_refusals[1:], refusal_lines, "control"):
        problems.append("the comparison is blind: a dropped refusal was not seen")
    for p in problems:
        print("FAIL " + p)
    lowered = sum(int(l.split()[3]) for l in hash_lines)
    total = sum(int(l.split()[2]) for l in hash_lines)
    print("check_colr_lowered: %d fonts-and-instances, %d color glyphs, %d lowered, %d refused (all on the committed list), "
          "%d problems" % (len(hash_lines), total, lowered, len(refusal_lines), len(problems)))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
