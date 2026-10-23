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
"""What a font's `COLR` lowers to in a scene (stories 14c and P4), stated a second time from fontTools.

    fonttools_colr_scene.py [--coords coords.json] [--encoded] <out.json> <font>...

This is an independent statement of scene_color.h's rules L1 to L11 over
fontTools' own decompilation of `COLR`, in the shape `font-colr-scene` prints:
each glyph with a color description lowered at an em of the face's units per em
at the origin (so a design map `T = (A, b)` becomes the item map `L = diag(1, -1)
A diag(1, -1)`, `t = (b_x, -b_y)`, and a gradient's map `D . T` with `D =
diag(1, -1)`), version 1 where the glyph has a record, else version 0.

Every paint's numbers are read as fontTools reads them: the item variation
store's deltas for the instance (`--coords`: a map from a font to the normalised
coordinates, 2.14 integers in axis order, the instance the face resource is
interned at, L1), each added to the stored value and rounded half up, which is
the arithmetic `fonttools_color.py` and `fonttools_metrics.py` use. The
transforms are fontTools' own (`Paint.getTransform()`, which computes a skew
with `tan` and a rotation with `cos` and `sin`), composed here, so the map is a
second reading of the specification's transforms, not of this library's.

The rules, restated (a box of a glyph named by `PaintColrGlyph` clips too, under the transforms at the reference): a `PaintGlyph` whose child reaches a solid or a gradient
through transforms is a fill of the glyph's run (the item map is the transforms
above and including the glyph, a gradient's own map the ones between); any other
child puts a clip of the glyph around it, a bound; a solid or gradient reached
any other way fills the plane under the innermost bound (item map the transforms
above that bound, a gradient's own map the rest) or, with no bound in force, is
refused `UNBOUNDED`. A version 1 glyph is one isolated source-over group in
linear light (sRGB primaries; `--encoded`: the sRGB transfer, gradients
unpremultiplied) around the clip of its `ClipList` box where it has one. A
`PaintComposite` mode 0 to 12 maps through 14c's table, 13 to 27 by identity and
anything else to clear. Refusals follow the first refused node in the order the
walk visits them. Writes {path: {"glyphs": {gid: answer}}}. Runs in the
fonttools image.
"""

import json
import math
import sys

from fontTools.misc.fixedTools import otRound
from fontTools.ttLib import TTFont
from fontTools.ttLib.tables import otTables as ot

from fonttools_color import Graph

PF = ot.PaintFormat
MAX_DEPTH = 64
MAX_OPS = 64
MAX_NODES = 65536

GRADIENT_FORMATS = {PF.PaintLinearGradient, PF.PaintRadialGradient, PF.PaintSweepGradient}
LEAVES = GRADIENT_FORMATS | {PF.PaintSolid}
TRANSFORMS = {PF.PaintTransform, PF.PaintTranslate, PF.PaintScale, PF.PaintScaleAroundCenter,
              PF.PaintScaleUniform, PF.PaintScaleUniformAroundCenter, PF.PaintRotate,
              PF.PaintRotateAroundCenter, PF.PaintSkew, PF.PaintSkewAroundCenter}
# The scene's operators (shape-scene list.h) for COLR's modes 0 to 12; 13 to 27 are the same numbers.
OPERATOR = {0: 1, 1: 2, 2: 3, 3: 0, 4: 4, 5: 5, 6: 6, 7: 7, 8: 8, 9: 9, 10: 10, 11: 11, 12: 12}
CLEAR = 1
FILL, CLIP_PUSH, CLIP_POP, GROUP_PUSH, GROUP_POP = 1, 2, 3, 4, 5
PAINT_GRADIENT, PAINT_FOREGROUND, PAINT_PALETTE = 1, 2, 3
PLANE, RECT = -2, -3
SRGB, LINEAR, SRGB_TRANSFER = 1, 1, 2
SRC_V0, SRC_V1 = 1, 2
UNBOUNDED, NO_RECORD = 1, 2


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


def chain_map(chain):
    t = IDENTITY
    for m in reversed(chain):   # chain[0] is the outermost: T = chain[0] . chain[1] . ...
        t = compose(m, t)
    return t


def item_map(chain):
    """L = diag(1, -1) A diag(1, -1), t = (e, -f), as [m00, m01, m10, m11, t0, t1]."""
    a, b, c, d, e, f = chain_map(chain)
    return [a, -c, -b, d, e, -f]


