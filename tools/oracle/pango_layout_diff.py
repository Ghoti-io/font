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
"""Paragraph layout against Pango, with the options that follow Pango: where each line breaks and
where each character lies along its line, in rows and in columns.

Pango (the pinned `pango` image) lays a paragraph out in the frame in
which text runs along x and lines stack along y; for a vertical gravity that frame is the one the
page is turned a quarter from. This library lays out the same text with
`GFNT_ORIENT_MIXED_PANGO` and `GFNT_HYPHENS_MANUAL_PANGO` in a column and in a row, and the two
are compared in that frame: the line each character is on, and where along the line it starts and
how long it is. Pango has no `vertical-lr`; the cases use `vertical-rl`, whose first line is the
first Pango line.

What is held equal on purpose: the font (`vertical-text.ttf`, whose metrics are whole numbers of
pixels at the size used, so Pango's rounding of font metrics cannot move anything), the size, and
the room along the lines. A soft hyphen's own rectangle holds the hyphen Pango adds, which is no
character of the text, so it is not compared; the characters after a break are.

**What it cannot see**, stated so the absence is a statement: one face, no justification (Pango's
is spaces only and this library's default is not; `GFNT_JUSTIFY_INTER_WORD` is the comparable
one and is not yet compared), no hit testing, no ink.

Usage: pango_layout_diff.py [-v]
"""

import json
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import oracle_env
import layout_snapshot

ROOT = oracle_env.ROOT
DRIVER = os.path.join(ROOT, "build", "linux", "release", "apps", "examples", "font-layout")
SCRATCH = os.path.join(ROOT, "build", "oracle", "pango-layout")
FONT = os.path.join(ROOT, "tests", "data", "fonts", "vertical-text.ttf")
FAMILY = "Vertical Text"
SIZE = 100
TOLERANCE = 0.6      # pixels: Pango works in 1/1024 pixel and rounds advances to whole pixels

J = "日本語"
CASES = [
    # name, text, writing mode ("horizontal" or "vertical-rl"), room in pixels
    ("latin-wrap", "AB ab AB ab AB ab", "horizontal", 300),
    ("latin-wrap-v", "AB ab AB ab AB ab", "vertical-rl", 300),
    ("cjk-wrap", J * 4, "horizontal", 530),
    ("cjk-wrap-v", J * 4, "vertical-rl", 530),
    ("mixed", "日AB本ab語 日AB", "vertical-rl", 400),
    ("digits-punct", "日" "2014" "、本。", "vertical-rl", 600),
    ("brackets", "日「語」本", "vertical-rl", 600),
    ("soft-hyphens", "AB­AB­AB­AB­AB", "horizontal", 300),
    ("soft-hyphens-v", "AB­AB­AB­AB­AB", "vertical-rl", 300),
    ("soft-hyphens-cjk", "日本­語日­本語", "horizontal", 250),
    ("soft-hyphens-cjk-v", "日本­語日­本語", "vertical-rl", 250),
    ("newlines", "日本\n語\n\n日", "vertical-rl", 600),
    ("long-word", "ABABABABABAB 日", "vertical-rl", 200),
    # Pango exposes line-height and rise attributes; percentages, baseline keywords,
    # text-top/bottom and top/bottom have no equivalent and remain Chromium-only.
    ("line-height-attribute", "AB ab", "horizontal", 1000, {"line_height": 1.5}),
    ("rise-attribute", "AB", "horizontal", 1000, {"rise": 10}),
    ("negative-rise-attribute", "AB", "horizontal", 1000, {"rise": -8}),
]


def pango_chars(cases):
    fonts = os.path.join(SCRATCH, "fonts")
    os.makedirs(fonts, exist_ok=True)
    shutil.copy(FONT, os.path.join(fonts, "vertical-text.ttf"))
    jobs = [dict(name=c[0], text=c[1], family=FAMILY, size=SIZE,
                 gravity="SOUTH" if c[2] == "horizontal" else "EAST", hint="NATURAL", room=c[3],
                 **(c[4] if len(c) > 4 else {})) for c in cases]
    with open(os.path.join(SCRATCH, "jobs.json"), "w") as h:
        json.dump(jobs, h)
    probe = os.path.join(ROOT, "tools", "oracle", "pango_probe.py")
    finished = subprocess.run(oracle_env.command("pango", [
        "python3", probe, os.path.join(SCRATCH, "jobs.json"), os.path.join(SCRATCH, "out.json"),
        fonts], scratch=SCRATCH), capture_output=True, text=True)
    if finished.returncode != 0:
        raise RuntimeError(finished.stderr[-800:])
    with open(os.path.join(SCRATCH, "out.json")) as h:
        return json.load(h)


