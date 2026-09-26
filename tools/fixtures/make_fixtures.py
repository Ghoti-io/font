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
"""Build the synthetic fixtures under `tests/data/fonts/`, byte-reproducibly.

documentation/design.md section 14.5: unit tests want fixtures that exercise a
code path, conformance wants the population that is not ours. This tool makes
the first kind. The second kind lives in the oracle image and is never copied
here, so nothing under `tests/data/fonts/` carries a third-party licence - every
fixture is drawn from outlines in this file, and says so in its own `name` table.

**It runs in the pinned `fonttools` image** (`tools/oracle/containers/`), and
that is what makes `check-fixtures` mean anything: a fixture's bytes are
whatever fontTools 4.66.0 wrote, so comparing committed bytes against a
regeneration is a real check on this library's inputs rather than on the host's
Python. `tools/fixtures/check_fixtures.py` is the driver; nothing here reaches
for a container itself.

Three reasons a generated fixture beats a byte array in a test:

  1. **It can be inspected.** `ttx tests/data/fonts/basic.ttf` prints what the
     fixture is. A test that builds its own bytes can only be read by reading
     the builder.
  2. **It is not this library's opinion.** Every expectation in `tests/` is one
     this library wrote for itself; `tests/sfnt_builder.h` says so in its own
     header. A fixture fontTools wrote is the one input to the unit suite that
     did not come from here.
  3. **It is the same bytes on every machine**, so a coverage hash or a golden
     bitmap over it (phase 1) means something.

`tests/sfnt_builder.h` keeps its job: the refusal arms - a checksum that does
not match, a directory entry past the blob, a length that ends mid-entry - are
bytes fontTools will not emit, and they stay hand-built.

Determinism, which is the whole contract here
---------------------------------------------

Everything that would otherwise vary with the clock, the filesystem or the
interpreter is pinned:

  * **`head.created` and `head.modified` are set to 0** (1904-01-01, visibly
    synthetic). `FontBuilder.__init__` sets both to `timestampNow()` - the
    defaults table in `fontBuilder.py` says 0 and is then overridden - so a
    generator that does not pin them writes different bytes every run. It sets
    `recalcTimestamp=False` on the font, so `save()` leaves them alone once set.
  * **No wall clock, no random, no host paths** reach a table. The only inputs
    are the literals below.
  * **The locale is the image's**, pinned to `C.UTF-8` in the Containerfile: an
    unset `LANG` produced fifty false disagreements in another library's
    oracle, and a Mac Roman `name` record is encoded through exactly that path.

Fixtures are written only where `--out` says, so this tool never writes into a
repository that a container has mounted read-only.
"""

import argparse
import os
import struct
import sys

from fontTools.fontBuilder import FontBuilder
from fontTools.pens.t2CharStringPen import T2CharStringPen
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib import TTCollection, TTFont, newTable
from fontTools.ttLib.tables._n_a_m_e import NameRecord
from fontTools.ttLib.tables._p_o_s_t import standardGlyphOrder
from fontTools.ttLib.tables._c_m_a_p import CmapSubtable
from fontTools.ttLib.tables._g_l_y_f import (Glyph, GlyphComponent,
    GlyphCoordinates, flagCubic, flagOnCurve)
from fontTools.ttLib.tables import ttProgram

UPEM = 1000

# head.created and head.modified: 2026-01-01T00:00:00Z, counted from the sfnt
# epoch of 1904-01-01 as the `head` table requires.
#
# **Not 0**, which was the first choice and was wrong in a way worth writing
# down. fontTools' `head` decompile treats any timestamp below 0x7C259DC0
# (1970-01-01, in this epoch) as a misencoded unix timestamp and silently adds
# that constant to it. A fixture written with 0 therefore reads back as 1970
# rather than 1904 - so `ttx_diff` would have reported `created` as a
# disagreement on every fixture, in a direction that is fontTools being helpful
# and this library being right. Any value at or above the threshold round-trips.
EPOCH = 3850070400

ASCENT = 800
DESCENT = -200

# The glyph repertoire. Five glyphs is enough to have a .notdef that is not
# empty, a blank glyph, two mapped glyphs, and one glyph whose name is *not* in
# the standard Macintosh order - which is the only way a `post` format 2.0
# fixture exercises both the index path and the extraNames path.
GLYPH_ORDER = [".notdef", "space", "A", "B", "ghoti.alt"]

ADVANCES = {".notdef": 600, "space": 300, "A": 640, "B": 620, "ghoti.alt": 700}

VENDOR = "GHTI"

# The last index into the standard Macintosh glyph order, which is the boundary
# `post` format 2.0 turns on: this index means the last standard name and the
# next one means the first stored string. Spelled from the vector's own length so
# it cannot drift from it.
GFNT_POST_LAST_STANDARD = len(standardGlyphOrder) - 1

# Every `OS/2` field this library reads, and every one of them distinctive.
#
# The same values go into all six version fixtures, which is the point. The
# version selects which fields the file *has*, so a version 0 fixture asked for
# a code page range and an x-height does not get them, and a reader of it must
# report zero. If these were left at fontTools' defaults - which are zero -
# "the field was read" and "the field was absent, so zero" would look identical
# and the six fixtures would prove nothing about the version gate.
#
# They are distinct from each other as well as from zero, because a parser that
# transposes two adjacent fields of the same width is the other defect here and
# two fields holding the same number cannot catch it.
OS2_FIELDS = dict(
    sTypoAscender=ASCENT,
    sTypoDescender=DESCENT,
    sTypoLineGap=90,
    usWinAscent=ASCENT,
    usWinDescent=-DESCENT,
    # Version 1 and later.
    ulCodePageRange1=0x00000651,
    ulCodePageRange2=0x10000001,
    # Version 2 and later.
    sxHeight=520,
    sCapHeight=720,
    usDefaultChar=0x003F,
    usBreakChar=0x0020,
    usMaxContext=3,
    # Version 5 only, and in *points*: fontTools multiplies by 20 on the way in
    # because the file holds twentieths of a point. So the fixture contains 160
    # and 1440, which is arithmetic a reader has to get right and which no font
    # in the oracle corpus exercises - it holds no version 5 at all.
    usLowerOpticalPointSize=8,
    usUpperOpticalPointSize=72,
)


def draw_notdef(pen):
    """A hollow box: two contours, so a fixture has a multi-contour glyph."""
    pen.moveTo((60, 0))
    pen.lineTo((540, 0))
    pen.lineTo((540, 700))
    pen.lineTo((60, 700))
    pen.closePath()
    pen.moveTo((140, 80))
    pen.lineTo((140, 620))
    pen.lineTo((460, 620))
    pen.lineTo((460, 80))
    pen.closePath()


