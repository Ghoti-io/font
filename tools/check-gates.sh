#!/bin/sh
#
# Prove that each shape-family gate fails on the defect it exists to catch.
#
# A gate that has never been seen to fail may be measuring nothing, so this
# runs the real scripts - not copies, not mocks - against fixtures it builds in
# a temporary directory (and, for check-fp-contract, against the real Makefile
# with the flag taken out):
#
#   control   a correct input; the gate must pass it, so that a gate which
#             rejects everything cannot pass this self-test
#   planted   an input with one defect; the gate must exit non-zero AND name
#             the offending file, symbol, instruction or edge, so that a gate
#             that fails for the wrong reason (a typo, a missing tool) does
#             not count
#   empty     nothing at all; the gate must fail rather than report success
#             over a population of zero
#
# The gates and the rule each enforces:
#
#   check-fp-contract  AD-17  every compile-flag set names -ffp-contract=off
#   check-edges        AD-2   #includes and NEEDED entries stay in the allowlist
#   check-libm         AD-22  no libm function referenced or defined (nothing is exempt)
#   check-no-x87       AD-17  no x87 instruction in an x86 object
#
# The cross-target hash gate needs cross compilers and so is not here; it is
# suite/tools/xarch/shape.sh (and suite/tools/xwin/shape.sh), which plants a
# differing bit and requires the gate to name the target.
#
# Environment, supplied by `make check-gates`:
#   GATE_PROJECT       this library's name
#   GATE_ALLOWED       its EDGES_ALLOWED alternation
#   GATE_LIBM_EXEMPT   no, for every library (math too); anything else fails
#   CC                 the C compiler
#
# The same text is in math, shape-geometry, shape-scene and shape-cpu;
# suite/tools/xarch/shape.sh fails when the four copies differ.

set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$HERE")"
CC="${CC:-cc}"
lib="${GATE_PROJECT:?GATE_PROJECT is not set; run this through make check-gates}"
allowed="${GATE_ALLOWED:?GATE_ALLOWED is not set; run this through make check-gates}"
exempt="${GATE_LIBM_EXEMPT:?GATE_LIBM_EXEMPT is not set; run this through make check-gates}"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT INT TERM

failures=0
checks=0

fail() {
  printf 'check-gates: FAIL: %s\n' "$*" >&2
  failures=$((failures + 1))
}

# expect_pass <label> <command...>
expect_pass() {
  label="$1"; shift
  checks=$((checks + 1))
  if out="$("$@" 2>&1)"; then
    printf '  ok   %s passes\n' "$label"
  else
    fail "$label should pass but failed:
$out"
  fi
}

# expect_fail <label> <needle> <command...>
# The needle is what the failure message must name.
expect_fail() {
  label="$1"; needle="$2"; shift 2
  checks=$((checks + 1))
  out="$("$@" 2>&1)"
  rc=$?
  if [ "$rc" -eq 0 ]; then
    fail "$label should fail but exited 0:
$out"
  elif ! printf '%s' "$out" | grep -qF -- "$needle"; then
    fail "$label failed, but without naming '$needle':
$out"
  else
    printf '  ok   %s fails, naming %s\n' "$label" "$needle"
  fi
}

mkdir "$work/empty"

# No library is exempt from the libm gate (AD-22, amended 2026-10-18: math is judged too).
checks=$((checks + 1))
if [ "$exempt" != no ]; then
  fail "$lib sets LIBM_EXEMPT to '$exempt'; it must be no (nothing is exempt, math included)"
fi

