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
"""Every glyph of every `EBLC`/`EBDT` strike, this library against fontTools.

documentation/design.md sections 7.5 and 14. `bitmap_diff.py` does this for the
standalone containers against Pillow; this does it for the strikes **inside an
sfnt**, where fontTools is the reference and reads the same two tables from the
same bytes.

What is compared, per glyph of per strike: its **state** - the strike carries it,
or does not - then its box, its two bearings, its advance, its depth, and every
pixel. And per strike, before any glyph: the ppem both ways, the depth, the
baseline, and the three glyph counts (present, absent, corrupt) which are over
**every** glyph whatever the stride. Those counts are the reason a strided run is
still worth something: a stride samples pixels, and a strike's glyph *set* is
what the whole index subtable walk decides, so a subtable read at the wrong
offset moves a count that a sample could step over.

**The population is two fonts, and that is the finding rather than a limitation
of this file.** Nothing on the host has these tables and nothing else in the
oracle image does either (`notes/font/EBLC.md` has the survey):

  * `uming.ttc`, four faces over **one** `EBLC`/`EBDT` byte range, six strikes of
    27,123 glyphs. Index formats 1 and 2 with image formats 5 and 7, and 97.8% of
    its index subtables are the one pairing `(2, 5)`. Both sides read it.
  * `mona.ttf`, three strikes of 7,225 glyphs in image format 6 - the only
    format 6 in Debian - which **fontTools cannot read at all**: its `EBLC` is
    four bytes short of the last entry of its last offset array
    (fonttools#317), and fontTools slices by the stated `indexTablesSize`. This
    library reads 7,224 of the 7,225 glyphs of each strike and refuses the last
    one, per glyph (M11). There is no second reading of those pixels, and
    `EXPECTED_DECLINED` below turns that into an assertion rather than a silent
    skip: if a future fontTools reads it, this gate fails and says to move the
    font to the both-sides list.

So the fixtures are not a supplement here, they are most of the evidence: index
formats 3, 4 and 5 and image formats 1 and 2 appear **nowhere** in the
population, and image format 6 appears only in the font the reference declines.
`strike-formats.ttf` draws one design in six pairings and fontTools reads every
one of them, which is what makes a fixture a measurement rather than a
restatement of what this library did. The report prints the census of pairings
each run actually visited, because a total that does not say which cells it
covered reads the same whatever it covered.

Usage:
    eblc_diff.py [--stride N] [--quiet] [font...]
"""

import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import corpus
import oracle_env

ROOT = oracle_env.ROOT
DRIVER = os.path.join(ROOT, "build", "linux", "release", "apps", "examples",
                      "font-bitmap")
REFERENCE = os.path.join(ROOT, "tools", "oracle", "fonttools_strikes.py")
FIXTURES = os.path.join(ROOT, "tests", "data", "fonts")

# Every glyph, by default. Unlike `glyf_diff` and `cff_diff` this corpus is two
# files, so there is nothing to thin: the exhaustive run is 1.8 million compared
# facts per face and the reference takes eleven seconds over it. `--stride` is
# here for a developer looking at one font, not because the gate needs it.
GLYPH_STRIDE = 1

# How many disagreements to print before the rest are counted and not shown.
TOTAL = 60

# Fonts this reference cannot read, and the error it must fail with.
#
# A named expectation rather than a skip, because the two are indistinguishable
# in a clean report and they mean opposite things: a font that quietly stopped
# being compared reads as agreement, and a font the reference *starts* reading is
# a second opinion this differential should be taking and is not. Either change
# fails this gate with the sentence that says what to do.
EXPECTED_DECLINED = {
    "mona.ttf": "unpack requires a buffer of 4 bytes",
}


class Skip(Exception):
    """This font cannot be compared, with a reason worth printing."""

    def __init__(self, why, expected=False):
        super().__init__(why)
        self.expected = expected


def strike_fonts(roots):
    """Which of @p roots' files carry both `EBLC` and `EBDT`.

    Asked of the image's own `fonttools-corpus-ebdt`, which reads a table
    directory with `struct` and opens nothing. One implementation of the
    predicate, used for the corpus and for the fixtures both - and deliberately
    neither reader's opinion: selecting a population with the reader under test
    would let a reader that lost `EBLC` support compare nothing and pass, and
    selecting it with the reference would drop `mona.ttf`, which is the one font
    the two disagree about being readable at all.

    It is also why the fixtures are not a hand-written list here: a new strike
    fixture joins this differential by existing, and one that stops carrying the
    tables leaves it loudly, in the count.
    """
    argv = oracle_env.command("fonttools", ["fonttools-corpus-ebdt"] + roots)
    finished = subprocess.run(argv, capture_output=True, text=True)
    if finished.returncode != 0:
        raise oracle_env.OracleUnavailable(
            "could not list the strike fonts under %s:\n%s"
            % (", ".join(roots), finished.stderr.strip()))
    return sorted(line for line in finished.stdout.split("\n") if line)