def draw_a(pen):
    """A triangle: one contour, three points, all on-curve."""
    pen.moveTo((40, 0))
    pen.lineTo((600, 0))
    pen.lineTo((320, 720))
    pen.closePath()


def draw_b(pen):
    """A box with a bar, so the glyph's bounding box is not its only contour."""
    pen.moveTo((80, 0))
    pen.lineTo((540, 0))
    pen.lineTo((540, 720))
    pen.lineTo((80, 720))
    pen.closePath()
    pen.moveTo((200, 300))
    pen.lineTo((200, 420))
    pen.lineTo((420, 420))
    pen.lineTo((420, 300))
    pen.closePath()


def draw_alt(pen):
    """A chevron, for the glyph whose name is not in the standard order."""
    pen.moveTo((60, 0))
    pen.lineTo((340, 360))
    pen.lineTo((620, 0))
    pen.lineTo((620, 200))
    pen.lineTo((340, 560))
    pen.lineTo((60, 200))
    pen.closePath()


def draw_space(pen):
    """Nothing. A blank glyph is a real shape in a `glyf` table: zero bytes."""


OUTLINES = {
    ".notdef": draw_notdef,
    "space": draw_space,
    "A": draw_a,
    "B": draw_b,
    "ghoti.alt": draw_alt,
}


def tt_glyphs():
    """The outlines as TrueType glyphs, in glyph order."""
    out = {}
    for name in GLYPH_ORDER:
        pen = TTGlyphPen(None)
        OUTLINES[name](pen)
        out[name] = pen.glyph()
    return out


def cff_charstrings():
    """The same outlines as Type 2 charstrings, for the `OTTO` fixture."""
    out = {}
    for name in GLYPH_ORDER:
        pen = T2CharStringPen(ADVANCES[name], None)
        OUTLINES[name](pen)
        out[name] = pen.getCharString()
    return out


def names(label):
    """The `name` table strings, which say whose fixture this is.

    A fixture that escapes into someone's font directory should be able to
    answer for itself, and the licence question this repository does not have
    (section 14.5) is one the fixture states rather than one a reader has to
    infer from its absence.
    """
    return {
        "copyright": "Copyright 2026 Corey Pennycuff. LGPL-3.0-only.",
        "familyName": "Ghoti Fixture %s" % label,
        "styleName": "Regular",
        "uniqueFontIdentifier": "Ghoti.io Font fixture: %s" % label,
        "fullName": "Ghoti Fixture %s Regular" % label,
        "version": "Version 1.000",
        "psName": "GhotiFixture%s-Regular" % label.replace(" ", ""),
        "manufacturer": "Ghoti.io",
        "description": "Synthetic fixture for Ghoti.io Font. Not a typeface.",
        "licenseDescription": "Licensed under the LGPL version 3.",
    }


def subtable(fmt, platform, encoding, mapping, language=0):
    """One `cmap` subtable, spelled explicitly.

    `FontBuilder.setupCharacterMap` decides the formats for you, which is the
    wrong tool here: the point of these fixtures is that a named format is
    present, so the format is named.
    """
    table = CmapSubtable.newSubtable(fmt)
    table.platformID = platform
    table.platEncID = encoding
    table.language = language
    table.cmap = dict(mapping)
    return table


def set_cmap(fb, subtables):
    """Install a `cmap` built from explicit subtables."""
    table = newTable("cmap")
    table.tableVersion = 0
    table.tables = list(subtables)
    fb.font["cmap"] = table


def pin(fb):
    """Take the clock out of the font.

    Called for every fixture, immediately before saving, because
    `FontBuilder.__init__` has already written `timestampNow()` into `head` by
    the time any of this runs.
    """
    _pin_font(fb.font)


def _pin_font(font):
    """The same, for a TTFont that is not behind a FontBuilder."""
    head = font["head"]
    head.created = EPOCH
    head.modified = EPOCH
    # A TTFont opened from a file has recalcTimestamp *True*, unlike one
    # FontBuilder made, so `save()` would write the current time into
    # `modified` and no two generations would agree. That is how
    # collection.ttc - the one fixture that is re-read before it is written -
    # came out different on every run while the other fifteen were stable.
    font.recalcTimestamp = False


def truetype(label, subtables, *, os2_version=4, glyph_names=False,
             mac_names=True, extra_names=None, order=None, glyphs=None,
             advances=None, padding=None, glyph_data_format=0):
    """A TrueType fixture: `glyf`, and every table this library parses.

    The table order here is the order `FontBuilder` requires and not a
    preference: `setupOS2` asserts that `hmtx` and `cmap` are already present,
    because it recalculates `xAvgCharWidth` from one and the Unicode ranges and
    first/last character index from the other.

    `order`, `glyphs` and `advances` default to the five-glyph repertoire every
    phase 0 fixture shares; the outline fixtures pass their own, because what
    they exercise is a *shape* and a shape needs its own glyphs.
    """
    order = list(order if order is not None else GLYPH_ORDER)
    glyphs = glyphs if glyphs is not None else tt_glyphs()
    advances = advances if advances is not None else ADVANCES
    fb = FontBuilder(UPEM, isTTF=True, glyphDataFormat=glyph_data_format)
    fb.setupGlyphOrder(order)
    fb.setupGlyf(glyphs)
    if padding is not None:
        # `loca`'s format follows from whether every glyph offset is even, and
        # the padding is the only switch that decides it: see
        # build_outline_loca_long.
        fb.font["glyf"].padding = padding
    fb.setupHorizontalMetrics({n: (advances.get(n, 600), 0) for n in order})
    fb.setupHorizontalHeader(ascent=ASCENT, descent=DESCENT, lineGap=0)
    set_cmap(fb, subtables)
    fb.setupOS2(version=os2_version, achVendID=VENDOR, **OS2_FIELDS)
    fb.setupNameTable(names(label), mac=mac_names)
    fb.setupPost(keepGlyphNames=glyph_names)
    if glyph_names and extra_names is not None:
        fb.font["post"].extraNames = list(extra_names)
    pin(fb)
    return fb


def unicode_cmap():
    """The ordinary case: a Windows BMP format 4, and its Unicode twin."""
    mapping = {0x20: "space", 0x41: "A", 0x42: "B"}
    return [
        subtable(4, 0, 3, mapping),
        subtable(4, 3, 1, mapping),
    ]


def build_basic(out):
    """Everything ordinary, and the baseline every other fixture varies from."""
    fb = truetype("Basic", unicode_cmap())
    fb.save(out)


