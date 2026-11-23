#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Font.
"""The automatic hinter's global model, fitted to FreeType's (autohinter plan, stage A1).

For one Latin face: the six blue zones (capital top and bottom, small f top, small top, small bottom,
small descender), each a reference and an overshoot taken from the extremes of the letters the zone is
named for, and the scale correction that puts the x-height (the overshoot of the small-top zone) on a
whole pixel, rounding up from 0.375. Predicted whole-pixel heights of the flat and round tops and of the
descender agree with FreeType's `autolight` in 120 of 120 cases (ten faces, twelve sizes from 8 to 24 ppem).

A face is `{"upem": N, "glyphs": {char: {"pts": [[x, y, on_curve], ...], "ends": [last point of each contour]}}}`,
as `autohint_dump.py` writes it. Analysis arithmetic is in Python floats; nothing here is the library's code.
"""
import math
import statistics

BLUES = {
    "CAP_TOP": ("THEZOCQS", True),
    "CAP_BOT": ("HEZLOCUS", False),
    "F_TOP": ("fijkdbh", True),
    "SMALL_TOP": ("xzroesc", True),
    "SMALL_BOT": ("xzroesc", False),
    "DESC": ("pqgjy", False),
}


def rnd(x):
    return math.floor(x + 0.5)


def classify(glyphs, ch, top):
    """(extreme y of the letter, whether the extreme is round): round when a neighbour of the point is off the curve."""
    pts = glyphs[ch]["pts"]
    ys = [p[1] for p in pts]
    ext = max(ys) if top else min(ys)
    i = next(k for k, p in enumerate(pts) if p[1] == ext)
    start = 0
    for end in glyphs[ch]["ends"]:
        if i <= end:
            break
        start = end + 1
    n = end - start + 1
    prev = pts[start + ((i - start - 1) % n)]
    nxt = pts[start + ((i - start + 1) % n)]
    return ext, prev[2] == 0 or nxt[2] == 0


def zones(glyphs):
    """{zone: (reference, overshoot, flat letters, round letters)} in font units."""
    out = {}
    for name, (chars, top) in BLUES.items():
        flats, rounds = [], []
        for ch in chars:
            if ch in glyphs:
                y, is_round = classify(glyphs, ch, top)
                (rounds if is_round else flats).append(y)
        if not flats and not rounds:
            continue
        ref = statistics.median(flats) if flats else statistics.median(rounds)
        shoot = statistics.median(rounds) if rounds else statistics.median(flats)
        out[name] = (ref, shoot, len(flats), len(rounds))
    return out


def scale_fit(z, ppem, upem):
    """(fitted x-height in pixels, the scale correction) for a size."""
    shoot = z["SMALL_TOP"][1] * ppem / upem
    fit = math.floor(shoot + 0.625)
    return fit, (fit / shoot if shoot > 0 else 1.0)


def predict(z, ppem, upem):
    """Whole-pixel heights FreeType gives the flat and round letters, by the model."""
    fit, k = scale_fit(z, ppem, upem)
    s = ppem / upem
    return {"x": rnd(z["SMALL_TOP"][0] * s * k), "o": fit, "H": rnd(z["CAP_TOP"][0] * s * k),
            "p": rnd(z["DESC"][0] * s * k)}
