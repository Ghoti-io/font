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
"""Every glyph's advance and the line metrics at a location, against two references.

documentation/design.md sections 7.7 and 14. `var_diff.py` asks where a variable
font's *points* go at a location; this asks how wide each glyph is there and how
tall a line, which `HVAR` and `MVAR` answer - or, in a font with no `HVAR`, the
phantom points `gvar` carries.

**The locations are `var_diff.py`'s own**, planned and seeded the same way, and
both references are handed **this library's** normalised coordinates, so that what
differs is the metric tables' arithmetic and not normalisation's rounding carried
into every glyph.

**Two references, and what each is for.**

  * fontTools reads every metric: the advance (its own `HVAR`-or-phantom-points
    rule), the left bearing (`hmtx` plus `HVAR`'s bearing mapping) and the line
    metrics (a base value plus `MVAR`'s delta). Where `HVAR` has no bearing
    mapping it says `none` and this library says it refuses, and the two are
    compared as the same statement.
  * FreeType reads the advance and the line metrics, from the code a different
    team wrote. It prints no bearing: its is the varied outline's `xMin` less the
    first phantom point, a box edge, and comparing that with a table's value
    would measure the difference between the two questions.

**What the real fonts can and cannot show, and the population is printed so
that nobody has to guess.** Every variable font in the image has `HVAR` with an
advance mapping, none has a bearing mapping, none lacks `HVAR`, and the only
`MVAR` (Inter's) carries no tag that moves a line - so the real fonts compare
advances through `HVAR` and show that the line metrics *do not* move, which is a
claim worth checking and is not the same as checking that they can. The other
three cases - an advance from phantom points, a bearing mapping, and `MVAR`
records that do move a line - are in the synthetic fixtures `variable-phantom.ttf`
and `variable-hvar.ttf`, which fontTools wrote, and each is named in the report.

Usage:
    metrics_var_diff.py [--stride N] [--fonts N] [--locations N] [--quiet] [font...]
"""

import os
import subprocess
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import corpus
import oracle_env
import unskippable
import var_diff

ROOT = oracle_env.ROOT
DRIVER = os.path.join(ROOT, "build", "linux", "release", "apps", "examples",
                      "font-metrics")
REFERENCE = os.path.join(ROOT, "tools", "oracle", "fonttools_metrics.py")

GLYPH_STRIDE = 7
PER_FONT = 6
TOTAL = 60

# How far a reference's whole-unit metric may be from this library's, by
# reference. Each is a measurement and the histogram says how many comparisons
# rested on it.
#
# fontTools: **none**. Both sum a delta at full precision and round once, and
# differ only in the direction of an exact half, which cannot occur in the
# arithmetic of a scalar built from 2.14 coordinates unless the delta is odd and
# the scalar exactly a half - the very case the unit tests pin. A unit of
# slack would hide the wrong direction.
#
# FreeType: rounds each region's contribution on its own and then sums, so a row
# of N regions can be off by up to N/2 whole units from a single rounding; the
# report prints the distribution of what it actually was.
ALLOWANCE = {"fontTools": 0, "FreeType": 1}

# **fontTools' side is its unrounded value, rounded this library's way** - see
# fonttools_metrics.half_away() - because the two differ on exactly the negative
# halves and nowhere else. A glyph a composite of which has a component flagged
# USE_MY_METRICS is a **known difference of FreeType's** that is counted under its
# own name: FreeType takes that glyph's advance from the component, and `HVAR`
# (and fontTools, and this library) from the glyph's own row. The count of
# glyph-locations it explains is printed.


class Skip(Exception):
    """This font cannot be compared, with a reason worth printing."""

    def __init__(self, why, expected=False):
        super().__init__(why)
        self.expected = expected


def parse(text):
    """One location's lines, as {'advance': {glyph: n}, 'bearing': ..., 'line': ...}.

    A value of None is a refusal (ours) or a statement that the font has no such
    thing (fontTools' `none`): they are one answer and are compared as one.
    """
    out = {"advance": {}, "bearing": {}, "line": {}, "variation": None,
           "usemymetrics": set(), "cvt": None}
    for line in text.splitlines():
        words = line.split()
        if line.startswith("variation: "):
            out["variation"] = [int(v) for v in line.split("normalised", 1)[1].split()]
        elif line.startswith("glyph ") and len(words) >= 3:
            glyph = int(words[1])
            kind = words[2]
            if kind == "usemymetrics":
                out["usemymetrics"].add(glyph)
                continue
            if kind not in ("advance", "bearing") or len(words) < 4:
                continue
            if words[3] in ("refused", "none"):
                out[kind][glyph] = None
            else:
                out[kind][glyph] = int(words[3])
        elif line.startswith("cvt ") and len(words) >= 2:
            out["cvt"] = None if words[1] == "refused" else \
                [int(v) for v in words[2:]]
            if words[1] == "refused":
                out["cvt"] = "refused"
        elif line.startswith("line ") and len(words) >= 3:
            if words[2] == "refused":
                out["line"][words[1]] = None
            else:
                out["line"][words[1]] = tuple(int(v) for v in words[2:])
    return out


