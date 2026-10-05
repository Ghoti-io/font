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
"""Every glyph's outline at a location in the design space, against fontTools.

documentation/design.md sections 7.7 and 14. `glyf_diff.py` compares the default
instance; this compares what `gvar` makes of it, which is a different question
and is the one a variable font exists to ask.

Two things are compared, and they are kept apart on purpose:

  * **The normalisation.** A user's location - `wght=700,opsz=14` - becomes
    normalised 2.14 coordinates through the axis's range and `avar`, and this
    library's answer is compared with fontTools' own (`normalizeValue`,
    `piecewiseLinearMap`). Both are rounded to 2.14 and **one unit is allowed**:
    the two readers do the division in different arithmetic - fixed point against
    float - and a tie lands either side. The largest difference seen is printed.
  * **The outlines at those coordinates.** fontTools is handed **this library's**
    normalised coordinates and not its own, so that what differs is `gvar`'s
    arithmetic and not the first comparison's rounding carried into every point
    of every glyph. A disagreement here is a delta, a scalar, an inferred point or
    a component offset.

**The population is the variable fonts the image carries, and the locations are
chosen to reach every case a tuple can distinguish** rather than to be many: the
default; every axis alone at its minimum, its maximum, and halfway to each from
the default (an intermediate tuple's ramp is only visible *between* its peak and
its edges); every axis at once at each extreme; alternating corners; and a few
seeded pseudo-random interior points. The seed is the font's name, so a run is
repeatable and two runs cover the same places.

**Coordinates are compared to one 64th of a font unit, and the allowance is
measured rather than asserted.** A delta is a whole number of font units and a
tuple's scalar is a 16.16 fraction, so a point's position is exact in this
library to the last bit of that fraction and fontTools computes the same product
in floating point. The two cannot differ by more than the 16.16 rounding of a
scalar times the delta - about 0.015 of a unit for the largest deltas real fonts
carry - which is a 64th. The report prints how many comparisons rested on the
allowance and the largest difference any showed, so a run that leans on it says
so and a difference of two 64ths is a finding.

Usage:
    var_diff.py [--stride N] [--fonts N] [--locations N] [--quiet] [font...]
"""

import os
import random
import subprocess
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import corpus
import glyf_diff
import oracle_env
import unskippable

# glyf_diff's canonical form rotates every contour to its smallest start, which is
# right for two sides that hold equal numbers and wrong for two that differ by a
# 64th: see contour_close(). The rest of its normalisation - closing each contour
# explicitly and dropping a zero-length line - is kept, and the rotation is made
# the identity so that this file's own comparison can try them all.
glyf_diff.rotate = lambda contour: tuple(contour)

ROOT = oracle_env.ROOT
DRIVER = glyf_diff.DRIVER
REFERENCE = os.path.join(ROOT, "tools", "oracle", "fonttools_variation.py")
FIXTURES = glyf_diff.FIXTURES

# Every nth glyph of a real font, by default; a prime, for the reason glyf_diff's
# is. Fixtures are compared at every glyph.
GLYPH_STRIDE = 7

# How many disagreements to print per font and in total.
PER_FONT = 6
TOTAL = 60

