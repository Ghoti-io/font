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
"""WOFF 2 files somebody else wrote: the W3C suite, IBM Plex and Source Code Pro.

documentation/design.md section 7.1. `woff2_diff.py` wraps this repository's corpus
with fontTools' writer; this reads what other encoders made - the reference
`woff2_compress` behind the Plex and Source Code Pro web fonts, and the W3C test
suite's generators, whose cases include files the specification says a decoder must
refuse. The fonts are in the oracle image (`fonttools-fetch-woff2`), none in this
repository.

For each file the question is the same two-sided one: does this library accept it,
does fontTools, and when both do, do they read the same font. For the W3C format
cases the suite's own verdict is in its index page and is printed beside any
disagreement, because "invalid" there means invalid for a validator, and a decoder
is not obliged to refuse everything a validator would.

Usage:
    woff2_real_diff.py [--quiet]
"""

import collections
import json
import os
import re
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import corpus
import oracle_env

ROOT = oracle_env.ROOT
EXAMPLES = os.path.join(ROOT, "build", "linux", "release", "apps", "examples")
SCRATCH = os.path.join(ROOT, "build", "oracle", "woff2-real")
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


def verdicts(everything):
    """{file basename: suite verdict} from the W3C format suite's index page."""
    for path in everything:
        if path.endswith("/w3c-format/testcaseindex.xht"):
            text = open(path, encoding="utf-8").read()
            found = {}
            for name, body in re.findall(
                    r'<div class="testCase" id="([^"]+)">(.*?)</div>\s*</div>',
                    text, re.S):
                m = re.search(r'Valid: <span[^>]*>(\w+)', body)
                if m:
                    found[name + ".woff2"] = m.group(1)
            return found
    return {}


def main(argv):
    quiet = "--quiet" in argv
    oracle_env.check_pin("fonttools")
    print(oracle_env.provenance(["fonttools"]))
    everything = corpus.fonts("woff2", refresh="--refresh" in argv)
    files = [f for f in everything if f.endswith(".woff2")]
    suite = verdicts(everything)

    shutil.rmtree(SCRATCH, ignore_errors=True)
    os.makedirs(SCRATCH)
    reference("read", files)
    driver = os.path.join(EXAMPLES, "font-tables")
    crashed = []
    for path in files:
        ours = os.path.join(SCRATCH, key(path) + ".ours")
        os.makedirs(ours)
        finished = subprocess.run([driver, path, ours], capture_output=True,
                                  text=True)
        if finished.returncode != 0:
            shutil.rmtree(ours)
            if finished.returncode < 0 or finished.returncode > 1:
                crashed.append("%s: exit %d" % (path, finished.returncode))
    judged = reference("judge", files)

    # Files that come with their source sfnt are read face by face, so a collection
    # is checked too: fontTools cannot read one, but the suite supplies the answer.
    paired = [f for f in files if os.path.exists(f[:-len(".woff2")] + ".ttf")]
    for path in paired:
        face = 0
        while True:
            ours = "%s.ours.%d" % (os.path.join(SCRATCH, key(path)), face)
            os.makedirs(ours)
            finished = subprocess.run([driver, path, ours, str(face)],
                                      capture_output=True, text=True)
            if finished.returncode != 0:
                shutil.rmtree(ours)
                break
            face += 1
    round_trips = reference("roundtrip", paired) if paired else {}

    tally = {}
    problems = []
    explained = collections.Counter()
    for path in files:
        outcome, why = judged[path]
        group = os.path.basename(os.path.dirname(path))
        tally.setdefault(group, {}).setdefault(outcome, 0)
        tally[group][outcome] += 1
        verdict = suite.get(os.path.basename(path))
        name = os.path.basename(path)
        if outcome == "differ":
            if "this library rejects" in why and verdict == "No":
                explained["stricter than fontTools, as the suite is"] += 1
            elif ("this library accepts" in why and verdict == "No"
                    and name.startswith("metadata-")):
                explained["invalid only in a metadata block, which is not read"] += 1
            elif (path in round_trips and not round_trips[path]
                    and "fontTools rejects" in why):
                explained["a collection fontTools cannot read, and which "
                          "round-trips to the suite's sfnt here"] += 1
            else:
                problems.append("%s: %s%s" % (
                    os.path.relpath(path, corpus.CORPUS), why,
                    " (suite says valid: %s)" % verdict if verdict else ""))
        elif group == "w3c-format" and verdict == "Yes" and outcome != "agree":
            problems.append("%s: the suite says this file is valid and it was "
                            "not read" % os.path.basename(path))
    for path, bad in sorted(round_trips.items()):
        for line in bad:
            problems.append("%s: %s" % (os.path.relpath(path, corpus.CORPUS), line))
    print("woff2 (real): %d files, %d of them with the sfnt they were made from"
          % (len(files), len(paired)))
    for group in sorted(tally):
        print("  %-20s %s" % (group, ", ".join(
            "%d %s" % (n, o) for o, n in sorted(tally[group].items()))))
    for reason, count in sorted(explained.items()):
        print("  explained, %d: %s" % (count, reason))
    for line in crashed:
        print("CRASH " + line)
    for line in problems[:(10 if quiet else 60)]:
        print("DISAGREE " + line)
    if len(problems) > 60:
        print("... and %d more" % (len(problems) - 60))
    return 1 if (problems or crashed) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
