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
"""Write tests/data/golden/coverage.txt: every fixture glyph, rendered.

documentation/design.md section 14.4. Every glyph of every committed fixture, at
six pixel sizes and four sub-pixel offsets plus one vertical one, with the
coverage's shape, its total and its hash.

**The committed file is the whole gate.** Three things read it, and they answer
three different questions:

  * `testGolden` re-renders on the build host and compares. Needs no container,
    so it runs in `make test` on a fresh clone, and it catches a change in the
    rasteriser that nobody intended.
  * `make check-golden` builds the same driver for a **big-endian** target and
    runs it under qemu. A hash that differs between the two is a host-dependent
    read or a `float` - which is section 1's determinism promise, and this is the
    only gate that can see it.
  * a person, when a glyph comes out wrong: the totals beside the hashes say how
    far off it is, and `font-render --art` draws it.

No container is needed to *write* it either: the fixtures are in the repository
and the driver is built by `make examples`. That is deliberate - a golden file
that could only be regenerated inside an image would be a file nobody
regenerates.

**Only the fixtures with outlines of their own are committed**, which is six of
the twenty-four. The rest vary a *table* - an `OS/2` version, a `cmap` format, a
`post` format - and draw the same five outlines as `basic.ttf`; committing their
renderings would have quintupled the file with nothing new in it. `post-v1.ttf`
alone would have been 6,450 lines of the same five shapes.

That exclusion is **probed rather than asserted**: every excluded fixture is
rendered too, and its first glyphs have to hash the same as `basic.ttf`'s. So a
fixture that quietly stops sharing those outlines fails the generator instead of
silently leaving a shape uncovered - which is what an exclusion nobody checks
turns into.
"""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
DRIVER = os.path.join(ROOT, "build", "linux", "release", "apps", "examples",
                      "font-render")
# The second driver: a bitmap container has no outlines to rasterise, and its
# strike's pixels reach a GFNT_Coverage through gfnt_coverage_from_bitmap(). Every
# step from a file's bytes to those pixels is an explicit shift, so they *should*
# be identical on a big-endian machine - and that is a claim until this gate
# measures it, which is the same reason the scan converter is here.
BITMAP_DRIVER = os.path.join(ROOT, "build", "linux", "release", "apps",
                             "examples", "font-bitmap")
FIXTURES = os.path.join(ROOT, "tests", "data", "fonts")
GOLDEN = os.path.join(ROOT, "tests", "data", "golden", "coverage.txt")

# The fixtures whose renderings are committed: the ones that carry outlines of
# their own. `basic.ttf` is the five outlines every phase 0 fixture shares; the
# five `outline-*.ttf` are phase 1's shapes.
WITH_OUTLINES = (
    # `bare.cff` is deliberately not here. Its font program is byte-for-byte
    # cff-curves.otf's, so its renderings are the same 175 images under another
    # name, and committing them would say nothing about big-endian reproduction
    # that the wrapped fixture does not already say. That the two rasterise
    # identically is asserted by testCff instead, which is where the claim
    # belongs - it is about the container, not about the scan converter. The probe
    # below is what keeps the exclusion honest, and it compares against the whole
    # committed set for exactly this fixture's sake.
    "basic.ttf",
    "outline-simple.ttf",
    "outline-composite.ttf",
    "outline-loca-long.ttf",
    "outline-cubic.ttf",
    "outline-cubic-flag.ttf",
    # Its last glyph is refused - a `loca` entry running backwards - and that
    # refusal is committed beside the shapes, because a glyph that started
    # rendering would mean M11's check had stopped firing.
    "outline-broken-loca.ttf",
    # Phase 2's charstrings, which reach the scan converter through a different
    # producer and - `cff-curves` and `cff-type1` above all - through **cubic**
    # segments, where `glyf` sends quadratics. The flattener's two arms are the
    # reason these are committed rather than probed: a cubic subdivided to a
    # different depth on a big-endian machine is exactly what this gate is for.
    "cff-curves.otf",
    "cff-type1.otf",
    "cff-arith.otf",
    "cff-cid.otf",
    "cff-hints.otf",
    "cff-seac.otf",
    "cff-subrs.otf",
    # Phase 3's Type 1 container. `type1.pfb` and `type1.pfa` are deliberately
    # absent - they hold cff-type1.otf's eight charstrings, so the probe below
    # covers them - but `type1-big.pfb` has eighty glyphs of its own, and by this
    # list's own rule a fixture with outlines of its own belongs here. It costs
    # two thousand lines, which is the price of the rule: an exclusion the probe
    # cannot justify is an exclusion nobody is checking.
    "type1-big.pfb",
)

