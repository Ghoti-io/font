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
"""Type 1 fonts as this library writes them: the URW base 35, as PFA and as PFB.

documentation/design.md section 12.4. `font-convert` re-wraps each Type 1 program of
the oracle image (the private portion encrypted afresh, the charstrings carried as
they were), and fontTools' `t1Lib`, which shares no code with this library, reads the
original and both outputs: the font dictionary, the private dictionary and every
decrypted charstring must be the same. A writer whose files only its own reader
decrypts would pass the unit tests and fail this.

Usage:
    type1_write_diff.py [--quiet]
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
SCRATCH = os.path.join(ROOT, "build", "oracle", "type1-write")
REFERENCE = os.path.join(ROOT, "tools", "oracle", "fonttools_type1.py")


def key(path):
    return path.replace("/", "_")


def main(argv):
    quiet = "--quiet" in argv
    oracle_env.check_pin("fonttools")
    print(oracle_env.provenance(["fonttools"]))
    everything = corpus.fonts("type1")
    shutil.rmtree(SCRATCH, ignore_errors=True)
    os.makedirs(SCRATCH)
    convert = os.path.join(EXAMPLES, "font-convert")
    problems = []
    fonts = []
    for path in everything:
        ok = True
        for extension in ("pfa", "pfb"):
            finished = subprocess.run([convert, path,
                os.path.join(SCRATCH, key(path) + "." + extension), extension],
                capture_output=True, text=True)
            if finished.returncode != 0:
                problems.append("%s -> %s: %s" % (os.path.basename(path), extension,
                    finished.stderr.strip().splitlines()[:2]))
                ok = False
        if ok:
            fonts.append(path)
    argv_ref = oracle_env.command("fonttools",
        ["python3", REFERENCE, SCRATCH] + fonts, scratch=SCRATCH)
    finished = subprocess.run(argv_ref, capture_output=True, text=True)
    if finished.returncode != 0:
        raise oracle_env.OracleUnavailable(
            "fontTools failed:\n%s" % finished.stderr[-2000:])
    answer = json.loads(finished.stdout.strip().splitlines()[-1])
    problems += answer["problems"]
    print("type1 written: %d programs in the image, %d written both ways, %d "
          "files read back by fontTools" % (len(everything), len(fonts),
                                            answer["checked"]))
    for line in problems[:(10 if quiet else 40)]:
        print("DISAGREE " + line)
    if problems or answer["checked"] != 2 * len(everything):
        print("type1 written: %d disagreement(s)" % len(problems))
        return 1
    print("type1 written: no disagreements")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
