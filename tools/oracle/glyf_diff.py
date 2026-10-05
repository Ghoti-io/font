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
"""Every glyph's outline, this library against fontTools.

documentation/design.md sections 7.3 and 14. `ttx_diff.py` compares the tables
this library parses; this compares the *shapes* it builds out of one of them,
which is a different question and needs a different reference call.

What it compares, per glyph:

  * whether the font assembled the glyph from components,
  * the stated bounding box - the **coordinate** box, which is what `glyf` means
    by xMin/yMax - and the box the points actually span,
  * every point's coordinates and its on-curve flag, after composites are
    resolved,
  * where each contour starts and how many points it has,
  * and the **path**: the segments `gfnt_outline_decompose()` makes of those
    points, against the segments a reference pen makes of the same ones.

The last two are not the same comparison, and that is the point of having both.
A reader can have every point and flag exactly right and still get `glyf`'s two
implicit constructions wrong - the on-curve point between two off-curve points,
and a contour that begins off-curve - because neither of those is in the file.
Only the path sees them. And a reader can produce a plausible path from points it
read wrongly, which only the point comparison sees.

**Paths are compared as cycles.** A closed contour has no starting vertex: where
a walk begins is a choice, and the two defensible answers differ - this library
and FreeType begin at the contour's last point, fontTools rotates to the first
on-curve point and draws the closing line explicitly. Both describe the same
closed contour. So each contour's segments are closed explicitly and then rotated
to a canonical start before comparison. Nothing else is normalised: the segment
types, their control points and their order all have to match.

Usage:
    glyf_diff.py [--stride N] [--fonts N] [--glyphs N] [--quiet] [font...]
"""

import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import corpus
import oracle_env
import unskippable

ROOT = oracle_env.ROOT
DRIVER = os.path.join(ROOT, "build", "linux", "release", "apps", "examples",
                      "font-outline")
REFERENCE = os.path.join(ROOT, "tools", "oracle", "fonttools_outlines.py")
FIXTURES = os.path.join(ROOT, "tests", "data", "fonts")

# Every nth glyph, by default. A prime, so the sample is not aligned with any
# block structure a font's glyph order happens to have - a stride of 10 in a
# font laid out in tens would ask about the same position in every block.
GLYPH_STRIDE = 7

# How many disagreements to print per font and in total.
PER_FONT = 6
TOTAL = 60

# The refusals this library is documented to make, which a glyph may carry
# instead of an outline. Anything else it refuses is a disagreement.
DOCUMENTED_REFUSALS = {
    # head.glyphDataFormat 1, or flag bit 0x80: the cubic `glyf` extension,
    # refused by name rather than drawn as quadratics (design.md section 7.3).
    "refused Unsupported feature",
    # A `loca` entry running backwards or past `glyf`, which condemns that glyph
    # and not the font (M11).
    "refused Corrupt data",
}

# 26.6: our driver prints font units times 64, the reference prints font units.
ONE_PIXEL = 64


class Skip(Exception):
    """This font cannot be compared, with a reason worth printing.

    `expected` separates two events that wear one word. A font with no `glyf`, or
    one both readers agree carries no indexable outline, was never going to be
    compared here and is counted. Anything else is a font this differential
    *meant* to compare and could not - and tools/oracle/unskippable.py fails the
    run when such a font is an sfnt, because that is the one event a clean total
    cannot be told apart from agreement.
    """

    def __init__(self, why, expected=False):
        super().__init__(why)
        self.expected = expected


def fixtures():
    """The synthetic fixtures, which are in every run by default.

    The same reason ttx_diff has them: the corpus holds no composite with a
    two-by-two transform, no contour stored beginning off-curve with a single
    on-curve point, and no `loca` entry running backwards. A differential whose
    population is only real fonts cannot ask about a construction no real font
    in it uses, and thinning a run with `--fonts` drops corpus fonts and never
    these.

    **And every glyph of them is compared, whatever `--stride` says.** That is
    not a detail: at the default stride of seven, the glyph carrying the
    two-by-two transform, the glyph stored beginning off-curve and the glyph
    using the cubic extension all fell between samples, and three planted
    defects went undetected in a run reporting zero. A fixture holds one
    construction in one glyph, so sampling its glyphs is sampling away the whole
    reason it exists; a real font holds thousands of glyphs drawing the same few
    shapes, which is what a stride is for.
    """
    manifest = os.path.join(FIXTURES, "MANIFEST")
    if not os.path.exists(manifest):
        return []
    found = []
    with open(manifest, encoding="utf-8") as handle:
        for line in handle:
            if line.startswith("#") or not line.strip():
                continue
            name = line.split("\t")[0]
            if name.endswith(".ttc"):
                # A collection needs a face index per face; the fixtures'
                # collection carries no outlines worth comparing that its
                # single-face siblings do not.
                continue
            found.append(os.path.join(FIXTURES, name))
    return found