# Fixtures that must render **nothing** through the rasteriser.
#
# `cff.otf` was here until phase 2, on the grounds that a font with no `glyf`
# could not draw. It can, and what it draws is worth more than the refusals it
# used to contribute: its charstrings are the same five outlines `basic.ttf`
# holds as `glyf`, so the subset check below now asserts that **two containers
# and one rasteriser produce identical pixels**. That is a cross-format
# comparison this gate could not make before and gets for free.
#
# The nine bitmap containers are here because they have no outlines at all, and
# that is the assertion: `gfnt_face_render_glyph()` must refuse every glyph of
# them. They would pass the subset probe below without it - a fixture that draws
# nothing draws no shape `basic.ttf` lacks - which is the shape of an absence
# assertion that goes on passing after it stops meaning anything. Their *pixels*
# are committed below, through the other driver.
# The strikes whose pixels are committed: all nine, because unlike the table
# fixtures no two of these draw the same thing through the same code. The three
# PCFs hold one design in three layouts, and that they agree is a unit test; that
# each *reproduces* on a big-endian machine is this file.
WITH_STRIKES = (
    "bitmap.hex",
    "bitmap-wide.hex",
    "bitmap.psf",
    "bitmap-v1.psf",
    "bitmap.bdf",
    "bitmap-ink.bdf",
    "bitmap.pcf",
    "bitmap-lsb.pcf",
    "bitmap-swap.pcf",
)

# Fixtures that must render **nothing through the rasteriser**.
#
# The nine strikes above are all of them and one more: they have no outlines at
# all, and that is the assertion. They would pass the subset probe below without
# it - a fixture that draws nothing draws no rendering the committed set lacks -
# which is the shape of an absence assertion that goes on passing after it stops
# meaning anything.
#
# `bare-matrix.cff` states a FontMatrix that reduces to no em, so every glyph of it
# is refused at every size. **It was in neither list and the generator had been
# failing on it since it landed** - and nothing noticed, because `check-golden`
# reads the committed file and no gate ran this script. That hole is closed in
# check_golden.py, which now regenerates first.
#
# `bitmap-gz.pcf.gz` is a strike too, and is here rather than in WITH_STRIKES: its
# renderings would be `bitmap.pcf`'s under another name, and what needs saying about
# it is about the gzip wrapper rather than about the pixels - which is
# `test_bitmap.cpp`'s to say, as `bare.cff`'s equivalence is `test_cff.cpp`'s.
NO_OUTLINES = ("bare-matrix.cff", "bitmap-gz.pcf.gz") + WITH_STRIKES

HEADER = """\
# Every committed fixture's glyphs, rasterised.
#
# Generated by tools/golden/make_golden.py from tests/data/fonts, through
# examples/font-render.c. Do not edit: `make golden` rewrites it and
# `make check-golden` fails on a difference.
#
# One line per rendering:
#
#   <fixture> <glyph> <ppem> <offset-x> <offset-y> <w>x<h>+<left>+<top> \\
#       <coverage total> <hash>
#
# The offsets are 26.6, so 16 is a quarter of a pixel. `left` and `top` are the
# integer position of the bitmap's top-left corner relative to the glyph origin,
# with y up. The total is the sum of every pixel's coverage, printed beside the
# hash because a hash says only "different" where a total says how different.
#
# A line reading `refused <reason>` is a glyph this library declines at that
# size, which is a fact worth committing rather than a gap to leave out.
#
# Only the fixtures carrying outlines of their own are here; the rest draw
# basic.ttf's five and the generator checks that they still do. See
# tools/golden/make_golden.py.
#
# documentation/design.md section 14.4.
"""


STRIKE_HEADER = """\
#
# And the bitmap containers, whose glyphs are pixels rather than paths. One line
# per glyph, in the same shape: the size column is the strike's own ppem, because
# a strike has exactly one, and the offsets are zero because a strike is not
# positioned sub-pixel. The coverage is what gfnt_coverage_from_bitmap() makes of
# the glyph, so these lines cover the bit and byte order arithmetic as well as the
# tables - which is the half of these formats a big-endian machine could break.
"""


def fixtures():
    """Every fixture MANIFEST lists, in its order."""
    manifest = os.path.join(FIXTURES, "MANIFEST")
    names = []
    with open(manifest, encoding="utf-8") as handle:
        for line in handle:
            if line.startswith("#") or not line.strip():
                continue
            names.append(line.split("\t")[0])
    return names


def render(name, label=None):
    """The driver's lines for one fixture, with the fixture as the label."""
    path = os.path.join(FIXTURES, name)
    finished = subprocess.run([DRIVER, label or name, path],
        capture_output=True, text=True)
    if finished.returncode != 0:
        raise SystemExit("font-render refused %s:\n%s"
                         % (name, finished.stderr))
    return finished.stdout


