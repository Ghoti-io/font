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
"""fontTools' answer for every CFF glyph: its program, its path, its advance.

Runs **inside** the pinned image, called by `cff_diff.py`.

Three things are read from fontTools rather than computed here, and each is a
place where a second implementation would be comparing this library against
itself:

  * `T2CharString.decompile()` divides the bytes into operators and operands, so
    the program being compared is the reference's reading of the same bytes;
  * `charstring.draw(pen)` runs fontTools' own interpreter - the curve
    operators, the flex family, and `endchar`'s accented-character form, which
    it turns into two components that the pen resolves through the glyph set;
  * `charstring.width` is the advance the *charstring* states, which fontTools
    computes from `nominalWidthX` and `defaultWidthX` exactly where this library
    does. It is a different number from `hmtx`'s and the two may disagree.

**Coordinates are printed in 26.6**, as `fonttools_outlines.py` prints them and
for the same reason: a charstring's `div` can put a coordinate between two font
units, and rounding here would make that a disagreement rather than a value.

Usage: fonttools_charstrings.py <font> <face> <stride> <first>
"""

import sys

from fontTools.misc.roundTools import otRound
from fontTools.pens.basePen import BasePen
from fontTools.pens.boundsPen import ControlBoundsPen
from fontTools.ttLib import TTFont


# 26.6: one font unit is 64.
ONE_PIXEL = 64

# 16.16, which is how a width is printed: a charstring's arithmetic is fixed
# point and its advance need not be a whole number.
ONE_FIXED = 65536

# Coordinates that were not a whole number of 64ths, counted so that the other
# side can report how many rather than widening its comparison for all of them.
INEXACT = [0]


def sixty_fourths(value):
    """A coordinate in 26.6, marked with `~` when it is not exactly one."""
    scaled = value * ONE_PIXEL
    rounded = otRound(scaled)
    if abs(scaled - rounded) > 1e-9:
        INEXACT[0] += 1
        return "%d~" % rounded
    return "%d" % rounded


class Path(BasePen):
    """One line per segment, in the spelling our driver prints.

    Constructed with the glyph set, which is what makes `endchar`'s accented
    character decompose: fontTools' interpreter emits it as two components, and
    `BasePen` resolves a component by drawing the named glyph through the set it
    was given. A pen without one would silently draw nothing for those glyphs.
    """

    def _moveTo(self, point):
        self.lines.append("move %s %s" % (sixty_fourths(point[0]),
                                          sixty_fourths(point[1])))

    def _lineTo(self, point):
        self.lines.append("line %s %s" % (sixty_fourths(point[0]),
                                          sixty_fourths(point[1])))

    def _qCurveToOne(self, control, point):
        self.lines.append("quad %s %s %s %s" % (
            sixty_fourths(control[0]), sixty_fourths(control[1]),
            sixty_fourths(point[0]), sixty_fourths(point[1])))

    def _curveToOne(self, first, second, point):
        self.lines.append("cubic %s %s %s %s %s %s" % (
            sixty_fourths(first[0]), sixty_fourths(first[1]),
            sixty_fourths(second[0]), sixty_fourths(second[1]),
            sixty_fourths(point[0]), sixty_fourths(point[1])))

    def _closePath(self):
        self.lines.append("close")

    def __init__(self, glyph_set):
        BasePen.__init__(self, glyph_set)
        self.lines = []


def fixed(value):
    """A number as 16.16, which is how both sides spell a charstring operand."""
    return "%d" % otRound(value * ONE_FIXED)