def split_locations(stdout):
    blocks = []
    for line in stdout.splitlines():
        if line.startswith("location "):
            blocks.append([])
        elif blocks:
            blocks[-1].append(line)
    return ["\n".join(block) for block in blocks]


def ours(path, face, stride, location):
    finished = subprocess.run([DRIVER, path, str(face), str(stride), "0",
        location], capture_output=True, text=True)
    if finished.returncode != 0:
        raise Skip(finished.stderr.strip().splitlines()[-1]
                   if finished.stderr.strip() else "the driver refused it")
    return finished.stdout


def axes_of(path, face):
    """The axes and which metric tables the font has, from fontTools."""
    argv = oracle_env.command("fonttools", ["python3", REFERENCE, path,
        str(face), "1", "0"])
    finished = subprocess.run(argv, capture_output=True, text=True)
    if finished.returncode != 0:
        raise Skip("fontTools: %s" % (finished.stderr.strip().splitlines()[-1]
                   if finished.stderr.strip() else "refused"))
    if "metrics: no fvar" in finished.stdout:
        raise Skip("no fvar", expected=True)
    axes = []
    tables = ""
    for line in finished.stdout.splitlines():
        if line.startswith("axis: "):
            tag, low, default, high = line[6:].split()
            axes.append((tag, float(low), float(default), float(high)))
        elif line.startswith("metrics: has"):
            tables = line[len("metrics: "):]
    return axes, tables


def reference_fonttools(path, face, stride, specs):
    argv = oracle_env.command("fonttools", ["python3", REFERENCE, path,
        str(face), str(stride), "0"] + specs)
    finished = subprocess.run(argv, capture_output=True, text=True)
    if finished.returncode != 0:
        raise Skip("fontTools: %s" % (finished.stderr.strip().splitlines()[-1]
                   if finished.stderr.strip() else "refused"))
    return split_locations(finished.stdout)


def reference_freetype(path, face, stride, specs):
    argv = oracle_env.command("freetype", ["freetype-metrics", path,
        str(face), str(stride), "0"] + specs)
    finished = subprocess.run(argv, capture_output=True, text=True)
    if finished.returncode != 0:
        raise Skip("FreeType: %s" % (finished.stderr.strip().splitlines()[-1]
                   if finished.stderr.strip() else "refused"))
    return split_locations(finished.stdout)


