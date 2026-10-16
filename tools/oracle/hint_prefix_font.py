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
"""Copy a font with one glyph's program cut after N instructions.

    hint_prefix_font.py <font> <glyph> <instructions> <out.ttf>

For hint_bisect.py: the first N where this library and FreeType disagree names
the instruction that made them. Prints how many instructions the program has.
"""

import sys

from fontTools.ttLib import TTFont
from fontTools.ttLib.tables import ttProgram


def boundaries(code):
    """Offsets at which each instruction of a program ends."""
    ends = []
    at = 0
    while at < len(code):
        op = code[at]
        if op == 0x40:
            at += 2 + code[at + 1] if at + 1 < len(code) else 1
        elif op == 0x41:
            at += 2 + 2 * code[at + 1] if at + 1 < len(code) else 1
        elif 0xB0 <= op <= 0xB7:
            at += 1 + (op - 0xB0 + 1)
        elif 0xB8 <= op <= 0xBF:
            at += 1 + 2 * (op - 0xB8 + 1)
        else:
            at += 1
        ends.append(min(at, len(code)))
    return ends


def main(argv):
    font = TTFont(argv[1])
    glyf = font["glyf"]
    glyph = glyf[font.getGlyphOrder()[int(argv[2])]]
    code = bytes(glyph.program.getBytecode())
    ends = boundaries(code)
    cut = min(int(argv[3]), len(ends))
    program = ttProgram.Program()
    suffix = bytes.fromhex(argv[5]) if len(argv) > 5 else b""
    program.fromBytecode((code[:ends[cut - 1]] if cut else b"") + suffix)
    glyph.program = program
    font.save(argv[4])
    print(len(ends))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
