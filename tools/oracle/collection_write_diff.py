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
"""Collections as this library writes them: every real one, as a TTC and a WOFF 2.

documentation/design.md section 12.4. `font-collect` gathers the faces of each real
collection - the corpus's own `.ttc` files and the W3C suite's three round-trip
collections - into a TrueType Collection and into a WOFF 2 collection.

  * The TTC is read by fontTools' `TTCollection`, and every table of every face must be
    the source face's, byte for byte: the writer shares tables, and changes none.
  * The WOFF 2 collection is read by this library (fontTools reads no WOFF 2
    collection) and each face must be the source face - tables byte for byte, `glyf`
    glyph by glyph - by the same comparison that checks the suite's collections.

Usage:
    collection_write_diff.py [--quiet]
"""

import json
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import corpus
import oracle_env

ROOT = oracle_env.ROOT
EXAMPLES = os.path.join(ROOT, "build", "linux", "release", "apps", "examples")
SCRATCH = os.path.join(ROOT, "build", "oracle", "collection-write")
REFERENCE = os.path.join(ROOT, "tools", "oracle", "fonttools_woff2.py")


def key(path):
    return path.replace("/", "_")


def reference(mode, files):
    argv = oracle_env.command("fonttools",
        ["python3", REFERENCE, mode, SCRATCH] + files, scratch=SCRATCH)
    finished = subprocess.run(argv, capture_output=True, text=True)
    if finished.returncode != 0:
        raise oracle_env.OracleUnavailable(
            "fontTools failed:\n%s" % finished.stderr[-2000:])
    return json.loads(finished.stdout.strip().splitlines()[-1])


def is_collection(path):
    with open(path, "rb") as handle:
        return handle.read(4) == b"ttcf"


def main(argv):
    quiet = "--quiet" in argv
    oracle_env.check_pin("fonttools")
    print(oracle_env.provenance(["fonttools"]))
    sources = [f for f in corpus.fonts("sfnt") if is_collection(f)]
    sources += [f for f in corpus.fonts("woff2")
                if f.endswith(".ttf") and "roundtrip-" in f and is_collection(f)]
    shutil.rmtree(SCRATCH, ignore_errors=True)
    os.makedirs(SCRATCH)
    collect = os.path.join(EXAMPLES, "font-collect")
    tables = os.path.join(EXAMPLES, "font-tables")
    problems = []
    ttcs = []
    woffs = []
    for path in sources:
        ttc = os.path.join(SCRATCH, key(path) + ".ttc")
        finished = subprocess.run([collect, ttc, "ttc", path], capture_output=True,
                                  text=True)
        if finished.returncode != 0:
            problems.append("%s -> ttc: %s" % (os.path.basename(path),
                finished.stderr.strip().splitlines()[:2]))
        else:
            ttcs.append(path)
        # The WOFF 2 collection sits beside a link to its source, which is how the
        # round-trip comparison finds the sfnt a file was made from.
        woff = os.path.join(SCRATCH, key(path) + ".woff2")
        finished = subprocess.run([collect, woff, "woff2", path], capture_output=True,
                                  text=True)
        if finished.returncode != 0:
            problems.append("%s -> woff2: %s" % (os.path.basename(path),
                finished.stderr.strip().splitlines()[:2]))
            continue
        os.symlink(path, woff[:-len(".woff2")] + ".ttf")
        face = 0
        while True:
            ours = "%s.ours.%d" % (os.path.join(SCRATCH, key(woff)), face)
            os.makedirs(ours, exist_ok=True)
            finished = subprocess.run([tables, woff, ours, str(face)],
                                      capture_output=True, text=True)
            if finished.returncode != 0:
                shutil.rmtree(ours)
                break
            face += 1
        woffs.append(woff)
    answer = reference("ttc", ttcs) if ttcs else {"checked": 0, "problems": []}
    problems += answer["problems"]
    round_trips = reference("roundtrip", woffs) if woffs else {}
    for path, bad in sorted(round_trips.items()):
        for line in bad:
            problems.append("%s: %s" % (os.path.basename(path), line))
    print("collections: %d sources (%d in the corpus, %d from the W3C suite), "
          "%d TTCs read by fontTools, %d WOFF 2 collections read back here"
          % (len(sources), sum(1 for s in sources if "sfnt" in s),
             sum(1 for s in sources if "woff2" in s), answer["checked"], len(woffs)))
    for line in problems[:(10 if quiet else 40)]:
        print("DISAGREE " + line)
    if problems:
        print("collections: %d disagreement(s)" % len(problems))
        return 1
    print("collections: no disagreements")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