def design_map(chain):
    """diag(1, -1) . T: where a design point under T lands in the list (the gradient's and a box's map)."""
    a, b, c, d, e, f = chain_map(chain)
    return [a, c, -b, -d, e, -f]


class Raw:
    """A paint node as fontTools holds it, with the resolved integers the lowering reads (L1)."""

    def __init__(self, graph, paint):
        self.paint = paint
        self.format = int(paint.Format)
        self.variable = bool(self.format in VAR_FORMATS)
        self.base = self.format - 1 if self.variable else self.format
        self.graph = graph
        base = getattr(paint, "VarIndexBase", 0xFFFFFFFF)
        self.v = None
        if self.base in LEAVES and self.base != PF.PaintSolid:
            names = ("x0", "y0", "x1", "y1", "x2", "y2") if self.base == PF.PaintLinearGradient else (
                ("x0", "y0", "r0", "x1", "y1", "r1") if self.base == PF.PaintRadialGradient else
                ("centerX", "centerY", "startAngle", "endAngle"))
            self.v = [graph.native(n, getattr(paint, n)) + graph.delta(base, k) for k, n in enumerate(names)]
        self.alpha = None
        if self.base == PF.PaintSolid:
            self.alpha = otRound(paint.Alpha * 16384) + graph.delta(base, 0)

    def stops(self):
        out = []
        for stop in self.paint.ColorLine.ColorStop:
            base = getattr(stop, "VarIndexBase", 0xFFFFFFFF)
            out.append((otRound(stop.StopOffset * 16384) + self.graph.delta(base, 0), stop.PaletteIndex,
                        otRound(stop.Alpha * 16384) + self.graph.delta(base, 1)))
        return out

    def transform(self):
        """fontTools' own transform of a transform paint, from the resolved numbers."""
        g, p, base = self.graph, self.paint, getattr(self.paint, "VarIndexBase", 0xFFFFFFFF)
        fmt = self.base

        class Resolved:
            pass

        r = Resolved()
        r.Format = PF(fmt)
        names = {
            PF.PaintTranslate: ("dx", "dy"),
            PF.PaintScale: ("scaleX", "scaleY"),
            PF.PaintScaleAroundCenter: ("scaleX", "scaleY", "centerX", "centerY"),
            PF.PaintScaleUniform: ("scale",),
            PF.PaintScaleUniformAroundCenter: ("scale", "centerX", "centerY"),
            PF.PaintRotate: ("angle",),
            PF.PaintRotateAroundCenter: ("angle", "centerX", "centerY"),
            PF.PaintSkew: ("xSkewAngle", "ySkewAngle"),
            PF.PaintSkewAroundCenter: ("xSkewAngle", "ySkewAngle", "centerX", "centerY"),
        }
        if fmt == PF.PaintTransform:
            t = p.Transform
            tb = getattr(t, "VarIndexBase", 0xFFFFFFFF)

            class T:
                pass

            tt = T()
            for k, n in enumerate(("xx", "yx", "xy", "yy", "dx", "dy")):
                setattr(tt, n, (otRound(getattr(t, n) * 65536) + g.delta(tb, k)) / 65536)
            r.Transform = tt
        else:
            for k, n in enumerate(names[fmt]):
                raw = g.native(n, getattr(p, n)) + g.delta(base, k)
                if n in ("scaleX", "scaleY", "scale"):
                    value = raw / 16384
                elif n in ("angle", "xSkewAngle", "ySkewAngle"):
                    value = raw / 16384 * 180
                else:
                    value = raw
                setattr(r, n, value)
        return tuple(ot.Paint.getTransform(r))


VAR_FORMATS = {PF[n] for n in dir(PF) if n.startswith("PaintVar")}
VAR_FORMATS = {int(f) for f in VAR_FORMATS}


def clamp_alpha(raw):
    return min(max(raw / 16384, 0.0), 1.0)


def paint_of(index, raw_alpha):
    alpha = clamp_alpha(raw_alpha)
    if index == 0xFFFF:
        return PAINT_FOREGROUND, 0, alpha
    return PAINT_PALETTE, index, alpha


