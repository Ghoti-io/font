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
"""fontTools' side of the WOFF 2 differential, run inside the oracle image.

    fonttools_woff2.py make <scratch> <font>...
    fonttools_woff2.py compare <scratch> <font>...

`make` writes each font as a WOFF 2 file three ways - the default transforms
(`glyf` and `loca`), those plus `hmtx`, and none at all - and, for each, the
tables fontTools reads back out of that file, one file per table named by the tag
as eight hex digits (the way `font-tables` names them). `compare` then reads the
tables `font-tables` wrote from the same WOFF 2 file and says where they differ
from fontTools' and from the font the file was made from.

`glyf` is compared glyph by glyph, not byte by byte: both decoders must produce
the same outline, but the byte layout of a simple glyph (which flag repeats it
uses, where it pads) is the decoder's own.
"""

import io
import json
import os
import sys

from fontTools.ttLib import TTFont, woff2
from fontTools.ttLib.sfnt import SFNTWriter

VARIANTS = {
    "default": {"glyf", "loca"},
    "hmtx": {"glyf", "loca", "hmtx"},
    "stored": set(),
}


def key(path):
    return path.replace("/", "_")


def make(scratch, fonts):
    report = {}
    for path in fonts:
        font = TTFont(path)
        report[path] = {}
        for variant, transformed in VARIANTS.items():
            if "glyf" not in font and variant != "default":
                continue
            font = TTFont(path)
            font.flavor = "woff2"
            font.flavorData = woff2.WOFFFlavorData()
            font.flavorData.transformedTables = set(
                t for t in transformed if t in font)
            target = os.path.join(scratch, "%s.%s.woff2" % (key(path), variant))
            try:
                font.save(target)
            except Exception as why:
                report[path][variant] = "refused: %s" % why
                continue
            back = TTFont(target)
            ref = target + ".ref"
            os.makedirs(ref, exist_ok=True)
            for tag in back.reader.tables:
                data = back.reader[tag]
                name = "%08x" % int.from_bytes(tag.encode("latin-1").ljust(4)[:4], "big")
                with open(os.path.join(ref, name), "wb") as handle:
                    handle.write(data)
            report[path][variant] = {
                "transform": {t: e.transformVersion
                              for t, e in back.reader.tables.items()
                              if t in ("glyf", "loca", "hmtx")}}
    print(json.dumps(report))


def tables_of(directory):
    out = {}
    for name in os.listdir(directory):
        tag = int(name, 16).to_bytes(4, "big").decode("latin-1")
        with open(os.path.join(directory, name), "rb") as handle:
            out[tag] = handle.read()
    return out


def glyph_view(font, name):
    g = font["glyf"][name]
    if g.numberOfContours == 0:
        return ("empty",)
    if g.isComposite():
        return ("composite", g.xMin, g.yMin, g.xMax, g.yMax,
                tuple((c.glyphName, c.flags & ~0x0100 & ~0x0200 & ~0x0020 & ~0x0004,
                       c.x, c.y, getattr(c, "transform", None))
                      for c in g.components),
                bytes(g.program.getBytecode()) if hasattr(g, "program") else b"")
    coords, ends, flags = g.getCoordinates(font["glyf"])
    return ("simple", g.xMin, g.yMin, g.xMax, g.yMax, tuple(coords), tuple(ends),
            tuple(f & 0x41 for f in flags), bytes(g.program.getBytecode()))


def assemble(tables, flavor):
    sink = io.BytesIO()
    writer = SFNTWriter(sink, len(tables), sfntVersion=flavor)
    for tag in sorted(tables):
        writer[tag] = tables[tag]
    writer.close()
    sink.seek(0)
    return TTFont(sink)


EXPLAINED = []


def compare_one(path, original, variant, scratch, problems):
    target = os.path.join(scratch, "%s.%s.woff2" % (key(path), variant))
    if not os.path.exists(target):
        return 0
    ours_dir = target + ".ours"
    if not os.path.isdir(ours_dir):
        problems.append("%s %s: nothing decoded" % (path, variant))
        return 1
    ours = tables_of(ours_dir)
    ref = tables_of(target + ".ref")
    if set(ours) != set(ref):
        problems.append("%s %s: tables %s differ" % (
            path, variant, sorted(set(ours) ^ set(ref))))
        return 1
    for tag in sorted(ref):
        if tag in ("glyf", "loca"):
            continue
        a, b = ours[tag], ref[tag]
        if tag == "head":
            a, b = a[:8] + a[12:], b[:8] + b[12:]
        if a != b:
            # fontTools' decode of a transformed `hmtx` can come back shorter than
            # the table its own writer was given (and than the origLength the file
            # states) for a font with a handful of proportional metrics and many
            # glyphs. When this library's table is the source font's, byte for
            # byte, the reference is the one that lost entries.
            if (tag == "hmtx" and variant == "hmtx"
                    and a == original.reader["hmtx"] and len(b) < len(a)):
                EXPLAINED.append(path)
                continue
            problems.append("%s %s: %s differs (%d vs %d bytes)" % (
                path, variant, tag, len(ours[tag]), len(ref[tag])))
    if "glyf" in ref and len(original["glyf"].glyphs) > 0:
        mine = assemble(ours, original.sfntVersion)
        theirs = TTFont(target)
        # Names come from `post`, or are made up from `cmap`; both fonts are
        # read the same way, so a glyph is looked up by its place.
        order_mine = mine.getGlyphOrder()
        order_theirs = theirs.getGlyphOrder()
        order_original = original.getGlyphOrder()
        if not len(order_mine) == len(order_theirs) == len(order_original):
            problems.append("%s %s: %d, %d and %d glyphs" % (
                path, variant, len(order_mine), len(order_theirs),
                len(order_original)))
            return 1
        for i, name in enumerate(order_mine):
            if glyph_view(mine, name) != glyph_view(theirs, order_theirs[i]):
                problems.append("%s %s: glyph %d differs from fontTools'"
                                % (path, variant, i))
                break
        for i, name in enumerate(order_mine):
            if glyph_view(mine, name) != glyph_view(original, order_original[i]):
                problems.append("%s %s: glyph %d differs from the source"
                                % (path, variant, i))
                break
    return 1


def compare(scratch, fonts):
    problems = []
    checked = 0
    for path in fonts:
        try:
            original = TTFont(path)
            for variant in VARIANTS:
                checked += compare_one(path, original, variant, scratch, problems)
        except Exception as why:
            problems.append("%s: %s: %s" % (path, type(why).__name__, why))
    print(json.dumps({"checked": checked, "problems": problems,
                      "explained": EXPLAINED}))


if __name__ == "__main__":
    mode, scratch, fonts = sys.argv[1], sys.argv[2], sys.argv[3:]
    {"make": make, "compare": compare}[mode](scratch, fonts)
