# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Font.
"""Experiments for autohint_probe.py: each takes run(labels, glyphs, report, ppems) and the sizes."""


def plain_bar(run, ppems):
    """The control: a rectangle. Every point sits on a flat edge; both hinters should agree to the 64th."""
    labels, glyphs, report = [], [], []
    for height in (500, 640, 700):
        labels.append("rect h=%d" % height)
        glyphs.append([[[100, 0, 1], [100, height, 1], [400, height, 1], [400, 0, 1]]])
        report.append([0, 1, 2, 3])
    run(labels, glyphs, report, ppems)


def handle_start(run, ppems):
    """A top edge whose flat run begins at a curve's control point, the control point `dx` before the on-curve
    point it runs to: is the control point moved with the edge? Reports the y of points 1 (the curve's start),
    2 (the control point), 3 and 4 (on the edge)."""
    labels, glyphs, report = [], [], []
    for dx in (4, 10, 20, 30, 45, 60, 100, 150):
        labels.append("handle dx=%d" % dx)
        glyphs.append([[[100, 0, 1], [100, 600, 1], [200 - dx, 700, 0], [200, 700, 1], [400, 700, 1], [400, 0, 1]]])
        report.append([1, 2, 3, 4])
    run(labels, glyphs, report, ppems)


def handle_slope(run, ppems):
    """As handle_start with dx fixed at 40 and the control point `dy` below the edge: how steep a first vector
    still counts as part of the flat run."""
    labels, glyphs, report = [], [], []
    for dy in (0, 2, 4, 8, 16, 32, 64):
        labels.append("handle dy=%d" % dy)
        glyphs.append([[[100, 0, 1], [100, 600, 1], [160, 700 - dy, 0], [200, 700, 1], [400, 700, 1], [400, 0, 1]]])
        report.append([1, 2, 3, 4])
    run(labels, glyphs, report, ppems)


def strong_sweep(run, ppems):
    """A corner point between two edges at every height: where does each hinter put it? Edges at 0 and 700; the
    point is the vertex (500, y) of a hexagon, a strong point (diagonal in and out)."""
    labels, glyphs, report = [], [], []
    for y in range(40, 661, 5):
        labels.append("vertex y=%d" % y)
        glyphs.append([[[100, 0, 1], [100, 700, 1], [400, 700, 1], [500, y, 1], [400, 0, 1]]])
        report.append([3])
    rows = run(labels, glyphs, report, ppems, show=False)
    bad = [r for r in rows if r[2] != r[3]]
    print("%d of %d differ" % (len(bad), len(rows)))
    for label, ppem, m, t in bad[:40]:
        print("%-18s ppem %2d ours %s theirs %s  (%+d)" % (label, ppem, m, t, t[0] - m[0]))


def slope_grid(run, ppems):
    """The first vector of a top edge's run, (dx, dy) from a control point to the edge: is the control point
    pinned to the edge (y equal to the edge's) or interpolated?"""
    labels, glyphs, report = [], [], []
    cases = [(dx, dy) for dx in (10, 20, 40, 80, 160) for dy in (1, 2, 3, 4, 6, 8, 12)]
    for dx, dy in cases:
        labels.append("dx=%d dy=%d" % (dx, dy))
        glyphs.append([[[100, 0, 1], [100, 600, 1], [200 - dx, 700 - dy, 0], [200, 700, 1], [400, 700, 1],
                        [400, 0, 1]]])
        report.append([2, 3])
    rows = run(labels, glyphs, report, ppems, show=False)
    for label, ppem, m, t in rows:
        pinned_m = m[0] == m[1]
        pinned_t = t[0] == t[1]
        if ppem == ppems[-1]:
            print("%-14s ours %-6s theirs %-6s %s" % (label, "pinned" if pinned_m else "free",
                                                       "pinned" if pinned_t else "free",
                                                       "" if pinned_m == pinned_t else "<-- differs"))


