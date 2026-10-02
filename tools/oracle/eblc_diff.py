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
"""Every glyph of every `EBLC`/`EBDT` strike, against fontTools and FreeType both.

documentation/design.md sections 7.5 and 14. `bitmap_diff.py` does this for the
standalone containers against Pillow; this does it for the strikes **inside an
sfnt**, against two references that read the same two tables from the same bytes.

**Two references, because each one alone has a hole the other covers.** fontTools
parses every field and composes no image, so a composite's pixels were this
repository's arithmetic on both sides of the comparison; and it cannot parse
`mona.ttf` at all. FreeType composes the image and reads `mona.ttf`, and in
exchange can report neither a strike's bit depth nor - for any face that also has
outlines - a strike's baseline, because `tt_size_select` prefers the scaled
outline's. Each reference prints `census unanswered <key>` for what it cannot
answer and the comparison skips exactly that; a reference that stops disclaiming
a key is compared on it from that run onwards, which is the direction the default
has to fail in.

The third voice is what settled the one defect this gate has found. Konatu.ttf
has 13,249 zero-length glyphs in each of its fourteen strikes, and this library
read a zero-length entry in an offset-array index format as a glyph that is
present with no pixels. fontTools said 2,323 carried where this library said
15,570; FreeType said 2,323 too, and the specification says the difference
between consecutive offsets is the data size. One reference disagreeing is a
question, and two agreeing is an answer.

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
ADAPTER = os.path.join(ROOT, "tools", "oracle", "fonttools_strikes.py")
FIXTURES = os.path.join(ROOT, "tests", "data", "fonts")

# How to ask each reference, in the order their answers are reported.
#
# fontTools runs an adapter from this directory because it is a library; FreeType
# runs a program compiled into its own image because it is a C library and a
# driver that linked it on the host would link whatever the host has. Neither
# invocation names a font path that the other does not see: the corpus is
# materialised under `build/oracle/corpus`, inside the tree both images mount.
REFERENCES = {
    "fonttools": lambda path, face, stride: ["python3", ADAPTER, path,
                                            str(face), str(stride)],
    "freetype": lambda path, face, stride: ["freetype-strikes", path,
                                            str(face), str(stride)],
}

# Every glyph, by default. Unlike `glyf_diff` and `cff_diff` this corpus is two
# files, so there is nothing to thin: the exhaustive run is 1.8 million compared
# facts per face and the reference takes eleven seconds over it. `--stride` is
# here for a developer looking at one font, not because the gate needs it.
GLYPH_STRIDE = 1

# How many disagreements to print before the rest are counted and not shown.
TOTAL = 60

# The `box` line's fields, in the order both sides print them.
BOX_FIELDS = ("width", "height", "bearing_x", "bearing_y", "advance", "depth")

# Fonts this reference cannot read, and the error it must fail with.
#
# A named expectation rather than a skip, because the two are indistinguishable
# in a clean report and they mean opposite things: a font that quietly stopped
# being compared reads as agreement, and a font the reference *starts* reading is
# a second opinion this differential should be taking and is not. Either change
# fails this gate with the sentence that says what to do.
EXPECTED_DECLINED = {
    "fonttools": {
        "mona.ttf": "unpack requires a buffer of 4 bytes",
    },
    # FreeType declines nothing here, and the empty mapping is the assertion:
    # every font in the population must reach it. `mona.ttf` is why this image
    # exists, so a FreeType that stopped reading it would be the one failure this
    # gate must not report as a skip.
    "freetype": {},
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
    # What this reference says it cannot answer, and whether its per-glyph state
    # is only "carried or not". Taken from the output rather than from a table
    # here, so that a reference which grows an accessor is compared on it without
    # anybody remembering to delete an exemption.
    unanswered = set()
    coarse = set()
    fills = set()
    for line in text.split("\n"):
        if not line:
            continue
        tokens = line.split(" ")
        if tokens[0] == "census":
            rest = line[len("census "):]
            census.append(rest)
            if tokens[1] == "unanswered":
                unanswered.add(" ".join(tokens[2:]))
            elif tokens[1] == "coarse":
                coarse.add(" ".join(tokens[2:]))
            elif tokens[1] == "fills":
                fills.add(" ".join(tokens[2:]))
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
    return Answer(scalars, strikes, glyphs, census, unanswered, coarse, fills)


class Answer:
    """One reader's whole answer for one face."""

    def __init__(self, scalars, strikes, glyphs, census, unanswered, coarse,
                 fills):
        self.scalars = scalars
        self.strikes = strikes
        self.glyphs = glyphs
        self.census = census
        # What the reference says it cannot answer, what it answers only coarsely,
        # and what it answers with a value of its own rather than the table's.
        self.unanswered = unanswered
        self.coarse = coarse
        self.fills = fills


