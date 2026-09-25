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
"""Print one `cmap` subtable's whole mapping, as fontTools reads it.

    fonttools_cmap.py <font> <face-index> <platform> <encoding>

Runs **inside the fonttools image** (tools/oracle/containers/fonttools), which
is why it imports fontTools unconditionally: the only way this file runs at all
is through `oracle_env.command()`, which has already proved the reference is
there and matches its pin.

Output is a batch, one `<codepoint> <glyph-id>` line per mapped codepoint, so
that a differential over a few hundred fonts pays for one process per font
rather than one per lookup. Glyph *ids* rather than names: fontTools' subtable
dictionaries are keyed by codepoint and valued by glyph name, and a name is one
indirection away from what this library answers - the indirection is done here,
beside the reference that defines it.

A codepoint absent from the output is unmapped, which is the same statement as
this library's glyph 0.
"""

import sys

from fontTools.ttLib import TTFont


def main(argv):
    if len(argv) != 5:
        sys.stderr.write(
            "usage: fonttools_cmap.py <font> <face> <platform> <encoding>\n")
        return 2
    path, face, platform, encoding = argv[1], int(argv[2]), int(argv[3]), \
        int(argv[4])

    # lazy=True so that a font's outlines are never parsed: this asks about one
    # table and a 30 MB CJK font should cost one table.
    font = TTFont(path, fontNumber=face, lazy=True)
    if "cmap" not in font:
        sys.stderr.write("%s: no cmap\n" % path)
        return 1

    chosen = None
    for subtable in font["cmap"].tables:
        if subtable.platformID == platform and subtable.platEncID == encoding:
            # First match wins, which is what this library's selection does on a
            # tie as well - both walk the table in file order.
            chosen = subtable
            break
    if chosen is None:
        sys.stderr.write("%s: no (%d,%d) subtable\n" % (path, platform,
                                                       encoding))
        return 1

    mapping = getattr(chosen, "cmap", None)
    if mapping is None:
        # Format 14 and its relatives carry variation sequences rather than a
        # plain mapping. This library does not select such a subtable, so being
        # asked for one is a defect in the caller rather than a font's problem.
        sys.stderr.write("%s: (%d,%d) format %s has no plain mapping\n"
                         % (path, platform, encoding, chosen.format))
        return 1

    print("# subtable %d %d %d" % (platform, encoding, chosen.format))
    out = sys.stdout
    for codepoint in sorted(mapping):
        name = mapping[codepoint]
        out.write("%d %d\n" % (codepoint, font.getGlyphID(name)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
