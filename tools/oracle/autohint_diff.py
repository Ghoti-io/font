#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Font.
"""This library's automatic hinter against FreeType's light one, on the Latin letters, point by point.

    autohint_diff.py [--ppem N[,N...]] [--letters ABC...] [--show N] [--points] [--records FILE] [--floor F] [FONT...]

For each face, size and Latin letter, the outline `font-hint-cff <font> ... auto` prints against the one
`freetype-hinted ... autolight` prints, in 26.6, both axes and the advance. Reports the glyphs identical,
the points identical and the letters that fail most; exits nonzero below `--floor`. The model the engine
was written from is `autohint_edges_model.py`, whose own comparison is `autohint_edges_diff.py`.
"""
import collections
import json
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import corpus
import hint_cff_diff
import hint_diff
import oracle_env

DEFAULT = ["DejaVuSans.ttf", "DejaVuSerif.ttf", "DejaVuSans-Bold.ttf", "LiberationSans-Regular.ttf",
           "LiberationSerif-Regular.ttf", "LiberationSans-Bold.ttf", "NimbusSans-Regular.otf",
           "NimbusRoman-Regular.otf", "NimbusRoman-Bold.otf", "C059-Roman.otf"]


def main(argv):
    ppems, names, letters, show, floor, points = [9, 11, 12, 14, 17, 24], [], None, 8, 0.0, False
    records = None
    args = argv[1:]
    while args:
        a = args.pop(0)
        if a == "--ppem":
            ppems = [int(v) for v in args.pop(0).split(",")]
        elif a == "--letters":
            letters = args.pop(0)
        elif a == "--show":
            show = int(args.pop(0))
        elif a == "--records":
            records = open(args.pop(0), "w")
        elif a == "--points":
            points = True
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
    glyphs_total = glyphs_same = points_total = points_same = adv_bad = 0
    by_letter = collections.Counter()
    seen = 0
    for path in paths:
        letters_of = {g["gid"]: ch for ch, g in faces[os.path.basename(path)]["glyphs"].items()
                      if letters is None or ch in letters}
        for ppem in ppems:
            mine = hint_cff_diff.ours(path, ppem, 1, "-", True)
            theirs, _ = hint_diff.theirs(path, 35, ppem, 1, "-", "autolight")
            if mine is None or theirs is None:
                continue
            for gid, ch in letters_of.items():
                a, b = mine.get(gid), theirs.get(gid)
                if a is None or b is None or isinstance(a, str) or isinstance(b, str) or len(a[1]) != len(b[1]):
                    continue
                bad = [i for i, (p, q) in enumerate(zip(a[1], b[1])) if p[0] != q[0] or p[1] != q[1]]
                if records:
                    g = faces[os.path.basename(path)]["glyphs"][ch]
                    json.dump({"font": os.path.basename(path), "ppem": ppem, "ch": ch, "upem": faces[os.path.basename(path)]["upem"],
                               "pts": g["pts"], "ends": g["ends"], "ours": [list(p[:2]) for p in a[1]],
                               "theirs": [list(p[:2]) for p in b[1]]}, records)
                    records.write("\n")
                glyphs_total += 1
                points_total += len(a[1])
                points_same += len(a[1]) - len(bad)
                adv_bad += a[0] != b[0]
                if not bad:
                    glyphs_same += 1
                else:
                    by_letter[ch] += 1
                    if seen < show:
                        seen += 1
                        i = bad[0]
                        if points:
                            orig = faces[os.path.basename(path)]["glyphs"][ch]
                            for k, (p, q) in enumerate(zip(a[1], b[1])):
                                print("   %3d %s orig %s ours %s theirs %s%s" % (
                                    k, "on " if orig["pts"][k][2] else "off", orig["pts"][k][:2], p[:2], q[:2],
                                    "  <--" if k in bad else ""))
                        print("%s ppem %d %s: %d of %d points differ, e.g. point %d ours %s theirs %s" % (
                            os.path.basename(path), ppem, ch, len(bad), len(a[1]), i, a[1][i][:2], b[1][i][:2]))
    print("compared %d glyphs: %d identical (%.1f%%); points %d of %d identical; %d advances differ" % (
        glyphs_total, glyphs_same, 100.0 * glyphs_same / max(1, glyphs_total), points_same, points_total, adv_bad))
    print("most failing letters:", by_letter.most_common(14))
    return 0 if glyphs_same / max(1, glyphs_total) >= floor else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
