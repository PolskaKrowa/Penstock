#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: $0 /path/to/opt /path/to/PenstockOptPlugin.so work-dir" >&2
  exit 2
fi
OPT=$1
PLUGIN=$2
WORK=$3
mkdir -p "$WORK"
cat > "$WORK/input.ll" <<'IR'
define i32 @main() {
entry:
  %answer = add i32 40, 2
  ret i32 %answer
}
IR

"$OPT" -load-pass-plugin="$PLUGIN" -passes=penstock-o4 -verify-each \
  -S "$WORK/input.ll" -o "$WORK/output.ll"
grep -Eq 'ret i32 42' "$WORK/output.ll"
echo "PASS: opt loaded PenstockO4Plugin and ran penstock-o4"
