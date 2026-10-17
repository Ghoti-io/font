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

    hint_probe_font.py <out-dir> <seed> <fonts> <glyphs-per-font> [max-steps] [axis|any] [composites] [variable|variable-empty|variable-hvar]

Run inside the fontTools image, for hint_probe_diff.py. A hand-written glyph
program exercises what its author thought of; a random program of valid
instructions over a few points exercises every combination of vectors,
reference points, rounding states and flags, and a disagreement with FreeType
on one is already small enough to read. The programs are valid: every point
index is inside the glyph, every control value index inside the table, so a
difference is a difference in what an instruction means and not in how a
malformed program is tolerated (that is a separate comparison).

The fonts are written to <out-dir>/probe-<seed>-<n>.ttf. With `variable`, each also has
one weight axis, random `gvar` deltas on every glyph (some left to interpolate) and
random `cvar` deltas on the control values.
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

    wild = 0.0       # the chance an index is out of range, and that a step is a bare instruction

    def point(self):
        if self.wild and self.rng.random() < self.wild:
            return self.rng.choice([POINTS, POINTS + 1, POINTS + 4, POINTS + 9, 200, 30000, -1, -7])
        return self.rng.randrange(POINTS)

    def cvt(self):
        if self.wild and self.rng.random() < self.wild:
            return self.rng.choice([CVT, CVT + 3, 500, 30000, -1, -9])
        return self.rng.randrange(CVT)

    empty = False

    def program(self, steps):
        r = self.rng
        if self.empty:
            return []
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

    BARE = ["ADD[ ]", "POP[ ]", "MDAP[1]", "MDRP[00000]", "MIAP[0]", "SRP0[ ]", "RCVT[ ]", "IP[ ]",
            "SHP[0]", "MIRP[00000]", "ALIGNRP[ ]", "GC[0]", "MD[0]", "SWAP[ ]", "DUP[ ]", "SCFS[ ]",
            "MSIRP[0]", "ISECT[ ]", "SHPIX[ ]", "WCVTP[ ]", "RS[ ]", "WS[ ]", "UTP[ ]", "SHC[0]",
            "DELTAP1[ ]", "ALIGNPTS[ ]", "SCVTCI[ ]", "SSW[ ]", "SLOOP[ ]", "FLIPPT[ ]", "SDB[ ]"]

    def step(self):
        r = self.rng
        if self.wild and r.random() < self.wild:
            kind = r.randrange(5)
            if kind == 0:
                return [r.choice(self.BARE)]
            if kind == 1:
                return [push([r.randrange(-2, 40)]), r.choice(["RS[ ]", "POP[ ]", "CALL[ ]", "SLOOP[ ]", "SHC[0]", "SHZ[1]", "SRP1[ ]"])]
            if kind == 2:
                return [push([r.randrange(-2, 40), r.randrange(-50, 50)]), r.choice(["WS[ ]", "WCVTP[ ]"])]
            if kind == 3:
                return [push([r.randrange(-1, 8)]), "SHC[%d]" % r.randrange(2)]
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
            # Not on a phantom point: the reference interpreter lets a MIAP there leak into
            # the next glyph it hints (state this library keeps per glyph), which a comparison
            # of glyphs hinted one after another cannot tell from a defect.
            point = p()
            if POINTS <= point < POINTS + 4:
                point -= 6
            return [push([point, c()]), "MIAP[%d]" % r.randrange(2)]
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


HVAR = [False]
WILD = [0.0]


def add_variation(rng, fb, order, glyphs, glyph_count, cvt, hvar=False):
    """One weight axis; deltas for every glyph on both sides of the default."""
    from fontTools.ttLib import newTable
    from fontTools.ttLib.tables.TupleVariation import TupleVariation

    fb.setupFvar(axes=[("wght", 100, 400, 900, "Weight")], instances=[])
    gvar = {}
    for index, name in enumerate(order):
        glyph = glyphs[name]
        count = len(glyph.components) if glyph.isComposite() else POINTS
        tuples = []
        for peak in ("up", "down"):
            deltas = []
            for i in range(count):
                if not glyph.isComposite() and rng.random() < 0.25:
                    deltas.append(None)       # left for interpolation
                else:
                    deltas.append((rng.randrange(-70, 71), rng.randrange(-70, 71)))
            if not glyph.isComposite() and all(d is None for d in deltas):
                deltas[0] = (3, -2)
            phantoms = [(rng.randrange(-40, 41), 0), (rng.randrange(-40, 41), 0), (0, 0), (0, 0)]
            axis = {"wght": (0, 1, 1)} if peak == "up" else {"wght": (-1, -1, 0)}
            tuples.append(TupleVariation(axis, deltas + phantoms))
        gvar[name] = tuples
    fb.setupGvar(gvar)
    cvar = newTable("cvar")
    cvar.version = 1
    cvar.variations = []
    for axis in ({"wght": (0, 1, 1)}, {"wght": (-1, -1, 0)}):
        cvar.variations.append(TupleVariation(axis, [
            rng.randrange(-60, 61) if rng.random() < 0.7 else None for _ in range(CVT)]))
    fb.font["cvar"] = cvar
    if hvar:
        add_hvar(rng, fb, order)


def add_hvar(rng, fb, order):
    """An `HVAR` giving every glyph an advance delta (no side bearing mapping), so that a
    face has two sources for its advance and the hinter has to pick the right one."""
    from fontTools.ttLib import newTable
    from fontTools.ttLib.tables import otTables as ot
    from fontTools.varLib import builder

    supports = [{"wght": (0, 1, 1)}, {"wght": (-1, -1, 0)}]
    region_list = builder.buildVarRegionList(supports, ["wght"])
    data = builder.buildVarData([0, 1], [[rng.randrange(-60, 61), rng.randrange(-60, 61)] for _ in order], optimize=False)
    store = builder.buildVarStore(region_list, [data])
    table = newTable("HVAR")
    table.table = ot.HVAR()
    table.table.Version = 0x00010000
    table.table.VarStore = store
    table.table.AdvWidthMap = None
    table.table.LsbMap = None
    table.table.RsbMap = None
    fb.font["HVAR"] = table


def make_font(rng, path, glyph_count, steps, oblique=True, composites=0, variable=False, wild=0.0):
    upem = rng.choice([1000, 2048])
    maker = Maker(rng, upem, oblique)
    maker.wild = wild
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
    if variable:
        add_variation(rng, fb, order, glyphs, glyph_count, ttCvt, HVAR[0])
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
    variable = len(argv) > 8 and argv[8].startswith("variable")
    WILD[0] = float(argv[9]) if len(argv) > 9 else 0.0
    HVAR[0] = len(argv) > 8 and argv[8].endswith("-hvar")
    Maker.empty = len(argv) > 8 and argv[8] == "variable-empty"
    os.makedirs(out, exist_ok=True)
    for n in range(fonts):
        rng = random.Random(seed * 100003 + n)
        make_font(rng, os.path.join(out, "probe-%d-%d.ttf" % (seed, n)), glyphs, steps, oblique, composites, variable, WILD[0])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
