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
"""Bring the image's fonts somewhere both sides of a differential can read them.

documentation/design.md section 14.5 keeps every third-party font out of this
repository: the real fonts live in the oracle image and nowhere else. But a
differential needs *both* readers to see the same bytes, and this library's
reader runs on the host.

So the corpus is materialised: one container run tars the image's fonts to
stdout and the host unpacks them under `build/oracle/corpus`, which `build/` is
already gitignored. They are cache, not content - `make oracle-corpus-clean`
removes them, and a fresh checkout has none until a differential asks.

Doing it in one `tar` rather than a `cp` per font is not only speed: a container
start per font is a chance per font for a partial copy, and the tar either
arrives whole or does not arrive.
"""

import os
import subprocess
import sys
import tarfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import oracle_env

ROOT = oracle_env.ROOT
CORPUS = os.path.join(ROOT, "build", "oracle", "corpus")

# Which list in the image to read. Each is a list rather than a glob here for
# the same reason: a denominator that is mostly files the differential skipped
# reports the same clean number whatever it covered. The bitmap corpus is several
# hundred PCF files, and the EBDT corpus is the **whole** Debian population of
# embedded bitmap strikes inside an sfnt - thirty-three fonts of the 506 sfnts in
# the image, which is the measurement `notes/font/EBLC.md` records and the reason
# that differential's report says what rests on fixtures instead.
#
# `sfnt` and `ebdt` are one program in the image with one argument between them,
# because they were two and disagreed: the EBDT list selected by sfnt magic while
# the sfnt list globbed `*.ttf`/`*.otf`/`*.ttc`/`*.otc`, so Debian's five
# bitmap-only `.otb` fonts were in one corpus and invisible to the other.
# `variable` is the sixteen sfnts with both `fvar` and `gvar`: a design space and
# the outlines that move through it. A font with the first and not the second -
# Cantarell, whose outlines are CFF2 - has nothing a variation differential can
# compare, and is in the `sfnt` list like every other font.
LISTS = {
    "sfnt": "fonttools-corpus",
    "bitmap": "fonttools-corpus-bitmap",
    "ebdt": "fonttools-corpus-ebdt",
    "variable": "fonttools-corpus-variable",
}


def fonts(kind="sfnt", refresh=False):
    """The corpus on the host, materialising it first if it is not there.

    Returns a list of absolute paths, sorted, so that two runs cover the same
    fonts in the same order and a diff of two reports is readable.
    """
    if kind not in LISTS:
        raise ValueError("no such corpus: %r" % kind)
    target = os.path.join(CORPUS, kind)
    stamp = os.path.join(target, ".complete")

    if refresh or not os.path.exists(stamp):
        _materialise(kind, target, stamp)

    found = []
    for base, _, names in os.walk(target):
        for name in names:
            if name == ".complete":
                continue
            found.append(os.path.join(base, name))
    return sorted(found)


def _materialise(kind, target, stamp):
    """Copy the image's fonts out, in one tar, and stamp it only when done."""
    if os.path.exists(stamp):
        os.remove(stamp)
    os.makedirs(target, exist_ok=True)

    # `tar` of the listed files only, from /usr/share/fonts, so the archive's
    # paths are relative and nothing outside the corpus comes along.
    script = (
        "cd /usr/share/fonts && %s | sed 's|^/usr/share/fonts/||' "
        "| tar -cf - -T -" % LISTS[kind])
    argv = oracle_env.command("fonttools", ["sh", "-c", script])
    with subprocess.Popen(argv, stdout=subprocess.PIPE) as running:
        with tarfile.open(fileobj=running.stdout, mode="r|") as archive:
            # filter="data" refuses absolute paths, symlinks out of the tree and
            # device nodes. The archive comes from an image this repository
            # builds, but a corpus extractor that trusts its input is the same
            # mistake as a font parser that trusts a table's offset.
            archive.extractall(path=target, filter="data")
        if running.wait() != 0:
            raise oracle_env.OracleUnavailable(
                "could not read the corpus out of the image")

    # The stamp is written last, so an interrupted extraction is repeated rather
    # than read as complete.
    with open(stamp, "w", encoding="utf-8") as handle:
        handle.write("%s corpus, from %s\n"
                     % (kind, oracle_env.provenance(["fonttools"])))


def main(argv):
    kind = argv[1] if len(argv) > 1 else "sfnt"
    refresh = "--refresh" in argv
    found = fonts(kind, refresh=refresh)
    print("%s corpus: %d fonts under %s" % (kind, len(found),
                                            os.path.join(CORPUS, kind)))
    return 0 if found else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
