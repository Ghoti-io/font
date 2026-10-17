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
"""Targeted hinting batteries: one instruction's meaning pinned down against FreeType.

documentation/design.md section 8.5. hint_probe_diff.py samples random programs;
this sweeps the edges, each battery being a small family of programs on a few
points that varies one thing at a time - an operand, a flag, an angle - because
the places two interpreters part are thresholds and ties, and a random sample
meets them rarely (a planted change to ISECT's cutoff for grazing lines went
unseen by it, which is why the ISECT battery exists).

Every battery runs in both interpreter versions it applies to, and every point of
every case is compared in 26.6 with FreeType's, with no tolerance. A battery that
disagrees prints its first cases.

Usage: hint_batteries.py [battery...]   (none: all)
"""

import itertools
import json
import os
import random
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hint_diff
import oracle_env

ROOT = oracle_env.ROOT
SCRATCH = os.path.join(ROOT, "build", "oracle", "probe")


COMPARED = [0]


def build(name, spec):
    os.makedirs(SCRATCH, exist_ok=True)
    spec_path = os.path.join(SCRATCH, name + ".json")
    font = os.path.join(SCRATCH, name + ".ttf")
    with open(spec_path, "w") as handle:
        json.dump(spec, handle)
    done = subprocess.run(oracle_env.command("fonttools", [
        "python3", os.path.join(ROOT, "tools", "oracle", "hint_case_font.py"),
        font, spec_path], scratch=SCRATCH), capture_output=True, text=True)
    if done.returncode != 0:
        raise RuntimeError("could not build %s: %s" % (name, done.stderr[-300:]))
    return font


def compare(name, spec, versions=("35", "40"), ppem=10, label=None):
    """Run a spec through both interpreters; return the mismatching cases."""
    font = build(name, spec)
    bad = []
    for version in versions:
        coords = spec.get("coords", "-")
        mine, _ = hint_diff.ours(font, int(version), ppem, 1, coords)
        yours, _ = hint_diff.theirs(font, int(version), ppem, 1, coords)
        if mine is None or yours is None:
            return [("driver failed", version)]
        for index, case in enumerate(spec["cases"]):
            a, b = mine.get(index + 1), yours.get(index + 1)
            if not (isinstance(a, str) or isinstance(b, str) or a is None or b is None):
                COMPARED[0] += 1
            if isinstance(a, str) or isinstance(b, str) or a is None or b is None:
                if a != b:
                    bad.append((version, (label or (lambda c: c["asm"][-2:]))(case), a, b))
            elif a[1] != b[1] or a[0] != b[0]:
                bad.append((version, (label or (lambda c: c["asm"][-2:]))(case),
                            [(k, p[:2], q[:2]) for k, (p, q) in enumerate(zip(a[1], b[1])) if p != q][:2]
                            or "advance ours %d theirs %d" % (a[0], b[0])))
    return bad


def case(points, asm, ends=None):
    return {"points": points, "ends": ends or [len(points) - 1], "asm": asm}


# -- the batteries ---------------------------------------------------------------

def rounding():
    """Every rounding state, over -200..200, by way of MDAP[1]."""
    modes = ["RTG[ ]", "RTHG[ ]", "RTDG[ ]", "RDTG[ ]", "RUTG[ ]", "ROFF[ ]"]
    cases = []
    for mode in modes:
        for v in range(-200, 201, 3):
            cases.append(case([[0, v, 1], [10, 10, 1], [20, 20, 1]],
                              ["SVTCA[0]", mode, "PUSH[ ] 0", "MDAP[1]"]))
    for operand in (0x48, 0x68, 0x40, 0x88, 0x4F, 0x58, 0x0B, 0xA3, 0x20):
        for v in range(-130, 131, 7):
            cases.append(case([[0, v, 1], [10, 10, 1], [20, 20, 1]],
                              ["SVTCA[0]", "PUSH[ ] %d" % operand, "SROUND[ ]", "PUSH[ ] 0", "MDAP[1]"]))
    return {"upem": 640, "cases": cases}, ("35", "40"), 10


def mdrp():
    cases = []
    for D in (5, 10, 30, 31, 32, 33, 70, 100, 0, -5, -10, -30, -31, -32, -33, -70, -100):
        for bits in itertools.product("01", repeat=2):
            cases.append(case([[100, 0, 1], [100 + D, 0, 1], [0, 300, 1], [300, 300, 1]],
                              ["SVTCA[1]", "PUSH[ ] 0", "SRP0[ ]", "PUSH[ ] 1", "MDRP[0%s%s00]" % bits]))
    return {"upem": 640, "cases": cases}, ("35",), 10