def emptied(record):
    """A glyph reduced to "produces pixels or does not", for a reference that
    cannot tell an absent glyph from an empty one.

    FreeType cannot, on a face it judges bitmap-only: `ttgload.c` turns a glyph no
    index subtable covers into a successful 0x0 bitmap carrying the `hmtx` advance,
    on the stated ground that a missing glyph in a bitmap-only font is whitespace.
    So for such a face `present` with a 0x0 box and `uncarried` are one state, and
    both sides are reduced the same way before being compared. Everything else
    still separates: a glyph this library calls absent and the reference draws
    pixels for is still a disagreement, and so is the reverse.

    The distinction is not lost from the gate, only from this arm of it - fontTools
    reads the table's own answer and compares it on every one of these faces. The
    same goes for the one other thing this costs: a glyph both sides call 0x0 is
    not compared further, so a genuinely empty glyph's *advance* goes uncompared
    against this reference on a bitmap-only face.
    """
    state = record.get("state")
    if state != "present":
        return "no-pixels"
    box = (record.get("box") or "").split(" ")
    if len(box) >= 2 and box[0] == "0" and box[1] == "0":
        return "no-pixels"
    return "present"


def carried(state):
    """A per-glyph state reduced to what every reference can decide.

    FreeType has `Invalid_Argument` for a glyph no subtable covers and
    `Invalid_File_Format` for data that does not parse, so the absent/corrupt
    split looked available - and over the one font in the population with a broken
    glyph it is not: mona.ttf's last glyph comes back from FreeType under the same
    code as a glyph nothing covers. So against a reference that says
    `census coarse state`, what is compared is whether the glyph produces pixels,
    and the M11 classification is this library's alone. Which it was already: the
    point of coarsening here is that the report says so instead of scoring a
    wording difference 21,672 times.
    """
    return "present" if state == "present" else "uncarried"


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


def theirs(name, path, face, stride):
    """One reference's answer, inside its own pinned image."""
    argv = oracle_env.command(name, REFERENCES[name](path, face, stride))
    finished = subprocess.run(argv, capture_output=True, text=True)
    if finished.returncode != 0:
        last = (finished.stderr.strip().splitlines()[-1]
                if finished.stderr.strip() else "refused")
        raise Skip("%s: %s" % (name, last),
                   expected=os.path.basename(path)
                       in EXPECTED_DECLINED[name])
    return parse(finished.stdout)


def alone(path, face, stride):
    """What this library read of a face nothing will compare it against.

    One line, and only the strike-level facts: the three glyph counts are the
    whole of what can be said about a face whose pixels *this* reference will not
    look at, and saying it is better than a report where the face appears as the
    word "skipped" and nothing else. With two references the line usually means
    less than it did - mona.ttf is declined by fontTools and read in full by
    FreeType - but a face declined by both is exactly what it still has to catch.
    """
    try:
        strikes = ours(path, face, stride).strikes
    except Skip as why:
        return "this library declined it too: %s" % why
    return "; ".join(
        "%s ppem %s present %s corrupt %s"
        % (number, facts.get("ppem_y"), facts.get("present"),
           facts.get("corrupt"))
        for number, facts in sorted(strikes.items())) or "no strikes"