def parse(text):
    """The line protocol both sides print, as a structure rather than a dict.

    Compared per glyph rather than as one flat mapping, because a glyph whose
    state the two sides disagree about has no box and no rows on one side, and a
    flat comparison would score that once as a state and then again as every row
    the other side printed. One disagreement is one finding.
    """
    scalars = {}
    strikes = {}
    glyphs = {}
    census = []
    for line in text.split("\n"):
        if not line:
            continue
        tokens = line.split(" ")
        if tokens[0] == "census":
            census.append(line[len("census "):])
            continue
        if tokens[0] == "g":
            key = (int(tokens[1]), int(tokens[2]))
            field = tokens[3]
            record = glyphs.setdefault(key, {"rows": {}})
            if field == "row":
                record["rows"][int(tokens[4])] = " ".join(tokens[5:])
            else:
                record[field] = " ".join(tokens[4:])
            continue
        if tokens[0] == "strike":
            strikes.setdefault(int(tokens[1]), {})[tokens[2]] = \
                " ".join(tokens[3:])
            continue
        if len(tokens) == 2:
            scalars[tokens[0]] = tokens[1]
            continue
        raise ValueError("a line neither side's protocol has: %r" % line)
    return scalars, strikes, glyphs, census


def ours(path, face, stride):
    """This library's answer, through `font-bitmap --strikes`."""
    finished = subprocess.run(
        [DRIVER, "--strikes", path, str(face), str(stride)],
        capture_output=True, text=True)
    if finished.returncode != 0:
        raise Skip("this library: %s"
                   % (finished.stderr.strip().splitlines()[-1]
                      if finished.stderr.strip() else "refused"))
    return parse(finished.stdout)


def theirs(path, face, stride):
    """fontTools' answer, through the adapter, inside the pinned image."""
    argv = oracle_env.command("fonttools",
        ["python3", REFERENCE, path, str(face), str(stride)])
    finished = subprocess.run(argv, capture_output=True, text=True)
    if finished.returncode != 0:
        last = (finished.stderr.strip().splitlines()[-1]
                if finished.stderr.strip() else "refused")
        raise Skip("fontTools: %s" % last,
                   expected=os.path.basename(path) in EXPECTED_DECLINED)
    return parse(finished.stdout)


def alone(path, face, stride):
    """What this library read of a face nothing will compare it against.

    One line, and only the strike-level facts: the three glyph counts are the
    whole of what can be said about a font whose pixels no second reader will
    look at, and saying it is better than a report where that font appears as the
    word "skipped" and nothing else.
    """
    try:
        _, strikes, _, _ = ours(path, face, stride)
    except Skip as why:
        return "this library declined it too: %s" % why
    return "; ".join(
        "%s ppem %s present %s corrupt %s"
        % (number, facts.get("ppem_y"), facts.get("present"),
           facts.get("corrupt"))
        for number, facts in sorted(strikes.items())) or "no strikes"