#---------------------------------------------------------------------------
printf 'check-fp-contract\n'
# -ffp-contract=off is what makes a compiled double the same on every host.
# The real Makefile is the control; the same Makefile with the flag taken out,
# everywhere or from one flag set, is the planted defect and must name the
# library and the set.
F="$HERE/check-fp-contract.sh"
expect_pass 'fp-contract/control (the Makefile as it is)' "$F" "$ROOT/Makefile"
sed 's/ -ffp-contract=off//g' "$ROOT/Makefile" > "$work/Makefile.nofp"
expect_fail 'fp-contract/planted (the flag removed everywhere)' "$lib: CFLAGS does not name -ffp-contract=off" "$F" "$work/Makefile.nofp" "$ROOT"
expect_fail 'fp-contract/planted (the flag removed everywhere, C++)' "$lib: CXXFLAGS does not name -ffp-contract=off" "$F" "$work/Makefile.nofp" "$ROOT"
sed '/^CXXFLAGS :=/s/ -ffp-contract=off//' "$ROOT/Makefile" > "$work/Makefile.nofp-cxx"
expect_fail 'fp-contract/planted (removed from CXXFLAGS only)' "$lib: CXXFLAGS does not name -ffp-contract=off" "$F" "$work/Makefile.nofp-cxx" "$ROOT"
sed '/^CFLAGS :=/s/ -ffp-contract=off//' "$ROOT/Makefile" > "$work/Makefile.nofp-c"
expect_fail 'fp-contract/planted (removed from CFLAGS only, so LIB_CFLAGS and the sanitizer sets too)' "$lib: LIB_CFLAGS does not name -ffp-contract=off" "$F" "$work/Makefile.nofp-c" "$ROOT"
sed '/^ASAN_CFLAGS :=/s/$/ -ffp-contract=on/' "$ROOT/Makefile" > "$work/Makefile.asan-on"
expect_fail 'fp-contract/planted (the sanitizer set alone says on)' "$lib: ASAN_CFLAGS uses -ffp-contract=on" "$F" "$work/Makefile.asan-on" "$ROOT"
expect_pass 'fp-contract/control with an EXTRA_CFLAGS that is harmless' env EXTRA_CFLAGS=-O3 EXTRA_CXXFLAGS=-O3 "$F" "$ROOT/Makefile"
expect_fail 'fp-contract/planted (EXTRA_CFLAGS=-ffp-contract=fast on the real build)' "$lib: CFLAGS uses -ffp-contract=fast" env EXTRA_CFLAGS=-ffp-contract=fast "$F" "$ROOT/Makefile"
expect_fail 'fp-contract/planted (EXTRA_CXXFLAGS=-ffast-math on the real build)' "$lib: CXXFLAGS uses -ffast-math" env EXTRA_CXXFLAGS=-ffast-math "$F" "$ROOT/Makefile"
expect_fail 'fp-contract/empty' 'measuring nothing' "$F" "$work/empty/Makefile"

#---------------------------------------------------------------------------
printf 'check-stamps\n'
# Not a shape-family gate, but the one that keeps the flag change above from
# being a rebuild that does not happen; one planted removal shows it live here.
sed '/^\$(OBJ_DIR)\/%\.o: src\/%\.c/s/ \$(FLAGS_STAMP)//' "$ROOT/Makefile" > "$work/Makefile.nostamp"
expect_pass 'stamps/control' python3 "$HERE/check-stamps.py" "$ROOT/Makefile"
expect_fail 'stamps/planted (the library compile rule loses its stamp)' 'names no flag stamp' \
  python3 "$HERE/check-stamps.py" "$work/Makefile.nostamp"

#---------------------------------------------------------------------------
printf 'check-edges --includes\n'
E="$HERE/check-edges.sh"
FAMILY="math shape-geometry shape-scene shape-cpu font vector shape-vulkan shape-webgpu shape-toolpath shape-solid"
# Every name the allowlist permits, one per line.
allowed_list="$(printf '%s\n' "$allowed" | tr '|' '\n')"

edge() { E_allowed="$allowed"; "$E" --allowed "$allowed" --self "$lib" "$@"; }

# The control includes this library's own header and one header of every
# allowed library, in all three places the gate reads.
ctl="$work/inc-control"
mkdir -p "$ctl/src" "$ctl/include" "$ctl/examples"
{
  printf '#include <ghoti.io/%s/core.h>\n' "$lib"
  for a in $allowed_list; do printf '#include <ghoti.io/%s/x.h>\n' "$a"; done
} > "$ctl/src/a.c"
cp "$ctl/src/a.c" "$ctl/include/a.h"
cp "$ctl/src/a.c" "$ctl/examples/a.c"
expect_pass 'edges/control' edge --includes "$ctl"
expect_fail 'edges/empty (includes)' 'measuring nothing' edge --includes "$work/empty"

