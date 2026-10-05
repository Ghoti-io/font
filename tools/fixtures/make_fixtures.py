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
from fontTools.cffLib import cffStandardStrings
from fontTools.designspaceLib import AxisDescriptor
from fontTools.ttLib.tables.TupleVariation import TupleVariation
from fontTools.misc.roundTools import otRound
from fontTools.ttLib.tables.DefaultTable import DefaultTable

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


# A variable font whose `gvar` has every construction a reader has to tell apart.
#
# It exists because the real variable fonts in the oracle image are four producers'
# ordinary output, and the things they share are what a reader gets right by
# accident: a tuple that names every point, a peak at the end of an axis, a
# composite that is a base and a mark. Each glyph below is one *encoding choice*
# and is named for it. fontTools writes the table - it decides which peaks are
# shared tuples, which point lists are shared, and how deltas are packed - so the
# encoding is not this library's opinion of the format.
#
# Nothing here is third-party: every outline is drawn below, and every delta is a
# small number chosen so that a wrong reading is visibly a different shape.
VARIABLE_ORDER = [".notdef", "space", "tri", "bar", "ring", "dot", "acc", "acc2",
                  "ghost"]

VARIABLE_ADVANCES = {".notdef": 600, "space": 300, "tri": 400, "bar": 300,
                     "ring": 500, "dot": 150, "acc": 700, "acc2": 800,
                     "ghost": 250}

# Eight points round a square and four inside it. The outer contour alternates
# on- and off-curve so that "which point moved" is also "which kind of point".
RING = [
    [(0, 0, True), (200, -50, False), (400, 0, True), (450, 200, False),
     (400, 400, True), (200, 450, False), (0, 400, True), (-50, 200, False)],
    [(100, 100, True), (100, 300, True), (300, 300, True), (300, 100, True)],
]

TRI = [[(0, 0, True), (300, 0, True), (150, 400, True)]]

# Two corners named and the four points between them left to be inferred. Along
# each edge the inferred point sits at a different place relative to the two named
# ones - between them, at one of them, and in a position where only one axis has
# anything to interpolate - which is the whole of what IUP has to get right.
BAR = [[(0, 0, True), (100, 0, True), (200, 0, True), (200, 300, True),
        (100, 300, True), (0, 300, True)]]

DOT = [[(0, 0, True), (0, 100, True), (100, 100, True), (100, 0, True)]]

VARIABLE_GLYPHS = {
    ".notdef": raw_glyph(OUTLINE_CONTOURS),
    "space": raw_glyph([]),
    "tri": raw_glyph(TRI),
    "bar": raw_glyph(BAR),
    "ring": raw_glyph(RING),
    "dot": raw_glyph(DOT),
    # Four components of one glyph, each placed a different way: byte offsets, word
    # offsets, a scaled component with the scaled-offset flag, and one placed by
    # matching points - whose delta is ignored, because there is no offset for it
    # to move.
    "acc": composite_glyph([
        component("dot", x=100, y=100),
        component("dot", x=-200, y=300),
        component("dot", x=50, y=50, transform=((0.5, 0), (0, 0.5)),
                  flags=0x0800),
        component("dot", first=0, second=1),
    ]),
    # A composite of a composite: the inner one must be varied before the outer
    # one places it.
    "acc2": composite_glyph([
        component("acc", x=0, y=0),
        component("tri", x=500, y=0),
    ]),
    # No contours, and variation data: the phantom points alone, which is what a
    # `space` carries when its advance varies.
    "ghost": raw_glyph([]),
}


def _phantoms(*moves):
    """Four phantom points, each a delta or None."""
    out = list(moves) + [None] * (4 - len(moves))
    return out


def _tuple(axes, deltas):
    """A TupleVariation from {tag: (start, peak, end)} and a delta list."""
    return TupleVariation(axes, deltas)


def variable_variations():
    """Every glyph's tuples, as fontTools wants them for `setupGvar`."""
    wght_up = {"wght": (0.0, 1.0, 1.0)}
    wght_down = {"wght": (-1.0, -1.0, 0.0)}
    wdth_up = {"wdth": (0.0, 1.0, 1.0)}
    wdth_down = {"wdth": (-1.0, -1.0, 0.0)}
    return {
        # One point named: IUP's single-point rule moves the whole contour by it.
        # And a second tuple that names every point, on the other side of the
        # axis, so the two directions of one axis are separate tuples.
        "tri": [
            _tuple(wght_up, [None, None, (0, 100)] + _phantoms()),
            _tuple(wght_down, [(10, 0), (-10, 0), (0, -20)]
                   + _phantoms((0, 0), (0, 0))),
        ],
        # Two named corners; the rest inferred.
        "bar": [
            _tuple(wght_up, [(-20, -10), None, None, (40, 30), None, None]
                   + _phantoms()),
        ],
        "ring": [
            # An intermediate region: the ramp is only visible between the start
            # and the peak, and a reader that treats it as a plain tuple gets
            # every value there wrong.
            _tuple({"wght": (0.25, 0.5, 1.0)},
                   [(i, -i) for i in range(12)] + _phantoms()),
            # Named points in two contours: two in the first (opposite one
            # another, so the inference wraps) and one in the second (so the
            # whole contour moves).
            _tuple(wdth_down,
                   [None, (30, 0), None, None, None, (-30, 10), None, None,
                    (15, -15), None, None, None] + _phantoms()),
            # A corner of the design space: both axes at once, so the scalar is a
            # product.
            _tuple({"wght": (0.0, 1.0, 1.0), "wdth": (0.0, 1.0, 1.0)},
                   [(2, 3)] * 12 + _phantoms()),
            # An axis's far end alone, so the peak at -1 is a different tuple from
            # the corner above.
            _tuple(wght_down, [(-3, -3)] * 12 + _phantoms()),
        ],
        "dot": [
            _tuple(wght_up, [(8, 0), (8, 0), (-8, 0), (-8, 0)] + _phantoms()),
            _tuple(wdth_up, [(0, 5), (0, -5), (0, -5), (0, 5)] + _phantoms()),
        ],
        # A composite's "points" are its components, in order, before the phantoms.
        # The last component is matched by points, and its delta is ignored.
        "acc": [
            _tuple(wght_up, [(10, 20), (-5, 0), (7, 7), (100, 100)]
                   + _phantoms()),
            # Only the second component named: the others stay where they are.
            _tuple(wdth_up, [None, (0, 30), None, None] + _phantoms()),
        ],
        "acc2": [
            _tuple(wght_up, [(-50, 25), (0, 0)] + _phantoms()),
        ],
        "ghost": [
            _tuple(wght_up, _phantoms((0, 0), (30, 0), (0, 0), (0, 0))),
        ],
    }


def _variable_font(label):
    """`fvar`, `avar` and `gvar` around the glyphs above, before it is saved."""
    fb = truetype(label, outline_cmap(VARIABLE_ORDER),
                  order=VARIABLE_ORDER, glyphs=VARIABLE_GLYPHS,
                  advances=VARIABLE_ADVANCES, glyph_names=True)
    # Every glyph's left side bearing is its own xMin, as in a font anyone ships.
    # `truetype()` writes zero, which is harmless while nothing looks at it - but a
    # glyph set drawn at a location shifts every outline by `lsb - xMin`, the
    # TrueType rule that the left side bearing point is the origin, so a font that
    # states a bearing its outline does not have is drawn somewhere other than
    # where its points are. That would make the reference disagree about *every*
    # glyph for a reason that has nothing to do with `gvar`.
    glyf = fb.font["glyf"]
    hmtx = fb.font["hmtx"]
    for name in VARIABLE_ORDER:
        glyph = glyf[name]
        glyph.recalcBounds(glyf)
        hmtx.metrics[name] = (VARIABLE_ADVANCES[name],
                              glyph.xMin if hasattr(glyph, "xMin") else 0)
    weight = AxisDescriptor(name="Weight", tag="wght", minimum=100, default=400,
                            maximum=900)
    # Hidden, which is the one flag the format defines and which no real font in
    # the image sets on an axis a reader would otherwise see.
    width = AxisDescriptor(name="Width", tag="wdth", minimum=75, default=100,
                           maximum=125, hidden=True)
    fb.setupFvar([weight, width], [
        {"location": {"wght": 300, "wdth": 100}, "stylename": "Light"},
        # The only instance with a PostScript name, so the two record sizes the
        # format allows are both present.
        {"location": {"wght": 400, "wdth": 100}, "stylename": "Regular",
         "postscriptfontname": "VariableGvar-Regular"},
        {"location": {"wght": 700, "wdth": 75}, "stylename": "Bold Condensed"},
    ])
    avar = fb.font["avar"] = newTable("avar")
    avar.segments = {
        # Bent on both sides of the default and unbent at the ends.
        "wght": {-1.0: -1.0, -0.5: -0.25, 0.0: 0.0, 0.5: 0.75, 1.0: 1.0},
        # Present and the identity: a map with pairs that does nothing.
        "wdth": {-1.0: -1.0, 0.0: 0.0, 1.0: 1.0},
    }
    fb.setupGvar(variable_variations())
    return fb


def build_variable_gvar(out):
    """`fvar`, `avar` and `gvar`, written by fontTools around the glyphs above."""
    fb = _variable_font("Variable Gvar")
    pin(fb)
    fb.save(out)


# What `HVAR` and `MVAR` say in `variable-hvar.ttf`. Every number is small and
# distinct so that a wrong row, a wrong region or a wrong sign is visibly a
# different advance, and each is meant to be worked out by hand: the unit tests
# hold this library to the same numbers on a hand-built table and the oracle holds
# it to fontTools and FreeType on this one.
#
# Two regions, a ramp up to the end of each axis: wght 0..1 and wdth 0..1.
HVAR_REGIONS = [{"wght": (0.0, 1.0, 1.0)}, {"wdth": (0.0, 1.0, 1.0)}]
# Advance rows, one per glyph in VARIABLE_ORDER: (wght delta, wdth delta). `ghost`
# is (0, 0) on purpose: its `gvar` moves its phantom points by 30, so a reader
# that preferred the phantom points to `HVAR` would answer 280 and not 250.
HVAR_ADVANCE_ROWS = [(10, 0), (0, 5), (20, -10), (-7, 3), (33, 0), (5, 5),
                     (-40, 10), (15, -15), (0, 0)]
# Left-bearing rows: three, shared by the nine glyphs through the mapping (glyph
# `i` takes row `i % 3`), which is what makes it a *mapping* and not a list.
HVAR_BEARING_ROWS = [(5, 0), (-5, 2), (12, -3)]
# MVAR, by tag: (wght delta, wdth delta). The hhea, window and typographic
# metrics each have their own, and the descenders of the three differ in sign
# convention - the window descent is stored positive - which is the point.
MVAR_ROWS = {
    "hasc": (30, 5), "hdsc": (-12, 0), "hlgp": (3, 3),
    "hcla": (20, -4), "hcld": (6, 1),
    "tasc": (25, 0), "tdsc": (-9, 2), "tlgp": (4, 0),
}


def _var_store(rows_by_data):
    """A VarStore from [(region indices, [row...])]: one VarData each."""
    from fontTools.varLib import builder

    regions = builder.buildVarRegionList(HVAR_REGIONS, ["wght", "wdth"])
    datas = [builder.buildVarData(indices, rows, optimize=False)
             for indices, rows in rows_by_data]
    return builder.buildVarStore(regions, datas)


# What `avar` version 2 says in `variable-avar2.ttf`: each axis is moved by a delta
# that is a function of the others, in 2.14 units (16384 is the whole axis). Three
# regions: wght ramping up, wdth ramping up, and wght ramping *down* so that the
# negative half of an axis is exercised too.
AVAR2_REGIONS = [{"wght": (0.0, 1.0, 1.0)}, {"wdth": (0.0, 1.0, 1.0)},
                 {"wght": (-1.0, -1.0, 0.0)}]
# Two rows, (R0, R1, R2) each: row 0 moves the *width* axis by weight, row 1 moves
# the *weight* axis by width. The index map sends axis 0 (weight) to row 1 and
# axis 1 (width) to row 0, so the map is what chooses - a reader that took the axis
# number as the row would swap them.
AVAR2_ROWS = [(-2500, 0, 800), (0, 3000, 0)]


def build_variable_avar2(out):
    """`variable-gvar.ttf`'s font with an `avar` version 2: a store and a map."""
    from fontTools.ttLib.tables import otTables as ot
    from fontTools.varLib import builder

    fb = _variable_font("Variable Avar2")
    avar = fb.font["avar"]
    avar.majorVersion = 2
    avar.minorVersion = 0
    table = avar.table = ot.avar()
    regions = builder.buildVarRegionList(AVAR2_REGIONS, ["wght", "wdth"])
    data = builder.buildVarData([0, 1, 2], [list(r) for r in AVAR2_ROWS],
                                optimize=False)
    table.VarStore = builder.buildVarStore(regions, [data])
    table.VarIdxMap = builder.buildDeltaSetIndexMap([(0 << 16) | 1,
                                                     (0 << 16) | 0])
    pin(fb)
    fb.save(out)


def build_variable_stat(out):
    """`variable-gvar.ttf`'s font with a `STAT` of all four axis value formats."""
    from fontTools.otlLib.builder import buildStatTable

    fb = _variable_font("Variable Stat")
    buildStatTable(fb.font, [
        {"tag": "wght", "name": "Weight", "ordering": 0, "values": [
            # Format 3, elidable, linked to Bold: how Regular is stated.
            {"value": 400, "name": "Regular", "flags": 0x2, "linkedValue": 700},
            # Format 1.
            {"value": 700, "name": "Bold"},
            # Format 2: 550 to 650 around a nominal 600.
            {"nominalValue": 600, "rangeMinValue": 550, "rangeMaxValue": 650,
             "name": "Semibold", "flags": 0x1},
        ]},
        {"tag": "wdth", "name": "Width", "ordering": 1, "values": [
            {"value": 100, "name": "Normal", "flags": 0x2},
            {"value": 75, "name": "Condensed"},
        ]},
    ], locations=[
        # Format 4: both axes at once.
        {"name": "Bold Condensed", "location": {"wght": 700, "wdth": 75}},
    ], elidedFallbackName=2)
    pin(fb)
    fb.save(out)


def build_variable_featurevars(out):
    """`variable-gvar.ttf`'s font with a GSUB whose `rvrn` varies with the axes."""
    from fontTools.varLib.featureVars import addFeatureVariations

    fb = _variable_font("Variable FeatureVariations")
    # Bounds are normalised coordinates, after avar. Three records, in the order a
    # location is matched against them: the upper half of weight; the lighter
    # half of weight *and* the wider half of width (two conditions that both have
    # to hold); and the narrower half of width alone.
    addFeatureVariations(fb.font, [
        ([{"wght": (0.5, 1.0)}], {"bar": "dot"}),
        ([{"wght": (-1.0, -0.5), "wdth": (0.25, 1.0)}], {"tri": "ring"}),
        ([{"wdth": (-1.0, -0.5)}], {"bar": "acc", "tri": "acc2"}),
    ], featureTag="rvrn")
    pin(fb)
    fb.save(out)


def build_variable_cvar(out):
    """`variable-gvar.ttf`'s font with a `cvt ` and a `cvar` that moves it."""
    from array import array

    from fontTools.ttLib.tables.TupleVariation import TupleVariation

    fb = _variable_font("Variable Cvar")
    font = fb.font
    cvt = font["cvt "] = newTable("cvt ")
    cvt.values = array("h", [100, -50, 300, 7, 0, 1000, -3, 42])
    cvar = font["cvar"] = newTable("cvar")
    cvar.version = 1
    cvar.variations = [
        # Every value: the weight ramp up to 1.
        TupleVariation({"wght": (0.0, 1.0, 1.0)},
                       [10, -20, 0, 7, 3, -9, 5, 1]),
        # Some values only, so the tuple names points: the width ramp, which
        # moves values 1, 3 and 6 and leaves the rest where they are.
        TupleVariation({"wdth": (0.0, 1.0, 1.0)},
                       [None, 40, None, -15, None, None, 12, None]),
        # An intermediate region on weight, peaking in the lighter half, and a
        # two-axis corner, so that both kinds of scalar are in one table.
        TupleVariation({"wght": (-1.0, -0.5, 0.0)},
                       [-8, None, 25, None, None, 6, None, -2]),
        TupleVariation({"wght": (0.0, 1.0, 1.0), "wdth": (-1.0, -1.0, 0.0)},
                       [None, None, None, None, 9, None, None, -30]),
    ]
    pin(fb)
    fb.save(out)