def slope_on_curve(run, ppems):
    """As slope_grid but the first point of the run is an on-curve corner, not a control point: a polygon
    (100,0) (100,600) (200-dx, 700-dy) (200,700) (400,700) (400,0). Pinned means the first point has the same y as
    the edge's other points."""
    labels, glyphs, report = [], [], []
    cases = [(dx, dy) for dx in (10, 20, 40, 80, 160) for dy in (0, 1, 2, 3, 4, 6, 8, 12)]
    for dx, dy in cases:
        labels.append("dx=%d dy=%d" % (dx, dy))
        glyphs.append([[[100, 0, 1], [100, 600, 1], [200 - dx, 700 - dy, 1], [200, 700, 1], [400, 700, 1],
                        [400, 0, 1]]])
        report.append([2, 3])
    rows = run(labels, glyphs, report, ppems, show=False)
    for label, ppem, m, t in rows:
        if ppem == ppems[-1]:
            print("%-14s ours %-6s theirs %-6s %s" % (label, "pinned" if m[0] == m[1] else "free",
                                                       "pinned" if t[0] == t[1] else "free",
                                                       "" if (m[0] == m[1]) == (t[0] == t[1]) else "<-- differs"))


def flat_by_size(run, ppems):
    """For each size, the largest dy (with dx=160) at which FreeType still pins the run's first point, and ours."""
    labels, glyphs, report = [], [], []
    dys = list(range(0, 13))
    for dy in dys:
        labels.append("dy=%d" % dy)
        glyphs.append([[[100, 0, 1], [100, 600, 1], [40, 700 - dy, 1], [200, 700, 1], [400, 700, 1], [400, 0, 1]]])
        report.append([2, 3])
    rows = run(labels, glyphs, report, ppems, show=False)
    for ppem in ppems:
        ours = "".join("P" if r[2][0] == r[2][1] else "." for r in rows if r[1] == ppem)
        theirs = "".join("P" if r[3][0] == r[3][1] else "." for r in rows if r[1] == ppem)
        print("ppem %2d dy=0..12  ours %s  theirs %s" % (ppem, ours, theirs))


def flat_by_length(run, ppems):
    """The ratio test: at a size where dy=1 is one 64th of a pixel or more, how long must the run be before
    FreeType calls it flat? Runs of dx = 100 .. 1800 with dy=1 and dy=2."""
    labels, glyphs, report = [], [], []
    cases = []
    for dy in (1, 2):
        for dx in (100, 200, 400, 800, 1200, 1800):
            cases.append((dx, dy))
    for dx, dy in cases:
        labels.append("dx=%d dy=%d" % (dx, dy))
        glyphs.append([[[0, 0, 1], [0, 600, 1], [2000 - dx, 700 - dy, 1], [2000, 700, 1], [2400, 700, 1],
                        [2400, 0, 1]]])
        report.append([2, 3])
    rows = run(labels, glyphs, report, ppems, upem=1000, show=False)
    for ppem in ppems:
        print("ppem %d: " % ppem + "  ".join("%s=%s/%s" % (r[0].replace("dx=", "").replace(" dy=", "/"),
              "P" if r[2][0] == r[2][1] else ".", "P" if r[3][0] == r[3][1] else ".") for r in rows if r[1] == ppem))


def sloped_run(run, ppems):
    """A run whose first vector is (800, dy): the y of its first two points and its far end, ours and FreeType's,
    against the y the unhinted outline would give. Shows whether the points of a nearly flat run are made equal or
    moved together."""
    labels, glyphs, report = [], [], []
    for dy in (0, 1, 2, 3, 5, 8):
        labels.append("dy=%d" % dy)
        glyphs.append([[[0, 0, 1], [0, 600, 1], [1200, 700 - dy, 1], [2000, 700, 1], [2400, 700, 1], [2400, 0, 1]]])
        report.append([2, 3, 4])
    run(labels, glyphs, report, ppems)


L_POINTS = [(21, 0, 1), (257, 0, 1), (257, 15, 1), (194, 18, 0), (183, 28, 0), (182, 84, 1), (182, 681, 1),
            (178, 683, 1), (127, 667, 0), (90, 656, 0), (19, 639, 1), (19, 623, 1), (25, 623, 1), (36, 624, 0),
            (48, 625, 0), (56, 625, 1), (88, 625, 0), (98, 611, 0), (98, 564, 1), (98, 87, 1), (97, 33, 0),
            (83, 19, 0), (21, 15, 1)]