planted_libs=""
for f in $FAMILY runtime-core lang-tang text; do
  [ "$f" = "$lib" ] && continue
  if printf '%s\n' "$allowed_list" | grep -qx "$f"; then continue; fi
  planted_libs="$planted_libs $f"
done
for f in $planted_libs; do
  for where in src include examples; do
    t="$work/inc-$f-$where"
    mkdir -p "$t/src" "$t/include" "$t/examples"
    cp "$ctl/src/a.c" "$t/src/a.c"
    case "$where" in
      examples) ext=c ;;
      *) ext=c ;;
    esac
    printf '#include <ghoti.io/%s/x.h>\n' "$f" > "$t/$where/planted.$ext"
    expect_fail "edges/planted ($f in $where)" "$lib -> $f" edge --includes "$t"
    expect_fail "edges/planted ($f in $where) names the file" "planted.$ext" edge --includes "$t"
  done
done
# The quoted spelling of the same include.
for f in $planted_libs; do
  t="$work/inc-quoted-$f"
  mkdir -p "$t/src"
  printf '#include "ghoti.io/%s/x.h"\n' "$f" > "$t/src/q.c"
  expect_fail "edges/planted ($f, quoted)" "$lib -> $f" edge --includes "$t"
  break
done
# A name that merely begins with an allowed one is not that library.
t="$work/inc-extends"
mkdir -p "$t/src"
printf '#include <ghoti.io/cutil-extra/x.h>\n' > "$t/src/x.c"
expect_fail 'edges/planted (cutil-extra is not cutil)' "$lib -> cutil-extra" edge --includes "$t"

#---------------------------------------------------------------------------
printf 'check-edges --links\n'
# The link-line check cannot use a committed fixture, because what it reads is
# a built shared object's NEEDED list. So it builds real ones against stub
# libraries named as the suite names them.
case "$(uname -s)" in
  Linux)
    stubs="$work/stubs"
    mkdir -p "$stubs"
    printf 'int gstub_ref(void) { return 1; }\n' > "$work/stub.c"
    link_family="$(printf '%s\n' cutil cutil-extra $FAMILY runtime-core lang-tang text $allowed_list | sort -u)"
    stub_ok=1
    for s in $link_family; do
      $CC -shared -fPIC -o "$stubs/libghoti.io-$s-0.so" "$work/stub.c" \
        -Wl,-soname,"libghoti.io-$s-0.so.0" >"$work/stub-$s.log" 2>&1 || stub_ok=0
    done
    if [ "$stub_ok" -eq 0 ]; then
      fail "could not build the stub libraries"
    else
      printf 'int gconsumer(void) { return 2; }\n' > "$work/consumer.c"
      # build_so <dir> <stub>... : a shared object that NEEDs those stubs.
      build_so() {
        d="$1"; shift
        mkdir -p "$d"
        flags=""
        for s in "$@"; do flags="$flags -l:libghoti.io-$s-0.so"; done
        # shellcheck disable=SC2086
        $CC -shared -fPIC -o "$d/libconsumer.so" "$work/consumer.c" -L"$stubs" \
          -Wl,--no-as-needed $flags >"$d/build.log" 2>&1
      }
      okset="$lib"
      for a in $allowed_list; do okset="$okset $a"; done
      if build_so "$work/links-control" $okset; then
        expect_pass 'links/control (itself and every allowed library)' edge --links "$work/links-control"
      else
        fail "could not build the link control"
      fi
      for f in $planted_libs; do
        if build_so "$work/links-$f" cutil "$f"; then
          expect_fail "links/planted ($f)" "$lib -> $f" edge --links "$work/links-$f"
        else
          fail "could not build the $f link fixture"
        fi
      done
      if build_so "$work/links-extends" cutil-extra; then
        expect_fail 'links/planted (cutil-extra is not cutil)' "$lib -> cutil-extra" edge --links "$work/links-extends"
      else
        fail "could not build the cutil-extra link fixture"
      fi
    fi
    ;;
  *)
    printf '  NOT RUN: the NEEDED fixtures are built for ELF; this host is %s\n' "$(uname -s)"
    ;;
esac
expect_fail 'edges/empty (links)' 'measuring nothing' edge --links "$work/empty"

