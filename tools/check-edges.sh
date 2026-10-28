#!/bin/sh
#
# Fail on a dependency edge the shape family's spine forbids, in either place
# it can hide.
#
# AD-2: a library includes the headers of, and links the shared objects of, the
# libraries its manifest line names and nothing else. `math` reaches `cutil`
# only; `shape-geometry` reaches `cutil` and `math`; `shape-scene` reaches no
# sink; no sink reaches a front-end or another sink; `font` is never a
# dependency of any of them.
#
# The rule is checked twice because the two checks see different things: a
# manifest and a clean #include list can both be right while the shared object
# still links a forbidden library, and an #include can be forbidden while the
# link line is clean.
#
# The check is an allowlist, not a list of known-bad names: any
# <ghoti.io/X/...> include or libghoti.io-X NEEDED entry whose X is neither the
# library itself nor in --allowed is an edge. A new forbidden library then needs
# no edit here, and a typo in a deny list cannot let one through.
#
# Usage:
#   check-edges.sh --allowed 'a|b' --self NAME --includes <root>
#       scan <root>/src, include, examples and bench for #include
#   check-edges.sh --allowed 'a|b' --self NAME --links <dir>
#       read the NEEDED list of every shared object under <dir>
#
# --allowed is an extended-regex alternation of library names. Each mode fails
# on an empty population.
#
# The same text is in math, shape-geometry, shape-scene and shape-cpu;
# suite/tools/xarch/shape.sh fails when the four copies differ.

set -eu

usage='usage: check-edges.sh --allowed <a|b> --self <name> (--includes <root> | --links <dir>)'

allowed="" self="" mode="" target=""
while [ $# -gt 0 ]; do
  case "$1" in
    --allowed) allowed="${2:?$usage}"; shift 2 ;;
    --self) self="${2:?$usage}"; shift 2 ;;
    --includes | --links) mode="$1"; target="${2:?$usage}"; shift 2 ;;
    *) printf '%s\n' "$usage" >&2; exit 2 ;;
  esac
done
if [ -z "$allowed" ] || [ -z "$self" ] || [ -z "$mode" ]; then
  printf '%s\n' "$usage" >&2
  exit 2
fi

ALLOWED="$self|$allowed"
# What may follow an allowed name in a NEEDED entry: the BRANCH suffix of a
# build that named one (cutil-dev). A different library whose name merely
# begins with an allowed one (cutil-extra) is an edge. GHOTI_BRANCH (for
# example -nightly) adds the suffix of a build with a branch of its own.
SUFFIXES='-dev'
if [ -n "${GHOTI_BRANCH:-}" ]; then
  SUFFIXES="$SUFFIXES|$(printf '%s' "${GHOTI_BRANCH}" | sed 's/[][\.^$*+?(){}|\/]/\\&/g')"
fi
status=0

case "$mode" in
  --includes)
    files=""
    for d in "$target/src" "$target/include" "$target/examples" "$target/bench"; do
      if [ -d "$d" ]; then
        found="$(find "$d" -type f \( -name '*.c' -o -name '*.h' \
          -o -name '*.cpp' \) | sort)"
        files="$files
$found"
      fi
    done
    files="$(printf '%s\n' "$files" | sed '/^$/d')"
    if [ -z "$files" ]; then
      printf 'check-edges: no sources under %s/src or %s/include; this gate is measuring nothing\n' \
        "$target" "$target" >&2
      exit 1
    fi
    count=0
    for f in $files; do
      count=$((count + 1))
      hits="$(grep -nE '^[[:space:]]*#[[:space:]]*include[^"<]*[<"][^>"]*ghoti\.io/[^/>"]+/' "$f" || true)"
      [ -n "$hits" ] || continue
      while IFS= read -r line; do
        n="${line%%:*}"
        lib="$(printf '%s\n' "$line" \
          | sed -E 's/.*ghoti\.io\/([^\/>"]+)\/.*/\1/')"
        if ! printf '%s\n' "$lib" | grep -qE "^($ALLOWED)\$"; then
          printf 'check-edges: forbidden edge %s -> %s: %s:%s: %s\n' \
            "$self" "$lib" "$f" "$n" "${line#*:}" >&2
          status=1
        fi
      done <<HITS
$hits
HITS
    done
    if [ "$status" -ne 0 ]; then
      exit 1
    fi
    printf 'check-edges: %s: %d source files, every #include of another Ghoti library is in {%s}\n' \
      "$self" "$count" "$allowed"
    ;;

  --links)
    objects="$(find "$target" -type f \( -name '*.so' -o -name '*.so.*' \
      -o -name '*.dll' -o -name '*.dylib' \) 2>/dev/null | sort)"
    if [ -z "$objects" ]; then
      printf 'check-edges: no shared objects under %s; this gate is measuring nothing\n' \
        "$target" >&2
      exit 1
    fi
    count=0
    for so in $objects; do
      count=$((count + 1))
      if [ "$(uname -s)" = Darwin ] && command -v otool >/dev/null 2>&1; then
        needed="$(otool -L "$so" | sed '1d' | awk '{print $1}' \
          | sed 's|.*/||')"
      elif command -v readelf >/dev/null 2>&1 \
        && readelf -d "$so" >/dev/null 2>&1; then
        needed="$(readelf -d "$so" \
          | sed -n 's/.*Shared library: \[\(.*\)\].*/\1/p')"
      elif command -v objdump >/dev/null 2>&1; then
        # objdump -p reads both ELF (NEEDED) and PE (DLL Name:), which is what
        # a Windows build has.
        needed="$(objdump -p "$so" \
          | awk '$1 == "NEEDED" {print $2} /DLL Name:/ {print $3}')"
      else
        printf 'check-edges: neither readelf nor objdump is available; cannot read %s\n' \
          "$so" >&2
        exit 1
      fi
      if [ -z "$needed" ]; then
        # A real shared object links at least libc. An empty answer is a
        # reader that read nothing, which would pass any planted link.
        printf 'check-edges: read no dependencies from %s; the reader is not seeing the link line\n' \
          "$so" >&2
        exit 1
      fi
      for dep in $needed; do
        case "$dep" in
          libghoti.io-*)
            lib="${dep#libghoti.io-}"
            # libghoti.io-shape-geometry-0.so.0 -> shape-geometry.
            lib="$(printf '%s\n' "$lib" \
              | sed -E 's/(-[0-9]+)?(-debug)?(\.so.*|\.dll.*|\.dylib.*)$//')"
            # Exactly an allowed name, or one followed by a BRANCH suffix
            # (cutil-dev, cutil-0-debug); never a different library that
            # merely starts with one.
            if ! printf '%s\n' "$lib" | grep -qE "^($ALLOWED)($SUFFIXES)?\$"; then
              printf 'check-edges: forbidden edge %s -> %s: %s has NEEDED %s\n' \
                "$self" "$lib" "$so" "$dep" >&2
              status=1
            fi
            ;;
        esac
      done
    done
    if [ "$status" -ne 0 ]; then
      exit 1
    fi
    printf 'check-edges: %s: %d shared objects, every Ghoti library they link is in {%s}\n' \
      "$self" "$count" "$allowed"
    ;;
esac