def serif_l(run, ppems):
    """The shape of a real serif's top-left (Nimbus Roman l, as quadratic points), with the y of point 13 -
    the control point after the first flat vector - moved: where does each hinter move points 11-15?"""
    labels, glyphs, report = [], [], []
    for y13 in (622, 623, 624, 625, 626, 627):
        pts = [list(p) for p in L_POINTS]
        pts[13][1] = y13
        labels.append("p13 y=%d" % y13)
        glyphs.append([pts])
        report.append([11, 12, 13, 14, 15])
    run(labels, glyphs, report, ppems)


def bar_and_top(run, ppems):
    """A horizontal bar (a stem of thickness t at y 300) beside a block whose top is at 700 and bottom at 0: the y of
    the bar's two edges and the block's top, ours and FreeType's."""
    labels, glyphs, report = [], [], []
    for t in (20, 30, 40, 50, 60, 80, 120):
        labels.append("bar t=%d" % t)
        glyphs.append([[[0, 300, 1], [0, 300 + t, 1], [300, 300 + t, 1], [300, 300, 1]],
                       [[700, 0, 1], [700, 700, 1], [1000, 700, 1], [1000, 0, 1]]])
        report.append([0, 1, 5, 6])
    run(labels, glyphs, report, ppems)


def fuzz_boxes(run, ppems):
    """Random rectilinear glyphs: one to three boxes at random heights and thicknesses. Every difference is a
    reproducer already small enough to read; they are listed fewest points first."""
    import os
    import random
    rng = random.Random(int(os.environ.get("PROBE_SEED", "7")))
    labels, glyphs, report = [], [], []
    for n in range(int(os.environ.get("PROBE_COUNT", "96"))):
        boxes = []
        x = 0
        for _ in range(rng.choice((1, 1, 2, 2, 3))):
            y0 = rng.randrange(-30, 650, 7)
            h = rng.randrange(14, 400, 3)
            w = rng.randrange(80, 300, 10)
            boxes.append([[x, y0, 1], [x, y0 + h, 1], [x + w, y0 + h, 1], [x + w, y0, 1]])
            x += w + rng.randrange(40, 120, 10)
        labels.append("n%d:" % n + ",".join("%d+%d" % (b[0][1], b[1][1] - b[0][1]) for b in boxes))
        glyphs.append(boxes)
        report.append(list(range(4 * len(boxes))))
    rows = run(labels, glyphs, report, ppems, show=False)
    bad = [r for r in rows if r[2] != r[3]]
    print("%d of %d (glyph, size) differ" % (len(bad), len(rows)))
    bad.sort(key=lambda r: (len(r[2]), r[0]))
    for label, ppem, m, t in bad[:25]:
        print("%-34s ppem %2d ours %s theirs %s" % (label, ppem, m, t))


def blue_reach(run, ppems):
    """A box whose bottom edge is `y0` units from the baseline (and, in a second family, whose top is `d` from the
    cap height): how far from a blue zone does an edge still snap to it? Prints the box's bottom y at each size,
    ours and FreeType's, as the pixel-grid position that tells a snap from none."""
    labels, glyphs, report = [], [], []
    for y0 in range(-48, 49, 4):
        labels.append("bottom y0=%d" % y0)
        glyphs.append([[[0, y0, 1], [0, y0 + 300, 1], [300, y0 + 300, 1], [300, y0, 1]]])
        report.append([0, 3])
    run(labels, glyphs, report, ppems)


