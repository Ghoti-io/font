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
"""The glyph path provider's cross-target probe, on this machine (story 14b).

tools/scene-hash.c renders AD-5's and AD-1's fixtures through shape-cpu, checks
both equalities and prints a hash of what it drew. The committed hash is
tests/data/golden/scene.hash; suite/tools/xarch/shape.sh and
suite/tools/xwin/shape.sh require the same number on every target. This gate
requires it here, so that the committed number is the host's and moves only on
purpose (`make scene-hash` rewrites it, as `make golden` rewrites the
renderings), and shows the comparison can fail: the probe run with its plant
(FONT_SCENE_PLANT=1, one bit flipped) must not match.

    check-scene-hash.py PROBE FONTS_DIR [--update]
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
COMMITTED = os.path.join(os.path.dirname(HERE), "tests", "data", "golden", "scene.hash")


def run(probe, fonts, **env):
    done = subprocess.run([probe, fonts], capture_output=True, text=True, env={**os.environ, **env})
    fields = {}
    for line in done.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 2:
            fields.setdefault(parts[0], []).append(parts[1:])
    if done.returncode != 0:
        fails = [" ".join(x) for x in fields.get("font-scene.FAIL", [])]
        sys.exit("check-scene-hash: the probe failed (exit %d): %s\n%s"
                 % (done.returncode, "; ".join(fails) or "no FAIL line", done.stderr[-400:]))
    h = fields.get("font-scene.hash", [[None]])[-1][0]
    c = fields.get("font-scene.count", [[None]])[-1][0]
    if not h or not c:
        sys.exit("check-scene-hash: the probe printed no hash")
    return h, c, fields.get("font-scene.fixture", [])


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) != 2:
        sys.exit(__doc__)
    probe, fonts = args
    h, c, fixtures = run(probe, fonts)
    if "--update" in sys.argv:
        with open(COMMITTED, "w") as f:
            f.write("# tools/scene-hash.c over tests/data/fonts: AD-5's and AD-1's fixtures through shape-cpu\n")
            f.write("# (story 14b). Written by `make scene-hash`; every target must print it.\n")
            for name, value in fixtures:
                f.write("fixture %s %s\n" % (name, value))
            f.write("hash %s\ncount %s\n" % (h, c))
        print("check-scene-hash: wrote %s: %s over %s renders" % (COMMITTED, h, c))
        return
    want = {}
    with open(COMMITTED) as f:
        for line in f:
            parts = line.split()
            if len(parts) == 2 and parts[0] in ("hash", "count"):
                want[parts[0]] = parts[1]
    if (h, c) != (want.get("hash"), want.get("count")):
        sys.exit("check-scene-hash: the probe prints %s over %s renders; %s commits %s over %s"
                 % (h, c, COMMITTED, want.get("hash"), want.get("count")))
    planted, _, _ = run(probe, fonts, FONT_SCENE_PLANT="1")
    if planted == h:
        sys.exit("check-scene-hash: the planted bit printed the committed hash: the comparison sees nothing")
    print("check-scene-hash: %s over %s renders, as committed; AD-5 and AD-1 hold; the planted bit is refused"
          % (h, c))


if __name__ == "__main__":
    main()
