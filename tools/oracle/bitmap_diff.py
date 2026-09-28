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
"""Every bitmap glyph of a PCF or BDF font, this library against Pillow.

documentation/design.md sections 7.1, 7.5 and 14. fontTools reads neither of
these formats, so the reference is Pillow's `PcfFontFile` and `BdfFontFile`
(`tools/oracle/pillow_bitmaps.py` is the adapter) - a reader descended from PIL's
X font support that shares no code with this library.

What is compared, per glyph: **every pixel**, the box, the two bearings and the
advance. Pixels are the thing worth comparing here for the reason paths are in
`cff_diff`: a box compared alone cannot see a row read from the wrong offset, and
a font whose rows are all shifted by one still has the right box.

The population is the real one. `xfonts-terminus` in the oracle image ships
several hundred PCF files - one per encoding, several hundred glyphs each - and
they are **compressed**, which this library does not read - not for want of a
decoder, since `compress` implements RFC 1952, but because `font` has not taken that
dependency (design.md section 7.1). So this driver
decompresses each into `build/oracle` and feeds both readers the plain bytes; the
gzip container itself is therefore not under test here, and the report says so.

What this differential **cannot** cover, stated because a run that printed a clean
number without saying so would be claiming more than it checked:

  * **PSF and `.hex` have no reference at all.** Nothing in the image reads them.
    What covers them is the four-container identity in `tests/unit/test_bitmap.cpp`
    - one design written four ways, which no single reader can make agree by
    being wrong the same way twice - and the unit suite's crafted refusals.
  * **A PCF's encodings table has no reference either**, because Pillow's reading
    of it is wrong: `_load_encoding` indexes the offsets array by character code
    rather than by `code - firstCol`, so for a font whose codes start at 0x20 -
    most of them - every glyph is reported 32 positions from where it is. This
    driver therefore compares glyphs by *index*, which is how the metrics and
    bitmap tables key them, and leaves the mapping to the unit suite. Pillow's BDF
    encoding is sound and **is** compared.
  * **A PCF whose bit order and byte order differ** is a layout Pillow does not
    implement: `_load_bitmaps` leaves the byte-order bit commented out in its own
    source. `bitmap-swap.pcf` is that font on purpose, and it is counted as a
    layout the reference declines rather than as a disagreement.

Each of those three is a *named* count in the report rather than a silent
exclusion, because a differential whose denominator hides what it skipped reports
the same clean number either way.

Usage:
    bitmap_diff.py [--stride N] [--fonts N] [--quiet] [font...]
"""

import gzip
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import corpus
import oracle_env

ROOT = oracle_env.ROOT
DRIVER = os.path.join(ROOT, "build", "linux", "release", "apps", "examples",
                      "font-bitmap")
REFERENCE = os.path.join(ROOT, "tools", "oracle", "pillow_bitmaps.py")
FIXTURES = os.path.join(ROOT, "tests", "data", "fonts")
UNPACKED = os.path.join(ROOT, "build", "oracle", "bitmap-plain")

# Every nth glyph of a real font, by default. A prime, for the reason
# `glyf_diff`'s stride is one: a stride sharing a factor with a font's block
# structure asks about the same position in every block.
GLYPH_STRIDE = 7

# How many disagreements to print per font and in total.
PER_FONT = 6
TOTAL = 60


class Skip(Exception):
    """This font cannot be compared, with a reason worth printing.

    `expected` separates two events that wear one word, as `cff_diff`'s does: a
    font neither side was ever going to compare is counted, and a font this
    differential *meant* to compare and could not is named on screen even under
    --quiet, because that is the one event a clean run cannot be told from
    agreement.
    """

    def __init__(self, why, expected=False):
        super().__init__(why)
        self.expected = expected


def fixtures():
    """The two synthetic containers Pillow can read, at every glyph.

    The corpus's PCFs are real fonts and hold what real fonts hold: one bit order,
    one byte order, rows padded the way the machine that compiled them padded
    them. What they hold **none** of is a compressed metrics table, an
    ink-metrics table, a glyph with an empty box, an advance that differs from the
    width, or a row whose bits past the width are set. Those are the fixtures', and
    thinning a run with `--fonts` drops corpus fonts and never these.
    """
    manifest = os.path.join(FIXTURES, "MANIFEST")
    if not os.path.exists(manifest):
        return []
    found = []
    with open(manifest, encoding="utf-8") as handle:
        for line in handle:
            if line.startswith("#") or "\t" not in line:
                continue
            name = line.split("\t", 1)[0]
            if name.endswith(".pcf") or name.endswith(".bdf"):
                found.append(os.path.join(FIXTURES, name))
    return sorted(found)


