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
"""What a font's `COLR` lowers to in a scene (story 14c), stated a second time from fontTools.

    fonttools_colr_scene.py <out.json> <font>...

This is an independent statement of scene_color.h's rules, over fontTools'
own decompilation of `COLR`, in the shape `font-colr-scene` prints: each glyph
with a color description lowered at an em of the face's units per em at the
origin (so a design map `T = (A, b)` becomes the item map `L = diag(1, -1) A
diag(1, -1)`, `t = (b_x, -b_y)`), version 1 where the glyph has a record, else
version 0. The transforms are fontTools' own (`Paint.getTransform()`, which
computes a skew with `tan` and a rotation with `cos` and `sin`), composed here,
so the map is a second reading of the specification's transforms, not of this
library's. Refusals follow the first refused node in the order the walk visits
them. Writes {path: {"glyphs": {gid: answer}}}. Runs in the fonttools image.
"""

import json
import sys

from fontTools.ttLib import TTFont
from fontTools.ttLib.tables import otTables as ot

PF = ot.PaintFormat
MAX_DEPTH = 64
MAX_OPS = 64

GRADIENTS = {PF.PaintLinearGradient, PF.PaintRadialGradient, PF.PaintSweepGradient}
TRANSFORMS = {PF.PaintTransform, PF.PaintTranslate, PF.PaintScale, PF.PaintScaleAroundCenter,
              PF.PaintScaleUniform, PF.PaintScaleUniformAroundCenter, PF.PaintRotate,
              PF.PaintRotateAroundCenter, PF.PaintSkew, PF.PaintSkewAroundCenter}
# The scene's operators (shape-scene list.h) for COLR's modes 0 to 12.
OPERATOR = {0: 1, 1: 2, 2: 3, 3: 0, 4: 4, 5: 5, 6: 6, 7: 7, 8: 8, 9: 9, 10: 10, 11: 11, 12: 12}
FILL, CLIP_PUSH, CLIP_POP, GROUP_PUSH, GROUP_POP = 1, 2, 3, 4, 5
PAINT_FOREGROUND, PAINT_PALETTE = 2, 3
SRC_V0, SRC_V1 = 1, 2
GRADIENT, BLEND_MODE, VARIABLE, UNBOUNDED, NO_RECORD = 1, 2, 3, 4, 5


class Refused(Exception):
    def __init__(self, why, fmt, mode=0):
        self.why, self.fmt, self.mode = why, fmt, mode


class Limit(Exception):
    pass


def compose(outer, inner):
    """outer(inner(p)) for affine maps (a, b, c, d, e, f): x' = a x + c y + e, y' = b x + d y + f."""
    a1, b1, c1, d1, e1, f1 = outer
    a2, b2, c2, d2, e2, f2 = inner
    return (a1 * a2 + c1 * b2, b1 * a2 + d1 * b2, a1 * c2 + c1 * d2, b1 * c2 + d1 * d2,
            a1 * e2 + c1 * f2 + e1, b1 * e2 + d1 * f2 + f1)


IDENTITY = (1.0, 0.0, 0.0, 1.0, 0.0, 0.0)


def item_map(chain):
    t = IDENTITY
    for m in reversed(chain):   # chain[0] is the outermost: T = chain[0] . chain[1] . ...
        t = compose(m, t)
    a, b, c, d, e, f = t        # A = [[a, c], [b, d]], b = (e, f)
    # L = diag(1, -1) A diag(1, -1); t = (e, -f).  [m00, m01, m10, m11, t0, t1]
    return [a, -c, -b, d, e, -f]


def paint_of(index, alpha):
    alpha = min(max(alpha, 0.0), 1.0)
    if index == 0xFFFF:
        return PAINT_FOREGROUND, 0, alpha
    return PAINT_PALETTE, index, alpha