def build_os2(out, version):
    """One `OS/2` version.

    Six of these exist because the 327-font oracle corpus holds versions 1, 3
    and 4 only - no 0, no 2, no 5 - and a planted defect that read version 2's
    fields out of a version 1 table passed a four-font sample of it
    (design.md section 14.7). The version field selects which fields exist, so
    an unexercised version is an unexercised branch.
    """
    fb = truetype("OS2 v%d" % version, unicode_cmap(), os2_version=version)
    fb.save(out)


def build_cmap_format0(out):
    """A Macintosh byte-encoding table: the whole 256-byte array."""
    mapping = {0x20: "space", 0x41: "A", 0x42: "B"}
    fb = truetype("Cmap Format 0", [subtable(0, 1, 0, mapping)])
    fb.save(out)


def build_cmap_format6(out):
    """A trimmed array: firstCode and a run, with nothing outside it."""
    mapping = {0x41: "A", 0x42: "B", 0x43: "ghoti.alt"}
    fb = truetype("Cmap Format 6", [subtable(6, 1, 0, mapping)])
    fb.save(out)


def build_cmap_format12(out):
    """Groups, and codepoints outside the BMP that a format 4 cannot hold."""
    bmp = {0x20: "space", 0x41: "A", 0x42: "B"}
    wide = dict(bmp)
    wide.update({0x10041: "A", 0x10042: "B", 0x1F600: "ghoti.alt"})
    return_value = truetype("Cmap Format 12", [
        subtable(4, 3, 1, bmp),
        subtable(12, 3, 10, wide),
    ])
    return_value.save(out)


def build_cmap_symbol(out):
    """A (3,0) symbol table and nothing else.

    The only subtable maps 0xF041, so a lookup of U+0041 succeeds only through
    the 0xF0xx rule in section 7.2. A font shaped like this is why that rule is
    in the library, and no font in the oracle corpus is shaped like this.
    """
    mapping = {0xF020: "space", 0xF041: "A", 0xF042: "B"}
    fb = truetype("Cmap Symbol", [subtable(4, 3, 0, mapping)])
    fb.save(out)


def build_post_v2(out):
    """`post` format 2.0, across the boundary the format turns on.

    "A", "B" and "space" are in the standard Macintosh order and are spelled as
    indices into it; "ghoti.alt" is not, and is spelled as a Pascal string in the
    table's own list. Reading this table needs the 258-entry standard vector that
    section 7.2 deferred to exactly this generator.

    **"dcroat" is here because it is standard name 257, the last one.** The first
    version of this fixture used indices 0-3 and 258, which leaves the boundary
    itself untested: a planted off-by-one that treated index 257 as a stored name
    rather than the last standard one passed a two-font smoke run with zero
    disagreements. 38 of the corpus's 291 format 2.0 fonts do use index 257, so
    the full run catches it - but the fixtures are what travel with a thinned
    one, and a boundary is exactly the thing a fixture should hold.
    """
    boundary = standardGlyphOrder[GFNT_POST_LAST_STANDARD]
    order = GLYPH_ORDER + [boundary]
    glyphs = tt_glyphs()
    pen = TTGlyphPen(None)
    draw_a(pen)
    glyphs[boundary] = pen.glyph()
    metrics = {n: (ADVANCES[n], 0) for n in GLYPH_ORDER}
    metrics[boundary] = (580, 0)

    fb = FontBuilder(UPEM, isTTF=True)
    fb.setupGlyphOrder(order)
    fb.setupGlyf(glyphs)
    fb.setupHorizontalMetrics(metrics)
    fb.setupHorizontalHeader(ascent=ASCENT, descent=DESCENT, lineGap=0)
    set_cmap(fb, [subtable(4, 0, 3, {0x20: "space", 0x41: "A", 0x42: "B"}),
                  subtable(4, 3, 1, {0x20: "space", 0x41: "A", 0x42: "B",
                                     0x111: boundary})])
    fb.setupOS2(version=4, achVendID=VENDOR, **OS2_FIELDS)
    fb.setupNameTable(names("Post v2"), mac=True)
    fb.setupPost(keepGlyphNames=True)
    pin(fb)
    fb.save(out)


def build_post_v1(out):
    """`post` format 1.0: the font *is* the standard Macintosh glyph order.

    All 258 glyphs, in that order, because that is what the format means - a
    format 1.0 table stores no names at all and a reader answers from the
    generated vector alone. Most of the glyphs are empty, which keeps the
    fixture at a few kilobytes; the three that are also in the ordinary
    repertoire are drawn.

    No font in the oracle corpus is format 1.0 - it holds 291 format 2.0 and 51
    format 3.0 and nothing else - so this fixture is the only cover that path
    has, the same position `os2-v0`, `os2-v2` and `os2-v5` are in.
    """
    order = list(standardGlyphOrder)
    if len(order) != 258:
        raise SystemExit("the standard glyph order is not 258 names")

    glyphs = {}
    metrics = {}
    for name in order:
        pen = TTGlyphPen(None)
        OUTLINES.get(name, draw_space)(pen)
        glyphs[name] = pen.glyph()
        metrics[name] = (ADVANCES.get(name, 500), 0)

    fb = FontBuilder(UPEM, isTTF=True)
    fb.setupGlyphOrder(order)
    fb.setupGlyf(glyphs)
    fb.setupHorizontalMetrics(metrics)
    fb.setupHorizontalHeader(ascent=ASCENT, descent=DESCENT, lineGap=0)
    set_cmap(fb, [subtable(4, 3, 1, {0x20: "space", 0x41: "A", 0x42: "B"})])
    fb.setupOS2(version=4, achVendID=VENDOR, **OS2_FIELDS)
    fb.setupNameTable(names("Post v1"), mac=True)
    fb.setupPost(keepGlyphNames=False)
    # fontTools writes a format 1.0 table as the bare header, which is what the
    # format is; it is not offered by setupPost because a font is rarely allowed
    # to claim it.
    fb.font["post"].formatType = 1.0
    pin(fb)
    fb.save(out)


# The Macintosh (platEncID, langID) pairs this fixture exercises, and what each
# one must decode as. Taken from fontTools' own encoding map - see
# tools/vectors/make_vectors.py, which generates the library's table from the
# same place.
#
# The point of the pairs under platEncID 0 is that the encoding ID does not
# decide: 0 with langID 17 is Turkish, not Roman, and a reader that ignores the
# language decodes real letters to the wrong real letters without failing.
MAC_ENCODING_CASES = [
    (0, 0, "mac_roman"),
    (0, 15, "mac_iceland"),
    (0, 17, "mac_turkish"),
    (0, 18, "mac_croatian"),
    (0, 24, "mac_latin2"),
    (0, 37, "mac_romanian"),
    (6, 0, "mac_greek"),
    (7, 0, "mac_cyrillic"),
    (29, 0, "mac_latin2"),
    (35, 0, "mac_turkish"),
    (37, 0, "mac_iceland"),
]