def plain(path):
    """A readable path for @p path, decompressing a `.pcf.gz` if it is one.

    Both readers are handed the same plain bytes, so the comparison is of two
    readings of one PCF and not of two decompressors.
    """
    if not path.endswith(".gz"):
        return path
    os.makedirs(UNPACKED, exist_ok=True)
    target = os.path.join(UNPACKED, os.path.basename(path)[:-3])
    if not os.path.exists(target):
        with gzip.open(path, "rb") as source:
            with open(target + ".part", "wb") as sink:
                shutil.copyfileobj(source, sink)
        os.replace(target + ".part", target)
    return target


def theirs(path):
    """Pillow's answer for the same font, as {key: value}."""
    argv = oracle_env.command("fonttools", ["python3", REFERENCE, path])
    finished = subprocess.run(argv, capture_output=True, text=True)
    if finished.returncode != 0:
        raise Skip("Pillow: %s"
                   % (finished.stderr.strip().splitlines()[-1]
                      if finished.stderr.strip() else "refused"))
    return parse(finished.stdout)


def parse(text):
    """One `key value` line per fact, into a dictionary."""
    values = {}
    for line in text.splitlines():
        if not line.strip():
            continue
        # The key is everything before the last space for a row line and before
        # the first space otherwise; both are handled by splitting on the known
        # prefixes rather than guessing.
        if line.startswith(("glyph ", "char ")) and " row " in line:
            key, value = line.rsplit(" ", 1)
        elif line.startswith(("glyph ", "char ")) and " box" in line:
            head, value = line.split(" box ", 1)
            key = head + " box"
        elif line.startswith("property "):
            parts = line.split(" ", 2)
            key = " ".join(parts[:2])
            value = parts[2] if len(parts) > 2 else ""
        elif " " in line:
            key, value = line.split(" ", 1)
        else:
            key, value = line, ""
        values[key] = value.strip()
    return values


def our_glyphs(text):
    """Our driver's output as {index: (box, [rows])} plus its own mapping."""
    glyphs = {}
    mapping = {}
    current = None
    rows = []
    box = None
    for line in text.splitlines():
        if line.startswith("map U+"):
            # `map U+0041: glyph 2`, and the key kept here is the codepoint's
            # digits alone - keeping the `U+` too spelled the lookup key `U+U+0041`
            # and every mapping read as one the reference had not reported, which a
            # count of "unreported" facts is exactly the shape to hide.
            code, glyph = line[len("map U+"):].split(": glyph ")
            mapping[code] = glyph
            continue
        if line.startswith("glyph ") and ": bitmap: " in line:
            if current is not None:
                glyphs[current] = (box, rows)
            head, rest = line.split(": bitmap: ", 1)
            current = int(head.split(" ", 1)[1])
            rows = []
            # `WxH, bearing X,Y, advance A, D bpp, strike ...`
            size, bearing, advance = rest.split(", ")[0:3]
            width, height = size.split("x")
            bearing_x, bearing_y = bearing.replace("bearing ", "").split(",")
            box = "%s %s %s %s %s" % (width, height, bearing_x, bearing_y,
                advance.replace("advance ", ""))
            continue
        if current is not None and line and all(c in ".#" for c in line):
            rows.append(line)
            continue
        if line.startswith("glyph ") and " name:" in line:
            continue
    if current is not None:
        glyphs[current] = (box, rows)
    return glyphs, mapping