def blue_reach_scan(run, ppems):
    """For each size, the farthest below and above the baseline a box's bottom edge still snaps to the blue zone,
    in font units, ours and FreeType's (the box is 300 tall; a snap is a bottom y on the zone's grid fit)."""
    labels, glyphs, report = [], [], []
    ys = list(range(-60, 61, 2))
    for y0 in ys:
        labels.append("y0=%d" % y0)
        glyphs.append([[[0, y0, 1], [0, y0 + 300, 1], [300, y0 + 300, 1], [300, y0, 1]]])
        report.append([0])
    import os
    rows = run(labels, glyphs, report, ppems, show=False, overshoot=int(os.environ.get("PROBE_OVERSHOOT", "12")))
    for ppem in ppems:
        res = {}
        for who, k in (("ours", 2), ("theirs", 3)):
            snapped = [int(r[0].split("=")[1]) for r in rows if r[1] == ppem and r[k][0] == 0]
            res[who] = (min(snapped) if snapped else None, max(snapped) if snapped else None)
        print("ppem %2d snap range of the bottom edge (font units): ours %s theirs %s" % (ppem, res["ours"], res["theirs"]))


def merged_tops(run, ppems):
    """Two boxes whose tops are nearly level: B = 103..294 and A = 173..(294+delta). When the tops merge into one
    edge, which of the two bottoms does the edge pair with, and where does the merged top go?"""
    labels, glyphs, report = [], [], []
    for delta in range(-30, 41, 3):
        labels.append("delta=%+d" % delta)
        glyphs.append([[[0, 103, 1], [0, 294, 1], [200, 294, 1], [200, 103, 1]],
                       [[400, 173, 1], [400, 294 + delta, 1], [600, 294 + delta, 1], [600, 173, 1]]])
        report.append([0, 1, 4, 5])
    run(labels, glyphs, report, ppems)


def merged_tops_sweep(run, ppems):
    """merged_tops with the second box's bottom moved: B = 103..294 fixed, A = a..294. Prints B's bottom, A's
    bottom, the shared top (ours then FreeType's) and the unhinted y of each for comparison."""
    labels, glyphs, report = [], [], []
    for a in range(110, 290, 15):
        labels.append("A bottom=%d" % a)
        glyphs.append([[[0, 103, 1], [0, 294, 1], [200, 294, 1], [200, 103, 1]],
                       [[400, a, 1], [400, 294, 1], [600, 294, 1], [600, a, 1]]])
        report.append([0, 4, 1])
    run(labels, glyphs, report, ppems)


def blue_stem_width(run, ppems):
    """A box sitting on the baseline (bottom 0) with a growing height: where does FreeType put its top, and does it
    keep the scaled height or fit it to the pixel grid?"""
    labels, glyphs, report = [], [], []
    for h in range(40, 200, 10):
        labels.append("baseline h=%d" % h)
        glyphs.append([[[0, 0, 1], [0, h, 1], [200, h, 1], [200, 0, 1]]])
        report.append([0, 1])
    for h in range(40, 200, 10):
        labels.append("floating h=%d" % h)
        glyphs.append([[[0, 233, 1], [0, 233 + h, 1], [200, 233 + h, 1], [200, 233, 1]]])
        report.append([0, 1])
    run(labels, glyphs, report, ppems)


def near_edges(run, ppems):
    """Box S = 33..119 (on the baseline blue) beside box L = b..420, with b swept across S's top: the edges of
    S's top and L's bottom come within a few font units of each other, on opposite sides."""
    labels, glyphs, report = [], [], []
    for b in range(60, 200, 8):
        labels.append("L bottom=%d" % b)
        glyphs.append([[[0, b, 1], [0, 420, 1], [200, 420, 1], [200, b, 1]],
                       [[400, 33, 1], [400, 119, 1], [600, 119, 1], [600, 33, 1]]])
        report.append([0, 1, 4, 5])
    run(labels, glyphs, report, ppems)


def merged_bottoms(run, ppems):
    """A short box 40..72 beside a tall box d..392 with d swept near 40: when the two bottoms merge into one edge,
    where does the merged bottom go, and what happens to the short box's top?"""
    labels, glyphs, report = [], [], []
    for d in range(26, 70, 4):
        labels.append("tall bottom=%d" % d)
        glyphs.append([[[0, 40, 1], [0, 72, 1], [200, 72, 1], [200, 40, 1]],
                       [[400, d, 1], [400, 392, 1], [600, 392, 1], [600, d, 1]]])
        report.append([0, 1, 4, 5])
    run(labels, glyphs, report, ppems)