def build_name_mac_encodings(out):
    """One Macintosh record per single-byte encoding, each holding every byte.

    The record's text is **all of 0x80 to 0xFF** decoded through that encoding,
    so two tables that differ anywhere differ here, and a reader that chose the
    wrong one cannot produce the right string by accident. fontTools encodes the
    text back to bytes with the same codec, so the fixture's bytes are the
    reference's opinion of the encoding rather than this repository's.

    Why it has to exist: all 658 Macintosh name records across the 327-font
    corpus are (platEncID 0, Mac Roman). The language-keyed rules and the seven
    non-Roman tables have no coverage from real fonts at all, and a table with
    no coverage is a table nobody has read.
    """
    fb = FontBuilder(UPEM, isTTF=True)
    fb.setupGlyphOrder(GLYPH_ORDER)
    fb.setupGlyf(tt_glyphs())
    fb.setupHorizontalMetrics({n: (ADVANCES[n], 0) for n in GLYPH_ORDER})
    fb.setupHorizontalHeader(ascent=ASCENT, descent=DESCENT, lineGap=0)
    set_cmap(fb, unicode_cmap())
    fb.setupOS2(version=4, achVendID=VENDOR, **OS2_FIELDS)
    fb.setupNameTable(names("Name Mac Encodings"), mac=True)
    fb.setupPost(keepGlyphNames=False)

    table = fb.font["name"]
    high = bytes(range(0x80, 0x100))
    for encoding, language, codec in MAC_ENCODING_CASES:
        record = NameRecord()
        record.platformID = 1
        record.platEncID = encoding
        record.langID = language
        # Sample text (19), so that one nameID can carry every case: records are
        # keyed by (platform, encoding, language, nameID) and these differ in the
        # first three.
        record.nameID = 19
        record.string = high.decode(codec)
        table.names.append(record)
    table.names.sort(key=lambda r: (r.platformID, r.platEncID, r.langID,
                                    r.nameID))
    pin(fb)
    fb.save(out)


def build_post_v3(out):
    """`post` format 3.0: the header, and the statement that there are no names."""
    fb = truetype("Post v3", unicode_cmap(), glyph_names=False)
    fb.save(out)


def build_name_macroman(out):
    """A Macintosh Roman `name` record with a byte above ASCII.

    U+2122 encodes to 0xAA in Mac Roman, and the twelve records that the
    `ttx_diff` oracle counts across 327 real fonts are that same character in
    that same encoding (design.md section 18.1). This is the fixture the Mac
    Roman deferral in section 7.2 is waiting for, and the reason it is here
    rather than in `sfnt_builder.h` is that the encoding must come from
    fontTools rather than from a byte this library chose.
    """
    strings = names("Name MacRoman")
    strings["trademark"] = "Ghoti™ is not a trademark"
    strings["description"] = "Fixture with a Mac Roman ™ above ASCII."
    fb = FontBuilder(UPEM, isTTF=True)
    fb.setupGlyphOrder(GLYPH_ORDER)
    fb.setupGlyf(tt_glyphs())
    fb.setupHorizontalMetrics({n: (ADVANCES[n], 0) for n in GLYPH_ORDER})
    fb.setupHorizontalHeader(ascent=ASCENT, descent=DESCENT, lineGap=0)
    set_cmap(fb, unicode_cmap())
    fb.setupOS2(version=4, achVendID=VENDOR, **OS2_FIELDS)
    fb.setupNameTable(strings, mac=True)
    fb.setupPost(keepGlyphNames=False)
    pin(fb)
    fb.save(out)


def build_cff(out):
    """An `OTTO` face: a `CFF ` table where the `glyf` would be.

    Phase 2 parses the charstrings. Phase 0 reads the flavour, the directory and
    every metric table, and needs a fixture where those are the same questions
    over a different container.
    """
    fb = FontBuilder(UPEM, isTTF=False)
    fb.setupGlyphOrder(GLYPH_ORDER)
    fb.setupCFF(
        names("CFF")["psName"],
        {"FullName": names("CFF")["fullName"], "Weight": "Regular"},
        cff_charstrings(),
        {})
    fb.setupHorizontalMetrics({n: (ADVANCES[n], 0) for n in GLYPH_ORDER})
    fb.setupHorizontalHeader(ascent=ASCENT, descent=DESCENT, lineGap=0)
    set_cmap(fb, unicode_cmap())
    fb.setupOS2(version=4, achVendID=VENDOR, **OS2_FIELDS)
    fb.setupNameTable(names("CFF"), mac=True)
    fb.setupPost(keepGlyphNames=False)
    pin(fb)
    fb.save(out)


def build_collection(out):
    """A `ttcf` holding two faces that share their tables.

    Shared tables are the point: the second face's directory entries point into
    the first face's bytes, so a reader that treats an offset as being relative
    to its own face reads garbage. `shareTables=True` is what makes the sharing
    real rather than two fonts concatenated.
    """
    faces = []
    for index, version in enumerate((4, 1)):
        fb = truetype("Collection %d" % index, unicode_cmap(),
            os2_version=version)
        scratch = out + ".face%d" % index
        fb.save(scratch)
        face = TTFont(scratch)
        _pin_font(face)
        faces.append(face)
    collection = TTCollection()
    collection.fonts = faces
    collection.save(out, shareTables=True)
    for face in faces:
        face.close()
    for index in range(len(faces)):
        os.remove(out + ".face%d" % index)


# The fixture set: filename -> (builder, what it is for).
#
# The description is not decoration. It is written into MANIFEST beside the
# filename, so `tests/data/fonts/` says what each of its files exercises and
# cannot drift from this table - the two are one edit.

# ---------------------------------------------------------------------------
# The outline fixtures (phase 1).
#
# Everything above varies a *table*; these vary a **shape**, and they are drawn
# by setting `glyf`'s points and flags directly rather than through a pen. That
# is deliberate: a pen normalises. `TTGlyphPen` always starts a contour at an
# on-curve point, so a font whose contour is *stored* beginning off-curve - which
# the format allows and real fonts contain - cannot be written with one, and the
# reader's rule for that case would have no fixture at all.
#
# The four constructions below are the whole of what `glyf` leaves to the reader
# (design.md section 7.3), and two of them are written twice on purpose:
# `quad-implied` and `off-curve-start` hold the *same curve* with the points
# rotated, so a reader that gets either rule wrong makes them disagree, and the
# test asserts they decompose identically.

