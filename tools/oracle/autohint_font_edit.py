#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Font.
"""Move points of one glyph of a real face and hint the edited face with this library and with FreeType.

    autohint_font_edit.py FONT CHAR PPEM[,PPEM...] "i:dy[,i:dy...]" ["..." ...]

Each edit string is one variant: a point index or range (`4`, `38-65`) and the number of font units to add to its y
(`4:30`) or, with an x, to its x (`4:x30`); after a `|` the points whose y is printed (default: the edited ones). The face keeps its own
zones and every other glyph, so a rule that depends on the whole face (zones from real letters) can be probed with a
real outline instead of a hand-built one. Prints the y of the edited points, ours then FreeType's. Needs the fontTools
and FreeType images (see autohint_probe.py).
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import corpus
import hint_cff_diff
import hint_diff
import oracle_env

ROOT = os.path.dirname(os.path.dirname(HERE))
SCRATCH = os.path.join(ROOT, "build", "scratch", "probe")

EDIT = '''
import sys
from fontTools.ttLib import TTFont
src, out, ch, spec = sys.argv[1:5]
f = TTFont(src)
name = f.getBestCmap()[ord(ch)]
from fontTools.ttLib.tables._g_l_y_f import Glyph, GlyphCoordinates
src_glyph = f["glyf"][name]
cs, ends, flags = src_glyph.getCoordinates(f["glyf"])
g = Glyph()
g.numberOfContours = len(ends)
g.coordinates = GlyphCoordinates(list(cs))
g.flags = bytearray(x & 1 for x in flags)
g.endPtsOfContours = list(ends)
g.program = __import__("fontTools.ttLib.tables.ttProgram", fromlist=["Program"]).Program()
g.program.fromBytecode(b"")
f["glyf"][name] = g
coords = [list(c) for c in g.coordinates]
for item in spec.split("|")[0].split(","):
    if item:
        i, d = item.split(":")
        lo, _, hi = i.partition("-")
        for k in range(int(lo), int(hi or lo) + 1):
            if d.startswith("x"):
                coords[k][0] += int(d[1:])
            else:
                coords[k][1] += int(d)
g.coordinates = GlyphCoordinates([tuple(c) for c in coords])
g.recalcBounds(f["glyf"])
if "hdmx" in f: del f["hdmx"]
f.save(out)
print(f.getGlyphOrder().index(name))
'''


def main(argv):
    if len(argv) < 5:
        print(__doc__)
        return 2
    name, ch = argv[1], argv[2]
    ppems = [int(v) for v in argv[3].split(",")]
    path = [p for p in corpus.fonts("sfnt") if p.endswith("/" + name)][0]
    os.makedirs(SCRATCH, exist_ok=True)
    for n, spec in enumerate(argv[4:]):
        out = os.path.join(SCRATCH, "edit%d.ttf" % n)
        script = os.path.join(SCRATCH, "edit.py")
        open(script, "w").write(EDIT)
        done = subprocess.run(oracle_env.command("fonttools", ["python3", script, path, out, ch, spec], scratch=SCRATCH),
                              capture_output=True, text=True, stdin=subprocess.DEVNULL)
        if done.returncode != 0:
            raise SystemExit(done.stderr[-600:])
        gid = int(done.stdout.split()[-1])
        if "|" in spec:
            points = [int(i) for i in spec.split("|")[1].split(",") if i]
        else:
            points = [int(i.split(":")[0].split("-")[0]) for i in spec.split("|")[0].split(",") if i]
        for ppem in ppems:
            mine = hint_cff_diff.ours(out, ppem, 1, "-", True)
            theirs, _ = hint_diff.theirs(out, 35, ppem, 1, "-", "autolight")
            a, b = mine[gid][1], theirs[gid][1]
            ys_a, ys_b = [a[k][1] for k in points], [b[k][1] for k in points]
            print("%-28s ppem %2d ours %s theirs %s%s" % (spec, ppem, ys_a, ys_b, "" if ys_a == ys_b else "   <-- differs"))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
