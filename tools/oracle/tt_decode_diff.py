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
"""TrueType programs, decoded: this library against fontTools.

documentation/design.md section 8.5. The interpreter (not built yet) is only as
good as its decoder, and a decoder that reads one instruction's length wrongly
puts every later byte of the program out of step, so this compares the two
decoders over everything the corpus holds:

  * **the opcode table**: one program holding every opcode value 0 to 255 with
    zero operands, which fixes each opcode's mnemonic, how many low bits are
    flags and how much data follows;
  * `fpgm`, `prep` and every glyph's program in every TrueType font of the
    corpus and the fixtures, instruction by instruction and push value by push
    value.

An 'NPUSHB' or 'NPUSHW' of zero values is legal and fontTools asserts on it, so
the sweep gives each one value; the empty push is in this library's unit tests.

fontTools does not name 0x92 (`GETDATA`, which the specification lists and
FreeType implements); it prints `INSTR146`. That one spelling is accepted and
counted. A program fontTools cannot read at all is counted, not compared.

Usage:
    tt_decode_diff.py [--quiet] [font...]
"""

import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import corpus
import oracle_env
import unskippable

ROOT = oracle_env.ROOT
DRIVER = os.path.join(ROOT, "build", "linux", "release", "apps", "examples",
                      "font-program")
REFERENCE = os.path.join(ROOT, "tools", "oracle", "fonttools_programs.py")
FIXTURES = os.path.join(ROOT, "tests", "data", "fonts")

# fontTools has no name for 0x92.
SPELLINGS = {"INSTR146[ ]": "GETDATA[ ]"}


def opcode_program():
    """Every opcode once, with the zero operands it needs."""
    out = bytearray()
    for op in range(256):
        out.append(op)
        if op == 0x40:
            out.extend((1, 0))
        elif op == 0x41:
            out.extend((1, 0, 0))
        elif 0xB0 <= op <= 0xB7:
            out.extend(bytes(op - 0xB0 + 1))
        elif 0xB8 <= op <= 0xBF:
            out.extend(bytes(2 * (op - 0xB8 + 1)))
    return out.hex()


def ours(args):
    finished = subprocess.run([DRIVER] + args, capture_output=True, text=True)
    return finished.returncode, finished.stdout, finished.stderr


def reference(args):
    argv = oracle_env.command("fonttools", ["python3", REFERENCE] + args)
    finished = subprocess.run(argv, capture_output=True, text=True)
    if finished.returncode != 0:
        raise oracle_env.OracleUnavailable(
            "fontTools: %s" % finished.stderr.strip()[-300:])
    return finished.stdout


def sections(text):
    """{heading: [lines]} for one font's output; headings keep their order."""
    found = {}
    heading = None
    for line in text.splitlines():
        if line.startswith("== "):
            heading = line[3:]
            found[heading] = []
        elif heading is not None and not line.startswith("!! "):
            found[heading].append(SPELLINGS.get(line, line))
    return found


def problems(text):
    return [line for line in text.splitlines() if line.startswith("!! ")]


def first_difference(mine, theirs):
    for at in range(max(len(mine), len(theirs))):
        a = mine[at] if at < len(mine) else "<end>"
        b = theirs[at] if at < len(theirs) else "<end>"
        if a != b:
            return at, a, b
    return None


def split_fonts(text):
    """{path: output} from a reference run over several fonts."""
    found = {}
    path = None
    for line in text.splitlines():
        if line.startswith("### "):
            path = line[4:]
            found[path] = []
        elif path is not None:
            found[path].append(line)
    return {key: "\n".join(value) for key, value in found.items()}


def fixtures():
    manifest = os.path.join(FIXTURES, "MANIFEST")
    found = []
    if os.path.exists(manifest):
        with open(manifest, encoding="utf-8") as handle:
            for line in handle:
                if line.startswith("#") or not line.strip():
                    continue
                name = line.split("\t")[0]
                if not name.endswith(".ttc"):
                    found.append(os.path.join(FIXTURES, name))
    return found


def main(argv):
    quiet = "--quiet" in argv
    paths = [a for a in argv[1:] if not a.startswith("--")]
    failures = 0

    # The opcode table.
    code = opcode_program()
    status, text, err = ours(["--hex", code])
    if status != 0:
        print("opcode table: the driver failed: %s" % err.strip())
        return 1
    mine = sections(text)["hex"]
    theirs = sections(reference(["--hex", code]))["hex"]
    where = first_difference(mine, theirs)
    if where:
        print("opcode table: line %d: ours %r, fontTools %r" % where)
        failures += 1
    print("opcode table: %d lines compared" % len(mine))

    if not paths:
        paths = [p for p in corpus.fonts("sfnt")] + fixtures()
    paths = [p for p in paths if os.path.exists(p)]
    their_text = split_fonts(reference(paths))

    programs = 0
    fonts = 0
    unreadable = 0
    declined = 0
    structural = []
    dropped = []
    for path in paths:
        status, text, err = ours([path])
        theirs_all = their_text.get(path, "")
        if status != 0 and "Not a format" in err or "Unsupported" in err:
            continue
        if status != 0:
            dropped.append((path, "the driver failed: %s" % err.strip()[-120:]))
            continue
        if theirs_all.startswith("?? font"):
            dropped.append((path, "fontTools could not open it"))
            continue
        mine = sections(text)
        theirs = sections(theirs_all)
        if not mine and not theirs:
            continue
        fonts += 1
        structural.extend((path, line) for line in problems(text))
        for heading in sorted(set(mine) | set(theirs)):
            if heading not in mine and theirs[heading][:1] and \
                    theirs[heading][0].startswith("?? "):
                # fontTools failed on a glyph this library finds no program in
                # (the bitmap-only sfnt's empty glyf, for one).
                unreadable += 1
                continue
            if heading not in theirs and mine[heading][:1] and \
                    mine[heading][0].startswith("?? "):
                declined += 1       # a glyph this library declines by name
                continue
            if heading not in mine or heading not in theirs:
                print("%s: %s is only in %s" % (os.path.basename(path), heading,
                      "ours" if heading in mine else "fontTools"))
                failures += 1
                continue
            if mine[heading][:1] in (["?? Unsupported feature"],
                                     ["?? Corrupt data"]):
                # A glyph this library declines by name (the cubic extension,
                # a loca entry out of bounds) has no program to read.
                declined += 1
                continue
            if theirs[heading] and theirs[heading][0].startswith("?? "):
                unreadable += 1
                continue
            programs += 1
            where = first_difference(mine[heading], theirs[heading])
            if where:
                failures += 1
                if failures <= 20:
                    print("%s: %s: line %d: ours %r, fontTools %r"
                          % ((os.path.basename(path), heading) + where))

    if unskippable.check("tt_decode_diff", dropped):
        failures += 1
    print("%d fonts, %d programs compared, %d fontTools could not read, "
          "%d declined by this library by name, %d structural findings, "
          "%d disagreement(s)"
          % (fonts, programs, unreadable, declined, len(structural), failures))
    if not quiet:
        for path, line in structural[:30]:
            print("  structural: %s: %s" % (os.path.basename(path), line))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
