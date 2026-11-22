#!/usr/bin/env bash
# lang-bench — benchmark Penstock against idiomatic C++, hand-tuned C++
# and safe Rust using the Freshet-lang standards repository, and annotate
# every kernel where another language won.
#
# This is the machine-local twin of .github/workflows/freshet-benchmarks.yml:
# run it on any machine that has cloned the Penstock repository (the
# workflow file carries the same steps for CI). All heavy lifting is done
# by the standards repository's own drivers:
#
#   Freshet-lang/benchmarks/bench.py      build + checksum gate + timing
#   Freshet-lang/benchmarks/annotate.py   IR/disassembly/perf annotation
#
# usage:
#   tools/lang-bench/run.sh [--perf auto|force|off] [--kernels k1,k2]
#                            [--reps 5] [--freshet-lang PATH|URL]
#   env: FRESHET_LANG_REPO (default: https://github.com/OWNER/Freshet-lang)
#        CXX, RUSTC (optional toolchain overrides)
set -euo pipefail
cd "$(dirname "$0")/../.."

PERF=auto
KERNELS=
REPS=5
LANG_REPO=${FRESHET_LANG_REPO:-https://github.com/OWNER/Freshet-lang}
while [ $# -gt 0 ]; do
  case "$1" in
    --perf) PERF=$2; shift 2;;
    --kernels) KERNELS=$2; shift 2;;
    --reps) REPS=$2; shift 2;;
    --freshet-lang) LANG_REPO=$2; shift 2;;
    *) echo "unknown flag: $1" >&2; exit 2;;
  esac
done

# ---- locate or build the compiler --------------------------------------
PEN=${PENSTOCK:-}
if [ -z "$PEN" ]; then
  if [ -x bin/penstock ]; then PEN=bin/penstock; fi
  if [ -x build/bin/penstock ]; then PEN=build/bin/penstock; fi
  if [ -z "$PEN" ]; then
    echo "== building penstock (cmake)"
    cmake -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build -j"$(nproc)"
    PEN=build/bin/penstock
  fi
fi
PEN="$(cd "$(dirname "$PEN")" && pwd)/$(basename "$PEN")"
echo "== penstock: $PEN"

# ---- locate or clone the standards repository --------------------------
if [ -d "$LANG_REPO" ]; then
  LANG_DIR="$LANG_REPO"
elif [ -d Freshet-lang ]; then
  LANG_DIR=Freshet-lang
else
  echo "== cloning $LANG_REPO"
  git clone --depth 1 "$LANG_REPO" Freshet-lang
  LANG_DIR=Freshet-lang
fi
if [ ! -f "$LANG_DIR/benchmarks/bench.py" ]; then
  echo "no benchmarks/ under $LANG_DIR — is it the Freshet-lang repo?" >&2
  exit 2
fi

# ---- benchmark (checksum-gated) -----------------------------------------
echo "== benchmarks (freshet vs idiomatic/tuned C++ vs safe Rust)"
KFLAG=""
[ -n "$KERNELS" ] && KFLAG="--kernels $KERNELS"
python3 "$LANG_DIR/benchmarks/bench.py" \
  --penstock "$PEN" --reps "$REPS" $KFLAG --out "$(pwd)/results.json"

# ---- annotate the losses -------------------------------------------------
echo "== loss annotation (LLVM IR + disassembly + perf [$PERF])"
python3 "$LANG_DIR/benchmarks/annotate.py" \
  --results "$(pwd)/results.json" \
  --penstock "$PEN" --perf "$PERF" --out "$(pwd)/reports"

echo
echo "== results: results.md / results.json"
echo "== reports: reports/INDEX.md (+ reports/losses/*.md)"
sed -n '1,60p' reports/INDEX.md || true
