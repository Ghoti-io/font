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
"""Plant a defect in the TrueType decoder and see both gates reject it.

A differential that has only ever passed has not been shown to be able to fail
(the standing rule for every gate here). Each mutant is one line of
src/hint/, and must be refused twice:

  * by the unit tests (testHint), and
  * by tt_decode_diff.py against fontTools, run over one small TrueType fixture
    (the opcode-table sweep alone is enough to see these).

The file is restored and the artifacts rebuilt after every mutant, whatever
happened.

Usage: tt_decode_mutation_check.py   (PREFIX and BUILD as for make)
"""

import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PREFIX = os.environ.get("PREFIX", "/home/corey/Documents/ghoti.io/.local")
BUILD = os.environ.get("BUILD", "linux/release")
MAKE_BUILD = BUILD.removeprefix("linux/")
APPS = ROOT / "build" / BUILD / "apps"
UNIT = APPS / "testHint"
DRIVER = APPS / "examples" / "font-program"
ARCHIVE = APPS / "libghoti.io-font-8.a"
FIXTURE = ROOT / "tests" / "data" / "fonts" / "woff2-triplets-source.ttf"
LD_PATH = f"{PREFIX}/lib/ghoti.io:{PREFIX}/lib"

# (file, needle, replacement, what it breaks)
MUTANTS = [
    ("src/hint/tt_decode.c", "      unit = 2;\n      // fall through\n    case GFNT_HINT_DATA_NPUSHB:",
     "      // fall through\n    case GFNT_HINT_DATA_NPUSHB:",
     "NPUSHW read as bytes"),
    ("src/hint/tt_ops.c", 'R(0x68, 0x6B, "ROUND", 2,', 'R(0x68, 0x6B, "ROUND", 1,',
     "ROUND's flag width"),
    ("src/hint/tt_ops.c", 'R(0xB8, 0xBF, "PUSHW", 0, 0, GFNT_HINT_VARIABLE, GFNT_HINT_DATA_PUSHW)',
     'R(0xB8, 0xBF, "PUSHW", 0, 0, GFNT_HINT_VARIABLE, GFNT_HINT_DATA_PUSHB)',
     "PUSHW read as bytes"),
]


def run(command, *, env=None, expect_failure=False):
    done = subprocess.run(command, cwd=ROOT, capture_output=True, text=True,
                          env={**os.environ, "LD_LIBRARY_PATH": LD_PATH, **(env or {})})
    if (done.returncode != 0) != expect_failure:
        want = "failure" if expect_failure else "success"
        raise RuntimeError(f"expected {want} from {' '.join(map(str, command))} "
                           f"(exit {done.returncode}):\n{(done.stdout + done.stderr)[-600:]}")


def rebuild():
    for app in (UNIT, DRIVER):
        run(["make", f"PREFIX={PREFIX}", f"BUILD={MAKE_BUILD}", str(app.relative_to(ROOT))])


def forget(source):
    obj = ROOT / "build" / BUILD / "objects" / "hint" / (Path(source).stem + ".o")
    for artifact in (obj, ARCHIVE, UNIT, DRIVER):
        artifact.unlink(missing_ok=True)


def main():
    try:
        for source, needle, replacement, what in MUTANTS:
            path = ROOT / source
            original = path.read_text()
            if original.count(needle) != 1:
                raise RuntimeError(f"{source}: the needle for '{what}' matches "
                                   f"{original.count(needle)} times, not once")
            try:
                path.write_text(original.replace(needle, replacement))
                forget(source)
                rebuild()
                run([str(UNIT), "--gtest_brief=1"], expect_failure=True)
                run(["python3", "tools/oracle/oracle_run.py", "fonttools", "--",
                     "python3", "tools/oracle/tt_decode_diff.py", "--quiet",
                     str(FIXTURE)], expect_failure=True)
            finally:
                path.write_text(original)
                forget(source)
                rebuild()
            print(f"tt_decode_mutation_check: rejected: {what}")
    except (OSError, RuntimeError) as error:
        print(f"tt_decode_mutation_check: {error}", file=sys.stderr)
        return 1
    print("tt_decode_mutation_check: all mutants rejected by the unit tests and the oracle")
    return 0


if __name__ == "__main__":
    sys.exit(main())