def compare(path, stride, categories, report):
    """One font. Returns (fields, disagreements, glyphs, kind)."""
    readable = plain(path)
    mine_text = subprocess.run([DRIVER, readable, "1"], capture_output=True,
        text=True)
    if mine_text.returncode != 0:
        raise Skip("this library: %s"
                   % (mine_text.stderr.strip().splitlines()[-1]
                      if mine_text.stderr.strip() else "refused"))
    mine, my_map = our_glyphs(mine_text.stdout)
    reference = theirs(readable)

    fields = 0
    differed = 0
    seen = 0
    kind = reference.get("format", "?")

    if kind == "pcf":
        # Pillow leaves the byte-order bit commented out in `_load_bitmaps`, so a
        # font whose bits and bytes run opposite ways with a scan unit of more than
        # one byte is laid out differently by it. Decided from the format word
        # rather than from how many glyphs disagreed, so that a real defect cannot
        # be reclassified as a limitation by being widespread enough.
        padindex, bit_msb, byte_msb, scan = (
            int(value) for value in reference.get("layout", "0 0 0 1").split())
        if bit_msb != byte_msb and scan > 1:
            raise Skip("Pillow does not implement this layout: its bit and byte "
                       "orders differ with a %d-byte scan unit" % scan,
                       expected=True)

    # A BDF's glyph with no character is absent from Pillow's list entirely, and a
    # PCF's glyph index is what its metrics and bitmaps are keyed by. So the key
    # differs per format, and the glyphs each side could not name are counted.
    for index in sorted(mine):
        if index % stride:
            continue
        seen += 1
        if kind == "bdf":
            code = None
            for candidate, glyph in my_map.items():
                if int(glyph) == index:
                    code = candidate
                    break
            if code is None:
                # `ENCODING -1`: a glyph this font reaches only by name, which
                # Pillow keeps no record of at all.
                categories["unencoded-glyph"] += 1
                continue
            prefix = "char U+%s" % code
        else:
            prefix = "glyph %d" % index

        box_key = prefix + " box"
        if box_key not in reference:
            categories["no-reference-glyph"] += 1
            continue
        fields += 1
        if mine[index][0] != reference[box_key]:
            differed += 1
            if report[0] < TOTAL:
                report[0] += 1
                print("    %s %s: ours %s, Pillow %s"
                    % (os.path.basename(path), box_key, mine[index][0],
                       reference[box_key]))
        for number, row in enumerate(mine[index][1]):
            key = "%s row %d" % (prefix, number)
            if key not in reference:
                categories["no-reference-row"] += 1
                continue
            fields += 1
            if row != reference[key]:
                differed += 1
                if report[0] < TOTAL:
                    report[0] += 1
                    print("    %s %s: ours %s, Pillow %s"
                        % (os.path.basename(path), key, row, reference[key]))

    # A BDF's mapping is Pillow's to check, because its BDF encoding is sound. A
    # PCF's is not, for the reason in the module docstring.
    if kind == "bdf":
        for code, glyph in sorted(my_map.items()):
            if ("char U+%s box" % code) not in reference:
                categories["no-reference-mapping"] += 1
                continue
            fields += 1
            # The reference names a glyph by the codepoint that reaches it, so
            # agreement here is that both readers reached a glyph of that code at
            # all - and the box and rows above are what say it is the same glyph.
    else:
        categories["pcf-mapping-unchecked"] += 1

    return fields, differed, seen, kind


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
        # A font named on the command line is one somebody is looking at, so every
        # glyph of it is compared rather than every seventh.
        synthetic = set(rest)
    else:
        real = corpus.fonts("bitmap")
        if limit is not None:
            step = max(1, len(real) // limit)
            real = real[::step][:limit]
        synthetic = set(fixtures())
        fonts = sorted(synthetic) + real

    if not fonts:
        sys.stderr.write("no fonts to compare\n")
        return 1

    compared = 0
    disagreements = 0
    glyphs = 0
    skipped = []
    report = [0]
    per_kind = {"pcf": 0, "bdf": 0}
    categories = {"no-reference-glyph": 0, "no-reference-row": 0,
                  "no-reference-mapping": 0, "pcf-mapping-unchecked": 0,
                  "unencoded-glyph": 0}

    for path in fonts:
        try:
            fields, differed, seen, kind = compare(path,
                1 if path in synthetic else stride, categories, report)
        except Skip as why:
            skipped.append((path, str(why), why.expected))
            continue
        per_kind[kind] = per_kind.get(kind, 0) + 1
        compared += fields
        disagreements += differed
        glyphs += seen
        if not quiet:
            sys.stdout.write("  %-52s %6d glyphs, %7d fields%s\n"
                % (os.path.basename(path), seen, fields,
                   ", disagreements: %d" % differed if differed else ""))

    print("bitmap_diff: %d PCF and %d BDF font(s) of %d looked at, %d glyphs, "
          "%d fields compared, %d disagreements"
          % (per_kind.get("pcf", 0), per_kind.get("bdf", 0), len(fonts), glyphs,
             compared, disagreements))
    print("bitmap_diff: %d font(s) skipped, %d of them for a reason this "
          "differential expects" % (len(skipped),
              sum(1 for _, _, expected in skipped if expected)))
    print("bitmap_diff: %d PCF encodings table(s) left unchecked - Pillow's "
          "reading of one is wrong when a font's first code is not zero, so "
          "glyphs are compared by index and the mapping is the unit suite's"
          % categories["pcf-mapping-unchecked"])
    print("bitmap_diff: PSF and .hex have no reference in this image at all; "
          "what covers them is one design written four ways in "
          "tests/unit/test_bitmap.cpp")
    print("bitmap_diff: %d glyph(s), %d row(s) and %d mapping(s) the reference "
          "did not report at all, and %d BDF glyph(s) with no character, which "
          "Pillow keeps no record of"
          % (categories["no-reference-glyph"], categories["no-reference-row"],
             categories["no-reference-mapping"], categories["unencoded-glyph"]))
    # Printed whether or not it is empty, for the reason cff_diff prints its
    # skips unconditionally: a font that silently stopped being compared reads as
    # agreement.
    for path, why, expected in skipped:
        print("  %-52s skipped: %s%s" % (os.path.basename(path), why,
            "" if expected else "  <- not expected"))

    if disagreements:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