def build_variable_hvar(out):
    """`variable-gvar.ttf` with `HVAR` (advance and bearing mappings) and `MVAR`."""
    from fontTools.ttLib.tables import otTables as ot
    from fontTools.varLib import builder

    fb = _variable_font("Variable Hvar")
    font = fb.font
    tags = list(MVAR_ROWS)
    store = _var_store([
        ([0, 1], [list(row) for row in HVAR_ADVANCE_ROWS]),
        ([0, 1], [list(row) for row in HVAR_BEARING_ROWS]),
        ([0, 1], [list(MVAR_ROWS[tag]) for tag in tags]),
    ])
    hvar = font["HVAR"] = newTable("HVAR")
    table = hvar.table = ot.HVAR()
    table.Version = 0x00010000
    table.VarStore = store
    table.AdvWidthMap = builder.buildVarIdxMap(
        [(0 << 16) | i for i in range(len(VARIABLE_ORDER))], VARIABLE_ORDER)
    table.LsbMap = builder.buildVarIdxMap(
        [(1 << 16) | (i % 3) for i in range(len(VARIABLE_ORDER))],
        VARIABLE_ORDER)
    table.RsbMap = None
    mvar = font["MVAR"] = newTable("MVAR")
    table = mvar.table = ot.MVAR()
    table.Version = 0x00010000
    table.Reserved = 0
    table.VarStore = store
    table.ValueRecordSize = 8
    records = []
    for row, tag in enumerate(tags):
        record = ot.MetricsValueRecord()
        record.ValueTag = tag
        record.VarIdx = (2 << 16) | row
        records.append(record)
    table.ValueRecord = sorted(records, key=lambda r: r.ValueTag)
    table.ValueRecordCount = len(records)
    pin(fb)
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

# --------------------------------------------------------------------------
# CFF, assembled here rather than by fontTools
# --------------------------------------------------------------------------
#
# design.md section 7.4, and the reason this exists at all: a `T2CharStringPen`
# writes the operators a *pen* needs - moves, lines, curves - and nothing else.
# It writes no `flex`, no `hintmask`, no accented character, no arithmetic, no
# subroutine, and it cannot build a CID-keyed font, a charset in a format other
# than 0, or an encoding at all. A survey of the oracle image's 35 OTF fonts
# found the same gaps from the other end: they use every curve operator,
# `hintmask`, `cntrmask` and both subroutine flavours, and between them not one
# flex, not one accented character and not one CID font.
#
# So the fixtures below are CFF tables written byte by byte and wrapped in an
# sfnt that fontTools builds. That split is deliberate: everything a pen can
# write comes from the reference, and the constructions it cannot write are
# hand-built and named in the MANIFEST - the same arrangement `outline-simple`'s
# repeated flag run and `outline-broken-loca`'s patched entry already use.
#
# Both sides of `cff_diff.py` still read these fonts: fontTools decompiles and
# draws the same bytes this library does, so a hand-built charstring is not this
# library's opinion about what it means.

# Type 2 operators, by the name this file writes them as. A tuple of the bytes
# the format spells them with, so the two-byte ones are not a special case at
# every call site.
T2_OPERATORS = {
    "hstem": (1,), "vstem": (3,), "vmoveto": (4,), "rlineto": (5,),
    "hlineto": (6,), "vlineto": (7,), "rrcurveto": (8,), "callsubr": (10,),
    "return": (11,), "endchar": (14,), "hstemhm": (18,), "hintmask": (19,),
    "cntrmask": (20,), "rmoveto": (21,), "hmoveto": (22,), "vstemhm": (23,),
    "rcurveline": (24,), "rlinecurve": (25,), "vvcurveto": (26,),
    "hhcurveto": (27,), "callgsubr": (29,), "vhcurveto": (30,),
    "hvcurveto": (31,),
    "and": (12, 3), "or": (12, 4), "not": (12, 5), "abs": (12, 9),
    "add": (12, 10), "sub": (12, 11), "div": (12, 12), "neg": (12, 14),
    "eq": (12, 15), "drop": (12, 18), "put": (12, 20), "get": (12, 21),
    "ifelse": (12, 22), "random": (12, 23), "mul": (12, 24), "sqrt": (12, 26),
    "dup": (12, 27), "exch": (12, 28), "index": (12, 29), "roll": (12, 30),
    "hflex": (12, 34), "flex": (12, 35), "hflex1": (12, 36), "flex1": (12, 37),
}

# Type 1 operators. Sharing one table with Type 2 would be wrong in the way
# that matters: `13` is `hsbw` in Type 1 and reserved in Type 2, `9` is
# `closepath` in one and nothing in the other, and `255` introduces a plain
# integer here and a 16.16 there.
T1_OPERATORS = {
    "hstem": (1,), "vstem": (3,), "vmoveto": (4,), "rlineto": (5,),
    "hlineto": (6,), "vlineto": (7,), "rrcurveto": (8,), "closepath": (9,),
    "callsubr": (10,), "return": (11,), "hsbw": (13,), "endchar": (14,),
    "rmoveto": (21,), "hmoveto": (22,), "vhcurveto": (30,), "hvcurveto": (31,),
    "dotsection": (12, 0), "vstem3": (12, 1), "hstem3": (12, 2),
    "seac": (12, 6), "sbw": (12, 7), "div": (12, 12),
    "callothersubr": (12, 16), "pop": (12, 17), "setcurrentpoint": (12, 33),
}


class Fixed:
    """An operand written in the 255 form: 16.16 in Type 2, an integer in Type 1.

    A wrapper rather than a float, because which form an operand is written in
    is a property of the *program* being built and not of the number's value: a
    charstring that says `100` and one that says `100.0` are different bytes and
    a reader can be wrong about exactly one of them.
    """

    def __init__(self, value):
        self.value = value


def t2_operand(value):
    """One Type 2 operand, in the shortest form that holds it."""
    if isinstance(value, Fixed):
        return b"\xff" + struct.pack(">i", otRound(value.value * 65536))
    if not isinstance(value, int):
        raise TypeError("a charstring operand is an int or a Fixed: %r" % value)
    if -107 <= value <= 107:
        return bytes([value + 139])
    if 108 <= value <= 1131:
        shifted = value - 108
        return bytes([247 + (shifted >> 8), shifted & 0xFF])
    if -1131 <= value <= -108:
        shifted = -value - 108
        return bytes([251 + (shifted >> 8), shifted & 0xFF])
    if -32768 <= value <= 32767:
        return b"\x1c" + struct.pack(">h", value)
    # Past what the 16-bit form holds, the only way to say it is the 16.16 one.
    return b"\xff" + struct.pack(">i", value * 65536)


def t1_operand(value):
    """One Type 1 operand. `255` is a plain 32-bit integer here."""
    if isinstance(value, Fixed):
        raise TypeError("Type 1 has no fixed-point operand")
    if -107 <= value <= 107:
        return bytes([value + 139])
    if 108 <= value <= 1131:
        shifted = value - 108
        return bytes([247 + (shifted >> 8), shifted & 0xFF])
    if -1131 <= value <= -108:
        shifted = -value - 108
        return bytes([251 + (shifted >> 8), shifted & 0xFF])
    return b"\xff" + struct.pack(">i", value)


def charstring(*tokens, type1=False):
    """A charstring from operands, operator names and `("mask", bytes)` pairs."""
    operators = T1_OPERATORS if type1 else T2_OPERATORS
    operand = t1_operand if type1 else t2_operand
    out = bytearray()
    for token in tokens:
        if isinstance(token, str):
            if token not in operators:
                raise KeyError("no such charstring operator: %s" % token)
            out += bytes(operators[token])
        elif isinstance(token, tuple) and token and token[0] == "mask":
            out += token[1]
        else:
            out += operand(token)
    return bytes(out)


def cff_index(items):
    """An INDEX: a count, an offset size, count+1 offsets, then the data.

    The offsets are **one-based from the byte before the data**, which is the
    detail a reader gets wrong silently: an off-by-one here shifts every element
    by a byte, and a charstring read one byte late is a different program.
    """
    if not items:
        return struct.pack(">H", 0)
    offsets = [1]
    for item in items:
        offsets.append(offsets[-1] + len(item))
    largest = offsets[-1]
    off_size = 1 if largest < 0x100 else 2 if largest < 0x10000 else \
        3 if largest < 0x1000000 else 4
    out = bytearray(struct.pack(">HB", len(items), off_size))
    for offset in offsets:
        out += offset.to_bytes(off_size, "big")
    for item in items:
        out += item
    return bytes(out)


def cff_dict_operand(value):
    """A DICT operand. Integers only: the reals this library reads are the
    FontMatrix's, and the fixtures that carry one write it explicitly."""
    if -107 <= value <= 107:
        return bytes([value + 139])
    if 108 <= value <= 1131:
        shifted = value - 108
        return bytes([247 + (shifted >> 8), shifted & 0xFF])
    if -1131 <= value <= -108:
        shifted = -value - 108
        return bytes([251 + (shifted >> 8), shifted & 0xFF])
    if -32768 <= value <= 32767:
        return b"\x1c" + struct.pack(">h", value)
    return b"\x1d" + struct.pack(">i", value)


def cff_dict_offset(value):
    """An offset operand, always in the five-byte form.

    Not an optimisation to avoid: a DICT whose offsets change width when their
    values change would change the DICT's *size* when it is patched, and every
    offset in the font would move. Fixing the width makes the layout computable
    in one pass.
    """
    return b"\x1d" + struct.pack(">i", value)


def cff_dict_real(value=None, digits=6, *, text=None):
    """A DICT real, nibble-encoded, as a `FontMatrix` carries one.

    `text` writes an exact spelling instead of a formatted value, because with a
    real number the *encoding* is what a fixture is for: one number can be
    written with a decimal point, with an exponent, or with more mantissa digits
    than a 16.16 can hold, and a reader has a separate path for each of those.
    """
    if text is None:
        text = ("%.*g" % (digits, value))
    nibbles = []
    for char in text:
        if char.isdigit():
            nibbles.append(int(char))
        elif char == ".":
            nibbles.append(0x0A)
        elif char in "eE":
            nibbles.append(0x0B)
        elif char == "-":
            if nibbles and nibbles[-1] == 0x0B:
                # 0x0C is "E-" in one nibble, so it *replaces* the exponent
                # marker rather than following it. Emitting both would spell an
                # exponent twice, which is not a number at all.
                nibbles[-1] = 0x0C
            else:
                nibbles.append(0x0E)
        elif char == "+":
            continue
        else:
            raise ValueError("cannot spell %r as a DICT real" % text)
    nibbles.append(0x0F)
    if len(nibbles) % 2:
        nibbles.append(0x0F)
    out = bytearray(b"\x1e")
    for at in range(0, len(nibbles), 2):
        out.append((nibbles[at] << 4) | nibbles[at + 1])
    return bytes(out)


def cff_dict(entries):
    """A DICT from `(operator, [operands])` pairs, operands first as the format
    wants them. An operator is an int, or a tuple for the two-byte ones."""
    out = bytearray()
    for operator, operands in entries:
        for operand in operands:
            if isinstance(operand, bytes):
                out += operand
            else:
                out += cff_dict_operand(operand)
        if isinstance(operator, tuple):
            out += bytes(operator)
        else:
            out += bytes([operator])
    return bytes(out)


def cff_charset_format0(sids):
    """Format 0: one SID per glyph, glyph 0 left out because it is `.notdef`."""
    out = bytearray(b"\x00")
    for sid in sids:
        out += struct.pack(">H", sid)
    return bytes(out)


def cff_charset_ranges(ranges, wide=False):
    """Format 1 or 2: `(first SID, how many follow)` ranges.

    The difference between the two is one field's width, which is exactly the
    kind of pair where a reader tests one arm and assumes the other.
    """
    out = bytearray(b"\x02" if wide else b"\x01")
    for first, left in ranges:
        out += struct.pack(">HH" if wide else ">HB", first, left)
    return bytes(out)


def cff_encoding_format0(codes, supplements=()):
    """Format 0: a code per glyph, from glyph 1, plus optional supplements."""
    out = bytearray([0x80 if supplements else 0x00, len(codes)])
    out += bytes(codes)
    if supplements:
        out.append(len(supplements))
        for code, sid in supplements:
            out += struct.pack(">BH", code, sid)
    return bytes(out)


def cff_encoding_format1(ranges, supplements=()):
    """Format 1: `(first code, how many follow)` ranges, from glyph 1."""
    out = bytearray([0x81 if supplements else 0x01, len(ranges)])
    for first, left in ranges:
        out += bytes([first, left])
    if supplements:
        out.append(len(supplements))
        for code, sid in supplements:
            out += struct.pack(">BH", code, sid)
    return bytes(out)


def cff_fdselect_format3(ranges, sentinel):
    """Format 3: `(first glyph, FD)` ranges and a sentinel past the last glyph."""
    out = bytearray(b"\x03")
    out += struct.pack(">H", len(ranges))
    for first, fd in ranges:
        out += struct.pack(">HB", first, fd)
    out += struct.pack(">H", sentinel)
    return bytes(out)


# What every fixture below claims as its FontBBox. A CFF's bounding box is the
# font's own claim rather than a computed fact - this library never reads it -
# but fontTools' `head` compiler does, so it has to be there and it has to
# contain the glyphs, or the sfnt around the table would state a box its own
# outlines leave.
CFF_FONT_BBOX = (0, -200, 1000, 900)


def assemble_cff(font_name, charstrings, *, strings=(), gsubrs=(),
                 charset=None, encoding=None, privates=None, fdselect=None,
                 is_cid=False, charstring_type=None, font_matrix=None,
                 extra_top=()):
    """One CFF table's bytes.

    `privates` is a list of `(entries, local subrs)`: one for an ordinary font,
    one per Font DICT for a CID-keyed one. Every offset is written in the
    five-byte form, so each structure's size is known before its position is,
    and the layout is computed in one pass rather than iterated to a fixed
    point.

    The `Subrs` offset is the one that is **relative to its own Private DICT**
    rather than to the table, so it is the Private DICT's own length - which is
    why the local subroutines are placed immediately after it.
    """
    privates = privates if privates is not None else [({}, ())]

    # Each Private DICT, with its Subrs offset pointing just past itself.
    private_blocks = []
    for entries, subrs in privates:
        items = [(op, [value]) for op, value in sorted(entries.items())]
        if subrs:
            probe = cff_dict(items + [(19, [cff_dict_offset(0)])])
            block = cff_dict(items + [(19, [cff_dict_offset(len(probe))])])
            private_blocks.append((block, cff_index(list(subrs))))
        else:
            private_blocks.append((cff_dict(items), b""))

    # The Font DICTs of a CID-keyed font, each naming one Private DICT. Built
    # with placeholder offsets first, only to learn their size.
    def font_dicts(offsets):
        return [cff_dict([((12, 38), [391 + index]),
                          (18, [cff_dict_offset(size),
                                cff_dict_offset(offset)])])
                for index, (size, offset) in enumerate(offsets)]

    placeholder = [(len(block), 0) for block, _ in private_blocks]
    fdarray_probe = cff_index(font_dicts(placeholder)) if is_cid else b""

    charstrings_index = cff_index(list(charstrings))

    def top_dict(offsets):
        entries = list(extra_top)
        entries.append((5, list(CFF_FONT_BBOX)))
        if is_cid:
            # ROS first, which is what makes the font CID-keyed at all: the
            # registry and ordering are SIDs into the strings, and the
            # supplement is a number.
            entries.append(((12, 30), [391, 392, 0]))
        if font_matrix is not None:
            entries.append(((12, 7), [
                v if isinstance(v, bytes) else cff_dict_real(v)
                for v in font_matrix]))
        if charstring_type is not None:
            entries.append(((12, 6), [charstring_type]))
        if charset is not None:
            entries.append((15, [cff_dict_offset(offsets["charset"])]))
        if encoding is not None:
            entries.append((16, [cff_dict_offset(offsets["encoding"])]))
        entries.append((17, [cff_dict_offset(offsets["charstrings"])]))
        if is_cid:
            entries.append(((12, 36), [cff_dict_offset(offsets["fdarray"])]))
            if fdselect is not None:
                entries.append(((12, 37),
                    [cff_dict_offset(offsets["fdselect"])]))
        else:
            entries.append((18, [cff_dict_offset(len(private_blocks[0][0])),
                                 cff_dict_offset(offsets["private"])]))
        return cff_dict(entries)

    zero = {"charset": 0, "encoding": 0, "charstrings": 0, "fdarray": 0,
            "fdselect": 0, "private": 0}
    header = bytes([1, 0, 4, 2])
    name_index = cff_index([font_name.encode("ascii")])
    # Latin-1 and not ASCII: CFF says its strings hold ASCII and fonts put
    # Latin-1 in them, which is what fontTools decodes them as, so a fixture has
    # to be able to carry a byte above 0x7F for the reader to be tested on one.
    string_index = cff_index([s.encode("latin-1") for s in strings])
    gsubr_index = cff_index(list(gsubrs))
    top_index_probe = cff_index([top_dict(zero)])

    # Everything before the variable blocks is now a known size.
    at = len(header) + len(name_index) + len(top_index_probe) \
        + len(string_index) + len(gsubr_index)
    offsets = dict(zero)
    if charset is not None:
        offsets["charset"] = at
        at += len(charset)
    if encoding is not None:
        offsets["encoding"] = at
        at += len(encoding)
    if is_cid and fdselect is not None:
        offsets["fdselect"] = at
        at += len(fdselect)
    offsets["charstrings"] = at
    at += len(charstrings_index)
    if is_cid:
        offsets["fdarray"] = at
        at += len(fdarray_probe)
    private_offsets = []
    for block, subrs in private_blocks:
        private_offsets.append((len(block), at))
        at += len(block) + len(subrs)
    offsets["private"] = private_offsets[0][1]

    top_index = cff_index([top_dict(offsets)])
    if len(top_index) != len(top_index_probe):
        raise AssertionError("the Top DICT changed size when it was patched, "
                             "so every offset in this font is now wrong")

    out = bytearray()
    out += header
    out += name_index
    out += top_index
    out += string_index
    out += gsubr_index
    if charset is not None:
        out += charset
    if encoding is not None:
        out += encoding
    if is_cid and fdselect is not None:
        out += fdselect
    out += charstrings_index
    if is_cid:
        fdarray = cff_index(font_dicts(private_offsets))
        if len(fdarray) != len(fdarray_probe):
            raise AssertionError("the FDArray changed size when it was patched")
        out += fdarray
    for block, subrs in private_blocks:
        out += block
        out += subrs
    return bytes(out)