# How far two readers' normalised 2.14 coordinates may differ, by reference.
#
# One unit against fontTools, whose float arithmetic can land a tie either side.
# **None against FreeType**: this library's normalisation is FreeType's own
# arithmetic - 16.16 with half-away rounding at each division and (x + 2) >> 2 to
# 2.14 - so the two must agree to the bit, and a unit of slack would hide exactly
# the difference that choice was made to remove.
NORMAL_ALLOWANCE = {"fontTools": 1, "FreeType": 0}
# And how far two readers' 26.6 coordinates may, by what kind of glyph it is.
#
# One 64th for a simple glyph: its points are a stored integer plus a delta that
# this library computes exactly to twenty-four bits and rounds **once**, so it can
# differ from fontTools' float by at most the rounding of that one value.
#
# Two for a composite, and the reason is structural rather than a looseness: a
# component's offset is held in 26.6 - rounded once, when the delta is added - and
# then goes through the component's own transform, which rounds again, where
# fontTools keeps the whole composition in floating point and rounds only at the
# pen. Two roundings of a half 64th each can land a whole 64th apart, and a third
# in the final value can make it two. The histogram below is printed per kind, so a
# composite that reaches two says so and a *simple* glyph that does is a finding.
POINT_ALLOWANCE = 1
COMPOSITE_ALLOWANCE = 2
# FreeType's, and each is a measurement with a reason rather than a margin.
#
# A simple glyph's point: two 64ths. FreeType holds a tuple's scalar to sixteen
# bits where this library holds it to twenty-four, so its own rounding error is
# the delta times half a sixteen-bit step - a quarter of a font unit at the very
# largest deltas, and under a 64th at the ones real fonts carry. Over 8.2 million
# coordinates it differed by one 64th for 12,888 and by two for 4.
#
# A composite's point: **half a font unit per level of nesting**, 32 64ths each,
# and two levels is 64 plus what the glyph's own points can differ by. FreeType
# rounds a component's *offset* to whole font units (it adds `FT_fixedToInt` of
# the delta to the stored integer), where this library and fontTools keep the
# fraction - 26.6 can hold it and the specification does not say to round. The
# measured distribution is what that explanation predicts: 99.99% of composite
# coordinates within 33, spread across 0 to 32 as the delta's fractional part is,
# and the rest at most 64 - which is the composites that nest. An allowance this
# wide does not see a component off by a fraction of a unit, and that is what the
# fontTools comparison is for: it holds this library to two 64ths on the same
# glyphs. What this one adds is a second implementation's agreement that no
# component is *placed* wrongly, which a shared misreading of the format would
# survive in fontTools alone.
FREETYPE_POINT_ALLOWANCE = 2
FREETYPE_COMPOSITE_ALLOWANCE = 66


class Skip(Exception):
    """This font cannot be compared, with a reason worth printing.

    `expected` has the meaning it has in glyf_diff: a font that was never going to
    be compared here, as against one this differential meant to compare and could
    not - which tools/oracle/unskippable.py fails the run for.
    """

    def __init__(self, why, expected=False):
        super().__init__(why)
        self.expected = expected


def fixtures():
    """The synthetic variable fixtures: every `variable-*.ttf` in the manifest."""
    manifest = os.path.join(FIXTURES, "MANIFEST")
    if not os.path.exists(manifest):
        return []
    found = []
    with open(manifest, encoding="utf-8") as handle:
        for line in handle:
            if line.startswith("#") or not line.strip():
                continue
            name = line.split("\t")[0]
            if name.startswith("variable-") and name.endswith(".ttf"):
                found.append(os.path.join(FIXTURES, name))
    return found


def plan(axes, seed, limit):
    """The locations to compare at, as `tag=value,...` strings.

    @p axes is [(tag, min, default, max)] in user coordinates. Deterministic: the
    same font gives the same list in every run, which is what lets two reports be
    diffed.
    """
    def spell(values):
        return ",".join("%s=%s" % (tag, repr(round(value, 4)))
                        for (tag, _, _, _), value in zip(axes, values))

    defaults = [d for _, _, d, _ in axes]
    seen = []

    def add(values):
        text = spell(values)
        if text not in seen:
            seen.append(text)

    add(defaults)
    for index, (tag, low, default, high) in enumerate(axes):
        for value in (low, (low + default) / 2.0, (default + high) / 2.0, high):
            moved = list(defaults)
            moved[index] = value
            add(moved)
    add([low for _, low, _, _ in axes])
    add([high for _, _, _, high in axes])
    add([low if i % 2 == 0 else high for i, (_, low, _, high) in
         enumerate(axes)])
    add([high if i % 2 == 0 else low for i, (_, low, _, high) in
         enumerate(axes)])
    chooser = random.Random(seed)
    for _ in range(3):
        add([chooser.uniform(low, high) for _, low, _, high in axes])
    return seen[:limit] if limit else seen


def ours(path, face, stride, location):
    """This library's output at one location, as the driver printed it."""
    finished = subprocess.run([DRIVER, path, str(face), str(stride), "0",
        location], capture_output=True, text=True)
    if finished.returncode != 0:
        raise Skip(finished.stderr.strip().splitlines()[-1]
                   if finished.stderr.strip() else "the driver refused it")
    return finished.stdout


