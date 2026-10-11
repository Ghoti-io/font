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
"""WOFF 2 as this library writes it: every corpus font, read back two ways.

documentation/design.md section 12.4. `font-convert` writes each font as a WOFF 2 with
`gfnt_write_woff2()`; fontTools reads the file, and this library reads it, and each
result is compared with the font that went in - every table byte for byte (`head`
apart from its adjustment and the transformed bit), `glyf` glyph by glyph. A writer
whose output only its own reader can read would pass the reader's differential and
fail this one.

Collections are written as their first face, which is all the writer holds.

Usage:
    woff2_write_diff.py [--max-bytes N] [--fonts N] [--stride N] [--quiet]
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
SCRATCH = os.path.join(ROOT, "build", "oracle", "woff2-write")
REFERENCE = os.path.join(ROOT, "tools", "oracle", "fonttools_woff2.py")
DEFAULT_MAX_BYTES = 8 * 1024 * 1024


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
    args = [a for a in argv[1:] if a != "--quiet"]
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
    fonts = [f for f in everything if os.path.getsize(f) <= max_bytes][::stride]
    if limit:
        fonts = fonts[:limit]

    shutil.rmtree(SCRATCH, ignore_errors=True)
    os.makedirs(SCRATCH)
    convert = os.path.join(EXAMPLES, "font-convert")
    tables = os.path.join(EXAMPLES, "font-tables")
    problems = []
    for path in fonts:
        target = os.path.join(SCRATCH, key(path) + ".woff2")
        finished = subprocess.run([convert, path, target, "woff2"],
            capture_output=True, text=True)
        if finished.returncode != 0:
            problems.append("%s: font-convert: %s" % (
                os.path.basename(path), finished.stderr.strip().splitlines()[:2]))
            continue
        ours = target + ".ours"
        os.makedirs(ours)
        finished = subprocess.run([tables, target, ours],
            capture_output=True, text=True)
        if finished.returncode != 0:
            shutil.rmtree(ours)
    present = [f for f in fonts
               if os.path.exists(os.path.join(SCRATCH, key(f) + ".woff2"))]
    compared = reference("written", present)
    problems += compared["problems"]
    sizes = [(os.path.getsize(f),
              os.path.getsize(os.path.join(SCRATCH, key(f) + ".woff2")))
             for f in present]
    print("woff2 written: %d fonts, %d compared; %d corpus fonts left out "
          "(over %d bytes); output is %.0f%% of the input's size"
          % (len(fonts), compared["checked"], len(everything) - len(fonts),
             max_bytes, 100.0 * sum(b for _, b in sizes) / max(1, sum(a for a, _ in sizes))))
    if compared["explained"]:
        print("explained, %d font(s): fontTools' own decode of a transformed hmtx "
              "loses entries; the table this library reads from the same file is "
              "the source's, byte for byte" % len(compared["explained"]))
    for line in problems[:(10 if quiet else 40)]:
        print("DISAGREE " + line)
    if len(problems) > 40:
        print("... and %d more" % (len(problems) - 40))
    if problems or compared["checked"] != len(fonts):
        print("woff2 written: %d disagreement(s)" % len(problems))
        return 1
    print("woff2 written: no disagreements")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
