#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Font.
"""A model of the automatic hinter's y fitting of one Latin glyph (autohinter plan, stages A2 and A3).

Segments (runs of points whose outgoing vectors are horizontal), edges (segments at one height), stems (an edge
linked to the opposite edge across a stem), blue-zone alignment, the placement of stems from an anchor, the
alignment of the points on edges, and the interpolation of the points between them. Fitted to FreeType's light
automatic hinter by experiment (`autohint_edges_diff.py`); analysis arithmetic is in Python floats and nothing
here is the library's code. Coordinates are font units in, 26.6 pixels out.
"""
import math

import autohint_model as model
from autohint_model import mulfix, muldiv

SKIP_ZONE = None
UP, DOWN, LEFT, RIGHT = 1, -1, -2, 2   # outgoing vector directions: vertical or horizontal


def pixround(v):
    return (int(v) + 32) & ~63


def direction(dx, dy):
    """The direction of a vector, or 0 when it is neither near horizontal nor near vertical (a ratio of 14 to 1)."""
    ax, ay = abs(dx), abs(dy)
    if ax == 0 and ay == 0:
        return 0
    if ax >= 14 * ay:
        return RIGHT if dx > 0 else LEFT
    if ay >= 14 * ax:
        return UP if dy > 0 else DOWN
    return 0


class Glyph:
    def __init__(self, glyph):
        self.pts = glyph["pts"]
        self.ends = glyph["ends"]
        self.n = len(self.pts)
        self.contour = []          # contour index of each point
        self.next = [0] * self.n
        self.prev = [0] * self.n
        start = 0
        for c, end in enumerate(self.ends):
            for i in range(start, end + 1):
                self.contour.append(c)
                self.next[i] = start if i == end else i + 1
                self.prev[i] = end if i == start else i - 1
            start = end + 1
        self.out_dir = [direction(self.pts[self.next[i]][0] - self.pts[i][0], self.pts[self.next[i]][1] - self.pts[i][1])
                        for i in range(self.n)]
        self.in_dir = [direction(self.pts[i][0] - self.pts[self.prev[i]][0], self.pts[i][1] - self.pts[self.prev[i]][1])
                       for i in range(self.n)]
        area = 0.0
        for i in range(self.n):
            j = self.next[i]
            area += self.pts[i][0] * self.pts[j][1] - self.pts[j][0] * self.pts[i][1]
        self.clockwise = area < 0

    def is_weak(self, i):
        """Off the curve, inside a straight run, or at a corner too slight to matter."""
        if not self.on(i):
            return True
        if self.in_dir[i] == self.out_dir[i] and self.out_dir[i] != 0:
            return True
        if self.in_dir[i] != 0 or self.out_dir[i] != 0:
            return False
        p, a, n = self.pts[self.prev[i]], self.pts[i], self.pts[self.next[i]]
        ix, iy, ox, oy = a[0] - p[0], a[1] - p[1], n[0] - a[0], n[1] - a[1]
        li, lo, lt = math.hypot(ix, iy), math.hypot(ox, oy), math.hypot(ix + ox, iy + oy)
        return li + lo - lt < lt / 16.0

    def y(self, i):
        return self.pts[i][1]

    def x(self, i):
        return self.pts[i][0]

    def on(self, i):
        return self.pts[i][2] == 1


class Segment:
    def __init__(self, direction_, first):
        self.dir = direction_
        self.points = [first]
        self.link = None
        self.serif = None
        self.edge = None