def reference(path, face, stride, specs):
    """fontTools' output for every location, split into one text per location."""
    argv = oracle_env.command("fonttools", ["python3", REFERENCE, path,
        str(face), str(stride), "0"] + specs)
    finished = subprocess.run(argv, capture_output=True, text=True)
    if finished.returncode != 0:
        raise Skip("fontTools: %s"
                   % (finished.stderr.strip().splitlines()[-1]
                      if finished.stderr.strip() else "refused"))
    axes = []
    blocks = []
    header = []
    for line in finished.stdout.splitlines():
        if line.startswith("axis: "):
            tag, low, default, high = line[6:].split()
            axes.append((tag, float(low), float(default), float(high)))
        elif line.startswith("location "):
            blocks.append([])
        elif line.startswith("outlines: ") and blocks:
            blocks[-1].append(line)
        elif blocks:
            blocks[-1].append(line)
        else:
            header.append(line)
    return axes, header, ["\n".join(block) for block in blocks]


def reference_freetype(path, face, stride, specs):
    """FreeType's output for every location, split into one text per location."""
    argv = oracle_env.command("freetype", ["freetype-variation", path,
        str(face), str(stride), "0"] + specs)
    finished = subprocess.run(argv, capture_output=True, text=True)
    if finished.returncode != 0:
        raise Skip("FreeType: %s"
                   % (finished.stderr.strip().splitlines()[-1]
                      if finished.stderr.strip() else "refused"))
    blocks = []
    for line in finished.stdout.splitlines():
        if line.startswith("location "):
            blocks.append([])
        elif blocks:
            blocks[-1].append(line)
    return ["\n".join(block) for block in blocks]


def reference_axes(path, face):
    """The font's axes, from the reference and with no location."""
    argv = oracle_env.command("fonttools", ["python3", REFERENCE, path,
        str(face), "1", "0"])
    finished = subprocess.run(argv, capture_output=True, text=True)
    if finished.returncode != 0:
        raise Skip("fontTools: %s"
                   % (finished.stderr.strip().splitlines()[-1]
                      if finished.stderr.strip() else "refused"))
    if "outlines: no gvar" in finished.stdout:
        raise Skip("no gvar", expected=True)
    axes = []
    for line in finished.stdout.splitlines():
        if line.startswith("axis: "):
            tag, low, default, high = line[6:].split()
            axes.append((tag, float(low), float(default), float(high)))
    return axes


def normalised_of(text):
    """The `variation:` line's coordinates, or None."""
    for line in text.splitlines():
        if line.startswith("variation: "):
            return [int(value) for value in
                    line.split("normalised", 1)[1].split()]
    return None


def close(want, got, histogram, tolerance):
    """Two 26.6 coordinates, allowing `tolerance` 64ths and counting the rest."""
    left = int(str(want).rstrip("~"))
    right = int(got)
    difference = abs(left - right)
    if difference > tolerance:
        return False
    histogram[difference] = histogram.get(difference, 0) + 1
    return True


def contour_close(theirs, mine, tolerance):
    """Whether two closed contours are the same cycle, within the allowance.

    Returns the per-coordinate differences if they are, else None. **Every
    rotation is tried**, because a closed contour has no first vertex and the two
    readers begin at different ones. glyf_diff rotates each side to its
    lexicographically smallest start, which is stable only while the two sides
    hold *equal* numbers: here they differ by a 64th, and the smallest rotation of
    one can be a different segment from the smallest rotation of the other. That
    was the whole of nine Inter disagreements before this was written, and the
    way it showed was a path of the right length with every segment a 64th off
    from the wrong neighbour.
    """
    count = len(theirs)
    if count != len(mine):
        return None
    if count == 0:
        # A contour that was one point repeated: both sides dropped its zero-length
        # line and are left with nothing to draw, which is a match. `range(0)`
        # would try no rotation at all and call it a mismatch.
        return []
    for shift in range(count):
        differences = []
        for index in range(count):
            left = theirs[index]
            right = mine[(index + shift) % count]
            if len(left) != len(right) or left[0] != right[0]:
                break
            for a, b in zip(left[1:], right[1:]):
                difference = abs(int(str(a).rstrip("~")) - int(b))
                if difference > tolerance:
                    break
                differences.append(difference)
            else:
                continue
            break
        else:
            return differences
    return None


def paths_close(want, got, histogram, tolerance):
    """Two paths, contour for contour, each as a cycle within the allowance."""
    if want is None or got is None or len(want) != len(got):
        return False
    matched = []
    for theirs, mine in zip(want, got):
        differences = contour_close(theirs, mine, tolerance)
        if differences is None:
            return False
        matched.extend(differences)
    for difference in matched:
        histogram[difference] = histogram.get(difference, 0) + 1
    return True


