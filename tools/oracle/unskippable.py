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
"""The gate: a differential may not drop an sfnt because something failed.

Every differential here catches a `Skip` per font, prints it, and carries on.
That is right for a *stated* reason - this font has no `CFF `, that one has no
`glyf` - and it is how a finding disappears when the reason is "the reference
exited non-zero". The font leaves the denominator, one line joins thirty-five
others, and the total is as clean as if the font had agreed.

**The clause was invisible for as long as the population deserved it.** Before
the sfnt corpus stopped selecting by file extension, every font `glyf_diff` had
ever excluded that way was a *fixture that is not an sfnt at all* - a PCF, a BDF,
a PSF, a `.hex`, a Type 1 program, a bare CFF - so the clause had never once been
wrong, and read as an obviously sensible guard. The five `.otb` fonts that arrived
with the corpus change were the first real fonts it swallowed, and they were
hiding a defect in this library. `ttx_diff` was dropping `mona.ttf` the same way,
for a short `OS/2` that fontTools raises on, which took that font's other 7,300
fields out of the gate with it.

So the rule here: **an unexplained skip of a file the corpus selector calls an
sfnt fails the run.** Three things about its shape matter.

  * *The corpus selector decides what an sfnt is, not this file.* A real font is
    an sfnt because `fonttools-corpus` put it in the corpus; a fixture is one
    because that same program, run over `tests/data/fonts`, says so. There is no
    second magic test here to drift from the one in the image - which is the
    mistake that produced two disagreeing corpus lists in the first place.

  * *An expected skip is still a skip and is still counted.* This gate is about
    the ones a differential meant to compare. Which are which is the
    differential's own `expected` flag, because only it knows that "no `CFF `
    table" is a fact and "AssertionError" is not.

  * *A declaration is an assertion in both directions.* A fixture built for the
    reference to choke on - `outline-broken-loca.ttf` is the one - is named here
    with the reason, and if it ever stops being skipped this gate fails too and
    says to remove the entry. An exemption that cannot expire is how a gate goes
    quiet; `eblc_diff.EXPECTED_DECLINED` is the same shape.
"""

import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import corpus
import oracle_env

FIXTURES = os.path.join(oracle_env.ROOT, "tests", "data", "fonts")

# Files that are sfnts and that a reference is **expected** to fail on, by the
# differential that meets them. Keyed by basename, with the reason, and asserted
# in both directions - a name here that is not skipped fails the run.
#
# One entry, and it is a fixture whose whole purpose is to be unreadable: `loca`
# names a glyph past the end of `glyf`, so fontTools raises `not enough 'glyf'
# table data` and this library refuses that glyph per glyph (M11). The fixture
# exists to assert that difference, which means the reference failing on it is
# the fixture working.
EXPECTED = {
    "glyf_diff": {
        "outline-broken-loca.ttf":
            "loca names a glyph past the end of glyf, which is what the fixture "
            "is for - fontTools refuses the whole font and this library refuses "
            "that glyph",
    },
}

_sfnt_fixtures = None


def sfnt_fixtures():
    """Which files under tests/data/fonts the image's selector calls sfnts.

    One container run, memoised, because every differential asks. Asked of the
    image rather than tested here for the reason in the module docstring: the
    magic test has one implementation and it is not this one.
    """
    global _sfnt_fixtures
    if _sfnt_fixtures is None:
        argv = oracle_env.command("fonttools", ["fonttools-corpus", FIXTURES])
        finished = subprocess.run(argv, capture_output=True, text=True)
        if finished.returncode != 0:
            raise oracle_env.OracleUnavailable(
                "could not ask the image which fixtures are sfnts: %s"
                % finished.stderr.strip())
        _sfnt_fixtures = {line for line in finished.stdout.split("\n") if line}
    return _sfnt_fixtures


def is_sfnt(path):
    """Whether the corpus selector calls this path an sfnt.

    True for anything in any corpus it built - a path is in one because the
    selector chose it - and for a fixture the same selector names.
    """
    real = os.path.realpath(path)
    if real in sfnt_fixtures():
        return True
    for kind in ("sfnt", "ebdt"):
        try:
            if real in {os.path.realpath(one) for one in corpus.fonts(kind)}:
                return True
        except Exception:  # noqa: BLE001 - a corpus that is not there is not sfnt
            continue
    return False


def check(name, unexplained, out=sys.stderr):
    """Fail the run if an sfnt was dropped for an unexplained reason.

    @p unexplained is [(path, why)] for the skips the differential did *not* call
    expected. Returns True when the run must fail, having said why.

    Both directions. A file this gate expects to be skipped and that was not is
    also a failure: either the reference got better, in which case the entry
    comes out and the font joins the comparison, or the differential stopped
    reaching that font at all, which is the thing this whole file is about.
    """
    expected = EXPECTED.get(name, {})
    failed = False
    seen = set()

    for path, why in unexplained:
        base = os.path.basename(path)
        if not is_sfnt(path):
            # Not an sfnt, so this differential was never going to read it: the
            # bitmap and Type 1 fixtures reach the sfnt gates because the fixture
            # list is every file in the directory, which is deliberate - a
            # fixture joins a differential by existing.
            continue
        seen.add(base)
        if base in expected:
            continue
        failed = True
        out.write("%s: %s is an sfnt and was dropped for an unexplained "
                  "reason: %s\n" % (name, base, why))
        out.write("%s: a font the corpus selector calls an sfnt must be "
                  "compared or declared. If the reference genuinely cannot read "
                  "it, say so per *table* or per *glyph* the way the adapters "
                  "do, so the rest of the font is still compared; if the file is "
                  "meant to be unreadable, add it to EXPECTED in "
                  "tools/oracle/unskippable.py with the reason.\n" % name)

    for base, why in sorted(expected.items()):
        if base not in seen:
            failed = True
            out.write("%s: %s is declared unreadable by the reference (%s) and "
                      "was not skipped. Either the reference can read it now - "
                      "remove the entry from EXPECTED in "
                      "tools/oracle/unskippable.py and let it be compared - or "
                      "this differential has stopped reaching the font at "
                      "all.\n" % (name, base, why))
    return failed