def mirp():
    cvt = [30, -30, 100, -100, 200, 5, -5, 0]
    cases = []
    for D in (30, -30, 100, -100, 0, 10, -10, 150):
        for ci in range(len(cvt) - 1):
            for bits in itertools.product("01", repeat=2):
                cases.append(case([[100, 0, 1], [100 + D, 0, 1], [0, 300, 1], [300, 300, 1]],
                                  ["SVTCA[1]", "PUSH[ ] 0", "SRP0[ ]", "PUSH[ ] 1 %d" % ci,
                                   "MIRP[0%s00]" % "".join(bits)]))
    return {"upem": 640, "cvt": cvt + [0] * 8, "cases": cases}, ("35",), 10


def original_distance():
    """MDRP measures the original in font units, MIRP in scaled points (at 12 ppem on 1000)."""
    rng = random.Random(5)
    cases = []
    for _ in range(150):
        a, b = rng.randrange(-300, 900), rng.randrange(-300, 900)
        cases.append(case([[0, a, 1], [0, b, 1], [100, 0, 1]],
                          ["SVTCA[0]", "ROFF[ ]", "PUSH[ ] 0", "SRP0[ ]", "PUSH[ ] 1 0", "MIRP[00100]"]))
        cases.append(case([[0, a, 1], [0, b, 1], [100, 0, 1]],
                          ["SVTCA[0]", "ROFF[ ]", "PUSH[ ] 0", "SRP0[ ]", "PUSH[ ] 1", "MDRP[00000]"]))
    return {"upem": 1000, "cvt": [30000], "cases": cases}, ("35",), 12


def isect():
    """Crossings, parallel lines, and the cutoff in between (tan 1/19)."""
    cases = []
    for dy in range(98, 110):
        cases.append(case([[900, 900, 1], [0, 0, 1], [6400, 0, 1], [3200, 0, 1], [5200, dy, 1]],
                          ["SVTCA[1]", "PUSH[ ] 0 1 2 3 4", "ISECT[ ]"]))
    for dy in list(range(1, 14)) + [20, 50, 200]:
        cases.append(case([[900, 900, 1], [0, 0, 1], [640, 0, 1], [320, 0, 1], [520, dy, 1]],
                          ["SVTCA[1]", "PUSH[ ] 0 1 2 3 4", "ISECT[ ]"]))
    for q in itertools.permutations([1, 2, 3, 4]):
        cases.append(case([[900, 900, 1], [0, 0, 1], [640, 100, 1], [100, 640, 1], [500, 0, 1]],
                          ["SVTCA[1]", "PUSH[ ] 0 %d %d %d %d" % q, "ISECT[ ]"]))
    rng = random.Random(9)
    for _ in range(120):
        pts = [[rng.randrange(0, 2000), rng.randrange(-1500, 2000), 1] for _ in range(5)]
        cases.append(case(pts, ["SVTCA[1]", "PUSH[ ] 0 1 2 3 4", "ISECT[ ]"]))
    return {"upem": 640, "cases": cases}, ("35", "40"), 10


def measuring():
    """MD[0] differences the current points, MD[1] the original in font units."""
    rng = random.Random(3)
    cases = []
    for variant in (0, 1):
        for _ in range(100):
            a, b = rng.randrange(-400, 900), rng.randrange(-400, 900)
            cases.append(case([[10, a, 1], [10, b, 1], [0, 0, 1], [500, 500, 1], [300, 0, 1]],
                              ["SVTCA[0]", "PUSH[ ] 0 1", "MD[%d]" % variant, "PUSH[ ] 4", "SWAP[ ]", "SCFS[ ]"]))
    return {"upem": 1000, "cases": cases}, ("35", "40"), 12


def freedom():
    """A freedom vector nearly at right angles to the projection vector moves nothing."""
    cases = []
    for dx in (0, 5, 10, 20, 30, 40, 60, 62, 64, 66, 80, 100, 160, 200, 300, 500, 1000):
        cases.append(case([[100, 53, 1], [0, 0, 1], [0, 0, 1], [dx, -1000, 1]],
                          ["SVTCA[1]", "PUSH[ ] 2 3", "SPVTL[0]", "PUSH[ ] 0", "MDAP[1]"]))
    return {"upem": 640, "cases": cases}, ("35", "40"), 10


