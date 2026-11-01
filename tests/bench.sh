#!/usr/bin/env bash
# Freshet benchmarks — best of 3 runs (sandbox noise mitigation)
set -u
cd "$(dirname "$0")/.."
PEN=bin/penstock
# C baseline uses the same toolchain penstock picked: explicit LLVM_ROOT,
# else newest shipped/installed LLVM (23 .. 19)
if [ -n "${LLVM_ROOT:-}" ]; then
  LLVM=$LLVM_ROOT
else
  LLVM=""
  for v in 23 22 21 20 19; do
    for d in ../tools/llvm$v/usr/lib/llvm-$v /usr/lib/llvm-$v; do
      [ -x "$d/bin/clang" ] && LLVM="$d" && break 2
    done
  done
fi
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# wall-clock milliseconds for "$@" (used by the R4 sections below)
ms() {
  local t0 t1
  t0=$(date +%s%N)
  "$@" >/dev/null 2>&1
  t1=$(date +%s%N)
  echo $(( (t1 - t0) / 1000000 ))
}

# wall-clock milliseconds for "$@" (used by the R4 sections below)
ms() {
  local t0 t1
  t0=$(date +%s%N)
  "$@" >/dev/null 2>&1
  t1=$(date +%s%N)
  echo $(( (t1 - t0) / 1000000 ))
}

# wall-clock milliseconds for "$@" (used by the R4 sections below)
ms() {
  local t0 t1
  t0=$(date +%s%N)
  "$@" >/dev/null 2>&1
  t1=$(date +%s%N)
  echo $(( (t1 - t0) / 1000000 ))
}

# wall-clock milliseconds for "$@" (used by the R4 sections below)
ms() {
  local t0 t1
  t0=$(date +%s%N)
  "$@" >/dev/null 2>&1
  t1=$(date +%s%N)
  echo $(( (t1 - t0) / 1000000 ))
}

# wall-clock milliseconds for "$@" (used by the R4 sections below)
ms() {
  local t0 t1
  t0=$(date +%s%N)
  "$@" >/dev/null 2>&1
  t1=$(date +%s%N)
  echo $(( (t1 - t0) / 1000000 ))
}

# wall-clock milliseconds for "$@" (used by the R4 sections below)
ms() {
  local t0 t1
  t0=$(date +%s%N)
  "$@" >/dev/null 2>&1
  t1=$(date +%s%N)
  echo $(( (t1 - t0) / 1000000 ))
}

# wall-clock milliseconds for "$@" (used by the R4 sections below)
ms() {
  local t0 t1
  t0=$(date +%s%N)
  "$@" >/dev/null 2>&1
  t1=$(date +%s%N)
  echo $(( (t1 - t0) / 1000000 ))
}

echo "== Freshet AoS vs SoA (8-field struct, 2 hot fields, 4M elems x10) =="
$PEN tests/bench_soa.fsh -o "$TMP/bs" 2>/dev/null
best_aos=999999; best_soa=999999
for i in 1 2 3; do
  line=$("$TMP/bs" 2>/dev/null)
  aos=$(echo "$line" | sed -n 's/.*~\([0-9]*\) ms | SoA.*/\1/p')
  soa=$(echo "$line" | sed -n 's/.*SoA sum=[0-9.]*  ~\([0-9]*\) ms.*/\1/p')
  [ "${aos:-999}" -lt "$best_aos" ] && best_aos=$aos
  [ "${soa:-999}" -lt "$best_soa" ] && best_soa=$soa
done
echo "freshet AoS best : ${best_aos} ms"
echo "freshet SoA best : ${best_soa} ms   (in-compiler speedup $(python3 -c "print(f'{$best_aos/$best_soa:.2f}x')"))"

echo
echo "== C baseline (clang -O3 -march=native, serial) =="
$LLVM/bin/clang -O3 -march=native tests/bench_soa_c.c -o "$TMP/bsc" -lm
"$TMP/bsc"
"$TMP/bsc"