class Lower:
    def __init__(self, font, graph, gid, encoded):
        self.gid = gid
        self.graph = graph
        self.encoded = encoded
        self.items = []
        self.chain = []
        self.bounded = False
        self.bound_ops = 0
        self.nodes = 0
        self.clips = graph.clips()
        self.v1 = graph.base
        colr = font["COLR"].table
        self.layer_list = colr.LayerList.Paint if getattr(colr, "LayerList", None) else []

    # --- the items -------------------------------------------------------------------------------------------

    def gradient_extra(self, raw, t_chain):
        stops = sorted(raw.stops(), key=lambda s: s[0])   # the store's stable sort by offset
        kind = {PF.PaintLinearGradient: 1, PF.PaintRadialGradient: 2, PF.PaintSweepGradient: 3}[raw.base]
        v = raw.v
        if kind == 1:
            geo = [v[0], v[1], v[2], v[3], v[4] - v[0], v[5] - v[1]]
        elif kind == 2:
            geo = list(v)
        else:
            # The stored angles are biased by one half turn (add 1.0, multiply by 180 degrees).
            geo = [v[0], v[1], (v[2] + 16384) * 180.0 / 16384.0, (v[3] + 16384) * 180.0 / 16384.0]
        extend = int(raw.paint.ColorLine.Extend)
        return {"g": {
            "kind": kind,
            "extend": extend if extend <= 2 else 0,
            "space": [SRGB, SRGB_TRANSFER if self.encoded else LINEAR],
            "premul": 0 if self.encoded else 1,
            "geo": [float(x) for x in geo],
            "map": design_map(t_chain),
            "stops": [[offset / 16384, *self.stop_paint(index, alpha)] for offset, index, alpha in stops],
        }}

    @staticmethod
    def stop_paint(index, alpha):
        kind, entry, a = paint_of(index, alpha)
        return [kind, entry, a]

    def leaf(self, raw, shape, item_chain, t_chain):
        """A fill of @p shape (a glyph id, or the plane) with a solid or a gradient leaf."""
        m = item_map(item_chain)
        if raw.base == PF.PaintSolid:
            kind, entry, alpha = paint_of(raw.paint.PaletteIndex, raw.alpha)
            self.items.append([FILL, shape, kind, entry, alpha, 0] + m + [None])
        else:
            self.items.append([FILL, shape, PAINT_GRADIENT, 0, 1.0, 0] + m + [self.gradient_extra(raw, t_chain)])

    def group_push(self, operator, extra=None):
        self.items.append([GROUP_PUSH, -1, 0, 0, 0, operator] + item_map([]) + [extra])

    def group_pop(self):
        self.items.append([GROUP_POP, -1, 0, 0, 0, 0] + item_map([]) + [None])

    # --- the walk ---------------------------------------------------------------------------------------------

    def read(self, paint, depth):
        self.nodes += 1
        if depth > MAX_DEPTH or self.nodes > MAX_NODES:
            raise Limit()
        return Raw(self.graph, paint)

    def node(self, paint, depth):
        raw = self.read(paint, depth)
        fmt = raw.base
        if fmt == PF.PaintColrLayers:
            for i in range(paint.FirstLayerIndex, paint.FirstLayerIndex + paint.NumLayers):
                self.node(self.layer_list[i], depth + 1)
        elif fmt in LEAVES:
            if not self.bounded:
                raise Refused(UNBOUNDED, int(fmt))
            self.leaf(raw, PLANE, self.chain[:self.bound_ops], self.chain[self.bound_ops:])
        elif fmt == PF.PaintGlyph:
            between = []
            child = paint.Paint
            d = depth + 1
            while True:
                c = self.read(child, d)
                if c.base not in TRANSFORMS:
                    break
                if len(between) == MAX_OPS:
                    raise Limit()
                between.append(c.transform())
                child = child.Paint
                d += 1
            gid = self.graph.font.getGlyphID(paint.Glyph)
            if c.base in LEAVES:
                self.leaf(c, gid, self.chain, between)
            else:
                self.items.append([CLIP_PUSH, gid, 0, 0, 0, 0] + item_map(self.chain) + [None])
                was = (self.bounded, self.bound_ops)
                self.bounded, self.bound_ops = True, len(self.chain)
                self.node(paint.Paint, depth + 1)
                self.bounded, self.bound_ops = was
                self.items.append([CLIP_POP, -1, 0, 0, 0, 0] + item_map([]) + [None])
        elif fmt == PF.PaintColrGlyph:
            root = self.v1.get(paint.Glyph)
            if root is None:
                raise Refused(NO_RECORD, int(fmt))
            # The referenced glyph's own clip box, under the transforms in force here, bounds its graph (L5).
            box = self.clips.get(str(self.graph.font.getGlyphID(paint.Glyph)))
            was = (self.bounded, self.bound_ops)
            if box is not None:
                x0, y0, x1, y1 = box
                self.items.append([CLIP_PUSH, RECT, 0, 0, 0, 0] + design_map(self.chain)
                                  + [{"box": [float(x0), float(y0), float(x1), float(y1)]}])
                self.bounded, self.bound_ops = True, len(self.chain)
            self.node(root, depth + 1)
            if box is not None:
                self.items.append([CLIP_POP, -1, 0, 0, 0, 0] + item_map([]) + [None])
            self.bounded, self.bound_ops = was
        elif fmt == PF.PaintComposite:
            mode = int(paint.CompositeMode)
            operator = OPERATOR[mode] if mode <= 12 else (mode if mode <= 27 else CLEAR)
            self.group_push(0)
            self.node(paint.BackdropPaint, depth + 1)
            self.group_push(operator)
            self.node(paint.SourcePaint, depth + 1)
            self.group_pop()
            self.group_pop()
        elif fmt in TRANSFORMS:
            if len(self.chain) == MAX_OPS:
                raise Limit()
            self.chain.append(raw.transform())
            self.node(paint.Paint, depth + 1)
            self.chain.pop()
        else:
            raise ValueError("format %d" % fmt)

    def glyph(self, name, root):
        """A version 1 glyph: the group, the clip of its box, the graph."""
        self.group_push(0, {"space": [SRGB, SRGB_TRANSFER if self.encoded else LINEAR]})
        box = self.clips.get(str(self.gid[name]))
        if box is not None:
            x0, y0, x1, y1 = box
            self.items.append([CLIP_PUSH, RECT, 0, 0, 0, 0] + design_map([]) + [{"box": [float(x0), float(y0), float(x1), float(y1)]}])
            self.bounded, self.bound_ops = True, 0
        self.node(root, 0)
        if box is not None:
            self.items.append([CLIP_POP, -1, 0, 0, 0, 0] + item_map([]) + [None])
        self.group_pop()


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


