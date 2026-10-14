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
"""How each character of the Basic Multilingual Plane sits in vertical text: this library
(UAX #50, CSS `text-orientation: mixed`) against Pango (gravity east, hint natural).

notes/font/VERTICAL.md. The two disagree for some characters because they decide differently:
UAX #50 gives every character its own `Vertical_Orientation`, and Pango decides by script and
East Asian width. This survey puts every assigned, non-control BMP character in a font made for
the purpose (a box each, 600 across and 900 down, so that the advance a layout engine used says
which way the character was set), lays out each character alone and between two ideographs in
each engine, and lists where they differ. It is how `GFNT_ORIENT_MIXED_SCRIPT` was fitted, and
the lists below the table are the measurements that option's rule is held to.

Usage:
    orientation_survey.py [--ranges] [--names] [--pango]   # the disagreements; with --pango,
                                                         # this library set as Pango sets it
                                                         # (an exit status of 1 if any remain)
    orientation_survey.py --build <out>   # (inside the fontTools image) write the survey font
"""

import json
import os
import subprocess
import sys
import unicodedata

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import oracle_env
import layout_snapshot

ROOT = oracle_env.ROOT
SCRATCH = os.path.join(ROOT, "build", "oracle", "orientation")
DRIVER = os.path.join(ROOT, "build", "linux", "release", "apps", "examples", "font-layout")
FAMILY = "Orientation Survey"
SIZE = 100
H_ADV, V_ADV = 600, 900          # units of 1000: told apart by the advance a layout used
JI = "日"


def survey_cps():
    return [c for c in range(0x20, 0x10000)
            if not 0xD800 <= c < 0xE000 and unicodedata.category(chr(c))[0] != "C"
            and unicodedata.category(chr(c)) != "Zs" and c != 0x2028 and c != 0x2029
            and unicodedata.category(chr(c))[0] != "M"]


def build(out):
    """Inside the fontTools image: a TrueType font with a box for every survey character."""
    from fontTools.fontBuilder import FontBuilder
    from fontTools.pens.ttGlyphPen import TTGlyphPen
    cps = survey_cps()   # includes the ideograph the between-test is built with
    order = [".notdef"] + ["u%04X" % c for c in cps]
    fb = FontBuilder(1000, isTTF=True)
    fb.setupGlyphOrder(order)
    pen = TTGlyphPen(None)
    pen.moveTo((50, 0)); pen.lineTo((50, 700)); pen.lineTo((550, 700)); pen.lineTo((550, 0))
    pen.closePath()
    glyph = pen.glyph()
    fb.setupGlyf({n: glyph for n in order})
    fb.setupHorizontalMetrics({n: (H_ADV, 50) for n in order})
    fb.setupHorizontalHeader(ascent=880, descent=-120, lineGap=0)
    fb.setupVerticalMetrics({n: (V_ADV, 180) for n in order})
    fb.setupVerticalHeader(ascent=500, descent=-500, lineGap=0)
    fb.setupCharacterMap({c: "u%04X" % c for c in cps})
    fb.setupOS2(sTypoAscender=880, sTypoDescender=-120, sTypoLineGap=0, usWinAscent=880,
                usWinDescent=120)
    fb.setupNameTable({"familyName": FAMILY, "styleName": "Regular"})
    fb.setupPost()
    fb.font["head"].created = fb.font["head"].modified = 0x7C259DC0 + 1
    fb.save(out)


def ranges(cps):
    """Consecutive code points as 'U+XXXX-U+YYYY' strings."""
    out = []
    for c in cps:
        if out and c == out[-1][1] + 1:
            out[-1][1] = c
        else:
            out.append([c, c])
    return ["U+%04X" % a if a == b else "U+%04X-U+%04X" % (a, b) for a, b in out]


