#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Font.
"""Compare this library's reading of Adobe Font Metrics files with fontTools'.

    afm_diff.py [--quiet] [--binary PATH] FILE.afm...

`font-afm` (examples/font-afm.c) prints what the library read in thousandths; fontTools'
`afmLib` is asked the same question of the same file, and every header number, every
character's code, advance, box and name, and every kerning pair and amount must agree.
Differences are printed and make the run fail, and so does a file that neither reads, an
empty list of files, or a reference that has fewer than ten characters in a file (a
reference that reads nothing agrees with a reader that reads nothing).

fontTools folds a duplicated name into one entry, as a dictionary does, so a file that lists a
name twice is compared by the first of this library's entries for it, and the counts are
compared as sets.
"""
import argparse
import os
import subprocess
import sys
from decimal import Decimal, ROUND_HALF_UP

from fontTools.afmLib import AFM


def milli(value):
    """A number as thousandths, rounded half away from zero, as the library holds it."""
    d = Decimal(str(value)) * 1000
    return int(d.quantize(Decimal(1), rounding=ROUND_HALF_UP))


def ours(binary, path):
    run = subprocess.run([binary, path], capture_output=True, text=True)
    if run.returncode != 0:
        return None, run.stderr.strip()
    header, chars, kerns = {}, [], []
    for line in run.stdout.splitlines():
        kind, _, rest = line.partition(" ")
        if kind == "H":
            key, _, value = rest.partition(" ")
            header[key] = value
        elif kind == "C":
            f = rest.split(" ")
            chars.append({"code": int(f[1]), "wx": int(f[2]), "wy": int(f[3]), "name": f[4],
                          "bbox": tuple(int(v) for v in f[5:9]), "lig": " ".join(f[9:])})
        elif kind == "K":
            first, second, x = rest.split(" ")
            kerns.append((first, second, int(x)))
    return {"header": header, "chars": chars, "kerns": kerns}, None


def compare(path, binary):
    mine, err = ours(binary, path)
    if mine is None:
        return ["this library refused the file: " + err]
    try:
        ref = AFM(path)
    except Exception as e:   # the reference refusing it is a result, not a crash
        return ["fontTools refused the file: %s" % e]
    problems = []
    if len(ref._chars) < 10:
        problems.append("the reference read only %d characters" % len(ref._chars))
    attrs = ref._attrs
    h = mine["header"]
    for key in ("FontName", "FullName", "FamilyName", "Weight"):
        if h.get(key, "") != str(attrs.get(key, "")):
            problems.append("%s: %r against %r" % (key, h.get(key), attrs.get(key)))
    for key in ("ItalicAngle", "UnderlinePosition", "UnderlineThickness", "CapHeight", "XHeight",
                "Ascender", "Descender"):
        if key in attrs and int(h[key]) != milli(attrs[key]):
            problems.append("%s: %s against %s" % (key, h[key], attrs[key]))
    if "FontBBox" in attrs:
        got = tuple(int(v) for v in h["FontBBox"].split())
        want = tuple(milli(v) for v in attrs["FontBBox"])
        if got != want:
            problems.append("FontBBox: %s against %s" % (got, want))
    first = {}
    for c in mine["chars"]:
        first.setdefault(c["name"], c)
    if set(first) != set(ref._chars):
        problems.append("the character names differ: %d against %d" % (len(first), len(ref._chars)))
    for name, (code, width, bbox) in ref._chars.items():
        c = first.get(name)
        if c is None:
            continue
        if c["code"] != code or c["wx"] != milli(width) or c["bbox"] != tuple(milli(v) for v in bbox):
            problems.append("%s: %r against %r" % (name, (c["code"], c["wx"], c["bbox"]),
                                                   (code, width, bbox)))
    mine_pairs = {}
    for a, b, x in mine["kerns"]:
        mine_pairs.setdefault((a, b), x)
    if set(mine_pairs) != set(ref._kerning):
        problems.append("the kerning pairs differ: %d against %d" % (len(mine_pairs), len(ref._kerning)))
    for pair, x in ref._kerning.items():
        if pair in mine_pairs and mine_pairs[pair] != milli(x):
            problems.append("kern %s: %s against %s" % (pair, mine_pairs[pair], x))
    return problems


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--quiet", action="store_true")
    parser.add_argument("--binary", default=os.environ.get("GFNT_FONT_AFM", "build/linux/release/apps/examples/font-afm"))
    parser.add_argument("files", nargs="*")
    args = parser.parse_args()
    if not args.files:
        print("afm_diff: no files given (an empty comparison agrees with everything)", file=sys.stderr)
        return 1
    failed = 0
    for path in args.files:
        problems = compare(path, args.binary)
        if problems:
            failed += 1
            print("%s: %d difference(s)" % (path, len(problems)))
            for p in problems[:20]:
                print("    " + p)
        elif not args.quiet:
            print("%s: agrees" % path)
    print("afm_diff: %d file(s), %d differ" % (len(args.files), failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