def environment():
    """What MPPEM, MPS and GETINFO answer, read out through y."""
    tests = [["MPPEM[ ]"], ["MPS[ ]"]]
    for sel in (0, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 31, 63, 127, 255, 1023, 8191):
        tests.append(["PUSH[ ] %d" % sel, "GETINFO[ ]"])
    cases = [case([[0, 0, 1], [10, 10, 1]], ["SVTCA[0]"] + t + ["PUSH[ ] 0", "SWAP[ ]", "SCFS[ ]"]) for t in tests]
    return {"upem": 640, "cases": cases}, ("35", "40"), 12


def variation():
    """What a variable face's interpreter is told: GETVARIATION's values and order, and GETINFO's variation bit."""
    tests = []
    for read in (["GETVARIATION[ ]"], ["GETVARIATION[ ]", "POP[ ]"]):
        tests.append(read + ["PUSH[ ] 0", "SWAP[ ]", "SCFS[ ]"])
    for sel in (8, 9, 15, 63, 1023, 2047):
        tests.append(["PUSH[ ] %d" % sel, "GETINFO[ ]", "PUSH[ ] 0", "SWAP[ ]", "SCFS[ ]"])
    cases = [case([[0, 0, 1], [10, 10, 1]], ["SVTCA[0]"] + t) for t in tests]
    return {"upem": 640, "cases": cases, "axes": 2, "coords": "8192,-4096"}, ("35", "40"), 12


def variation_default():
    """The same questions of a variable face at its default instance, which sets no coordinates."""
    spec, versions, ppem = variation()
    spec["coords"] = "-"
    # GETVARIATION there is an undefined instruction, whose handling is the failure
    # tolerance's business; the GETINFO answers are this battery's.
    spec["cases"] = [c for c in spec["cases"] if "GETVARIATION[ ]" not in c["asm"]]
    return spec, versions, ppem


def cvar():
    """The control values of a variable face: each one read out with RCVT, at coordinates that
    make its delta a half, a third and a fraction of a unit."""
    count = 40
    cvt = [100 + 37 * i for i in range(count)]
    deltas = [(-1) ** i * (1 + 2 * i) for i in range(count)]
    cases = [case([[0, 0, 1], [10, 10, 1]],
                  ["SVTCA[0]", "PUSH[ ] %d" % i, "RCVT[ ]", "PUSH[ ] 0", "SWAP[ ]", "SCFS[ ]"])
             for i in range(count)]
    return {"upem": 1000, "cases": cases, "axes": 1, "cvt": cvt, "cvar": deltas,
            "coords": "8192"}, ("35", "40"), 12


def cvar_other_coordinates():
    spec, versions, ppem = cvar()
    spec["coords"] = "5461"
    return spec, versions, ppem


def cvar_negative():
    spec, versions, ppem = cvar()
    spec["coords"] = "-8192"
    return spec, versions, ppem


FAILURES = [
    ("rcvt index", ["PUSH[ ] 999", "RCVT[ ]", "POP[ ]"]),
    ("rcvt negative", ["PUSH[ ] -3", "RCVT[ ]", "POP[ ]"]),
    ("wcvtp index", ["PUSH[ ] 999 50", "WCVTP[ ]"]),
    ("mdap point", ["PUSH[ ] 99", "MDAP[1]"]),
    ("mdrp point", ["PUSH[ ] 99", "MDRP[00000]"]),
    ("mdap negative point", ["PUSH[ ] -1", "MDAP[1]"]),
    ("srp0 point", ["PUSH[ ] 99", "SRP0[ ]"]),
    ("underflow add", ["ADD[ ]"]),
    ("underflow pop", ["POP[ ]"]),
    ("underflow mdap", ["MDAP[1]"]),
    ("div zero", ["PUSH[ ] 5 0", "DIV[ ]", "POP[ ]"]),
    ("undefined call", ["PUSH[ ] 3", "CALL[ ]"]),
    ("call out of range", ["PUSH[ ] 99", "CALL[ ]"]),
    ("rs index", ["PUSH[ ] 99", "RS[ ]", "POP[ ]"]),
    ("ws index", ["PUSH[ ] 99 5", "WS[ ]"]),
    ("undefined opcode", ["INSTR127[ ]"]),
    ("jmpr far", ["PUSH[ ] 500", "JMPR[ ]"]),
    ("shp no loop", ["PUSH[ ] 77", "SHP[1]"]),
    ("gc point", ["PUSH[ ] 77", "GC[0]", "POP[ ]"]),
    ("deltap point", ["PUSH[ ] 1 99 0", "DELTAP1[ ]"]),
    ("loop negative", ["PUSH[ ] -2", "SLOOP[ ]"]),
    ("miap cvt", ["PUSH[ ] 0 500", "MIAP[0]"]),
    ("miap point", ["PUSH[ ] 88 0", "MIAP[0]"]),
    ("ip point", ["PUSH[ ] 88", "IP[ ]"]),
    ("alignrp point", ["PUSH[ ] 88", "ALIGNRP[ ]"]),
    ("shc contour", ["PUSH[ ] 9", "SHC[1]"]),
    ("iup after fail", ["PUSH[ ] 1000", "RCVT[ ]", "POP[ ]", "IUP[0]"]),
    # A move that does not happen still sets the reference points, which the next one uses.
    ("mdrp bad then mdrp", ["PUSH[ ] 99", "MDRP[10000]", "PUSH[ ] 2", "MDRP[00100]"]),
    ("mdrp bad (no rp0) then mdrp", ["PUSH[ ] 99", "MDRP[00000]", "PUSH[ ] 2", "MDRP[00100]"]),
    ("mirp bad cvt then mdrp", ["PUSH[ ] 2 99", "MIRP[10000]", "PUSH[ ] 1", "MDRP[00100]"]),
    ("miap bad then mdrp", ["PUSH[ ] 2 99", "MIAP[0]", "PUSH[ ] 1", "MDRP[00100]"]),
    ("miap bad point then ip", ["PUSH[ ] 99 0", "MIAP[0]", "PUSH[ ] 1", "IP[ ]"]),
    ("srp wide", ["PUSH[ ] 32767 32767", "ADD[ ]", "PUSH[ ] 3", "ADD[ ]", "SRP0[ ]", "PUSH[ ] 2", "MDRP[00100]"]),
]


