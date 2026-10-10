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
"""What fontTools says a font asks of a line and of each glyph, for layout_diff.py.

    fonttools_layout.py <in.json> <out.json>

`in.json` maps a font path to a list of glyph ids. For each font the answer has
the units per em, the ascent, descent (negative) and line gap the font asks for
under this library's default policy - OS/2 typographic metrics when `fsSelection`
bit 7 is set, `hhea` otherwise - and the exact outline bounds, in font units, of
each listed glyph (`BoundsPen`, which solves the curves; a glyph with no ink is
`null`). Runs in the fonttools image.
"""

import json
import sys

from fontTools.pens.boundsPen import BoundsPen
from fontTools.ttLib import TTFont


def describe(path, gids):
    font = TTFont(path, fontNumber=0, lazy=True)
    upem = font["head"].unitsPerEm
    hhea = font["hhea"] if "hhea" in font else None
    os2 = font["OS/2"] if "OS/2" in font else None
    if os2 is not None and (os2.fsSelection & (1 << 7)):
        asc, desc, gap = os2.sTypoAscender, os2.sTypoDescender, os2.sTypoLineGap
    elif hhea is not None:
        asc, desc, gap = hhea.ascent, hhea.descent, hhea.lineGap
    elif os2 is not None:
        asc, desc, gap = os2.usWinAscent, -os2.usWinDescent, 0
    else:
        asc = desc = gap = 0
    order = font.getGlyphOrder()
    glyphs = font.getGlyphSet()
    bounds = {}
    for gid in gids:
        if gid >= len(order):
            bounds[str(gid)] = None
            continue
        pen = BoundsPen(glyphs)
        try:
            glyphs[order[gid]].draw(pen)
        except Exception:
            bounds[str(gid)] = None
            continue
        bounds[str(gid)] = list(pen.bounds) if pen.bounds else None
    return {"upem": upem, "asc": asc, "desc": desc, "gap": gap, "bounds": bounds}


def main(argv):
    with open(argv[1], encoding="utf-8") as handle:
        wanted = json.load(handle)
    out = {}
    for path, gids in wanted.items():
        try:
            out[path] = describe(path, gids)
        except Exception as why:
            out[path] = None
            sys.stderr.write("%s: %s\n" % (path, why))
    with open(argv[2], "w", encoding="utf-8") as handle:
        json.dump(out, handle)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