def compare(path, face, stride, counts, report):
    """One face. Returns (fields, disagreements, glyphs, census)."""
    my_scalars, my_strikes, my_glyphs, _ = ours(path, face, stride)
    # The reference is asked second, so that a font this library cannot read at
    # all is reported as ours rather than as a reference failure.
    their_scalars, their_strikes, their_glyphs, census = theirs(path, face,
        stride)

    label = "%s:%d" % (os.path.basename(path), face)
    fields = 0
    differed = 0

    def says(key, mine, theirs_value):
        nonlocal fields, differed
        fields += 1
        if mine == theirs_value:
            return True
        differed += 1
        if report[0] < TOTAL:
            report[0] += 1
            print("    %s %s: ours %r, fontTools %r"
                  % (label, key, mine, theirs_value))
        return False

    for key in sorted(set(my_scalars) | set(their_scalars)):
        says(key, my_scalars.get(key), their_scalars.get(key))
    for number in sorted(set(my_strikes) | set(their_strikes)):
        mine = my_strikes.get(number, {})
        reference = their_strikes.get(number, {})
        for key in sorted(set(mine) | set(reference)):
            says("strike %d %s" % (number, key), mine.get(key),
                 reference.get(key))

    for key in sorted(set(my_glyphs) | set(their_glyphs)):
        mine = my_glyphs.get(key, {})
        reference = their_glyphs.get(key, {})
        where = "g %d strike %d" % key
        my_state = mine.get("state")
        their_state = reference.get("state")
        if not says(where + " state", my_state, their_state):
            # The box and the rows of a glyph the two sides disagree about the
            # existence of are a consequence of that one disagreement, not
            # further findings. Counted, so the report can say how many facts the
            # state disagreement cost.
            counts["state-consequent"] += max(len(mine.get("rows", {})),
                                              len(reference.get("rows", {})))
            continue
        if my_state != "present":
            counts["absent-both"] += 1
            continue
        says(where + " box", mine.get("box"), reference.get("box"))
        my_rows = mine.get("rows", {})
        their_rows = reference.get("rows", {})
        for row in sorted(set(my_rows) | set(their_rows)):
            says("%s row %d" % (where, row), my_rows.get(row),
                 their_rows.get(row))

    return fields, differed, len(set(my_glyphs) | set(their_glyphs)), census


