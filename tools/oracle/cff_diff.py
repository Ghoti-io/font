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
"""Every CFF glyph's charstring, this library against fontTools.

documentation/design.md sections 7.4 and 14. `glyf_diff.py` is the same idea for
the other outline format, and there are two differentials rather than one because
the two formats do not answer the same questions.

What it compares, per glyph:

  * **the program**, operator by operator with its operands in 16.16 and a
    `hintmask`'s mask bytes in hex, against fontTools' decompilation of the same
    bytes;
  * **the path**, against fontTools' own interpreter through a reference pen;
  * **the advance the charstring states**, which is `nominalWidthX` plus a delta
    or `defaultWidthX` where the program states none;
  * the **control box** of every point the program produced;
  * the **name** the charset gives the glyph, which for a CFF font is where glyph
    names live at all;
  * and whether the glyph is an **accented character** - `endchar` with four
    operands - which fontTools reports by emitting two components.

The first two are the pair that makes this worth running. A program compared
alone cannot see a curve operator whose geometry is wrong: `hflex` recomputes a y
this library could get wrong while reading every operand correctly. A path
compared alone cannot see *which* operand was misread, and for a glyph that draws
plausibly it cannot see that one was. The mask bytes are a third thing again: a
reader that miscounts stem hints reads the next operator out of the middle of a
mask, and the operators after it are then nonsense that a path comparison reports
as a shape.

**Paths are compared as cycles**, and the comparison, the one-64th allowance and
the canonical rotation are `glyf_diff`'s own functions rather than copies of
them: one contour-closing rule, imported.

Usage:
    cff_diff.py [--stride N] [--fonts N] [--quiet] [font...]
"""

import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import corpus
import glyf_diff
import oracle_env

ROOT = oracle_env.ROOT
DRIVER = os.path.join(ROOT, "build", "linux", "release", "apps", "examples",
                      "font-charstring")
REFERENCE = os.path.join(ROOT, "tools", "oracle", "fonttools_charstrings.py")
FIXTURES = os.path.join(ROOT, "tests", "data", "fonts")

# Every nth glyph of a real font, by default. A prime, for the reason
# glyf_diff's is: a stride that shares a factor with a font's block structure
# asks about the same position in every block.
GLYPH_STRIDE = 7

# How many disagreements to print per font and in total.
PER_FONT = 6
TOTAL = 60

# 16.16, which is how an operand and an advance are printed.
ONE_FIXED = 65536

# What our dump's "I cannot count the stems a subroutine declared" note becomes,
# so that the comparison can end where the dump did rather than at a token it
# cannot read as an operand.
PARTIAL = "(partial)"


class Skip(Exception):
    """This font cannot be compared, with a reason worth printing."""


def fixtures():
    """The synthetic fixtures, at every glyph, in every run.

    The corpus's 35 OTF fonts were surveyed before this differential was
    written, and what they hold is: every curve operator, `hintmask`,
    `cntrmask`, local and global subroutines, and the stem operators. What they
    hold **none** of is the whole flex family, an accented character, a
    CID-keyed font, a custom encoding, a charset in a format other than 0, or
    any of the arithmetic operators. Those are exactly what the fixtures carry,
    and thinning a run with `--fonts` drops corpus fonts and never these.
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
                continue
            found.append(os.path.join(FIXTURES, name))
    return found


def operand(text):
    """One of our dump's operands as 16.16.

    The dump prints a whole number as itself and a fractional one as
    `N+F/65536`, so that a file whose purpose is to show exactly what the font
    said carries no second rounding rule. Both spellings are the same sum.
    """
    if "+" in text:
        whole, _, fraction = text.partition("+")
        numerator, _, _ = fraction.partition("/")
        return int(whole) * ONE_FIXED + int(numerator)
    return int(text) * ONE_FIXED


def our_program_line(text):
    """One line of our charstring dump, as `name operands...` in 16.16.

    Our dump leads with the byte offset, which the reference has no counterpart
    for and which is dropped here rather than compared: two readers agreeing
    about where an operator *is* follows from agreeing about the operators
    before it.
    """
    parts = text.split()
    if len(parts) < 2:
        return None
    if not parts[0].isdigit():
        return None
    name = parts[1]
    out = [name]
    at = 2
    while at < len(parts) and parts[at] != "mask":
        if parts[at].startswith("("):
            # The dump saying, on the line, that it cannot go on: a hintmask
            # whose stems were declared inside a subroutine it did not run.
            out.append(PARTIAL)
            return " ".join(out)
        out.append(str(operand(parts[at])))
        at += 1
    if at < len(parts):
        out.append("mask")
        out.extend(parts[at + 1:])
    return " ".join(out)


def ours(path, face, stride, first):
    """This library's dump, as {key: value}."""
    finished = subprocess.run([DRIVER, path, str(face), str(stride),
        str(first)], capture_output=True, text=True)
    if finished.returncode != 0:
        raise Skip(finished.stderr.strip().splitlines()[-1]
                   if finished.stderr.strip() else "the driver refused it")
    return parse_ours(finished.stdout)