# 1.0 in F2Dot14 is 16384, so a scale is exact only if it is a multiple of
# 1/16384. Every scale here is a power-of-two fraction, which keeps fontTools'
# float arithmetic and this library's fixed-point arithmetic exactly equal and
# so keeps a differential's zero meaning "the rule agrees" rather than "the
# rounding happened to land the same way".
HALF = 0.5
QUARTER = 0.25

# Component flag bits fontTools preserves rather than recomputing.
FLAG_ROUND_XY_TO_GRID = 0x0004
FLAG_USE_MY_METRICS = 0x0200
FLAG_OVERLAP_COMPOUND = 0x0400
FLAG_SCALED_COMPONENT_OFFSET = 0x0800
FLAG_UNSCALED_COMPONENT_OFFSET = 0x1000


def raw_glyph(contours, *, cubic=False, program=b""):
    """A simple glyph from explicit points, flags and contour ends.

    `contours` is a list of lists of `(x, y, on_curve)`. Nothing here is
    derived: the point order and the on-curve flags are the fixture.
    """
    glyph = Glyph()
    glyph.numberOfContours = len(contours)
    coordinates = []
    flags = bytearray()
    ends = []
    for contour in contours:
        for x, y, on_curve in contour:
            coordinates.append((x, y))
            if on_curve:
                flags.append(flagOnCurve)
            else:
                flags.append(flagCubic if cubic else 0)
        ends.append(len(coordinates) - 1)
    glyph.coordinates = GlyphCoordinates(coordinates)
    glyph.flags = flags
    glyph.endPtsOfContours = ends
    glyph.program = ttProgram.Program()
    glyph.program.fromBytecode(program)
    return glyph


def component(name, *, x=None, y=None, first=None, second=None, transform=None,
              flags=0):
    """One component of a composite.

    fontTools decides `ARG_1_AND_2_ARE_WORDS`, `ARGS_ARE_XY_VALUES` and which
    of the three transform encodings to write from what is set here, and
    preserves only the flag bits it cannot derive - which is why `flags` carries
    just those. So an offset of 100 is written as a byte and one of -200 as a
    word without either being asked for, and both encodings appear below.
    """
    part = GlyphComponent()
    part.glyphName = name
    if first is None:
        part.x = x
        part.y = y
    else:
        part.firstPt = first
        part.secondPt = second
    if transform is not None:
        part.transform = transform
    part.flags = flags
    return part


def composite_glyph(components, *, program=b""):
    """A composite glyph from its components."""
    glyph = Glyph()
    glyph.numberOfContours = -1
    glyph.components = list(components)
    glyph.program = ttProgram.Program()
    glyph.program.fromBytecode(program)
    return glyph


# A leaf, drawn four ways. The first is the ordinary case and the other three
# are the constructions a pen cannot write.
LEAF_EXPLICIT = [[(100, 0, True), (100, 400, False), (400, 400, True),
                  (400, 0, False)]]
# Two off-curve points in a row: the on-curve point at (300, 500) is implicit
# and the reader has to synthesise it.
LEAF_IMPLIED = [[(100, 0, True), (200, 500, False), (400, 500, False),
                 (500, 0, True)]]
# Stored starting off-curve, with exactly one on-curve point and it last: the
# path begins at that point and every other point is walked in order. One
# on-curve point is the whole of the case, so this glyph has one.
LEAF_OFF_START = [[(100, 400, False), (400, 400, False), (400, 0, False),
                   (100, 0, True)]]
# The same curve as LEAF_IMPLIED, stored from a different point of the cycle:
# the first stored point is off-curve and there are *two* on-curve points, so a
# reader has to pick where to begin. The two defensible answers differ - this
# library and FreeType begin at the contour's last point, fontTools rotates to
# the first on-curve point and draws the closing line explicitly - and both
# describe the same closed contour. That is why the path differential compares
# contours as cycles: a closed contour's starting vertex is not part of what it
# means, and this glyph is the fixture that says so.
LEAF_OFF_START_ROTATED = [[(200, 500, False), (400, 500, False),
                           (500, 0, True), (100, 0, True)]]
# Nothing on-curve at all: the path begins at the midpoint between the last
# point and the first, and every segment's endpoint is implicit.
LEAF_ALL_OFF = [[(100, 0, False), (100, 400, False), (400, 400, False),
                 (400, 0, False)]]

# An arch, which is here for one reason: its control point is outside the curve.
# `glyf`'s xMin/yMax are the *coordinate* box by definition - the specification
# says "minimum x for coordinate data" and fontTools writes exactly that - so a
# glyph whose control box and whose curve differ is what tells the two apart. In
# every other fixture they coincide, which made a test comparing the stated box
# against the drawn curve pass while asserting the wrong thing.
ARCH = [[(0, 0, True), (50, 100, False), (100, 0, True)]]

OUTLINE_CONTOURS = [
    [(80, 0, True), (520, 0, True), (520, 700, True), (80, 700, True)],
    [(160, 80, True), (160, 620, True), (440, 620, True), (440, 80, True)],
]

# A 200-point contour, which exists for one reason: a component's point index
# is one byte when it fits in one, and **unsigned** - unlike a one-byte offset,
# which is signed. The two readings of the same two bytes agree for every index
# below 128, so a composite that matches point 2 cannot tell them apart. This
# glyph is long enough to be matched at index 150, where they differ, and a
# sign-extended read turns a valid font into a refusal.
#
# It is a zigzag rather than a circle because a circle needs trigonometry and
# this generator has none: every coordinate in every fixture is integer
# arithmetic, so that the bytes do not depend on a libm.
MANY_POINTS = [[(index * 3, 100 if index % 2 else 0, True)
                for index in range(200)]]

# A bar whose horizontal edges are walked in two-unit steps, here for the
# `REPEAT` flag and nothing else. A `glyf` flag stream may write one flag byte
# and a count instead of writing the byte again, and a writer does that only
# where consecutive points share a flag - the same on-curve bit, deltas of the
# same shortness and the same sign. Every other glyph above alternates:
# `many-points` is a zigzag, so its y delta changes sign at every point, and a
# leaf changes direction at every corner. So no fixture carried a repeat record
# at all, and the arm that expands one was reached only through the oracle's real
# fonts - where every font has them - which left `make test` unable to see a
# defect in it on a machine with no container.
#
# Each edge is 259 repeats of one flag, which is deliberately more than 255. The
# count is a single byte, so one run has to be written as two records, and a
# reader that expands the first and forgets to come back for the second reads the
# rest of the stream as coordinates.
REPEATED_FLAGS = [
    [(x, 0, True) for x in range(0, 520, 2)]
    + [(x, 100, True) for x in range(518, -2, -2)]
]