def failures():
    """What becomes of a glyph whose program does something invalid after moving a point."""
    pts = [[0, 13, 1], [10, 200, 1], [20, 20, 1]]
    cases = []
    for name, bad in FAILURES:
        cases.append(case(pts, ["SVTCA[0]", "PUSH[ ] 0", "MDAP[1]"] + bad + ["PUSH[ ] 1", "MDAP[1]"]))
    return {"upem": 640, "cvt": [10, 20, 30], "cases": cases, "functions": 4}, ("35", "40"), 10


STACK_OPS = [
    ("ip rp1 bad", ["PUSH[ ] 200", "SRP1[ ]", "IP[ ]"], 3),
    ("ip rp2 bad", ["PUSH[ ] 200", "SRP2[ ]", "IP[ ]"], 3),
    ("ip loop 2 rp1 bad", ["PUSH[ ] 200", "SRP1[ ]", "PUSH[ ] 2", "SLOOP[ ]", "IP[ ]"], 4),
    ("ip point bad", ["PUSH[ ] 99", "IP[ ]"], 3),
    ("shp ref bad", ["PUSH[ ] 200", "SRP1[ ]", "SHP[1]"], 3),
    ("shp2 ref bad", ["PUSH[ ] 200", "SRP2[ ]", "SHP[0]"], 3),
    ("shp loop 3 short", ["PUSH[ ] 3", "SLOOP[ ]", "SHP[1]"], 2),
    ("shp point bad", ["PUSH[ ] 99", "SHP[1]"], 3),
    ("alignrp rp0 bad", ["PUSH[ ] 200", "SRP0[ ]", "ALIGNRP[ ]"], 3),
    ("alignrp loop 2 rp0 bad", ["PUSH[ ] 200", "SRP0[ ]", "PUSH[ ] 2", "SLOOP[ ]", "ALIGNRP[ ]"], 4),
    ("alignrp point bad", ["PUSH[ ] 99", "ALIGNRP[ ]"], 3),
    ("shpix point bad", ["PUSH[ ] 99 5", "SHPIX[ ]"], 3),
    ("shpix short loop", ["PUSH[ ] 4", "SLOOP[ ]", "PUSH[ ] 5", "SHPIX[ ]"], 3),
    ("shc ref bad", ["PUSH[ ] 200", "SRP1[ ]", "PUSH[ ] 0", "SHC[1]"], 3),
    ("shc contour bad", ["PUSH[ ] 9", "SHC[1]"], 3),
    ("shz ref bad", ["PUSH[ ] 200", "SRP1[ ]", "PUSH[ ] 1", "SHZ[1]"], 3),
    ("shz zone bad", ["PUSH[ ] 5", "SHZ[1]"], 3),
    ("mdap bad", ["PUSH[ ] 99", "MDAP[1]"], 3),
    ("miap point bad", ["PUSH[ ] 99 0", "MIAP[1]"], 3),
    ("miap cvt bad", ["PUSH[ ] 1 99", "MIAP[1]"], 3),
    ("mdrp point bad", ["PUSH[ ] 99", "MDRP[10000]"], 3),
    ("mdrp rp0 bad", ["PUSH[ ] 200", "SRP0[ ]", "PUSH[ ] 1", "MDRP[00000]"], 3),
    ("mirp cvt bad", ["PUSH[ ] 1 99", "MIRP[10000]"], 3),
    ("mirp cvt -1", ["PUSH[ ] 1 -1", "MIRP[00000]"], 3),
    ("mirp cvt -2", ["PUSH[ ] 1 -2", "MIRP[00000]"], 3),
    ("msirp point bad", ["PUSH[ ] 99 5", "MSIRP[1]"], 3),
    ("flippt loop 2 bad", ["PUSH[ ] 2", "SLOOP[ ]", "PUSH[ ] 99 1", "FLIPPT[ ]"], 3),
    ("fliprgon bad", ["PUSH[ ] 1 99", "FLIPRGON[ ]"], 3),
    ("gc bad", ["PUSH[ ] 99", "GC[0]"], 3),
    ("md bad", ["PUSH[ ] 99 1", "MD[0]"], 3),
    ("scfs bad", ["PUSH[ ] 99 5", "SCFS[ ]"], 3),
    ("utp bad", ["PUSH[ ] 99", "UTP[ ]"], 3),
    ("alignpts bad", ["PUSH[ ] 99 1", "ALIGNPTS[ ]"], 3),
    ("isect bad", ["PUSH[ ] 99 1 2 3 4", "ISECT[ ]"], 3),
    ("spvtl bad", ["PUSH[ ] 99 1", "SPVTL[0]"], 3),
    ("deltap bad point", ["PUSH[ ] 17 99 1", "DELTAP1[ ]"], 3),
    ("deltap short", ["PUSH[ ] 5", "DELTAP1[ ]"], 3),
    ("deltap n 2 one pair", ["PUSH[ ] 17 1 2", "DELTAP1[ ]"], 3),
    ("deltac short", ["PUSH[ ] 5", "DELTAC1[ ]"], 3),
    ("deltac bad cvt", ["PUSH[ ] 17 99 1", "DELTAC1[ ]"], 3),
    ("szp0 bad", ["PUSH[ ] 5", "SZP0[ ]"], 3),
    ("cindex bad", ["PUSH[ ] 50", "CINDEX[ ]"], 3),
    ("mindex bad", ["PUSH[ ] 50", "MINDEX[ ]"], 3),
    ("rs bad", ["PUSH[ ] 99", "RS[ ]"], 3),
    ("ws bad", ["PUSH[ ] 99 4", "WS[ ]"], 3),
    ("rcvt bad", ["PUSH[ ] 99", "RCVT[ ]"], 3),
    ("wcvtp bad", ["PUSH[ ] 99 4", "WCVTP[ ]"], 3),
    ("add underflow with 1", ["CLEAR[ ]", "PUSH[ ] 7", "ADD[ ]"], 0),
    ("mdrp underflow", ["CLEAR[ ]", "MDRP[00000]"], 0),
    ("jmpr bad", ["PUSH[ ] 700", "JMPR[ ]"], 3),
    ("call bad", ["PUSH[ ] 9", "CALL[ ]"], 3),
    ("sdb big", ["PUSH[ ] 30000", "SDB[ ]"], 3),
    ("sloop huge", ["PUSH[ ] 30000", "SLOOP[ ]"], 3),
]