def reference(path, face, stride, first):
    """fontTools' answer for the same glyphs, as {key: value}."""
    argv = oracle_env.command("fonttools",
        ["python3", REFERENCE, path, str(face), str(stride), str(first)])
    finished = subprocess.run(argv, capture_output=True, text=True)
    if finished.returncode != 0:
        raise Skip("fontTools: %s"
                   % (finished.stderr.strip().splitlines()[-1]
                      if finished.stderr.strip() else "refused"))
    return parse_theirs(finished.stdout)


def parse_ours(text):
    """Our driver's output, as {key: value}.

    The program arrives as a block between `glyph N program:` and
    `glyph N program-end` rather than one prefixed line each, because it is
    written by the library's own dump function - which is the thing being
    tested, and wrapping it here would be testing a copy of it.
    """
    values = {}
    paths = {}
    program = None
    glyph_of_program = None
    for line in text.splitlines():
        if line.startswith("charstrings: "):
            values["charstrings"] = line[len("charstrings: "):]
            continue
        if not line.startswith("glyph "):
            if program is not None:
                converted = our_program_line(line)
                if converted is not None:
                    program.append(converted)
                elif line.startswith("charstring "):
                    # The dump's own header line: which language, and how long.
                    pass
            continue
        rest = line[len("glyph "):]
        index, _, rest = rest.partition(" ")
        colon = index.endswith(":")
        if colon:
            index = index[:-1]
        if not index.isdigit():
            continue
        glyph = int(index)
        if colon:
            values["glyph.%d.shape" % glyph] = rest
        elif rest == "program:":
            program = []
            glyph_of_program = glyph
        elif rest == "program-end":
            values["glyph.%d.program" % glyph_of_program] = tuple(program or ())
            program = None
        elif rest.startswith("counts: "):
            # Ours only: a CFF stores no points, so the reference has no count
            # to disagree with. Read and not compared.
            continue
        elif rest.startswith("width: "):
            # `stated` is ours only, for the same reason: fontTools reports the
            # width it computed and not how it was carried.
            values["glyph.%d.width" % glyph] = rest[len("width: "):].split()[0]
        elif rest.startswith("name: "):
            values["glyph.%d.name" % glyph] = rest[len("name: "):]
        elif rest.startswith("control: "):
            values["glyph.%d.control" % glyph] = glyf_diff.box(rest[9:])
        elif rest.startswith("path: "):
            paths.setdefault(glyph, []).append(glyf_diff.segment(rest[6:]))
    for glyph, segments in paths.items():
        values["glyph.%d.path" % glyph] = glyf_diff.canonical(segments)
    return values


