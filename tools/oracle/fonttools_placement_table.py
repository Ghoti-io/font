# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Font.
#
# Ghoti.io Font is free software: you can redistribute it and/or modify it
# under the terms of the GNU Lesser General Public License version 3 as
# published by the Free Software Foundation.
"""fontTools' own drawing of the placement fixtures: each glyph's left edge.

Runs **inside** the pinned image, called by `placement_diff.py`. The glyph set draws
through a decomposing pen, which is how fontTools places a glyph: a simple glyph by
`lsb - xMin`, a composite not at all (design.md section 7.3). Nothing here
computes a placement; this is the reference as it answers.

Usage: fonttools_placement_table.py <font> [wght normalised]
"""

import sys

from fontTools.pens.recordingPen import DecomposingRecordingPen
from fontTools.ttLib import TTFont


def main(argv):
    font = TTFont(argv[1])
    if len(argv) > 2:
        glyph_set = font.getGlyphSet(location={"wght": float(argv[2])},
                                     normalized=True)
    else:
        glyph_set = font.getGlyphSet()
    edges = []
    for name in font.getGlyphOrder()[2:]:
        pen = DecomposingRecordingPen(glyph_set)
        glyph_set[name].draw(pen)
        xs = [point[0] for _, args in pen.value for point in args if point]
        edges.append(min(xs) if xs else None)
    print(" ".join("-" if edge is None else "%g" % edge for edge in edges))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
