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
"""WOFF 2: every corpus font, wrapped by fontTools and unwrapped here.

documentation/design.md section 7.1. WOFF 2 is the format with a second reader
in it - `glyf` and `hmtx` are stored in a form of their own - so the question is
not whether the Brotli stream inflates but whether what comes out is the font that
went in. For each font small enough to be worth compressing at fontTools' quality
(the cap is the `--max-bytes` option, and the report says how many fonts it left
out), fontTools writes three files: the default transforms, those plus `hmtx`, and
the null transform. This reads each with `font-tables` and compares, table by
table, against what fontTools reads back out of the same file; `glyf` is compared
glyph by glyph (`fonttools_woff2.py` says why), against fontTools' decode and
against the font the file was made from.

Collections are not here: fontTools writes no WOFF 2 collection. A collection is
covered by a fixture the generator builds.

Usage:
    woff2_diff.py [--max-bytes N] [--fonts N] [--stride N] [--quiet] [--reuse]
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
SCRATCH = os.path.join(ROOT, "build", "oracle", "woff2")
REFERENCE = os.path.join(ROOT, "tools", "oracle", "fonttools_woff2.py")
VARIANTS = ("default", "hmtx", "stored")

# fontTools compresses at Brotli quality 11, which costs seconds per megabyte.
DEFAULT_MAX_BYTES = 2 * 1024 * 1024


def key(path):
    return path.replace("/", "_")


def reference(mode, fonts):
    argv = oracle_env.command("fonttools",
        ["python3", REFERENCE, mode, SCRATCH] + fonts, scratch=SCRATCH)
    finished = subprocess.run(argv, capture_output=True, text=True)
    if finished.returncode != 0:
        raise oracle_env.OracleUnavailable(
            "fontTools failed:\n%s" % finished.stderr[-2000:])
    return json.loads(finished.stdout.strip().splitlines()[-1])


def main(argv):
    max_bytes = DEFAULT_MAX_BYTES
    limit = None
    stride = 1
    quiet = "--quiet" in argv
    reuse = "--reuse" in argv
    args = [a for a in argv[1:] if a not in ("--quiet", "--reuse")]
    while args:
        flag = args.pop(0)
        value = args.pop(0)
        if flag == "--max-bytes":
            max_bytes = int(value)
        elif flag == "--fonts":
            limit = int(value)
        elif flag == "--stride":
            stride = int(value)
        else:
            raise SystemExit("unknown option %s" % flag)

    oracle_env.check_pin("fonttools")
    print(oracle_env.provenance(["fonttools"]))
    everything = corpus.fonts("sfnt")
    # A collection says what it is in its first four bytes; its name does not. The W3C decoder
    # suite's round-trip collections are named .ttf.
    def is_collection(path):
        with open(path, "rb") as handle:
            return handle.read(4) == b"ttcf"
    candidates = [f for f in everything if not is_collection(f)]
    small = [f for f in candidates if os.path.getsize(f) <= max_bytes]
    fonts = small[::stride]
    if limit:
        fonts = fonts[:limit]
    left_out = len(everything) - len(fonts)

    if reuse:
        # The fontTools half is the slow one - Brotli at quality 11 - and does not
        # depend on this library, so a rerun after a fix can keep it.
        made = {}
        for path in fonts:
            for variant in VARIANTS:
                shutil.rmtree(os.path.join(
                    SCRATCH, "%s.%s.woff2.ours" % (key(path), variant)),
                    ignore_errors=True)
    else:
        shutil.rmtree(SCRATCH, ignore_errors=True)
        os.makedirs(SCRATCH)
        made = reference("make", fonts)
    refused = {p: {v: r for v, r in vs.items() if isinstance(r, str)}
               for p, vs in made.items()}
    refused = {p: vs for p, vs in refused.items() if vs}

    driver = os.path.join(EXAMPLES, "font-tables")
    failures = []
    files = 0
    for path in fonts:
        for variant in VARIANTS:
            target = os.path.join(SCRATCH, "%s.%s.woff2" % (key(path), variant))
            if not os.path.exists(target):
                continue
            files += 1
            ours = target + ".ours"
            os.makedirs(ours)
            finished = subprocess.run([driver, target, ours],
                capture_output=True, text=True)
            if finished.returncode != 0:
                shutil.rmtree(ours)
                failures.append("%s %s: font-tables: %s" % (
                    os.path.basename(path), variant,
                    (finished.stderr or "").strip().splitlines()[:2]))
    compared = reference("compare", fonts)
    problems = failures + compared["problems"]

    print("woff2: %d fonts, %d files written by fontTools, %d compared; "
          "%d corpus fonts left out (collections, over %d bytes)"
          % (len(fonts), files, compared["checked"], left_out, max_bytes))
    if refused:
        print("fontTools refused to write %d font(s):" % len(refused))
        for p, vs in sorted(refused.items())[:5]:
            print("  %s: %s" % (os.path.basename(p), vs))
    if compared["explained"]:
        print("explained, %d font(s): fontTools' own decode of the transformed "
              "hmtx is shorter than the table it was given and than the length the "
              "file states; this library's is the source font's, byte for byte"
              % len(compared["explained"]))
    for line in problems[:(10 if quiet else 40)]:
        print("DISAGREE " + line)
    if len(problems) > 40:
        print("... and %d more" % (len(problems) - 40))
    if problems or compared["checked"] != files:
        print("woff2: %d disagreement(s)" % len(problems))
        return 1
    print("woff2: no disagreements")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
