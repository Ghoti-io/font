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
"""Plant a defect in a point instruction and see FreeType's comparison refuse it.

A differential that has only ever passed has not been shown able to fail. Each
mutant is one change to src/hint/ that makes an instruction mean something a
little different, and each must be refused by hint_probe_diff.py (random programs) or
hint_batteries.py (targeted ones), both compared with FreeType, and not merely
by this library's own unit tests, because the claim being tested is the comparison's.

The file is restored and the example rebuilt after every mutant.

Usage: hint_mutation_check.py   (PREFIX and BUILD as for make)
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
DRIVER = APPS / "examples" / "font-hint"
ARCHIVE = APPS / "libghoti.io-font-0.a"

MUTANTS = [
    ("src/hint/tt_move.c", "bool behind = org_dist < 0;", "bool behind = false;",
     "MDRP's minimum distance forgets which side the point was on"),
    ("src/hint/tt_move.c", "if (19 * cross <= along) {", "if (21 * cross <= along) {",
     "ISECT's cutoff for a grazing angle"),
    ("src/hint/tt_move.c", "      z = ZONE(2);\n      limit = gs->zp[2] == 0 ?",
     "      z = &run->zone[e];\n      limit = e == 0 ?",
     "SHZ moves the zone its operand names instead of zp2's"),
    ("src/hint/tt_move.c", "if (org <= o1) {", "if (units <= u1) {",
     "IUP decides which side of the touched pair a point is on in font units"),
    ("src/hint/tt_move.c", "if (f > -0x400 && f < 0x400) {\n    f = 0;\n  }\n  if (f == 0 || gfnt_hint_frozen(run)) {",
     "if (f > -0x100 && f < 0x100) {\n    f = 0;\n  }\n  if (f == 0 || gfnt_hint_frozen(run)) {",
     "the cutoff for a freedom vector nearly at right angles to the projection vector"),
    ("src/hint/tt_prog.c", "z->cur[n - 3u].x = gfnt_hint_pixel(z->org[n - 3u].x);",
     "z->cur[n - 3u].x = z->org[n - 3u].x;",
     "the advance phantom point is not placed on the grid"),
    ("src/hint/glyph.c", "if (!gfnt_hint_ctx_compat(ctx)) dx = gfnt_hint_grid(dx);",
     "if (!gfnt_hint_ctx_compat(ctx)) dx = dx;",
     "a composite's x offset is not rounded when the flag asks for it"),
    ("src/hint/glyph.c", "        dy = gfnt_hint_grid(dy);", "        dy = dy;",
     "a composite's y offset is not rounded when the flag asks for it"),
    ("src/hint/glyph.c", "if (c->flags & GFNT_GLYF_USE_MY_METRICS) {",
     "if (false && (c->flags & GFNT_GLYF_USE_MY_METRICS)) {",
     "a composite ignores USE_MY_METRICS"),
    ("src/hint/tt_move.c", "        run->depth -= (size_t)gs->loop;\n        gs->loop = 1;\n        break;\n      }\n      if (rp2 >= z1->count) {",
     "        gs->loop = 1;\n        break;\n      }\n      if (rp2 >= z1->count) {",
     "IP with no first reference point leaves its points on the stack"),
    ("src/hint/tt_move.c", "        run->depth -= (size_t)gs->loop;      /* the points are taken and nothing is moved */",
     "        /* left */",
     "SHP with no reference point leaves its points on the stack"),
    ("src/hint/tt_move.c", "          gs->rp[2] = (uint32_t)p & 0xFFFFu;\n          if (op & 0x10) gs->rp[0] = (uint32_t)p & 0xFFFFu;\n          break;\n        }\n        org_dist = gfnt_hint_original_distance",
     "          break;\n        }\n        org_dist = gfnt_hint_original_distance",
     "an MDRP of a point that is not there leaves the reference points alone"),
    ("src/hint/tt_exec.c", "          if (run->depth < 2u) {\n            LENIENT(GFNT_HINT_FAIL_STACK_UNDERFLOW);\n            break;\n          }\n          index = POP();",
     "          if (run->depth < 2u) {\n            LENIENT(GFNT_HINT_FAIL_STACK_UNDERFLOW);\n            run->depth = 0;\n            break;\n          }\n          index = POP();",
     "DELTAC with too few pairs throws the rest of the stack away"),
    ("src/hint/tt_prog.c", "  if (run->ctx->params.version >= 40 && !(run->instruct_control & 4u)) {\n    *x = run->start_pp[i].x;",
     "  if (0) {\n    *x = run->start_pp[i].x;",
     "version 40 reads the bearings back from the program's phantom points"),
    ("src/hint/glyph.c", "components[k].delta_x = gfnt_hint_delta_fixed(wide[k]);",
     "components[k].delta_x = 0;",
     "gvar does not move a composite's component offsets"),
    ("src/hint/glyph.c", "if (!v || !gfnt_face_has_table(face, GFNT_TAG_HVAR)) return;",
     "if (1) return;",
     "the advance ignores HVAR and takes gvar's phantom points"),
    ("src/hint/tt_exec.c", "result |= 1 << 10;  /* a variable face */", "result |= 1 << 9;",
     "GETINFO reports a variable face in the wrong bit"),
    ("src/var/cvar.c", "#define GFNT_CVAR_INTERPRETER_BIAS 0x200", "#define GFNT_CVAR_INTERPRETER_BIAS 0",
     "a control value delta is floored with none of the interpreter's bias"),
    ("src/hint/tt_move.c", "org_dist = gfnt_hint_scaled_original_distance(run, z1, (size_t)p, z0, rp0);\n        cur_dist",
     "org_dist = gfnt_hint_original_distance(run, z1, (size_t)p, z0, rp0);\n        cur_dist",
     "MIRP measures the original distance in font units"),
]


def run(command, *, env=None, expect_failure=False):
    done = subprocess.run(command, cwd=ROOT, capture_output=True, text=True,
                          env={**os.environ, **(env or {})})
    if (done.returncode != 0) != expect_failure:
        want = "failure" if expect_failure else "success"
        raise RuntimeError(f"expected {want} from {' '.join(map(str, command))} "
                           f"(exit {done.returncode}):\n{(done.stdout + done.stderr)[-600:]}")


def rebuild():
    run(["make", f"PREFIX={PREFIX}", f"BUILD={MAKE_BUILD}", str(DRIVER.relative_to(ROOT))])


def forget(source):
    obj = ROOT / "build" / BUILD / "objects" / "hint" / (Path(source).stem + ".o")
    for artifact in (obj, ARCHIVE, DRIVER):
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
                refused = False
                for version in ("35",):
                    done = subprocess.run(
                        ["python3", "tools/oracle/oracle_run.py", "fonttools,freetype", "--",
                         "python3", "tools/oracle/hint_probe_diff.py", "--seed", "41",
                         "--fonts", "4", "--glyphs", "100", "--steps", "8",
                         "--vectors", "axis", "--version", version, "--show", "0"],
                        cwd=ROOT, capture_output=True, text=True)
                    refused = refused or done.returncode != 0
                done = subprocess.run(
                    ["python3", "tools/oracle/oracle_run.py", "fonttools,freetype", "--",
                     "python3", "tools/oracle/hint_batteries.py"],
                    cwd=ROOT, capture_output=True, text=True)
                refused = refused or done.returncode != 0
                if not refused:
                    done = subprocess.run(
                        ["python3", "tools/oracle/oracle_run.py", "fonttools,freetype", "--",
                         "python3", "tools/oracle/hint_probe_diff.py", "--seed", "73",
                         "--fonts", "2", "--glyphs", "100", "--steps", "6", "--vectors", "axis",
                         "--version", "40", "--wild", "0.3", "--ppem", "12,17", "--show", "0"],
                        cwd=ROOT, capture_output=True, text=True)
                    refused = refused or done.returncode != 0
                if not refused:
                    done = subprocess.run(
                        ["python3", "tools/oracle/oracle_run.py", "fonttools,freetype", "--",
                         "python3", "tools/oracle/hint_probe_diff.py", "--seed", "43",
                         "--fonts", "3", "--glyphs", "40", "--composites", "15", "--steps", "1",
                         "--vectors", "axis", "--version", "40", "--variable", "hvar",
                         "--coords", "16384;-16384;8192", "--ppem", "16", "--tolerance", "1",
                         "--show", "0"],
                        cwd=ROOT, capture_output=True, text=True)
                    refused = refused or done.returncode != 0
                if not refused:
                    raise RuntimeError(f"the comparison accepted: {what}")
            finally:
                path.write_text(original)
                forget(source)
                rebuild()
            print(f"hint_mutation_check: refused: {what}")
    except (OSError, RuntimeError) as error:
        print(f"hint_mutation_check: {error}", file=sys.stderr)
        return 1
    print("hint_mutation_check: every planted defect was refused by the FreeType comparison")
    return 0


if __name__ == "__main__":
    sys.exit(main())