def curve_entry(run, ppems):
    """A flat top entered through two off-curve points, as the serif of a roman F: P3 is the last off-curve point,
    dy below the flat top and dx before its first on-curve point. Does FreeType count P3 as part of the flat
    segment (and so put it on the edge), or treat it as a weak point between the stem and the flat?"""
    labels, glyphs, report = [], [], []
    for dy in (0, 1, 2, 3, 4, 6, 9):
        for dx in (2, 6, 14, 28, 60):
            labels.append("dy=%d dx=%d" % (dy, dx))
            top = 634
            glyphs.append([[[0, 0, 1], [0, 590, 1], [2, 620, 0], [100 - dx - 28, top - dy, 0], [100, top, 1], [260, top, 1],
                            [260, 0, 1]]])
            report.append([1, 2, 3, 4])
    run(labels, glyphs, report, ppems)


def curve_exit(run, ppems):
    """curve_entry mirrored: a flat top left through off-curve points. P3 is the first off-curve point after the
    flat's last on-curve point, dy below the top and dx + 28 after it."""
    labels, glyphs, report = [], [], []
    for dy in (0, 1, 2, 3, 4, 6, 9):
        for dx in (2, 6, 14, 28, 60):
            labels.append("dy=%d dx=%d" % (dy, dx))
            top = 634
            glyphs.append([[[0, 0, 1], [0, 590, 1], [20, top, 1], [160, top, 1], [160 + dx + 28, top - dy, 0],
                            [300, 620, 0], [300, 0, 1]]])
            report.append([2, 3, 4, 5])
    run(labels, glyphs, report, ppems)


def curve_exit_two(run, ppems):
    """The flat top ends in an off-curve point at the same height, then a second off-curve point dy lower and dx + 28
    on, then an on-curve point: the shape of the top right of a bold roman r."""
    labels, glyphs, report = [], [], []
    for dy in (0, 1, 2, 3, 4, 6, 9):
        for dx in (2, 6, 14, 28, 60):
            labels.append("dy=%d dx=%d" % (dy, dx))
            top = 634
            glyphs.append([[[0, 0, 1], [0, 590, 1], [20, top, 1], [160, top, 1], [178, top, 0],
                            [178 + dx + 28, top - dy, 0], [330, top - 8, 1], [330, 0, 1]]])
            report.append([3, 4, 5, 6])
    run(labels, glyphs, report, ppems)


L_OUTLINE = [[21, 0, 1], [257, 0, 1], [257, 15, 1], [194, 18, 0], [183, 28, 0], [182, 84, 1], [182, 681, 1], [178, 683, 1],
             [127, 667, 0], [90, 656, 0], [19, 639, 1], [19, 623, 1], [25, 623, 1], [36, 624, 0], [48, 625, 0], [56, 625, 1],
             [88, 625, 0], [98, 611, 0], [98, 564, 1], [98, 87, 1], [97, 33, 0], [83, 19, 0], [21, 15, 1]]


def roman_l(run, ppems):
    """The l of a roman text face (23 points) with the short flat run at the foot of its flag changed: the run's
    two points (11 and 12) move vertically, which decides whether they join the longer flat run that follows."""
    labels, glyphs, report = [], [], []
    for y in (620, 621, 622, 623, 624, 625, 626):
        for x2 in (22, 25, 30, 36):
            pts = [list(p) for p in L_OUTLINE]
            pts[11][1] = y
            pts[12] = [x2, y, 1]
            labels.append("y=%d x2=%d" % (y, x2))
            glyphs.append([pts])
            report.append([10, 11, 12, 13, 14, 15])
    run(labels, glyphs, report, ppems)