def answer(path, coords, encoded):
    font = TTFont(path, lazy=False)
    if "COLR" not in font:
        return {"glyphs": {}}
    order = font.getGlyphOrder()
    gid = {name: i for i, name in enumerate(order)}
    v0 = v0_layers(font)
    normal = None
    if coords is not None and "fvar" in font:
        normal = {axis.axisTag: value / 16384.0 for axis, value in zip(font["fvar"].axes, coords)}
    graph = None
    if getattr(font["COLR"], "version", 0) == 1:
        graph = Graph(font, normal)
    glyphs = {}
    for name in order:
        if graph is not None and name in graph.base:
            low = Lower(font, graph, gid, encoded)
            try:
                low.glyph(name, graph.base[name])
                glyphs[str(gid[name])] = {"src": SRC_V1, "refusal": 0, "format": 0, "mode": 0, "items": low.items}
            except Refused as r:
                glyphs[str(gid[name])] = {"src": SRC_V1, "refusal": r.why, "format": int(r.fmt), "mode": r.mode,
                                          "items": []}
            except (Limit, RecursionError):
                glyphs[str(gid[name])] = {"error": "limit"}
        elif v0.get(name):
            items = []
            for layer, index in v0[name]:
                kind, entry, alpha = paint_of(index, 16384)
                items.append([FILL, gid[layer], kind, entry, alpha, 0] + item_map([]) + [None])
            glyphs[str(gid[name])] = {"src": SRC_V0, "refusal": 0, "format": 0, "mode": 0, "items": items}
    return {"glyphs": glyphs}


def main(argv):
    args = argv[1:]
    coords = {}
    encoded = False
    while args and args[0] in ("--coords", "--encoded"):
        if args[0] == "--encoded":
            encoded = True
            args = args[1:]
        else:
            with open(args[1], encoding="utf-8") as handle:
                coords = json.load(handle)
            args = args[2:]
    out = args[0]
    result = {path: answer(path, coords.get(path), encoded) for path in args[1:]}
    with open(out, "w", encoding="utf-8") as handle:
        json.dump(result, handle)


if __name__ == "__main__":
    sys.setrecursionlimit(10000)
    main(sys.argv)