def main(argv):
    quiet = "--quiet" in argv
    stride = GLYPH_STRIDE
    rest = []
    skip_next = False
    for index, argument in enumerate(argv[1:], start=1):
        if skip_next:
            skip_next = False
            continue
        if argument in ("--stride", "--glyphs"):
            stride = int(argv[index + 1])
            skip_next = True
        elif argument == "--quiet":
            continue
        else:
            rest.append(argument)

    if not os.path.exists(DRIVER):
        sys.stderr.write("%s is not built; run `make examples`\n" % DRIVER)
        return 1

    print(oracle_env.provenance(["fonttools"]))
    if rest:
        fonts = rest
        synthetic = set()
    else:
        synthetic = set(strike_fonts([FIXTURES]))
        fonts = sorted(synthetic) + corpus.fonts("ebdt")
    if not fonts:
        sys.stderr.write("no fonts with strikes to compare\n")
        return 1

    compared = 0
    disagreements = 0
    glyphs = 0
    faces = 0
    skipped = []
    report = [0]
    census = {}
    extent_sets = set()
    counts = {"state-consequent": 0, "absent-both": 0}

    for path in fonts:
        # The face count is this library's, which is the one the loop has to
        # agree with: if the two sides disagreed about how many faces a
        # collection has, `faces` is a compared field inside every face and says
        # so.
        count = 1
        try:
            count = int(ours(path, 0, stride)[0].get("faces", 1))
        except Skip:
            pass
        for face in range(count):
            try:
                fields, differed, seen, lines = compare(path, face, stride,
                    counts, report)
            except Skip as why:
                skipped.append(("%s:%d" % (os.path.basename(path), face),
                                str(why), why.expected, alone(path, face,
                                    stride)))
                continue
            faces += 1
            compared += fields
            disagreements += differed
            glyphs += seen
            for line in lines:
                census[line] = census.get(line, 0) + 1
            # Per **face**, as a set: the question is how many distinct tables
            # those faces read between them, and a flat set of extents counts
            # each face's EBLC and EBDT separately - which answered "6 extents
            # behind 6 faces" for a run where four of the faces shared one pair,
            # the exact sharing the number exists to show.
            extent_sets.add(frozenset(line for line in lines
                                      if line.startswith("table ")))
            if not quiet:
                sys.stdout.write("  %-44s %6d glyphs, %8d fields%s\n"
                    % ("%s:%d" % (os.path.basename(path), face), seen, fields,
                       ", disagreements: %d" % differed if differed else ""))

    # What the reference said it was reading, aggregated. Printed rather than
    # compared: this library has no public accessor for an index format or an
    # image format, so these are the only statement of which cells of the grid a
    # run visited - and "0 disagreements" over a grid nobody named is a number
    # that reads the same however little it covered.
    pairs = {}
    flags = {}
    depths = {}
    composites = 0
    components = 0
    nested = 0
    for line, times in sorted(census.items()):
        tokens = line.split(" ")
        if tokens[0] == "pair":
            key = (int(tokens[1]), int(tokens[2]))
            pairs[key] = pairs.get(key, 0) + int(tokens[4]) * times
        elif tokens[0] == "sizetable":
            flags[int(tokens[3])] = flags.get(int(tokens[3]), 0) + times
            depths[int(tokens[5])] = depths.get(int(tokens[5]), 0) + times
        elif tokens[0] == "composite":
            composites += int(tokens[3]) * times
            components += int(tokens[5]) * times
            nested += int(tokens[7]) * times

    print("eblc_diff: %d face(s) of %d font(s), %d glyph(s), %d field(s) "
          "compared, %d disagreement(s)"
          % (faces, len(fonts), glyphs, compared, disagreements))
    print("eblc_diff: index/image format pairings this run visited: %s"
          % (", ".join("(%d,%d) x%d" % (index, image, count)
                       for (index, image), count in sorted(pairs.items()))
             or "none"))
    print("eblc_diff: strike flags seen: %s; bit depths seen: %s - a strike "
          "whose metrics are the vertical set, and any depth but 1, are "
          "fixture-only or absent, and the unit suite is their cover"
          % (", ".join("%d x%d" % pair for pair in sorted(flags.items()))
             or "none",
             ", ".join("%d x%d" % pair for pair in sorted(depths.items()))
             or "none"))
    print("eblc_diff: %d composite glyph(s) of %d component(s), %d of them "
          "drawing another composite - image formats 8 and 9, which appear in no "
          "Debian font at all. fontTools reads a component list and does not "
          "compose it, so the placement arithmetic is this repository's on both "
          "sides here: what checks the rule itself is strike-composite.ttf, where "
          "each composite has a non-composite twin drawn by the generator's own "
          "arithmetic" % (composites, components, nested))
    print("eblc_diff: %d face(s) over %d distinct table extent(s) - a "
          "collection whose faces share one EBLC and one EBDT, as uming.ttc's "
          "four do, re-checks the collection path rather than the format, and "
          "the two numbers differing is how this run says so"
          % (faces, len(extent_sets)))
    print("eblc_diff: %d glyph(s) both sides agree this strike does not carry, "
          "and %d row(s) left uncompared behind a state disagreement"
          % (counts["absent-both"], counts["state-consequent"]))
    print("eblc_diff: %d font(s) are the whole Debian population of this table; "
          "index formats 3, 4 and 5 and image formats 1 and 2 appear in none of "
          "them, so what covers those is strike-formats.ttf and nothing else"
          % len(corpus.fonts("ebdt")))
    for path, why, expected, mine in skipped:
        print("  %-44s skipped: %s%s" % (path, why,
            "" if expected else "  <- not expected"))
        # What this library read of a font nothing compared it on. Printed
        # because "skipped" and "read by one reader and nobody else" are
        # different facts, and the second one is a claim this repository is
        # making with no second opinion behind it - the one thing a clean
        # differential must not leave looking checked.
        if mine:
            print("  %-44s  ours alone: %s" % ("", mine))

    # The expected declines, checked both ways. A font that stays unreadable to
    # the reference is a number in the report; one that becomes readable is work
    # to do, and one that was never looked at is the silent case this whole block
    # exists to make impossible.
    failed = False
    for name, expected_error in sorted(EXPECTED_DECLINED.items()):
        matching = [why for path, why, _, _ in skipped
                    if path.split(":")[0] == name]
        if not matching:
            if any(os.path.basename(path) == name for path in fonts):
                print("eblc_diff: %s was NOT declined by fontTools. Its EBLC is "
                      "readable now, so it belongs in the both-sides population: "
                      "drop it from EXPECTED_DECLINED and let it be compared."
                      % name)
                failed = True
            continue
        for why in matching:
            if expected_error not in why:
                print("eblc_diff: %s was declined for a different reason than "
                      "the one recorded: %r does not contain %r"
                      % (name, why, expected_error))
                failed = True

    # A run that compared nothing is the failure this whole file exists to
    # avoid, and it is reachable without anybody editing this file: an image
    # rebuilt without the two font packages still answers its version probe, so
    # the corpus would come back empty and every number above would be zero. A
    # clean report of nothing reads exactly like a clean report of everything.
    if not faces:
        print("eblc_diff: no face was compared at all. The population is empty "
              "or unreadable - check that the image carries fonts-arphic-uming "
              "and fonts-mona (`make oracle-build`).")
        failed = True
    elif not any(path in corpus.fonts("ebdt") for path in fonts):
        print("eblc_diff: every face compared was a fixture. The real "
              "population did not reach this run, so nothing here is a "
              "statement about the format as it ships.")
        failed = True

    if disagreements or failed:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