def cff_otf(label, order, cff_bytes, advances, mapping=None):
    """An `OTTO` face whose `CFF ` table is the bytes given.

    fontTools builds the sfnt - the directory, `head`, `hhea`, `hmtx`, `cmap`,
    `OS/2`, `name`, `post`, `maxp` - and then the `CFF ` table it made is
    replaced. `setupCFF` still runs, and is still worth running: it is what sets
    `maxp` to version 0.5 and makes the flavour `OTTO`, and getting that from the
    reference rather than by hand is the point of building fixtures in the image.
    """
    fb = FontBuilder(UPEM, isTTF=False)
    fb.setupGlyphOrder(order)
    fb.setupCFF(names(label)["psName"],
        {"FullName": names(label)["fullName"], "Weight": "Regular"},
        {name: T2CharStringPen(advances.get(name, 600), None).getCharString()
         for name in order},
        {})
    set_raw_table(fb, "CFF ", cff_bytes)
    # `head`'s own compiler reads the CFF Top DICT's FontBBox for xMin..yMax,
    # and a table it cannot parse is one it cannot read that from. So the box is
    # written here instead, from the same constant the table states, and the
    # recalculation is turned off rather than left to fail.
    fb.font.recalcBBoxes = False
    head = fb.font["head"]
    head.xMin, head.yMin, head.xMax, head.yMax = CFF_FONT_BBOX
    fb.setupHorizontalMetrics({n: (advances.get(n, 600), 0) for n in order})
    fb.setupHorizontalHeader(ascent=ASCENT, descent=DESCENT, lineGap=0)
    set_cmap(fb, [subtable(4, 3, 1, mapping or {})])
    fb.setupOS2(version=4, achVendID=VENDOR, **OS2_FIELDS)
    fb.setupNameTable(names(label), mac=True)
    fb.setupPost(keepGlyphNames=False)
    pin(fb)
    return fb

# How many standard strings CFF predefines, which is where a font's own strings
# start. The same number src/cff/cff_strings.h carries, and from the same place.
GFNT_CFF_STANDARD_STRING_COUNT = len(cffStandardStrings)


def sid(name):
    """A standard string's SID, asked of fontTools rather than remembered.

    Every fixture below names glyphs the charset has to spell as SIDs, and
    writing those numbers down would be writing down a table section 14 says
    comes from an oracle. `cffStandardStrings` is the same list
    `tools/vectors/make_vectors.py` generates the library's copy from.
    """
    return cffStandardStrings.index(name)


def cff_names(order):
    """A format 0 charset naming these glyphs, and the strings it needs.

    A glyph whose name is one of the 391 standard strings is named by its SID;
    anything else goes into the font's own `String` INDEX, which is what a SID
    of 391 or more means. Getting this wrong is quiet: a charset of zeroes names
    every glyph `.notdef`, and fontTools renames the duplicates rather than
    complaining - which is how the first version of these fixtures passed its
    own generation and failed the differential on eleven names.
    """
    strings = []
    sids = []
    for name in order[1:]:
        if name in cffStandardStrings:
            sids.append(cffStandardStrings.index(name))
            continue
        if name not in strings:
            strings.append(name)
        sids.append(GFNT_CFF_STANDARD_STRING_COUNT + strings.index(name))
    return cff_charset_format0(sids), strings


def cff_curves_parts(font_matrix=None, licence_in_notice=False):
    """Every curve operator in the form a pen never writes.

    A `T2CharStringPen` writes `rrcurveto` and `rlineto` and nothing else, so a
    fixture built by one exercises two of the eleven drawing operators. Each
    glyph here is one operator in its hardest form:

      * `hvcurveto` and `vhcurveto` with **three** groups and the trailing fifth
        operand, which belongs to the last group and not to the count's parity;
      * `hhcurveto` and `vvcurveto` with the leading odd operand, which applies
        to the **first curve only** - a reader that applies it to every group
        draws the second one wrong, and a planted defect of exactly that shape
        produced three disagreements in 24,657 fields of real fonts and none at
        all in a thinned run;
      * `rcurveline` and `rlinecurve`, whose operand lists end in a segment of
        the other kind;
      * all four of `flex`, `hflex`, `hflex1` and `flex1`, of which the corpus's
        35 CFF fonts contain not one. `flex1` appears twice, because which axis
        its single trailing operand belongs to depends on which way the whole
        flex travelled, and one glyph can only be one of those.
    """
    order = [".notdef", "space", "hv", "vh", "hh", "vv", "curveline",
             "linecurve", "flex", "hflex", "hflex1", "flex1x", "flex1y"]
    advances = {name: 600 for name in order}
    advances["space"] = 300
    # Each program ends with a leg back down and to the right before its
    # implicit close, which is not decoration: a chain of curves that all run
    # up-and-right encloses a **sliver** with its closing line, and a sliver's
    # extreme row of pixels carries so little coverage that a quarter-pixel
    # horizontal shift decides whether it rounds to nothing. The golden gate
    # asserts that a horizontal offset does not change a glyph's vertical
    # extent, and it is right to: the property is about the rasteriser, and a
    # fixture too thin to state it is a fixture that weakens the gate.
    leg = (250, -500, "rlineto")
    programs = {
        ".notdef": charstring(600, 50, 0, "rmoveto", 500, "hlineto", 700,
            "vlineto", -500, "hlineto", "endchar"),
        "space": charstring(300, "endchar"),
        "hv": charstring(600, 100, 200, "rmoveto",
            50, 60, 70, 80, 90, 100, 110, 120, 130, 140, 150, 160, 25,
            "hvcurveto", *leg, "endchar"),
        "vh": charstring(600, 100, 200, "rmoveto",
            50, 60, 70, 80, 90, 100, 110, 120, 130, 140, 150, 160, 25,
            "vhcurveto", *leg, "endchar"),
        "hh": charstring(600, 100, 200, "rmoveto",
            20, 30, 40, 50, 60, 30, 40, 50, 60, "hhcurveto", *leg, "endchar"),
        "vv": charstring(600, 100, 200, "rmoveto",
            20, 30, 40, 50, 60, 30, 40, 50, 60, "vvcurveto", *leg, "endchar"),
        "curveline": charstring(600, 100, 200, "rmoveto",
            30, 40, 50, 60, 70, 80, 30, -40, 50, -60, 70, -80, 40, 50,
            "rcurveline", *leg, "endchar"),
        "linecurve": charstring(600, 100, 200, "rmoveto",
            40, 50, 60, -30, 30, 40, 50, 60, 70, 80, "rlinecurve", *leg,
            "endchar"),
        "flex": charstring(600, 100, 200, "rmoveto",
            30, 20, 40, 30, 50, 0, 50, 0, 40, -30, 30, -20, 50, "flex",
            *leg, "endchar"),
        "hflex": charstring(600, 100, 200, "rmoveto",
            30, 40, 30, 50, 60, 40, 30, "hflex", *leg, "endchar"),
        "hflex1": charstring(600, 100, 200, "rmoveto",
            30, 20, 40, 30, 50, 60, 40, -30, 50, "hflex1", *leg, "endchar"),
        "flex1x": charstring(600, 100, 200, "rmoveto",
            40, 10, 50, 20, 60, 0, 70, -10, 80, -20, 90, "flex1", *leg,
            "endchar"),
        "flex1y": charstring(600, 100, 200, "rmoveto",
            10, 40, 20, 50, 0, 60, -10, 70, -20, 80, 90, "flex1", *leg,
            "endchar"),
    }
    charset, strings = cff_names(order)
    # The Top DICT's own strings, which this font program needs because it is
    # also written out with no sfnt around it: a bare CFF has no `name` table,
    # and a fixture in someone's font directory has to state its own licence
    # (section 14.5). Both fixtures carry them so that the two files stay
    # byte-identical - that is what makes comparing them a statement about the
    # container rather than about two fonts - and the wrapped one is then also
    # the font that has *both* sources, which is its own question to ask.
    #
    # `Notice` is deliberately left out: the copyright is in `Copyright`, and a
    # reader that only looked at `Notice` should find nothing there.
    label = names("CFF curves")
    base = GFNT_CFF_STANDARD_STRING_COUNT + len(strings)
    # "R\u00e9gular" earns two tests with one string. The \u00e9 is one Latin-1
    # byte (0xE9) that has to come back as two bytes of UTF-8, which is the only
    # place the reader's decoding of a CFF string is exercised. And the `name`
    # table of the wrapped fixture says "Regular" for the same field, so the two
    # sources disagree on purpose: that is what makes it possible to tell which
    # one a face with both of them actually read.
    strings = list(strings) + [label["version"], label["fullName"],
        label["familyName"], "R\u00e9gular", label["copyright"]]
    extra_top = (
        (0, [base]),            # version
        (2, [base + 1]),        # FullName
        (3, [base + 2]),        # FamilyName
        (4, [base + 3]),        # Weight
        # Copyright, or Notice - the two operators say the same kind of thing and
        # a font states one of them, so both spellings need a fixture. A reader
        # that only looked at whichever one it happened to meet first would drop
        # the licence of every font that used the other.
        ((1, [base + 4]) if licence_in_notice else ((12, 0), [base + 4])),
    )
    table = assemble_cff(label["psName"],
        [programs[name] for name in order], charset=charset, strings=strings,
        extra_top=extra_top, font_matrix=font_matrix)
    return order, advances, table


def build_cff_curves(out):
    order, advances, table = cff_curves_parts()
    fb = cff_otf("CFF curves", order, table, advances, {0x20: "space"})
    fb.save(out)


def build_bare_matrix_cff(out):
    """A bare CFF whose `FontMatrix` does not reduce to an em.

    The two axes are scaled differently - 1/2048 across and 1/1024 down - which
    is a font whose charstring coordinates mean something this library would have
    to transform them out of rather than report. Wrapped in an sfnt it would be
    refused for disagreeing with `head.unitsPerEm`; bare there is no `head`, and
    the matrix is the only statement of the em there is, so the refusal has to
    come from reading the matrix itself.

    Both scales are exactly representable in 16.16, which is the point: the
    refusal must come from the matrix not reducing, and not from a reciprocal
    that could not be inverted.
    """
    # The translation components carry the real-number encodings nothing else
    # states: an exponent, and a mantissa longer than a 16.16 can hold. They can
    # live here precisely because this matrix is refused - no value read out of
    # it is used for anything, so a fixture can ask the decoder the awkward
    # questions without also having to mean something by the answers.
    _, _, table = cff_curves_parts(
        font_matrix=(1 / 2048, 0, 0, 1 / 1024,
                     cff_dict_real(text="5E2"),
                     cff_dict_real(text="12345678901234567890")),
        licence_in_notice=True)
    with open(out, "wb") as handle:
        handle.write(table)


def build_bare_cff(out):
    """The same `CFF ` table as `cff-curves.otf`, with no sfnt around it.

    Byte-for-byte the same font program, deliberately: a bare CFF face and the
    wrapped face must draw every glyph identically, and that is only a statement
    about the container if the bytes being read are the same bytes. It is the
    test the synthetic table directory exists to pass.

    A bare CFF is what a PDF `FontFile3` carries and what fontTools' `cffLib`
    writes on its own, and it has no magic number: `01 00` is a version, and the
    two bytes after it are sizes. So this fixture is also the only input that
    can ask whether the loader accepts it for the right reasons.
    """
    _, _, table = cff_curves_parts()
    with open(out, "wb") as handle:
        handle.write(table)


def build_cff_arith(out):
    """The arithmetic operators, which compute the coordinates they draw with.

    Nothing in the corpus uses one, and every one of them is a way for a
    charstring to say a number this library has to get exactly right: `div`
    produces a fraction, the 255 form *is* a fraction, and `put`/`get`,
    `index`, `roll`, `dup` and `exch` move operands around under the drawing
    operator that will read them.

    `random` is the one member of the family this library refuses rather than
    answers, and no fixture carries it: a font whose shape depends on a random
    number cannot be part of design.md section 1's promise that one font at one
    size gives one bitmap for ever.
    """
    order = [".notdef", "space", "divide", "fixed", "stack", "transient",
             "logic"]
    advances = {name: 600 for name in order}
    advances["space"] = 300
    programs = {
        ".notdef": charstring(600, 50, 0, "rmoveto", 500, "hlineto", 700,
            "vlineto", -500, "hlineto", "endchar"),
        "space": charstring(300, "endchar"),
        # 600/5 is 120, and the quotient is what rlineto draws with.
        "divide": charstring(600, 100, 100, "rmoveto", 600, 5, "div", 0,
            "rlineto", 0, 300, "rlineto", -60, 0, "rlineto", "endchar"),
        # Half a unit, in the only form that can say it.
        "fixed": charstring(600, Fixed(100.5), 100, "rmoveto",
            Fixed(200.25), 0, "rlineto", 0, Fixed(300.75), "rlineto",
            "endchar"),
        # dup, exch, drop and index, each leaving exactly what the next
        # drawing operator takes - and tracing a shape with area while they do
        # it, because a fixture whose glyph is two pixels states nothing about
        # the rasteriser that renders it.
        "stack": charstring(600, 100, "dup", "rmoveto",
            400, 0, "rlineto",
            0, 300, "rlineto",
            1, 2, 3, "drop", "rlineto",
            0, -400, "exch", "rlineto",
            40, 50, 1, "index", "drop", "rlineto",
            "endchar"),
        # The transient array, which is the only storage a charstring has.
        "transient": charstring(600, 50, 50, "rmoveto",
            200, 5, "put", 5, "get", 0, "rlineto", 0, 400, "rlineto",
            -100, 0, "rlineto", "endchar"),
        # eq, not, or and ifelse, which a font would use to pick between two
        # shapes and which here pick between two coordinates.
        "logic": charstring(600, 100, 100, "rmoveto",
            1, 1, "eq", 0, "not", "add", 200, "mul", 0, "rlineto",
            300, 200, 1, 2, "ifelse", 0, "exch", "rlineto",
            -200, 0, "rlineto", "endchar"),
    }
    charset, strings = cff_names(order)
    table = assemble_cff(names("CFF arith")["psName"],
        [programs[name] for name in order], charset=charset, strings=strings)
    fb = cff_otf("CFF arith", order, table, advances, {0x20: "space"})
    fb.save(out)


