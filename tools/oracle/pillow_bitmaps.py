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
"""Pillow's reading of a PCF or BDF bitmap font, in this library's terms.

Runs **inside the pinned oracle image** (`tools/oracle/containers/`); the driver
is `bitmap_diff.py`. fontTools cannot read either of these formats at all, so the
reference here is Pillow's `PcfFontFile` and `BdfFontFile`, which descend from
PIL's X font support and share no code with this library.

Two things about that reference have to be stated up front, because a
differential that quietly worked around either would be reporting agreement it
had arranged.

**Pillow's PCF encoding is wrong when a font's first code is not zero.**
`PcfFontFile._load_encoding` builds the code-to-glyph list by indexing the
`BDF_ENCODINGS` offsets array with the *character code* rather than with
`code - firstCol`:

    for i in range(first_col, len(encoding)):
        encoding_offset = encoding_offsets[ord(bytearray([i]).decode(...))]

For a font whose codes start at 0x20 - which is most of them - every glyph is
reported 32 positions from where it is. Measured on this repository's own
`bitmap.pcf`: Pillow reports glyphs at 0x21, 0x22, 0x2C, 0x32, 0x5C and 0xDE for
a font whose codes are 0x20, 0x2E, 0x41, 0x42, 0x4C, 0x52, 0x7C and 0xFE, and
drops the two whose shifted positions fall below `firstCol`. So **this script does
not use Pillow's PCF encoding at all**: it reads the metrics and bitmap tables
through the same private methods Pillow's own constructor uses, which are keyed by
glyph index the way the file keys them, and the encoding table is left to
`bitmap_diff.py` to report as uncovered. Pillow's *BDF* encoding is correct and is
compared.

**Pillow ignores a PCF's byte order.** `_load_bitmaps` reads the bit order and
the row padding and leaves the byte-order bit commented out in its own source, so
a font whose bits and bytes run opposite ways with a scan unit of more than one
byte is read with the bytes of each unit in the wrong order. This repository's
`bitmap-lsb.pcf` is exactly that font, on purpose, and `bitmap_diff.py` counts it
as a layout the reference cannot read rather than as a disagreement - the reading
this library does is libXfont's and FreeType's.

**It reaches into Pillow's internals**, `_load_metrics` and `_load_bitmaps`, for
exactly that reason. The pin is an exact version, so the internals are as pinned
as the public API is; and the alternative was to compare the glyphs Pillow's
public list names, which for PCF are the wrong glyphs.

A **PCF's** glyphs are keyed by glyph index and a **BDF's** by codepoint, for
reasons that are each the reference's: Pillow's PCF encoding cannot be trusted, and
its BDF reader keeps no glyph order at all.

Output is one `key value` line per fact, in this library's conventions rather than
Pillow's - pixels top to bottom, `bearing_y` the top edge above the baseline and
positive upwards - so the driver compares numbers and not spellings. Pillow's own
`dst` box is y-down from the baseline, and the conversion is here, once.

Usage: pillow_bitmaps.py <font> [stride] [first]
"""

import os
import sys


def emit(key, value):
    sys.stdout.write("%s %s\n" % (key, value))


def load(path):
    """The font, and which format it turned out to be."""
    from PIL import BdfFontFile, PcfFontFile

    with open(path, "rb") as handle:
        magic = handle.read(4)
    if magic[:4] == b"\x01fcp":
        return PcfFontFile.PcfFontFile(open(path, "rb")), "pcf"
    if magic[:9] == b"STARTFONT"[:4]:
        return BdfFontFile.BdfFontFile(open(path, "rb")), "bdf"
    raise SystemExit("pillow_bitmaps: not a PCF or a BDF: %s" % path)


def rows(image, width, height):
    """The glyph as one string of `#` and `.` per row, top row first."""
    out = []
    pixels = image.load()
    for y in range(height):
        out.append("".join(
            "#" if pixels[x, y] else "." for x in range(width)))
    return out


