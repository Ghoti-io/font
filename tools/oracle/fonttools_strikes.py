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


# The image formats whose data is a list of other glyphs rather than rows.
COMPOSITE_FORMATS = (8, 9)

# The image formats that carry their own metrics in front of the data. Format 5
# is the one that does not: its metrics are the index subtable's, which is the
# reason index formats 2 and 5 exist.
OWN_METRICS_FORMATS = (1, 2, 6, 7, 8, 9)


def metrics_of(glyph, subtable):
    """The glyph's own metrics, or the constant ones its subtable states.

    Decided from the **image format**, not by probing the object. fontTools'
    `BitmapGlyph.__getattr__` decompiles on a miss and consumes `self.data` doing
    it, so an attribute that is absent leaves the object unable to answer the next
    question - a `hasattr` for `componentArray` on a format 1 glyph made its
    metrics unreadable afterwards, and the failure surfaced on a *different* line
    than the probe. The format is a fact about the font and asking it costs
    nothing.
    """
    if subtable is not None and subtable.imageFormat in OWN_METRICS_FORMATS:
        return glyph.metrics
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


def is_composite(subtable):
    """Whether this subtable's glyphs are composites: image format 8 or 9."""
    return subtable is not None and subtable.imageFormat in COMPOSITE_FORMATS


def paint(data, owner, name, x, y, grid, depth, depth_cap=16):
    """OR one glyph's pixels into @p grid at (@p x, @p y), recursing for a composite.

    **This is the one piece of the differential that is not a second reading.**
    fontTools parses a composite's component list - which is a real check, and the
    check that catches image format 8's pad byte, because a reader that skipped it
    reads garbage glyph ids - but it does not compose the image, so the three lines
    of placement arithmetic below are this repository's on *both* sides of the
    comparison. Agreement here therefore says the components, their offsets, their
    own pixels and the composite's box all match, and says nothing about whether
    the placement rule itself is right.

    What says that: FreeType's `tt_sbit_decoder_load_compound()`, which places a
    component at `x_pos + dx` and `y_pos + dy` in destination pixels with y running
    down, OR-s it in, and uses none of the component's own bearings; and
    `strike-composite.ttf`, where every composite has a non-composite twin drawn
    from the generator's own arithmetic, so a transcription slip in one place shows
    as two glyphs of one strike differing.
    """
    if depth > depth_cap:
        raise SystemExit("composite nesting deeper than %d" % depth_cap)
    glyph = data.get(name)
    if glyph is None:
        raise SystemExit("a composite component the strike does not carry: %s"
                         % name)
    subtable = owner.get(name)
    metrics = metrics_of(glyph, subtable)
    if is_composite(subtable):
        for component in glyph.componentArray:
            paint(data, owner, component.name, x + component.xOffset,
                  y + component.yOffset, grid, depth + 1, depth_cap)
        return
    width, height, _, _, _ = box(metrics)
    for row, bits in enumerate(art(glyph, metrics, 1)):
        for column, cell in enumerate(bits):
            if cell == "#":
                grid[y + row][x + column] = "#"
    del width, height


