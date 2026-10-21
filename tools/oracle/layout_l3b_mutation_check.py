#!/usr/bin/env python3
"""Ensure the column widow rule changes a real break decision."""
from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


sys.path.insert(0, str(Path(__file__).resolve().parent))
from layout_sources import source_with

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "src/layout/layout.c"   # replaced by the file holding the site
PREFIX = os.environ.get("PREFIX", str(ROOT.parents[1] / ".local"))
REQUESTED_BUILD = Path(os.environ.get("BUILD", "linux/release"))
BUILD_PARENT = REQUESTED_BUILD.parent if len(REQUESTED_BUILD.parts) > 1 else Path("linux")
# Keep the mutant and restore builds isolated from the caller's ordinary build.
MAKE_BUILD = f"layout-l3b-mutation-{os.getpid()}"
BUILD_DIR = ROOT / "build" / BUILD_PARENT / MAKE_BUILD
APP = BUILD_DIR / "apps" / "testLayout"
LD_PATH = f"{PREFIX}/lib/ghoti.io:{PREFIX}/lib"
TEST = "LayoutColumns.AutoHonorsWidowsWhenAWholeLineCanMove"


def atomic_write(path: Path, contents: bytes, mode: int) -> None:
    """Replace a source file atomically, keeping its permissions."""
    fd, temporary_name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(contents)
            stream.flush()
            os.fsync(stream.fileno())
        temporary.chmod(mode)
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=ROOT,
        env={**os.environ, "LD_LIBRARY_PATH": LD_PATH},
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )


def check_build(command: list[str], label: str) -> None:
    result = run(command)
    if result.returncode:
        raise RuntimeError(f"{label} failed (exit {result.returncode}):\n{result.stdout}")


def main() -> int:
    global SOURCE
    needle = (
        b"while (layout->line_count - at < widows && at > begin\n"
        b"          && at - begin > orphans) {"
    )
    try:
        SOURCE = source_with(ROOT, needle.decode())
    except RuntimeError as exc:
        print(f"layout_l3b_mutation_check: expected one widow adjustment: {exc}", file=sys.stderr)
        return 1
    original = SOURCE.read_bytes()
    mode = SOURCE.stat().st_mode & 0o7777

    error: Exception | None = None
    restored = False
    try:
        # Keep `orphans` referenced to satisfy strict -Werror builds while
        # disabling only the widow condition under test.
        mutant = original.replace(
            needle,
            b"while (false && layout->line_count - at < widows && at > begin\n"
            b"          && at - begin > orphans) {",
        )
        atomic_write(SOURCE, mutant, mode)
        check_build(["make", f"PREFIX={PREFIX}", f"BUILD={MAKE_BUILD}", str(APP.relative_to(ROOT))],
                    "mutant build")
        result = run([str(APP), f"--gtest_filter={TEST}", "--gtest_brief=1"])
        expected_failure = (
            result.returncode != 0
            and f"[  FAILED  ] {TEST}" in result.stdout
            and "columns.column_line_starts[1]" in result.stdout
        )
        if not expected_failure:
            raise RuntimeError(
                "mutation did not produce the expected widow assertion failure "
                f"(exit {result.returncode}):\n{result.stdout}"
            )
    except (OSError, RuntimeError) as exc:
        error = exc
    finally:
        try:
            # Atomic replacement means interruption cannot leave a truncated
            # source file. Always restore the exact bytes seen at entry.
            atomic_write(SOURCE, original, mode)
            restored = SOURCE.read_bytes() == original
            if not restored:
                raise RuntimeError("source restoration did not reproduce original bytes")
        except (OSError, RuntimeError) as exc:
            error = RuntimeError(f"source restore failed: {exc}")
        try:
            if restored:
                check_build(
                    ["make", f"PREFIX={PREFIX}", f"BUILD={MAKE_BUILD}", str(APP.relative_to(ROOT))],
                    "restore build",
                )
        except (OSError, RuntimeError) as exc:
            error = RuntimeError(f"{error}; {exc}" if error else str(exc))
        finally:
            shutil.rmtree(BUILD_DIR, ignore_errors=True)

    if error:
        print(f"layout_l3b_mutation_check: {error}", file=sys.stderr)
        return 1
    print("layout_l3b_mutation_check: disabling widow adjustment triggers the expected assertion")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
