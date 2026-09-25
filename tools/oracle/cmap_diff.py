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
"""Every codepoint of every real font, this library against fontTools.

documentation/design.md section 14, and the reason this file matters more than
any test in `tests/`: every expectation there is one this library wrote for
itself, and a corpus grown from your own understanding of a format cannot find
the place where that understanding is wrong. This is the first thing in the
repository that asks an outside authority.

What it compares, per font:

  * every codepoint fontTools reports as mapped,
  * **both neighbours of each of those**, which is where a segment that runs one
    entry too far shows up - the defect class format 4 is famous for (M10),
  * a stride sample over the whole 0..0x10FFFF space, so that a codepoint this
    library maps and the reference does not is caught rather than never asked
    about,
  * and with `--exhaustive`, all 1,114,112 codepoints, which is what section 14
    asks for and what the stride sample is a cheaper stand-in for.

The two sides are asked about **the same subtable**: this library's own
preference order picks one, its driver prints which, and the reference is then
asked about that pair rather than about whichever subtable fontTools would have
chosen. Comparing two different subtables of the same font would be a
comparison of two correct answers.

One deliberate library behaviour is replicated here rather than scored as a
disagreement: a Windows symbol subtable `(3,0)` is read through its
0xF000..0xF0FF range, so a request for 'A' is answered from 0xF041. That is what
every other implementation does with a symbol font, and the differential says so
in its output when it applies it.

Usage:
    cmap_diff.py [--exhaustive] [--stride N] [--fonts N] [--quiet] [font...]
"""

import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import corpus
import oracle_env

ROOT = oracle_env.ROOT
DRIVER = os.path.join(ROOT, "build", "linux", "release", "apps", "examples",
                      "font-cmap")
REFERENCE = os.path.join(ROOT, "tools", "oracle", "fonttools_cmap.py")

# The stride sample's step. A prime, so that the sample is not aligned with the
# block structure of anything: a step of 1000 would ask about 0x0000, 0x03E8,
# 0x07D0 ... and miss every range boundary that happens to be a round number.
STRIDE = 997

MAX_UNICODE = 0x10FFFF

# How many disagreements to print per font, and in total, before saying how many
# more there were. A differential that prints two hundred thousand lines is one
# nobody reads.
PER_FONT = 8
TOTAL = 60


class Skip(Exception):
    """This font cannot be compared, with a reason worth printing."""


def ours_header(path, face):
    """Ask this library which subtable it will use. Empty stdin: no lookups."""
    finished = subprocess.run([DRIVER, path, str(face)], input="",
        capture_output=True, text=True)
    if finished.returncode != 0:
        raise Skip(finished.stderr.strip() or "the driver refused the font")
    for line in finished.stdout.splitlines():
        if line.startswith("# subtable "):
            platform, encoding, fmt = (int(x) for x in line.split()[2:5])
            return platform, encoding, fmt
    raise Skip("the driver named no subtable")


def ours(path, face, codepoints):
    """This library's answers: {codepoint: glyph}, with failures as strings."""
    request = "".join("%d\n" % cp for cp in codepoints)
    finished = subprocess.run([DRIVER, path, str(face)], input=request,
        capture_output=True, text=True)
    if finished.returncode != 0:
        raise Skip(finished.stderr.strip() or "the driver refused the font")
    answers = {}
    for line in finished.stdout.splitlines():
        if line.startswith("#"):
            continue
        key, _, value = line.partition(" ")
        answers[int(key)] = int(value) if value.isdigit() else value
    return answers


def reference(path, face, platform, encoding):
    """fontTools' answers for the same subtable: {codepoint: glyph}."""
    argv = oracle_env.command("fonttools",
        ["python3", REFERENCE, path, str(face), str(platform), str(encoding)])
    finished = subprocess.run(argv, capture_output=True, text=True)
    if finished.returncode != 0:
        raise Skip("fontTools: %s" % (finished.stderr.strip().splitlines()[-1]
                                      if finished.stderr.strip() else "refused"))
    mapping = {}
    for line in finished.stdout.splitlines():
        if line.startswith("#"):
            continue
        key, _, value = line.partition(" ")
        mapping[int(key)] = int(value)
    return mapping


def queries(mapped, exhaustive, stride):
    """Which codepoints to ask about, and why each is in the set."""
    if exhaustive:
        return range(0, MAX_UNICODE + 1)
    wanted = set()
    for codepoint in mapped:
        # The mapped codepoint, and both neighbours: a range that starts one too
        # early or ends one too late is invisible to a set of mapped codepoints
        # alone, because the extra entry is exactly where nothing was asked.
        for offset in (-1, 0, 1):
            neighbour = codepoint + offset
            if 0 <= neighbour <= MAX_UNICODE:
                wanted.add(neighbour)
    wanted.update(range(0, MAX_UNICODE + 1, stride))
    return sorted(wanted)