def composed(data, owner, name, metrics):
    """A composite's rows, as art, in its own box."""
    width, height, _, _, _ = box(metrics)
    grid = [["."] * width for _ in range(height)]
    paint(data, owner, name, 0, 0, grid, 0)
    return ["".join(row) for row in grid]


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

    # A face of a collection that simply has no strikes, reported rather than
    # raised. wqy-zenhei.ttc carries EBLC on face 2 and not on faces 0 and 1, so
    # asking for the table raised KeyError and the differential logged two
    # "skipped" lines marked `<- not expected` for two faces about which both
    # readers in fact agree completely. A face with no strikes is a comparison
    # with the answer zero, and `strikes 0` from both sides is that comparison -
    # which is worth more than a skip, because a reader that lost EBLC support
    # would report zero here for every face and the differential's own
    # "every face compared was a fixture" guard would then be what catches it.
    #
    # The membership test is of the table directory, not a parse, so mona.ttf -
    # whose EBLC is present and unreadable - still raises below and is still an
    # expected decline rather than a face with no strikes.
    if "EBLC" not in font.reader.tables or "EBDT" not in font.reader.tables:
        out.write("faces %d\n" % len(found))
        out.write("glyphs %d\n" % glyphs)
        out.write("strikes 0\n")
        # And no scaled sizes either, which has to be *said* rather than left out:
        # this library reports zero for such a face, and an early return that
        # printed nothing made the two disagree about `scaled` on wqy-zenhei's two
        # strikeless faces. A key one side omits is a disagreement, which is the
        # right default - so the fix is to answer, not to make the key optional.
        out.write("scaled 0\n")
        return

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

    # `EBSC`: the sizes the face offers by scaling another size's bitmaps. Eleven
    # faces in Debian have one - Anonymous Pro's four and seven of Wine's.
    #
    # The substitute is resolved to a strike index **here**, from this reference's
    # own reading of the strike list, which is what makes comparing it worth
    # anything: the record states a ppem pair, and the two sides agreeing on which
    # strike that is means they agree on the pair *and* on every strike's ppem.
    scales = (font["EBSC"].bitmapScaleTables
              if "EBSC" in font.reader.tables else [])
    out.write("scaled %d\n" % len(scales))
    for number, scale in enumerate(scales):
        where = len(eblc.strikes)
        for at, strike in enumerate(eblc.strikes):
            size = strike.bitmapSizeTable
            if (size.ppemX == scale.substitutePpemX
                    and size.ppemY == scale.substitutePpemY):
                where = at
                break
        out.write("scale %d ppem_x %d\n" % (number, scale.ppemX))
        out.write("scale %d ppem_y %d\n" % (number, scale.ppemY))
        out.write("scale %d sub_ppem_x %d\n" % (number, scale.substitutePpemX))
        out.write("scale %d sub_ppem_y %d\n" % (number, scale.substitutePpemY))
        out.write("scale %d sub_index %d\n" % (number, where))
        # A BitmapScale states no `flags`, so which of its two sbitLineMetrics
        # applies comes from the substitute strike - the only statement of the
        # direction anywhere near these bytes. Both sides apply that, because it is
        # the only reading the format allows rather than a choice either made.
        line = scale.hori
        if where < len(eblc.strikes):
            size = eblc.strikes[where].bitmapSizeTable
            line = scale.hori if size.flags & 1 else scale.vert
        out.write("scale %d ascent %d\n" % (number, line.ascender))
        # The raw bytes, for the same reason the strike's descent is a census line:
        # see `resolved_descent()` in eblc_diff.py.
        out.write("census scaledescent %d stated %d min_after_bl %d\n"
                  % (number, line.descender, line.minAfterBL))

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
        # Each census line's first word is its own, because the differential
        # parses them by keyword: a second kind of line that also began "strike"
        # was read as this one and reported a strike with `flags 4` and a bit
        # depth of 6, which are the composite counts wearing these names. The
        # numbers were nonsense and the table they printed still looked like a
        # table.
        out.write("census sizetable %d flags %d depth %d subtables %d\n"
                  % (number, size.flags, size.bitDepth,
                     len(strike.indexSubTables)))
        pairs = {}
        for subtable in strike.indexSubTables:
            key = (subtable.indexFormat, subtable.imageFormat)
            pairs[key] = pairs.get(key, 0) + 1
        for (index_format, image_format), count in sorted(pairs.items()):
            out.write("census pair %d %d count %d\n"
                      % (index_format, image_format, count))
        # How many composites, and how many components between them. Printed
        # rather than compared, like every census line: what it answers is "did
        # this run visit a composite at all", which for a format with no
        # population is the question the total cannot answer.
        strike_data = ebdt.strikeData[number]
        where = {}
        for subtable in strike.indexSubTables:
            for member in subtable.names:
                where[member] = subtable
        composites = [glyph_name for glyph_name in strike_data
                      if is_composite(where.get(glyph_name))]
        parts = sum(len(strike_data[glyph_name].componentArray)
                    for glyph_name in composites)
        nested = sum(1 for glyph_name in composites
                     if any(is_composite(where.get(component.name))
                            for component
                            in strike_data[glyph_name].componentArray))
        composites = len(composites)
        out.write("census composite %d count %d components %d nested %d\n"
                  % (number, composites, parts, nested))

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
        # **The raw bytes, as a census line, not a compared `descent`.**
        #
        # `descender` has no stated sign in the specification and this population
        # writes it both ways: 30 faces negative, Anonymous Pro's four positive
        # with a `minAfterBL` of -2, two zero. This library resolves that against
        # `minAfterBL` and reports a descent that is negative where the ink is
        # below the baseline; fontTools reports the byte.
        #
        # So the two cannot be compared directly, and the wrong fix would be to
        # apply the same rule here - an oracle that agrees by construction checks
        # nothing. What is emitted instead is the two bytes the rule reads, and
        # `eblc_diff.py` derives the expected descent from *these* and compares
        # that. The rule is then checked against values this reference read on its
        # own, which is the whole point of having one.
        out.write("census descent %d stated %d min_after_bl %d\n"
                  % (number, line.descender, line.minAfterBL))
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
            rows = (composed(data, owner, name, metrics)
                    if is_composite(owner.get(name))
                    else art(bitmap, metrics, size.bitDepth))
            for y, row in enumerate(rows):
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