def program_lines(charstring):
    """The decompiled program, one operator per line, operands after it.

    The operands follow the operator here and precede it in the file, which is
    the one normalisation this comparison makes: a charstring is written
    postfix and is *read* as an instruction with arguments, and putting the name
    first is what makes a disagreement greppable by operator.

    A `hintmask` or `cntrmask` carries its mask bytes, which fontTools keeps in
    the program as a `bytes`. They are printed as hex, because the mask's width
    is decided by how many stems have been declared - a reader that miscounted
    them reads the next operator out of the middle of the mask, and the mask's
    bytes are where that shows.
    """
    lines = []
    pending = []
    for token in charstring.program:
        if isinstance(token, bytes):
            # Always right after its hintmask or cntrmask.
            if lines:
                lines[-1] += " mask " + " ".join("%02X" % b for b in token)
            continue
        if isinstance(token, str):
            lines.append(" ".join([token] + pending))
            pending = []
            continue
        pending.append(fixed(token))
    if pending:
        lines.append("trailing-operands %d" % len(pending))
    return lines


class Type1Glyph:
    """One Type 1 charstring, wrapped so a pen can resolve a component to it.

    `seac` is why this exists. fontTools' Type 1 interpreter draws an accented
    character by calling `pen.addComponent(name, ...)`, and `BasePen` resolves a
    component by looking the name up in the glyph set it was given - swallowing a
    `KeyError` if it is not there. The font's own glyph set holds `T2CharString`
    glyphs, because fontTools' CFF reader ignores `CharstringType`, so passing it
    here drew nothing at all and reported no path: a differential reads that as
    agreement unless something says otherwise.
    """

    def __init__(self, charstring):
        self.charstring = charstring
        self.width = 0

    def draw(self, pen):
        self.charstring.draw(pen)


def type1_subrs(top):
    """The Private DICT's local subroutines, as Type 1 programs."""
    from fontTools.misc.psCharStrings import T1CharString

    subrs = []
    private = getattr(top, "Private", None)
    for subr in getattr(private, "Subrs", []) or []:
        subrs.append(T1CharString(bytecode=subr.bytecode, subrs=subrs))
    return subrs


def as_type1(charstring, subrs):
    """The same bytes, read as the Type 1 program the Top DICT says they are.

    fontTools' `T1CharString` is a separate interpreter with the operators Type 1
    has and Type 2 does not - `hsbw`, `closepath`, `seac`, `callothersubr`,
    `setcurrentpoint` - and it takes bytecode and subroutines directly, so no
    container has to hand it anything.
    """
    from fontTools.misc.psCharStrings import T1CharString

    return T1CharString(bytecode=charstring.bytecode, subrs=subrs)


# Streams cffLib still holds lazy references into.
_STREAMS = []


def load(path, face):
    """`(order, charstrings, glyph_set, top)` for an sfnt or a bare CFF.

    A bare CFF is a font program with no sfnt around it - what a PDF
    `FontFile3` carries - and `TTFont` cannot open one at all. `cffLib` reads
    one directly, and everything this reference does afterwards needs only the
    charstrings, the glyph order, and something a pen can resolve an accented
    character's components against.

    The sniff is the same rule the library uses: major version 1 and a header at
    least the four bytes every CFF header has. It cannot collide with an sfnt,
    whose first byte is 0x00, `O`, `t` or `w`.

    Returns None when there are no charstrings to compare.
    """
    with open(path, "rb") as handle:
        head = handle.read(4)
    if len(head) == 4 and head[0] == 1 and head[2] >= 4:
        from fontTools.cffLib import CFFFontSet
        from io import BytesIO

        with open(path, "rb") as handle:
            stream = BytesIO(handle.read())
        # cffLib reads charstrings *lazily* out of the stream it is handed, so
        # the stream has to outlive this function - a `with` around the decompile
        # closes it and every later read raises "seek of closed file". Keeping it
        # here says so where it can be seen.
        _STREAMS.append(stream)
        cff = CFFFontSet()
        cff.decompile(stream, None)
        top = cff[cff.fontNames[0]]
        charstrings = top.CharStrings
        # A CFF's own glyph order is its charset, which is also the only place a
        # bare one states its glyph names - there is no `post` to hold them.
        return list(top.charset), charstrings, charstrings, top

    font = TTFont(path, fontNumber=face, lazy=True)
    if "CFF " not in font:
        return None
    cff = font["CFF "].cff
    top = cff[cff.fontNames[0]]
    return font.getGlyphOrder(), top.CharStrings, font.getGlyphSet(), top