echo
echo "== R4 bench: loop fusion (FFT-shaped batch pipeline, best of 3) =="
# optimised build (default -O3: fusion + unroll + cross-lane on) vs
# baseline (--no-loop-opts).  Checksums must agree: source-level loop
# transforms are output-preserving by construction.
$PEN tests/bench_fuse.fsh -o "$TMP/bf" 2>/dev/null
$PEN --no-loop-opts tests/bench_fuse.fsh -o "$TMP/bf0" 2>/dev/null
if [ -x "$TMP/bf" ] && [ -x "$TMP/bf0" ]; then
  s1=$("$TMP/bf" 2>/dev/null); s0=$("$TMP/bf0" 2>/dev/null)
  if [ "$s1" = "$s0" ]; then echo "checksums agree ($s1)"
  else echo "CHECKSUM MISMATCH: opt='$s1' baseline='$s0'"; fi
  best_f=999999; best_f0=999999
  for i in 1 2 3; do
    t=$(ms "$TMP/bf");  [ "${t:-999999}" -lt "$best_f" ]  && best_f=$t
    t=$(ms "$TMP/bf0"); [ "${t:-999999}" -lt "$best_f0" ] && best_f0=$t
  done
  echo "fusion on  best : ${best_f} ms"
  echo "fusion off best : ${best_f0} ms   (speedup $(python3 -c "print(f'{$best_f0/$best_f:.2f}x')"))"
else
  echo "skipped (build failed)"
fi

echo
echo "== R4 bench: cross-lane unswitch (uniform branch, best of 3) =="
$PEN tests/bench_lane.fsh -o "$TMP/bl" 2>/dev/null
$PEN --no-loop-opts tests/bench_lane.fsh -o "$TMP/bl0" 2>/dev/null
if [ -x "$TMP/bl" ] && [ -x "$TMP/bl0" ]; then
  l1=$("$TMP/bl" 2>/dev/null); l0=$("$TMP/bl0" 2>/dev/null)
  if [ "$l1" = "$l0" ]; then echo "checksums agree ($l1)"
  else echo "CHECKSUM MISMATCH: opt='$l1' baseline='$l0'"; fi
  best_l=999999; best_l0=999999
  for i in 1 2 3; do
    t=$(ms "$TMP/bl");  [ "${t:-999999}" -lt "$best_l" ]  && best_l=$t
    t=$(ms "$TMP/bl0"); [ "${t:-999999}" -lt "$best_l0" ] && best_l0=$t
  done
  echo "unswitch on  best : ${best_l} ms"
  echo "unswitch off best : ${best_l0} ms   (speedup $(python3 -c "print(f'{$best_l0/$best_l:.2f}x')"))"
else
  echo "skipped (build failed)"
fi

echo
echo "== R4 bench: loop fusion (FFT-shaped batch pipeline, best of 3) =="
# optimised build (default -O3: fusion + unroll + cross-lane on) vs
# baseline (--no-loop-opts).  Checksums must agree: source-level loop
# transforms are output-preserving by construction.
$PEN tests/bench_fuse.fsh -o "$TMP/bf" 2>/dev/null
$PEN --no-loop-opts tests/bench_fuse.fsh -o "$TMP/bf0" 2>/dev/null
if [ -x "$TMP/bf" ] && [ -x "$TMP/bf0" ]; then
  s1=$("$TMP/bf" 2>/dev/null); s0=$("$TMP/bf0" 2>/dev/null)
  if [ "$s1" = "$s0" ]; then echo "checksums agree ($s1)"
  else echo "CHECKSUM MISMATCH: opt='$s1' baseline='$s0'"; fi
  best_f=999999; best_f0=999999
  for i in 1 2 3; do
    t=$(ms "$TMP/bf");  [ "${t:-999999}" -lt "$best_f" ]  && best_f=$t
    t=$(ms "$TMP/bf0"); [ "${t:-999999}" -lt "$best_f0" ] && best_f0=$t
  done
  echo "fusion on  best : ${best_f} ms"
  echo "fusion off best : ${best_f0} ms   (speedup $(python3 -c "print(f'{$best_f0/$best_f:.2f}x')"))"
else
  echo "skipped (build failed)"
fi

echo
echo "== R4 bench: cross-lane unswitch (uniform branch, best of 3) =="
$PEN tests/bench_lane.fsh -o "$TMP/bl" 2>/dev/null
$PEN --no-loop-opts tests/bench_lane.fsh -o "$TMP/bl0" 2>/dev/null
if [ -x "$TMP/bl" ] && [ -x "$TMP/bl0" ]; then
  l1=$("$TMP/bl" 2>/dev/null); l0=$("$TMP/bl0" 2>/dev/null)
  if [ "$l1" = "$l0" ]; then echo "checksums agree ($l1)"
  else echo "CHECKSUM MISMATCH: opt='$l1' baseline='$l0'"; fi
  best_l=999999; best_l0=999999
  for i in 1 2 3; do
    t=$(ms "$TMP/bl");  [ "${t:-999999}" -lt "$best_l" ]  && best_l=$t
    t=$(ms "$TMP/bl0"); [ "${t:-999999}" -lt "$best_l0" ] && best_l0=$t
  done
  echo "unswitch on  best : ${best_l} ms"
  echo "unswitch off best : ${best_l0} ms   (speedup $(python3 -c "print(f'{$best_l0/$best_l:.2f}x')"))"