def parse_theirs(text):
    """The reference's output, as {key: value}."""
    values = {}
    paths = {}
    programs = {}
    for line in text.splitlines():
        if line.startswith("charstrings: "):
            tail = line[len("charstrings: "):]
            if tail.endswith("inexact coordinate(s)"):
                values["inexact"] = int(tail.split()[0])
            else:
                values["charstrings"] = tail
            continue
        if not line.startswith("glyph "):
            continue
        rest = line[len("glyph "):]
        index, _, rest = rest.partition(" ")
        colon = index.endswith(":")
        if colon:
            index = index[:-1]
        if not index.isdigit():
            continue
        glyph = int(index)
        if colon:
            values["glyph.%d.shape" % glyph] = rest
        elif rest.startswith("width: "):
            values["glyph.%d.width" % glyph] = rest[len("width: "):]
        elif rest.startswith("name: "):
            values["glyph.%d.name" % glyph] = rest[len("name: "):]
        elif rest.startswith("control: "):
            values["glyph.%d.control" % glyph] = glyf_diff.box(rest[9:])
        elif rest.startswith("path: "):
            paths.setdefault(glyph, []).append(glyf_diff.segment(rest[6:]))
        elif rest.startswith("program: "):
            programs.setdefault(glyph, []).append(rest[len("program: "):])
    for glyph, segments in paths.items():
        values["glyph.%d.path" % glyph] = glyf_diff.canonical(segments)
    for glyph, lines in programs.items():
        values["glyph.%d.program" % glyph] = tuple(lines)
    return values


def is_synthetic_cid_name(want, got):
    """Whether this is fontTools' invented name for a CID-keyed glyph."""
    return (got == "-" and isinstance(want, str) and want.startswith("cid")
            and want[3:].isdigit())


def programs_agree(want, got, partial):
    """Two programs, allowing ours to stop where it says it cannot continue.

    Our dump stops at a `hintmask` that follows a subroutine call, and says so
    on the line: the mask's width is decided by how many stems have been
    declared, a subroutine can declare them, and a dump does not run
    subroutines. Where that happens the prefix is compared and the run counts
    it, rather than either side pretending to know the rest.
    """
    if want is None or got is None:
        return False
    for at, mine in enumerate(got):
        if mine.endswith(PARTIAL):
            # The last line this library could read: its operator and operands
            # still have to match, and the mask bytes after them are what it
            # said it could not know.
            head = mine[:-len(PARTIAL)].strip()
            if at >= len(want) or not want[at].startswith(head):
                return False
            partial[0] += 1
            return True
        if at >= len(want) or want[at] != mine:
            return False
    return len(got) == len(want)


