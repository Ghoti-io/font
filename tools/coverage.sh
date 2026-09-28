#!/bin/sh
#
# Summarize gcov data for a build that was compiled with --coverage.
#
# Run from the project root, with the object directory as the only argument. The
# annotated source it reads is left behind as <object-dir>/coverage.txt, because
# the summary prints percentages and a triage needs the lines: grep it for
# `#####` to get them.
# Prints per-file line coverage, the project total, and - separately - the
# growth and resize lines that never executed.
#
# That last list is the point of the report. A dynamic structure whose
# reallocation path is never taken by any test is not covered by "all tests
# pass"; a defect of exactly that shape (a hash table that corrupted itself
# when it grew) survived a full green suite in this codebase.

set -eu

OBJ_DIR="${1:?usage: coverage.sh <object-dir>}"
# The floor and the smallest report worth believing, both overridable so that a
# one-off run over part of the suite can say so rather than fail.
#
# The floor is a tripwire, not a target. What the report is read for is the list
# of lines nothing reaches; the percentage is here so that a change which stops
# reaching a hundred of them cannot pass unnoticed, and it is deliberately a
# little below what the suite achieves - close enough to notice a regression,
# far enough not to fail on a line somebody adds before its test.
#
# GFNT_COVERAGE_LINES is the control. Every figure below is a ratio, and a ratio
# is fine about a report that collapsed: a sweep that measured one file would
# print 100% and pass a floor of any height. So the denominator has a floor of
# its own.
#
# **It has to be raised when the library grows**, or it stops being a control: it
# was 3,000 against a 3,148-line library, and phase 2 took that to 4,951. A floor
# a third below the real figure would let a report that lost a third of the
# library through unnoticed.
FLOOR="${GFNT_COVERAGE_FLOOR:-96}"
LINES_FLOOR="${GFNT_COVERAGE_LINES:-4500}"

if ! command -v gcov >/dev/null 2>&1; then
  echo "coverage: gcov not found (install gcc's gcov)" >&2
  exit 1
fi

GCDA=$(find "$OBJ_DIR" -name '*.gcda' 2>/dev/null || true)
if [ -z "$GCDA" ]; then
  echo "coverage: no profile data under $OBJ_DIR" >&2
  echo "coverage: the tests must run after an instrumented build" >&2
  exit 1
fi

# gcov resolves the Source: paths relative to the directory it runs in, so it
# is invoked from the project root with the object directory passed per file.
#
# `-t` writes the annotated source to stdout instead of to a .gcov file, and
# every object's output is concatenated into one stream. That is not a
# convenience: a header with code in it - src/core/fixed.h - is compiled into
# many objects, and gcov names its output after the *source*, so each object's
# report overwrote the last one. Whichever object happened to be processed last
# decided what the report said about that header, and a line executed by every
# other translation unit read as never executed. Measured: fixed.h's two
# saturation lines flipped between covered and uncovered depending on nothing
# but find's order.
REPORT="$OBJ_DIR/coverage.txt"
rm -f "$REPORT"
for g in $GCDA; do
  gcov -t -p -r -o "$(dirname "$g")" "$g" 2>/dev/null >> "$REPORT" || true
done

awk -v floor="$FLOOR" -v lines_floor="$LINES_FLOOR" '
  # The Source: line starts each file section of the stream - one per object, so
  # the same source can appear many times and its lines are merged below.
  /^[ \t]*-:[ \t]*0:Source:/ {
    n = index($0, "Source:")
    src = substr($0, n + 7)
    sub(/^[ \t]+/, "", src)
    # Only report on the library itself, not tests or system headers.
    if (src !~ /^src\//) { src = "SKIP" }
    next
  }
  src == "" || src == "SKIP" { next }

  {
    # Each line is "<count>:<lineno>:<text>", where the count is a number, a
    # dash for a non-executable line, or ##### for one never executed.
    c1 = index($0, ":")
    if (c1 == 0) next
    count = substr($0, 1, c1 - 1)
    rest  = substr($0, c1 + 1)
    c2 = index(rest, ":")
    if (c2 == 0) next
    lineno = substr(rest, 1, c2 - 1) + 0
    text   = substr(rest, c2 + 1)
    gsub(/^[ \t]+|[ \t]+$/, "", count)
    if (count == "-" || lineno == 0) next

    # A header included more than once per object appears repeatedly; a line
    # counts as executed if any instantiation reached it.
    key = src ":" lineno
    seen[key] = 1
    if (count != "#####" && count != "$$$$$") {
      hit[key] = 1
    } else if (!(key in hit)) {
      body[key] = text
    }
    file_of[key] = src
  }

  END {
    for (key in seen) {
      f = file_of[key]
      total[f]++
      grand_total++
      if (key in hit) {
        covered[f]++
        grand_covered++
      } else {
        t = body[key]
        if (t ~ /realloc|capacity|[Gg]row|GROW|rehash|resize|reserve/) {
          split(key, parts, ":")
          gaps[f] = gaps[f] " " parts[2]
          gap_count++
        }
      }
    }

    printf "\n%-52s %8s %s\n", "FILE", "LINES", "COVERED"
    n = 0
    for (f in total) { files[++n] = f }
    # Simple insertion sort: least covered first, so the gaps lead.
    for (i = 2; i <= n; i++) {
      v = files[i]; rv = covered[v] / total[v]
      j = i - 1
      while (j >= 1 && (covered[files[j]] / total[files[j]]) > rv) {
        files[j + 1] = files[j]; j--
      }
      files[j + 1] = v
    }
    for (i = 1; i <= n; i++) {
      f = files[i]
      printf "%-52s %8d %6.1f%%\n", f, total[f], covered[f] * 100 / total[f]
    }
    printf "%-52s %8d %6.1f%%\n", "TOTAL", grand_total,
      (grand_total ? grand_covered * 100 / grand_total : 0)
    if (grand_total < lines_floor) {
      printf "\ncoverage: only %d executable lines were measured, under the %d " \
          "this report is supposed to see. Something did not compile with " \
          "--coverage, or did not run.\n", grand_total, lines_floor
      failed = 1
    }
    else if (grand_covered * 100 < floor * grand_total) {
      printf "\ncoverage: %.1f%% is below the floor of %d%%. The lines nobody " \
          "reaches are listed above; documentation/development.md says which " \
          "kinds are expected to stay there.\n",
          grand_covered * 100 / grand_total, floor
      failed = 1
    }

    if (gap_count > 0) {
      printf "\n%d growth/capacity lines never executed:\n", gap_count
      for (i = 1; i <= n; i++) {
        f = files[i]
        if (f in gaps) printf "  %s:%s\n", f, gaps[f]
      }
      printf "\nA reallocation path no test reaches is untested, not working.\n"
    } else {
      printf "\nEvery growth/capacity line was executed.\n"
    }
    if (failed) {
      exit 1
    }
  }
' "$REPORT"
