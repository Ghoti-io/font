#!/bin/sh
#
# Fail if an x86 object holds an x87 instruction (AD-17).
#
# The x87 stack computes in 80-bit registers, so a `double` sum is rounded
# twice and the last bit can differ from every target that computes a double in
# a double. An i686 build with `-msse2 -mfpmath=sse` (xarch_fp_cflags in
# suite/tools/xarch/targets.sh) has none; this finds one that has. A `long
# double` is the other source: on x86-64 it is x87 too.
#
# The disassembly is read, not the flags: the flags are what was intended, the
# instructions are what was built. On x86 every mnemonic that begins with `f`
# is x87 state or an x87 operation (fld, fstp, fadd, fxch, fnstcw, fwait,
# fxsave and the rest); SSE and AVX mnemonics never do (vfmadd begins with v).
# Segment prefixes and lock/rep prefixes are skipped to reach the mnemonic.
#
# One use of x87 is not computation and is allowed: the i386 calling convention
# (System V and Windows alike) returns a float or a double in st(0), so a
# function that returns one built with -mfpmath=sse still moves its result from
# an SSE register through memory into the x87 stack (`fldl`, `flds`) and a
# caller moves it back (`fstpl`, `fstps`), and a returned 0.0 or 1.0 is `fldz`
# or `fld1`; a caller that ignores a result pops it with `fstp %st(0)`. Those
# are loads and stores of a value that is already a float or a double, and
# they round nothing. The gate allows exactly them, with a memory operand, and
# refuses everything else: arithmetic (fadd, fmul, fsqrt), a register-to-register
# fld, a long double (`fldt`, `fstpt`), an integer conversion (`fild`,
# `fistp`), the control word (`fldcw`, `fnstcw`) and the rest. With no x87
# arithmetic there is no 80-bit intermediate to round twice.
#
# Files that are not x86 are named and not judged, because other
# architectures' mnemonics begin with `f` for good reasons (arm64's fadd).
#
# Usage: check-no-x87.sh [--require-x86] <file>...
#   a file is an object or an archive of objects. OBJDUMP names the program
#   (i686-linux-gnu-objdump for a cross build). --require-x86 fails when no file
#   is x86, so the cross-build gate cannot pass on the wrong architecture.
#   An empty list, a file objdump cannot read, or a population with no
#   instruction at all fails.
#
# The same text is in math, shape-geometry, shape-scene and shape-cpu;
# suite/tools/xarch/shape.sh fails when the four copies differ.

set -u

require=no
if [ "${1:-}" = --require-x86 ]; then
  require=yes
  shift
fi
if [ $# -eq 0 ]; then
  printf 'check-no-x87: no files given; this gate is measuring nothing\n' >&2
  exit 1
fi

OBJDUMP="${OBJDUMP:-objdump}"
status=0
nx86=0
nother=0
ninsn=0

for f in "$@"; do
  if [ ! -f "$f" ]; then
    printf 'check-no-x87: %s is not a file\n' "$f" >&2
    exit 1
  fi
  hdr="$("$OBJDUMP" -f "$f" 2>/dev/null)"
  if [ -z "$hdr" ]; then
    printf 'check-no-x87: %s cannot be read by %s\n' "$f" "$OBJDUMP" >&2
    exit 1
  fi
  if ! printf '%s\n' "$hdr" | grep -q 'architecture: i386'; then
    nother=$((nother + 1))
    printf 'check-no-x87: %s is not x86; not judged\n' "$f"
    continue
  fi
  nx86=$((nx86 + 1))
  out="$("$OBJDUMP" -d --no-show-raw-insn "$f" 2>/dev/null)"
  if [ -z "$out" ]; then
    printf 'check-no-x87: %s disassembled to nothing\n' "$f" >&2
    exit 1
  fi
  res="$(printf '%s\n' "$out" | awk -F'\t' -v file="$f" '
    / file format / { member = $1; sub(/:.*/, "", member) }
    $1 ~ /^ *[0-9a-f]+:$/ {
      insn++
      n = split($2, w, /[ ]+/)
      op = ""; rest = ""
      for (i = 1; i <= n; i++) {
        if (w[i] == "") continue
        if (op == "") {
          if (w[i] ~ /^(lock|rep|repz|repnz|repe|repne|data16|addr32|notrack|bnd|cs|ds|es|ss|fs|gs|rex[.A-Z]*)$/) continue
          op = w[i]
        } else {
          rest = rest w[i]
        }
      }
      if (op ~ /^f[a-z0-9]+$/) {
        # The ABI transfers: a load or store of a float or double through
        # memory, the constants 0.0 and 1.0, and the pop of an ignored result.
        allowed = 0
        if (op ~ /^(fld|fst|fstp)[sl]$/ && rest !~ /%st/) allowed = 1
        if (op == "fldz" || op == "fld1") allowed = 1
        if (op == "fstp" && rest == "%st(0)") allowed = 1
        if (allowed) transfer++
        else {
          printf "HIT %s (%s): %s\n", file, member, $2
          hits++
        }
      }
    }
    END { printf "COUNT %d\n", insn }')"
  ninsn=$((ninsn + $(printf '%s\n' "$res" | sed -n 's/^COUNT //p')))
  hits="$(printf '%s\n' "$res" | sed -n 's/^HIT //p')"
  if [ -n "$hits" ]; then
    printf '%s\n' "$hits" | while IFS= read -r l; do
      printf 'check-no-x87: x87 instruction in %s\n' "$l" >&2
    done
    status=1
  fi
done

if [ "$status" -ne 0 ]; then
  exit 1
fi
if [ "$require" = yes ] && [ "$nx86" -eq 0 ]; then
  printf 'check-no-x87: no x86 file among %d; the cross gate looked at the wrong architecture\n' "$#" >&2
  exit 1
fi
if [ "$nx86" -eq 0 ]; then
  printf 'check-no-x87: SKIPPED, %d files and none is x86 (x87 cannot occur); run the i686 arm with suite/tools/xarch/shape.sh\n' \
    "$nother"
  exit 0
fi
if [ "$ninsn" -eq 0 ]; then
  printf 'check-no-x87: read no instruction from %d x86 files; this gate is measuring nothing\n' "$nx86" >&2
  exit 1
fi
printf 'check-no-x87: %d x86 files, %d instructions, no x87\n' "$nx86" "$ninsn"