else
  echo "skipped (build failed)"
fi

echo
echo "== R4 bench: loop fusion (FFT-shaped batch pipeline, best of 3) =="
# optimised build (default -O3: fusion + unroll + cross-lane on) vs
# baseline (--no-loop-opts).  Checksums must agree: source-level loop
# transforms are output-preserving by construction.
$PEN tests/bench_fuse.fsh -o "$TMP/bf" 2>/dev/null
$PEN --no-loop-opts tests/bench_fuse.fsh -o "$TMP/bf0" 2>/dev/null
if [ -x "$TMP/bf" ] && [ -x "$TMP/bf0" ]; then
  s1=$("$TMP/bf" 2>/dev/null); s0=$("$TMP/bf0" 2>/dev/null)
  if [ "$s1" = "$s0" ]; then echo "checksums agree ($s1)"
  else echo "CHECKSUM MISMATCH: opt='$s1' baseline='$s0'"; fi
  best_f=999999; best_f0=999999
  for i in 1 2 3; do
    t=$(ms "$TMP/bf");  [ "${t:-999999}" -lt "$best_f" ]  && best_f=$t
    t=$(ms "$TMP/bf0"); [ "${t:-999999}" -lt "$best_f0" ] && best_f0=$t
  done
  echo "fusion on  best : ${best_f} ms"
  echo "fusion off best : ${best_f0} ms   (speedup $(python3 -c "print(f'{$best_f0/$best_f:.2f}x')"))"
else
  echo "skipped (build failed)"
fi

echo
echo "== R4 bench: cross-lane unswitch (uniform branch, best of 3) =="
$PEN tests/bench_lane.fsh -o "$TMP/bl" 2>/dev/null
$PEN --no-loop-opts tests/bench_lane.fsh -o "$TMP/bl0" 2>/dev/null
if [ -x "$TMP/bl" ] && [ -x "$TMP/bl0" ]; then
  l1=$("$TMP/bl" 2>/dev/null); l0=$("$TMP/bl0" 2>/dev/null)
  if [ "$l1" = "$l0" ]; then echo "checksums agree ($l1)"
  else echo "CHECKSUM MISMATCH: opt='$l1' baseline='$l0'"; fi
  best_l=999999; best_l0=999999
  for i in 1 2 3; do
    t=$(ms "$TMP/bl");  [ "${t:-999999}" -lt "$best_l" ]  && best_l=$t
    t=$(ms "$TMP/bl0"); [ "${t:-999999}" -lt "$best_l0" ] && best_l0=$t
  done
  echo "unswitch on  best : ${best_l} ms"
  echo "unswitch off best : ${best_l0} ms   (speedup $(python3 -c "print(f'{$best_l0/$best_l:.2f}x')"))"
else
  echo "skipped (build failed)"
fi

echo
echo "== R4 bench: loop fusion (FFT-shaped batch pipeline, best of 3) =="
# optimised build (default -O3: fusion + unroll + cross-lane on) vs
# baseline (--no-loop-opts).  Checksums must agree: source-level loop
# transforms are output-preserving by construction.
$PEN tests/bench_fuse.fsh -o "$TMP/bf" 2>/dev/null
$PEN --no-loop-opts tests/bench_fuse.fsh -o "$TMP/bf0" 2>/dev/null
if [ -x "$TMP/bf" ] && [ -x "$TMP/bf0" ]; then
  s1=$("$TMP/bf" 2>/dev/null); s0=$("$TMP/bf0" 2>/dev/null)
  if [ "$s1" = "$s0" ]; then echo "checksums agree ($s1)"
  else echo "CHECKSUM MISMATCH: opt='$s1' baseline='$s0'"; fi
  best_f=999999; best_f0=999999
  for i in 1 2 3; do
    t=$(ms "$TMP/bf");  [ "${t:-999999}" -lt "$best_f" ]  && best_f=$t
    t=$(ms "$TMP/bf0"); [ "${t:-999999}" -lt "$best_f0" ] && best_f0=$t
  done
  echo "fusion on  best : ${best_f} ms"
  echo "fusion off best : ${best_f0} ms   (speedup $(python3 -c "print(f'{$best_f0/$best_f:.2f}x')"))"
else
  echo "skipped (build failed)"
fi