def pcf_glyphs(font):
    """(index, advance, bearing_x, bearing_y, width, height, image) per glyph.

    Through Pillow's own table readers, keyed by glyph index. See the module
    docstring for why its public per-character list is not used here.
    """
    metrics = font._load_metrics()
    bitmaps = font._load_bitmaps(metrics)
    out = []
    for index, metric in enumerate(metrics):
        if metric is None:
            continue
        # Pillow's tuple, in its order: the box's size, its side bearings, the
        # advance, and the two halves of its height.
        (_xsize, _ysize, left, right, width, ascent, descent,
            _attributes) = metric
        out.append((index, width, left, ascent, right - left, ascent + descent,
                    bitmaps[index]))
    return out


def bdf_glyphs(font):
    """The same from Pillow's BDF reader, keyed by **character** and not by index.

    Pillow throws a BDF's glyph order away: it keeps a list indexed by character
    code, so a glyph with `ENCODING -1` - a glyph reachable only by name, which is
    a real thing a font ships - is simply absent, and every glyph after it would be
    numbered one lower than this library numbers it. Keying by codepoint is what
    makes the comparison independent of that, and `bitmap_diff.py` counts the
    unencoded glyphs as ones the reference does not report.
    """
    out = []
    for code, glyph in enumerate(font.glyph):
        if glyph is None:
            continue
        (advance, _advance_y), dst, src, image = glyph
        x0, y0, x1, y1 = dst
        out.append((code, advance, x0, -y0, x1 - x0, y1 - y0, image, src))
    return out


def main(argv):
    if len(argv) < 2:
        sys.stderr.write("usage: pillow_bitmaps.py <font> [stride] [first]\n")
        return 2
    path = argv[1]
    stride = int(argv[2]) if len(argv) > 2 else 1
    first = int(argv[3]) if len(argv) > 3 else 0

    font, kind = load(path)
    emit("format", kind)

    if kind == "pcf":
        glyphs = pcf_glyphs(font)
        emit("glyphs", len(glyphs))
        # The layout the format word states, so that the driver can tell a layout
        # Pillow does not implement from a disagreement about a glyph. It is a fact
        # about the *input*, which is the only kind of fact that can classify a
        # skip honestly: inferring it from how many glyphs came out wrong would call
        # a real defect a limitation the moment it affected enough glyphs.
        fp, fmt, _i16, _i32 = font._getformat(8)  # PCF_BITMAPS
        emit("layout", "%d %d %d %d" % (fmt & 3, 1 if fmt & 8 else 0,
            1 if fmt & 4 else 0, 1 << ((fmt & 0x30) >> 4)))
        # The properties this library reads as a font's own names. Pillow keeps
        # them as bytes, which is what they are: XLFD says a property string is
        # Latin-1.
        for name in ("FAMILY_NAME", "COPYRIGHT", "WEIGHT_NAME", "FONT_NAME",
                     "PIXEL_SIZE"):
            value = font.info.get(name.encode("ascii"))
            if value is None:
                continue
            if isinstance(value, bytes):
                value = value.decode("latin-1")
            emit("property %s" % name, value)
        for (index, advance, bearing_x, bearing_y, width, height,
                image) in glyphs:
            if index < first or (index - first) % stride:
                continue
            emit("glyph %d box" % index, "%d %d %d %d %d"
                % (width, height, bearing_x, bearing_y, advance))
            if width and height:
                for number, row in enumerate(rows(image, width, height)):
                    emit("glyph %d row %d" % (index, number), row)
        return 0

    glyphs = bdf_glyphs(font)
    emit("glyphs", len(glyphs))
    for name in ("FAMILY_NAME", "COPYRIGHT", "WEIGHT_NAME", "PIXEL_SIZE"):
        value = font.info.get(name.encode("ascii"))
        if value is None:
            continue
        if isinstance(value, bytes):
            value = value.decode("latin-1")
        emit("property %s" % name, value)
    for position, (code, advance, bearing_x, bearing_y, width, height, image,
            src) in enumerate(glyphs):
        if position < first or (position - first) % stride:
            continue
        emit("char U+%04X box" % code, "%d %d %d %d %d"
            % (width, height, bearing_x, bearing_y, advance))
        if width and height:
            # Pillow packs every BDF glyph into one wide image and hands out a
            # source rectangle into it, so the rows are cropped rather than read
            # from the origin.
            cropped = image.crop(src)
            for number, row in enumerate(rows(cropped, width, height)):
                emit("char U+%04X row %d" % (code, number), row)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