def compare(path, face, stride, locations, report, histogram, normal_histogram,
            references):
    """One font at every planned location, against each reference.

    Returns (compared, disagreements, glyph-locations, locations, declined).
    """
    axes = reference_axes(path, face)
    if not axes:
        raise Skip("no axes", expected=True)
    seed = zlib.crc32(os.path.basename(path).encode("utf-8"))
    places = plan(axes, seed, locations)

    mine = []
    for place in places:
        mine.append(ours(path, face, stride, place))
    specs = []
    for place, text in zip(places, mine):
        coords = normalised_of(text)
        specs.append("%s@%s" % (place, ",".join(str(c) for c in coords)
                                 if coords else ""))
    blocks_by = {}
    if "fontTools" in references:
        _, _, blocks_by["fontTools"] = reference(path, face, stride, specs)
    if "FreeType" in references:
        blocks_by["FreeType"] = reference_freetype(path, face, stride, specs)
    for name, blocks in blocks_by.items():
        if len(blocks) != len(places):
            raise Skip("%s answered %d of %d locations"
                       % (name, len(blocks), len(places)))

    compared = 0
    disagreements = 0
    glyph_places = 0
    declined = 0
    shown = 0

    def complain(message):
        nonlocal shown
        if shown < PER_FONT and report[0] < TOTAL:
            sys.stderr.write("  %s: %s\n" % (os.path.basename(path), message))
            shown += 1
            report[0] += 1

    seen_glyph_places = set()
    for name, blocks in blocks_by.items():
        for number, (place, text, block) in enumerate(zip(places, mine, blocks)):
            want = normalised_of(block)
            got = normalised_of(text)
            compared += 1
            if want is None or got is None or len(want) != len(got):
                disagreements += 1
                complain("%s: %s: normalised coordinates: %s %r, this library %r"
                         % (name, place, name, want, got))
                continue
            for a, b in zip(want, got):
                difference = abs(a - b)
                key = (name, "normalised")
                normal_histogram.setdefault(name, {})
                normal_histogram[name][difference] = \
                    normal_histogram[name].get(difference, 0) + 1
                if difference > NORMAL_ALLOWANCE[name]:
                    disagreements += 1
                    complain("%s: %s: normalised coordinates: %s %r, this "
                             "library %r" % (name, place, name, want, got))
                    break

            theirs = glyf_diff.parse(block, scale=1)
            mine_values = glyf_diff.parse(text, scale=glyf_diff.ONE_PIXEL)
            refused = {key.split(".")[1]: str(value) for key, value in
                       mine_values.items()
                       if key.endswith(".shape")
                       and str(value).startswith("refused")}
            glyphs = set()
            for key in set(theirs) | set(mine_values):
                if not key.endswith(".path"):
                    continue
                glyph = key.split(".")[1]
                if glyph in refused:
                    continue
                glyphs.add(glyph)
                compared += 1
                composite = str(mine_values.get("glyph.%s.shape" % glyph, "")
                                ).startswith("composite 1")
                kind = "composite" if composite else "simple"
                if name == "fontTools":
                    allowed = COMPOSITE_ALLOWANCE if composite else POINT_ALLOWANCE
                else:
                    allowed = (FREETYPE_COMPOSITE_ALLOWANCE if composite
                               else FREETYPE_POINT_ALLOWANCE)
                if paths_close(theirs.get(key), mine_values.get(key),
                               histogram.setdefault((name, kind), {}), allowed):
                    continue
                disagreements += 1
                complain("%s: %s: glyph %s: %s %r, this library %r"
                         % (name, place, glyph, name, theirs.get(key),
                            mine_values.get(key)))
            if name == "fontTools":
                for glyph, why in refused.items():
                    declined += 1
                    disagreements += 1
                    complain("%s: glyph %s: this library refused it as %r at a "
                             "location no glyph is documented to be refused at"
                             % (place, glyph, why))
                glyph_places += len(glyphs)
    return compared, disagreements, glyph_places, len(places), declined