def ours(case):
    """(line index, along start, along length) of every character, in Pango's frame."""
    name, text, mode, room = case[:4]
    style = case[4] if len(case) > 4 else {}
    argv = [DRIVER, "--size", str(SIZE), "--mode", mode, "--orient", "mixed-pango",
            "--hyphens", "manual-pango", "--justify", "inter-word",
            # Pango has neither text-autospace nor text-spacing-trim. CSS's defaults would add an
            # eighth of an em between an ideograph and a Latin letter or digit, and set a closing
            # mark at the end of a line at half width so that it fits; Pango does neither.
            "--text-autospace", "no-autospace", "--text-spacing-trim", "space-all", FONT]
    if "line_height" in style:
        argv[-1:-1] = ["--line-height-multiple", str(style["line_height"])]
    if "rise" in style:
        argv[-1:-1] = ["--vertical-align", "length:%s" % style["rise"]]
    finished = subprocess.run(argv, input="%d\t%s\n" % (room * 64, text.encode().hex()),
                              capture_output=True, text=True)
    answer = json.loads(finished.stdout.splitlines()[0])
    if not style:
        layout_snapshot.record("pango", name, answer)
    if "error" in answer:
        raise RuntimeError("%s: %s" % (name, answer["error"]))
    lines = answer["lines"]
    out = []
    glyph_y = {}
    for line in lines:
        for run in line["runs"]:
            for glyph in run["g"]:
                glyph_y[glyph[1]] = glyph[5] / 64.0
    for i, rect in enumerate(answer["chars"]):
        x, y, w, h = [v / 64.0 for v in rect]
        if w == 0 and h == 0:
            out.append(None)
            continue
        if mode == "horizontal":
            along, length, across = x, w, y
        else:
            along, length, across = y, h, x
        line = None
        for k, ln in enumerate(lines):
            bx, by, bw, bh = [v / 64.0 for v in ln["box"]]
            lo, hi = (bx, bx + bw) if mode != "horizontal" else (by, by + bh)
            if lo - 0.01 <= across + 0.01 and across + 0.01 < hi + 0.01:
                line = k
                break
        if style:
            out.append((line, along, length, glyph_y.get(len(text[:i].encode("utf-8")))))
        else:
            out.append((line, along, length))
    return out


def theirs(raw, mode, styled=False):
    """The same triples from Pango's per-character rectangles."""
    rects = raw["chars"]
    thickness = float(SIZE)   # the font's ascent and descent come to the size
    out = []
    for x, y, w, h in rects:
        if styled:
            out.append((int(round(y / thickness)), x, w, y + raw["ascent"]))
        else:
            out.append((int(round(y / thickness)), x, w))
    return out


def main():
    verbose = "-v" in sys.argv
    print(oracle_env.provenance(["pango"]))
    raw = pango_chars(CASES)
    differ = compared = 0
    for case, r in zip(CASES, raw):
        name, text, mode, room = case[:4]
        mine = ours(case)
        styled = len(case) > 4
        pango = theirs(r, mode, styled)
        problems = []
        for i, ch in enumerate(text):
            if ch in "­\n" or mine[i] is None:
                continue
            if ch == " " and pango[i][2] == 0:
                continue          # a space hung at the end of a line has no length in Pango
            compared += 1
            a, b = pango[i], mine[i]
            baseline_diff = styled and (a[3] is None or abs(a[3] - b[3]) > TOLERANCE)
            if a[0] != b[0] or abs(a[1] - b[1]) > TOLERANCE or abs(a[2] - b[2]) > TOLERANCE \
                    or baseline_diff:
                problems.append("char %d %r: pango line %d at %.1f length %.1f, this line %s at "
                                "%.1f length %.1f" % (i, ch, a[0], a[1], a[2], b[0], b[1], b[2]))
        if problems:
            differ += 1
            print("%s differs:" % name)
            for line in problems[:6]:
                print("   " + line)
        elif verbose:
            print("  %s: agrees" % name)
    print("pango_layout_diff: %d cases, %d characters compared, %d cases differ"
          % (len(CASES), compared, differ))
    layout_snapshot.finish("pango")
    return 1 if differ else 0


if __name__ == "__main__":
    sys.exit(main())
