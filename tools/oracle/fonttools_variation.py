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
"""fontTools' answer for every glyph's outline at a location in the design space.

Runs **inside** the pinned image, called by `var_diff.py`.

Two things are fontTools' own and nothing here reimplements either:

  * the **normalisation** - `normalizeValue` for the clamp and the two linear
    halves, `piecewiseLinearMap` for `avar` - which is what `font.getGlyphSet(
    location=)` expects its argument to already have been through, and
  * the **instance**: `getGlyphSet(location=)` applies `gvar` to a glyph's points
    (`supportScalar` for the tuple scalars, `iup_delta` for the untouched points)
    and to a composite's component offsets, and draws the result through a pen.

What this script does is print them in the spelling `font-outline` prints, so that
the two can be compared line for line. The path is what is compared and not the
points: an instance's points are not an object fontTools exposes, and the pen is
what it draws.

**Coordinates are printed in 26.6 and not rounded to font units**, for the reason
`fonttools_outlines.py` states at length - and here it matters more, because a
delta times a scalar is a fraction at almost every point and this library keeps
it to a 64th. A value that is not a whole number of 64ths is marked `~`, and
`var_diff.py` allows one 64th there and counts how many such comparisons the run
rested on.

Usage: fonttools_variation.py <font> <face> <stride> <first> <spec>...
"""

import sys

from fontTools.misc.roundTools import otRound
from fontTools.pens.basePen import BasePen
from fontTools.ttLib import TTFont
from fontTools.varLib.models import normalizeValue

from fonttools_outlines import flattened

ONE_PIXEL = 64
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
    """One line per segment, in the spelling our driver prints."""

    def __init__(self, glyph_set):
        BasePen.__init__(self, glyph_set)
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


def parse_location(text):
    """`wght=700,opsz=14` as {tag: float}."""
    location = {}
    for item in text.split(","):
        if not item:
            continue
        tag, _, value = item.partition("=")
        location[tag] = float(value)
    return location


def normalised(font, user):
    """The location in -1..1 after `avar`, from fontTools' own functions.

    A version 2 `avar` is `renormalizeLocation()`'s job: the segment maps and then
    the store, which only that method knows the order of.
    """
    fvar = font["fvar"]
    avar = font["avar"] if "avar" in font else None
    result = {}
    for axis in fvar.axes:
        value = user.get(axis.axisTag, axis.defaultValue)
        value = max(axis.minValue, min(axis.maxValue, value))
        normal = normalizeValue(value, (axis.minValue, axis.defaultValue,
                                        axis.maxValue))
        result[axis.axisTag] = normal
    if avar is not None:
        return avar.renormalizeLocation(result, font, dropZeroes=False)
    return result


def axes_of(font):
    """Every axis, as `tag min default max` in user coordinates."""
    return [(axis.axisTag, axis.minValue, axis.defaultValue, axis.maxValue)
            for axis in font["fvar"].axes]


class Varied:
    """A glyf table whose every glyph is fontTools' instance at the location.

    **Why not `glyph_set[name].draw(pen)`**, which is the obvious call: a glyph set
    cannot draw a composite with a component placed by matching points. Its
    `getComponentInfo()` builds a transform from the component's `x` and `y`, and
    a matched component has `firstPt` and `secondPt` instead, so the call raises
    `AttributeError` at *any* location, the default included. `glyf_diff.py`
    never met this because it resolves composites with `Glyph.getCoordinates()`,
    which handles point matching - and so does this.

    `getCoordinates()` finds a component's glyph by indexing the table it is
    handed, so handing it this makes every component come out varied. Each
    instance is fontTools' own - `_getGlyphInstance()` applies `gvar` with
    `supportScalar` and `iup_delta` and moves a composite's component offsets -
    and nothing here computes a delta.
    """

    def __init__(self, glyph_set, glyf):
        self.glyph_set = glyph_set
        self.glyf = glyf
        self.cache = {}

    def __getitem__(self, name):
        if name not in self.cache:
            self.cache[name] = self.glyph_set[name]._getGlyphInstance()
        return self.cache[name]

    def __getattr__(self, name):
        # Anything else a glyph asks its table for - `getGlyphName`, `glyphs` - is
        # the real table's to answer.
        return getattr(self.glyf, name)


def draw(font, order, glyph_set, stride, first):
    """Every sampled glyph's path at the glyph set's location."""
    glyf = font["glyf"]
    varied = Varied(glyph_set, glyf)
    for index in range(first, len(order), stride):
        name = order[index]
        try:
            flat = flattened(varied, name)
            pen = Path(None)
            flat.draw(pen, varied)
        except Exception as why:  # noqa: BLE001 - the reason is the output
            print("glyph %d: reference refused %s" % (index, type(why).__name__))
            continue
        for line in pen.lines:
            print("glyph %d path: %s" % (index, line))


def main(argv):
    """Usage: fonttools_variation.py <font> <face> <stride> <first> <spec>...

    Each `spec` is `<user location>@<normalised coordinates>`: the user location
    is `wght=700,opsz=14`, and the normalised coordinates are the 2.14 integers
    **this library** computed for it, comma-separated, or empty. The glyphs are
    drawn at the normalised ones when given, so that what is compared is `gvar`'s
    arithmetic and not two rounding rules for normalisation, which
    `var_diff.py` compares on their own from the `variation:` line printed here.
    """
    if len(argv) < 5:
        sys.stderr.write("usage: %s <font> <face> <stride> <first> [spec]...\n"
                         % argv[0])
        return 2
    path = argv[1]
    face = int(argv[2])
    stride = max(1, int(argv[3]))
    first = int(argv[4])

    font = TTFont(path, fontNumber=face, lazy=True)
    if "glyf" not in font or "gvar" not in font:
        print("outlines: no gvar")
        return 0
    for tag, low, default, high in axes_of(font):
        print("axis: %s %r %r %r" % (tag, low, default, high))
    order = font.getGlyphOrder()
    print("outlines: %d glyph(s), stride %d from %d"
          % (len(order), stride, first))
    for number, spec in enumerate(argv[5:]):
        text, _, given = spec.partition("@")
        user = parse_location(text)
        location = normalised(font, user)
        print("location %d: %s" % (number, text))
        print("variation: %d axes, normalised %s" % (
            len(location), " ".join(str(otRound(location[axis.axisTag] * 16384))
                                     for axis in font["fvar"].axes)))
        if given:
            used = {axis.axisTag: int(value) / 16384.0
                    for axis, value in zip(font["fvar"].axes, given.split(","))}
        else:
            used = location
        glyph_set = font.getGlyphSet(location=used, normalized=True)
        draw(font, order, glyph_set, stride, first)
    print("outlines: %d inexact coordinate(s)" % INEXACT[0])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
