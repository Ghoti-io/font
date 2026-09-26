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
"""fontTools' answer for every glyph's outline, in the same shape our dump uses.

Runs **inside** the pinned image, called by `glyf_diff.py`.

Two things are read from fontTools rather than computed here, and that is the
whole design:

  * `Glyph.getCoordinates()` resolves composites - point matching, all three
    transform encodings, the scaled-offset flag - so the component arithmetic
    being compared is fontTools' own and not a second copy of ours.
  * the flattened glyph's `draw()` walks the contours, so the implicit on-curve
    midpoint and the contour that begins off-curve are fontTools' rules too.

Composing them needs one step: `getCoordinates()` hands back points and
`draw()` wants a glyph, so the flattened points are put into a fresh simple
`Glyph` and that is what draws. Both halves are the reference's.

`BasePen` rather than `RecordingPen`: the recording pen keeps fontTools'
multi-control `qCurveTo` and its `None` terminator verbatim, and expanding those
here would be a third implementation of the midpoint rule. `BasePen` already
expands them into one segment per curve.

**Coordinates are printed in 26.6, and are not rounded to font units.** That is
the whole of what the first run of this differential was about. `glyf` leaves the
on-curve point between two off-curve points implicit, and it is their midpoint -
which is a *half* unit whenever their sum is odd. This library keeps that half
exactly, because 26.6 can hold it; rounding it here to an integer reported it as
a disagreement in every font with an odd coordinate sum, which is most of them,
and the rounding was the reference script's rather than fontTools'. So
`getCoordinates()` is asked without `round=`, and each value is multiplied by 64
and printed as an integer number of 64ths - exact for every integer coordinate
and every half. A value that is not a whole number of 64ths can only come from a
composite whose transform is not dyadic; it is marked, and `glyf_diff.py` counts
those separately rather than widening its comparison for them.

Usage: fonttools_outlines.py <font> <face> <stride> <first>
"""

import sys

from fontTools.misc.roundTools import otRound
from fontTools.pens.basePen import BasePen
from fontTools.ttLib import TTFont
from fontTools.ttLib.tables import ttProgram
from fontTools.ttLib.tables._g_l_y_f import Glyph, GlyphCoordinates


# 26.6: one font unit is 64, and a half unit - which is where an implicit
# midpoint lands - is 32.
ONE_PIXEL = 64

# Values that were not a whole number of 64ths, counted so that the other side
# can report how many rather than widening its comparison for all of them.
INEXACT = [0]


def sixty_fourths(value):
    """A coordinate in 26.6, marked with `~` when it is not exactly one.

    fontTools composes a component's transform in floating point; this library
    composes it in 26.6 and rounds at each step. Where the transform is dyadic -
    a half, a quarter, the identity - both are exact and the two agree to the
    last bit. Where it is not, which is every sheared or arbitrarily scaled
    component, the reference's value falls between two 64ths and neither side is
    wrong about it.

    The mark is what lets the other side allow a difference of one 64th **there
    and nowhere else**, and count how many such coordinates a run rested on. A
    blanket tolerance would have hidden a real off-by-one everywhere.
    """
    scaled = value * ONE_PIXEL
    rounded = otRound(scaled)
    if abs(scaled - rounded) > 1e-9:
        INEXACT[0] += 1
        return "%d~" % rounded
    return "%d" % rounded


class Path(BasePen):
    """One line per segment, in the spelling our driver prints."""

    def __init__(self, glyph):
        BasePen.__init__(self, None)
        self.glyph = glyph
        self.lines = []

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


def flattened(glyf, name):
    """The glyph with its components resolved, as a simple glyph.

    No `round=`: a composite's transformed coordinates are kept as they come out
    of the transform, because this library keeps them in 26.6 rather than
    rounding them to font units, and rounding here would make every scaled
    component a disagreement.
    """
    coordinates, ends, flags = glyf[name].getCoordinates(glyf)
    out = Glyph()
    out.numberOfContours = len(ends)
    out.coordinates = GlyphCoordinates(coordinates)
    out.endPtsOfContours = list(ends)
    out.flags = bytearray(flags)
    out.program = ttProgram.Program()
    out.program.fromBytecode(b"")
    return out


def tag_of(flag):
    """`on`, `quad` or `cubic`, as our dump spells them."""
    if flag & 0x01:
        return "on"
    return "cubic" if flag & 0x80 else "quad"


def main(argv):
    if len(argv) < 5:
        sys.stderr.write("usage: %s <font> <face> <stride> <first>\n" % argv[0])
        return 2
    path = argv[1]
    face = int(argv[2])
    stride = max(1, int(argv[3]))
    first = int(argv[4])

    font = TTFont(path, fontNumber=face, lazy=True)
    if "glyf" not in font:
        # Said rather than left silent: a differential reads no output as
        # agreement, and "this font has no glyf" is a fact both sides can hold.
        print("outlines: no glyf")
        return 0
    glyf = font["glyf"]
    order = font.getGlyphOrder()
    print("outlines: %d glyph(s), stride %d from %d"
          % (len(order), stride, first))

    for index in range(first, len(order), stride):
        name = order[index]
        source = glyf[name]
        try:
            flat = flattened(glyf, name)
        except Exception as why:  # noqa: BLE001 - the reason is the output
            print("glyph %d: reference refused %s" % (index, type(why).__name__))
            continue
        points = list(flat.coordinates)
        ends = list(flat.endPtsOfContours)
        print("glyph %d: composite %d, points %d, contours %d"
              % (index, 1 if source.isComposite() else 0, len(points),
                 len(ends)))
        if hasattr(source, "xMin"):
            print("glyph %d stated: %d %d %d %d"
                  % (index, source.xMin, source.yMin, source.xMax, source.yMax))
        else:
            print("glyph %d stated: empty" % index)
        if points:
            xs = [p[0] for p in points]
            ys = [p[1] for p in points]
            print("glyph %d control: %s %s %s %s"
                  % (index, sixty_fourths(min(xs)), sixty_fourths(min(ys)),
                     sixty_fourths(max(xs)), sixty_fourths(max(ys))))
        else:
            print("glyph %d control: empty" % index)
        start = 0
        for contour, end in enumerate(ends):
            print("glyph %d contour %d: first %d, points %d"
                  % (index, contour, start, end + 1 - start))
            start = end + 1
        for at, point in enumerate(points):
            print("glyph %d point %d: %s %s %s"
                  % (index, at, sixty_fourths(point[0]),
                     sixty_fourths(point[1]), tag_of(flat.flags[at])))
        pen = Path(flat)
        flat.draw(pen, glyf)
        for line in pen.lines:
            print("glyph %d path: %s" % (index, line))
    # Printed last so that the other side can report it: a coordinate that is
    # not a whole number of 64ths is one neither side can hold exactly.
    print("outlines: %d inexact coordinate(s)" % INEXACT[0])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