echo
echo "== R4 bench: cross-lane unswitch (uniform branch, best of 3) =="
$PEN tests/bench_lane.fsh -o "$TMP/bl" 2>/dev/null
$PEN --no-loop-opts tests/bench_lane.fsh -o "$TMP/bl0" 2>/dev/null
if [ -x "$TMP/bl" ] && [ -x "$TMP/bl0" ]; then
  l1=$("$TMP/bl" 2>/dev/null); l0=$("$TMP/bl0" 2>/dev/null)
  if [ "$l1" = "$l0" ]; then echo "checksums agree ($l1)"
  else echo "CHECKSUM MISMATCH: opt='$l1' baseline='$l0'"; fi
  best_l=999999; best_l0=999999
  for i in 1 2 3; do
    t=$(ms "$TMP/bl");  [ "${t:-999999}" -lt "$best_l" ]  && best_l=$t
    t=$(ms "$TMP/bl0"); [ "${t:-999999}" -lt "$best_l0" ] && best_l0=$t
  done
  echo "unswitch on  best : ${best_l} ms"
  echo "unswitch off best : ${best_l0} ms   (speedup $(python3 -c "print(f'{$best_l0/$best_l:.2f}x')"))"
else
  echo "skipped (build failed)"
fi

echo
echo "== R4 bench: loop fusion (FFT-shaped batch pipeline, best of 3) =="
# optimised build (default -O3: fusion + unroll + cross-lane on) vs
# baseline (--no-loop-opts).  Checksums must agree: source-level loop
# transforms are output-preserving by construction.
$PEN tests/bench_fuse.fsh -o "$TMP/bf" 2>/dev/null
$PEN --no-loop-opts tests/bench_fuse.fsh -o "$TMP/bf0" 2>/dev/null
if [ -x "$TMP/bf" ] && [ -x "$TMP/bf0" ]; then
  s1=$("$TMP/bf" 2>/dev/null); s0=$("$TMP/bf0" 2>/dev/null)
  if [ "$s1" = "$s0" ]; then echo "checksums agree ($s1)"
  else echo "CHECKSUM MISMATCH: opt='$s1' baseline='$s0'"; fi
  best_f=999999; best_f0=999999
  for i in 1 2 3; do
    t=$(ms "$TMP/bf");  [ "${t:-999999}" -lt "$best_f" ]  && best_f=$t
    t=$(ms "$TMP/bf0"); [ "${t:-999999}" -lt "$best_f0" ] && best_f0=$t
  done
  echo "fusion on  best : ${best_f} ms"
  echo "fusion off best : ${best_f0} ms   (speedup $(python3 -c "print(f'{$best_f0/$best_f:.2f}x')"))"
else
  echo "skipped (build failed)"
fi

echo
echo "== R4 bench: cross-lane unswitch (uniform branch, best of 3) =="
$PEN tests/bench_lane.fsh -o "$TMP/bl" 2>/dev/null
$PEN --no-loop-opts tests/bench_lane.fsh -o "$TMP/bl0" 2>/dev/null
if [ -x "$TMP/bl" ] && [ -x "$TMP/bl0" ]; then
  l1=$("$TMP/bl" 2>/dev/null); l0=$("$TMP/bl0" 2>/dev/null)
  if [ "$l1" = "$l0" ]; then echo "checksums agree ($l1)"
  else echo "CHECKSUM MISMATCH: opt='$l1' baseline='$l0'"; fi
  best_l=999999; best_l0=999999
  for i in 1 2 3; do
    t=$(ms "$TMP/bl");  [ "${t:-999999}" -lt "$best_l" ]  && best_l=$t
    t=$(ms "$TMP/bl0"); [ "${t:-999999}" -lt "$best_l0" ] && best_l0=$t
  done
  echo "unswitch on  best : ${best_l} ms"
  echo "unswitch off best : ${best_l0} ms   (speedup $(python3 -c "print(f'{$best_l0/$best_l:.2f}x')"))"
else
  echo "skipped (build failed)"
fi

echo
echo "== R4 bench: loop fusion (FFT-shaped batch pipeline, best of 3) =="
# optimised build (default -O3: fusion + unroll + cross-lane on) vs
# baseline (--no-loop-opts).  Checksums must agree: source-level loop
# transforms are output-preserving by construction.
$PEN tests/bench_fuse.fsh -o "$TMP/bf" 2>/dev/null
$PEN --no-loop-opts tests/bench_fuse.fsh -o "$TMP/bf0" 2>/dev/null
if [ -x "$TMP/bf" ] && [ -x "$TMP/bf0" ]; then
  s1=$("$TMP/bf" 2>/dev/null); s0=$("$TMP/bf0" 2>/dev/null)
  if [ "$s1" = "$s0" ]; then echo "checksums agree ($s1)"
  else echo "CHECKSUM MISMATCH: opt='$s1' baseline='$s0'"; fi
  best_f=999999; best_f0=999999
  for i in 1 2 3; do
    t=$(ms "$TMP/bf");  [ "${t:-999999}" -lt "$best_f" ]  && best_f=$t
    t=$(ms "$TMP/bf0"); [ "${t:-999999}" -lt "$best_f0" ] && best_f0=$t
  done
  echo "fusion on  best : ${best_f} ms"
  echo "fusion off best : ${best_f0} ms   (speedup $(python3 -c "print(f'{$best_f0/$best_f:.2f}x')"))"
