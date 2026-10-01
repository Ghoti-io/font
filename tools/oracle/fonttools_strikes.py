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
"""fontTools' reading of an sfnt's `EBLC`/`EBDT` strikes, as comparable lines.

Runs **inside the oracle image**, where fontTools is pinned. `eblc_diff.py` on
the host runs this and `examples/font-bitmap --strikes`, and compares the two
line for line. The shapes are identical on purpose: every line is
`<key> <value>`, so a disagreement names a field rather than a line somebody has
to diff by eye.

Three things about the reference are worth stating here rather than in the
differential, because they are facts about fontTools and not about the
comparison:

  * **The metrics of a glyph come from two different places**, and which one is
    the image format's business. Formats 1, 2, 6 and 7 store them with the
    pixels and fontTools puts them on the glyph; format 5 stores one set in the
    `EBLC` index subtable for every glyph it covers and fontTools leaves the
    glyph without any. So `metrics_of()` below reads the glyph's own and falls
    back to its subtable's - which is the same two-source rule this library
    implements, and the reason `strike-formats.ttf` draws one design in six
    pairings: if either side had the rule wrong, the pixels would survive and the
    box would not.

  * **A row comes back byte-aligned and MSB-first whatever the format stores.**
    `getRow()` does for the reference what `gfnt_bitmap_normalise_row()` does
    here, so the comparison is of pixels rather than of packings - and the bits
    past the width are **not** compared, because this library zeroes them and
    fontTools does not promise to. That invariant is the unit suite's
    (`tests/unit/test_bitmap.cpp`), and it belongs there: it is a property of one
    reader, not an agreement between two.

  * **`ascent` and `descent` are selected by the strike's `flags`**, not summed
    or averaged: bit 0 says the strike is for horizontal text and bit 1 vertical,
    and `sbitLineMetrics` is stated twice. The selection is the specification's
    rule and both sides apply it. Nothing in Debian has `flags` of anything but
    1, so what covers the vertical arm is a fixture and not this population -
    `eblc_diff.py` prints how many strikes it saw of each, so the hole is a
    number in the report rather than an assumption.

Usage:
    fonttools_strikes.py <font> [face-index] [stride]
"""

import sys

from fontTools.ttLib import TTCollection, TTFont


def metrics_of(glyph, subtable):
    """The glyph's own metrics, or the constant ones its subtable states.

    Index formats 2 and 5 state one `BigGlyphMetrics` for every glyph they cover
    and the pixels carry none; every other format puts a `SmallGlyphMetrics` or a
    `BigGlyphMetrics` in front of the pixels. fontTools models that difference by
    leaving `metrics` off the glyph, so an adapter that only ever read one of the
    two would report a box of zeros for 97.8% of the real population.
    """
    own = getattr(glyph, "metrics", None)
    if own is not None:
        return own
    return getattr(subtable, "metrics", None)


def box(metrics):
    """(width, height, bearingX, bearingY, advance), whichever struct this is.

    `BigGlyphMetrics` names its five horizontal fields `horiBearingX` and so on
    because it carries a vertical three beside them; `SmallGlyphMetrics` names
    them `BearingX`, `BearingY` and `Advance` with no prefix, because the
    specification says which direction they are in is the **strike's** to state
    through its `flags` and the struct has only one set. So the two cannot be
    read by one spelling, and an adapter that assumed the big one reports nothing
    for image formats 1 and 2.

    Nothing in the population has a strike whose `flags` are anything but
    horizontal, so the case where a small struct's bearings are *vertical* is
    untested by this differential and `eblc_diff.py` counts the strikes it saw of
    each kind rather than leaving that unsaid.
    """
    if hasattr(metrics, "horiBearingX"):
        return (metrics.width, metrics.height, metrics.horiBearingX,
                metrics.horiBearingY, metrics.horiAdvance)
    return (metrics.width, metrics.height, metrics.BearingX, metrics.BearingY,
            metrics.Advance)


def art(glyph, metrics, depth):
    """One string per row, `#` for a set pixel, truncated to the width.

    Truncated rather than padded: `getRow()` returns whole bytes and the bits
    past the width are nobody's promise. See the module docstring.
    """
    rows = []
    for y in range(metrics.height):
        data = glyph.getRow(y, bitDepth=depth, metrics=metrics)
        bits = "".join("#" if (byte >> (7 - bit)) & 1 else "."
                       for byte in data for bit in range(8))
        rows.append(bits[:metrics.width])
    return rows


def faces(path):
    """Every face of the file, and how many there are.

    A `.ttc` is the shape this format actually ships in - `uming.ttc` is four
    faces over one `EBLC` - so the collection case is the normal one here rather
    than an afterthought.
    """
    if path.lower().endswith((".ttc", ".otc")):
        collection = TTCollection(path, lazy=True)
        return collection.fonts
    return [TTFont(path, lazy=True)]