def main():
    if "--build" in sys.argv:
        build(sys.argv[sys.argv.index("--build") + 1])
        return 0
    os.makedirs(os.path.join(SCRATCH, "fonts"), exist_ok=True)
    font = os.path.join(SCRATCH, "fonts", "survey.ttf")
    print(oracle_env.provenance(["fonttools", "pango"]))
    subprocess.run(oracle_env.command("fonttools", ["python3", os.path.abspath(__file__),
                   "--build", font], scratch=SCRATCH), check=True, capture_output=True)
    cps = survey_cps()
    jobs = []
    for c in cps:
        jobs.append(dict(name="alone-%d" % c, text=chr(c), family=FAMILY, size=SIZE,
                         gravity="EAST", hint="NATURAL", room=None))
        jobs.append(dict(name="between-%d" % c, text=JI + chr(c) + JI, family=FAMILY, size=SIZE,
                         gravity="EAST", hint="NATURAL", room=None))
    with open(os.path.join(SCRATCH, "jobs.json"), "w") as h:
        json.dump(jobs, h)
    probe = os.path.join(ROOT, "tools", "oracle", "pango_probe.py")
    finished = subprocess.run(oracle_env.command("pango", [
        "python3", probe, os.path.join(SCRATCH, "jobs.json"), os.path.join(SCRATCH, "pango.json"),
        os.path.join(SCRATCH, "fonts")], scratch=SCRATCH), capture_output=True, text=True)
    if finished.returncode != 0:
        raise RuntimeError(finished.stderr[-800:])
    with open(os.path.join(SCRATCH, "pango.json")) as h:
        theirs = json.load(h)

    # This library: one request per job, in the same order.
    requests = []
    for c in cps:
        requests.append("%d\t%s" % (10 ** 7, chr(c).encode().hex()))
        requests.append("%d\t%s" % (10 ** 7, (JI + chr(c) + JI).encode().hex()))
    orient = "mixed-pango" if "--pango" in sys.argv else "mixed"
    finished = subprocess.run([DRIVER, "--size", str(SIZE), "--mode", "vertical-rl",
                               "--orient", orient, font],
                              input="\n".join(requests) + "\n", capture_output=True, text=True)
    answers = [json.loads(line) for line in finished.stdout.splitlines()]
    for i, cp in enumerate(cps):
        layout_snapshot.record("orientation", "alone-%d" % cp, answers[2 * i])
        layout_snapshot.record("orientation", "between-%d" % cp, answers[2 * i + 1])

    def pango_way(job, result, between):
        adv = result["chars"][1 if between else 0][2]
        return "sideways" if abs(adv - H_ADV / 10.0) < abs(adv - V_ADV / 10.0) else "upright"

    def our_way(answer, between):
        runs = [r for line in answer["lines"] for r in line["runs"]]
        glyphs = [(r["o"], g) for r in runs for g in r["g"]]
        # the glyph for the character itself: the middle one, or the only one
        need = 2 if between else 1
        if len(glyphs) < need:
            return None            # the engine drew nothing for it (a lone mark, say)
        o = glyphs[1 if between else 0][0]
        return "sideways" if o == 2 else "upright"

    differ = {"alone": [], "between": []}
    agree = {"alone": 0, "between": 0}
    for i, c in enumerate(cps):
        for k, kind in enumerate(("alone", "between")):
            a = pango_way(jobs[2 * i + k], theirs[2 * i + k], kind == "between")
            b = our_way(answers[2 * i + k], kind == "between")
            if b is None:
                continue
            if a == b:
                agree[kind] += 1
            else:
                differ[kind].append((c, a, b))
    for kind in ("alone", "between"):
        print("%s: %d characters agree, %d differ" % (kind, agree[kind], len(differ[kind])))
    for kind in ("alone", "between"):
        for way in ("upright", "sideways"):
            sel = [c for c, a, b in differ[kind] if a == way]
            if sel:
                print("%s: Pango sets these %s and this library the other way (%d):"
                      % (kind, way, len(sel)))
                if "--ranges" in sys.argv:
                    print("   " + " ".join(ranges(sel)))
                if "--names" in sys.argv:
                    for c in sel:
                        print("   U+%04X %s" % (c, unicodedata.name(chr(c), "?")))
    if "--pango" in sys.argv:
        left = len(differ["alone"]) + len(differ["between"])
        print("orientation_survey --pango: %d characters differ from Pango" % left)
        layout_snapshot.finish("orientation")
        return 1 if left else 0
    layout_snapshot.finish("orientation")
    return 0


if __name__ == "__main__":
    sys.exit(main())