def main(argv):
    global POINT_ALLOWANCE, COMPOSITE_ALLOWANCE, FREETYPE_POINT_ALLOWANCE
    global FREETYPE_COMPOSITE_ALLOWANCE
    references = ["fontTools", "FreeType"]
    quiet = "--quiet" in argv
    stride = GLYPH_STRIDE
    locations = None
    limit = None
    rest = []
    skip_next = False
    for index, argument in enumerate(argv[1:], start=1):
        if skip_next:
            skip_next = False
            continue
        if argument in ("--stride", "--glyphs"):
            stride = int(argv[index + 1])
            skip_next = True
        elif argument == "--fonts":
            limit = int(argv[index + 1])
            skip_next = True
        elif argument == "--locations":
            locations = int(argv[index + 1])
            skip_next = True
        elif argument == "--only":
            references = [argv[index + 1]]
            skip_next = True
        elif argument == "--allowance":
            # For measuring: widen the coordinate allowance to see how far a
            # disagreement really goes. The default is the claim; this is how it
            # is checked.
            POINT_ALLOWANCE = int(argv[index + 1])
            COMPOSITE_ALLOWANCE = POINT_ALLOWANCE
            FREETYPE_POINT_ALLOWANCE = POINT_ALLOWANCE
            FREETYPE_COMPOSITE_ALLOWANCE = POINT_ALLOWANCE
            skip_next = True
        elif argument == "--quiet":
            continue
        else:
            rest.append(argument)

    if not os.path.exists(DRIVER):
        sys.stderr.write("%s is not built; run `make examples`\n" % DRIVER)
        return 1

    if rest:
        fonts = rest
        synthetic = 0
    else:
        real = corpus.fonts("variable")
        if limit is not None:
            step = max(1, len(real) // limit)
            real = real[::step][:limit]
        synthetic = len(fixtures())
        fonts = fixtures() + real
    if not fonts:
        sys.stderr.write("no fonts to compare\n")
        return 1

    compared = 0
    disagreements = 0
    glyph_places = 0
    places = 0
    declined = 0
    skipped = []
    report = [0]
    histogram = {}
    normal_histogram = {}
    synthetic_paths = set(fixtures())
    for path in fonts:
        try:
            fields, differed, seen, where, refused = compare(path, 0,
                1 if path in synthetic_paths else stride, locations, report,
                histogram, normal_histogram, references)
        except Skip as why:
            skipped.append((path, str(why), why.expected))
            continue
        compared += fields
        disagreements += differed
        glyph_places += seen
        places += where
        declined += refused
        if not quiet:
            sys.stdout.write("  %-46s %3d locations, %8d glyph-locations%s\n"
                % (os.path.basename(path), where, seen,
                   ", disagreements: %d" % differed if differed else ""))

    print("var_diff: %d font(s) (%d synthetic fixtures at every glyph, %d real "
          "at every %dth), %d locations, %d glyph-locations, %d comparisons, %d "
          "disagreements"
          % (len(fonts) - len(skipped), synthetic, len(fonts) - synthetic,
             stride, places, glyph_places, compared, disagreements))
    for name in references:
        print("var_diff: normalised coordinates, by how far %s's differs from "
              "this library's in 2.14 units: %s; %d allowed"
              % (name, ", ".join("%d: %d" % (k, v) for k, v in sorted(
                  normal_histogram.get(name, {}).items())) or "none",
                 NORMAL_ALLOWANCE[name]))
        for kind in ("simple", "composite"):
            if name == "fontTools":
                allowed = COMPOSITE_ALLOWANCE if kind == "composite" \
                    else POINT_ALLOWANCE
            else:
                allowed = FREETYPE_COMPOSITE_ALLOWANCE if kind == "composite" \
                    else FREETYPE_POINT_ALLOWANCE
            print("var_diff: %s glyphs' coordinates, by how far %s's differs "
                  "from this library's in 64ths of a font unit: %s; %d allowed"
                  % (kind, name, ", ".join("%d: %d" % (k, v) for k, v in sorted(
                      histogram.get((name, kind), {}).items())) or "none",
                     allowed))
    for path, why, _ in skipped:
        print("var_diff: skipped %s: %s" % (os.path.basename(path), why))

    dropped = unskippable.check("var_diff",
        [(path, why) for path, why, expected in skipped if not expected])
    if dropped:
        return 1
    if not compared:
        sys.stderr.write("var_diff: nothing was compared\n")
        return 1
    if glyph_places < 100:
        sys.stderr.write("var_diff: only %d glyph-locations across %d fonts, "
                         "which is too few to be a corpus\n"
                         % (glyph_places, len(fonts)))
        return 1
    return 1 if disagreements else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
