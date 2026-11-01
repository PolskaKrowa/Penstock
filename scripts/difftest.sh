#!/usr/bin/env bash
# Differential optimisation testing (optimisation-audit §10.2).
#
# For every positive test program: compile and run the SAME program under
# several optimisation configurations and compare the FULL observable
# stdout — not a checksum.  Any disagreement is a miscompile.
#
#   baseline      -O0                                   (no source passes)
#   default       -O2                                   (fusion/unroll/lane + constfold)
#   noloopts      -O2 --no-loop-opts                    (differential hatch)
#   o3            -O3                                   (full pipeline)
#   phaseD        -O2 --licm --partial-unroll --peel --interchange --tile \
#                     --distribute --unroll-and-jam     (all new transforms)
#
# usage: scripts/difftest.sh [test files...]   (default: tests/pos_*.fsh)
set -u
cd "$(dirname "$0")/.."
PEN=bin/penstock
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
pass=0; fail=0; skip=0

if [ ! -x "$PEN" ]; then echo "difftest: $PEN not built" >&2; exit 1; fi

files=("$@")
if [ ${#files[@]} -eq 0 ]; then files=(tests/pos_*.fsh); fi

run_cfg() {  # name, flags...
  local name=$1; shift
  if ! $PEN "$@" "$src" -o "$TMP/$name" 2>"$TMP/$name.err"; then
    echo "COMPILE-FAIL($name)"; return
  fi
  "$TMP/$name" 2>/dev/null
}

for src in "${files[@]}"; do
  base=$(basename "$src" .fsh)
  a=$(run_cfg base -O0)
  [ "$a" = "COMPILE-FAIL(-O0)" ] && { skip=$((skip+1)); continue; }
  b=$(run_cfg def -O2)
  c=$(run_cfg noloopts -O2 --no-loop-opts)
  d=$(run_cfg o3 -O3)
  e=$(run_cfg phaseD -O2 --licm --partial-unroll --peel --interchange \
        --tile --distribute --unroll-and-jam)
  if [ "$a" = "$b" ] && [ "$a" = "$c" ] && [ "$a" = "$d" ] && [ "$a" = "$e" ]; then
    pass=$((pass+1))
  else
    fail=$((fail+1))
    echo "  DIFF  $base"
    echo "    -O0        : $(echo "$a" | head -2 | tr '\n' ' ')"
    echo "    -O2        : $(echo "$b" | head -2 | tr '\n' ' ')"
    echo "    -O2-noopt  : $(echo "$c" | head -2 | tr '\n' ' ')"
    echo "    -O3        : $(echo "$d" | head -2 | tr '\n' ' ')"
    echo "    -O2-phaseD : $(echo "$e" | head -2 | tr '\n' ' ')"
  fi
done
echo "difftest: passed $pass, differed $fail, skipped $skip"
[ $fail -eq 0 ]