def expected(mapping, codepoint, symbol):
    """What the reference says, including the symbol range when it applies."""
    glyph = mapping.get(codepoint, 0)
    if glyph == 0 and symbol and codepoint <= 0xFF:
        glyph = mapping.get(0xF000 | codepoint, 0)
    return glyph


def compare(path, face, exhaustive, stride, report):
    """One font. Returns (codepoints compared, disagreements found)."""
    platform, encoding, fmt = ours_header(path, face)
    mapping = reference(path, face, platform, encoding)
    symbol = (platform, encoding) == (3, 0)

    asked = queries(mapping, exhaustive, stride)
    answers = ours(path, face, asked)

    shown = 0
    disagreements = 0
    for codepoint in asked:
        want = expected(mapping, codepoint, symbol)
        got = answers.get(codepoint)
        if got == want:
            continue
        disagreements += 1
        if shown < PER_FONT and report[0] < TOTAL:
            sys.stderr.write(
                "  %s: U+%04X: fontTools %s, this library %s "
                "(subtable %d,%d format %d)\n"
                % (os.path.basename(path), codepoint, want, got, platform,
                   encoding, fmt))
            shown += 1
            report[0] += 1
    return len(asked), disagreements, len(mapping), symbol


def main(argv):
    exhaustive = "--exhaustive" in argv
    quiet = "--quiet" in argv
    stride = STRIDE
    limit = None
    rest = []
    skip_next = False
    for index, argument in enumerate(argv[1:], start=1):
        if skip_next:
            skip_next = False
            continue
        if argument == "--stride":
            stride = int(argv[index + 1])
            skip_next = True
        elif argument == "--fonts":
            limit = int(argv[index + 1])
            skip_next = True
        elif argument in ("--exhaustive", "--quiet"):
            continue
        else:
            rest.append(argument)

    if not os.path.exists(DRIVER):
        sys.stderr.write("%s is not built; run `make examples`\n" % DRIVER)
        return 1

    fonts = rest or corpus.fonts("sfnt")
    if limit is not None:
        # A deterministic thinning rather than the first N, so a short run still
        # covers every foundry and both outline formats.
        step = max(1, len(fonts) // limit)
        fonts = fonts[::step][:limit]
    if not fonts:
        sys.stderr.write("no fonts to compare\n")
        return 1

    compared = 0
    disagreements = 0
    mapped_total = 0
    skipped = []
    symbol_fonts = 0
    report = [0]

    for path in fonts:
        try:
            asked, differed, mapped, symbol = compare(path, 0, exhaustive,
                stride, report)
        except Skip as why:
            skipped.append((path, str(why)))
            continue
        compared += asked
        disagreements += differed
        mapped_total += mapped
        symbol_fonts += 1 if symbol else 0
        if not quiet:
            sys.stdout.write("  %-52s %7d codepoints, %5d mapped%s\n"
                % (os.path.basename(path), asked, mapped,
                   ", disagreements: %d" % differed if differed else ""))

    print("cmap_diff: %d fonts, %d codepoints compared, %d mapped by the "
          "reference, %d disagreements%s"
          % (len(fonts) - len(skipped), compared, mapped_total, disagreements,
             ", exhaustive" if exhaustive else ", stride %d" % stride))
    if symbol_fonts:
        print("cmap_diff: %d font(s) read through the Windows symbol range, "
              "which this differential applies to the reference too"
              % symbol_fonts)
    for path, why in skipped:
        print("cmap_diff: skipped %s: %s" % (os.path.basename(path), why))

    # The denominators, so that a differential which has stopped comparing
    # cannot report a clean run. A corpus that fails to materialise, a driver
    # that refuses every font, a reference that answers nothing - each would
    # otherwise look exactly like agreement.
    if not compared:
        sys.stderr.write("cmap_diff: nothing was compared\n")
        return 1
    if mapped_total < 1000:
        sys.stderr.write(
            "cmap_diff: the reference mapped only %d codepoints across %d "
            "fonts, which is too few to be a corpus\n"
            % (mapped_total, len(fonts)))
        return 1
    if len(skipped) > len(fonts) // 4:
        sys.stderr.write(
            "cmap_diff: %d of %d fonts were skipped; the run is not a "
            "measurement of the corpus\n" % (len(skipped), len(fonts)))
        return 1
    return 1 if disagreements else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