def report(path, index, stride, out):
    """Print every comparable fact about one face's strikes."""
    found = faces(path)
    if index >= len(found):
        raise SystemExit("face %d of a file with %d" % (index, len(found)))
    font = found[index]
    glyphs = font["maxp"].numGlyphs
    order = font.getGlyphOrder()

    # EBLC first and on its own line of code, because when it raises - mona.ttf
    # (fonttools#317) - the half-built table is already cached on the TTFont and
    # `EBDT` will then decompile against a locator that stopped early. A reader
    # that asked for both and caught one exception would report a strike count
    # from a table that failed to parse.
    eblc = font["EBLC"]
    ebdt = font["EBDT"]

    out.write("faces %d\n" % len(found))
    out.write("glyphs %d\n" % glyphs)
    out.write("strikes %d\n" % len(eblc.strikes))

    # The census: facts only the reference reports, which `eblc_diff.py` prints
    # as what the run covered rather than comparing. This library has no public
    # accessor for an index format, an image format or a table's offset - by
    # design, they are how a strike is stored and not what a caller asks for - so
    # a census line is not a disagreement waiting to happen. It is the answer to
    # "which cells of the format grid did that clean number actually visit",
    # which a differential that printed only a total cannot give.
    for tag in ("EBLC", "EBDT"):
        entry = font.reader.tables[tag]
        out.write("census table %s offset %d length %d\n"
                  % (tag, entry.offset, entry.length))
    for number, strike in enumerate(eblc.strikes):
        size = strike.bitmapSizeTable
        out.write("census strike %d flags %d depth %d subtables %d\n"
                  % (number, size.flags, size.bitDepth,
                     len(strike.indexSubTables)))
        pairs = {}
        for subtable in strike.indexSubTables:
            key = (subtable.indexFormat, subtable.imageFormat)
            pairs[key] = pairs.get(key, 0) + 1
        for (index_format, image_format), count in sorted(pairs.items()):
            out.write("census pair %d %d count %d\n"
                      % (index_format, image_format, count))

    for number, strike in enumerate(eblc.strikes):
        size = strike.bitmapSizeTable
        # Bit 0 horizontal, bit 1 vertical. The specification's rule, applied by
        # both sides; see the module docstring on why that is not circular.
        line = size.hori if size.flags & 1 else size.vert
        data = ebdt.strikeData[number]

        # Which subtable covers a glyph, so that a format-5 glyph can find the
        # constant metrics its subtable states.
        owner = {}
        for subtable in strike.indexSubTables:
            for name in subtable.names:
                owner[name] = subtable

        present = sum(1 for name in order if name in data)
        out.write("strike %d ppem_x %d\n" % (number, size.ppemX))
        out.write("strike %d ppem_y %d\n" % (number, size.ppemY))
        out.write("strike %d depth %d\n" % (number, size.bitDepth))
        out.write("strike %d ascent %d\n" % (number, line.ascender))
        out.write("strike %d descent %d\n" % (number, line.descender))
        out.write("strike %d present %d\n" % (number, present))
        out.write("strike %d absent %d\n" % (number, glyphs - present))
        # Always zero, and compared anyway: this reference has no per-glyph
        # refusal, so a font it parsed at all is a font it found no bad glyph in.
        # That is a claim about the font, and this library makes the same one in a
        # form that can differ (M11) - so the two being equal is a comparison and
        # not a tautology.
        out.write("strike %d corrupt 0\n" % number)

        for glyph in range(glyphs):
            # Every stride'th glyph **and the last**, which is the rule
            # `font-bitmap --strikes` applies: the final entry of an offset array
            # is the one a producer gets wrong, so a stride that stepped over it
            # would leave the one glyph that matters unsampled.
            if glyph % stride != 0 and glyph + 1 != glyphs:
                continue
            name = order[glyph]
            if name not in data:
                out.write("g %d %d state absent\n" % (glyph, number))
                continue
            bitmap = data[name]
            metrics = metrics_of(bitmap, owner.get(name))
            if metrics is None:
                raise SystemExit("no metrics for glyph %d of strike %d"
                                 % (glyph, number))
            out.write("g %d %d state present\n" % (glyph, number))
            width, height, bearing_x, bearing_y, advance = box(metrics)
            out.write("g %d %d box %d %d %d %d %d %d\n"
                      % (glyph, number, width, height, bearing_x, bearing_y,
                         advance, size.bitDepth))
            for y, row in enumerate(art(bitmap, metrics, size.bitDepth)):
                out.write("g %d %d row %d %s\n" % (glyph, number, y, row))


def main(argv):
    if len(argv) < 2:
        sys.stderr.write("usage: %s <font> [face-index] [stride]\n" % argv[0])
        return 2
    index = int(argv[2]) if len(argv) > 2 else 0
    stride = int(argv[3]) if len(argv) > 3 else 1
    report(argv[1], index, max(1, stride), sys.stdout)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
