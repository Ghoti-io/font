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
"""fontTools' disassembly of TrueType programs, for tt_decode_diff.py.

    fonttools_programs.py --hex <bytes>
    fonttools_programs.py <font> [<font>...]

Prints what font-program prints (`== fpgm`, `== prep`, `== glyph N`, then the
instructions, one a line), after stripping fontTools' trailing comments. A
program fontTools cannot read prints `?? <error>` under its heading. With
several fonts, each is preceded by `### <path>`.
"""

import re
import sys

from fontTools.ttLib import TTFont
from fontTools.ttLib.tables import ttProgram

COMMENT = re.compile(r"\t/\*.*\*/$")


def assembly(program):
    """Normalised lines of a Program, or raises."""
    return [COMMENT.sub("", line) for line in program.getAssembly()]


def from_bytes(data):
    program = ttProgram.Program()
    program.fromBytecode(data)
    return program


def emit(title, program):
    print("== %s" % title)
    try:
        lines = assembly(program)
    except Exception as why:  # noqa: BLE001 - whatever fontTools raises
        print("?? %s" % type(why).__name__)
        return
    for line in lines:
        print(line)


def font(path):
    try:
        ttf = TTFont(path, lazy=True, fontNumber=0)
    except Exception as why:  # noqa: BLE001
        print("?? font %s" % type(why).__name__)
        return
    for tag in ("fpgm", "prep"):
        try:
            if tag in ttf:
                emit(tag, ttf[tag].program)
        except Exception as why:  # noqa: BLE001
            print("== %s\n?? %s" % (tag, type(why).__name__))
    try:
        if "glyf" not in ttf:
            return
        glyf = ttf["glyf"]
    except Exception as why:  # noqa: BLE001 - a font fontTools cannot read whole
        print("?? font %s" % type(why).__name__)
        return
    for gid, name in enumerate(ttf.getGlyphOrder()):
        try:
            glyph = glyf[name]
            if not glyph.numberOfContours:
                continue
            # A composite with no instructions has no \`program\` attribute at all.
            program = getattr(glyph, "program", None)
            if program is None or not program.getBytecode():
                continue
        except Exception as why:  # noqa: BLE001
            print("== glyph %d" % gid)
            print("?? %s" % type(why).__name__)
            continue
        emit("glyph %d" % gid, program)


def main(argv):
    if len(argv) == 3 and argv[1] == "--hex":
        emit("hex", from_bytes(bytes.fromhex(argv[2])))
        return 0
    for path in argv[1:]:
        print("### %s" % path)
        font(path)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
