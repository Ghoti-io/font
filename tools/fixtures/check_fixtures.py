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
"""`make check-fixtures`: the committed fixtures are what the generator writes.

Regenerates `tests/data/fonts/` into `build/fixtures/generated` inside the
pinned `fonttools` image and compares byte for byte. A fixture is only useful
if it can be rebuilt: an edited font nobody can regenerate is a byte array with
extra steps, and a generator nobody runs is documentation.

**Two checks, not one, because they need different things.**

  1. *The set* - every committed fixture is listed in MANIFEST, every listed
     fixture is committed, and nothing else is in the directory. This needs no
     container and always runs. It is what catches a fixture added by hand, a
     fixture deleted without updating the generator, and the `.ttx` someone left
     behind while debugging.
  2. *The bytes* - each committed fixture equals a fresh generation. This needs
     the image, because the bytes are whatever fontTools 4.66.0 wrote and the
     host's fontTools (if it even has one) would answer a different question.

Without a container engine the first check still runs, and the gate then
**fails** saying how many files went unverified. That is a deliberate departure
from every `check-oracle-*` target, which skips loudly instead: a differential
compares against an outside population a contributor may not have, and
`make test` has to pass without it. This gate compares this repository's own
committed bytes, is not in `TEST_GATES`, and is run by someone asking exactly
that question - so a zero exit it did not earn would be a lie, and there is no
useful unrequired mode to offer.

**The comparison carries its own control.** `tools/check-reader.py` runs every
pattern against a planted violation in the same invocation, because a gate that
can only be trusted because someone once watched it fail has sensitivity a year
out of date. The same reasoning applies harder here: this gate's entire content
is one `==`, and an `==` that has rotted into comparing a file with itself
reports every fixture clean. So after the real comparison, one fixture's bytes
are mutated in memory and fed back through the same function, which must report
a difference or the gate fails.

Updating a fixture is a separate, deliberate act: `make fixtures` regenerates
and installs, and the diff it produces belongs in a commit that says why the
bytes moved.
"""

import argparse
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "oracle"))
import oracle_env


class GeneratorRefused(Exception):
    """The generator ran and said no. Distinct from an unreachable image."""

ROOT = oracle_env.ROOT
COMMITTED = os.path.join(ROOT, "tests", "data", "fonts")
GENERATED = os.path.join(ROOT, "build", "fixtures", "generated")
GENERATOR = os.path.join("tools", "fixtures", "make_fixtures.py")

MANIFEST = "MANIFEST"


def read(path):
    with open(path, "rb") as handle:
        return handle.read()


def same(left, right):
    """Whether two files' contents are identical.

    Its own function so that the control below can call exactly what the real
    comparison calls, rather than a second spelling of it that could agree with
    the first while both are wrong.
    """
    return read(left) == read(right)


def manifest_names(path):
    """The filenames MANIFEST lists, in the order it lists them."""
    names = []
    with open(path, "r", encoding="utf-8") as handle:
        for line in handle:
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            names.append(line.rstrip("\n").split("\t")[0])
    return names


def generate(out_dir):
    """Run the generator in its image, into a directory it may write."""
    if os.path.exists(out_dir):
        shutil.rmtree(out_dir)
    os.makedirs(out_dir)
    argv = oracle_env.command("fonttools",
        ["python3", GENERATOR, "--out", out_dir], scratch=out_dir)
    finished = subprocess.run(argv, capture_output=True, text=True)
    if finished.returncode != 0:
        # Not OracleUnavailable: the image was reached and the generator ran and
        # refused. Reporting that as "cannot reach the reference" sends the
        # reader to their container setup when the actual message - a cross-check
        # that failed, a vector whose shape changed - is right there.
        raise GeneratorRefused(
            "the fixture generator failed:\n%s%s"
            % (finished.stdout, finished.stderr))
    return sorted(n for n in os.listdir(out_dir))


def check_set(committed):
    """The directory and MANIFEST agree, and nothing strays. No container."""
    problems = []
    if not os.path.isdir(committed):
        return ["%s does not exist; run `make fixtures`" % committed]

    present = sorted(n for n in os.listdir(committed)
                     if not n.startswith("."))
    if MANIFEST not in present:
        return ["%s has no %s" % (committed, MANIFEST)]

    listed = manifest_names(os.path.join(committed, MANIFEST))
    fonts = [n for n in present if n != MANIFEST]

    for name in sorted(set(listed) - set(fonts)):
        problems.append("%s lists %s, which is not committed" % (MANIFEST, name))
    for name in sorted(set(fonts) - set(listed)):
        problems.append(
            "%s is in tests/data/fonts/ and not in %s - a fixture added by "
            "hand, or a stray file" % (name, MANIFEST))
    if listed != sorted(listed):
        problems.append("%s is not in sorted order" % MANIFEST)
    return problems


def check_bytes(committed, generated_dir, generated):
    """Each committed fixture equals a fresh generation. Needs the image."""
    problems = []
    compared = 0
    total_bytes = 0

    for name in generated:
        theirs = os.path.join(generated_dir, name)
        ours = os.path.join(committed, name)
        if not os.path.exists(ours):
            problems.append(
                "%s is generated and not committed; run `make fixtures`" % name)
            continue
        compared += 1
        total_bytes += os.path.getsize(ours)
        if not same(ours, theirs):
            problems.append(
                "%s differs from a fresh generation (committed %d bytes, "
                "generated %d bytes)"
                % (name, os.path.getsize(ours), os.path.getsize(theirs)))

    for name in sorted(set(os.listdir(committed)) - set(generated)):
        if name.startswith("."):
            continue
        problems.append(
            "%s is committed and the generator does not write it" % name)

    return problems, compared, total_bytes


