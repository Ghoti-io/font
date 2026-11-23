#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Font.
"""The automatic hinter's global model against FreeType's (autohinter plan, stage A1).

    autohint_zones_diff.py [--ppem N[,N...]] [FONT...]

For each face and size, the whole-pixel height FreeType's light automatic hinter gives the top of x (flat),
o (round), H (flat) and the bottom of p, against the height `autohint_model.py` predicts from the face's own
letters. Exits nonzero on any difference. Needs both reference images.
"""
import json
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import autohint_model as model
import corpus
import hint_cff_explore as explore
import oracle_env

DEFAULT = ["DejaVuSans.ttf", "DejaVuSerif.ttf", "DejaVuSans-Bold.ttf", "LiberationSans-Regular.ttf",
           "LiberationSerif-Regular.ttf", "LiberationSans-Bold.ttf", "NimbusSans-Regular.otf",
           "NimbusRoman-Regular.otf", "NimbusRoman-Bold.otf", "C059-Roman.otf"]


def main(argv):
    ppems, names = [8, 9, 10, 11, 12, 13, 14, 15, 16, 18, 20, 24], []
    args = argv[1:]
    while args:
        a = args.pop(0)
        if a == "--ppem":
            ppems = [int(v) for v in args.pop(0).split(",")]
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
    total = bad = 0
    for path in paths:
        face = faces[os.path.basename(path)]
        glyphs, upem = face["glyphs"], face["upem"]
        z = model.zones(glyphs)
        for ppem in ppems:
            auto = explore.freetype(path, 35, ppem, 1, "autolight")
            if not auto:
                continue

            def seen(ch, fn):
                return fn(y for _, y in auto[glyphs[ch]["gid"]][1]) / 64

            want = model.predict(z, ppem, upem)
            got = {"x": seen("x", max), "o": seen("o", max), "H": seen("H", max), "p": seen("p", min)}
            for key in want:
                total += 1
                if abs(want[key] - got[key]) > 0.02:
                    bad += 1
                    print("%s ppem %d %s: model %d, FreeType %.2f" % (os.path.basename(path), ppem, key, want[key], got[key]))
    print("autohint_zones_diff: %d of %d heights agree" % (total - bad, total))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
