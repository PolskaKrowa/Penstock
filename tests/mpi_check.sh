#!/usr/bin/env bash
# hybrid MPI + parfor
# conformance runner.  Compiles tests/mpi_conformance.fsht with Penstock
# (#link mpi) and runs it under mpirun with 4 ranks; rank 0's final line
# must be MPI_CONFORMANCE_OK.
#
# usage: tests/mpi_check.sh [penstock-path] [mpirun-path] [ranks]
# Exit codes: 0 pass, 1 fail, 77 SKIP (no mpirun/penstock — the CI
# matrix treats 77 as not-run, not failed).
set -u
PEN=${1:-bin/penstock}
MPIRUN=${2:-$(command -v mpirun || command -v mpiexec || true)}
RANKS=${3:-4}
cd "$(dirname "$0")/.."

if [ ! -x "$PEN" ]; then
  echo "mpi_check: penstock not found at $PEN — SKIP"
  exit 77
fi
if [ -z "$MPIRUN" ] || [ ! -x "$MPIRUN" ]; then
  echo "mpi_check: no mpirun/mpiexec on PATH — SKIP"
  exit 77
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
if ! "$PEN" tests/mpi_conformance.fsht -o "$TMP/mpi_conf" 2>"$TMP/err"; then
  echo "mpi_check: compile failed:"; sed -n '1,5p' "$TMP/err"
  exit 1
fi

if "$MPIRUN" -np "$RANKS" "$TMP/mpi_conf" >"$TMP/out" 2>"$TMP/err" \
    || "$MPIRUN" -np "$RANKS" --allow-run-as-root \
         "$TMP/mpi_conf" >"$TMP/out" 2>"$TMP/err"; then
  if grep -q "MPI_CONFORMANCE_OK" "$TMP/out" \
     && [ "$(grep -c 'after 64.00' "$TMP/out")" -eq "$RANKS" ]; then
    echo "mpi_check: $RANKS rank(s) PASSED (extern fn MPI between parfor \
regions; idle workers did not interfere)"
    exit 0
  fi
fi
echo "mpi_check: FAILED — output:"; sed -n '1,8p' "$TMP/out" 2>/dev/null
sed -n '1,5p' "$TMP/err" 2>/dev/null
exit 1