OUTLINE_GLYPHS = {
    ".notdef": raw_glyph(OUTLINE_CONTOURS),
    "space": raw_glyph([]),
    "quad-explicit": raw_glyph(LEAF_EXPLICIT),
    "quad-implied": raw_glyph(LEAF_IMPLIED),
    "off-curve-start": raw_glyph(LEAF_OFF_START),
    "off-curve-start-rotated": raw_glyph(LEAF_OFF_START_ROTATED),
    "all-off-curve": raw_glyph(LEAF_ALL_OFF),
    "two-contours": raw_glyph(OUTLINE_CONTOURS),
    # One point, which is a contour the format allows and which draws nothing.
    # It is here because "a contour with no segments" is the shortest path
    # through the walk and the one a length calculation gets wrong.
    "single-point": raw_glyph([[(300, 300, True)]]),
    # Instructions, which are skipped by their stated length and never run
    # (design.md section 8.5). Two bytes of them, so that a reader which
    # forgets to skip lands inside the flag stream rather than on its end.
    "with-instructions": raw_glyph(LEAF_EXPLICIT, program=b"\x00\x01"),
    "many-points": raw_glyph(MANY_POINTS),
    "arch": raw_glyph(ARCH),
    "repeated-flags": raw_glyph(REPEATED_FLAGS),
}

OUTLINE_ORDER = list(OUTLINE_GLYPHS)

COMPOSITE_GLYPHS = dict(OUTLINE_GLYPHS)
COMPOSITE_GLYPHS.update({
    # A byte offset, which is every accent in every font.
    "comp-offset": composite_glyph([
        component("quad-explicit", x=100, y=50)]),
    # -200 does not fit a signed byte, so fontTools writes words. The two
    # encodings of the same field are the pair that catches a reader which
    # reads one size for both.
    "comp-word-offset": composite_glyph([
        component("quad-explicit", x=300, y=-200)]),
    # A single scale, applied before the offset: the Microsoft reading, which
    # is the default when neither offset flag is set.
    "comp-scale": composite_glyph([
        component("quad-explicit", x=100, y=100,
                  transform=[[HALF, 0], [0, HALF]])]),
    # The *same* component and the *same* offset with SCALED_COMPONENT_OFFSET
    # set, which places it somewhere else. This is a minimal pair: the two
    # glyphs differ in one flag bit, so a reader that ignores the bit puts them
    # in the same place and the test that compares them fails.
    "comp-scaled-offset": composite_glyph([
        component("quad-explicit", x=100, y=100,
                  transform=[[HALF, 0], [0, HALF]],
                  flags=FLAG_SCALED_COMPONENT_OFFSET)]),
    # And the flag that spells the default out loud, which must place the
    # component exactly where comp-scale does.
    "comp-unscaled-offset": composite_glyph([
        component("quad-explicit", x=100, y=100,
                  transform=[[HALF, 0], [0, HALF]],
                  flags=FLAG_UNSCALED_COMPONENT_OFFSET)]),
    # Two different scales, which is a different encoding and not just a
    # different number.
    "comp-xy-scale": composite_glyph([
        component("quad-explicit", x=0, y=0,
                  transform=[[HALF, 0], [0, QUARTER]])]),
    # A shear, whose off-diagonal terms are the two a transposed read swaps.
    # The matrix is asymmetric in both directions for that reason.
    "comp-two-by-two": composite_glyph([
        component("quad-explicit", x=0, y=0,
                  transform=[[1.0, QUARTER], [0, HALF]])]),
    # Point matching: the second component moves so that its point 0 lands on
    # point 2 of what has been assembled so far.
    "comp-point-match": composite_glyph([
        component("quad-explicit", x=0, y=0),
        component("quad-implied", first=2, second=0)]),
    # Point matching with a transform, which is applied to the component
    # *before* the two points are made to coincide.
    "comp-point-match-scaled": composite_glyph([
        component("quad-explicit", x=0, y=0),
        component("quad-implied", first=2, second=0,
                  transform=[[HALF, 0], [0, HALF]])]),
    # A composite whose component is itself a composite. Its point indices
    # count from the start of *its own* contribution, which is the thing a
    # reader gets wrong by using the outline's length instead.
    "comp-nested": composite_glyph([
        component("comp-offset", x=200, y=0),
        component("quad-implied", x=-100, y=300)]),
    # USE_MY_METRICS, which changes no coordinate and must therefore change no
    # coordinate: a reader that treats an unknown flag as a transform moves the
    # component.
    "comp-use-my-metrics": composite_glyph([
        component("quad-explicit", x=50, y=0, flags=FLAG_USE_MY_METRICS)]),
    # ROUND_XY_TO_GRID and OVERLAP_COMPOUND, both of which this library reads
    # and ignores - the first because there is no hinting to round for.
    "comp-round-and-overlap": composite_glyph([
        component("quad-explicit", x=50, y=0,
                  flags=FLAG_ROUND_XY_TO_GRID | FLAG_OVERLAP_COMPOUND)]),
    # Point matching at an index that does not fit in a signed byte. 150 is
    # a point of `many-points`, and reading the index as signed makes it -106,
    # which is no point at all: the glyph stops loading rather than loading
    # wrongly, and every index the other fixtures use is below 128 where the
    # two readings agree.
    "comp-point-match-high": composite_glyph([
        component("many-points", x=0, y=0),
        component("quad-explicit", first=150, second=0)]),
    # Two components and instructions after them, which is where a composite's
    # instruction length lives and what a reader walking components has to stop
    # before.
    "comp-instructions": composite_glyph([
        component("quad-explicit", x=0, y=0),
        component("quad-explicit", x=400, y=0)], program=b"\x00\x01\x02\x03"),
})

COMPOSITE_ORDER = list(COMPOSITE_GLYPHS)

OUTLINE_ADVANCES = {name: 600 for name in COMPOSITE_ORDER}
OUTLINE_ADVANCES["space"] = 300


def outline_cmap(order):
    """Map each glyph past `space` to a codepoint from U+0041 up."""
    mapping = {0x20: "space"}
    for index, name in enumerate(n for n in order if n not in
                                 (".notdef", "space")):
        mapping[0x41 + index] = name
    return [subtable(4, 3, 1, mapping)]


def build_outline_simple(out):
    """The four constructions `glyf` leaves to the reader, each its own glyph."""
    fb = truetype("Outline Simple", outline_cmap(OUTLINE_ORDER),
                  order=OUTLINE_ORDER, glyphs=OUTLINE_GLYPHS,
                  advances=OUTLINE_ADVANCES, glyph_names=True)
    fb.save(out)


def build_outline_composite(out):
    """Every composite flag, and the minimal pair that proves one is read."""
    fb = truetype("Outline Composite", outline_cmap(COMPOSITE_ORDER),
                  order=COMPOSITE_ORDER, glyphs=COMPOSITE_GLYPHS,
                  advances=OUTLINE_ADVANCES, glyph_names=True)
    fb.save(out)