def tolerance_stack():
    """After something invalid, how much of the stack is left and what is on top."""
    cases = []
    for name, ops, _ in STACK_OPS:
        base = ["SVTCA[0]", "PUSH[ ] 11 22 33 44 55 66 77"]
        pts = [[0, 13, 1], [10, 200, 1], [20, 20, 1], [30, 40, 1], [40, 90, 1], [50, 120, 1]]
        cases.append(case(pts, base + ops + ["DEPTH[ ]", "PUSH[ ] 0", "SWAP[ ]", "SCFS[ ]"]))
        cases.append(case(pts, base + ops + ["PUSH[ ] 0", "SWAP[ ]", "SCFS[ ]"]))
    return {"upem": 640, "cvt": [10, 20, 30], "cases": cases, "functions": 4}, ("35", "40"), 10


def compat():
    """Version 40's compatibility rules: which moves are ignored, which points are touched."""
    pts = [[0, 0, 1], [50, 100, 1], [100, 200, 1], [150, 300, 1]]
    programs = [
        ["SVTCA[0]", "PUSH[ ] 1 30", "SHPIX[ ]"],
        ["SVTCA[0]", "PUSH[ ] 1", "MDAP[0]", "PUSH[ ] 1 30", "SHPIX[ ]"],
        ["SVTCA[0]", "PUSH[ ] 1", "MDAP[1]", "PUSH[ ] 1 30", "SHPIX[ ]"],
        ["SVTCA[0]", "PUSH[ ] 1 30", "SHPIX[ ]", "PUSH[ ] 1 20", "SHPIX[ ]"],
        ["SVTCA[0]", "IUP[0]", "PUSH[ ] 1 30", "SHPIX[ ]"],
        ["SVTCA[0]", "IUP[0]", "IUP[1]", "PUSH[ ] 1 30", "SHPIX[ ]"],
        ["SVTCA[1]", "PUSH[ ] 1 30", "SHPIX[ ]"],
        ["SVTCA[0]", "PUSH[ ] 1 30", "SHPIX[ ]", "IUP[0]"],
        ["SVTCA[1]", "PUSH[ ] 1", "MDAP[1]", "PUSH[ ] 3 20", "SHPIX[ ]", "IUP[1]"],
        ["SVTCA[1]", "PUSH[ ] 2 3", "ALIGNPTS[ ]", "IUP[1]"],
        ["SVTCA[0]", "PUSH[ ] 0 1 2 3 1", "ISECT[ ]", "IUP[0]", "IUP[1]"],
        ["SVTCA[0]", "PUSH[ ] 0 1 2 3 1", "ISECT[ ]", "IUP[1]", "IUP[0]"],
        ["SVTCA[1]", "PUSH[ ] 1", "MDRP[10000]", "IUP[0]", "IUP[1]", "PUSH[ ] 2 15", "SHPIX[ ]"],
    ]
    cases = [case(pts, p) for p in programs]
    return {"upem": 640, "cases": cases}, ("40",), 10