def build_cff_hints(out):
    """Stem hints, and the masks whose width the stem count decides.

    Nothing here is drawn from a hint: there is no hinter (design.md section
    8.5). What the hints decide is **how many bytes to step over**, and a reader
    that miscounts them reads its next operator out of the middle of a mask. So
    the fixture's glyphs are the ways a stem count can be got wrong:

      * an odd operand count on `hstemhm`, which means the first operand is the
        glyph's advance and not a stem edge;
      * ten stems, so the mask is two bytes rather than one;
      * stems declared **inside a subroutine**, which is what makes a mask's
        width unknowable without running the program - the reason
        ::gfnt_charstring_dump() takes the metrics of a run;
      * operands still on the stack at `hintmask`, which are an implicit `vstem`
        declaration and the one place a stem count grows with no stem operator;
      * `cntrmask`, which is the same counting with a different operator;
      * and a width carried on `vstem`, which the specification's list of
        stack-clearing operators that may carry one does not name - an omission
        rather than a rule, and fontTools reads it the same way.
    """
    order = [".notdef", "space", "stems", "masktwo", "maskinsubr", "implicit",
             "cntr", "widthstem"]
    advances = {name: 600 for name in order}
    advances["space"] = 300
    box = (100, 100, "rmoveto", 400, "hlineto", 600, "vlineto", -400,
           "hlineto", "endchar")
    # Five horizontal and five vertical stems: ten hints, so a two-byte mask.
    ten_stems = (100, 20, 150, 20, 200, 20, 250, 20, 300, 20, "hstemhm",
                 100, 20, 150, 20, 200, 20, 250, 20, 300, 20, "vstemhm")
    subrs = [
        # Declares the hints the charstring will mask with, and returns.
        charstring(100, 30, 200, 30, 400, 30, "hstemhm", "return"),
    ]
    programs = {
        ".notdef": charstring(600, 50, 0, "rmoveto", 500, "hlineto", 700,
            "vlineto", -500, "hlineto", "endchar"),
        "space": charstring(300, "endchar"),
        # Five operands on hstemhm: two stems and, first, the advance.
        "stems": charstring(600, 100, 50, 300, 50, "hstemhm",
            100, 50, 300, 50, "vstemhm", "hintmask", ("mask", b"\xf0"), *box),
        "masktwo": charstring(600, *ten_stems, "hintmask",
            ("mask", b"\xaa\xc0"), *box),
        # The subroutine declares three stems; the mask here is one byte wide
        # because of them, and nothing in this charstring says so.
        "maskinsubr": charstring(600, -107, "callsubr", "hintmask",
            ("mask", b"\xe0"), *box),
        # No stem operator at all: the four operands before hintmask are an
        # implicit vstem declaration.
        "implicit": charstring(600, 100, 50, 300, 50, "hintmask",
            ("mask", b"\xc0"), *box),
        "cntr": charstring(600, 100, 50, 300, 50, "hstemhm", 100, 50, 300, 50,
            "vstemhm", "cntrmask", ("mask", b"\xf0"), "hintmask",
            ("mask", b"\x90"), *box),
        # The advance on vstem, which the specification's list omits.
        "widthstem": charstring(600, 100, 50, 300, 50, "vstem", *box),
    }
    charset, strings = cff_names(order)
    table = assemble_cff(names("CFF hints")["psName"],
        [programs[name] for name in order], charset=charset, strings=strings,
        # defaultWidthX 600 and nominalWidthX **0**, so that a stated width of
        # 600 in these programs is an advance of 600 and agrees with `hmtx`.
        # The glyph whose two disagree on purpose is in `cff-subrs.otf`, and a
        # second one here by accident made the stem widths unreadable.
        privates=[({20: 600, 21: 0}, subrs)])
    fb = cff_otf("CFF hints", order, table, advances, {0x20: "space"})
    fb.save(out)


def build_cff_subrs(out):
    """Subroutines, their bias, and the two width defaults.

    The bias is the trap: a Type 2 subroutine number is biased by **how many
    subroutines there are** - 107 below 1240 of them - so subroutine 0 of a
    small font is called by pushing -107. A reader that forgets the bias calls
    the wrong subroutine in every real font, which is why a planted off-by-one
    in it produced 156 disagreements across the corpus.

    The two width defaults are the other half: `defaultWidthX` is the advance of
    a charstring that states none, and `nominalWidthX` is what a stated one is a
    delta from. A reader that swapped them, or that read a delta as an absolute,
    gets every advance in the font wrong and no outline comparison sees it. One
    glyph here states a width its `hmtx` entry disagrees with, because the two
    are different facts and a reader should report both.
    """
    order = [".notdef", "space", "local", "global", "nested", "noreturn",
             "last", "defaultw", "nominalw", "mismatch"]
    advances = {name: 600 for name in order}
    advances["space"] = 300
    advances["defaultw"] = 555
    advances["nominalw"] = 545
    # hmtx says 400 and the charstring says 700.
    advances["mismatch"] = 400
    local = [
        charstring(400, "hlineto", 600, "vlineto", -400, "hlineto", "return"),
        # Calls subroutine 0, which is number -107 once the bias is applied.
        # Written as -106 in the first version of this fixture, which made it
        # call *itself* - and the depth limit refused the glyph, which is the
        # right answer to the wrong program.
        charstring(-107, "callsubr", "return"),
        charstring(200, "hlineto", 300, "vlineto", -200, "hlineto"),
    ]
    global_subrs = [
        charstring(300, "hlineto", 500, "vlineto", -300, "hlineto", "return"),
    ]
    programs = {
        ".notdef": charstring(600, 50, 0, "rmoveto", 500, "hlineto", 700,
            "vlineto", -500, "hlineto", "endchar"),
        "space": charstring(300, "endchar"),
        # Local subroutine 0, which is number -107 once the bias is applied.
        "local": charstring(600, 100, 100, "rmoveto", -107, "callsubr",
            "endchar"),
        "global": charstring(600, 100, 100, "rmoveto", -107, "callgsubr",
            "endchar"),
        # Subroutine 1 calls subroutine 0: one level of nesting, reached by two
        # different biased numbers.
        "nested": charstring(600, 100, 100, "rmoveto", -106, "callsubr",
            "endchar"),
        # Subroutine 2 ends without `return`, which the format allows: running
        # off the end of a subroutine returns to the caller.
        "noreturn": charstring(600, 100, 100, "rmoveto", -105, "callsubr",
            "endchar"),
        # The last subroutine, so that the top of the range is reached as well
        # as the bottom.
        "last": charstring(600, 200, 200, "rmoveto", -105, "callsubr",
            "endchar"),
        # No width on the first stack-clearing operator: the advance is
        # defaultWidthX.
        "defaultw": charstring(100, 100, "rmoveto", 400, "hlineto", 600,
            "vlineto", -400, "hlineto", "endchar"),
        # A delta from nominalWidthX: 500 + 45.
        "nominalw": charstring(45, 100, 100, "rmoveto", 400, "hlineto", 600,
            "vlineto", -400, "hlineto", "endchar"),
        # 500 + 200, which hmtx says is 400.
        "mismatch": charstring(200, 100, 100, "rmoveto", 400, "hlineto", 600,
            "vlineto", -400, "hlineto", "endchar"),
    }
    charset, strings = cff_names(order)
    table = assemble_cff(names("CFF subrs")["psName"],
        [programs[name] for name in order], gsubrs=global_subrs,
        charset=charset, strings=strings,
        privates=[({20: 555, 21: 500}, local)])
    fb = cff_otf("CFF subrs", order, table, advances, {0x20: "space"})
    fb.save(out)


def build_cff_seac(out):
    """An accented character, and the charset and encoding that make it findable.

    `endchar` with four operands draws two *other* glyphs: a base and an accent,
    named by **Standard Encoding code** whatever the font's own encoding says.
    That is three lookups a reader can get wrong independently - the code to a
    name, the name to a SID, the SID to a glyph - and the corpus's 35 CFF fonts
    contain no glyph that uses the construction at all.

    The charset is in format 1, one-glyph ranges, so the range arithmetic is
    exercised by a font whose SIDs are not contiguous; one glyph's name lives in
    the font's own `String` INDEX rather than among the standard strings, which
    is the only way a SID at or above 391 is reached. The encoding is a custom
    one **with a supplement**, which maps a code to a glyph by name rather than
    by position - and which has to be read after the base format's array has
    been walked past.
    """
    order = [".notdef", "space", "A", "acute", "Aacute", "ghoti.alt"]
    advances = {name: 600 for name in order}
    advances["space"] = 300
    programs = {
        ".notdef": charstring(600, 50, 0, "rmoveto", 500, "hlineto", 700,
            "vlineto", -500, "hlineto", "endchar"),
        "space": charstring(300, "endchar"),
        "A": charstring(600, 50, 0, "rmoveto", 250, 700, 250, -700, "rlineto",
            "endchar"),
        "acute": charstring(600, 200, 750, "rmoveto", 100, 150, 60, -150,
            "rlineto", "endchar"),
        # adx 0, ady 40, base 'A' (code 65), accent 'acute' (code 194).
        "Aacute": charstring(600, 0, 40, 65, 194, "endchar"),
        "ghoti.alt": charstring(600, 100, 100, "rmoveto", 300, "hlineto", 300,
            "vlineto", -300, "hlineto", "endchar"),
    }
    # One range per glyph, because these SIDs are not contiguous - and the last
    # is in this font's own String INDEX, which is what a SID of 391 means.
    charset = cff_charset_ranges([(sid("space"), 0), (sid("A"), 0),
        (sid("acute"), 0), (sid("Aacute"), 0), (391, 0)])
    # Codes for glyphs 1..4, and a supplement naming the fifth by SID.
    encoding = cff_encoding_format0([0x20, 0x41, 0xC2, 0xC1],
        supplements=[(0xFF, 391)])
    table = assemble_cff(names("CFF seac")["psName"],
        [programs[name] for name in order],
        strings=["ghoti.alt"], charset=charset, encoding=encoding)
    fb = cff_otf("CFF seac", order, table, advances,
        {0x20: "space", 0x41: "A", 0xC1: "Aacute"})
    fb.save(out)


def build_cff_cid(out):
    """A CID-keyed font: an `FDArray`, an `FDSelect`, and a charset of CIDs.

    Three things change when a Top DICT carries `ROS`, and a reader that handles
    only the first two draws most CID fonts nearly right:

      * the Private DICT is **per glyph**, chosen by `FDSelect` out of the
        `FDArray`, so two glyphs in one font have different local subroutines
        *and* different width defaults;
      * the charset holds **CIDs and not SIDs**, so a reader that looks a glyph's
        name up in the standard strings prints whatever string happens to sit at
        that number;
      * there is no `Encoding` at all - the format forbids one - so a code cannot
        be resolved by anything but a `cmap`.

    `FDSelect` is in format 3 here, the range form, which is what real CID fonts
    use; format 0's array is exercised by a hand-built table in `test_cff.cpp`,
    where a fixture would be 64 kB of one byte per glyph.
    """
    order = [".notdef", "cid.one", "cid.two", "cid.three"]
    advances = {name: 600 for name in order}
    first_subrs = [charstring(400, "hlineto", 600, "vlineto", -400, "hlineto",
        "return")]
    second_subrs = [charstring(200, "hlineto", 300, "vlineto", -200, "hlineto",
        "return")]
    programs = {
        ".notdef": charstring(600, 50, 0, "rmoveto", 500, "hlineto", 700,
            "vlineto", -500, "hlineto", "endchar"),
        # FD 0's subroutine, and FD 0's defaultWidthX.
        "cid.one": charstring(100, 100, "rmoveto", -107, "callsubr", "endchar"),
        # FD 1's subroutine, which is a different shape at the same number.
        "cid.two": charstring(100, 100, "rmoveto", -107, "callsubr", "endchar"),
        "cid.three": charstring(600, 200, 200, "rmoveto", 200, "hlineto", 200,
            "vlineto", -200, "hlineto", "endchar"),
    }
    table = assemble_cff(names("CFF cid")["psName"],
        [programs[name] for name in order],
        strings=["Ghoti", "Fixture"],
        # CIDs, deliberately not 1, 2, 3: a reader that returned the glyph index
        # would agree with a charset that did.
        charset=cff_charset_ranges([(11, 0), (22, 0), (33, 0)], wide=True),
        is_cid=True,
        fdselect=cff_fdselect_format3([(0, 0), (2, 1)], len(order)),
        privates=[({20: 600, 21: 600}, first_subrs),
                  ({20: 480, 21: 500}, second_subrs)])
    fb = cff_otf("CFF cid", order, table, advances, {})
    fb.save(out)


def type1_parts():
    """Type 1 charstrings, inside a CFF that says so.

    `CharstringType 1` is a Top DICT entry the format allows and nobody ships,
    and it is the only way to put a Type 1 *program* where both this library and
    fontTools will read it as one - fontTools has a Type 1 interpreter of its
    own, so the language is compared against a reference rather than against
    this library's expectations.

    That matters because Type 1 is not a dialect of Type 2. It carries its
    advance and side bearing in `hsbw`, its `255` operand is a plain integer
    rather than a 16.16, its subroutine numbers are unbiased, it closes contours
    with `closepath`, it spells the accented character `seac` and corrects the
    accent's position by a side bearing, and it reaches flex and hint
    replacement through `callothersubr` and `pop`. Every one of those is a glyph
    here.
    """
    order = [".notdef", "space", "A", "acute", "Aacute", "flex", "hints",
             "current"]
    advances = {name: 600 for name in order}
    advances["space"] = 300
    # Two drawing subroutines, called with **unbiased** numbers: Type 1 has no
    # bias, which is the difference from Type 2 that a reader sharing one call
    # path gets wrong in both directions at once.
    subrs = [
        charstring(400, "hlineto", 600, "vlineto", -400, "hlineto", "return",
            type1=True),
        charstring(200, "hlineto", 300, "vlineto", -200, "hlineto", "return",
            type1=True),
    ]
    programs = {
        # hsbw: a side bearing of 50 and an advance of 600, and the drawing
        # starts at the side bearing rather than at the origin.
        ".notdef": charstring(50, 600, "hsbw", 0, 0, "rmoveto", 500, "hlineto",
            700, "vlineto", -500, "hlineto", "closepath", "endchar",
            type1=True),
        "space": charstring(0, 300, "hsbw", "endchar", type1=True),
        "A": charstring(50, 600, "hsbw", 0, 0, "rmoveto", 250, 700, "rlineto",
            250, -700, "rlineto", "closepath", "endchar", type1=True),
        "acute": charstring(200, 600, "hsbw", 0, 750, "rmoveto", 100, 150,
            "rlineto", 60, -150, "rlineto", "closepath", "endchar", type1=True),
        # seac: asb adx ady bchar achar. The accent moves by adx corrected for
        # the difference between this charstring's side bearing and asb.
        "Aacute": charstring(50, 600, "hsbw", 200, 0, 40, 65, 194, "seac",
            type1=True),
        # Flex, the way Type 1 spells it: othersubr 1 opens it, seven rmovetos
        # are collected rather than drawn, othersubr 0 closes it and leaves the
        # end point for two pops and a setcurrentpoint.
        # Flex, the way Type 1 spells it. The operand order is the trap:
        # `callothersubr` takes the arguments, then **how many** of them, then
        # **which** OtherSubr - so starting a flex is `0 1 callothersubr` and
        # not `1 0`, which is OtherSubr 0 given one argument and which fontTools
        # answers with an IndexError rather than a flex.
        "flex": charstring(50, 600, "hsbw", 100, 100, "rmoveto",
            0, 1, "callothersubr",
            50, 50, "rmoveto", 0, 2, "callothersubr",
            50, 0, "rmoveto", 0, 2, "callothersubr",
            50, -50, "rmoveto", 0, 2, "callothersubr",
            50, 0, "rmoveto", 0, 2, "callothersubr",
            50, 50, "rmoveto", 0, 2, "callothersubr",
            50, 0, "rmoveto", 0, 2, "callothersubr",
            50, -50, "rmoveto", 0, 2, "callothersubr",
            # The three arguments to OtherSubr 0 are the flex depth and the
            # **absolute coordinates of the final point**, which the seven
            # collected points have already reached: (500, 100) here. Writing
            # anything else makes two references disagree - FreeType and this
            # library leave the point the path actually reached for the two
            # `pop`s, and fontTools leaves these arguments - and no font in the
            # world can tell the readings apart, because in every one of them
            # the two are the same numbers.
            50, 500, 100, 3, 0, "callothersubr", "pop", "pop",
            "setcurrentpoint",
            0, 200, "rlineto", "closepath", "endchar", type1=True),
        # hstem3 and vstem3, and hint replacement: othersubr 3 hands back the
        # subroutine number that `pop callsubr` then calls.
        "hints": charstring(50, 600, "hsbw", 0, 100, 300, 100, 500, 100,
            "hstem3", 0, 100, 300, 100, 500, 100, "vstem3",
            "dotsection", 100, 100, "rmoveto",
            0, 1, 3, "callothersubr", "pop", "callsubr",
            "closepath", "endchar", type1=True),
        # sbw, div and setcurrentpoint: the two-axis side bearing, a quotient,
        # and the one operator that sets an absolute position.
        "current": charstring(50, 0, 600, 0, "sbw", 100, 100, "rmoveto",
            600, 5, "div", 0, "rlineto", 300, 400, "setcurrentpoint",
            200, "hlineto", 0, -300, "rlineto", -150, 0, "rlineto",
            "closepath", "endchar", type1=True),
    }
    return order, advances, subrs, programs


