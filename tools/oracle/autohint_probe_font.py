#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Font.
"""Write a TrueType font whose glyphs are the shapes in a JSON file, for autohint_probe.py.

    autohint_probe_font.py <spec.json> <out.ttf>

The spec is {"upem": 1000, "glyphs": [{"contours": [[[x, y, on], ...], ...]}, ...]}. Glyph n (from 0) is
mapped to the nth code point of CODEPOINTS: Latin letters with no accent, because a hinter may treat an accented
letter's top edge on its own and a shape under test must meet the plain rules. Unless the spec says "base": false the
font also has the plain letters A-Z and a-z as boxes, which a hinter reads its zones and standard width from; without
them it leaves the glyphs unhinted. There are no instructions. Run inside the fontTools image.
"""
import json
import sys

from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib.tables._g_l_y_f import Glyph, GlyphCoordinates


# U+0250..U+02AF only. FreeType gives the phonetic extensions from U+1D00 their own style (small capitals, with
# blue zones of their own), so a shape mapped there is hinted by other rules than the plain Latin ones under test.
CODEPOINTS = list(range(0x250, 0x2B0))


# The Greek probes use code points FreeType's Greek style covers and that carry no accent: archaic and symbol letters.
GREEK_CODEPOINTS = list(range(0x3D8, 0x3F0))


def greek_letters(upem, overshoot=12, heights=None):
    """The Greek letters FreeType reads the Greek zones from, as boxes and round letters with the Latin proportions."""
    u = upem / 1000.0
    h = heights or {}
    cap, xh, asc, desc, over = (int(h.get("cap", 700) * u), int(h.get("xh", 500) * u), int(h.get("asc", 720) * u),
                                int(h.get("desc", -200) * u), int(overshoot * u))
    w = int(500 * u)
    out = []

    def box(x0, y0, x1, y1):
        return [[x0, y0, 1], [x0, y1, 1], [x1, y1, 1], [x1, y0, 1]]

    for cp in list(range(0x391, 0x3A2)) + list(range(0x3A3, 0x3AA)):
        if cp in (0x398, 0x39F, 0x3A9, 0x3A3):
            out.append((cp, [ellipse(int(50 * u), -over, w, cap + over)]))
        else:
            out.append((cp, [box(int(50 * u), 0, w, cap)]))
    for cp in range(0x3B1, 0x3CA):
        ch = chr(cp)
        if ch in "αεοσςω":
            out.append((cp, [ellipse(int(50 * u), -over, w, xh + over)]))
            continue
        top = asc if ch in "βθδζλξ" else xh
        bottom = desc if ch in "γημρφχψςβζξ" else 0
        out.append((cp, [box(int(50 * u), bottom, w, top)]))
    return out


def glyph_of(contours):
    from fontTools.ttLib.tables._g_l_y_f import Glyph
    g = Glyph()
    coords, ends, flags = [], [], []
    for c in contours:
        for x, y, on in c:
            coords.append((x, y))
            flags.append(1 if on else 0)
        ends.append(len(coords) - 1)
    g.numberOfContours = len(ends)
    g.coordinates = GlyphCoordinates(coords)
    g.flags = bytearray(flags)
    g.endPtsOfContours = ends
    g.program = __import__("fontTools.ttLib.tables.ttProgram", fromlist=["Program"]).Program()
    g.program.fromBytecode(b"")
    return g


def ellipse(x0, y0, x1, y1):
    """A clockwise quadratic ellipse through eight points, extrema on the box."""
    cx, cy = (x0 + x1) // 2, (y0 + y1) // 2
    # The control points sit a unit inside the box, so that the extreme of the letter is its on-curve point alone.
    return [[x0, cy, 1], [x0, y1 - 1, 0], [cx, y1, 1], [x1, y1 - 1, 0], [x1, cy, 1], [x1, y0 + 1, 0], [cx, y0, 1],
            [x0, y0 + 1, 0]]


def base_letters(upem, overshoot=12, heights=None):
    """The Latin letters a script's zones and standard width are read from: boxes, with a round `o`, in the
    proportions of an ordinary text face (cap height 0.7, x-height 0.5, ascender 0.72, descender -0.2)."""
    u = upem / 1000.0
    h = heights or {}
    cap, xh, asc, desc, over = (int(h.get("cap", 700) * u), int(h.get("xh", 500) * u), int(h.get("asc", 720) * u),
                                int(h.get("desc", -200) * u), int(overshoot * u))
    out = []
    w = int(500 * u)
    stem = int(80 * u)

    def box(x0, y0, x1, y1):
        return [[x0, y0, 1], [x0, y1, 1], [x1, y1, 1], [x1, y0, 1]]

    for c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ":
        if c in "COSQG":
            out.append((ord(c), [ellipse(int(50 * u), -over, w, cap + over)]))
        else:
            out.append((ord(c), [box(int(50 * u), 0, w, cap)]))
    for c in "abcdefghijklmnopqrstuvwxyz":
        if c in "oecas":
            out.append((ord(c), [ellipse(int(50 * u), -over, w, xh + over)]))
            continue
        top = asc if c in "bdfhkl" else xh
        bottom = desc if c in "gjpqy" else 0
        if c in "ij":
            # A dot on the ascender, as a real i and j have; FreeType reads its ascender zone from these tops too.
            out.append((ord(c), [box(int(50 * u), bottom, int(130 * u), xh),
                                 box(int(50 * u), asc - int(120 * u), int(130 * u), asc)]))
            continue
        out.append((ord(c), [box(int(50 * u), bottom, w, top)]))
    return out


def main():
    spec = json.load(open(sys.argv[1]))
    upem = spec.get("upem", 1000)
    n = len(spec["glyphs"])
    greek = spec.get("script") == "greek"
    codepoints = GREEK_CODEPOINTS if greek else CODEPOINTS
    letters = greek_letters if greek else base_letters
    names = [".notdef"] + ["g%d" % i for i in range(n)]
    assert n <= len(codepoints), "at most %d glyphs per font" % len(codepoints)
    cmap = {codepoints[i]: "g%d" % i for i in range(n)}
    base = spec.get("base", True)
    if base:
        for cp, contours in letters(upem, spec.get("overshoot", 12), spec.get("heights")):
            name = "u%04X" % cp
            names.append(name)
            cmap[cp] = name
    fb = FontBuilder(upem, isTTF=True)
    fb.setupGlyphOrder(names)
    fb.setupCharacterMap(cmap)
    glyphs = {".notdef": TTGlyphPen(None).glyph()}
    for i, g in enumerate(spec["glyphs"]):
        glyphs["g%d" % i] = glyph_of(g["contours"])
    if base:
        for cp, contours in letters(upem, spec.get("overshoot", 12), spec.get("heights")):
            glyphs["u%04X" % cp] = glyph_of(contours)
    fb.setupGlyf(glyphs)
    fb.setupHorizontalMetrics({name: (upem, 0) for name in names})
    fb.setupHorizontalHeader(ascent=int(upem * 0.8), descent=-int(upem * 0.2))
    fb.setupNameTable({"familyName": "Probe", "styleName": "Regular"})
    fb.setupOS2(sTypoAscender=int(upem * 0.8), sTypoDescender=-int(upem * 0.2), usWinAscent=int(upem * 0.8),
                usWinDescent=int(upem * 0.2))
    fb.setupPost()
    fb.save(sys.argv[2])


main()