def compare(name, path, face, stride, mine_answer, counts, report, answered):
    """One face against one reference. Returns (fields, disagreements, glyphs, census)."""
    my_scalars = mine_answer.scalars
    my_strikes = mine_answer.strikes
    my_glyphs = mine_answer.glyphs
    # The reference is asked second, so that a font this library cannot read at
    # all is reported as ours rather than as a reference failure.
    answer = theirs(name, path, face, stride)
    their_scalars, their_strikes, their_glyphs = (answer.scalars,
        answer.strikes, answer.glyphs)
    census = answer.census
    unanswered = answer.unanswered
    coarsen = carried if "state" in answer.coarse else (lambda state: state)
    # On a bitmap-only face this reference reports absence as an empty glyph, so
    # the two states are merged for that face - see `emptied()`.
    merge_empty = "empty-vs-absent" in answer.coarse
    # Whether this reference substitutes an outline's advance for a stated zero.
    # FreeType does, in `ttgload.c`, and says so; see `BOX_FIELDS` below.
    fills_zero_advance = "zero-advance" in answer.fills

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
            print("    %s %s: ours %r, %s %r"
                  % (label, key, mine, name, theirs_value))
        return False

    for key in sorted(set(my_scalars) | set(their_scalars)):
        says(key, my_scalars.get(key), their_scalars.get(key))
    for number in sorted(set(my_strikes) | set(their_strikes)):
        mine = my_strikes.get(number, {})
        reference = their_strikes.get(number, {})
        for key in sorted(set(mine) | set(reference)):
            # Skipped only where the reference itself said it cannot answer.
            if ("strike %s" % key) in unanswered:
                counts["unanswered"] += 1
                continue
            # Which strike-level keys this reference actually answered, so the
            # guard below can say when one of them stopped being answered at all.
            answered.add("%s %s" % (name, key))
            says("strike %d %s" % (number, key), mine.get(key),
                 reference.get(key))

    for key in sorted(set(my_glyphs) | set(their_glyphs)):
        mine = my_glyphs.get(key, {})
        reference = their_glyphs.get(key, {})
        where = "g %d strike %d" % key
        if merge_empty:
            my_state = emptied(mine)
            their_state = emptied(reference)
        else:
            my_state = coarsen(mine.get("state"))
            their_state = coarsen(reference.get("state"))
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
        # Field by field rather than as one string, so a disagreement names the
        # advance or the bearing instead of making the reader diff six numbers -
        # and so that the one exemption below can be about one field.
        my_box = (mine.get("box") or "").split(" ")
        their_box = (reference.get("box") or "").split(" ")
        for at, field in enumerate(BOX_FIELDS):
            was = my_box[at] if at < len(my_box) else None
            is_ = their_box[at] if at < len(their_box) else None
            if (field == "advance" and fills_zero_advance
                    and was == "0" and is_ not in (None, "0")):
                # The reference filled a stated zero from `hmtx`. Allowed in
                # exactly this direction and counted, never the other way: a
                # reference reporting zero where this library reports a width is
                # a disagreement, and so is any other advance difference. What
                # keeps this from hiding a wrong zero of ours is the other
                # reference, which reads the table's own value and compares it.
                counts["zero-advance-filled"] += 1
                continue
            says("%s %s" % (where, field), was, is_)
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

    print(oracle_env.provenance(sorted(REFERENCES)))
    if rest:
        fonts = rest
        synthetic = set()
    else:
        synthetic = set(strike_fonts([FIXTURES]))
        fonts = sorted(synthetic) + corpus.fonts("ebdt")
    if not fonts:
        sys.stderr.write("no fonts with strikes to compare\n")
        return 1

    compared = dict.fromkeys(REFERENCES, 0)
    disagreements = dict.fromkeys(REFERENCES, 0)
    faces = dict.fromkeys(REFERENCES, 0)
    glyphs = 0
    skipped = []
    report = [0]
    census = {}
    extent_sets = set()
    counts = {"state-consequent": 0, "absent-both": 0, "unanswered": 0,
              "zero-advance-filled": 0}
    answered = set()

    for path in fonts:
        # The face count is this library's, which is the one the loop has to
        # agree with: if a reference disagreed about how many faces a collection
        # has, `faces` is a compared field inside every face and says so.
        count = 1
        try:
            count = int(ours(path, 0, stride).scalars.get("faces", 1))
        except Skip:
            pass
        for face in range(count):
            label = "%s:%d" % (os.path.basename(path), face)
            # Asked once and compared against every reference, so that a
            # disagreement between two references over the same face is visible
            # as this library agreeing with one of them and not the other.
            try:
                mine = ours(path, face, stride)
            except Skip as why:
                skipped.append((label, str(why), False, ""))
                continue
            seen = 0
            parts = []
            declined = []
            for name in sorted(REFERENCES):
                try:
                    fields, differed, seen, lines = compare(name, path, face,
                        stride, mine, counts, report, answered)
                except Skip as why:
                    declined.append((label, str(why), why.expected))
                    continue
                faces[name] += 1
                compared[name] += fields
                disagreements[name] += differed
                parts.append("%s %d field(s)%s" % (name, fields,
                    ", %d disagreement(s)" % differed if differed else ""))
                if name != "fonttools":
                    continue
                # The census and the table extents are fontTools' alone: it is the
                # reference with an opinion about index and image formats, and
                # FreeType exposes neither.
                for line in lines:
                    census[line] = census.get(line, 0) + 1
                # Per **face**, as a set: the question is how many distinct tables
                # those faces read between them, and a flat set of extents counts
                # each face's EBLC and EBDT separately - which answered "6 extents
                # behind 6 faces" for a run where four of the faces shared one
                # pair, the exact sharing the number exists to show.
                extent_sets.add(frozenset(line for line in lines
                                          if line.startswith("table ")))
            # The "ours alone" line belongs to a face **no** reference looked at.
            # With one reference a decline and an unread face were the same thing;
            # with two they are not, and mona.ttf is the case that separates them -
            # fontTools declines it and FreeType reads all three strikes, so
            # printing "this library read it and nobody checked" beside it would
            # now be false. It is still printed for a face both decline, which is
            # the thing that line exists to make impossible to miss.
            unread = not parts
            for label_, why, expected in declined:
                skipped.append((label_, why, expected,
                                alone(path, face, stride) if unread else ""))
            glyphs += seen
            if not quiet and parts:
                sys.stdout.write("  %-44s %6d glyphs, %s\n"
                    % (label, seen, "; ".join(parts)))

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

    for name in sorted(REFERENCES):
        print("eblc_diff: vs %s: %d face(s) of %d font(s), %d field(s) "
              "compared, %d disagreement(s)"
              % (name, faces[name], len(fonts), compared[name],
                 disagreements[name]))
    print("eblc_diff: %d advance(s) a reference filled in from `hmtx` because the "
          "table states zero - FreeType does this for a renderer's benefit in "
          "ttgload.c and says so, and fontTools reading the stated zero is what "
          "keeps the exemption from hiding one of ours"
          % counts["zero-advance-filled"])
    print("eblc_diff: %d glyph-strike pair(s) seen; %d field(s) left uncompared "
          "because the reference printed `census unanswered` for them - a bit "
          "depth no FreeType accessor reports, and a strike baseline it reports "
          "only for a face with no outlines"
          % (glyphs, counts["unanswered"]))
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
          "Debian font at all, so every one of them is a fixture. fontTools reads "
          "a component list without composing it; **FreeType composes**, and its "
          "`ttsbit.c` is where this library's placement rule came from, so a "
          "composite's pixels now have a second reader rather than this "
          "repository's arithmetic on both sides"
          % (composites, components, nested))
    print("eblc_diff: %d face(s) over %d distinct table extent(s) - a "
          "collection whose faces share one EBLC and one EBDT, as uming.ttc's "
          "four do, re-checks the collection path rather than the format, and "
          "the two numbers differing is how this run says so"
          % (faces["fonttools"], len(extent_sets)))
    print("eblc_diff: %d glyph(s) both sides agree this strike does not carry, "
          "and %d row(s) left uncompared behind a state disagreement"
          % (counts["absent-both"], counts["state-consequent"]))
    print("eblc_diff: %d font(s) are the whole Debian population of this table - "
          "589 font packages scanned, 5,927 distinct sfnt files, and these are "
          "the ones that carry it (notes/font/EBLC.md). Index formats **4 and 5** "
          "are what no font here uses, and strike-formats.ttf is their only "
          "cover; index format 3 and image formats 1 and 2 are covered by real "
          "fonts, which an earlier survey of a guessed shortlist reported as "
          "appearing nowhere" % len(corpus.fonts("ebdt")))
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
    for reference in sorted(REFERENCES):
        expected = EXPECTED_DECLINED[reference]
        # Every decline by this reference must be one that is written down. An
        # unexpected decline is the silent case: a font that stopped being
        # compared reads exactly like a font that agreed.
        for label, why, _, _ in skipped:
            if not why.startswith(reference + ": "):
                continue
            font = label.split(":")[0]
            if font not in expected:
                print("eblc_diff: %s declined %s and nothing says it should: %s"
                      % (reference, font, why))
                failed = True
        for font, expected_error in sorted(expected.items()):
            matching = [why for label, why, _, _ in skipped
                        if label.split(":")[0] == font
                        and why.startswith(reference + ": ")]
            if not matching:
                if any(os.path.basename(path) == font for path in fonts):
                    print("eblc_diff: %s was NOT declined by %s. It is readable "
                          "now, so it belongs in that reference's compared "
                          "population: drop it from EXPECTED_DECLINED."
                          % (font, reference))
                    failed = True
                continue
            for why in matching:
                if expected_error not in why:
                    print("eblc_diff: %s was declined by %s for a different "
                          "reason than the one recorded: %r does not contain %r"
                          % (font, reference, why, expected_error))
                    failed = True

    # A run that compared nothing is the failure this whole file exists to
    # avoid, and it is reachable without anybody editing this file: an image
    # rebuilt without the two font packages still answers its version probe, so
    # the corpus would come back empty and every number above would be zero. A
    # clean report of nothing reads exactly like a clean report of everything.
    for name in sorted(REFERENCES):
        if not faces[name]:
            print("eblc_diff: %s compared no face at all. Either the population "
                  "is empty - check that the image carries the font packages "
                  "(`make oracle-build`) - or that reference declined every font "
                  "in it, which a clean report cannot be allowed to mean." % name)
            failed = True
    # A strike-level key that no face answered is a comparison that silently
    # stopped happening. The one at risk is FreeType's baseline: it answers
    # `ascent` and `descent` only for a face it judges bitmap-only, so that whole
    # arm rests on which fonts the corpus holds - and the `.otb` files were
    # invisible to the corpus list until it stopped selecting by file extension.
    # Drop them and every other number in this report stays exactly as clean.
    #
    # "Bitmap-only" is FreeType's judgement rather than "has no `glyf`": Wine's
    # fonts have one and are bitmap-only by that test, so they answer the baseline
    # too - which is why removing just the `.otb` files does not trip this, and
    # removing every bitmap-only face does. That is how it was checked.
    if not rest:
        for key in ("fonttools ascent", "fonttools descent", "fonttools depth",
                    "freetype ascent", "freetype descent"):
            if key not in answered:
                print("eblc_diff: no face answered `%s`, so that comparison did "
                      "not happen at all. FreeType reports a strike's own "
                      "baseline only for a face it judges bitmap-only, which here "
                      "means the .otb fonts and Wine's - check that the image "
                      "still carries fonts-terminus-otb, fonts-creep2 and "
                      "fonts-wine, and that the corpus list still selects by sfnt "
                      "magic rather than by file extension." % key)
                failed = True

    if not any(path in corpus.fonts("ebdt") for path in fonts):
        print("eblc_diff: every face compared was a fixture. The real "
              "population did not reach this run, so nothing here is a "
              "statement about the format as it ships.")
        failed = True

    if any(disagreements.values()) or failed:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