def zones():
    """SHZ acts on zp2's zone; IUP adds a lone touched point's shift to where points are."""
    pts = [[0, 0, 1], [300, 100, 1], [600, 0, 1], [500, 700, 1], [100, 600, 1], [300, 350, 1]]
    base = ["SVTCA[0]", "PUSH[ ] 1", "MDAP[1]"]
    programs = [
        base + ["PUSH[ ] 1", "SRP1[ ]", "PUSH[ ] 1", "SHZ[1]"],
        base + ["PUSH[ ] 1", "SRP2[ ]", "PUSH[ ] 1", "SHZ[0]", "IUP[0]"],
        base + ["PUSH[ ] 1", "SRP1[ ]", "PUSH[ ] 1", "SHZ[1]", "IUP[0]"],
        base + ["PUSH[ ] 0", "SHZ[1]"],
        base + ["PUSH[ ] 1", "SRP1[ ]", "PUSH[ ] 1", "SHC[1]"],
    ]
    return {"upem": 640, "cases": [case(pts, p, [3, 5]) for p in programs]}, ("35", "40"), 10


def iup():
    rng = random.Random(1)
    cases = []
    for _ in range(300):
        pts = [[rng.randrange(0, 700), rng.randrange(-200, 800), 1] for _ in range(12)]
        asm = ["SVTCA[%d]" % rng.randrange(2)]
        for t in rng.sample(range(12), rng.randrange(1, 7)):
            asm += ["PUSH[ ] %d %d" % (t, rng.randrange(-70, 70)), "SHPIX[ ]"]
        asm += rng.choice([["IUP[0]"], ["IUP[1]"], ["IUP[0]", "IUP[1]"]])
        cases.append(case(pts, asm, [5, 11]))
    return {"upem": 1000, "cases": cases}, ("35",), 12


def iup_ties():
    """Points a font unit apart whose scaled positions tie, which is where IUP's choice of side shows."""
    rng = random.Random(2)
    cases = []
    for _ in range(300):
        pts = [[rng.choice([600, 601, 602, 603, 604, 605, 606, 607, 608, 100, 101, 102, 350, 351]),
                rng.choice([300, 301, 302, 303, 304, 305, 306, 307, 308, 100, 101, 102, 650, 651]), 1]
               for _ in range(12)]
        asm = ["SVTCA[%d]" % rng.randrange(2)]
        for t in rng.sample(range(12), rng.randrange(2, 5)):
            asm += ["PUSH[ ] %d %d" % (t, rng.randrange(-60, 60)), "SHPIX[ ]"]
        asm += rng.choice([["IUP[0]"], ["IUP[1]"], ["IUP[0]", "IUP[1]"]])
        cases.append(case(pts, asm, [5, 11]))
    return {"upem": 1000, "cases": cases}, ("35",), 12


