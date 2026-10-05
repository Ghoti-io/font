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
"""Make fonts with a layout table taken out, for the shaping differential.

    derive_fonts.py <out dir> <source>:<variant> [...]

Runs inside the fontTools image. A real font that lacks `GSUB` is rare, and the
shaper has a whole path for one: Arabic is shaped from the Unicode presentation
forms, marks are placed from the glyphs' boxes, glyph classes are guessed from the
characters. Taking a table out of a font that has it is the cheapest way to reach
that path with a font the oracle can also read. The variant names what is removed:
`nogsub`, `nogpos`, `nogdef`, or `nolayout` for all three.
"""

import os
import sys

from fontTools.ttLib import TTFont

REMOVE = {
    "nogsub": ("GSUB",),
    "nogpos": ("GPOS",),
    "nogdef": ("GDEF",),
    "nolayout": ("GSUB", "GPOS", "GDEF"),
}


def main(argv):
    out_dir = argv[1]
    for item in argv[2:]:
        source, variant = item.rsplit(":", 1)
        base = os.path.splitext(os.path.basename(source))[0]
        target = os.path.join(out_dir, "%s.%s.ttf" % (base, variant))
        if os.path.exists(target) and \
                os.path.getmtime(target) >= os.path.getmtime(source):
            continue
        font = TTFont(source, fontNumber=0)
        for table in REMOVE[variant]:
            if table in font:
                del font[table]
        font.save(target)
        print(target)


main(sys.argv)