def ours(path, face, stride, first):
    """This library's dump, as {key: value}."""
    finished = subprocess.run([DRIVER, path, str(face), str(stride),
        str(first)], capture_output=True, text=True)
    if finished.returncode != 0:
        raise Skip(finished.stderr.strip().splitlines()[-1]
                   if finished.stderr.strip() else "the driver refused it")
    return parse(finished.stdout, scale=ONE_PIXEL)


def stated_box(text, scale):
    """The box the file states, in whole font units.

    The one value the two sides print in different units: the file holds whole
    font units, the driver prints 26.6, so ours is divided back. A value that
    does not divide is kept as itself, because that would mean the driver had
    invented a fraction the file cannot hold and silently rounding it away is
    how that stops being visible.
    """
    if text in ("empty", "none"):
        return text
    out = []
    for number in text.split():
        value = int(number)
        if scale == 1 or value % scale == 0:
            out.append(str(value // scale if scale != 1 else value))
        else:
            out.append("%d/%d" % (value, scale))
    return " ".join(out)


def reference(path, face, stride, first):
    """fontTools' answer for the same glyphs, as {key: value}."""
    argv = oracle_env.command("fonttools",
        ["python3", REFERENCE, path, str(face), str(stride), str(first)])
    finished = subprocess.run(argv, capture_output=True, text=True)
    if finished.returncode != 0:
        raise Skip("fontTools: %s"
                   % (finished.stderr.strip().splitlines()[-1]
                      if finished.stderr.strip() else "refused"))
    return parse(finished.stdout, scale=1)


# Both sides print coordinates in 26.6 and nothing here rescales them. The
# reference multiplies its own by 64 rather than rounding to font units, because
# an implicit midpoint is a *half* unit whenever two off-curve coordinates sum to
# an odd number - which is most fonts - and this library holds that half exactly.
# The first run of this differential rounded the reference's midpoints and
# reported a disagreement for every one of them.


def parse(text, scale):
    """One side's output, as {key: value}, with the paths kept per contour."""
    values = {}
    paths = {}
    for line in text.splitlines():
        if line.startswith("outlines: "):
            text = line[len("outlines: "):]
            if text.endswith("inexact coordinate(s)"):
                values["inexact"] = int(text.split()[0])
            else:
                values["outlines"] = text
            continue
        if not line.startswith("glyph "):
            continue
        rest = line[len("glyph "):]
        index, _, rest = rest.partition(" ")
        # A `glyph N: ...` line puts the colon on the index, and a
        # `glyph N point M: ...` line does not. Stripping it here rather than
        # matching two shapes is what makes the first kind parse at all: it did
        # not, so the composite flag, the point count and the contour count were
        # in neither side's dictionary and were never compared - and a field an
        # instrument does not emit reads exactly like a field that agrees.
        colon = index.endswith(":")
        if colon:
            index = index[:-1]
        if not index.isdigit():
            continue
        glyph = int(index)
        if colon:
            values["glyph.%d.shape" % glyph] = rest
        elif rest.startswith("stated: "):
            # The file's own box, in whole font units on both sides: ours comes
            # out of the driver in 26.6, so it is divided back.
            values["glyph.%d.stated" % glyph] = stated_box(rest[8:], scale)
        elif rest.startswith("drawn: "):
            # Ours only: the curve's own box has no counterpart in the file or
            # in the reference, so it is read and not compared.
            continue
        elif rest.startswith("control: "):
            values["glyph.%d.control" % glyph] = box(rest[9:])
        elif rest.startswith("contour "):
            number, _, tail = rest[len("contour "):].partition(": ")
            values["glyph.%d.contour.%s" % (glyph, number)] = tail
        elif rest.startswith("point "):
            number, _, tail = rest[len("point "):].partition(": ")
            values["glyph.%d.point.%s" % (glyph, number)] = coordinates(tail)
        elif rest.startswith("path: "):
            paths.setdefault(glyph, []).append(segment(rest[6:]))
    for glyph, segments in paths.items():
        values["glyph.%d.path" % glyph] = canonical(segments)
    return values


def box(text):
    """A box's four numbers, in 26.6 on both sides.

    Kept as text rather than parsed: a reference value may carry the `~` that
    says it fell between two 64ths, and near() is what reads it.
    """
    if text == "empty":
        return "empty"
    return " ".join(text.split())


def coordinates(text):
    """A point: two numbers in 26.6 and a tag."""
    parts = text.split()
    if len(parts) < 3:
        return text
    return "%s %s %s" % (parts[0], parts[1], parts[2])


def segment(text):
    """One path segment as (verb, coordinates...), in 26.6."""
    parts = text.split()
    verb = parts[0]
    if verb == "refused":
        return (text,)
    return tuple(parts)


def near(want, got, allowance):
    """Whether two values agree, allowing one 64th where the reference said so.

    A reference value marked `~` fell between two 64ths, so the two sides
    rounded it differently and a difference of one unit is the most that can
    mean. `allowance` counts how many such comparisons a run rested on and the
    largest difference any of them showed, so that the report can say both
    rather than asserting that they were small.
    """
    if want is None or got is None:
        return False
    want_parts = str(want).split()
    got_parts = str(got).split()
    if len(want_parts) != len(got_parts):
        return False
    marked = False
    for left, right in zip(want_parts, got_parts):
        if left == right:
            continue
        if not left.endswith("~"):
            return False
        try:
            difference = abs(int(left[:-1]) - int(right))
        except ValueError:
            return False
        if difference > 1:
            return False
        marked = True
        allowance[1] = max(allowance[1], difference)
    if marked:
        allowance[0] += 1
    return True


def paths_agree(want, got, allowance):
    """Two canonical paths, allowing the same one-64th where it was marked."""
    if want is None or got is None or len(want) != len(got):
        return False
    for mine, theirs in zip(got, want):
        if len(mine) != len(theirs):
            return False
        for left, right in zip(theirs, mine):
            if len(left) != len(right) or left[0] != right[0]:
                return False
            if not near(" ".join(left[1:]), " ".join(right[1:]), allowance):
                return False
    return True


# How many zero-length `line` segments were dropped, over the whole run. Printed,
# because a normalisation nobody counts is indistinguishable from one that is
# quietly absorbing real differences.
DEGENERATE = [0]


def canonical(segments):
    """The path as a tuple of contours, each rotated to a canonical start.

    A contour arrives as `move`, some segments, `close`. Three normalisations, in
    this order, and each is a choice with no meaning that the two sides make
    differently:

      1. **A zero-length `line` is dropped.** A `line` to the point the walk is
         already standing on draws nothing, and whether a pen emits one is
         decided by which of two *coincident* points it treats as the contour's
         start. `UKIJTughra.ttf` has contours ending in two identical on-curve
         points - glyph 462's contour 7 ends (516,505), (516,505) - and begins
         off-curve, so the start is one of that pair and the two readers pick
         different ones. fontTools then walks from one duplicate to the other and
         emits a line of length zero; this library starts at the other and emits
         none. The curves are identical; it is the segment count that differed, so
         rotation alone could not reconcile them.
      2. **The closing segment is made explicit** - a walk that ends where it
         started emits none and one that ends elsewhere leaves it to `close` - so
         that both sides have the same number of segments whichever rule picked
         the starting vertex.
      3. **The cycle is rotated to its smallest rotation**, since a closed contour
         has no first vertex.

    Dropping a degenerate line is shape-preserving in a way an exemption would not
    be: it removes a segment that contributes no ink and moves no point, from both
    sides, and the count is reported.
    """
    out = []
    contour = None
    start = None
    current = None
    for piece in segments:
        if piece[0] == "move":
            contour = []
            start = piece[1:]
            current = start
            continue
        if piece[0] == "close":
            if contour is None:
                continue
            if current != start:
                contour.append(("line",) + start)
            out.append(rotate(contour))
            contour = None
            continue
        if contour is None:
            # A segment before any move, which no walk produces: kept as itself
            # so that a comparison sees it rather than dropping it.
            out.append((piece,))
            continue
        if piece[0] == "line" and tuple(piece[1:]) == tuple(current):
            # Normalisation 1. Not appended, and `current` is unchanged because
            # the segment did not move anything.
            DEGENERATE[0] += 1
            continue
        contour.append(piece)
        current = piece[-2:]
    if contour:
        out.append(rotate(contour))
    return tuple(out)


def rotate(contour):
    """The lexicographically smallest rotation of a cycle of segments."""
    if not contour:
        return ()
    rotations = [tuple(contour[at:] + contour[:at])
                 for at in range(len(contour))]
    return min(rotations)


# How many fonts both readers agree carry no indexable outline, counted rather
# than left in the skip list: a *stated* skip and a reference crash print the same
# "skipped" line, and the five fonts this counts arrived in the corpus as a crash.
BITMAP_ONLY = [0]


def compare(path, face, stride, report, allowance):
    """One font. Returns (compared, disagreements, glyphs, inexact, declined)."""
    mine = ours(path, face, stride, 0)
    theirs = reference(path, face, stride, 0)

    if theirs.get("outlines") == "no glyf":
        raise Skip("no glyf table", expected=True)

    if theirs.get("outlines", "").startswith("no indexed glyf"):
        # The bitmap-only sfnt: `glyf` of zero bytes under a `loca` of two, which
        # is five of this corpus's 506 fonts and 100% of that shape. Neither
        # reader can index an outline in it, and both say so - fontTools by
        # loading no glyphs at all and warning about `loca` while it does, this
        # library by answering GFNT_ERR_UNSUPPORTED for the face.
        #
        # **Asserted and then skipped, in that order**, and the assertion is on
        # the *kind* of refusal rather than on there being one. This library used
        # to answer GFNT_ERR_CORRUPT for all 1,326 of Terminus's glyphs while
        # gfnt_face_has_outlines() said yes - a refusal, so a check for "did it
        # refuse" would have passed and let the defect back in. "Corrupt" here
        # means this library read the `loca` as indexing something and found that
        # something broken; what is true is that the table indexes nothing, which
        # is a fact about the face and is GFNT_ERR_UNSUPPORTED. The driver prints
        # the result's own name, so the two are distinguishable in the output both
        # sides already produce and no field had to be invented to tell them apart.
        shapes = [str(value) for key, value in mine.items()
                  if key.endswith(".shape")]
        wrong = [shape for shape in shapes
                 if not shape.startswith("refused Unsupported")]
        if not shapes or wrong:
            report[0] += 1
            print("  %s: the reference indexes no glyf here; this library "
                  "answered %s for %d of %d sampled glyph(s) rather than "
                  "refusing the face as unsupported"
                  % (os.path.basename(path),
                     wrong[0] if wrong else "nothing", len(wrong), len(shapes)))
            return (0, 1, 0, 0, 0)
        BITMAP_ONLY[0] += 1
        raise Skip("no outline a loca indexes - bitmap-only, and both readers "
                   "say so", expected=True)

    # Glyphs this library declined. Their other keys are not compared: a glyph
    # it refused to read has no points to disagree about, and counting each of
    # them would turn one documented refusal into a hundred disagreements.
    declined = {}
    for key, value in mine.items():
        if key.endswith(".shape") and str(value).startswith("refused "):
            declined[key.split(".")[1]] = str(value)

    keys = sorted(set(mine) | set(theirs))
    shown = 0
    compared = 0
    disagreements = 0
    undocumented = 0
    glyphs = set()
    for key in keys:
        if key in ("outlines", "inexact"):
            # The header carries the stride and the glyph count; the count is
            # compared through numGlyphs in ttx_diff and the stride is ours.
            continue
        parts = key.split(".")
        if len(parts) > 1 and parts[1] in declined:
            if key.endswith(".shape"):
                if declined[parts[1]] not in DOCUMENTED_REFUSALS:
                    undocumented += 1
                    if shown < PER_FONT and report[0] < TOTAL:
                        sys.stderr.write(
                            "  %s: %s: this library refused it as %r, which is "
                            "not a documented refusal\n"
                            % (os.path.basename(path), key, declined[parts[1]]))
                        shown += 1
                        report[0] += 1
            continue
        want = theirs.get(key)
        got = mine.get(key)
        if want is None and got is None:
            continue
        compared += 1
        if key.startswith("glyph."):
            glyphs.add(parts[1])
        if key.endswith(".path"):
            if paths_agree(want, got, allowance):
                continue
        elif near(want, got, allowance):
            continue
        disagreements += 1
        if shown < PER_FONT and report[0] < TOTAL:
            sys.stderr.write("  %s: %s: fontTools %r, this library %r\n"
                % (os.path.basename(path), key, want, got))
            shown += 1
            report[0] += 1
    return (compared, disagreements + undocumented, len(glyphs),
            theirs.get("inexact", 0), len(declined))


def main(argv):
    quiet = "--quiet" in argv
    stride = GLYPH_STRIDE
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
        real = corpus.fonts("sfnt")
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
    glyphs = 0
    inexact = 0
    declined = 0
    skipped = []
    report = [0]
    # [how many comparisons rested on the one-64th allowance, the largest
    # difference any of them showed].
    allowance = [0, 0]

    synthetic_paths = set(fixtures())
    for path in fonts:
        try:
            # Every glyph of a fixture, a sample of a real font. See fixtures().
            fields, differed, seen, imprecise, refused = compare(path, 0,
                1 if path in synthetic_paths else stride, report, allowance)
        except Skip as why:
            skipped.append((path, str(why), why.expected))
            continue
        compared += fields
        disagreements += differed
        glyphs += seen
        inexact += imprecise
        declined += refused
        if not quiet:
            sys.stdout.write("  %-52s %6d glyphs, %7d fields%s\n"
                % (os.path.basename(path), seen, fields,
                   ", disagreements: %d" % differed if differed else ""))

    print("glyf_diff: %d font(s) (%d synthetic fixtures at every glyph, %d "
          "real at every %dth), %d glyphs, %d fields compared, %d "
          "disagreements"
          % (len(fonts) - len(skipped), synthetic, len(fonts) - synthetic,
             stride, glyphs, compared, disagreements))
    # Reported rather than tolerated. A coordinate the reference could not hold
    # in 26.6 comes from a composite whose transform is not dyadic: fontTools
    # composes it in floating point and this library in fixed point, so the two
    # round the same number differently. The allowance is one 64th of a font
    # unit and applies only where the reference marked the value; both figures
    # are printed so that a run which rests heavily on it says so, and so that
    # the largest difference is a measurement rather than a claim.
    print("glyf_diff: %d zero-length line segment(s) dropped from both sides - a "
          "line to the point the walk already stands on, which is what two readers "
          "emit differently when a contour ends in two coincident on-curve points "
          "and so has two candidate starting vertices. Shape-preserving: no ink, "
          "no point moved" % DEGENERATE[0])
    print("glyf_diff: %d coordinate(s) the reference could not hold in 26.6; "
          "%d comparison(s) rested on the one-64th allowance, largest "
          "difference %d/64 of a font unit"
          % (inexact, allowance[0], allowance[1]))
    if declined:
        print("glyf_diff: %d glyph(s) this library declined and is documented "
              "to decline" % declined)
    print("glyf_diff: %d font(s) both readers agree carry no outline any `loca` "
          "indexes - the bitmap-only sfnt, `glyf` of zero bytes under a `loca` of "
          "two. Not a comparison and not a crash either: for each, this library "
          "was checked to refuse the whole face rather than read an outline the "
          "reference says is not indexed" % BITMAP_ONLY[0])
    for path, why, _ in skipped:
        print("glyf_diff: skipped %s: %s" % (os.path.basename(path), why))

    # The gate: a file the corpus selector calls an sfnt may not leave the
    # denominator because something failed. See tools/oracle/unskippable.py - this
    # differential is where that clause was caught hiding five real fonts.
    dropped = unskippable.check("glyf_diff",
        [(path, why) for path, why, expected in skipped if not expected])

    # The denominators, so that a differential which has stopped comparing
    # cannot report a clean run.
    if dropped:
        return 1
    if not compared:
        sys.stderr.write("glyf_diff: nothing was compared\n")
        return 1
    if glyphs < 100:
        sys.stderr.write(
            "glyf_diff: only %d glyphs across %d fonts, which is too few to be "
            "a corpus\n" % (glyphs, len(fonts)))
        return 1
    return 1 if disagreements else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