def phantom():
    """Where the origin and the advance go: lsb against xMin, the advance, program or none."""
    cases = []
    for xmin in (0, 1, 31, 32, 33, 63, 64, 65, 70, 100, 200):
        for adv in (0, 1, 31, 32, 33, 300, 500, 501):
            for prog in ([], ["PUSH[ ] 0", "POP[ ]"], ["SVTCA[1]", "PUSH[ ] 1", "MDAP[1]"],
                         ["SVTCA[1]", "PUSH[ ] 4", "SRP2[ ]", "PUSH[ ] 0", "SHP[0]"],
                         ["SVTCA[1]", "PUSH[ ] 5", "SRP2[ ]", "PUSH[ ] 0", "SHP[0]"]):
                cases.append({"points": [[xmin, 13, 1], [xmin + 200, 13, 1], [xmin + 200, 213, 1], [xmin, 213, 1]],
                              "ends": [3], "asm": prog, "advance": adv, "bearing": 0})
    return {"upem": 640, "cases": cases}, ("35", "40"), 10


def composites():
    """Placing hinted components: transforms, offsets, rounding, anchors, metrics, programs."""
    XY, ROUND, USE_MY, SCALED, UNSCALED = 0x2, 0x4, 0x200, 0x800, 0x1000
    A = [[40, 13, 1], [240, 13, 1], [240, 213, 1], [40, 213, 1]]
    B = [[0, 0, 1], [100, 0, 1], [50, 100, 1]]
    cases = [
        {"points": A, "ends": [3], "asm": ["SVTCA[0]", "PUSH[ ] 0", "MDAP[1]", "PUSH[ ] 2", "MDAP[1]"], "advance": 300},
        {"points": B, "ends": [2], "asm": [], "advance": 170},
    ]
    first = len(cases)
    def comp(components, asm=(), advance=500):
        cases.append({"components": components, "asm": list(asm), "advance": advance})
    for flags in (XY, XY | ROUND, XY | USE_MY, XY | ROUND | USE_MY):
        for dx, dy in ((100, 50), (37, 41), (-37, -41), (31, 31), (32, 32), (-32, -32), (33, -33), (0, 0)):
            comp([[0, dx, dy, flags], [1, 300, 17, XY]])
            comp([[0, dx, dy, XY], [1, 300, 17, flags]])
            comp([[0, dx, dy, flags], [1, 300, 17, XY]], ["PUSH[ ] 0", "POP[ ]"])
    for transform in ([[0.5, 0], [0, 0.5]], [[1.0, 0], [0, 0.75]], [[0.866, 0.5], [-0.5, 0.866]],
                      [[1, 0.25], [0, 1]], [[0.75, 0.1], [-0.1, 0.75]]):
        for flags in (XY, XY | SCALED, XY | UNSCALED, XY | ROUND | SCALED):
            comp([[0, 37, 41, flags, transform], [1, 33, 29, XY]])
    comp([[0, 100, 50, XY], [1, 2, 1, 0]])                         # point matching
    comp([[0, 0, 0, XY], [1, 3, 0, 0, [[0.5, 0], [0, 0.5]]]])      # matching under a transform
    comp([[0, 100, 50, XY], [1, 0, 2, 0], [1, 5, 1, 0]])
    nested = len(cases) + first - first
    comp([[0, 100, 50, XY | ROUND], [1, 30, 17, XY]])
    comp([[nested, 10, 10, XY | ROUND], [1, 50, 20, XY]], ["SVTCA[0]", "PUSH[ ] 5", "MDAP[1]"])
    for prog in (["SVTCA[0]", "PUSH[ ] 5", "GC[0]", "PUSH[ ] 0", "SWAP[ ]", "SCFS[ ]"],
                 ["SVTCA[0]", "PUSH[ ] 0", "GC[1]", "PUSH[ ] 4", "SWAP[ ]", "SCFS[ ]"],
                 ["SVTCA[0]", "PUSH[ ] 0 1", "MD[1]", "PUSH[ ] 4", "SWAP[ ]", "SCFS[ ]"],
                 ["SVTCA[0]", "PUSH[ ] 1 20", "SHPIX[ ]"],
                 ["SVTCA[0]", "PUSH[ ] 4 20", "SHPIX[ ]", "IUP[0]"],
                 ["SVTCA[0]", "PUSH[ ] 4 1", "ALIGNPTS[ ]"],
                 ["SVTCA[0]", "PUSH[ ] 0", "SRP1[ ]", "PUSH[ ] 4", "SRP2[ ]", "PUSH[ ] 5", "IP[ ]"],
                 ["SVTCA[1]", "PUSH[ ] 7", "SRP2[ ]", "PUSH[ ] 1", "SHP[0]"],
                 ["SVTCA[0]", "PUSH[ ] 3", "MDAP[1]", "PUSH[ ] 5", "MDRP[10100]", "PUSH[ ] 6", "MDRP[00100]", "IUP[0]"]):
        comp([[0, 100, 50, XY], [1, 30, 17, XY]], prog)
    return {"upem": 640, "cases": cases}, ("35", "40"), 10


