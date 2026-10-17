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
"""Write a TrueType font from explicit hinting cases, for hint_case.py.

    hint_case_font.py <out.ttf> <cases.json>

The JSON is {"upem": N, "cvt": [...], "prep": [asm], "fpgm": [asm],
"maxp": {...}, "cases": [{"points": [[x, y, on]...], "ends": [...],
"asm": [asm lines]}...]}; every case becomes one glyph, in order, after
.notdef. For pinning one instruction's meaning down to a value rather than
sampling it, where hint_probe_font.py samples.
"""

import array
import json
import sys

from fontTools.fontBuilder import FontBuilder
from fontTools.ttLib import newTable
from fontTools.ttLib.tables import ttProgram
from fontTools.ttLib.tables._g_l_y_f import Glyph, GlyphCoordinates


def program(lines):
    prog = ttProgram.Program()
    prog.fromAssembly(lines)
    return prog


def main(argv):
    spec = json.load(open(argv[2]))
    upem = spec.get("upem", 1000)
    order = [".notdef"] + ["c%d" % i for i in range(len(spec["cases"]))]
    glyphs = {}
    empty = Glyph()
    empty.numberOfContours = 0
    glyphs[".notdef"] = empty
    for name, case in zip(order[1:], spec["cases"]):
        g = Glyph()
        if "components" in case:
            # A composite: [[case index, dx, dy, flags]...], flags a dict of
            # fontTools GlyphComponent flag names to set (ROUND_XY_TO_GRID, ...).
            from fontTools.ttLib.tables._g_l_y_f import GlyphComponent
            g.numberOfContours = -1
            g.components = []
            for entry in case["components"]:
                target, dx, dy, flags = entry[:4]
                comp = GlyphComponent()
                comp.glyphName = order[1 + target]
                if flags & 0x2:
                    comp.x, comp.y = dx, dy
                else:
                    comp.firstPt, comp.secondPt = dx, dy
                comp.flags = flags
                if len(entry) > 4:
                    comp.transform = [list(row) for row in entry[4]]
                g.components.append(comp)
            g.program = program(case["asm"])
            glyphs[name] = g
            continue
        pts = case["points"]
        g.numberOfContours = len(case["ends"])
        g.coordinates = GlyphCoordinates([(p[0], p[1]) for p in pts])
        g.flags = bytearray(p[2] for p in pts)
        g.endPtsOfContours = case["ends"]
        g.program = program(case["asm"])
        glyphs[name] = g
    fb = FontBuilder(upem, isTTF=True)
    fb.setupGlyphOrder(order)
    fb.setupCharacterMap({0x41 + i: n for i, n in enumerate(order[1:60])})
    fb.setupGlyf(glyphs)
    # The left side bearing is the glyph's own xMin, so the origin phantom point
    # sits at x = 0 and nothing is shifted to put it there.
    bearing = {n: case.get('bearing', min(p[0] for p in case['points']) if case.get('points') else 0)
               for n, case in zip(order[1:], spec['cases'])}
    bearing['.notdef'] = 0
    advance = {n: case.get('advance', upem // 2) for n, case in zip(order[1:], spec['cases'])}
    advance['.notdef'] = upem // 2
    fb.setupHorizontalMetrics({n: (advance[n], bearing[n]) for n in order})
    fb.setupHorizontalHeader(ascent=int(upem * 0.8), descent=-int(upem * 0.2))
    fb.setupNameTable({"familyName": "Case", "styleName": "Regular"})
    fb.setupOS2(sTypoAscender=int(upem * 0.8), sTypoDescender=-int(upem * 0.2),
                usWinAscent=int(upem * 0.8), usWinDescent=int(upem * 0.2))
    fb.setupPost()
    if spec.get("cvt"):
        cvt = newTable("cvt ")
        cvt.values = array.array("h", spec["cvt"])
        fb.font["cvt "] = cvt
    # A font without a control value program is hinted by FreeType with a zeroed
    # graphics state (no auto-flip, minimum distance 0), which is not what the
    # specification's defaults say; so every case font has one, empty in effect.
    t = newTable("prep")
    t.program = program(spec.get("prep") or ["PUSH[ ] 0", "POP[ ]"])
    fb.font["prep"] = t
    if spec.get("fpgm"):
        t = newTable("fpgm")
        t.program = program(spec["fpgm"])
        fb.font["fpgm"] = t
    maxp = fb.font["maxp"]
    maxp.maxZones = 2
    maxp.maxTwilightPoints = spec.get("twilight", 16)
    maxp.maxStorage = spec.get("storage", 16)
    maxp.maxFunctionDefs = spec.get("functions", 8)
    maxp.maxInstructionDefs = 0
    maxp.maxStackElements = 512
    maxp.maxSizeOfInstructions = 1024
    if spec.get("axes"):
        # A variable face with nothing to vary: the interpreter's view of it is what is
        # compared (what GETVARIATION and GETINFO answer), not any glyph's motion.
        axes = [("wght", 100, 400, 900, "Weight"), ("wdth", 50, 100, 200, "Width")][:spec["axes"]]
        fb.setupFvar(axes=axes, instances=[])
        fb.setupGvar({n: [] for n in order})
        if spec.get("cvar"):
            from fontTools.ttLib.tables.TupleVariation import TupleVariation
            cv = newTable("cvar")
            cv.version = 1
            cv.variations = [TupleVariation({"wght": (0, 1, 1)}, list(spec["cvar"])),
                             TupleVariation({"wght": (-1, -1, 0)}, [-d if d is not None else None for d in spec["cvar"]])]
            fb.font["cvar"] = cv
    fb.save(argv[1])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