def compare(path, face, stride, locations, report, histograms, references,
            population):
    """One font at every planned location. Returns (compared, differed, places)."""
    axes, tables = axes_of(path, face)
    if not axes:
        raise Skip("no axes", expected=True)
    seed = zlib.crc32(os.path.basename(path).encode("utf-8"))
    places = var_diff.plan(axes, seed, locations)
    mine = [ours(path, face, stride, place) for place in places]
    specs = []
    for place, text in zip(places, mine):
        coords = parse(text)["variation"]
        specs.append("%s@%s" % (place, ",".join(str(c) for c in coords)
                                 if coords else ""))
    blocks_by = {}
    if "fontTools" in references:
        blocks_by["fontTools"] = reference_fonttools(path, face, stride, specs)
    if "FreeType" in references:
        blocks_by["FreeType"] = reference_freetype(path, face, stride, specs)
    for name, blocks in blocks_by.items():
        if len(blocks) != len(places):
            raise Skip("%s answered %d of %d locations"
                       % (name, len(blocks), len(places)))

    compared = 0
    differed = 0
    shown = 0
    population.setdefault(tables, []).append(os.path.basename(path))

    def complain(message):
        nonlocal shown
        if shown < PER_FONT and report[0] < TOTAL:
            sys.stderr.write("  %s: %s\n" % (os.path.basename(path), message))
            shown += 1
            report[0] += 1

    def check(name, kind, label, want, got, place, explained=False):
        """One value from `name` against ours, counting the difference.

        `explained` is a known difference of that reference's, for this glyph
        only, and is counted rather than reported.
        """
        nonlocal compared, differed
        compared += 1
        if want is None or got is None:
            if want is None and got is None:
                return
            differed += 1
            complain("%s: %s: %s: %s says %r, this library %r"
                     % (place, kind, label, name, want, got))
            return
        if isinstance(want, tuple):
            difference = max(abs(a - b) for a, b in zip(want, got)) \
                if len(want) == len(got) else 1 << 30
        else:
            difference = abs(want - got)
        histogram = histograms.setdefault((name, kind), {})
        if difference > ALLOWANCE[name] and explained:
            histograms.setdefault((name, "explained"), {})[difference] = \
                histograms.get((name, "explained"), {}).get(difference, 0) + 1
            return
        if difference > ALLOWANCE[name]:
            differed += 1
            complain("%s: %s: %s: %s says %r, this library %r"
                     % (place, kind, label, name, want, got))
            return
        histogram[difference] = histogram.get(difference, 0) + 1

    for name, blocks in blocks_by.items():
        for place, text, block in zip(places, mine, blocks):
            theirs = parse(block)
            ours_ = parse(text)
            # Both lists come from fontTools: FreeType has no way to say it.
            marked = parse(blocks_by["fontTools"][places.index(place)])[
                "usemymetrics"] if "fontTools" in blocks_by else set()
            for glyph, want in theirs["advance"].items():
                check(name, "advance", "glyph %d" % glyph, want,
                      ours_["advance"].get(glyph, "missing"), place,
                      explained=name == "FreeType" and glyph in marked)
            if name == "fontTools":
                for glyph, want in theirs["bearing"].items():
                    check(name, "bearing", "glyph %d" % glyph, want,
                          ours_["bearing"].get(glyph, "missing"), place)
            for label, want in theirs["line"].items():
                check(name, "line", label, want, ours_["line"].get(label), place)
            if name == "fontTools" and theirs["cvt"] is not None:
                # One comparison per control value, so that the count says how
                # many values were held to the reference and not how many fonts.
                got = ours_["cvt"]
                if not isinstance(got, list) or len(got) != len(theirs["cvt"]):
                    check(name, "cvt", "values", 0, None, place)
                else:
                    for index, want in enumerate(theirs["cvt"]):
                        check(name, "cvt", "value %d" % index, want,
                              got[index], place)
    return compared, differed, len(places)


def main(argv):
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
        real = corpus.fonts("variable") + corpus.fonts("cff2")
        if limit is not None:
            step = max(1, len(real) // limit)
            real = real[::step][:limit]
        synthetic = len(var_diff.fixtures())
        fonts = var_diff.fixtures() + real
    if not fonts:
        sys.stderr.write("no fonts to compare\n")
        return 1

    compared = 0
    differed = 0
    places = 0
    skipped = []
    report = [0]
    histograms = {}
    population = {}
    synthetic_paths = set(var_diff.fixtures())
    for path in fonts:
        try:
            fields, bad, where = compare(path, 0,
                1 if path in synthetic_paths else stride, locations, report,
                histograms, references, population)
        except Skip as why:
            skipped.append((path, str(why), why.expected))
            continue
        compared += fields
        differed += bad
        places += where
        if not quiet:
            sys.stdout.write("  %-46s %3d locations, %8d comparisons%s\n"
                % (os.path.basename(path), where, fields,
                   ", disagreements: %d" % bad if bad else ""))

    print("metrics_var_diff: %d font(s) (%d synthetic fixtures at every glyph, "
          "%d real at every %dth), %d locations, %d comparisons, %d disagreements"
          % (len(fonts) - len(skipped), synthetic, len(fonts) - synthetic,
             stride, places, compared, differed))
    for tables, names in sorted(population.items()):
        print("metrics_var_diff: %d font(s) with %s" % (len(names), tables))
    for (name, kind), histogram in sorted(histograms.items()):
        print("metrics_var_diff: %s %s, by how far it differs from this "
              "library's in whole units: %s; %d allowed"
              % (name, kind, ", ".join("%d: %d" % (k, v) for k, v in
                                       sorted(histogram.items())) or "none",
                 ALLOWANCE[name]))
    for path, why, _ in skipped:
        print("metrics_var_diff: skipped %s: %s" % (os.path.basename(path), why))
    dropped = unskippable.check("metrics_var_diff",
        [(path, why) for path, why, expected in skipped if not expected])
    if dropped:
        return 1
    if not compared:
        sys.stderr.write("metrics_var_diff: nothing was compared\n")
        return 1
    return 1 if differed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