def state():
    """What the control value program leaves for a glyph, and what it does not."""
    pts = [[100, 100, 1], [300, 500, 1], [500, 100, 1], [700, 500, 1]]
    tests = [
        (["SVTCA[0]"], ["PUSH[ ] 0", "SRP0[ ]", "PUSH[ ] 1", "ALIGNRP[ ]"]),
        (["PUSH[ ] 2", "SRP0[ ]"], ["SVTCA[0]", "PUSH[ ] 1", "ALIGNRP[ ]"]),
        (["PUSH[ ] 0", "SZP0[ ]"], ["SVTCA[0]", "PUSH[ ] 1", "MDAP[0]"]),
        (["PUSH[ ] 2", "SLOOP[ ]"], ["SVTCA[0]", "PUSH[ ] 0", "SRP0[ ]", "PUSH[ ] 1 2", "ALIGNRP[ ]"]),
        (["RTHG[ ]"], ["SVTCA[0]", "PUSH[ ] 0", "MDAP[1]"]),
        (["ROFF[ ]"], ["SVTCA[0]", "PUSH[ ] 0", "MDAP[1]"]),
        (["PUSH[ ] 20", "SMD[ ]"], ["SVTCA[0]", "PUSH[ ] 0", "SRP0[ ]", "PUSH[ ] 1", "MDRP[01000]"]),
        (["PUSH[ ] 0", "SCVTCI[ ]"], ["SVTCA[0]", "PUSH[ ] 0", "SRP0[ ]", "PUSH[ ] 1 0", "MIRP[00100]"]),
        (["FLIPOFF[ ]"], ["SVTCA[0]", "PUSH[ ] 0", "SRP0[ ]", "PUSH[ ] 1 1", "MIRP[00000]"]),
        (["PUSH[ ] 10", "SDB[ ]"], ["SVTCA[0]", "PUSH[ ] 1 1 0", "DELTAP1[ ]"]),
        (["PUSH[ ] 80", "SSW[ ]", "PUSH[ ] 50", "SSWCI[ ]"], ["SVTCA[0]", "PUSH[ ] 0", "SRP0[ ]", "PUSH[ ] 1", "MDRP[00000]"]),
    ]
    return tests, ("35", "40"), 10


BATTERIES = {
    "rounding": rounding, "mdrp": mdrp, "mirp": mirp,
    "original-distance": original_distance, "isect": isect, "measuring": measuring,
    "freedom": freedom, "environment": environment, "compat": compat,
    "zones": zones, "variation": variation, "variation-default": variation_default,
    "tolerance-stack": tolerance_stack,
    "failures": failures,
    "cvar": cvar, "cvar-third": cvar_other_coordinates, "cvar-negative": cvar_negative, "iup": iup, "iup-ties": iup_ties,
    "phantom": phantom, "composites": composites,
}


def run_state():
    """Each (prep, glyph) pair is its own font: the prep is the thing varied."""
    tests, versions, ppem = state()
    pts = [[100, 100, 1], [300, 500, 1], [500, 100, 1], [700, 500, 1]]
    bad = []
    for n, (prep, glyph) in enumerate(tests):
        spec = {"upem": 640, "cvt": [300, -80, 50], "prep": prep + ["PUSH[ ] 0", "POP[ ]"],
                "cases": [case(pts, glyph)]}
        bad += compare("state%d" % n, spec, versions, ppem, label=lambda c, p=prep: p)
    return bad, len(tests) * len(versions)


def main(argv):
    wanted = argv[1:] or (list(BATTERIES) + ["state"])
    failed = 0
    for name in wanted:
        if name == "state":
            bad, count = run_state()
        else:
            spec, versions, ppem = BATTERIES[name]()
            count = len(spec["cases"]) * len(versions)
            bad = compare(name, spec, versions, ppem)
        print("%-18s %5d cases (%d hinted by both), %d disagree" % (name, count, COMPARED[0], len(bad)))
        COMPARED[0] = 0
        for entry in bad[:4]:
            print("    ", entry)
        failed += bool(bad)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
