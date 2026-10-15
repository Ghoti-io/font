#!/usr/bin/env python3
"""Prove the focused layout tests reject loss of inline-object run creation."""
from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "src/layout/layout.c"
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


def rejects_object_mutant() -> None:
    original = SOURCE.read_text()
    needle = "if (end == start + 1 && cps[start] == 0xFFFC && style->inline_object) {"
    replacement = "if (end == start + 1 && cps[start] == 0xFFFC && !style->inline_object) {"
    if original.count(needle) != 1:
        raise RuntimeError(f"expected one mutation site, found {original.count(needle)}")
    artifacts = (APP, APP.parent / "libghoti.io-font-7.a",
        ROOT / "build" / BUILD / "objects/layout/layout.o")
    try:
        SOURCE.write_text(original.replace(needle, replacement))
        for artifact in artifacts:
            artifact.unlink(missing_ok=True)
        run(["make", f"PREFIX={PREFIX}", f"BUILD={MAKE_BUILD}", str(APP.relative_to(ROOT))])
        run([str(APP), "--gtest_filter=LayoutInlineObject.OneReplacementCharacterBecomesAnAtomicCallerOwnedRun",
             "--gtest_brief=1"], expect_failure=True)
    finally:
        SOURCE.write_text(original)
        for artifact in artifacts:
            artifact.unlink(missing_ok=True)
        run(["make", f"PREFIX={PREFIX}", f"BUILD={MAKE_BUILD}", str(APP.relative_to(ROOT))])


def main() -> int:
    try:
        rejects_object_mutant()
    except (OSError, RuntimeError) as error:
        print(f"layout_l2_mutation_check: {error}", file=sys.stderr)
        return 1
    print("layout_l2_mutation_check: missing synthetic object run rejected")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
