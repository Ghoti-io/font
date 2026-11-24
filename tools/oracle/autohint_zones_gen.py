#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Font.
"""Write the zone strings of a script, from FreeType's answers, as the C source of its autofit.c entry.

    autohint_zones_gen.py RECORDS.jsonl [--max N]

RECORDS is what `autohint_diff.py --script RANGE --records RECORDS FONT` writes at several sizes. The tops FreeType snapped to whole
pixels are grouped by height (see autohint_zones_discover.py); each group of three or more letters is a top zone, and the letters
whose extreme is inside the group's height range, snapped or not, are its string. The same for bottoms, with the baseline zone being
every letter whose bottom is within three per cent of an em of zero. Prints one initialiser, zones in the order CAP_TOP, CAP_BOT,
F_TOP, SMALL_TOP, SMALL_BOT, DESC, with the tallest top group first.
"""
import collections
import json
import sys


def clusters(items, tol):
    items = sorted(items)
    groups = []
    for y, ch in items:
        if groups and y - groups[-1][-1][0] <= tol:
            groups[-1].append((y, ch))
        else:
            groups.append([(y, ch)])
    return [g for g in groups if len(g) >= 3]


def zones(path, cap=40):
    """(top, base, tall, descender strings, standard letter, a description) for one script's records."""
    glyphs = {}
    votes_top = collections.Counter()
    votes_bot = collections.Counter()
    upem = 1000
    for line in open(path):
        r = json.loads(line)
        upem = r["upem"]
        ys = [p[1] for p in r["pts"]]
        if not ys:
            continue
        glyphs[r["ch"]] = (max(ys), min(ys))
        s = r["ppem"] * 64.0 / r["upem"]
        theirs = [p[1] for p in r["theirs"]]
        if not theirs:
            continue
        tt, tb = max(theirs), min(theirs)
        if tt % 64 == 0 and abs(round(max(ys) * s) - tt) >= 2:
            votes_top[r["ch"]] += 1
        if tb % 64 == 0 and abs(round(min(ys) * s) - tb) >= 2:
            votes_bot[r["ch"]] += 1
    tol = 14 * upem / 1000.0
    tops = clusters([(glyphs[c][0], c) for c, v in votes_top.items() if v >= 2], tol)
    tops.sort(key=lambda g: -g[len(g) // 2][0])
    bottoms = clusters([(glyphs[c][1], c) for c, v in votes_bot.items() if v >= 2], tol)

    def letters(lo, hi, which):
        return sorted(c for c, g in glyphs.items() if lo <= g[which] <= hi)

    def text(chars):
        step = max(1, len(chars) // cap)
        return "".join(chars[::step])

    top_strings = []
    for g in tops[:3]:
        lo = g[0][0] - tol
        hi = g[-1][0] + tol
        top_strings.append(text(letters(lo, hi, 0)))
    base = [c for c, g in glyphs.items() if abs(g[1]) <= 0.03 * upem]
    desc = [g for g in bottoms if g[len(g) // 2][0] < -0.06 * upem]
    desc_string = text(letters(desc[0][0][0] - tol, desc[-1][-1][0] + tol, 1)) if desc else ""
    while len(top_strings) < 3:
        top_strings.append("")
    note = ("tops " + "; ".join("%d..%d n=%d" % (g[0][0], g[-1][0], len(g)) for g in tops)
            + " | bottoms " + "; ".join("%d..%d n=%d" % (g[0][0], g[-1][0], len(g)) for g in bottoms))
    standard = sorted(base)[0] if base else (sorted(glyphs)[0] if glyphs else "o")
    return top_strings[0], text(sorted(base)), top_strings[1], desc_string, standard, note


def main(argv):
    cap = int(argv[argv.index("--max") + 1]) if "--max" in argv else 40
    top, base, tall, desc, standard, note = zones(argv[1], cap)
    # CAP_TOP, CAP_BOT, F_TOP, SMALL_TOP, SMALL_BOT, DESC
    print('  {{U"%s", U"%s", U"%s", U"", U"", U"%s"}, U\'%s\'},' % (top, base, tall, desc, standard))
    print("// " + note)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