#---------------------------------------------------------------------------
printf 'check-libm\n'
# AD-22 (amended 2026-10-18): nothing comes from libm, not the transcendentals
# and not the functions C specifies exactly.  The gate prints its refused set
# (--names), so that the planted object below calls every one of them from the
# gate's own list, and no name can be added to the list without being planted.
M="$HERE/check-libm.sh"
cat > "$work/libm-control.c" <<'EOC'
/* What a library may reference: the C library's string functions and the gmath_ names. */
void * memcpy(void *, const void *, unsigned long);
unsigned long strlen(const char *);
double gmath_sqrt(double);
double gmath_floor(double);
double gmath_sin(double);
double libm_control(double x) {
  char b[8];
  memcpy(b, "abc", 4);
  return gmath_sqrt(x) + gmath_floor(x) + gmath_sin(x) + (double)strlen(b);
}
EOC
"$M" --names > "$work/libm-names.txt"
checks=$((checks + 1))
nnames=$(wc -l < "$work/libm-names.txt")
for must in sqrt floor ceil trunc round rint fabs copysign scalbn ldexp frexp fmod fma sin cos tan atan2 exp log pow hypot cbrt nearbyint lrint sqrtf sqrtl floorf floorl fabsf fabsl sinf sinl fmaf nan isnan __isnan; do
  if ! grep -qx "$must" "$work/libm-names.txt"; then
    fail "the libm gate does not refuse $must"
  fi
done
if [ "$nnames" -lt 500 ]; then
  fail "the libm gate refuses only $nnames names"
else
  printf '  ok   libm/the gate refuses %s names, among them sqrt, floor, fabs, fma, fmod and every f and l form\n' "$nnames"
fi
# One object that references every refused name.
{
  while read -r n; do
    printf 'extern void %s(void);\n' "$n"
  done < "$work/libm-names.txt"
  printf 'void libm_planted(void) {\n'
  while read -r n; do
    printf '  %s();\n' "$n"
  done < "$work/libm-names.txt"
  printf '}\n'
} > "$work/libm-planted.c"
# And one defining its own copy of the exactly-specified ones.
printf 'double sin(double x) { return x; }\n' > "$work/libm-defined.c"
printf 'double sqrt(double x) { return x; }\ndouble floor(double x) { return x; }\n' > "$work/libm-defined2.c"
# One reference at a time, each by a call a C programmer would write.
for pair in 'sin:double:double' 'sqrt:double:double' 'floor:double:double' 'fabs:double:double' 'rint:double:double' \
            'fmod:double:double,double' 'fma:double:double,double,double' 'sinf:float:float' 'sqrtl:long double:long double'; do
  fn="${pair%%:*}"; rest="${pair#*:}"; ret="${rest%%:*}"; args="${rest#*:}"
  case "$args" in
    *,*,*) call="$fn(x, x, x)"; decl="$ret $fn($args)"; params="double x" ;;
    *,*) call="$fn(x, x)"; decl="$ret $fn($args)"; params="double x" ;;
    *) call="$fn(($args)x)"; decl="$ret $fn($args)"; params="double x" ;;
  esac
  printf '%s;\ndouble libm_call_%s(%s) { return (double)%s; }\n' "$decl" "$fn" "$params" "$call" > "$work/libm-one-$fn.c"
done
# A builtin the compiler lowers to a call: sin has no instruction anywhere.
printf 'double libm_builtin_sin(double x) { return __builtin_sin(x); }\n' > "$work/libm-builtin-sin.c"
# The exactly-specified ones, as a C programmer writes them with the library's own flags minus the ones that
# turn them into instructions: sqrt keeps its errno path (a call for a negative argument), and a floor with no
# rounding instruction (SSE4.1) is a call.  Whether a target does call them is checked below, not assumed.
printf 'double libm_builtin_sqrt(double x) { return __builtin_sqrt(x); }\n' > "$work/libm-builtin-sqrt.c"
printf 'double libm_builtin_floor(double x) { return __builtin_floor(x); }\ndouble libm_builtin_fma(double x, double y, double z) { return __builtin_fma(x, y, z); }\n' > "$work/libm-builtin-floor.c"
built=1
$CC -O0 -fno-builtin -c "$work/libm-control.c" -o "$work/libm-control.o" 2>"$work/libm.log" || built=0
$CC -O0 -fno-builtin -c "$work/libm-planted.c" -o "$work/libm-planted.o" 2>>"$work/libm.log" || built=0
$CC -O0 -fno-builtin -c "$work/libm-defined.c" -o "$work/libm-defined.o" 2>>"$work/libm.log" || built=0
$CC -O0 -fno-builtin -c "$work/libm-defined2.c" -o "$work/libm-defined2.o" 2>>"$work/libm.log" || built=0
for f in "$work"/libm-one-*.c; do
  $CC -O0 -fno-builtin -c "$f" -o "${f%.c}.o" 2>>"$work/libm.log" || built=0
