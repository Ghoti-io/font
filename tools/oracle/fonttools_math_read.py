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
"""What fontTools reads from a font's MATH table, spelled as `font-math` prints it.

Runs inside the fontTools image. Prints the lines documented at the top of examples/font-math.c.
"""

import sys

from fontTools.ttLib import TTFont

CONSTANTS = None


def main(path):
    font = TTFont(path)
    if "MATH" not in font:
        print("no MATH", file=sys.stderr)
        return 3
    order = font.getGlyphOrder()
    gid = {name: i for i, name in enumerate(order)}
    t = font["MATH"].table
    c = t.MathConstants
    # MathConstants in table order: fontTools names them in that order, with the value records
    # holding .Value and the plain ones a bare number.
    out = []
    fields = [f for f in c.__dict__ if not f.startswith("_")]
    # __dict__ order is the order fontTools decoded them in, which is the table's.
    assert len(fields) == 56, len(fields)
    for i, f in enumerate(fields):
        v = getattr(c, f)
        out.append("C %d %d" % (i, v.Value if hasattr(v, "Value") else v))
    var = t.MathVariants
    out.append("O %d" % (var.MinConnectorOverlap if var else 0))
    per = {}

    def add(g, line):
        per.setdefault(g, []).append(line)

    gi = t.MathGlyphInfo
    if gi:
        for tag, info, cov, recs in (
                ("I", gi.MathItalicsCorrectionInfo, "Coverage", "ItalicsCorrection"),
                ("T", gi.MathTopAccentAttachment, "TopAccentCoverage", "TopAccentAttachment")):
            if info and getattr(info, cov):
                for name, rec in zip(getattr(info, cov).glyphs, getattr(info, recs)):
                    add(gid[name], (0 if tag == "I" else 1, "%s %d %d" % (tag, gid[name], rec.Value)))
        if gi.ExtendedShapeCoverage:
            for name in gi.ExtendedShapeCoverage.glyphs:
                add(gid[name], (2, "X %d" % gid[name]))
        ki = gi.MathKernInfo
        if ki and ki.MathKernCoverage:
            for name, rec in zip(ki.MathKernCoverage.glyphs, ki.MathKernInfoRecords):
                for corner, attr in enumerate(("TopRightMathKern", "TopLeftMathKern",
                        "BottomRightMathKern", "BottomLeftMathKern")):
                    k = getattr(rec, attr)
                    if k is None:
                        continue
                    hs = " ".join(str(h.Value) for h in k.CorrectionHeight)
                    ks = " ".join(str(v.Value) for v in k.KernValue)
                    add(gid[name], (3 + corner, "K %d %d %s | %s" % (gid[name], corner, hs, ks)))
    if var:
        for vertical, cov, cons in ((True, var.VertGlyphCoverage, var.VertGlyphConstruction),
                (False, var.HorizGlyphCoverage, var.HorizGlyphConstruction)):
            if not cov:
                continue
            base = 7 if vertical else 9
            for name, con in zip(cov.glyphs, cons):
                if con is None:
                    continue
                g = gid[name]
                if con.MathGlyphVariantRecord:
                    s = " ".join("%d:%d" % (gid[r.VariantGlyph], r.AdvanceMeasurement)
                                 for r in con.MathGlyphVariantRecord)
                    add(g, (base, "%s %d %s" % ("V" if vertical else "H", g, s)))
                a = con.GlyphAssembly
                if a and a.PartRecords:
                    s = " ".join("%d:%d:%d:%d:%d" % (gid[p.glyph], p.StartConnectorLength,
                            p.EndConnectorLength, p.FullAdvance, p.PartFlags)
                            for p in a.PartRecords)
                    add(g, (base + 1, "%s %d %d %s" % ("A" if vertical else "B", g,
                            a.ItalicsCorrection.Value, s)))
    for line in out:
        print(line)
    for g in sorted(per):
        for _, line in sorted(per[g], key=lambda x: x[0]):
            print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