def main(argv):
    if len(argv) < 5:
        sys.stderr.write("usage: %s <font> <face> <stride> <first>\n" % argv[0])
        return 2
    path = argv[1]
    face = int(argv[2])
    stride = max(1, int(argv[3]))
    first = int(argv[4])

    loaded = load(path, face)
    if loaded is None:
        # Said rather than left silent: a differential reads no output as
        # agreement, and "this font has no CFF" is a fact both sides can hold.
        print("charstrings: none")
        return 0
    order, charstrings, glyph_set, top = loaded
    # **fontTools' CFF reader ignores `CharstringType`**: `cffLib.CharStrings`
    # builds a `T2CharString` whatever the Top DICT says, so a font carrying
    # Type 1 programs - which the format allows - decompiles as Type 2 and comes
    # out as two leftover operands and no operators. It has a Type 1
    # interpreter; it just does not reach it from here. So the bytes are
    # re-wrapped below, which is the only way this reference can answer for the
    # Type 1 language at all.
    charstring_type = getattr(top, "CharstringType", 2)
    if charstring_type == 1:
        subrs = type1_subrs(top)
        # A glyph set of Type 1 programs, so that an accented character's
        # components resolve to the same language the charstring is in.
        glyph_set = {name: Type1Glyph(as_type1(charstrings[name], subrs))
                     for name in order}
    print("charstrings: %d glyph(s), stride %d from %d"
          % (len(order), stride, first))

    for index in range(first, len(order), stride):
        name = order[index]
        charstring = charstrings[name]
        # Before decompile(), which clears it: fontTools keeps the bytes and the
        # decompiled program in one attribute pair and discards the first.
        raw = len(charstring.bytecode or b"")
        if charstring_type == 1:
            charstring = as_type1(charstring, subrs)
        try:
            charstring.decompile()
            pen = Path(glyph_set)
            charstring.draw(pen)
        except Exception as why:  # noqa: BLE001 - the reason is the output
            print("glyph %d: reference refused %s" % (index, type(why).__name__))
            continue
        # An `endchar` with four operands is the accented-character form, and
        # fontTools turns it into two components rather than a flag - so the
        # program is where it can be seen.
        # Two spellings of one construction. Type 2 says `endchar` with four
        # operands - five when the first of them is the advance, so the count is
        # what says it and "exactly five tokens" was wrong for every such glyph
        # that also states a width - and Type 1 has an operator of its own.
        seac = 1 if any(
            line.startswith("seac")
            or (line.startswith("endchar ") and len(line.split()) in (5, 6))
            for line in program_lines(charstring)) else 0
        if any(line.startswith("sbw") for line in program_lines(charstring)):
            # fontTools' Type 1 interpreter drops `sbw`'s operands and sets
            # neither the advance nor the side bearing - its own source marks the
            # line XXX. So it is not a reference for such a glyph, and this says
            # so rather than reporting the zero it would otherwise print.
            print("glyph %d: reference refused sbw" % index)
            continue
        print("glyph %d: seac %d, bytes %d" % (index, seac, raw))
        print("glyph %d width: %s" % (index, fixed(charstring.width)))
        print("glyph %d name: %s" % (index, name))
        bounds = ControlBoundsPen(glyph_set)
        charstring.draw(bounds)
        if bounds.bounds is None:
            print("glyph %d control: empty" % index)
        else:
            print("glyph %d control: %s %s %s %s"
                  % tuple([index] + [sixty_fourths(v) for v in bounds.bounds]))
        for line in pen.lines:
            print("glyph %d path: %s" % (index, line))
        for line in program_lines(charstring):
            print("glyph %d program: %s" % (index, line))
    print("charstrings: %d inexact coordinate(s)" % INEXACT[0])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
