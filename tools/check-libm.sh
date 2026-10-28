#!/bin/sh
#
# Fail if a library references or defines a libm symbol (AD-22, amended
# 2026-10-18).
#
# glibc, musl, UCRT and Apple's libm differ in the last bit of the functions C
# and IEEE 754 do not require to be correctly rounded, and a compiler decides
# for itself whether sqrt, floor or fabs is an instruction or a call.  So no
# shape library takes anything from libm: `math` supplies sin, cos, tan, atan2,
# exp, log, pow, hypot, sqrt, floor, ceil, trunc, round, rint, fabs, copysign,
# scalbn, ldexp, frexp and ilogb, and whatever else a library needs is added
# there.  This gate refuses every function of <math.h> (C99, C11 and C23, and
# the extensions glibc adds), each in its double, `f`, `l`, `f32`, `f64`, `f128`,
# `f32x` and `f64x` forms, and the `__isnan` family.  The names are listed in
# BASES below; `--names` prints the whole refused set (check-gates.sh plants
# every one of them from it).
#
# What is read is the symbol table, so the gate sees the link, not the source: a
# call that was inlined away is not a reference, and one a header hid behind a
# macro, or a `__builtin_` the compiler lowered to a call (sqrt without
# -fno-math-errno, floor without SSE4.1, fma without the instruction), is.  For a
# shared object the dynamic table is read (`nm -D`, where a glibc symbol is
# `sin@GLIBC_2.2.5` and the version suffix is stripped); for an object or an
# archive, the ordinary one.  A name is refused undefined or defined: a library
# that defines its own `sin` has done the thing the gate exists to stop by
# another route.  No library is exempt, `math` included: its functions are
# `gmath_` names.
#
# Usage: check-libm.sh <file>...
#        check-libm.sh --names
#   a file is an object, an archive or a shared object. An empty list, a file
#   with no symbol table, or a population that yields no symbol at all fails: a
#   gate that read nothing would pass any planted reference.
#
# The same text is in math, shape-geometry, shape-scene and shape-cpu;
# suite/tools/xarch/shape.sh fails when the four copies differ.

set -u

# The base names.  Each is also refused with the suffixes below.
BASES='acos acosh asin asinh atan atan2 atanh cbrt ceil copysign cos cosh erf erfc
exp exp10 exp2 expm1 fabs fdim floor fma fmax fmin fmod frexp hypot ilogb ldexp
lgamma llogb llrint llround log log10 log1p log2 logb lrint lround modf nan
nearbyint nextafter nexttoward nextdown nextup pow remainder remquo rint round
roundeven scalb scalbln scalbn sin sincos sinh sqrt tan tanh tgamma trunc
fmaxmag fminmag fmaximum fmaximum_mag fmaximum_mag_num fmaximum_num fminimum
fminimum_mag fminimum_mag_num fminimum_num canonicalize fromfp fromfpx ufromfp
ufromfpx getpayload setpayload setpayloadsig totalorder totalordermag
acospi asinpi atanpi atan2pi cospi sinpi tanpi compoundn pown powr rootn rsqrt
exp10m1 exp2m1 log10p1 log2p1 logp1
fadd fsub fmul fdiv ffma fsqrt dadd dsub dmul ddiv dfma dsqrt
j0 j1 jn y0 y1 yn gamma drem significand finite isinf isnan pow10 lgamma_r
gamma_r signgam
__fpclassify __signbit __isnan __isinf __finite __iseqsig __issignaling'
SUFFIXES='f l f16 f32 f64 f128 f32x f64x'

all_names() {
  for b in $BASES; do
    printf '%s\n' "$b"
    for s in $SUFFIXES; do
      printf '%s%s\n' "$b" "$s"
    done
  done
}

if [ "${1:-}" = --names ]; then
  all_names
  exit 0
fi
if [ $# -eq 0 ]; then
  printf 'check-libm: no files given; this gate is measuring nothing\n' >&2
  exit 1
fi

names="$(all_names)"

NM="${NM:-nm}"
status=0
nfiles=0
nsyms=0

for f in "$@"; do
  nfiles=$((nfiles + 1))
  if [ ! -f "$f" ]; then
    printf 'check-libm: %s is not a file\n' "$f" >&2
    exit 1
  fi
  case "$f" in
    *.so | *.so.* | *.dll | *.dylib) syms="$("$NM" -D "$f" 2>/dev/null)" ;;
    *) syms="$("$NM" "$f" 2>/dev/null)" ;;
  esac
  rc=$?
  if [ "$rc" -ne 0 ] || [ -z "$syms" ]; then
    # An object with no symbols at all is not a library; refuse it rather than
    # certify it. (A shared object always has at least its imports.)
    printf 'check-libm: read no symbols from %s; the gate cannot see this file\n' "$f" >&2
    exit 1
  fi
  # "<type> <name>" per line; an archive adds blank lines and "member.o:" headers.
  list="$(printf '%s\n' "$syms" | awk '
    NF >= 2 && $(NF-1) ~ /^[A-Za-z?]$/ { n = $NF; sub(/@.*/, "", n); print $(NF-1), n }' | sort -u)"
  nsyms=$((nsyms + $(printf '%s\n' "$list" | sed '/^$/d' | wc -l)))
  # One pass: the refused names are read first, then the symbols.
  hits="$( { printf '%s\n' "$names"; printf '=SYMBOLS=\n'; printf '%s\n' "$list"; } | awk '
    $0 == "=SYMBOLS=" { insyms = 1; next }
    !insyms { refused[$1] = 1; next }
    ($2 in refused) { kind[$2] = kind[$2] $1 }
    END { for (n in kind) print n, kind[n] }' | sort)"
  if [ -n "$hits" ]; then
    while read -r n kinds; do
      kind=referenced
      case "$kinds" in
        *[TtDdBbRrWV]*) kind="defined" ;;
      esac
      printf 'check-libm: %s: libm function %s is %s (AD-22: take it from math)\n' \
        "$f" "$n" "$kind" >&2
    done <<EOH
$hits
EOH
    status=1
  fi
done

if [ "$nsyms" -eq 0 ]; then
  printf 'check-libm: read no symbols from %d files; this gate is measuring nothing\n' "$nfiles" >&2
  exit 1
fi
if [ "$status" -ne 0 ]; then
  exit 1
fi
printf 'check-libm: %d files, %d symbols read, none a libm function\n' "$nfiles" "$nsyms"
