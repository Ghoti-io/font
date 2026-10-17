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
"""Generate TrueType fonts whose glyphs run small random hinting programs.

    hint_probe_font.py <out-dir> <seed> <fonts> <glyphs-per-font> [max-steps] [axis|any] [composites]

Run inside the fontTools image, for hint_probe_diff.py. A hand-written glyph
program exercises what its author thought of; a random program of valid
instructions over a few points exercises every combination of vectors,
reference points, rounding states and flags, and a disagreement with FreeType
on one is already small enough to read. The programs are valid: every point
index is inside the glyph, every control value index inside the table, so a
difference is a difference in what an instruction means and not in how a
malformed program is tolerated (that is a separate comparison).

The fonts are written to <out-dir>/probe-<seed>-<n>.ttf.
"""

import os
import random
import sys

from fontTools.fontBuilder import FontBuilder
from fontTools.ttLib import TTFont
from fontTools.ttLib.tables import ttProgram
from fontTools.ttLib.tables._g_l_y_f import Glyph, GlyphCoordinates

POINTS = 12
CVT = 16


def push(values):
    return "PUSH[ ] " + " ".join(str(int(v)) for v in values)


class Maker:
    def __init__(self, rng, upem, oblique=True):
        self.rng = rng
        self.upem = upem
        self.oblique = oblique

    def point(self):
        return self.rng.randrange(POINTS)

    def cvt(self):
        return self.rng.randrange(CVT)

    def program(self, steps):
        r = self.rng
        asm = []
        # Setup: vectors, reference points, rounding.
        asm.append(r.choice(["SVTCA[0]", "SVTCA[1]", "SVTCA[0]", "SVTCA[1]"]))
        if steps > 1:
            for _ in range(r.randrange(0, 4)):
                asm.extend(self.step())
        else:
            # A reference point or two, so that one instruction has something to refer to.
            p0, p1, p2 = r.sample(range(POINTS), 3)
            asm.extend([push([p0]), "SRP0[ ]", push([p1]), "SRP1[ ]", push([p2]), "SRP2[ ]"])
        for _ in range(r.randrange(1, 2) if steps == 1 else r.randrange(2, steps + 1)):
            asm.extend(self.step())
        if steps > 1:
            for tail in r.choice([[], ["IUP[0]"], ["IUP[1]"], ["IUP[0]", "IUP[1]"]]):
                asm.append(tail)
        return asm

    def step(self):
        r = self.rng
        k = r.randrange(40)
        if not self.oblique and k in (38, 39):
            k = 0
        p = self.point
        c = self.cvt
        if k == 0:
            # Axis vectors only in pairs that agree: a freedom vector at right
            # angles to the projection vector has no defined movement.
            axis = r.randrange(2)
            return r.choice([["SVTCA[%d]" % axis], ["SPVTCA[%d]" % axis, "SFVTCA[%d]" % axis],
                             ["SPVTCA[%d]" % axis, "SFVTPV[ ]"]])
        if k == 1:
            return [push([p()]), r.choice(["SRP0[ ]", "SRP1[ ]", "SRP2[ ]"])]
        if k == 2:
            return [r.choice(["RTG[ ]", "RTHG[ ]", "RTDG[ ]", "RDTG[ ]", "RUTG[ ]", "ROFF[ ]"])]
        if k == 3:
            return [push([r.choice([0x48, 0x68, 0x40, 0x88, 0x4F, 0x58, 0x0B, 0xA3])]), "SROUND[ ]"]
        if k == 4:
            return [push([r.randrange(0, 200)]), r.choice(["SMD[ ]", "SCVTCI[ ]", "SSWCI[ ]"])]
        if k == 5:
            return [push([r.randrange(0, 300)]), "SSW[ ]"]
        if k == 6:
            return [r.choice(["FLIPON[ ]", "FLIPOFF[ ]"])]
        if k in (7, 8, 9):
            return [push([p()]), "MDAP[%d]" % r.randrange(2)]
        if k in (10, 11):
            return [push([p(), c()]), "MIAP[%d]" % r.randrange(2)]
        if k in (12, 13, 14, 15):
            return [push([p()]), "MDRP[%s]" % "".join(r.choice("01") for _ in range(5))]
        if k in (16, 17, 18, 19):
            return [push([p(), c()]), "MIRP[%s]" % "".join(r.choice("01") for _ in range(5))]
        if k == 20:
            return [push([p(), r.randrange(-100, 100)]), "MSIRP[%d]" % r.randrange(2)]
        if k == 21:
            return [push([p()]), "ALIGNRP[ ]"]
        if k in (22, 23):
            return [push([p()]), "IP[ ]"]
        if k == 24:
            return [push([p()]), "SHP[%d]" % r.randrange(2)]
        if k == 25:
            return [push([r.randrange(2)]), "SHC[%d]" % r.randrange(2)]
        if k == 26:
            return [push([1]), "SHZ[%d]" % r.randrange(2)]
        if k == 27:
            return [push([p(), r.randrange(-80, 80)]), "SHPIX[ ]"]
        if k == 28:
            return [push([p(), p()]), "ALIGNPTS[ ]"]
        if k == 29:
            return [push([p(), p(), p(), p(), p()]), "ISECT[ ]"]
        if k == 30:
            return [push([p()]), "GC[%d]" % r.randrange(2), push([p()]), "SWAP[ ]", "SCFS[ ]"]
        if k == 31:
            return [push([p(), p()]), "MD[%d]" % r.randrange(2), push([p()]), "SWAP[ ]", "SCFS[ ]"]
        if k == 32:
            return [push([p()]), "UTP[ ]"]
        if k == 33:
            n = r.randrange(1, 4)
            return [push([p() for _ in range(n)] + [n]), "SLOOP[ ]", r.choice(["ALIGNRP[ ]", "IP[ ]", "SHP[0]", "SHP[1]"])] if False else \
                [push([p() for _ in range(n)]), push([n]), "SLOOP[ ]", r.choice(["ALIGNRP[ ]", "IP[ ]", "SHP[0]", "SHP[1]"])]
        if k == 34:
            return [push([r.randrange(0, 4)]), "SDS[ ]"]
        if k == 35:
            return [push([r.randrange(6, 13)]), "SDB[ ]"]
        if k in (36, 37):
            args = []
            for _ in range(r.randrange(1, 3)):
                args.extend([r.randrange(256), p()])
            return [push(args + [len(args) // 2]), r.choice(["DELTAP1[ ]", "DELTAP2[ ]", "DELTAP3[ ]"])]
        if k == 38:
            return [push([p(), p()]), r.choice(["SPVTL[0]", "SPVTL[1]", "SFVTL[0]", "SFVTL[1]", "SDPVTL[0]", "SDPVTL[1]"])]
        return [push([r.randrange(-9000, 9000), r.randrange(-9000, 9000)]), r.choice(["SPVFS[ ]", "SFVFS[ ]"])]


def make_font(rng, path, glyph_count, steps, oblique=True, composites=0):
    upem = rng.choice([1000, 2048])
    maker = Maker(rng, upem, oblique)
    order = [".notdef"] + ["g%d" % i for i in range(1, glyph_count + composites)]
    glyphs = {}
    listing = {}
    shape = {}
    for index, name in enumerate(order):
        g = Glyph()
        if index >= glyph_count:
            # A composite of one to three earlier glyphs (simple or composite).
            from fontTools.ttLib.tables._g_l_y_f import GlyphComponent
            g.numberOfContours = -1
            g.components = []
            for _ in range(rng.randrange(1, 4)):
                comp = GlyphComponent()
                comp.glyphName = order[rng.randrange(1, index)]
                comp.flags = 0x2 | (0x4 if rng.random() < 0.4 else 0) | (0x200 if rng.random() < 0.2 else 0)
                if rng.random() < 0.15:
                    comp.flags |= rng.choice([0x800, 0x1000])
                comp.x, comp.y = rng.randrange(-200, 400), rng.randrange(-200, 400)
                if rng.random() < 0.25:
                    comp.transform = rng.choice([
                        [[0.5, 0], [0, 0.5]], [[1.0, 0], [0, 0.75]], [[0.866, 0.5], [-0.5, 0.866]],
                        [[1, 0.25], [0, 1]], [[0.75, 0.1], [-0.1, 0.75]]])
                g.components.append(comp)
            shape[name] = [[c.glyphName, c.x, c.y, c.flags, getattr(c, 'transform', None)] for c in g.components]
            if rng.random() < 0.6:
                asm = maker.program(steps)
            else:
                asm = []
            listing[name] = asm
            prog = ttProgram.Program()
            prog.fromAssembly(asm)
            g.program = prog
            glyphs[name] = g
            continue
        coords = []
        flags = []
        # Two contours of POINTS/2 points, spread over the em with some structure
        # (so that moves are not all degenerate): x and y on a coarse grid plus noise.
        for i in range(POINTS):
            x = rng.randrange(0, upem * 3 // 4)
            y = rng.randrange(-upem // 5, upem * 4 // 5)
            if rng.random() < 0.4:
                x = x // 50 * 50
                y = y // 50 * 50
            coords.append((x, y))
            flags.append(1 if rng.random() < 0.7 else 0)
        g.numberOfContours = 2
        g.coordinates = GlyphCoordinates(coords)
        g.flags = bytearray(flags)
        g.endPtsOfContours = [POINTS // 2 - 1, POINTS - 1]
        prog = ttProgram.Program()
        asm = maker.program(steps)
        listing[name] = asm
        shape[name] = [[c[0], c[1], f] for c, f in zip(coords, flags)]
        prog.fromAssembly(asm)
        g.program = prog
        glyphs[name] = g
    fb = FontBuilder(upem, isTTF=True)
    fb.setupGlyphOrder(order)
    fb.setupCharacterMap({0x41 + i: n for i, n in enumerate(order[1:60])})
    fb.setupGlyf(glyphs)
    fb.setupHorizontalMetrics({n: (upem // 2, glyphs[n].xMin if hasattr(glyphs[n], "xMin") else 0) for n in order})
    fb.setupHorizontalHeader(ascent=int(upem * 0.8), descent=-int(upem * 0.2))
    fb.setupNameTable({"familyName": "Probe", "styleName": "Regular"})
    fb.setupOS2(sTypoAscender=int(upem * 0.8), sTypoDescender=-int(upem * 0.2),
                usWinAscent=int(upem * 0.8), usWinDescent=int(upem * 0.2))
    fb.setupPost()
    fb.font["cvt "] = ttCvt = __import__("fontTools.ttLib", fromlist=["newTable"]).newTable("cvt ")
    import array
    ttCvt.values = array.array("h", [rng.choice([rng.randrange(-300, 900), rng.randrange(20, 200), 0]) for _ in range(CVT)])
    prep = __import__("fontTools.ttLib", fromlist=["newTable"]).newTable("prep")
    prep.program = ttProgram.Program()
    prep.program.fromAssembly(["PUSH[ ] 0", "POP[ ]"])
    fb.font["prep"] = prep
    maxp = fb.font["maxp"]
    maxp.maxZones = 2
    maxp.maxTwilightPoints = 16
    maxp.maxStorage = 16
    maxp.maxFunctionDefs = 0
    maxp.maxInstructionDefs = 0
    maxp.maxStackElements = 512
    maxp.maxSizeOfInstructions = 1024
    fb.save(path)
    import json
    with open(path + ".json", "w") as handle:
        json.dump({"programs": listing, "points": shape, "upem": upem,
                   "cvt": list(ttCvt.values)}, handle)


def main(argv):
    out, seed, fonts, glyphs = argv[1], int(argv[2]), int(argv[3]), int(argv[4])
    steps = int(argv[5]) if len(argv) > 5 else 8
    oblique = not (len(argv) > 6 and argv[6] == "axis")
    composites = int(argv[7]) if len(argv) > 7 else 0
    os.makedirs(out, exist_ok=True)
    for n in range(fonts):
        rng = random.Random(seed * 100003 + n)
        make_font(rng, os.path.join(out, "probe-%d-%d.ttf" % (seed, n)), glyphs, steps, oblique, composites)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
