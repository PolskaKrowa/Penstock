#!/usr/bin/env bash
# Lean proof-harness CI check (optimisation-audit Phase B).
#
# Runs the Penstock semantic kernel through the Lean 4 elaborator and
# FAILS on any `sorry` left in the file, so an open proof obligation can
# never silently pass as "verified".  Requires `lean` (or `lake`) on PATH.
#
# usage: scripts/check-lean.sh
set -u
cd "$(dirname "$0")/.."

LEAN_BIN="${LEAN:-lean}"
if ! command -v "$LEAN_BIN" >/dev/null 2>&1; then
  echo "check-lean: no Lean 4 toolchain found (set LEAN=/path/to/lean)" >&2
  echo "check-lean: the proof harness is SKIPPED, not verified" >&2
  exit 2
fi

OUT=$(mktemp)
trap 'rm -f "$OUT"' EXIT

echo "check-lean: elaborating lean/PenstockProofs.lean ..."
if ! "$LEAN_BIN" lean/PenstockProofs.lean 2>&1 | tee "$OUT"; then
  echo "check-lean: FAILED — the file does not elaborate" >&2
  exit 1
fi
if grep -n "declaration uses 'sorry'" "$OUT" >/dev/null; then
  echo "check-lean: FAILED — unresolved 'sorry' obligations:" >&2
  grep -n "declaration uses 'sorry'" "$OUT" >&2
  exit 1
fi
echo "check-lean: OK — kernel checked, no sorry obligations"