class Lower:
    def __init__(self, font, gid):
        self.gid = gid
        self.items = []
        colr = font["COLR"]
        self.v1 = {}
        table = getattr(colr, "table", None)
        if table is not None and getattr(table, "BaseGlyphList", None):
            for rec in table.BaseGlyphList.BaseGlyphPaintRecord:
                self.v1[rec.BaseGlyph] = rec.Paint
        self.layer_list = table.LayerList.Paint if table is not None and getattr(table, "LayerList", None) else []

    def fill(self, glyph, paint, chain):
        kind, entry, alpha = paint
        self.items.append([FILL, self.gid[glyph], kind, entry, alpha, 0] + item_map(chain))

    def read(self, paint, depth):
        if depth > MAX_DEPTH:
            raise Limit()
        fmt = paint.Format
        if PF(fmt).name.startswith("PaintVar"):
            raise Refused(VARIABLE, fmt)
        return fmt

    def node(self, paint, depth, chain):
        fmt = self.read(paint, depth)
        if fmt == PF.PaintColrLayers:
            for i in range(paint.FirstLayerIndex, paint.FirstLayerIndex + paint.NumLayers):
                self.node(self.layer_list[i], depth + 1, chain)
        elif fmt == PF.PaintSolid:
            raise Refused(UNBOUNDED, fmt)
        elif fmt in GRADIENTS:
            raise Refused(GRADIENT, fmt)
        elif fmt == PF.PaintGlyph:
            child = paint.Paint
            d = depth + 1
            while True:
                cf = self.read(child, d)
                if cf not in TRANSFORMS:
                    break
                child = child.Paint
                d += 1
            if cf == PF.PaintSolid:
                self.fill(paint.Glyph, paint_of(child.PaletteIndex, child.Alpha), chain)
            else:
                self.items.append([CLIP_PUSH, self.gid[paint.Glyph], 0, 0, 0, 0] + item_map(chain))
                self.node(paint.Paint, depth + 1, chain)
                self.items.append([CLIP_POP, -1, 0, 0, 0, 0] + item_map([]))
        elif fmt == PF.PaintColrGlyph:
            root = self.v1.get(paint.Glyph)
            if root is None:
                raise Refused(NO_RECORD, fmt)
            self.node(root, depth + 1, chain)
        elif fmt == PF.PaintComposite:
            mode = int(paint.CompositeMode)
            if mode > 12:
                raise Refused(BLEND_MODE, fmt, mode)
            self.items.append([GROUP_PUSH, -1, 0, 0, 0, 0] + item_map([]))
            self.node(paint.BackdropPaint, depth + 1, chain)
            self.items.append([GROUP_PUSH, -1, 0, 0, 0, OPERATOR[mode]] + item_map([]))
            self.node(paint.SourcePaint, depth + 1, chain)
            self.items.append([GROUP_POP, -1, 0, 0, 0, 0] + item_map([]))
            self.items.append([GROUP_POP, -1, 0, 0, 0, 0] + item_map([]))
        elif fmt in TRANSFORMS:
            if len(chain) == MAX_OPS:
                raise Limit()
            self.node(paint.Paint, depth + 1, chain + [tuple(paint.getTransform())])
        else:
            raise ValueError("format %d" % fmt)


def v0_layers(font):
    colr = font["COLR"]
    out = {}
    if getattr(colr, "version", 0) == 0:
        for base, layers in colr.ColorLayers.items():
            out[base] = [(layer.name, layer.colorID) for layer in layers]
        return out
    table = colr.table
    if getattr(table, "BaseGlyphRecordArray", None) and getattr(table, "LayerRecordArray", None):
        layers = table.LayerRecordArray.LayerRecord
        for rec in table.BaseGlyphRecordArray.BaseGlyphRecord:
            out[rec.BaseGlyph] = [(l.LayerGlyph, l.PaletteIndex)
                                  for l in layers[rec.FirstLayerIndex:rec.FirstLayerIndex + rec.NumLayers]]
    return out


def answer(path):
    font = TTFont(path, lazy=False)
    if "COLR" not in font:
        return {"glyphs": {}}
    order = font.getGlyphOrder()
    gid = {name: i for i, name in enumerate(order)}
    v0 = v0_layers(font)
    glyphs = {}
    for name in order:
        low = Lower(font, gid)
        if name in low.v1:
            try:
                low.node(low.v1[name], 0, [])
                glyphs[str(gid[name])] = {"src": SRC_V1, "refusal": 0, "format": 0, "mode": 0, "items": low.items}
            except Refused as r:
                glyphs[str(gid[name])] = {"src": SRC_V1, "refusal": r.why, "format": int(r.fmt), "mode": r.mode,
                                          "items": []}
            except (Limit, RecursionError):
                glyphs[str(gid[name])] = {"error": "limit"}
        elif v0.get(name):
            for layer, index in v0[name]:
                low.fill(layer, paint_of(index, 1.0), [])
            glyphs[str(gid[name])] = {"src": SRC_V0, "refusal": 0, "format": 0, "mode": 0, "items": low.items}
    return {"glyphs": glyphs}


def main(argv):
    out = argv[1]
    result = {path: answer(path) for path in argv[2:]}
    with open(out, "w", encoding="utf-8") as handle:
        json.dump(result, handle)


if __name__ == "__main__":
    sys.setrecursionlimit(10000)
    main(sys.argv)
