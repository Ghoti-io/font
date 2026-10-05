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
"""fontTools' answer for every glyph's advance, and the line metrics, at a location.

Runs **inside** the pinned image, called by `metrics_var_diff.py`.

What is fontTools' own, and what is not:

  * the **advance** is `glyph_set[name].width` - fontTools' own, which applies
    `HVAR` when the font has one and the difference of `gvar`'s first two phantom
    points when it does not;
  * the **line metrics** are the table's value plus a delta fontTools' own
    `VarStoreInstancer` computes from `MVAR`'s records - the `MVAR` table has no
    higher-level accessor in the version pinned, so the base value and the rounding
    are this script's and the delta is the library's;
  * the **side bearing** is `hmtx`'s plus `HVAR`'s left-bearing mapping through the
    same instancer, and is said to be absent where the table has no such mapping:
    that is a statement the library makes too (it refuses), and the two are
    compared as the same statement.

Printed in the spelling `font-metrics` prints, so the two compare line for line.
Every value is rounded with `otRound` - half *up*, where this library rounds half
*away from zero* - and the two differ only on an exactly-negative half, which
`metrics_var_diff.py` allows one unit for and counts.

Usage: fonttools_metrics.py <font> <face> <stride> <first> <spec>...
"""

import math
import sys

from fontTools.misc.roundTools import otRound
from fontTools.ttLib import TTFont
from fontTools.varLib.varStore import VarStoreInstancer

from fonttools_variation import axes_of, normalised, parse_location


def half_away(value):
    """A delta rounded the way this library rounds it: half away from zero.

    `otRound` is half *up*, so they differ on exactly the negative halves - a delta
    of -2.5 is -3 here and -2 in fontTools - and Inter's `opsz` axis puts a scalar of
    exactly one half at the middle of its range, where an odd delta lands on one.
    Both rules are defensible and FreeType's and HarfBuzz's is this one, so what is
    compared is **fontTools' unrounded delta, rounded this library's way**: the
    arithmetic is fontTools' and only the last step is not. The value is cleaned to
    six places first, so that a half carried as 2.4999999999 is still a half.
    """
    value = round(value, 6)
    return int(math.floor(abs(value) + 0.5)) * (1 if value >= 0 else -1)


def delta_of(instancer, index):
    """A VarIdx's delta at the location, as a float."""
    return instancer[index]


def line_value(font, instancer, records, table, field, tag, sign=1):
    """`field` of `table`, moved by the MVAR record for `tag` if there is one."""
    base = getattr(font[table], field)
    if tag in records:
        return base + half_away(delta_of(instancer, records[tag]))
    return base


def main(argv):
    """Usage: fonttools_metrics.py <font> <face> <stride> <first> <spec>..."""
    if len(argv) < 5:
        sys.stderr.write("usage: %s <font> <face> <stride> <first> [spec]...\n"
                         % argv[0])
        return 2
    path = argv[1]
    face = int(argv[2])
    stride = max(1, int(argv[3]))
    first = int(argv[4])

    font = TTFont(path, fontNumber=face, lazy=True)
    if "fvar" not in font:
        print("metrics: no fvar")
        return 0
    for tag, low, default, high in axes_of(font):
        print("axis: %s %r %r %r" % (tag, low, default, high))
    print("metrics: has HVAR %d, has MVAR %d, has gvar %d" % (
        "HVAR" in font, "MVAR" in font, "gvar" in font))
    order = font.getGlyphOrder()
    axes = font["fvar"].axes
    mvar = font["MVAR"].table if "MVAR" in font else None
    records = {}
    if mvar is not None:
        records = {r.ValueTag: r.VarIdx for r in mvar.ValueRecord}
    hvar = font["HVAR"].table if "HVAR" in font else None
    for number, spec in enumerate(argv[5:]):
        text, _, given = spec.partition("@")
        user = parse_location(text)
        location = normalised(font, user)
        print("location %d: %s" % (number, text))
        print("variation: %d axes, normalised %s" % (
            len(location), " ".join(str(otRound(location[axis.axisTag] * 16384))
                                     for axis in axes)))
        if given:
            used = {axis.axisTag: int(value) / 16384.0
                    for axis, value in zip(axes, given.split(","))}
        else:
            used = location
        glyph_set = font.getGlyphSet(location=used, normalized=True)
        for index in range(first, len(order), stride):
            name = order[index]
            try:
                glyph = glyph_set[name]
                base = font["hmtx"][name][0]
                if hvar is None and "gvar" in font and any(used.values()):
                    # With no `HVAR` the width is the phantom points', which
                    # fontTools sets only once the instance has been made.
                    glyph._getGlyphInstance()
                    print("glyph %d advance %d" % (index, glyph.width))
                else:
                    print("glyph %d advance %d" % (
                        index, base + half_away(glyph.width - base)))
            except Exception as why:  # noqa: BLE001 - the reason is the output
                print("glyph %d advance refused %s" % (index, type(why).__name__))
            source = font["glyf"][name] if "glyf" in font else None
            if source is not None and source.isComposite() and any(
                    c.flags & 0x200 for c in source.components):
                # FreeType takes such a glyph's advance from that component, which
                # neither fontTools nor `HVAR` does: said so the differential can
                # name the glyphs it is a known difference for.
                print("glyph %d usemymetrics" % index)
            if not any(used.values()):
                # The default instance: the bearing is `hmtx`'s, which needs no
                # table to say so.
                print("glyph %d bearing %d" % (index, font["hmtx"][name][1]))
            elif hvar is not None and hvar.LsbMap is not None:
                instancer = VarStoreInstancer(hvar.VarStore, axes, used)
                lsb = font["hmtx"][name][1]
                varidx = hvar.LsbMap.mapping[name]
                print("glyph %d bearing %d" % (index, lsb + half_away(
                    delta_of(instancer, varidx))))
            else:
                print("glyph %d bearing none" % index)
        if mvar is not None:
            instancer = VarStoreInstancer(mvar.VarStore, axes, used)
        else:
            instancer = None
        hhea = font["hhea"]
        print("line hhea %d %d %d" % (
            line_value(font, instancer, records, "hhea", "ascent", "hasc"),
            line_value(font, instancer, records, "hhea", "descent", "hdsc"),
            line_value(font, instancer, records, "hhea", "lineGap", "hlgp")))
        if "OS/2" in font:
            os2 = font["OS/2"]
            print("line win %d %d" % (
                line_value(font, instancer, records, "OS/2", "usWinAscent", "hcla"),
                -(os2.usWinDescent + (half_away(delta_of(instancer,
                    records["hcld"])) if "hcld" in records else 0))))
            print("line typo %d %d %d" % (
                line_value(font, instancer, records, "OS/2", "sTypoAscender",
                           "tasc"),
                line_value(font, instancer, records, "OS/2", "sTypoDescender",
                           "tdsc"),
                line_value(font, instancer, records, "OS/2", "sTypoLineGap",
                           "tlgp")))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