def segments_of(g):
    """Horizontal segments: maximal runs of consecutive points whose outgoing vector has one horizontal direction."""
    segs = []
    for c, end in enumerate(g.ends):
        start = 0 if c == 0 else g.ends[c - 1] + 1
        idx = list(range(start, end + 1))
        n = len(idx)
        # begin at a point whose predecessor does not continue the same horizontal run
        begin = 0
        for k in range(n):
            i = idx[k]
            if g.out_dir[i] in (LEFT, RIGHT) and g.out_dir[g.prev[i]] != g.out_dir[i]:
                begin = k
                break
        else:
            continue
        k = begin
        count = 0
        while count < n:
            i = idx[(k + count) % n]
            if g.out_dir[i] in (LEFT, RIGHT):
                d = g.out_dir[i]
                run = [i]
                m = count + 1
                while m < n + 0 and g.out_dir[idx[(k + m) % n]] == d:
                    run.append(idx[(k + m) % n])
                    m += 1
                run.append(idx[(k + m) % n])           # the point the last horizontal vector arrives at
                s = Segment(d, run[0])
                s.points = run
                ys = [g.y(p) for p in run]
                xs = [g.x(p) for p in run]
                s.min_y, s.max_y = min(ys), max(ys)
                s.pos = (s.min_y + s.max_y) // 2
                s.min_x, s.max_x = min(xs), max(xs)
                s.round = (not g.on(run[0])) or (not g.on(run[-1]))
                s.is_top = (d == RIGHT) == g.clockwise
                s.length = abs(g.x(run[-1]) - g.x(run[0]))
                tail = [q for q in run if g.on(q)]
                s.length = abs(g.x(tail[-1] if tail else run[-1]) - g.x(run[0]))
                segs.append(s)
                count = m
            else:
                count += 1
    return segs


def link_segments(segs, up):
    min_len = 8.0 * up / 2048.0
    for s in segs:
        s.link = None
        s.serif = None
    for a in segs:
        if a.is_top:
            continue                                   # a stem is found from its bottom edge upward
        best = None
        for b in segs:
            if not b.is_top or b.pos <= a.pos:
                continue
            lo, hi = max(a.min_x, b.min_x), min(a.max_x, b.max_x)
            overlap = hi - lo
            if overlap < min_len:
                continue
            dist = b.pos - a.pos
            score = dist * 3000.0 / up + 6000.0 / overlap * up / 2048.0
            if best is None or score < best[0]:
                best = (score, b)
        if best:
            a.link = best[1]
    for b in segs:
        if not b.is_top:
            continue
        best = None
        for a in segs:
            if a.is_top or a.pos >= b.pos:
                continue
            lo, hi = max(a.min_x, b.min_x), min(a.max_x, b.max_x)
            overlap = hi - lo
            if overlap < min_len:
                continue
            dist = b.pos - a.pos
            score = dist * 3000.0 / up + 6000.0 / overlap * up / 2048.0
            if best is None or score < best[0]:
                best = (score, a)
        if best:
            b.link = best[1]
    for s in segs:
        if s.link is not None and s.link.link is not s:
            s.serif = s.link
            s.link = None


class Edge:
    def __init__(self, seg):
        self.segs = [seg]
        self.fpos = seg.pos
        self.is_top = seg.is_top
        self.round = seg.round
        self.link = None
        self.serif = None
        self.blue = None
        self.pos = None
        self.opos = None
        self.done = False
        self.blue_ref = None
        self.blue_shoot = None


def edges_of(segs, scale, stdw):
    threshold = min(mulfix(int(stdw / 5), scale), 16)
    edges = []
    for s in segs:
        if False:
            continue                         # shorter than a pixel: no edge of its own
        for e in edges:
            if e.is_top == s.is_top and abs(mulfix(e.fpos - s.pos, scale)) < threshold:
                e.segs.append(s)
                s.edge = e
                break
        else:
            e = Edge(s)
            s.edge = e
            edges.append(e)
            edges.sort(key=lambda t: t.fpos)
    for e in edges:
        rounds = sum(1 for s in e.segs if s.round)
        e.round = rounds > 0 and rounds >= len(e.segs) - rounds
        for s in e.segs:
            serif = s.serif is not None and s.serif.edge is not None and s.serif.edge is not e
            if (s.link is not None and s.link.edge is not None) or serif:
                seg2 = s.serif if serif else s.link
                edge2 = e.serif if serif else e.link
                if edge2 is not None:
                    if abs(s.pos - seg2.pos) < abs(e.fpos - edge2.fpos):
                        edge2 = seg2.edge
                else:
                    edge2 = seg2.edge
                if serif:
                    e.serif = edge2
                else:
                    e.link = edge2
        if e.serif is not None and e.link is not None:
            e.serif = None
    return edges


