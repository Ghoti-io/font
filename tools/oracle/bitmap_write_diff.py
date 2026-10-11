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
"""Bitmap fonts as this library writes them: every real one, written and read back.

documentation/design.md section 12.4. `font-convert` writes each real bitmap font in
the corpus as a PCF and as a BDF, and two things are checked about the result.

  * **It is the font that went in.** `font-bitmap` dumps every glyph's pixels,
    bearings and advance, the strike's baseline and the whole encoding; the dump of
    the original and the dump of what was written must be identical apart from the
    container's name. That is this library reading its own output, so it can only
    say nothing was lost.
  * **Somebody else can read it.** `bitmap_diff.py` is pointed at the written files
    and compares this library's reading of each with Pillow's, which shares no code
    with either. A writer whose files only its own reader understands passes the
    first check and fails this one.

The population is the 234 PCFs of the oracle image's `xfonts-terminus` (gzipped, as
shipped) and the bitmap fixtures; the EBDT-carrying sfnts are written too and
checked the second way only, since their dump names a different container and a
different glyph order. PSF and `.hex` have no outside reader at all and are covered
by `tests/unit/test_bitmap_write.cpp`.

Usage:
    bitmap_write_diff.py [--fonts N] [--quiet]
"""

import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import corpus
import oracle_env

ROOT = oracle_env.ROOT
EXAMPLES = os.path.join(ROOT, "build", "linux", "release", "apps", "examples")
SCRATCH = os.path.join(ROOT, "build", "oracle", "bitmap-write")
FIXTURES = os.path.join(ROOT, "tests", "data", "fonts")


def dump(path):
    finished = subprocess.run([os.path.join(EXAMPLES, "font-bitmap"), path],
        capture_output=True, text=True)
    if finished.returncode != 0:
        return None
    lines = finished.stdout.splitlines()
    # The container's name is the one line that is supposed to change.
    return [line for line in lines if not line.startswith("container:")]


def main(argv):
    quiet = "--quiet" in argv
    limit = None
    if "--fonts" in argv:
        limit = int(argv[argv.index("--fonts") + 1])
    oracle_env.check_pin("fonttools")
    print(oracle_env.provenance(["fonttools"]))
    real = corpus.fonts("bitmap")
    fixtures = sorted(os.path.join(FIXTURES, f) for f in os.listdir(FIXTURES)
        if f.startswith("bitmap") and f.endswith((".pcf", ".bdf", ".pcf.gz")))
    sources = fixtures + real
    if limit:
        sources = sources[:limit]
    strikes = [f for f in corpus.fonts("ebdt")][:limit] if limit else corpus.fonts("ebdt")

    shutil.rmtree(SCRATCH, ignore_errors=True)
    os.makedirs(SCRATCH)
    convert = os.path.join(EXAMPLES, "font-convert")
    problems = []
    written = []
    refused = 0
    refusals = []
    for number, path in enumerate(sources + strikes):
        for fmt in ("pcf", "bdf"):
            target = os.path.join(SCRATCH, "%05d.%s" % (number, fmt))
            finished = subprocess.run([convert, path, target, fmt],
                capture_output=True, text=True)
            if finished.returncode != 0:
                said = finished.stdout + finished.stderr
                # Refusals the writer documents: a format that cannot say what the
                # strike says, a strike that is not in the first face (a collection
                # keeps them in others), and a glyph the font gets wrong, which the
                # reader reports per glyph and a writer will not paper over.
                if ("Unsupported" in said or "no strike at that index" in said
                        or "cannot be read" in said):
                    refused += 1
                    refusals.append("%s -> %s: %s" % (os.path.basename(path), fmt,
                        said.strip().splitlines()[-1]))
                    continue
                problems.append("%s -> %s: %s" % (os.path.basename(path), fmt,
                    finished.stderr.strip().splitlines()[:2]))
                continue
            written.append(target)
            if path in strikes:
                continue
            before, after = dump(path), dump(target)
            if before is None or after is None:
                problems.append("%s -> %s: a dump failed" % (os.path.basename(path), fmt))
            elif before != after:
                where = next((i for i, (a, b) in enumerate(zip(before, after)) if a != b),
                             min(len(before), len(after)))
                problems.append("%s -> %s differs at line %d of the dump: %r vs %r" % (
                    os.path.basename(path), fmt, where,
                    before[where] if where < len(before) else None,
                    after[where] if where < len(after) else None))

    print("bitmap write: %d source fonts (%d PCF in the image, %d fixtures, %d "
          "sfnt with strikes), %d files written, %d refused"
          % (len(sources) + len(strikes), len(real), len(fixtures), len(strikes),
             len(written), refused))
    for line in refusals:
        print("  refused: " + line)
    # Somebody else's reader, over everything written.
    finished = subprocess.run(
        [sys.executable, os.path.join(ROOT, "tools", "oracle", "bitmap_diff.py"),
         "--quiet"] + written, capture_output=True, text=True)
    summary = [l for l in finished.stdout.splitlines()
               if l.startswith("bitmap_diff: ") and "disagreements" in l]
    print("\n".join(summary[:1]))
    if finished.returncode != 0:
        problems.append("bitmap_diff: the written files disagree with Pillow:\n%s"
                        % finished.stdout[-1500:])
    for line in problems[:(10 if quiet else 40)]:
        print("DISAGREE " + line)
    if problems:
        print("bitmap write: %d disagreement(s)" % len(problems))
        return 1
    print("bitmap write: no disagreements")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