def roman_l_join(run, ppems):
    """roman_l with the short run fixed at y=623 and the off-curve point after it (point 13) moved: dx along and dy
    above the run. Points 11 and 12 reaching the edge value (480) means FreeType counted them in the flat segment."""
    labels, glyphs, report = [], [], []
    for dy in (-2, -1, 0, 1, 2, 3):
        for dx in (4, 6, 8, 11, 14, 20, 30):
            pts = [list(p) for p in L_OUTLINE]
            pts[13] = [25 + dx, 623 + dy, 0]
            pts[14] = [25 + dx + 12, 625, 0]
            pts[15] = [25 + dx + 20, 625, 1]
            labels.append("dy=%d dx=%d" % (dy, dx))
            glyphs.append([pts])
            report.append([11, 12, 13])
    run(labels, glyphs, report, ppems)


def direction_grid(run, ppems):
    import os
    off = os.environ.get("PROBE_OFF") == "1"
    """Is the vector from the end of a flat top to the next on-curve point itself flat? A top (0,T)-(160,T) and a
    point B at (160 + dx, T - dy): B at the edge's value means FreeType put the vector in the segment, below it means
    it did not. Both ours and FreeType's y of B are printed for a grid of dy and dx."""
    labels, glyphs, report = [], [], []
    for dy in (1, 2, 3, 4, 6, 8):
        for ratio in (8, 9, 10, 10.5, 11, 11.5, 12, 13, 14, 14.5, 15, 16, 20):
            dx = int(round(ratio * dy))
            if dx != ratio * dy and abs(dx - ratio * dy) > 0.5:
                continue
            labels.append("dy=%d dx=%d (%.1f)" % (dy, dx, dx / dy))
            glyphs.append([[[0, 0, 1], [0, 634, 1], [160, 634, 1], [160 + dx, 634 - dy, 0 if off else 1], [160 + dx + 100, 400, 1],
                            [160 + dx + 100, 0, 1]]])
            report.append([2, 3])
    run(labels, glyphs, report, ppems)


def near_limit(run, ppems):
    """roman_l with the short run of length L and the off-curve point after it 11 along and 1 above: the run joins the
    edge on the far side only when its last point counts as near its first, so the L at which the join stops is the
    limit. The upem scales it (a font of 2048 units has L doubled)."""
    import os
    upem = int(os.environ.get("PROBE_UPEM", "1000"))
    k = upem / 1000.0
    labels, glyphs, report = [], [], []
    for L in range(3, 31):
        pts = [[int(round(v[0] * k)), int(round(v[1] * k)), v[2]] for v in L_OUTLINE]
        pts[12] = [pts[11][0] + int(round(L * k)), pts[11][1], 1]
        pts[13] = [pts[12][0] + int(round(11 * k)), pts[11][1] + int(round(1 * k)), 0]
        pts[14] = [pts[13][0] + int(round(12 * k)), pts[15][1] - int(round(30 * k)), 0]
        pts[15] = [pts[14][0] + int(round(8 * k)), pts[15][1] - int(round(60 * k)), 1]
        labels.append("L=%d" % L)
        glyphs.append([pts])
        report.append([11, 12, 13])
    run(labels, glyphs, report, ppems, upem=upem)


T_OUTLINE = [[315, 0, 1], [315, 53, 1], [528, 80, 1], [528, 1255, 1], [477, 1255, 1], [224, 1255, 0], [131, 1235, 1],
             [104, 1026, 1], [37, 1026, 1], [37, 1341, 1], [1217, 1341, 1], [1217, 1026, 1], [1149, 1026, 1],
             [1122, 1235, 1], [1092, 1242, 0], [890, 1253, 0], [770, 1253, 1], [721, 1253, 1], [721, 80, 1],
             [934, 53, 1], [934, 0, 1]]


def serif_corner(run, ppems):
    """The right serif of a roman T (2048 per em): point 13 is the vertex between the near-vertical serif edge and
    the first off-curve point of the top. Its x and y are swept; whether FreeType interpolates it (weak) or places
    it between the edges (strong) shows in its y, and so does what ours does."""
    labels, glyphs, report = [], [], []
    for y in (1235, 1215, 1190):
        for x in range(1096, 1170, 4):
            pts = [list(p) for p in T_OUTLINE]
            pts[13] = [x, y, 1]
            labels.append("x=%d y=%d" % (x, y))
            glyphs.append([pts])
            report.append([12, 13, 14])
    run(labels, glyphs, report, ppems, upem=2048)
