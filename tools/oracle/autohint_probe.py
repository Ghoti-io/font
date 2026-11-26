#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Font.
"""Controlled shapes through this library's automatic hinter and FreeType's, point by point.

    autohint_probe.py [--ppem N,N,...] [--show] <experiment>

Real glyphs set many rules at once, so a disagreement on one cannot say which. Each experiment here builds a
family of tiny glyphs that differ in one property, hints them both ways and prints where each puts every point
of interest. An experiment is a function returning (labels, glyph contours, the indices of the points to report).
"""
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import hint_cff_diff
import hint_diff
import oracle_env

ROOT = os.path.dirname(os.path.dirname(HERE))
SCRATCH = os.path.join(ROOT, "build", "scratch", "probe")


def build(glyphs, upem=1000, name="probe", overshoot=12, script="latin", heights=None, o_stem=None, rings=None, letters=None):
    os.makedirs(SCRATCH, exist_ok=True)
    spec = os.path.join(SCRATCH, name + ".json")
    out = os.path.join(SCRATCH, name + ".ttf")
    json.dump({"upem": upem, "overshoot": overshoot, "script": script, "heights": heights or {}, "o_stem": o_stem, "rings": rings or {}, "letters": letters or {},
               "glyphs": [{"contours": g} for g in glyphs]}, open(spec, "w"))
    command = oracle_env.command("fonttools", ["python3", os.path.join(HERE, "autohint_probe_font.py"), spec, out],
                                 scratch=SCRATCH)
    done = subprocess.run(command, capture_output=True, text=True)
    if done.returncode != 0:
        raise SystemExit(done.stderr[-800:])
    return out


def hint(path, ppem):
    """({gid: points}, {gid: points}) for ours and FreeType's, each point (x, y, on) in 26.6."""
    mine = hint_cff_diff.ours(path, ppem, 1, "-", True)
    theirs, err = hint_diff.theirs(path, 35, ppem, 1, "-", "autolight")
    if mine is None or theirs is None:
        raise SystemExit("a hinter failed: " + (err or "ours"))
    return mine, theirs


def run(labels, glyphs, report, ppems, upem=1000, name="probe", show=True, overshoot=12, script="latin", heights=None):
    """Print, per glyph and size, the y of the reported points: ours then FreeType's, marked where they differ."""
    path = build(glyphs, upem, name, overshoot, script, heights)
    rows = []
    for ppem in ppems:
        mine, theirs = hint(path, ppem)
        for i, label in enumerate(labels):
            gid = i + 1
            m, t = mine.get(gid), theirs.get(gid)
            if not (isinstance(m, tuple) and isinstance(t, tuple)):
                rows.append((label, ppem, None, None))
                continue
            ys_m = [m[1][k][1] for k in report[i]]
            ys_t = [t[1][k][1] for k in report[i]]
            rows.append((label, ppem, ys_m, ys_t))
    if show:
        for label, ppem, m, t in rows:
            flag = "" if m == t else "   <-- differs"
            print("%-28s ppem %2d ours %s theirs %s%s" % (label, ppem, m, t, flag))
    return rows


def main(argv):
    ppems = [12, 17, 24]
    args = argv[1:]
    if args and args[0] == "--ppem":
        ppems = [int(v) for v in args[1].split(",")]
        args = args[2:]
    if not args:
        print(__doc__)
        return 2
    import autohint_probe_experiments as ex
    getattr(ex, args[0])(run, ppems)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