def build_cff_type1(out):
    order, advances, subrs, programs = type1_parts()
    charset, strings = cff_names(order)
    table = assemble_cff(names("CFF type1")["psName"],
        [programs[name] for name in order], charset=charset, strings=strings,
        charstring_type=1, privates=[({20: 600, 21: 600}, subrs)])
    fb = cff_otf("CFF type1", order, table, advances,
        {0x20: "space", 0x41: "A"})
    fb.save(out)


def t1_encrypt(plain, r, pad=4):
    """Adobe's cipher, done by the reference rather than by this script.

    `eexec.encrypt` is fontTools' own, which is the point: a fixture whose
    encryption came from the same understanding as the reader would agree with it
    whether or not either was right. The padding is `pad` bytes the decrypter
    throws away - four for a charstring, four for eexec - and they are fixed
    rather than random so that the fixture is byte-identical run to run.
    """
    from fontTools.misc import eexec

    return eexec.encrypt(b"GHOT"[:pad] + plain, r)[0]


def assemble_type1(label, order, advances, subrs, programs, *, encoding=None,
                   font_matrix=None, len_iv=4, rd="RD", nd="ND", np="NP",
                   dict_count=None):
    """One Type 1 font program's bytes, cleartext plus `eexec`-encrypted half.

    The spellings are parameters because they are what varies between writers and
    what a reader has to accept either way: `RD` or `-|`, `ND` or `|-`, `NP` or
    `|`. A fixture that only used one spelling would leave the other untested and
    half the fonts in the world unread.
    """
    info = names(label)
    matrix = font_matrix or (0.001, 0, 0, 0.001, 0, 0)
    lines = [
        "%%!PS-AdobeFont-1.0: %s 001.000" % info["psName"],
        "%% Generated by tools/fixtures/make_fixtures.py. Not a typeface.",
        "11 dict begin",
        "/FontInfo 6 dict dup begin",
        "/version (001.000) readonly def",
        "/Notice (%s) readonly def" % info["copyright"],
        "/FullName (%s) readonly def" % info["fullName"],
        "/FamilyName (%s) readonly def" % info["familyName"],
        "/Weight (Regular) readonly def",
        "/ItalicAngle 0 def",
        "end readonly def",
        "/FontName /%s def" % info["psName"],
        "/PaintType 0 def",
        "/FontType 1 def",
        "/FontMatrix [%s] readonly def" % " ".join(
            ("%g" % v) for v in matrix),
        "/FontBBox {%d %d %d %d} readonly def" % CFF_FONT_BBOX,
    ]
    if encoding is None:
        lines.append("/Encoding StandardEncoding def")
    else:
        lines.append("/Encoding 256 array")
        lines.append("0 1 255 {1 index exch /.notdef put} for")
        for code in sorted(encoding):
            lines.append("dup %d /%s put" % (code, encoding[code]))
        lines.append("readonly def")
    lines += ["currentdict end", "currentfile eexec"]
    clear = ("\n".join(lines) + "\n").encode("ascii")

    private = [
        "dup /Private 8 dict dup begin",
        "/RD {string currentfile exch readstring pop} executeonly def",
        "/ND {noaccess def} executeonly def",
        "/NP {noaccess put} executeonly def",
        "/lenIV %d def" % len_iv,
        "/BlueValues [] ND",
        "/MinFeature {16 16} ND",
        "/password 5839 def",
    ]
    body = ("\n".join(private) + "\n").encode("ascii")
    if subrs:
        body += ("/Subrs %d array\n" % len(subrs)).encode("ascii")
        for index, program in enumerate(subrs):
            # An empty entry is *not written at all*, which is what makes the
            # numbering sparse: a Type 1 subroutine array is indexed by the
            # numbers charstrings use, and a font may leave gaps in it. Writing
            # them as zero-length entries would fill the gaps and leave the
            # reader's gap handling untested while looking like it had covered it.
            if not program:
                continue
            cipher = t1_encrypt(program, 4330, len_iv)
            body += ("dup %d %d %s " % (index, len(cipher), rd)).encode("ascii")
            body += cipher + (" %s\n" % np).encode("ascii")
        body += b"ND\n"
    body += b"end\n"
    count = dict_count if dict_count is not None else len(order)
    body += ("2 index /CharStrings %d dict dup begin\n" % count).encode("ascii")
    for name in order:
        cipher = t1_encrypt(programs[name], 4330, len_iv)
        body += ("/%s %d %s " % (name, len(cipher), rd)).encode("ascii")
        body += cipher + (" %s\n" % nd).encode("ascii")
    # The closing sequence a real Type 1 font has, and the reason it is not
    # decoration: `definefont` is what puts the font in PostScript's
    # FontDirectory, and fontTools' reader runs the program and then looks for it
    # there - so a fixture without it is a font no reference can read, which is a
    # fixture that cannot be compared against one.
    body += (b"end\nend\nreadonly put\nnoaccess put\n"
             b"dup /FontName get exch definefont pop\n"
             b"mark currentfile closefile\n")

    return clear, t1_encrypt(body, 55665, 4)

def build_type1_big(out):
    """A Type 1 program too large for one allocation, with sparse subroutines.

    Three reallocation paths exist in this reader and no other fixture reaches
    any of them: the buffer the derived program is built in doubles past 4 KB,
    the glyph array doubles past sixteen entries, and the subroutine array grows
    to fit a number rather than a count. A reallocation path no test takes is
    untested rather than working, which is what the coverage report says in as
    many words.

    The subroutine numbers are **sparse** - 0, 1 and 40 - because a Type 1
    subroutine array is indexed by the numbers the charstrings use and a font may
    leave gaps in it. The gap has to read as "not there" rather than as whatever
    was in that memory, and one glyph here calls the high number so the growth is
    not merely allocated but used.
    """
    # Eighty glyphs. The buffer the derived program is built in starts at 4 KB,
    # and the count has had to rise twice: forty glyphs came to 3,832 bytes, and
    # sixty came to 3,863 once the empty subroutine entries stopped being written.
    # Each time it allocated once and left the doubling untested while looking
    # like it had covered it - which is why testType1 asserts the derived length
    # rather than trusting this comment.
    order = [".notdef"] + ["g%02d" % index for index in range(1, 80)]
    advances = {name: 600 for name in order}
    subrs = [charstring(200, "hlineto", 300, "vlineto", -200, "hlineto",
        "return", type1=True)] * 2
    # Sparse: numbers 0 and 1 above, then 40, with nothing between.
    subrs = subrs + [b""] * 38 + [charstring(150, "hlineto", 250, "vlineto",
        -150, "hlineto", "return", type1=True)]
    programs = {".notdef": charstring(50, 600, "hsbw", 0, 0, "rmoveto",
        500, "hlineto", 700, "vlineto", -500, "hlineto", "closepath", "endchar",
        type1=True)}
    for index, name in enumerate(order[1:], start=1):
        # Each glyph draws a slightly different box, and every eighth one calls
        # the sparse high-numbered subroutine.
        if index % 8 == 0:
            programs[name] = charstring(50, 600, "hsbw", 100, 100, "rmoveto",
                40, "callsubr", "closepath", "endchar", type1=True)
        else:
            programs[name] = charstring(50, 600, "hsbw", 10 + index, 20,
                "rmoveto", 300 + index, "hlineto", 400, "vlineto",
                -(300 + index), "hlineto", "closepath", "endchar", type1=True)
    clear, cipher = assemble_type1("Type1 big", order, advances, subrs, programs)
    trailer = (b"0" * 64 + b"\n") * 8 + b"cleartomark\n"
    with open(out, "wb") as handle:
        for kind, payload in ((1, clear), (2, cipher), (1, trailer)):
            handle.write(bytes([0x80, kind]))
            handle.write(len(payload).to_bytes(4, "little"))
            handle.write(payload)
        handle.write(bytes([0x80, 3]))


def build_type1_pfb(out):
    """A Type 1 font program in PFB framing, with the same charstrings as
    `cff-type1.otf`.

    The same programs deliberately: every glyph of the two must draw identically,
    and that is a statement about the container only if the bytes being
    interpreted are the same bytes. What differs is everything around them - PFB
    segment headers, `eexec`, the per-charstring cipher, and a PostScript
    dictionary where a CFF has INDEXes and DICTs.

    PFB is the framing with a *binary* private portion, so this fixture is the
    one that exercises the segment headers and the little-endian lengths in them
    - the only little-endian numbers this library reads, because they belong to a
    PC file format rather than to a font format.
    """
    order, advances, subrs, programs = type1_parts()
    clear, cipher = assemble_type1("Type1 PFB", order, advances, subrs, programs,
        encoding={0x20: "space", 0x41: "A", 0xC1: "Aacute"})
    trailer = (b"0" * 64 + b"\n") * 8 + b"cleartomark\n"
    with open(out, "wb") as handle:
        for kind, payload in ((1, clear), (2, cipher), (1, trailer)):
            handle.write(bytes([0x80, kind]))
            handle.write(len(payload).to_bytes(4, "little"))
            handle.write(payload)
        handle.write(bytes([0x80, 3]))


def build_type1_pfa(out):
    """The same font as ASCII: hex instead of binary, and no PFB framing.

    A PFA is what a PostScript printer was sent down a serial line, so the
    private portion is hex and there are no segment headers at all - the reader
    has to find `eexec` in the text and decide for itself where the data begins.
    This one also spells its operators the other way (`-|`, `|-`, `|` rather than
    `RD`, `ND`, `NP`), states a `/lenIV` of 0 so its charstrings carry no padding,
    declares a `/CharStrings` dictionary smaller than the number of glyphs it then
    defines - which PostScript allows and which a reader that trusted the count
    would truncate - and leaves `/Encoding` as `StandardEncoding` rather than
    building an array.
    """
    order, advances, subrs, programs = type1_parts()
    clear, cipher = assemble_type1("Type1 PFA", order, advances, subrs, programs,
        len_iv=0, rd="-|", nd="|-", np="|", dict_count=2)
    lines = []
    text = cipher.hex().encode("ascii")
    for at in range(0, len(text), 64):
        lines.append(text[at:at + 64])
    with open(out, "wb") as handle:
        handle.write(clear)
        handle.write(b"\n".join(lines) + b"\n")
        handle.write((b"0" * 64 + b"\n") * 8 + b"cleartomark\n")


# --------------------------------------------------------------------------
# The standalone bitmap containers (design.md section 7.1)
# --------------------------------------------------------------------------
#
# One design, written four ways. That is the point of these fixtures rather than
# an accident of laziness: PCF, BDF, PSF 2 and `.hex` have no bytes in common, so
# four faces that read to the *same pixels* is a check on each reader that none of
# them could pass by agreeing with itself. It is the same argument the PFB, PFA and
# CFF spellings of one Type 1 program make.
#
# Every glyph is 8 by 16, because that is the intersection of what the four can
# say: PSF 2 gives every cell one size and `.hex` gives every glyph sixteen rows.
# The per-glyph boxes, the negative bearings and the advance that is not the width
# go in `bitmap-ink.bdf`, where the format can express them.

BITMAP_ROWS = 16
BITMAP_WIDTH = 8
# The baseline the two formats that state one agree on: fourteen rows above it and
# two below, which is what Unifont's own BDFs use for a 16-row cell.
BITMAP_ASCENT = 14
BITMAP_DESCENT = 2


def bitmap_glyph(*rows):
    """Sixteen rows of eight pixels, written as text so the glyph is legible.

    `#` is a set pixel and anything else is clear. A row is padded on the right
    and a short list is padded at the bottom, so that a design can be written
    with only the rows that have something in them.
    """
    out = []
    for row in rows:
        value = 0
        for column in range(BITMAP_WIDTH):
            if column < len(row) and row[column] == "#":
                value |= 0x80 >> column
        out.append(value)
    out.extend([0] * (BITMAP_ROWS - len(out)))
    assert len(out) == BITMAP_ROWS
    return out


def bitmap_parts():
    """The shared design: (codepoint, name, rows) in glyph order.

    Sorted by codepoint, which is the order `.hex` files are written in and the
    order every one of the four containers will therefore list them in - so glyph
    3 is the same glyph in all four, which is what makes comparing them possible
    at all. The glyph order is this library's invention (design.md section 7.1)
    and this is where the invention is pinned.

    Two of these glyphs exist only to catch a bit-order mistake: `L` is the
    leftmost column alone and `R` is the rightmost. Under a reversed bit order each
    reads as the other, and *every other glyph here would still look plausible*.
    """
    return [
        (0x20, "space", bitmap_glyph()),
        (0x2E, "period", bitmap_glyph(
            "", "", "", "", "", "", "", "", "", "",
            "", "###", "###", "###")),
        (0x41, "A", bitmap_glyph(
            "", "  ##", " #  #", " #  #", "#    #", "#    #", "######",
            "#    #", "#    #", "#    #", "#    #")),
        (0x42, "B", bitmap_glyph(
            "", "#####", "#    #", "#    #", "#####", "#    #", "#    #",
            "#    #", "#####")),
        (0x4C, "L", bitmap_glyph(*(["#"] * BITMAP_ROWS))),
        (0x52, "R", bitmap_glyph(*(["       #"] * BITMAP_ROWS))),
        (0x7C, "bar", bitmap_glyph(*(["   ##"] * BITMAP_ROWS))),
        # A checkerboard, whose two row patterns are each other's complement: a
        # byte written to the wrong end of a scan unit swaps them, and nothing
        # else here would show that.
        (0xFE, "checker", bitmap_glyph(*(["# # # # ", " # # # #"] * 8))),
    ]


def hex_line(code, rows):
    """One `.hex` record: the codepoint, a colon, and the rows in hexadecimal."""
    return "%04X:%s" % (code, "".join("%02X" % row for row in rows))


def build_bitmap_hex(out):
    lines = [hex_line(code, rows) for code, _, rows in bitmap_parts()]
    with open(out, "wb") as handle:
        handle.write(("\n".join(lines) + "\n").encode("ascii"))


def build_bitmap_wide_hex(out):
    """The widths `.hex` allows beyond eight pixels.

    A 16-pixel glyph and a 32-pixel one, which is what a CJK Unifont page holds.
    The digit count is the only thing that says how wide a record is, so a reader
    that assumed eight would read the second glyph as garbage and the third as a
    line it could not parse.
    """
    rows16 = [0x8001 for _ in range(BITMAP_ROWS)]
    rows32 = [0x80000001 for _ in range(BITMAP_ROWS)]
    lines = [
        hex_line(0x41, bitmap_parts()[2][2]),
        "%04X:%s" % (0x4E00, "".join("%04X" % row for row in rows16)),
        # Six digits of codepoint, which the format allows and which every
        # astral-plane glyph uses.
        "%06X:%s" % (0x20000, "".join("%08X" % row for row in rows32)),
    ]
    with open(out, "wb") as handle:
        handle.write(("\n".join(lines) + "\n").encode("ascii"))