done
$CC -O2 -c "$work/libm-builtin-sin.c" -o "$work/libm-builtin-sin.o" 2>>"$work/libm.log" || built=0
$CC -O2 -fmath-errno -c "$work/libm-builtin-sqrt.c" -o "$work/libm-builtin-sqrt.o" 2>>"$work/libm.log" || built=0
$CC -O2 -c "$work/libm-builtin-floor.c" -o "$work/libm-builtin-floor.o" 2>>"$work/libm.log" || built=0
if [ "$built" -eq 1 ]; then
  expect_pass 'libm/control (memcpy, strlen and gmath_ names referenced)' "$M" "$work/libm-control.o"
  for fn in sin sqrt floor fabs rint fmod fma sinf sqrtl; do
    expect_fail "libm/planted ($fn referenced, object)" "libm function $fn is referenced" "$M" "$work/libm-one-$fn.o"
  done
  expect_fail 'libm/planted (a __builtin_sin the compiler lowered to a call)' 'libm function sin is referenced' "$M" "$work/libm-builtin-sin.o"
  # sqrt and floor are instructions on some targets, and then there is nothing for the gate to see
  # and nothing to plant; say so rather than pass.
  for pair in 'sqrt:libm-builtin-sqrt' 'floor:libm-builtin-floor' 'fma:libm-builtin-floor'; do
    fn="${pair%%:*}"; obj="$work/${pair#*:}.o"
    if nm "$obj" 2>/dev/null | grep -qE " U $fn\$"; then
      expect_fail "libm/planted (a __builtin_$fn the compiler lowered to a call)" "libm function $fn is referenced" "$M" "$obj"
    else
      printf '  skip libm/planted (__builtin_%s is an instruction on this target; nothing to plant)\n' "$fn"
    fi
  done
  # Every refused name must be named.
  checks=$((checks + 1))
  out="$("$M" "$work/libm-planted.o" 2>&1)"; rc=$?
  missing=""
  while read -r n; do
    printf '%s\n' "$out" | grep -q "libm function $n is " || missing="$missing $n"
  done < "$work/libm-names.txt"
  if [ "$rc" -eq 0 ]; then
    fail "libm/planted (every name) should fail but exited 0"
  elif [ -n "$missing" ]; then
    fail "libm/planted (every name) did not name:$missing"
  else
    printf '  ok   libm/planted (all %s names, every f and l form among them) fails, naming each\n' "$nnames"
  fi
  expect_fail 'libm/planted (sin defined, object)' 'libm function sin is defined' "$M" "$work/libm-defined.o"
  expect_fail 'libm/planted (sqrt defined, object)' 'libm function sqrt is defined' "$M" "$work/libm-defined2.o"
  expect_fail 'libm/planted (floor defined, object)' 'libm function floor is defined' "$M" "$work/libm-defined2.o"
  ar rcs "$work/libm-planted.a" "$work/libm-one-sqrt.o"
  expect_fail 'libm/planted (sqrt referenced, archive)' 'libm function sqrt is referenced' "$M" "$work/libm-planted.a"
  ar rcs "$work/libm-control.a" "$work/libm-control.o"
  expect_pass 'libm/control (an archive of the control)' "$M" "$work/libm-control.a"
  case "$(uname -s)" in
    Linux)
      if $CC -shared -fPIC -o "$work/libm-planted.so" "$work/libm-one-floor.o" -lm 2>"$work/libm-so.log"; then
        expect_fail 'libm/planted (floor from libm, shared object, version suffix stripped)' 'libm function floor is referenced' "$M" "$work/libm-planted.so"
      else
        fail "could not build the libm shared-object fixture"
      fi
      ;;
  esac
  # The gate has no exemption: the option is not accepted (it is read as a file that is not there).
  expect_fail 'libm/no exemption (--exempt is refused)' 'is not a file' "$M" --exempt "$work/libm-planted.o"
