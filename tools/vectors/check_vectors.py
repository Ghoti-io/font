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
"""`make check-vectors`: the committed vectors are what the generator emits.

The tables in `src/tables/post_names.h` and `src/name/mac_encodings.h` are
generated from the pinned `fonttools` image (design.md section 14: a vector comes
from an oracle and is never written from memory). This regenerates them into
`build/vectors/generated` and compares byte for byte.

Three gates cover these tables and none of them can skip:

  * **this one** catches a generator changed without regenerating, or a table
    edited by hand - wherever the image is present;
  * **testVectors** checks the compiled-in tables against the committed text in
    `tests/data/vectors/`, so a hand-edited header fails `make test` on a fresh
    clone with no container at all;
  * **ttx_diff** checks the *use*: every `post` glyph name and every Macintosh
    `name` record of 344 faces, against the reference.

The third is the only one that can find the tables wrong about reality, and the
first two are the only ones that can find them edited. Saying which gate covers
what is the point: this file's own agreement with the generator proves nothing
about whether the generator is right.

Like `check_fixtures.py`, it **fails rather than skipping** when the image is
unreachable, and for the same reason - it compares this repository's own bytes
and has nothing to report if it cannot regenerate them.
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
GENERATED = os.path.join(ROOT, "build", "vectors", "generated")
GENERATOR = os.path.join("tools", "vectors", "make_vectors.py")

# Every file the generator writes, relative to the repository root.
#
# Spelled here as well as there on purpose, and checked both ways below: a
# generator that starts writing a fourth file fails this gate until someone
# decides whether the new file should be committed, rather than the new file
# being silently uncompared. The reverse - a file listed here that the generator
# stopped writing - fails too.
EXPECTED = [
    os.path.join("src", "tables", "post_names.h"),
    os.path.join("src", "name", "mac_encodings.h"),
    os.path.join("tests", "data", "vectors", "standard_glyph_order.txt"),
    os.path.join("tests", "data", "vectors", "mac_encodings.txt"),
    os.path.join("tests", "data", "vectors", "mac_selector.txt"),
]


def read(path):
    with open(path, "rb") as handle:
        return handle.read()


def same(left, right):
    """Whether two files' contents are identical. Called by the control too."""
    return read(left) == read(right)


def generate(out_dir):
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
            "the vector generator failed:\n%s%s"
            % (finished.stdout, finished.stderr))
    found = []
    for base, _, names in os.walk(out_dir):
        for name in names:
            found.append(os.path.relpath(os.path.join(base, name), out_dir))
    return sorted(found), finished.stdout


def control(generated_dir, relative):
    """Prove `same()` can still say no, on the file this gate exists for."""
    source = os.path.join(generated_dir, relative)
    planted = source + ".control"
    data = bytearray(read(source))
    middle = len(data) // 2
    data[middle] ^= 0xFF
    with open(planted, "wb") as handle:
        handle.write(bytes(data))
    try:
        if same(source, planted):
            return ["THE CONTROL WAS NOT DETECTED: same() called %s identical "
                    "to a copy with the byte at offset %d flipped, so this "
                    "gate cannot see an edited table" % (relative, middle)]
    finally:
        os.remove(planted)
    return []


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--quiet", action="store_true",
        help="print one line on success")
    parser.add_argument("--update", action="store_true",
        help="regenerate and install, rather than checking; `make gen-vectors`")
    args = parser.parse_args(argv)

    try:
        provenance = oracle_env.provenance(["fonttools"])
        generated, said = generate(GENERATED)
    except GeneratorRefused as refused:
        sys.stderr.write("check-vectors: %s\n" % refused)
        return 1
    except oracle_env.OracleUnavailable as unavailable:
        sys.stderr.write(
            "check-vectors: %s\n"
            "check-vectors: this is a failure rather than a skip - the tables "
            "are generated and there is nothing to report without the "
            "generator. testVectors still checks them against the committed "
            "text in tests/data/vectors/.\n" % unavailable)
        return 1

    problems = []
    for name in sorted(set(generated) - set(EXPECTED)):
        problems.append(
            "the generator writes %s, which EXPECTED in this script does not "
            "list; decide whether it should be committed" % name)
    for name in sorted(set(EXPECTED) - set(generated)):
        problems.append(
            "EXPECTED lists %s and the generator does not write it" % name)

    if args.update:
        if problems:
            for problem in problems:
                sys.stderr.write("gen-vectors: %s\n" % problem)
            return 1
        for name in EXPECTED:
            target = os.path.join(ROOT, name)
            os.makedirs(os.path.dirname(target), exist_ok=True)
            shutil.copyfile(os.path.join(GENERATED, name), target)
        print(provenance)
        sys.stdout.write(said)
        print("gen-vectors: installed %d file(s). Read the diff." % len(EXPECTED))
        return 0

    compared = 0
    total = 0
    for name in EXPECTED:
        if name in problems:
            continue
        theirs = os.path.join(GENERATED, name)
        ours = os.path.join(ROOT, name)
        if not os.path.exists(theirs):
            continue
        if not os.path.exists(ours):
            problems.append("%s is generated and not committed; run "
                            "`make gen-vectors`" % name)
            continue
        compared += 1
        total += os.path.getsize(ours)
        if not same(ours, theirs):
            problems.append(
                "%s differs from a fresh generation; run `make gen-vectors` "
                "and read the diff" % name)

    if compared:
        problems += control(GENERATED, EXPECTED[0])
    else:
        problems.append("nothing was compared, so a clean result would mean "
                        "nothing")

    for problem in problems:
        sys.stderr.write("check-vectors: %s\n" % problem)
    if problems:
        return 1

    print(provenance)
    print("check-vectors: %d files, %d bytes, identical to a fresh generation, "
          "control passed" % (compared, total))
    return 0


if __name__ == "__main__":
    sys.exit(main())