def build_bitmap_psf(out):
    """PSF 2: the shared design, with a Unicode table.

    The table carries a second codepoint for one glyph and a *sequence* for
    another - a base and a combining mark that one cell stands for. A sequence is
    not a codepoint and cannot be in a codepoint-to-glyph map, so this is the
    fixture that says a reader must walk past one rather than refuse the font.
    """
    parts = bitmap_parts()
    header = struct.pack("<8I", 0x864AB572, 0, 32, 1, len(parts),
        BITMAP_ROWS * ((BITMAP_WIDTH + 7) // 8), BITMAP_ROWS, BITMAP_WIDTH)
    body = b"".join(bytes(rows) for _, _, rows in parts)
    table = b""
    for index, (code, _, _) in enumerate(parts):
        entry = chr(code).encode("utf-8")
        if index == 2:
            # A second character for the same cell, and then A with an acute as a
            # sequence of two codepoints.
            entry += "Α".encode("utf-8")
            entry += b"\xfe" + "Á".encode("utf-8")
        table += entry + b"\xff"
    with open(out, "wb") as handle:
        handle.write(header + body + table)


def build_bitmap_psf1(out):
    """PSF 1: 512 glyphs, because one bit of the mode byte says so.

    Nothing else in the file states the count, which is the trap. The Unicode
    table is UCS-2 little-endian with `0xFFFF` terminators, a different spelling of
    the same idea from version 2's UTF-8.
    """
    parts = bitmap_parts()
    count = 512
    # Mode: 512 glyphs, and a Unicode table with sequences in it.
    header = bytes([0x36, 0x04, 0x01 | 0x02 | 0x04, BITMAP_ROWS])
    cells = []
    for index in range(count):
        if index < len(parts):
            cells.append(bytes(parts[index][2]))
        else:
            # A diagonal, so that the glyphs past the shared design are still
            # distinguishable from each other and from a run of zeros.
            cells.append(bytes((0x80 >> (index % 8)) for _ in range(BITMAP_ROWS)))
    table = b""
    for index in range(count):
        if index < len(parts):
            table += struct.pack("<H", parts[index][0])
        else:
            table += struct.pack("<H", 0xE000 + index)
        if index == 2:
            table += struct.pack("<HHHH", 0x0391, 0xFFFE, 0x0041, 0x0301)
        table += struct.pack("<H", 0xFFFF)
    with open(out, "wb") as handle:
        handle.write(header + b"".join(cells) + table)


BDF_PROPERTIES = [
    ("FOUNDRY", '"Ghoti.io"'),
    ("FAMILY_NAME", '"Ghoti Fixture Bitmap"'),
    ("WEIGHT_NAME", '"Medium"'),
    ("SLANT", '"R"'),
    ("SETWIDTH_NAME", '"Normal"'),
    ("PIXEL_SIZE", "16"),
    ("POINT_SIZE", "160"),
    ("RESOLUTION_X", "75"),
    ("RESOLUTION_Y", "75"),
    ("SPACING", '"C"'),
    ("AVERAGE_WIDTH", "80"),
    ("CHARSET_REGISTRY", '"ISO10646"'),
    ("CHARSET_ENCODING", '"1"'),
    ("FONT_ASCENT", str(BITMAP_ASCENT)),
    ("FONT_DESCENT", str(BITMAP_DESCENT)),
    ("DEFAULT_CHAR", "32"),
    ("COPYRIGHT", '"Copyright 2026 Corey Pennycuff. LGPL-3.0-only."'),
]

BDF_XLFD = ("-Ghoti.io-Ghoti Fixture Bitmap-Medium-R-Normal--16-160-75-75-C-80"
            "-ISO10646-1")


def bdf_char(name, code, width, height, offset_x, offset_y, advance, rows):
    """One STARTCHAR block. `rows` is one integer per row, MSB leftmost."""
    stride = (width + 7) // 8
    out = ["STARTCHAR %s" % name, "ENCODING %d" % code,
           "SWIDTH %d 0" % (advance * 1000 // 16), "DWIDTH %d 0" % advance,
           "BBX %d %d %d %d" % (width, height, offset_x, offset_y), "BITMAP"]
    for row in rows[:height]:
        out.append(("%%0%dX" % (stride * 2)) % row)
    out.append("ENDCHAR")
    return out


def bdf_file(chars, *, box=(BITMAP_WIDTH, BITMAP_ROWS, 0, -BITMAP_DESCENT),
            properties=BDF_PROPERTIES):
    out = ["STARTFONT 2.1", "FONT %s" % BDF_XLFD,
           "SIZE 16 75 75",
           "FONTBOUNDINGBOX %d %d %d %d" % box,
           "STARTPROPERTIES %d" % len(properties)]
    out.extend("%s %s" % pair for pair in properties)
    out.append("ENDPROPERTIES")
    out.append("CHARS %d" % len(chars))
    for char in chars:
        out.extend(char)
    out.append("ENDFONT")
    return ("\n".join(out) + "\n").encode("ascii")


def build_bitmap_bdf(out):
    chars = [bdf_char(name, code, BITMAP_WIDTH, BITMAP_ROWS, 0, -BITMAP_DESCENT,
                 BITMAP_WIDTH, rows)
             for code, name, rows in bitmap_parts()]
    with open(out, "wb") as handle:
        handle.write(bdf_file(chars))


def build_bitmap_ink_bdf(out):
    """What only BDF and PCF can say: a box per glyph.

    Five things here that the shared design cannot carry, each of which a reader
    gets wrong in its own way:

      * a **negative x offset**, a glyph that hangs left of the pen;
      * a **descender**, whose box bottom is below the baseline, which is the test
        that `BBX`'s y is read as the bottom and not the top;
      * an **advance narrower than the box**, and one wider;
      * `ENCODING -1`, a glyph with no character, which must still be a glyph;
      * an **empty box**, `BBX 0 0 0 0` with no rows at all, which is what a space
        looks like when a writer does not pad it.
    """
    chars = [
        bdf_char("space", 0x20, 0, 0, 0, 0, 8, []),
        # A 12-pixel box drawn from two bytes a row, hanging two pixels left.
        #
        # **Its first row sets the four bits past the glyph's width**, which the
        # format leaves undefined and a writer is entitled to leave as rubbish.
        # A reader that carries them through makes two faces of one design compare
        # unequal - and without a row like this one here, the code that clears them
        # can be deleted and every test still passes, which is how it was found.
        bdf_char("Jhook", 0x4A, 12, 14, -2, -4, 10, [0xFFFF] + [0x0300] * 13),
        # A descender: the box bottom is four rows below the baseline.
        bdf_char("p", 0x70, 8, 12, 1, -4, 9, [0xFC] + [0x84] * 11),
        # An advance wider than the glyph, which is what a spacing accent has.
        bdf_char("wide", 0x57, 4, 4, 0, 10, 12, [0xF0, 0x90, 0x90, 0xF0]),
        # No character at all.
        bdf_char("unencoded", -1, 8, 8, 0, 0, 8, [0xAA, 0x55] * 4),
    ]
    with open(out, "wb") as handle:
        handle.write(bdf_file(chars, box=(12, 18, -2, -4)))


# PCF's table types, as its table of contents states them.
PCF_PROPERTIES = 1
PCF_ACCELERATORS = 2
PCF_METRICS = 4
PCF_BITMAPS = 8
PCF_INK_METRICS = 16
PCF_BDF_ENCODINGS = 32
PCF_SWIDTHS = 64
PCF_GLYPH_NAMES = 128
PCF_BDF_ACCELERATORS = 256

PCF_COMPRESSED_METRICS = 0x00000100


def pcf_pack(order, fmt, *values):
    """Pack in the byte order a table's format word declared."""
    return struct.pack(("<" if order == "lsb" else ">") + fmt, *values)


def pcf_format(*, pad=1, bit_msb=True, byte_msb=True, scan=1, base=0):
    """The format word: a shape in the high bytes, a bit layout in the low one."""
    pad_index = {1: 0, 2: 1, 4: 2, 8: 3}[pad]
    scan_index = {1: 0, 2: 1, 4: 2}[scan]
    return (base | pad_index | (0x04 if byte_msb else 0) |
            (0x08 if bit_msb else 0) | (scan_index << 4))


def pcf_rows(rows, width, *, pad, bit_msb, byte_msb, scan):
    """One glyph's rows, laid out the way a format word says.

    The inverse of what `src/bitmap/bitmap.c` does on the way in, and written as
    the inverse deliberately: a fixture whose layout came from the same
    understanding as the reader would agree with it whether or not either was
    right. Both operations are their own inverse, so the two sides look alike -
    what they must not share is the *decision* of when to apply them, and that is
    spelled out in both places from the format's own rules.
    """
    canonical = (width + 7) // 8
    row_bytes = ((canonical + pad - 1) // pad) * pad
    out = bytearray()
    for row in rows:
        data = bytearray(row.to_bytes(canonical, "big") + b"\x00" * (row_bytes - canonical))
        if not bit_msb:
            data = bytearray(int("{:08b}".format(byte)[::-1], 2) for byte in data)
        if (byte_msb != bit_msb) and scan > 1:
            for at in range(0, row_bytes, scan):
                data[at:at + scan] = data[at:at + scan][::-1]
        out += data
    return bytes(out)


def pcf_metric(order, left, right, width, ascent, descent):
    return pcf_pack(order, "hhhhhH", left, right, width, ascent, descent, 0)


def pcf_table(kind, payload):
    """A table plus the 4-byte alignment X pads every table to."""
    padding = (-len(payload)) % 4
    return (kind, payload + b"\x00" * padding)


def build_pcf(out, *, pad=1, bit_msb=True, byte_msb=True, scan=1,
              compressed=False, ink=False):
    parts = bitmap_parts()
    order = "msb" if byte_msb else "lsb"
    fmt = pcf_format(pad=pad, bit_msb=bit_msb, byte_msb=byte_msb, scan=scan)
    plain = pcf_format(pad=pad, bit_msb=bit_msb, byte_msb=byte_msb, scan=scan)
    tables = []

    # Properties. Nine bytes each, then padding computed from the *count*, then
    # the string block's length and the strings themselves.
    props = [("FAMILY_NAME", "Ghoti Fixture Bitmap"), ("WEIGHT_NAME", "Medium"),
             ("FONT_NAME", BDF_XLFD),
             ("COPYRIGHT", "Copyright 2026 Corey Pennycuff. LGPL-3.0-only."),
             ("CHARSET_REGISTRY", "ISO10646"), ("CHARSET_ENCODING", "1"),
             ("PIXEL_SIZE", 16), ("DEFAULT_CHAR", 0x20)]
    strings = bytearray()
    offsets = {}
    for name, value in props:
        for text in (name, value) if isinstance(value, str) else (name,):
            if text not in offsets:
                offsets[text] = len(strings)
                strings += text.encode("ascii") + b"\x00"
    payload = pcf_pack("lsb", "I", plain) + pcf_pack(order, "I", len(props))
    for name, value in props:
        if isinstance(value, str):
            payload += pcf_pack(order, "I", offsets[name]) + b"\x01"
            payload += pcf_pack(order, "I", offsets[value])
        else:
            payload += pcf_pack(order, "I", offsets[name]) + b"\x00"
            payload += pcf_pack(order, "I", value)
    payload += b"\x00" * ((-len(props)) % 4)
    payload += pcf_pack(order, "I", len(strings)) + bytes(strings)
    tables.append(pcf_table(PCF_PROPERTIES, payload))

    # The accelerators, which state the baseline.
    accel = pcf_pack("lsb", "I", plain)
    accel += bytes([1, 1, 0, 1, 1, 0, 0, 0])
    accel += pcf_pack(order, "iii", BITMAP_ASCENT, BITMAP_DESCENT, 0)
    accel += pcf_metric(order, 0, BITMAP_WIDTH, BITMAP_WIDTH, BITMAP_ASCENT,
        BITMAP_DESCENT)
    accel += pcf_metric(order, 0, BITMAP_WIDTH, BITMAP_WIDTH, BITMAP_ASCENT,
        BITMAP_DESCENT)
    tables.append(pcf_table(PCF_BDF_ACCELERATORS, accel))

    # Metrics, compressed or not. The two are different table shapes rather than
    # two encodings of one: the count is sixteen bits in the compressed form.
    metrics_format = plain | (PCF_COMPRESSED_METRICS if compressed else 0)
    payload = pcf_pack("lsb", "I", metrics_format)
    if compressed:
        payload += pcf_pack(order, "H", len(parts))
        for _ in parts:
            payload += bytes([0x80, 0x80 + BITMAP_WIDTH, 0x80 + BITMAP_WIDTH,
                0x80 + BITMAP_ASCENT, 0x80 + BITMAP_DESCENT])
    else:
        payload += pcf_pack(order, "I", len(parts))
        for _ in parts:
            payload += pcf_metric(order, 0, BITMAP_WIDTH, BITMAP_WIDTH,
                BITMAP_ASCENT, BITMAP_DESCENT)
    tables.append(pcf_table(PCF_METRICS, payload))

    if ink:
        # The ink metrics: the same glyphs' *drawn* extents rather than their
        # boxes. Nothing here reads them, and a file that carries them is what
        # proves the directory skips a table nobody asked for rather than
        # stumbling over it.
        payload = pcf_pack("lsb", "I", plain) + pcf_pack(order, "I", len(parts))
        for _ in parts:
            payload += pcf_metric(order, 0, BITMAP_WIDTH - 1, BITMAP_WIDTH,
                BITMAP_ASCENT - 1, BITMAP_DESCENT)
        tables.append(pcf_table(PCF_INK_METRICS, payload))

    # Bitmaps: an offset per glyph, then the four block sizes - one per padding -
    # of which only the one this format names is true of this file.
    data = bytearray()
    starts = []
    for _, _, rows in parts:
        starts.append(len(data))
        data += pcf_rows(rows, BITMAP_WIDTH, pad=pad, bit_msb=bit_msb,
            byte_msb=byte_msb, scan=scan)
    payload = pcf_pack("lsb", "I", plain) + pcf_pack(order, "I", len(parts))
    for start in starts:
        payload += pcf_pack(order, "I", start)
    row_bytes = {}
    for candidate in (1, 2, 4, 8):
        canonical = (BITMAP_WIDTH + 7) // 8
        row_bytes[candidate] = ((canonical + candidate - 1) // candidate) * candidate
    for candidate in (1, 2, 4, 8):
        payload += pcf_pack(order, "I",
            row_bytes[candidate] * BITMAP_ROWS * len(parts))
    payload += bytes(data)
    tables.append(pcf_table(PCF_BITMAPS, payload))

    # The encodings, which map positions in the font's own charset. This one says
    # ISO10646-1, so they are codepoints.
    low = [code for code, _, _ in parts]
    payload = pcf_pack("lsb", "I", plain)
    payload += pcf_pack(order, "HHHHH", min(low), max(low), 0, 0, 0x20)
    for code in range(min(low), max(low) + 1):
        glyph = 0xFFFF
        for index, (candidate, _, _) in enumerate(parts):
            if candidate == code:
                glyph = index
        payload += pcf_pack(order, "H", glyph)
    tables.append(pcf_table(PCF_BDF_ENCODINGS, payload))

    # The scalable widths, in thousandths of an em: another table nothing here
    # reads and every real PCF carries.
    payload = pcf_pack("lsb", "I", plain) + pcf_pack(order, "I", len(parts))
    for _ in parts:
        payload += pcf_pack(order, "I", BITMAP_WIDTH * 1000 // BITMAP_ROWS)
    tables.append(pcf_table(PCF_SWIDTHS, payload))

    # The glyph names.
    names_block = bytearray()
    name_offsets = []
    for _, name, _ in parts:
        name_offsets.append(len(names_block))
        names_block += name.encode("ascii") + b"\x00"
    payload = pcf_pack("lsb", "I", plain) + pcf_pack(order, "I", len(parts))
    for offset in name_offsets:
        payload += pcf_pack(order, "I", offset)
    payload += pcf_pack(order, "I", len(names_block)) + bytes(names_block)
    tables.append(pcf_table(PCF_GLYPH_NAMES, payload))

    # The header and the table of contents, whose integers are always
    # least-significant-byte first whatever the tables say.
    header = b"\x01fcp" + struct.pack("<i", len(tables))
    at = len(header) + 16 * len(tables)
    toc = b""
    body = b""
    for kind, payload in tables:
        formats = {
            PCF_METRICS: metrics_format,
        }
        toc += struct.pack("<iiii", kind, formats.get(kind, plain),
            len(payload), at)
        body += payload
        at += len(payload)
    with open(out, "wb") as handle:
        handle.write(header + toc + body)


def build_bitmap_pcf(out):
    build_pcf(out)


def build_bitmap_lsb_pcf(out):
    """The same design with every number and every bit the other way round.

    Bits least-significant-first, bytes least-significant-first - so every table's
    integers are little-endian too - a two-byte scan unit, rows padded to four
    bytes, and the metrics compressed. It must read to exactly the same glyphs as
    `bitmap.pcf`.

    **Its scan unit is inert and that is the point of `bitmap-swap.pcf`.** The
    bytes of a scan unit are reversed only when the bit order and the byte order
    *differ*; here they agree, so a reader that never implemented the reversal
    reads this file correctly. Discovering that took measuring the bytes: the
    fixture was written to cover the swap and covered everything except it.
    """
    build_pcf(out, pad=4, bit_msb=False, byte_msb=False, scan=2,
        compressed=True, ink=True)


def build_bitmap_swap_pcf(out):
    """The layout where a scan unit's bytes have to be reversed.

    Bits least-significant-first and bytes **most**, with a four-byte scan unit:
    the one combination of PCF's four independent layout choices where a reader
    has to undo two transformations rather than one, and the only file here that
    can tell a reader which way round it does them.

    Pillow cannot read this one - `PcfFontFile._load_bitmaps` leaves the byte-order
    bit commented out in its own source - so `bitmap_diff.py` counts it as a layout
    the reference declines rather than as a disagreement. What says this library is
    right is the other three containers of the same design: libXfont and FreeType
    both reverse here, and a reading that did not would not reproduce the design.
    """
    build_pcf(out, pad=4, bit_msb=False, byte_msb=True, scan=4)


def gzip_bytes(data):
    """Gzip @p data reproducibly.

    `mtime=0` because the default is the clock, and RFC 1952 puts it in the header:
    without it these fixtures would differ on every run and `check-fixtures` would
    fail on the second one. The compression level is stated rather than defaulted
    for the same reason, and **the zlib doing the work is the image's**, which is
    what makes byte-comparing a regeneration mean anything (section 14.7).
    """
    import gzip

    return gzip.compress(data, compresslevel=9, mtime=0)


def build_bitmap_gz_pcf(out):
    """The PCF that `bitmap.pcf` is, in the wrapper PCFs actually ship in.

    All 234 PCFs in the oracle image are `.pcf.gz`; a reader that cannot undo gzip
    reads almost no PCF that exists. It must produce byte-identical glyphs to
    `bitmap.pcf`, which is the whole assertion - the wrapper decides nothing about
    what the font is.
    """
    build_pcf(out + ".plain")
    with open(out + ".plain", "rb") as handle:
        plain = handle.read()
    os.remove(out + ".plain")
    with open(out, "wb") as handle:
        handle.write(gzip_bytes(plain))


def build_type1_gz_pfb(out):
    """A gzipped Type 1 program: two derivations, stacked.

    Nobody ships a `.pfb.gz`, and this fixture is not about that. It is about the
    *composition*: the gzip layer replaces the face's bytes, and then `eexec`
    replaces them again - so each layer has to free the one it consumed after
    reading it, and the first version of that code leaked the gzip blob because the
    Type 1 path assigned ownership directly. It must read identically to
    `type1.pfb`.
    """
    build_type1_pfb(out + ".plain")
    with open(out + ".plain", "rb") as handle:
        plain = handle.read()
    os.remove(out + ".plain")
    with open(out, "wb") as handle:
        handle.write(gzip_bytes(plain))

def set_raw_table(fb, tag, data):
    """Put bytes in the font under @p tag, parsed by nobody.

    fontTools has an object model for every table it knows, and a fixture whose
    subject is the *bytes* needs them to reach the file unexamined - so the table
    is a ::DefaultTable, whose compile() hands back what it was given. The bare
    CFF fixture does the same thing for the same reason.
    """
    table = DefaultTable(tag)
    table.data = data
    fb.font[tag] = table


# ---------------------------------------------------------------------------
# EBLC and EBDT: bitmap strikes inside an sfnt (design.md section 7.5).
#
# Written byte by byte rather than through fontTools, for the reason the PCF
# builder gives: what these fixtures exercise is the *cross-product* of index
# subtable format and image format, and a writer picks one cell of it. fontTools
# then reads the result back, which is what makes the bytes checkable without
# this file being the only opinion about them.
#
# Every offset in EBLC is from the start of EBLC, and every offset in EBLC's
# subtable headers is into EBDT. Keeping those two straight is the whole of the
# bookkeeping here, and the helpers below take the base they are relative to as
# an argument so that a caller cannot forget which it meant.

# The strikes of strikes.ttf, in ppem. Ten, twelve and sixteen so that a
# nearest-strike search has work to do: 13 is unambiguously 12's, 20 is 16's, and
# **11 is a tie** between 10 and 12, which is the case that makes the tie rule a
# rule rather than an accident of list order.
STRIKE_PPEMS = (10, 12, 16)


def eblc_line_metrics(ascender, descender, width_max):
    """One `sbitLineMetrics`: twelve bytes, the last two reserved."""
    return struct.pack(">bbBbbbbbbbbb", ascender, descender, width_max,
                       1, 0, 0,     # caret slope 1/0, offset 0: upright
                       0, 0,        # minOriginSB, minAdvanceSB
                       ascender, descender,  # maxBeforeBL, minAfterBL
                       0, 0)        # the two reserved bytes


def eblc_big_metrics(height, width, bearing_x, bearing_y, advance):
    """`BigGlyphMetrics`: eight bytes, horizontal then vertical.

    The vertical three are written as zero rather than invented. Nothing in the
    population states them for a horizontal strike, and a fixture that made them
    up would be the only evidence for whatever a reader did with them.
    """
    return struct.pack(">BBbbBbbB", height, width, bearing_x, bearing_y,
                       advance, 0, 0, 0)


def eblc_bit_rows(rows, width):
    """Rows packed **bit-aligned**: the next row starts at the next bit.

    Image formats 2, 5, 7 and 9 are laid out this way and 1, 6 and 8 are
    byte-aligned, which is the single axis that makes two otherwise identical
    glyph records different bytes. A w x h glyph is ceil(w*h/8) bytes here and
    h*ceil(w/8) there.
    """
    bits = []
    for row in rows:
        for column in range(width):
            bits.append(1 if column < len(row) and row[column] == "#" else 0)
    out = bytearray((len(bits) + 7) // 8)
    for index, bit in enumerate(bits):
        if bit:
            out[index // 8] |= 0x80 >> (index % 8)
    return bytes(out)


def strike_art(ppem, glyph):
    """One glyph's pixels at one strike, as text.

    A box with a bar across it, and the bar's row is the glyph index - so no two
    glyphs of a strike are the same pixels, and no two strikes of a glyph are
    either, which is what lets a test say *which* strike answered rather than
    only that one did.
    """
    width = ppem - 4
    height = ppem - 4
    rows = []
    for y in range(height):
        if y == 0 or y == height - 1:
            rows.append("#" * width)
        elif y == 1 + (glyph % max(1, height - 2)):
            rows.append("#" * width)
        else:
            rows.append("#" + "." * (width - 2) + "#")
    return rows


def build_strikes(out):
    """Three strikes of four glyphs, index format 2 with image format 5.

    **This pair is 97.8% of the real population** - 54,136 of the 55,356 index
    subtables in the two Debian packages that carry this table at all - so it is
    the one a fixture owes first. It is also the cheapest: constant metrics in
    EBLC, bit-aligned rows in EBDT, and no per-glyph offsets anywhere.

    Glyph 0 is **left out of every strike**. A strike is sparse over the face's
    glyph count - unlike a standalone container, where the file is the glyph list
    - and a glyph absent from a strike is absent rather than empty. Starting at
    glyph 1 is what makes that case exist in a fixture.
    """
    fb = truetype("Strikes", unicode_cmap())
    first, last = 1, len(GLYPH_ORDER) - 1

    ebdt = bytearray(struct.pack(">I", 0x00020000))
    size_tables = bytearray()
    index_regions = bytearray()
    # Where the first indexSubTableArray goes: after the header and the three
    # bitmapSizeTables, which is a position this loop has to know before it has
    # written them.
    base = 8 + 48 * len(STRIKE_PPEMS)

    for ppem in STRIKE_PPEMS:
        width = height = ppem - 4
        image_size = (width * height + 7) // 8
        data_offset = len(ebdt)
        for glyph in range(first, last + 1):
            packed = eblc_bit_rows(strike_art(ppem, glyph), width)
            assert len(packed) == image_size, (len(packed), image_size)
            ebdt += packed

        # One index subtable covering the whole range: array entry first, then
        # the subtable it points at.
        array = struct.pack(">HHI", first, last, 8)
        subtable = (struct.pack(">HHI", 2, 5, data_offset)
                    + struct.pack(">I", image_size)
                    + eblc_big_metrics(height, width, 1, height, width + 2))
        region = array + subtable

        size_tables += (struct.pack(">IIII",
                            base + len(index_regions), len(region), 1, 0)
                        + eblc_line_metrics(height, -1, width + 2)
                        + eblc_line_metrics(0, 0, 0)
                        + struct.pack(">HHBBBb", first, last, ppem, ppem, 1, 1))
        index_regions += region

    eblc = (struct.pack(">II", 0x00020000, len(STRIKE_PPEMS))
            + bytes(size_tables) + bytes(index_regions))

    set_raw_table(fb, "EBLC", eblc)
    set_raw_table(fb, "EBDT", bytes(ebdt))
    fb.save(out)


# The format cross-product. Index subtable format and image format are
# independent axes - one says how to find a glyph's bytes, the other says what
# those bytes are - and the real population exercises two cells of the grid out of
# the thirty-five that exist. Every other cell's only evidence is here.

STRIKE_FORMAT_ORDER = [".notdef"] + ["g%02d" % i for i in range(1, 13)]


def eblc_small_metrics(height, width, bearing_x, bearing_y, advance):
    """`SmallGlyphMetrics`: five bytes, no vertical pair."""
    return struct.pack(">BBbbB", height, width, bearing_x, bearing_y, advance)


def eblc_byte_rows(rows, width):
    """Rows packed **byte-aligned**: each row starts on a byte."""
    stride = (width + 7) // 8
    out = bytearray()
    for row in rows:
        packed = bytearray(stride)
        for column in range(width):
            if column < len(row) and row[column] == "#":
                packed[column // 8] |= 0x80 >> (column % 8)
        out += packed
    return bytes(out)


def format_art(glyph, width, height):
    """A glyph distinctive in both axes: a left edge, and a bar at row glyph % h."""
    rows = []
    for y in range(height):
        if y == glyph % height:
            rows.append("#" * width)
        else:
            rows.append("#" + "." * (width - 1))
    return rows


def build_strike_formats(out):
    """One strike, six index/image format pairings, one subtable each.

    Twelve glyphs in six pairs, so each cell of the grid owns a glyph range and a
    test can name which pairing it is asserting. All six draw the **same design**
    at the same size, which is the point: the bytes differ in every case and the
    pixels must not, so a reader that gets an offset or an alignment wrong produces
    a visible difference rather than a plausible one.

    The pairings, and why each is here:

      glyphs 1-2    index 1, image 1   4-byte offsets, small metrics, byte-aligned
      glyphs 3-4    index 1, image 2   the same with **bit-aligned** rows
      glyphs 5-6    index 3, image 6   2-byte offsets, big metrics, byte-aligned
      glyphs 7-8    index 1, image 7   big metrics, bit-aligned
      glyphs 9-10   index 4, image 2   a **sparse** glyph list, and glyph 10 absent
      glyphs 11-12  index 5, image 5   sparse *and* constant metrics

    Image format 6 is the one `mona.ttf` uses and fontTools cannot read; index
    format 3 and the sparse formats appear nowhere in Debian at all.
    """
    # **Eleven by seven, and the width is the whole point.** At any width that is a
    # multiple of eight, bit-aligned and byte-aligned rows are the *same bytes* - so
    # a fixture of 8-pixel glyphs cannot tell a reader that routed image format 2 to
    # the byte-aligned path from one that implemented the distinction. The first
    # version of this fixture was 8 wide and a mutation swapping those two paths
    # passed every test. Eleven bits a row means the second row starts mid-byte, and
    # seven rows means the glyph does not end on one either.
    width = 11
    height = 7
    advance = width + 2
    # Its own cmap, over its own glyph names: unicode_cmap() names space, A and B,
    # which this repertoire does not have.
    mapping = {0x41 + i: name
               for i, name in enumerate(STRIKE_FORMAT_ORDER[1:])}
    fb = truetype("Strike Formats", [subtable(4, 3, 1, mapping)],
                  order=STRIKE_FORMAT_ORDER,
                  glyphs={name: raw_glyph([]) for name in STRIKE_FORMAT_ORDER},
                  advances={name: 600 for name in STRIKE_FORMAT_ORDER})

    ebdt = bytearray(struct.pack(">I", 0x00020000))
    arrays = []
    bodies = []

    def art(glyph):
        return format_art(glyph, width, height)

    def small(glyph, aligned):
        rows = (eblc_byte_rows(art(glyph), width) if aligned
                else eblc_bit_rows(art(glyph), width))
        return eblc_small_metrics(height, width, 1, height, advance) + rows

    def big(glyph, aligned):
        rows = (eblc_byte_rows(art(glyph), width) if aligned
                else eblc_bit_rows(art(glyph), width))
        return eblc_big_metrics(height, width, 1, height, advance) + rows

    # --- glyphs 1-2: index 1, image 1. Offsets are from imageDataOffset, so the
    # first is always 0 and the last is a sentinel past the final glyph.
    data_at = len(ebdt)
    offsets = [0]
    for glyph in (1, 2):
        ebdt += small(glyph, True)
        offsets.append(len(ebdt) - data_at)
    bodies.append((1, 2, struct.pack(">HHI", 1, 1, data_at)
                   + b"".join(struct.pack(">I", o) for o in offsets)))

    # --- glyphs 3-4: index 1, image 2. The same index format over bit-aligned
    # rows, which is the pair that isolates the alignment axis.
    data_at = len(ebdt)
    offsets = [0]
    for glyph in (3, 4):
        ebdt += small(glyph, False)
        offsets.append(len(ebdt) - data_at)
    bodies.append((3, 4, struct.pack(">HHI", 1, 2, data_at)
                   + b"".join(struct.pack(">I", o) for o in offsets)))

    # --- glyphs 5-6: index 3, image 6. Offset16 rather than Offset32, and the
    # array is padded to a long-word boundary when the count is odd.
    data_at = len(ebdt)
    offsets = [0]
    for glyph in (5, 6):
        ebdt += big(glyph, True)
        offsets.append(len(ebdt) - data_at)
    body = struct.pack(">HHI", 3, 6, data_at) + b"".join(
        struct.pack(">H", o) for o in offsets)
    if len(offsets) % 2:
        body += b"\x00\x00"
    bodies.append((5, 6, body))

    # --- glyphs 7-8: index 1, image 7.
    data_at = len(ebdt)
    offsets = [0]
    for glyph in (7, 8):
        ebdt += big(glyph, False)
        offsets.append(len(ebdt) - data_at)
    bodies.append((7, 8, struct.pack(">HHI", 1, 7, data_at)
                   + b"".join(struct.pack(">I", o) for o in offsets)))

    # --- glyphs 9-10: index 4, image 2. The range covers both and the list names
    # only glyph 9, so glyph 10 is **in range and absent** - the one state no
    # non-sparse format can express, and the reason GFNT_BitmapRecord has a
    # `present` field at all.
    data_at = len(ebdt)
    ebdt += small(9, False)
    pairs = [(9, 0), (0xFFFF, len(ebdt) - data_at)]
    bodies.append((9, 10, struct.pack(">HHI", 4, 2, data_at)
                   + struct.pack(">I", 1)
                   + b"".join(struct.pack(">HH", g, o) for g, o in pairs)))

    # --- glyphs 11-12: index 5, image 5. Sparse *and* constant: the position in
    # the glyph list multiplies imageSize, not the glyph's distance from
    # firstGlyphIndex - which for a list that skips a glyph are different numbers.
    data_at = len(ebdt)
    image_size = (width * height + 7) // 8
    for glyph in (11, 12):
        packed = eblc_bit_rows(art(glyph), width)
        assert len(packed) == image_size, (len(packed), image_size)
        ebdt += packed
    body = (struct.pack(">HHI", 5, 5, data_at)
            + struct.pack(">I", image_size)
            + eblc_big_metrics(height, width, 1, height, advance)
            + struct.pack(">I", 2)
            + struct.pack(">HH", 11, 12))
    bodies.append((11, 12, body))

    # The indexSubTableArray, then the bodies it points at. Every
    # additionalOffsetToIndexSubtable is from the start of the array.
    array_bytes = len(bodies) * 8
    cursor = array_bytes
    for first, last, body in bodies:
        arrays.append(struct.pack(">HHI", first, last, cursor))
        cursor += len(body)
    region = b"".join(arrays) + b"".join(body for _, _, body in bodies)

    base = 8 + 48
    size_table = (struct.pack(">IIII", base, len(region), len(bodies), 0)
                  + eblc_line_metrics(height, -1, advance)
                  + eblc_line_metrics(0, 0, 0)
                  + struct.pack(">HHBBBb", 1, 12, 12, 12, 1, 1))
    eblc = struct.pack(">II", 0x00020000, 1) + size_table + region

    set_raw_table(fb, "EBLC", eblc)
    set_raw_table(fb, "EBDT", bytes(ebdt))
    fb.save(out)


STRIKE_COMPOSITE_ORDER = [".notdef"] + ["g%02d" % i for i in range(1, 10)]


def eblc_components(parts):
    """`numComponents` and an `EbdtComponent` each: glyph id, then two offsets.

    The offsets are **destination pixel coordinates** - a column from the left
    edge of the composite's own box and a row down from its top - and not the
    component's bearings, which the specification does not use to place it. That
    is the one semantic choice in this format, and `build_strike_composite()`
    below gives its three leaves three different bearings so that a reader which
    used them draws a visibly different glyph.
    """
    out = struct.pack(">H", len(parts))
    for glyph, dx, dy in parts:
        out += struct.pack(">Hbb", glyph, dx, dy)
    return out


def composite_art(width, height, parts, leaves):
    """What a composite's pixels must come out as, drawn here independently.

    The generator's own arithmetic, so that the *direct* glyph this fixture pairs
    each composite with is not a copy of what the composite says but a second
    statement of what it means. If this and the library disagree the fixture fails
    to build, and if both were wrong the same way the direct glyph and the
    composite would still differ from each other.
    """
    grid = [["."] * width for _ in range(height)]
    for glyph, dx, dy in parts:
        rows = leaves[glyph]
        for y, row in enumerate(rows):
            for x, cell in enumerate(row):
                if cell == "#":
                    grid[dy + y][dx + x] = "#"
    return ["".join(row) for row in grid]


def build_strike_composite(out):
    """One strike whose composite glyphs each have a non-composite twin.

    Image formats **8 and 9** are the composites: a list of other glyphs of the
    same strike, each placed at a signed pixel offset and OR-ed into the
    composite's own box. Nothing in Debian has one - `uming.ttc` and `mona.ttf`
    between them carry none - and fontTools reads the component list but does not
    compose the image, so **no second reader anywhere produces these pixels**.

    What stands in for one is an identity, which is the same device the four
    bitmap containers and the six format pairings use: every composite here draws
    exactly what a plain glyph of the same strike draws.

      glyphs 1-3    index 1, image 1   the bar, and the two direct twins
      glyph  4      index 1, image 7   the upright, big metrics and bit-aligned
      glyph  5      index 1, image 2   the dot, small metrics and bit-aligned
      glyphs 6-7    index 1, image 8   small metrics **and a pad byte**
      glyphs 8-9    index 1, image 9   big metrics, no pad

      glyph 6 (format 8) == glyph 8 (format 9) == glyph 2, drawn directly
      glyph 9, a composite **of a composite**, == glyph 3
      glyph 7 has zero components: its box, and no pixels

    Three things are deliberate beyond the identity:

      * **The leaves' bearings all differ from the composites'** - (3,4), (-2,2)
        and (7,6) against (1,7) - so a reader that positioned a component by its
        own bearings rather than by the component record's offsets would draw
        something else. One of them is negative, which is also the signed read.
      * **The components land at offsets that are not multiples of eight.** The
        upright goes to column 5 and the dot to column 9, so the blit crosses a
        byte boundary in one case and starts mid-byte in the other; a blit that
        only ever shifted by whole bytes would pass a fixture whose components all
        sat at column 0.
      * **The components' own image formats differ from the composite's**, and
        from each other: a component is found through the index like any glyph, so
        a format 8 composite drawing a byte-aligned leaf and a bit-aligned one is
        the case that says the two axes stay independent inside a composite.
    """
    width = 11
    height = 7
    advance = width + 2
    mapping = {0x41 + i: name
               for i, name in enumerate(STRIKE_COMPOSITE_ORDER[1:])}
    fb = truetype("Strike Composite", [subtable(4, 3, 1, mapping)],
                  order=STRIKE_COMPOSITE_ORDER,
                  glyphs={name: raw_glyph([])
                          for name in STRIKE_COMPOSITE_ORDER},
                  advances={name: 600 for name in STRIKE_COMPOSITE_ORDER})

    # The leaves, and the two placements every composite here uses.
    leaves = {
        1: ["#" * width],                      # the bar, 11 x 1
        4: ["#"] * height,                     # the upright, 1 x 7
        5: ["##", "##"],                       # the dot, 2 x 2
    }
    leaf_metrics = {1: (3, 4), 4: (-2, 2), 5: (7, 6)}
    plus_parts = [(1, 0, 3), (4, 5, 0)]
    plus = composite_art(width, height, plus_parts, leaves)
    # The nested one draws the format 8 composite and then the dot, so its own
    # component list names a *composite* and the recursion has somewhere to go.
    dotted_parts = [(6, 0, 0), (5, 9, 5)]
    dotted = composite_art(width, height, dotted_parts,
                           {**leaves, 6: plus})

    ebdt = bytearray(struct.pack(">I", 0x00020000))
    bodies = []

    def leaf(glyph, aligned, big):
        rows = leaves[glyph]
        packed = (eblc_byte_rows(rows, len(rows[0])) if aligned
                  else eblc_bit_rows(rows, len(rows[0])))
        bearing_x, bearing_y = leaf_metrics[glyph]
        metrics = (eblc_big_metrics if big else eblc_small_metrics)(
            len(rows), len(rows[0]), bearing_x, bearing_y, len(rows[0]) + 1)
        return metrics + packed

    def direct(rows):
        return (eblc_small_metrics(height, width, 1, height, advance)
                + eblc_byte_rows(rows, width))

    def offsets_body(index_format, image_format, data_at, records):
        """A format 1 subtable over @p records, appended to EBDT in order."""
        offsets = [0]
        for record in records:
            ebdt.extend(record)
            offsets.append(len(ebdt) - data_at)
        return (struct.pack(">HHI", index_format, image_format, data_at)
                + b"".join(struct.pack(">I", o) for o in offsets))

    # --- glyphs 1-3: the bar and the two direct twins, image format 1.
    data_at = len(ebdt)
    bodies.append((1, 3, offsets_body(1, 1, data_at,
        [leaf(1, True, False), direct(plus), direct(dotted)])))

    # --- glyph 4: the upright, image format 7 - big metrics, bit-aligned.
    data_at = len(ebdt)
    bodies.append((4, 4, offsets_body(1, 7, data_at, [leaf(4, False, True)])))

    # --- glyph 5: the dot, image format 2 - small metrics, bit-aligned.
    data_at = len(ebdt)
    bodies.append((5, 5, offsets_body(1, 2, data_at, [leaf(5, False, False)])))

    # --- glyphs 6-7: image format 8. SmallGlyphMetrics, then one pad byte, then
    # the components. Glyph 7 states none, which is a box and no pixels.
    data_at = len(ebdt)
    header = eblc_small_metrics(height, width, 1, height, advance) + b"\x00"
    bodies.append((6, 7, offsets_body(1, 8, data_at,
        [header + eblc_components(plus_parts),
         header + eblc_components([])])))

    # --- glyphs 8-9: image format 9. BigGlyphMetrics and **no** pad byte, which
    # is the whole difference between the two headers.
    data_at = len(ebdt)
    big = eblc_big_metrics(height, width, 1, height, advance)
    bodies.append((8, 9, offsets_body(1, 9, data_at,
        [big + eblc_components(plus_parts),
         big + eblc_components(dotted_parts)])))

    array_bytes = len(bodies) * 8
    cursor = array_bytes
    arrays = []
    for first, last, body in bodies:
        arrays.append(struct.pack(">HHI", first, last, cursor))
        cursor += len(body)
    region = b"".join(arrays) + b"".join(body for _, _, body in bodies)

    base = 8 + 48
    size_table = (struct.pack(">IIII", base, len(region), len(bodies), 0)
                  + eblc_line_metrics(height, -1, advance)
                  + eblc_line_metrics(0, 0, 0)
                  + struct.pack(">HHBBBb", 1, 9, 12, 12, 1, 1))
    eblc = struct.pack(">II", 0x00020000, 1) + size_table + region

    set_raw_table(fb, "EBLC", eblc)
    set_raw_table(fb, "EBDT", bytes(ebdt))
    fb.save(out)


FIXTURES = {
    "strike-composite.ttf": (build_strike_composite,
        "EBDT image formats 8 and 9: composite glyphs, each paired with a "
        "non-composite twin of the same strike that draws the same pixels - "
        "nothing in Debian has a composite and fontTools reads the component "
        "list without composing it, so the identity is the only check there is. "
        "A format 8 header with its pad byte and a format 9 one without, a "
        "composite of a composite, a composite with zero components, leaf "
        "bearings that all differ from the composites' so that using them would "
        "show, and components at columns 5 and 9 so the blit is not "
        "byte-aligned"),
    "strike-formats.ttf": (build_strike_formats,
        "One EBLC strike whose six index subtables use six different index/image "
        "format pairings - 4-byte and 2-byte offsets, sparse glyph lists, "
        "constant metrics, small and big metrics, byte- and bit-aligned rows - all "
        "drawing one design, so the bytes differ everywhere and the pixels must "
        "not. Covers every cell of the grid the real population does not: image "
        "format 6 appears only in files fontTools cannot read, and index formats "
        "3, 4 and 5 appear nowhere in Debian"),
    "strikes.ttf": (build_strikes,
        "Three EBLC strikes - 10, 12 and 16 ppem - of four glyphs each, index "
        "subtable format 2 with image format 5, which is 97.8% of the index "
        "subtables in the only two Debian packages that carry this table. The "
        "first fixture in this library with more than one strike, so the first "
        "input where GFNT_STRIKE_NEAREST has to choose; 11 ppem is a tie between "
        "two of them, and glyph 0 is in none of them"),
    "bitmap-gz.pcf.gz": (build_bitmap_gz_pcf,
        "The same PCF as bitmap.pcf inside gzip, which is how every PCF in the "
        "world ships: it must read to identical glyphs, because the wrapper "
        "decides nothing about what the font is"),
    "type1-gz.pfb.gz": (build_type1_gz_pfb,
        "A gzipped Type 1 program - two derivations stacked, inflate then eexec - "
        "which nobody ships and which is the only input where each layer has to "
        "free the one it consumed"),
    "bitmap.hex": (build_bitmap_hex,
        "GNU Unifont .hex: eight glyphs of 8x16 in hexadecimal, one per line, "
        "with no header, no baseline and no names - the same design as "
        "bitmap.psf, bitmap.bdf and bitmap.pcf, so all four must read to "
        "identical pixels"),
    "bitmap-wide.hex": (build_bitmap_wide_hex,
        ".hex at the other two widths the format allows: a 16-pixel glyph and a "
        "32-pixel one, and a six-digit codepoint past the BMP"),
    "bitmap.psf": (build_bitmap_psf,
        "PSF 2: the shared design with a Unicode table in UTF-8, where one cell "
        "answers two codepoints and one carries a two-codepoint sequence that a "
        "reader must walk past rather than map"),
    "bitmap-v1.psf": (build_bitmap_psf1,
        "PSF 1: 512 glyphs, which only one bit of the mode byte says, and a "
        "Unicode table in UCS-2 little-endian with 0xFFFE sequence separators"),
    "bitmap.bdf": (build_bitmap_bdf,
        "BDF: the shared design, with the properties that state the baseline "
        "(FONT_ASCENT, FONT_DESCENT), the pixel size, the family and the "
        "copyright, and a name per character"),
    "bitmap-ink.bdf": (build_bitmap_ink_bdf,
        "BDF: what only a per-glyph box can say - a negative x offset, a "
        "descender whose BBX y is below the baseline, advances narrower and "
        "wider than the box, ENCODING -1 for a glyph with no character, an "
        "empty BBX with no BITMAP rows at all, and a row whose bits past the "
        "glyph's width are set, which a reader has to clear"),
    "bitmap.pcf": (build_bitmap_pcf,
        "PCF: the shared design, most-significant bit and byte first, rows "
        "padded to one byte, uncompressed metrics, with properties, "
        "BDF accelerators, encodings, scalable widths and glyph names"),
    "bitmap-swap.pcf": (build_bitmap_swap_pcf,
        "PCF whose bits run least-significant-first and whose bytes run most, "
        "with a four-byte scan unit: the one combination where a scan unit's "
        "bytes have to be reversed as well as its bits, which no other fixture "
        "reaches and which Pillow cannot read"),
    "bitmap-lsb.pcf": (build_bitmap_lsb_pcf,
        "PCF laid out the other way in every dimension: bits and bytes "
        "least-significant first, a two-byte scan unit, rows padded to four, "
        "compressed metrics and an ink-metrics table nothing reads - and the "
        "same glyphs as bitmap.pcf, which is the only check on that arithmetic"),
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
    "cff-curves.otf": (build_cff_curves,
        "CFF: every curve operator in the form a pen never writes - three-group "
        "hvcurveto and vhcurveto with the trailing operand, hhcurveto and "
        "vvcurveto with the leading one, rcurveline, rlinecurve, and all four "
        "of flex, hflex, hflex1 and flex1 (twice, once per axis)"),
    "cff-arith.otf": (build_cff_arith,
        "CFF: the arithmetic operators computing the coordinates they draw "
        "with - div, the 255 fixed-point operand, dup, exch, drop, index, put, "
        "get, eq, not, mul, add and ifelse; no random, which is refused"),
    "cff-hints.otf": (build_cff_hints,
        "CFF: stem hints and the masks their count sizes - an odd operand "
        "count carrying the advance, a two-byte mask, stems declared inside a "
        "subroutine, an implicit vstem before hintmask, cntrmask, and a width "
        "on vstem"),
    "cff-subrs.otf": (build_cff_subrs,
        "CFF: local and global subroutines with the bias applied, nesting, a "
        "subroutine that ends without return, defaultWidthX, a nominalWidthX "
        "delta, and one glyph whose charstring advance hmtx disagrees with"),
    "cff-seac.otf": (build_cff_seac,
        "CFF: an accented character (endchar with four operands) named by "
        "Standard Encoding code, a charset in format 1, a name in the font's "
        "own String INDEX, and a custom encoding with a supplement"),
    "cff-cid.otf": (build_cff_cid,
        "CFF: CID-keyed - ROS, an FDArray of two Font DICTs with different "
        "Private DICTs and different local subroutines, FDSelect format 3, and "
        "a charset holding CIDs rather than SIDs"),
    "cff-type1.otf": (build_cff_type1,
        "CFF with CharstringType 1: Type 1 programs - hsbw, sbw, closepath, "
        "seac with its side-bearing correction, flex and hint replacement "
        "through callothersubr and pop, div, dotsection, hstem3 and "
        "setcurrentpoint"),
    "bare-matrix.cff": (build_bare_matrix_cff,
        "A bare CFF whose FontMatrix scales the two axes differently, so it "
        "states no em this library can report and is refused by name; its "
        "translation components carry a DICT real in exponent form and one with "
        "more mantissa digits than 16.16 holds, and it states its licence in "
        "Notice rather than Copyright"),
    "bare.cff": (build_bare_cff,
        "A bare CFF font program with no sfnt around it: the same CFF table as "
        "cff-curves.otf, so the two faces must draw identically"),
    "type1-big.pfb": (build_type1_big,
        "A Type 1 program larger than one allocation: 80 glyphs, subroutine "
        "numbers 0, 1 and 40 with a gap between, and a glyph that calls the "
        "high one - the three reallocation paths no other fixture reaches"),
    "type1.pfb": (build_type1_pfb,
        "A Type 1 font program in PFB framing: binary segments with "
        "little-endian lengths, eexec, a per-charstring cipher, a custom "
        "/Encoding, and the same charstrings as cff-type1.otf"),
    "type1.pfa": (build_type1_pfa,
        "The same Type 1 font as ASCII: hex after eexec, no segment framing, "
        "the -| |- | spellings, /lenIV 0, a /CharStrings count smaller than the "
        "glyphs it defines, and StandardEncoding"),
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
    "variable-hvar.ttf": (build_variable_hvar,
        "variable-gvar.ttf's glyphs with HVAR - an advance mapping and a left "
        "bearing mapping that shares three rows among nine glyphs, over two "
        "regions - and MVAR carrying the hhea, window and typographic ascent, "
        "descent and gap, all written by fontTools"),
    "variable-avar2.ttf": (build_variable_avar2,
        "variable-gvar.ttf's font with an avar version 2: segment maps, then a "
        "variation store of three regions (weight up, width up, weight down) "
        "that moves each axis as a function of the others, and an index map "
        "that sends the weight axis to the second row and the width axis to the "
        "first, written by fontTools"),
    "variable-stat.ttf": (build_variable_stat,
        "variable-gvar.ttf's font with a STAT version 1.2 holding every axis "
        "value format: a linked value (elidable), a plain value, a range around "
        "a nominal value, and a value on two axes at once, written by fontTools"),
    "variable-featurevars.ttf": (build_variable_featurevars,
        "variable-gvar.ttf's font with a GSUB 1.1 whose FeatureVariations has "
        "three records: one condition, two that must both hold, and a "
        "substitution of two glyphs; written by fontTools"),
    "variable-cvar.ttf": (build_variable_cvar,
        "variable-gvar.ttf's font with a cvt table of eight values and a cvar of "
        "four tuples: every value, a sparse list, an intermediate region and a "
        "two-axis corner, written by fontTools"),
    "variable-gvar.ttf": (build_variable_gvar,
        "fvar, avar and gvar written by fontTools: two axes (one hidden), three "
        "named instances (one with a PostScript name), a bent avar on one axis "
        "and an identity map on the other; and in gvar a single named point, an "
        "inferred run between two corners, named points in two contours of one "
        "glyph, an intermediate region, a two-axis corner, shared and embedded "
        "peaks, a composite whose component offsets move (one placed by matching "
        "points, whose delta is ignored), a composite of a composite, and a glyph "
        "with no contours that varies only in its phantom points"),
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
