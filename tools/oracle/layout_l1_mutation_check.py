#!/usr/bin/env python3
"""Prove the focused layout tests reject missing L1 shift and strut behavior."""
from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys


sys.path.insert(0, str(Path(__file__).resolve().parent))
from layout_sources import object_of, source_with

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "src/layout/layout.c"   # replaced by the file holding each site
PREFIX = os.environ.get("PREFIX", "/home/corey/Documents/ghoti.io/.local")
BUILD = os.environ.get("BUILD", "linux/release")
MAKE_BUILD = BUILD.removeprefix("linux/")
APP = ROOT / "build" / BUILD / "apps" / "testLayout"
LD_PATH = f"{PREFIX}/lib/ghoti.io:{PREFIX}/lib"


def run(command: list[str], *, expect_failure: bool = False) -> None:
    result = subprocess.run(command, cwd=ROOT, env={**os.environ, "LD_LIBRARY_PATH": LD_PATH})
    if (result.returncode != 0) != expect_failure:
        want = "failure" if expect_failure else "success"
        raise RuntimeError(f"expected {want}: {' '.join(command)} (exit {result.returncode})")


def rejects_mutant(needle: str, replacement: str, test_name: str) -> None:
    global SOURCE
    SOURCE = source_with(ROOT, needle)
    original = SOURCE.read_text()
    obj = object_of(ROOT, BUILD, SOURCE)
    try:
        SOURCE.write_text(original.replace(needle, replacement))
        # The project Makefile treats generated library artifacts as a
        # secondary build path; remove them so the mutated C source is linked.
        for artifact in (APP, APP.parent / "libghoti.io-font-7.a", obj):
            artifact.unlink(missing_ok=True)
        run(["make", f"PREFIX={PREFIX}", f"BUILD={MAKE_BUILD}", str(APP.relative_to(ROOT))])
        run([str(APP), f"--gtest_filter=LayoutLineBox.{test_name}", "--gtest_brief=1"], expect_failure=True)
    finally:
        SOURCE.write_text(original)
    for artifact in (APP, APP.parent / "libghoti.io-font-7.a", obj):
        artifact.unlink(missing_ok=True)
    run(["make", f"PREFIX={PREFIX}", f"BUILD={MAKE_BUILD}", str(APP.relative_to(ROOT))])


def main() -> int:
    try:
        rejects_mutant(
            "placed->y = line->baseline - shift - pg->dy;",
            "placed->y = line->baseline - pg->dy;",
            "BaselineShiftMovesGlyphsAcrossTheLineAxisInEveryWritingMode",
        )
        rejects_mutant(
            "if (inline_mode && p->options.strut) {",
            "if (false && inline_mode && p->options.strut) {",
            "StrutContributesToSmallAndEmptyLines",
        )
    except (OSError, RuntimeError) as error:
        print(f"layout_l1_mutation_check: {error}", file=sys.stderr)
        return 1
    print("layout_l1_mutation_check: shift and strut mutants rejected")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