def control(committed, generated_dir, generated):
    """Prove `same()` can still say no.

    Copies one generated fixture, flips a byte in the middle of it, and requires
    the comparison to report a difference. A byte in the middle rather than the
    first or last, because a comparison truncated to a header or to a length
    would still catch either end.
    """
    subject = None
    for name in generated:
        if name != MANIFEST:
            subject = name
            break
    if subject is None:
        return ["no fixture to run the control against"]

    source = os.path.join(generated_dir, subject)
    planted = source + ".control"
    data = bytearray(read(source))
    middle = len(data) // 2
    data[middle] ^= 0xFF
    with open(planted, "wb") as handle:
        handle.write(bytes(data))
    try:
        if same(source, planted):
            return ["THE CONTROL WAS NOT DETECTED: same() called %s identical "
                    "to a copy of itself with the byte at offset %d flipped. "
                    "This gate cannot see a changed fixture and its clean "
                    "results mean nothing." % (subject, middle)]
        if not same(source, os.path.join(committed, subject)):
            return ["the control's subject (%s) does not match its committed "
                    "copy, so the control proves nothing" % subject]
    finally:
        os.remove(planted)
    return []


def install(committed, generated_dir, generated):
    """Copy a fresh generation over the committed fixtures.

    Separate from the check, and never reached by it, because a gate that
    repairs what it finds wrong reports clean forever. What lands here lands in
    a commit whose message says why the bytes moved.
    """
    os.makedirs(committed, exist_ok=True)
    stale = sorted(set(n for n in os.listdir(committed)
                       if not n.startswith(".")) - set(generated))
    for name in generated:
        shutil.copyfile(os.path.join(generated_dir, name),
            os.path.join(committed, name))
    for name in stale:
        os.remove(os.path.join(committed, name))
    return generated, stale


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--quiet", action="store_true",
        help="print one line on success")
    parser.add_argument("--update", action="store_true",
        help="regenerate and install into tests/data/fonts, rather than "
             "checking; this is `make fixtures` and it is never what a gate "
             "does")
    args = parser.parse_args(argv)

    if args.update:
        try:
            provenance = oracle_env.provenance(["fonttools"])
            generated = generate(GENERATED)
        except (GeneratorRefused, oracle_env.OracleUnavailable) as why:
            sys.stderr.write("fixtures: %s\n" % why)
            return 1
        written, removed = install(COMMITTED, GENERATED, generated)
        print(provenance)
        print("fixtures: wrote %d file(s) to tests/data/fonts%s"
              % (len(written),
                 (", removed %d no longer generated: %s"
                  % (len(removed), " ".join(removed))) if removed else ""))
        return 0

    problems = check_set(COMMITTED)

    compared = 0
    total_bytes = 0
    provenance = None
    try:
        provenance = oracle_env.provenance(["fonttools"])
        generated = generate(GENERATED)
        byte_problems, compared, total_bytes = check_bytes(
            COMMITTED, GENERATED, generated)
        problems += byte_problems
        problems += control(COMMITTED, GENERATED, generated)
    except GeneratorRefused as refused:
        for problem in problems:
            sys.stderr.write("check-fixtures: %s\n" % problem)
        sys.stderr.write("check-fixtures: %s\n" % refused)
        return 1
    except oracle_env.OracleUnavailable as unavailable:
        # Fails, rather than skipping, and unlike every check-oracle-* target.
        # The difference is what is being checked. A differential compares
        # against an outside population a contributor may legitimately not
        # have, and `make test` must pass without it. This gate checks this
        # repository's *own committed bytes*, it is not in TEST_GATES, and
        # nobody runs it except to ask that question - so there is no reading
        # of a zero exit here that is not "the fixtures were verified", which
        # would be false. GHOTI_ORACLE_REQUIRED is not consulted for the same
        # reason: there is no useful unrequired mode.
        count = 0
        if os.path.isdir(COMMITTED):
            count = len([n for n in os.listdir(COMMITTED)
                         if not n.startswith(".")])
        for problem in problems:
            sys.stderr.write("check-fixtures: %s\n" % problem)
        sys.stderr.write(
            "check-fixtures: SKIPPED the byte comparison - %s\n"
            "check-fixtures: the set check ran and the bytes of %d committed "
            "file(s) are unverified, which is a failure rather than a skip: "
            "this gate has nothing to report if it cannot regenerate them.\n"
            % (unavailable, count))
        return 1

    # A denominator, reported rather than assumed. Nothing compared is the
    # failure mode a byte-identity gate is most likely to have: an empty
    # directory compares clean against an empty generation.
    if compared == 0:
        problems.append(
            "nothing was compared: the generator wrote no fixture that is "
            "also committed, so a clean result would mean nothing")

    for problem in problems:
        sys.stderr.write("check-fixtures: %s\n" % problem)
    if problems:
        return 1

    print(provenance)
    print("check-fixtures: %d fixtures, %d bytes, byte-identical to a fresh "
          "generation, control passed" % (compared, total_bytes))
    return 0


if __name__ == "__main__":
    sys.exit(main())