else
  echo "skipped (build failed)"
fi

echo
echo "== R4 bench: cross-lane unswitch (uniform branch, best of 3) =="
$PEN tests/bench_lane.fsh -o "$TMP/bl" 2>/dev/null
$PEN --no-loop-opts tests/bench_lane.fsh -o "$TMP/bl0" 2>/dev/null
if [ -x "$TMP/bl" ] && [ -x "$TMP/bl0" ]; then
  l1=$("$TMP/bl" 2>/dev/null); l0=$("$TMP/bl0" 2>/dev/null)
  if [ "$l1" = "$l0" ]; then echo "checksums agree ($l1)"
  else echo "CHECKSUM MISMATCH: opt='$l1' baseline='$l0'"; fi
  best_l=999999; best_l0=999999
  for i in 1 2 3; do
    t=$(ms "$TMP/bl");  [ "${t:-999999}" -lt "$best_l" ]  && best_l=$t
    t=$(ms "$TMP/bl0"); [ "${t:-999999}" -lt "$best_l0" ] && best_l0=$t
  done
  echo "unswitch on  best : ${best_l} ms"
  echo "unswitch off best : ${best_l0} ms   (speedup $(python3 -c "print(f'{$best_l0/$best_l:.2f}x')"))"
else
  echo "skipped (build failed)"
fi

echo
echo "== R4 bench: loop fusion (FFT-shaped batch pipeline, best of 3) =="
# optimised build (default -O3: fusion + unroll + cross-lane on) vs
# baseline (--no-loop-opts).  Checksums must agree: source-level loop
# transforms are output-preserving by construction.
$PEN tests/bench_fuse.fsh -o "$TMP/bf" 2>/dev/null
$PEN --no-loop-opts tests/bench_fuse.fsh -o "$TMP/bf0" 2>/dev/null
if [ -x "$TMP/bf" ] && [ -x "$TMP/bf0" ]; then
  s1=$("$TMP/bf" 2>/dev/null); s0=$("$TMP/bf0" 2>/dev/null)
  if [ "$s1" = "$s0" ]; then echo "checksums agree ($s1)"
  else echo "CHECKSUM MISMATCH: opt='$s1' baseline='$s0'"; fi
  best_f=999999; best_f0=999999
  for i in 1 2 3; do
    t=$(ms "$TMP/bf");  [ "${t:-999999}" -lt "$best_f" ]  && best_f=$t
    t=$(ms "$TMP/bf0"); [ "${t:-999999}" -lt "$best_f0" ] && best_f0=$t
  done
  echo "fusion on  best : ${best_f} ms"
  echo "fusion off best : ${best_f0} ms   (speedup $(python3 -c "print(f'{$best_f0/$best_f:.2f}x')"))"
else
  echo "skipped (build failed)"
fi

echo
echo "== R4 bench: cross-lane unswitch (uniform branch, best of 3) =="
$PEN tests/bench_lane.fsh -o "$TMP/bl" 2>/dev/null
$PEN --no-loop-opts tests/bench_lane.fsh -o "$TMP/bl0" 2>/dev/null
if [ -x "$TMP/bl" ] && [ -x "$TMP/bl0" ]; then
  l1=$("$TMP/bl" 2>/dev/null); l0=$("$TMP/bl0" 2>/dev/null)
  if [ "$l1" = "$l0" ]; then echo "checksums agree ($l1)"
  else echo "CHECKSUM MISMATCH: opt='$l1' baseline='$l0'"; fi
  best_l=999999; best_l0=999999
  for i in 1 2 3; do
    t=$(ms "$TMP/bl");  [ "${t:-999999}" -lt "$best_l" ]  && best_l=$t
    t=$(ms "$TMP/bl0"); [ "${t:-999999}" -lt "$best_l0" ] && best_l0=$t
  done
  echo "unswitch on  best : ${best_l} ms"
  echo "unswitch off best : ${best_l0} ms   (speedup $(python3 -c "print(f'{$best_l0/$best_l:.2f}x')"))"
else
  echo "skipped (build failed)"
fi