def compare(path, face, stride, report, allowance, partial, categories):
    """One font. Returns (compared, disagreements, glyphs, inexact, declined)."""
    mine = ours(path, face, stride, 0)
    theirs = reference(path, face, stride, 0)

    if theirs.get("charstrings") == "none":
        raise Skip("no CFF table")
    if mine.get("charstrings") == "none":
        raise Skip("this library reports no charstrings")

    declined = {}
    for key, value in mine.items():
        if key.endswith(".shape") and str(value).startswith("refused "):
            declined[key.split(".")[1]] = str(value)

    # Glyphs the *reference* could not run. Not a tolerance and not a bucket
    # anything else can fall into: fontTools implements none of Type 2's
    # arithmetic operators - every one of `add`, `div`, `put`, `index`, `roll`
    # and the rest raises NotImplementedError - so for a glyph that uses one
    # there is no second engine to compare against, and saying so is the only
    # honest answer. The count is printed, so a run that rests on it says so.
    unavailable = set()
    for key, value in theirs.items():
        if key.endswith(".shape") and str(value).startswith("reference refused"):
            unavailable.add(key.split(".")[1])
            categories["no-reference"] += 1

    keys = sorted(set(mine) | set(theirs))
    shown = 0
    compared = 0
    disagreements = 0
    glyphs = set()
    for key in keys:
        if key in ("charstrings", "inexact"):
            continue
        parts = key.split(".")
        if len(parts) > 1 and parts[1] in unavailable:
            continue
        if len(parts) > 1 and parts[1] in declined:
            # A glyph this library refused has no program to disagree about.
            # Counted, and reported as a disagreement: unlike `glyf`, there is
            # no documented per-glyph refusal for a CFF charstring - a font
            # either has one or is corrupt.
            if key.endswith(".shape"):
                disagreements += 1
                if shown < PER_FONT and report[0] < TOTAL:
                    sys.stderr.write("  %s: %s: this library %r\n"
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
            if glyf_diff.paths_agree(want, got, allowance):
                continue
        elif key.endswith(".program"):
            if programs_agree(want, got, partial):
                continue
        elif key.endswith(".name") and is_synthetic_cid_name(want, got):
            # A CID-keyed font's charset holds CIDs, and a CID is not a name:
            # fontTools invents `cid00011` for such a glyph and this library
            # reports that it has none. A checked category, not an excuse - the
            # reference's name has to be exactly that shape and ours has to be
            # the absence.
            categories["cid-has-no-name"] += 1
            continue
        elif glyf_diff.near(want, got, allowance):
            continue
        disagreements += 1
        if shown < PER_FONT and report[0] < TOTAL:
            sys.stderr.write("  %s: %s: fontTools %r, this library %r\n"
                % (os.path.basename(path), key, want, got))
            shown += 1
            report[0] += 1
    return (compared, disagreements, len(glyphs), theirs.get("inexact", 0),
            len(declined))


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
    allowance = [0, 0]
    partial = [0]
    charstring_fonts = 0
    # Named differences, each a fact about the reference rather than a
    # tolerance. Printed whether or not they are zero, so that one appearing in
    # a run over real fonts is visible.
    categories = {"no-reference": 0, "cid-has-no-name": 0}

    synthetic_paths = set(fixtures())
    for path in fonts:
        try:
            fields, differed, seen, imprecise, refused = compare(path, 0,
                1 if path in synthetic_paths else stride, report, allowance,
                partial, categories)
        except Skip as why:
            skipped.append((path, str(why)))
            continue
        charstring_fonts += 1
        compared += fields
        disagreements += differed
        glyphs += seen
        inexact += imprecise
        declined += refused
        if not quiet:
            sys.stdout.write("  %-52s %6d glyphs, %7d fields%s\n"
                % (os.path.basename(path), seen, fields,
                   ", disagreements: %d" % differed if differed else ""))

    print("cff_diff: %d font(s) with charstrings of %d looked at, %d glyphs, "
          "%d fields compared, %d disagreements"
          % (charstring_fonts, len(fonts), glyphs, compared, disagreements))
    print("cff_diff: %d coordinate(s) the reference could not hold in 26.6; "
          "%d comparison(s) rested on the one-64th allowance, largest "
          "difference %d/64 of a font unit"
          % (inexact, allowance[0], allowance[1]))
    print("cff_diff: %d glyph(s) the reference cannot run at all (it "
          "implements none of Type 2's arithmetic operators); %d glyph(s) whose "
          "only difference is that fontTools invents a name for a CID"
          % (categories["no-reference"], categories["cid-has-no-name"]))
    if partial[0]:
        print("cff_diff: %d program(s) compared as a prefix, because a hintmask "
              "followed a subroutine call and a dump cannot count stems it did "
              "not run" % partial[0])
    if declined:
        print("cff_diff: %d glyph(s) this library refused" % declined)
    for path, why in skipped:
        if not quiet:
            print("cff_diff: skipped %s: %s" % (os.path.basename(path), why))

    # The denominators, so that a differential which has stopped comparing
    # cannot report a clean run. A CFF corpus is small - 35 of the image's 327
    # fonts - so the floor is on glyphs rather than on fonts.
    if not compared:
        sys.stderr.write("cff_diff: nothing was compared\n")
        return 1
    if charstring_fonts < 2:
        sys.stderr.write("cff_diff: only %d font(s) had charstrings at all, "
                         "which is not a population\n" % charstring_fonts)
        return 1
    if glyphs < 100:
        sys.stderr.write("cff_diff: only %d glyphs across %d fonts, which is "
                         "too few to be a corpus\n" % (glyphs, charstring_fonts))
        return 1
    return 1 if disagreements else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