def stem_center(org_center, cur_len):
    """The centre of a stem under 1.5 pixels wide: a pixel's middle, or a little off it."""
    u, d = (32, 32) if cur_len <= 64 else (38, 26)
    p1 = pixround(org_center)
    return p1 - u if abs(org_center - (p1 - u)) < abs(org_center - (p1 + d)) else p1 + d


def standard_width(glyphs, up):
    """The face's standard horizontal stem width: the thinnest stem of 'o' in font units (the quantised smallest)."""
    if "o" not in glyphs:
        return 50.0 * up / 2048.0
    g = Glyph(glyphs["o"])
    segs = segments_of(g)
    link_segments(segs, up)
    widths = sorted(abs(s.link.pos - s.pos) for s in segs if s.link is not None)
    return float(widths[0]) if widths else 50.0 * up / 2048.0


def hint(glyph, z, ppem, up, stdw=None):
    """(the hinted y of each point in 26.6, the corrected scale)."""
    g = Glyph(glyph)
    scale, new, _ = model.scales(z, ppem, up)
    stdw = stdw or 160.0 * up / 2048.0
    segs = segments_of(g)
    link_segments(segs, up)
    edges = edges_of(segs, new, stdw)
    for e in edges:
        e.opos = mulfix(e.fpos, new)
        e.pos = e.opos
    zones = []
    for name, (ref, sh, nf, nr) in z.items():
        if name == SKIP_ZONE:
            continue
        top = model.BLUES[name][1]
        reffit, shootfit, active = model.blue_fit(ref, sh, new)
        if not active:
            continue
        zones.append((top, ref, sh, reffit, shootfit))
    best_limit = min(mulfix(up // 40, new), 32)
    for e in edges:
        bd = best_limit
        for (top, ref, sh, reffit, shootfit) in zones:
            if top != e.is_top:
                continue
            d = abs(mulfix(e.fpos - ref, new))
            if d < bd:
                bd, e.blue, e.blue_ref = d, reffit, ref
                e.blue_shoot = sh
            if e.round and d != 0:
                under = e.fpos < ref
                if top != under:
                    d = abs(mulfix(e.fpos - sh, new))
                    if d < bd:
                        bd, e.blue, e.blue_ref = d, shootfit, ref
                        e.blue_shoot = sh
    anchor = None
    nedges = len(edges)

    def clamp(k):
        e = edges[k]
        if k > 0 and e.pos < edges[k - 1].pos:
            e.pos = edges[k - 1].pos
        if k + 1 < nedges and edges[k + 1].done and e.pos > edges[k + 1].pos:
            e.pos = edges[k + 1].pos

    def align_linked(base, stem):
        stem.pos = base.pos + (stem.opos - base.opos)
        stem.done = True

    for k, e in enumerate(edges):
        blue, edge1, edge2 = e.blue, None, e.link
        if blue is not None:
            edge1 = e
        elif edge2 is not None and edge2.blue is not None:
            blue, edge1, edge2 = edge2.blue, edge2, e
        if edge1 is None:
            continue
        edge1.pos = blue
        edge1.done = True
        if edge2 is not None and edge2.blue is None:
            align_linked(edge1, edge2)
        if anchor is None:
            anchor = e
    has_serifs = False
    for k, e in enumerate(edges):
        if e.done:
            continue
        l = e.link
        if l is None:
            has_serifs = True
            continue
        if l.blue is not None:
            align_linked(l, e)
            continue
        org_len = l.opos - e.opos
        cur_len = org_len
        if anchor is None:
            org_center = e.opos + (org_len >> 1)
            if cur_len < 96:
                p1 = stem_center(org_center, cur_len)
                e.pos = p1 - (cur_len >> 1)
            else:
                e.pos = pixround(e.opos)
            l.pos = e.pos + cur_len
            anchor = e
        else:
            org_pos = anchor.pos + (e.opos - anchor.opos)
            org_center = org_pos + (org_len >> 1)
            if l.done:
                e.pos = l.pos - cur_len
            elif cur_len < 96:
                p1 = stem_center(org_center, cur_len)
                e.pos = p1 - (cur_len >> 1)
                l.pos = e.pos + cur_len
            else:
                p1 = pixround(org_pos)
                d1 = abs(p1 + (cur_len >> 1) - org_center)
                p2 = pixround(org_pos + org_len) - cur_len
                d2 = abs(p2 + (cur_len >> 1) - org_center)
                e.pos = p1 if d1 < d2 else p2
                l.pos = e.pos + cur_len
        e.done = True
        l.done = True
        clamp(k)
        kl = edges.index(l)
        clamp(kl)
    for k, e in enumerate(edges):
        if e.done:
            continue
        delta = abs(e.serif.opos - e.opos) if e.serif is not None else 1000
        if delta < 64 + 16:
            e.pos = e.serif.pos + (e.opos - e.serif.opos)
        elif anchor is None:
            e.pos = pixround(e.opos)
            anchor = e
        else:
            before = next((edges[m] for m in range(k - 1, -1, -1) if edges[m].done), None)
            after = next((edges[m] for m in range(k + 1, nedges) if edges[m].done), None)
            if before is not None and after is not None:
                if after.opos == before.opos:
                    e.pos = before.pos
                else:
                    e.pos = before.pos + muldiv(e.opos - before.opos, after.pos - before.pos, after.opos - before.opos)
            else:
                e.pos = anchor.pos + ((e.opos - anchor.opos + 16) & ~31)
        e.done = True
        clamp(k)
    y = [None] * g.n
    touched = [False] * g.n
    for e in edges:
        for s in e.segs:
            for p in s.points:
                y[p] = e.pos
                touched[p] = True
    edge_list = sorted(edges, key=lambda e: e.fpos)
    weak = [g.is_weak(i) for i in range(g.n)]
    for i in range(g.n):
        if y[i] is not None or weak[i]:
            continue
        org = g.y(i)
        oy = mulfix(org, new)
        touched[i] = True
        if not edge_list:
            y[i] = oy
        elif org <= edge_list[0].fpos:
            y[i] = oy + (edge_list[0].pos - edge_list[0].opos)
        elif org >= edge_list[-1].fpos:
            y[i] = oy + (edge_list[-1].pos - edge_list[-1].opos)
        else:
            for a, b in zip(edge_list, edge_list[1:]):
                if a.fpos <= org <= b.fpos:
                    y[i] = a.pos + muldiv(org - a.fpos, b.pos - a.pos, b.fpos - a.fpos) if b.fpos != a.fpos else a.pos
                    break
    # weak points: interpolated between the touched points of their contour
    start = 0
    for end in g.ends:
        idx = list(range(start, end + 1))
        start = end + 1
        tp = [i for i in idx if touched[i]]
        if not tp:
            for i in idx:
                y[i] = mulfix(g.y(i), new)
            continue
        if len(tp) == 1:
            d = y[tp[0]] - mulfix(g.y(tp[0]), new)
            for i in idx:
                if not touched[i]:
                    y[i] = mulfix(g.y(i), new) + d
            continue
        for a, b in zip(tp, tp[1:] + tp[:1]):
            ia, ib = idx.index(a), idx.index(b)
            between = []
            k = (ia + 1) % len(idx)
            while k != ib:
                between.append(idx[k])
                k = (k + 1) % len(idx)
            u1, u2 = g.y(a), g.y(b)
            c1, c2 = y[a], y[b]
            if u1 > u2:
                u1, u2, c1, c2 = u2, u1, c2, c1
            d1 = c1 - mulfix(u1, new)
            d2 = c2 - mulfix(u2, new)
            for i in between:
                u = g.y(i)
                oy = mulfix(u, new)
                if u <= u1:
                    y[i] = oy + d1
                elif u >= u2:
                    y[i] = oy + d2
                else:
                    y[i] = c1 + muldiv(u - u1, c2 - c1, u2 - u1) if u2 != u1 else c1
    return y, new