def build_outline_loca_long(out):
    """`head.indexToLocFormat` 1: a long `loca`.

    There is no switch for this. `loca`'s format follows from its own contents -
    fontTools writes the short form when every offset is even and the table fits
    in 128 KiB - and `glyf`'s padding is what makes the offsets even. With
    `padding = 0` and a glyph of odd length, the offsets stop being even and the
    long form is what can hold them.

    The corpus is why this fixture exists: a short `loca` stores each offset
    halved, so the two formats are not one code path with a width parameter, and
    a font small enough to be a fixture is a font that gets the short form by
    default.
    """
    glyphs = dict(OUTLINE_GLYPHS)
    # An odd-length instruction program makes this glyph's description odd, and
    # with no padding every later offset odd with it.
    glyphs["with-instructions"] = raw_glyph(LEAF_EXPLICIT, program=b"\x00")
    fb = truetype("Outline Loca Long", outline_cmap(OUTLINE_ORDER),
                  order=OUTLINE_ORDER, glyphs=glyphs,
                  advances=OUTLINE_ADVANCES, padding=0)
    fb.save(out)
    # The fixture is only this fixture if it came out long. Checked here rather
    # than in a test, because a generator that quietly produced the short form
    # would leave the test asserting something true of every other fixture.
    written = TTFont(out)
    if written["head"].indexToLocFormat != 1:
        raise SystemExit(
            "outline-loca-long.ttf came out with indexToLocFormat "
            "%d: the padding trick no longer forces the long form"
            % written["head"].indexToLocFormat)


def build_outline_cubic(out):
    """The cubic `glyf` extension, which this library refuses by name.

    Flag bit 0x80 is reserved in OpenType 1.9 and is the cubic control-point
    flag in the proposed extension that fontTools already writes. A reader that
    ignores the bit treats a cubic control point as a quadratic one and draws a
    different shape with no error at all, which is the one outcome worth ruling
    out: this library reports ::GFNT_ERR_UNSUPPORTED and names the extension.

    The font also has to declare `head.glyphDataFormat` 1 - fontTools refuses to
    write cubic points into a format 0 font - and that declaration is the other
    half of the refusal: the format field is the font *stating* the extension,
    the flag bit is one glyph *using* it, a reader has to honour whichever it
    meets first, and a font can set the field while containing no cubic glyph.
    Both are refused, with a different message each, and this fixture trips the
    field first.

    So this fixture exists to be *refused*, and `testGlyf` asserts the refusal
    rather than a shape.
    """
    glyphs = dict(OUTLINE_GLYPHS)
    glyphs["quad-explicit"] = raw_glyph(
        [[(100, 0, True), (100, 400, False), (400, 400, False),
          (400, 0, True)]], cubic=True)
    fb = truetype("Outline Cubic", outline_cmap(OUTLINE_ORDER),
                  order=OUTLINE_ORDER, glyphs=glyphs,
                  advances=OUTLINE_ADVANCES, glyph_data_format=1)
    fb.save(out)




def build_outline_cubic_flag(out):
    """A cubic control point in a font that declares glyphDataFormat 0.

    `outline-cubic.ttf` is refused for what its `head` says, and that refusal
    fires first - so the per-glyph flag check behind it never ran, and a planted
    defect that read a cubic control point as a quadratic one was invisible. Two
    defences, and the outer one hid the inner.

    This fixture separates them. fontTools will not write flag bit 0x80 into a
    format 0 font, so the font is written as format 1 and the declaration is
    then patched back to 0: what is left is a font that claims to be ordinary
    and contains a cubic point, which is the shape a reader has to catch on the
    flag alone. It is also a real shape - a font written by a tool that set the
    flag and forgot the field - rather than only a test article.
    """
    glyphs = dict(OUTLINE_GLYPHS)
    glyphs["quad-explicit"] = raw_glyph(
        [[(100, 0, True), (100, 400, False), (400, 400, False),
          (400, 0, True)]], cubic=True)
    fb = truetype("Outline Cubic Flag", outline_cmap(OUTLINE_ORDER),
                  order=OUTLINE_ORDER, glyphs=glyphs,
                  advances=OUTLINE_ADVANCES, glyph_data_format=1)
    fb.save(out)

    with open(out, "rb") as handle:
        data = bytearray(handle.read())
    head_offset = table_offset(data, "head")
    # head.glyphDataFormat is the last field of the table: 52 bytes in.
    if int.from_bytes(data[head_offset + 52:head_offset + 54], "big",
                      signed=True) != 1:
        raise SystemExit("head.glyphDataFormat was not 1 before the patch")
    struct.pack_into(">h", data, head_offset + 52, 0)
    with open(out, "wb") as handle:
        handle.write(bytes(data))


def table_offset(data, want):
    """Where a table starts, from the directory of a written font."""
    tables = int.from_bytes(data[4:6], "big")
    for index in range(tables):
        base = 12 + index * 16
        if bytes(data[base:base + 4]).decode("latin-1") == want:
            return int.from_bytes(data[base + 8:base + 12], "big")
    raise SystemExit("no %s table in the written font" % want)


def build_outline_broken_loca(out):
    """A `loca` entry that runs backwards, which condemns one glyph (M11).

    Written by fontTools and then patched: no font builder will produce this,
    and a hand-built font would be a second implementation of the sfnt writer
    whose bugs would look like this library's. So the bytes are edited, and the
    edit is arithmetic on the directory rather than a literal offset - a
    hard-coded offset would silently patch the wrong table the first time any
    other fixture detail changed.

    `loca` holds numGlyphs+1 offsets, and glyph *n* is the bytes between entry
    *n* and entry *n+1*. So every entry but the last is shared by two glyphs,
    and lowering one of those breaks both - the first runs backwards and the
    second starts a byte early and reads nonsense. The first attempt at this
    fixture lowered entry 5 and condemned two glyphs, which is not the property
    M11 is about.

    The **last** entry is the one that belongs to a single glyph. Lowering it
    condemns the final glyph and nothing else, so "one entry running backwards"
    and "one glyph unreadable" are the same statement, which is what the test
    asserts.
    """
    fb = truetype("Outline Broken Loca", outline_cmap(OUTLINE_ORDER),
                  order=OUTLINE_ORDER, glyphs=OUTLINE_GLYPHS,
                  advances=OUTLINE_ADVANCES)
    fb.save(out)

    with open(out, "rb") as handle:
        data = bytearray(handle.read())
    tables = int.from_bytes(data[4:6], "big")
    entries = {}
    for index in range(tables):
        base = 12 + index * 16
        tag = bytes(data[base:base + 4]).decode("latin-1")
        entries[tag] = (int.from_bytes(data[base + 8:base + 12], "big"),
                        int.from_bytes(data[base + 12:base + 16], "big"))
    loca_offset, loca_length = entries["loca"]
    head_offset = entries["head"][0]
    long_format = int.from_bytes(data[head_offset + 50:head_offset + 52],
                                "big", signed=True)
    if long_format != 0:
        raise SystemExit("this fixture assumes a short loca; it came out long")
    count = loca_length // 2
    if count < 2:
        raise SystemExit("loca is too short to hold a glyph")
    last = count - 1
    start = int.from_bytes(
        data[loca_offset + (last - 1) * 2:loca_offset + (last - 1) * 2 + 2],
        "big")
    if start == 0:
        raise SystemExit("the last glyph starts at zero; nothing to run back")
    struct.pack_into(">H", data, loca_offset + last * 2, start - 1)
    with open(out, "wb") as handle:
        handle.write(bytes(data))

