#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Font.
"""The automatic hinter's glyph model against FreeType's, point by point (autohinter plan, stages A2 and A3).

    autohint_edges_diff.py [--ppem N[,N...]] [--letters ABC...] [--show N] [FONT...]

For each face, size and Latin letter, the y of every point `autohint_edges_model.py` gives, against the y of
FreeType's light automatic hinter, to one 26.6 unit. Reports the glyphs identical, the points identical and the
letters that fail most; exits nonzero below `--floor`. Needs both reference images.
"""
import collections
import json
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import autohint_edges_model as edges
import autohint_model as model
import corpus
import hint_cff_explore as explore
import oracle_env

DEFAULT = ["DejaVuSans.ttf", "DejaVuSerif.ttf", "DejaVuSans-Bold.ttf", "LiberationSans-Regular.ttf",
           "LiberationSerif-Regular.ttf", "LiberationSans-Bold.ttf", "NimbusSans-Regular.otf",
           "NimbusRoman-Regular.otf", "NimbusRoman-Bold.otf", "C059-Roman.otf"]


def main(argv):
    ppems, names, letters, show, floor = [9, 11, 12, 14, 17, 24], [], None, 8, 0.0
    args = argv[1:]
    while args:
        a = args.pop(0)
        if a == "--ppem":
            ppems = [int(v) for v in args.pop(0).split(",")]
        elif a == "--letters":
            letters = args.pop(0)
        elif a == "--show":
            show = int(args.pop(0))
        elif a == "--floor":
            floor = float(args.pop(0))
        else:
            names.append(a)
    names = names or DEFAULT
    paths = [p for n in names for p in corpus.fonts("sfnt") if p.endswith("/" + n)]
    here = os.path.dirname(os.path.abspath(__file__))
    with tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR") or None) as scratch:
        dump = os.path.join(scratch, "faces.json")
        command = oracle_env.command("fonttools", argv=["python3", os.path.join(here, "autohint_dump.py"), dump] + paths,
                                     scratch=scratch)
        done = subprocess.run(command, capture_output=True, text=True)
        if done.returncode != 0:
            sys.stderr.write(done.stderr[-800:])
            return 2
        faces = json.load(open(dump))
    glyphs_total = glyphs_same = points_total = points_same = 0
    by_letter = collections.Counter()
    seen = 0
    for path in paths:
        face = faces[os.path.basename(path)]
        G, upem = face["glyphs"], face["upem"]
        z = model.zones(G)
        stdw = edges.standard_width(G, upem)
        for ppem in ppems:
            auto = explore.freetype(path, 35, ppem, 1, "autolight")
            if not auto:
                continue
            for ch, glyph in G.items():
                if letters and ch not in letters:
                    continue
                theirs = auto.get(glyph["gid"])
                if isinstance(theirs, str) or theirs is None or len(theirs[1]) != len(glyph["pts"]):
                    continue
                ours, _ = edges.hint(glyph, z, ppem, upem, stdw=stdw)
                bad = [i for i, (a, b) in enumerate(zip(ours, theirs[1])) if abs(a - b[1]) > 1.0]
                glyphs_total += 1
                points_total += len(ours)
                points_same += len(ours) - len(bad)
                if not bad:
                    glyphs_same += 1
                else:
                    by_letter[ch] += 1
                    if seen < show:
                        seen += 1
                        print("%s ppem %d %s: %d of %d points differ, e.g. point %d ours %.2f theirs %.2f" % (
                            os.path.basename(path), ppem, ch, len(bad), len(ours), bad[0], ours[bad[0]] / 64,
                            theirs[1][bad[0]][1] / 64))
    print("compared %d glyphs: %d identical (%.1f%%); points %d of %d identical" % (
        glyphs_total, glyphs_same, 100.0 * glyphs_same / max(1, glyphs_total), points_same, points_total))
    print("most failing letters:", by_letter.most_common(14))
    return 0 if glyphs_same / max(1, glyphs_total) >= floor else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