else
  fail "could not build the libm fixtures: $(cat "$work/libm.log")"
fi
expect_fail 'libm/empty (no files)' 'measuring nothing' "$M"
: > "$work/libm-nosyms.o"
expect_fail 'libm/empty (a file with no symbols)' 'read no symbols' "$M" "$work/libm-nosyms.o"

#---------------------------------------------------------------------------
printf 'check-no-x87\n'
X="$HERE/check-no-x87.sh"
case "$(uname -m)" in
  x86_64 | i386 | i486 | i586 | i686)
    cat > "$work/x87-control.c" <<'EOC'
double x87_control(double a, double b) { return a * b + a / b; }
EOC
    cat > "$work/x87-longdouble.c" <<'EOC'
long double x87_planted(long double a, long double b) { return a * b; }
EOC
    cat > "$work/x87-asm.c" <<'EOC'
void x87_asm(void) { __asm__ volatile("fld1\n\tfld1\n\tfaddp\n\tfstp %%st(0)" ::: "st", "st(1)"); }
EOC
    # The one allowed use of x87: moving a value that is already a double
    # (fldz and a pop, as a returned 0.0 and an ignored result are).
    cat > "$work/x87-abi.c" <<'EOC'
void x87_abi(void) { __asm__ volatile("fldz\n\tfstp %%st(0)" ::: "st"); }
EOC
    if $CC -O2 -msse2 -mfpmath=sse -ffp-contract=off -c "$work/x87-control.c" -o "$work/x87-control.o" 2>"$work/x87.log" \
      && $CC -O2 -c "$work/x87-longdouble.c" -o "$work/x87-longdouble.o" 2>>"$work/x87.log" \
      && $CC -O2 -c "$work/x87-asm.c" -o "$work/x87-asm.o" 2>>"$work/x87.log" \
      && $CC -O2 -c "$work/x87-abi.c" -o "$work/x87-abi.o" 2>>"$work/x87.log"; then
      expect_pass 'x87/control (double arithmetic in SSE)' "$X" "$work/x87-control.o"
      expect_fail 'x87/planted (long double arithmetic)' 'x87 instruction in' "$X" "$work/x87-longdouble.o"
      expect_fail 'x87/planted (long double names the file)' 'x87-longdouble.o' "$X" "$work/x87-longdouble.o"
      expect_pass 'x87/control (fldz and a pop: the ABI transfers, no arithmetic)' "$X" "$work/x87-abi.o"
      expect_fail 'x87/planted (hand-written faddp)' 'faddp' "$X" "$work/x87-asm.o"
      ar rcs "$work/x87-planted.a" "$work/x87-control.o" "$work/x87-asm.o"
      expect_fail 'x87/planted (inside an archive, with a clean member beside it)' 'faddp' "$X" "$work/x87-planted.a"
      expect_fail 'x87/planted (one bad file among clean ones)' 'faddp' "$X" "$work/x87-control.o" "$work/x87-asm.o"
      expect_pass 'x87/--require-x86 on an x86 file' "$X" --require-x86 "$work/x87-control.o"
    else
      fail "could not build the x87 fixtures: $(cat "$work/x87.log")"
    fi
    printf 'not an object\n' > "$work/x87-text.o"
    expect_fail 'x87/unreadable (a file objdump cannot read)' 'cannot be read' "$X" "$work/x87-text.o"
    ;;
  *)
    printf '  NOT RUN: the x87 plants need an x86 compiler; this host is %s\n' "$(uname -m)"
    ;;
esac
expect_fail 'x87/empty (no files)' 'measuring nothing' "$X"

#---------------------------------------------------------------------------
if [ "$failures" -ne 0 ]; then
  printf 'check-gates: %d of %d checks FAILED\n' "$failures" "$checks" >&2
  exit 1
fi
printf 'check-gates: %s: all %d checks passed; every gate fails on its planted violation\n' "$lib" "$checks"