FIXTURES = {
    "basic.ttf": (build_basic,
        "TrueType, cmap format 4 on (0,3) and (3,1), OS/2 v4, post v3"),
    "os2-v0.ttf": (lambda out: build_os2(out, 0),
        "OS/2 version 0: no code page ranges, no x-height, no optical size"),
    "os2-v1.ttf": (lambda out: build_os2(out, 1),
        "OS/2 version 1: code page ranges and nothing later"),
    "os2-v2.ttf": (lambda out: build_os2(out, 2),
        "OS/2 version 2: x-height, cap height, default and break char"),
    "os2-v3.ttf": (lambda out: build_os2(out, 3),
        "OS/2 version 3: version 2's fields with version 3's fsType rules"),
    "os2-v4.ttf": (lambda out: build_os2(out, 4),
        "OS/2 version 4: the common case in the oracle corpus"),
    "os2-v5.ttf": (lambda out: build_os2(out, 5),
        "OS/2 version 5: optical point sizes, stored as twentieths"),
    "cmap-format0.ttf": (build_cmap_format0,
        "cmap format 0 on (1,0): the 256-byte glyph array"),
    "cmap-format6.ttf": (build_cmap_format6,
        "cmap format 6 on (1,0): a trimmed array with firstCode"),
    "cmap-format12.ttf": (build_cmap_format12,
        "cmap format 12 on (3,10) past the BMP, beside a format 4"),
    "cmap-symbol.ttf": (build_cmap_symbol,
        "cmap (3,0) symbol only: U+0041 reachable only via the 0xF0xx rule"),
    "post-v1.ttf": (build_post_v1,
        "post format 1.0: all 258 standard glyphs, no stored names"),
    "post-v2.ttf": (build_post_v2,
        "post format 2.0: standard indices including 257, and one extra name"),
    "post-v3.ttf": (build_post_v3,
        "post format 3.0: a header that states it has no names"),
    "name-macroman.ttf": (build_name_macroman,
        "name: a Macintosh Roman record holding U+2122 as byte 0xAA"),
    "name-mac-encodings.ttf": (build_name_mac_encodings,
        "name: every single-byte Macintosh encoding, all 128 high bytes each"),
    "cff.otf": (build_cff,
        "OTTO flavour: a CFF outline table, every metric table unchanged"),
    "collection.ttc": (build_collection,
        "ttcf: two faces sharing tables, with different OS/2 versions"),
    "outline-simple.ttf": (build_outline_simple,
        "glyf: an explicit quadratic, an implied midpoint, a contour starting "
        "off-curve, the same contour stored from another point of its cycle, "
        "an all-off-curve contour, a one-point contour, instructions, a "
        "200-point contour, an arch whose control box is not its curve, and a "
        "bar whose two edges are each a 259-long run of one repeated flag"),
    "outline-composite.ttf": (build_outline_composite,
        "glyf composites: byte and word offsets, all three transform "
        "encodings, scaled and unscaled offsets, point matching below and "
        "above the signed-byte boundary, nesting, USE_MY_METRICS"),
    "outline-loca-long.ttf": (build_outline_loca_long,
        "loca: head.indexToLocFormat 1, so offsets are stored whole rather "
        "than halved"),
    "outline-cubic.ttf": (build_outline_cubic,
        "glyf: head.glyphDataFormat 1, the cubic extension this library "
        "refuses by name rather than mis-drawing"),
    "outline-cubic-flag.ttf": (build_outline_cubic_flag,
        "glyf: flag bit 0x80 in a font declaring glyphDataFormat 0, so the "
        "per-glyph refusal is the only one that can fire"),
    "outline-broken-loca.ttf": (build_outline_broken_loca,
        "loca: the last entry running backwards, so that exactly one glyph is "
        "corrupt and the rest of the font still answers (M11)"),
}

MANIFEST_HEADER = """\
# The synthetic fixtures, and what each one exercises.
#
# Generated by tools/fixtures/make_fixtures.py in the pinned fonttools image.
# Do not edit either this file or the fonts beside it: `make check-fixtures`
# regenerates them and fails on a byte difference.
#
# Every font here is drawn from outlines in that script, carries this
# repository's copyright, and is LGPL-3.0-only like the code around it. No
# third-party font is committed to this repository at all; the real fonts a
# differential needs live only in the oracle image (design.md section 14.5).
#
# Tab-separated: <filename> <what it exercises>.
"""


def build(out_dir, only=None):
    """Write every fixture, and the MANIFEST that describes them."""
    os.makedirs(out_dir, exist_ok=True)
    written = []
    for name in sorted(FIXTURES):
        if only and name not in only:
            continue
        builder, _ = FIXTURES[name]
        builder(os.path.join(out_dir, name))
        written.append(name)

    with open(os.path.join(out_dir, "MANIFEST"), "w", encoding="utf-8",
            newline="\n") as handle:
        handle.write(MANIFEST_HEADER)
        for name in sorted(FIXTURES):
            handle.write("%s\t%s\n" % (name, FIXTURES[name][1]))
    written.append("MANIFEST")
    return written


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", required=True,
        help="directory to write into; never the repository itself when this "
             "runs in a container, where the repository is mounted read-only")
    parser.add_argument("--only", action="append", default=None,
        help="build just this fixture (repeatable); for iterating on one")
    parser.add_argument("--list", action="store_true",
        help="print the fixture set and exit")
    args = parser.parse_args(argv)

    if args.list:
        for name in sorted(FIXTURES):
            print("%-20s %s" % (name, FIXTURES[name][1]))
        return 0

    written = build(args.out, only=args.only)
    print("wrote %d file(s) to %s" % (len(written), args.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