def render_strike(name):
    """The bitmap driver's lines for one strike fixture."""
    if not os.path.exists(BITMAP_DRIVER):
        raise SystemExit("%s is not built; run `make examples`" % BITMAP_DRIVER)
    path = os.path.join(FIXTURES, name)
    finished = subprocess.run([BITMAP_DRIVER, "--golden", name, path],
        capture_output=True, text=True)
    if finished.returncode != 0:
        raise SystemExit("font-bitmap refused %s:\n%s"
                         % (name, finished.stderr))
    return finished.stdout


def shapes(text):
    """The set of renderings in some output, with the fixture name removed.

    A rendering is its size, position, total and hash: what is left of a line
    once the label and the glyph index are dropped. Comparing *sets* rather than
    sequences is what makes this work across fixtures whose glyph orders differ -
    `post-v1.ttf` is the 258-name standard order, so its glyph 3 is `space` where
    `basic.ttf`'s is `B`. The claim being checked is not "the same glyph renders
    the same" but the weaker and sufficient "this fixture introduces no shape
    basic.ttf does not already have".
    """
    out = set()
    for line in text.splitlines():
        parts = line.split()
        if len(parts) < 6:
            continue
        # label glyph ppem offset-x offset-y ... -> drop the label and the glyph.
        out.add(" ".join(parts[2:]))
    return out


def probe_shared(committed):
    """No excluded fixture draws a rendering the committed set does not.

    The baseline is the **union** of every committed fixture's renderings, and was
    `basic.ttf`'s alone until two fixtures made that wrong: `bare.cff`'s font
    program is byte-for-byte `cff-curves.otf`'s, so it draws exactly the committed
    set's shapes under another name and is excluded for that reason - and against a
    baseline of `basic.ttf` alone it read as three hundred new ones. The generator
    had been failing on it, unnoticed, because nothing ran the generator.

    The union is also the honest statement of what the exclusion claims: not "this
    fixture draws basic.ttf's five outlines" but "this fixture introduces no
    rendering the committed file already covers".
    """
    reference = set()
    for text in committed.values():
        reference |= shapes(text)
    if not reference:
        raise SystemExit("nothing rendered at all; the probe has no baseline")
    probed = 0
    for name in fixtures():
        if name in WITH_OUTLINES:
            continue
        text = render(name)
        if name in NO_OUTLINES:
            # A fixture with no `glyf` must refuse every glyph. If it ever
            # started answering, this file would be missing a shape.
            answered = [line for line in text.splitlines()
                        if "refused" not in line]
            if answered:
                raise SystemExit(
                    "%s has no glyf but rendered %d glyph(s); the golden set "
                    "excludes it on the grounds that it cannot"
                    % (name, len(answered)))
            probed += 1
            continue
        extra = shapes(text) - reference
        if extra:
            raise SystemExit(
                "%s draws %d rendering(s) basic.ttf does not, so the golden set "
                "is missing a shape. Either add it to WITH_OUTLINES or find out "
                "what changed. First: %s"
                % (name, len(extra), sorted(extra)[0]))
        probed += 1
    return probed


def build():
    """The whole file, as text, and how many exclusions were probed."""
    if not os.path.exists(DRIVER):
        raise SystemExit("%s is not built; run `make examples`" % DRIVER)
    listed = fixtures()
    for name in WITH_OUTLINES + NO_OUTLINES + WITH_STRIKES:
        if name not in listed:
            raise SystemExit(
                "%s is named here but is not in MANIFEST; the fixture list and "
                "this one have drifted" % name)
    rendered = {name: render(name) for name in WITH_OUTLINES}
    probed = probe_shared(rendered)
    strikes = {name: render_strike(name) for name in WITH_STRIKES}
    out = [HEADER]
    for name in listed:
        if name in WITH_OUTLINES:
            out.append(rendered[name])
    out.append(STRIKE_HEADER)
    for name in listed:
        if name in WITH_STRIKES:
            out.append(strikes[name])
    return "".join(out), probed


def main(argv):
    text, probed = build()
    if "--stdout" in argv:
        sys.stdout.write(text)
        return 0
    os.makedirs(os.path.dirname(GOLDEN), exist_ok=True)
    with open(GOLDEN, "w", encoding="utf-8") as handle:
        handle.write(text)
    lines = sum(1 for line in text.splitlines() if not line.startswith("#"))
    print("golden: %d rendering(s) over %d fixture(s) with outlines of their own "
          "and %d strike(s), %d bytes; %d other fixture(s) checked to draw "
          "nothing new" % (lines, len(WITH_OUTLINES), len(WITH_STRIKES),
              len(text), probed))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
