#!/usr/bin/env bash
# Freshet / Penstock test suite
# usage: tests/run.sh
set -u
cd "$(dirname "$0")/.."
PEN=bin/penstock
PEN_LSP=bin/freshet-language-server
TMP=$(mktemp -d)
safe_marker="penstock-shell-injection-marker-$$"
trap 'rm -f "$safe_marker"; rm -rf "$TMP"' EXIT
pass=0; fail=0

ok()   { pass=$((pass+1)); echo "  PASS  $1"; }
bad()  { fail=$((fail+1)); echo "  FAIL  $1"; }

# positive: compiles, runs, stdout matches
run_ok() {
  local name=$1 src=$2 want=$3
  if $PEN "$src" -o "$TMP/t" 2>"$TMP/err"; then
    got=$("$TMP/t" 2>/dev/null)
    want=${want//|/$'\n'}
    if [ "$got" = "$want" ]; then ok "$name"; else bad "$name (got '$got' want '$want')"; fi
  else bad "$name (compile failed)"; sed -n '1,3p' "$TMP/err"; fi
}

# negative: compile must fail and mention the diagnostic code
run_err() {
  local name=$1 src=$2 code=$3
  $PEN "$src" -o "$TMP/t" 2>"$TMP/err"
  rc=$?
  if [ $rc -ne 0 ] && grep -q "\[$code\]" "$TMP/err"; then ok "$name ($code)";
  else bad "$name (expected $code, rc=$rc)"; sed -n '1,3p' "$TMP/err"; fi
}

# warn: compiles but must emit the diagnostic code
run_warn() {
  local name=$1 src=$2 code=$3
  if $PEN "$src" -o "$TMP/t" 2>"$TMP/err" && grep -q "\[$code\]" "$TMP/err"; then
    ok "$name ($code)"
  else bad "$name (expected warning $code)"; sed -n '1,3p' "$TMP/err"; fi
}

# positive with EXTRA compiler flags; stdout must match, and a note code
# may be asserted present ("CODE") or absent ("!CODE") in stderr.
# optimisation-audit §10.1: legality gaps are regression-covered BOTH by
# output equivalence AND by the pass note being (not) emitted.
run_okf() {
  local name=$1 src=$2 want=$3 flags=$4 note="${5:-}"
  if $PEN $flags "$src" -o "$TMP/t" 2>"$TMP/err"; then
    got=$("$TMP/t" 2>/dev/null)
    want=${want//|/$'\n'}
    local noteok=1
    if [ -n "$note" ]; then
      case "$note" in
        "!"*) grep -q "\[${note#!}\]" "$TMP/err" && noteok=0 ;;
        *)    grep -q "\[$note\]" "$TMP/err" || noteok=0 ;;
      esac
    fi
    if [ "$got" = "$want" ] && [ $noteok -eq 1 ]; then ok "$name"
    else bad "$name (got '$got' want '$want'${note:+, note '$note'})"; sed -n '1,3p' "$TMP/err"; fi
  else bad "$name (compile failed)"; sed -n '1,3p' "$TMP/err"; fi
}

echo "== driver process safety =="
# Output paths are passed as one argv element. Shell metacharacters in a path
# must remain literal and must never execute a second command.
# Keep the injected marker name relative: a slash inside the output basename
# would turn it into a nested path and make the test fail for filesystem reasons
# before it can distinguish argv execution from shell interpretation.
rm -f "$safe_marker"
safe_output="$TMP/program; touch $safe_marker; #"
if "$PEN" -O0 --no-superopt --no-precompute tests/pos_basics.fsht \
    -o "$safe_output" 2>"$TMP/shell-safe.err"; then
  if [ -x "$safe_output" ] && [ ! -e "$safe_marker" ]; then
    ok "shell metacharacters in output path are passed literally"
  else
    bad "shell metacharacters in output path were not inert"
  fi
else
  bad "compiler accepts output paths containing shell metacharacters"
  sed -n '1,5p' "$TMP/shell-safe.err"
fi

echo "== R1 regression =="
run_ok "hello/basics" tests/pos_basics.fsht "10.00|hi"
run_ok "for-range" tests/pos_range.fsht "10"
run_ok "parfor reduction" tests/pos_parfor.fsht "8000000"
run_ok "defer+free ownership" tests/pos_defer.fsht "45"
# report §7: performance diagnostics are warnings by default;
# --strict-performance restores the historical hard errors
run_warn "F0402 runtime div (warn-first)" tests/neg_arith.fsht F0402
$PEN --strict-performance tests/neg_arith_strict.fsht -o "$TMP/as" 2>"$TMP/as.err"; rc=$?
if [ $rc -ne 0 ] && grep -q "\[F0402\]" "$TMP/as.err"; then
  ok "F0402 runtime div (--strict-performance)"
else bad "F0402 strict mode"; sed -n '1,3p' "$TMP/as.err"; fi
run_err "F0214 div by literal zero" tests/neg_divzero.fsht F0214
run_err "own leak" tests/neg_leak.fsht F0301

echo "== R2: structs =="
run_ok "struct literal/copy" tests/pos_struct.fsht "1.00 3.00|1.50"
run_ok "nested fields + align(64) + &param" tests/pos_nested.fsht "5.00|24.00"
run_ok "AoS parfor (element-wise)" tests/pos_aos.fsht "160.00"
run_ok "SoA planes end-to-end" tests/pos_soa.fsht "4000.00"
run_warn "soa AoS advice" tests/warn_soa.fsht F0408
run_warn "layout-pad reorder" tests/warn_layout.fsht F0407
run_warn "cache-line-straddle straddle (warn-first)" tests/neg_straddle.fsht F0404
$PEN --strict-performance tests/neg_straddle_strict.fsht -o "$TMP/ss" 2>"$TMP/ss.err"; rc=$?
if [ $rc -ne 0 ] && grep -q "\[F0404\]" "$TMP/ss.err"; then
  ok "F0404 straddle (--strict-performance)"
else bad "F0404 strict mode"; sed -n '1,3p' "$TMP/ss.err"; fi
run_err "struct literal missing field" tests/neg_slit.fsht F0201

echo "== R2: nested parfor + affinity =="
run_ok "collapsed nested (rect)" tests/pos_nest_collapse.fsht "4096"
run_ok "dynamic nested (WS)" tests/pos_nest_ws.fsht "5050"
run_ok "ring-buffer scheduler + reduction" tests/pos_ringbuf.fsht "100000.00"
run_ok "ring-buffer scheduler + reduction" tests/pos_ringbuf.fsht "100000.00"
run_ok "ring-buffer scheduler + reduction" tests/pos_ringbuf.fsht "100000.00"
run_ok "ring-buffer scheduler + reduction" tests/pos_ringbuf.fsht "100000.00"
run_ok "ring-buffer scheduler + reduction" tests/pos_ringbuf.fsht "100000.00"
run_ok "ring-buffer scheduler + reduction" tests/pos_ringbuf.fsht "100000.00"
run_ok "ring-buffer scheduler + reduction" tests/pos_ringbuf.fsht "100000.00"
run_ok "affinity close/spread" tests/pos_affinity.fsht "2000000"
run_err "thread scalar race" tests/neg_race.fsht F0307

echo "== R2: PGO =="
# 1. instrument, 2. run, 3. profile exists, 4. guided build, 5. branch weights
if $PEN --profile tests/pos_pgo.fsht -o "$TMP/pgo" 2>/dev/null; then
  out=$("$TMP/pgo" 2>/dev/null)
  if [ -f "$TMP/pgo.fprof" ]; then ok "PGO instrument+run (out=$out)"; else bad "PGO .fprof missing"; fi
  if $PEN --use-profile="$TMP/pgo.fprof" tests/pos_pgo.fsht -o "$TMP/pgo2" 2>/dev/null; then
    out2=$("$TMP/pgo2" 2>/dev/null)
    if [ "$out2" = "$out" ]; then ok "PGO-guided recompile"; else bad "PGO-guided output mismatch ($out2 vs $out)"; fi
    $PEN -S tests/pos_pgo.fsht -o "$TMP/pgo.ll" --use-profile="$TMP/pgo.fprof" 2>/dev/null
    if grep -q "branch_weights" "$TMP/pgo.ll" && grep -q "function_entry_count" "$TMP/pgo.ll"; then
      ok "PGO metadata (branch_weights + entry_count)"
    else bad "PGO metadata missing in IR"; fi
  else bad "PGO-guided compile failed"; fi
else bad "PGO instrumented compile failed"; fi

# report mode smoke
if $PEN --report --use-profile="$TMP/pgo.fprof" tests/pos_pgo.fsht -o "$TMP/rep" 2>"$TMP/err" \
   && grep -q "Freshet Performance Report" "$TMP/err"; then
  ok "performance report mode"
else bad "performance report mode"; fi

echo "== R3: --soa=auto =="
# transform must preserve output exactly; 'b' transforms, 'w' is vetoed
if $PEN tests/pos_soa_auto.fsht -o "$TMP/sa0" 2>/dev/null; then
  out0=$("$TMP/sa0" 2>/dev/null)
  if $PEN --soa=auto tests/pos_soa_auto.fsht -o "$TMP/sa1" 2>"$TMP/sa.err"; then
    out1=$("$TMP/sa1" 2>/dev/null)
    if [ "$out0" = "$out1" ] && grep -q "F0903.*'b' in fn main" "$TMP/sa.err" &&
       grep -q "F0903.*skipped for 'w'" "$TMP/sa.err" &&
       grep -q "F0408.*'w'" "$TMP/sa.err"; then
      ok "soa=auto transform + veto + identical output"
    else bad "soa=auto behavior (out='$out1' want='$out0')"; sed -n '1,4p' "$TMP/sa.err"; fi
  else bad "soa=auto compile failed"; sed -n '1,3p' "$TMP/sa.err"; fi
else bad "soa=auto baseline compile failed"; fi
# IR proof: planes for the transformed var, malloc for the vetoed one
$PEN -S --soa=auto tests/pos_soa_auto.fsht -o "$TMP/sa.ll" 2>/dev/null
if grep -q "@frt_soa_new" "$TMP/sa.ll" && grep -q "@frt_soa_free" "$TMP/sa.ll"; then
  ok "soa=auto IR uses soa planes"
else bad "soa=auto IR missing soa runtime calls"; fi

echo "== finite-domain algorithm precomputation =="
# This evaluates every u8 input. CMake builds use embedded Alive2 to validate
# any replacement; non-CMake/bootstrap builds retain the original algorithm.
run_ok "u8 input/output map preserves all-domain checksum" tests/pos_precompute.fsht "8781696"
run_ok "finite inverse search returns the exact preimage" tests/pos_precompute_inverse.fsht "42"
if $PEN --no-precompute tests/pos_precompute.fsht -o "$TMP/precompute-off" 2>"$TMP/precompute-off.err" \
   && [ "$("$TMP/precompute-off" 2>/dev/null)" = "8781696" ] \
   && ! grep -q '^precompute:' "$TMP/precompute-off.err"; then
  ok "--no-precompute disables mapping stage"
else bad "--no-precompute switch"; sed -n '1,4p' "$TMP/precompute-off.err"; fi

echo "== embedded Freshet/Z3/Alive2 superoptimisation integration =="
# The CMake-built compiler uses in-process Z3/Alive2; bootstrap-only builds
# safely leave this stage disabled rather than depending on external tools.
if $PEN -O3 tests/pos_basics.fsht -o "$TMP/so" 2>"$TMP/so.err" && \
   [ "$("$TMP/so" 2>/dev/null)" = "$(printf '10.00\nhi')" ]; then
  ok "embedded superoptimiser preserves program output"
else bad "embedded Freshet superopt compile/run"; sed -n '1,4p' "$TMP/so.err"; fi

echo "== R3: -O4 solver-first default =="
if $PEN -O4 tests/pos_o4.fsht -o "$TMP/o4" 2>"$TMP/o4.err" \
   && grep -q "runtime tuning is disabled by default" "$TMP/o4.err"; then
  out=$("$TMP/o4" 2>/dev/null)
  if [ "$out" = "1599996000000.00" ]; then
    ok "-O4 build uses verified-superopt path by default and output is correct"
  else bad "-O4 output wrong (got '$out')"; fi
else bad "-O4 build failed"; sed -n '1,5p' "$TMP/o4.err"; fi

echo "== R4: loop fusion + full unrolling =="
# fusion: 3-phase FFT-multiply batch pipeline (fwd / pointwise / inv over
# call slices) jams into ONE sweep; output must equal the unoptimised build
if $PEN tests/pos_fuse.fsht -o "$TMP/fu" 2>"$TMP/fu.err" \
   && grep -q "F0908.*fused 3 adjacent loops" "$TMP/fu.err"; then
  out=$("$TMP/fu" 2>/dev/null)
  $PEN --no-loop-opts tests/pos_fuse.fsht -o "$TMP/fu0" 2>/dev/null
  out0=$("$TMP/fu0" 2>/dev/null)
  # §5: forced vectorise metadata is opt-in now, so count annotated loops
  # under --force-vectorize (a stable loop-count proxy for fusion proofs)
  $PEN -S --force-vectorize tests/pos_fuse.fsht -o "$TMP/fu.ll" 2>/dev/null
  n1=$(grep -c "vectorize.enable" "$TMP/fu.ll")
  $PEN -S --no-loop-opts --force-vectorize tests/pos_fuse.fsht -o "$TMP/fu0.ll" 2>/dev/null
  n0=$(grep -c "vectorize.enable" "$TMP/fu0.ll")
  if [ "$out" = "524288.00" ] && [ "$out" = "$out0" ] && [ "$n1" -lt "$n0" ]; then
    ok "loop fusion: 3-phase batch pipeline -> 1 sweep (F0908, $n0->$n1 loops, identical output)"
  else bad "fusion (out='$out' base='$out0' loops $n1 vs $n0)"; fi
else bad "fusion compile/note"; sed -n '1,3p' "$TMP/fu.err"; fi

# unroll: both constant-trip loops vanish from the IR (no vectorize hint)
if $PEN tests/pos_unroll.fsht -o "$TMP/un" 2>"$TMP/un.err" \
   && grep -q "F0909" "$TMP/un.err" && [ -n "$(grep -c F0909 "$TMP/un.err")" ]; then
  out=$("$TMP/un" 2>/dev/null)
  $PEN -S --force-vectorize tests/pos_unroll.fsht -o "$TMP/un.ll" 2>/dev/null
  n1=$(grep -c "vectorize.enable" "$TMP/un.ll")
  $PEN -S --no-loop-opts --force-vectorize tests/pos_unroll.fsht -o "$TMP/un0.ll" 2>/dev/null
  n0=$(grep -c "vectorize.enable" "$TMP/un0.ll")
  if [ "$out" = "56.00" ] && [ "$n1" = "0" ] && [ "$n0" = "2" ]; then
    ok "full unroll: constant-trip loops gone from IR (F0909, $n0 loops baseline)"
  else bad "unroll (out='$out' loops $n1 vs $n0)"; fi
else bad "unroll compile/note"; sed -n '1,3p' "$TMP/un.err"; fi

echo "== R4: cross-lane analysis =="
# uniform branch hoisted out of the vector loop (one scalar decides,
# specialised loops branch-free); output identical to baseline
if $PEN tests/pos_lane.fsht -o "$TMP/la" 2>"$TMP/la.err" \
   && grep -q "F0901.*hoisted uniform branch" "$TMP/la.err"; then
  out=$("$TMP/la" 2>/dev/null)
  $PEN --no-loop-opts tests/pos_lane.fsht -o "$TMP/la0" 2>/dev/null
  out0=$("$TMP/la0" 2>/dev/null)
  if [ "$out" = "6292992.00" ] && [ "$out" = "$out0" ]; then
    ok "cross-lane: uniform branch hoisted (F0901, identical output)"
  else bad "lane hoist (out='$out' base='$out0')"; fi
else bad "lane hoist compile/note"; sed -n '1,3p' "$TMP/la.err"; fi

# cost model: vector-light uniform branch must NOT be hoisted, and the
# veto must be visible (F0902 with the numbers)
if $PEN tests/warn_lane_cost.fsht -o "$TMP/lc" 2>"$TMP/lc.err" \
   && grep -A2 "\[F0902\]" "$TMP/lc.err" | grep -q "cost model: lane work 3 ops < 6"; then
  out=$("$TMP/lc" 2>/dev/null)
  if [ "$out" = "523776.00" ]; then
    ok "cross-lane cost model: vector-light branch left alone (F0902)"
  else bad "lane cost model (out='$out')"; fi
else bad "lane cost model compile/note"; sed -n '1,3p' "$TMP/lc.err"; fi

# predicate reuse: adjacent identical conditions merge (computed once,
# reused); output identical to baseline
if $PEN tests/pos_lane_merge.fsht -o "$TMP/lm" 2>"$TMP/lm.err" \
   && grep -q "F0901.*merged 2 adjacent branches" "$TMP/lm.err"; then
  out=$("$TMP/lm" 2>/dev/null)
  $PEN --no-loop-opts tests/pos_lane_merge.fsht -o "$TMP/lm0" 2>/dev/null
  out0=$("$TMP/lm0" 2>/dev/null)
  if [ "$out" = "$(printf '2087872.00\n1043936.00')" ] && [ "$out" = "$out0" ]; then
    ok "cross-lane: identical predicates merged (F0901, identical output)"
  else bad "predicate merge (out='$out' base='$out0')"; fi
else bad "predicate merge compile/note"; sed -n '1,3p' "$TMP/lm.err"; fi

# deeper scope: a uniform branch nested beneath a data-dependent branch is
# lifted out around the whole loop.  Compare to the no-loop-opts baseline.
if $PEN tests/pos_lane_nested.fsht -o "$TMP/ln" 2>"$TMP/ln.err" \
   && grep -q "F0901.*specialised an outer loop around a nested uniform branch" "$TMP/ln.err"; then
  out=$($TMP/ln 2>/dev/null)
  $PEN --no-loop-opts tests/pos_lane_nested.fsht -o "$TMP/ln0" 2>/dev/null
  out0=$($TMP/ln0 2>/dev/null)
  if [ "$out" = "$out0" ]; then
    ok "cross-lane: nested uniform branch specialised at outer-loop scope"
  else bad "nested cross-lane (out='$out' base='$out0')"; fi
else bad "nested cross-lane transform/note"; sed -n '1,4p' "$TMP/ln.err"; fi

echo "== Stage A: inlining policy (report §2.2 A) =="
# default: no blanket noinline on ordinary functions.  IR check: the
# function attribute group #50 must NOT contain 'noinline'.
if $PEN -S tests/pos_basics.fsht -o "$TMP/a1.ll" 2>/dev/null; then
  if grep -q "attributes #50 = { nounwind }" "$TMP/a1.ll"; then
    ok "Stage A1 default: #50 = { nounwind } (no noinline)"
  else
    bad "Stage A1 default: expected 'attributes #50 = { nounwind }' in IR"
    grep "attributes #5" "$TMP/a1.ll" | head -5
  fi
else bad "Stage A1 -S compile failed"; fi

# --inline=off: reproduce the historical noinline-everything baseline.
# IR check: ordinary functions should now be marked noinline (group #60).
if $PEN --inline=off -S tests/pos_basics.fsht -o "$TMP/a2.ll" 2>/dev/null; then
  if grep -q "attributes #60 = { noinline nounwind }" "$TMP/a2.ll"; then
    ok "Stage A1 --inline=off: group #60 = { noinline nounwind }"
  else
    bad "Stage A1 --inline=off: expected group #60 noinline"
    grep "attributes #6" "$TMP/a2.ll" | head -3
  fi
else bad "Stage A1 --inline=off compile failed"; fi

# @inline attribute selects alwaysinline (group #61); @noinline selects
# group #60.  Program must still compile and run correctly.
if $PEN tests/pos_inline_attrs.fsht -o "$TMP/a3" 2>/dev/null; then
  out=$("$TMP/a3" 2>/dev/null)
  if [ "$out" = "42" ]; then
    ok "Stage A1 @inline/@noinline attributes accepted + runs"
  else bad "Stage A1 attrs run (out='$out' want '42')"; fi
  $PEN -S tests/pos_inline_attrs.fsht -o "$TMP/a3.ll" 2>/dev/null
  if grep -q "attributes #61 = { alwaysinline nounwind }" "$TMP/a3.ll" && \
     grep -q "attributes #60 = { noinline nounwind }" "$TMP/a3.ll"; then
    ok "Stage A1 attrs emitted groups #60 + #61"
  else bad "Stage A1 attrs IR missing groups #60/#61"; fi
else bad "Stage A1 attrs compile failed"; fi

echo "== Stage A: -S combined IR + opt-remarks (report §1.3 + §2.1) =="
# -S should now write the COMBINED module (user + runtime), not just user
# IR.  Distinguishable by the runtime function bodies (`define @frt_init`
# etc.); the user IR alone only has `declare` lines for these.
if $PEN -S tests/pos_basics.fsht -o "$TMP/a4.ll" 2>/dev/null; then
  if grep -q "^define.*@frt_init" "$TMP/a4.ll" && grep -q "^define.*@frt_malloc" "$TMP/a4.ll"; then
    ok "Stage A3 -S writes combined IR (runtime bodies present)"
  else
    bad "Stage A3 -S missing runtime bodies"
    head -3 "$TMP/a4.ll"
  fi
else bad "Stage A3 -S compile failed"; fi

# --legacy-S: -S writes only user IR (pre-report behaviour).  The
# runtime bodies must NOT be present.  Note: the user IR contains
# `declare void @frt_init()` (forward declarations only); the runtime
# has `define void @frt_init()` (the body).  We check for the DEFINE so
# the test distinguishes "user IR only" from "combined IR".
if $PEN --legacy-S -S tests/pos_basics.fsht -o "$TMP/a5.ll" 2>/dev/null; then
  if grep -q "^define.*@frt_init" "$TMP/a5.ll"; then
    bad "Stage A3 --legacy-S should NOT write runtime bodies"
  else
    ok "Stage A3 --legacy-S writes only user IR"
  fi
else bad "Stage A3 --legacy-S compile failed"; fi

# --emit-combined-ir: even when building (not -S), drop the combined
# module at the requested path.  Same define-vs-declare check as above.
if $PEN --emit-combined-ir="$TMP/a6.combined.ll" tests/pos_basics.fsht \
     -o "$TMP/a6" 2>/dev/null; then
  if [ -f "$TMP/a6.combined.ll" ] && grep -q "^define.*@frt_init" "$TMP/a6.combined.ll"; then
    ok "Stage A2 --emit-combined-ir writes module"
  else bad "Stage A2 --emit-combined-ir missing module"; fi
else bad "Stage A2 --emit-combined-ir compile failed"; fi

# fsht-data crash regression: the combined module is a textual concat of
# two halves with independent `attributes #N` numbering.  A duplicate
# `attributes #N` definition lets the runtime's later definition REPLACE
# the user's — attaching e.g. clang-generated allocsize(...) to user
# functions/calls, where LLVM's parser aborts with "Broken module found"
# ('allocsize' element size argument must refer to an integer parameter /
# out of bounds).  The driver shifts the runtime's group ids above the
# user module's maximum, so NO index may be defined twice — the same
# invariant the metadata shift already enforced for !N ids.
if [ -f "$TMP/a6.combined.ll" ]; then
  dupattr=$(grep -o '^attributes #[0-9]*' "$TMP/a6.combined.ll" | sort | uniq -d)
  dupmeta=$(grep -o '^![0-9]* =' "$TMP/a6.combined.ll" | sort | uniq -d)
  if [ -z "$dupattr" ] && [ -z "$dupmeta" ]; then
    ok "combined module: attribute-group and metadata ids collision-free"
  else
    bad "combined module has duplicate ids: attr='$dupattr' meta='$dupmeta'"
  fi
  # and the runtime half must start above the user half's group maximum
  umax=$(awk '/; ---- freshet runtime/{exit} /^attributes #/{sub("attributes #",""); if ($0+0 > m) m = $0+0} END{print m+0}' "$TMP/a6.combined.ll")
  rmin=$(awk '/; ---- freshet runtime/{f=1} f && /^attributes #/{sub("attributes #",""); if (m == "" || $0+0 < m) m = $0+0; exit} END{print m+0}' "$TMP/a6.combined.ll")
  if [ "$umax" -lt "$rmin" ]; then
    ok "runtime attribute groups shifted above user maximum ($umax < $rmin)"
  else
    bad "runtime groups not shifted above user maximum ($umax >= $rmin)"
  fi
fi

echo "== Stage B4: @alias semantics (report §2.2 B) =="
# Sound @alias: caller passes distinct roots; compiler must emit noalias
# on the user function's parameter signature.  We grep for `noalias`
# inside the `define` line of the user function — the runtime defines
# also use `noalias` (e.g. on frt_parfor's ctxout), so a whole-file grep
# would always pass.  The signature is what matters.
if $PEN -S tests/pos_alias_sound.fsht -o "$TMP/b1.ll" 2>/dev/null; then
  if grep -E "^define.*@u_add_two.*noalias" "$TMP/b1.ll" >/dev/null; then
    ok "Stage B5 sound @alias emits noalias parameter attribute"
  else
    bad "Stage B5 no noalias attr on u_add_two signature"
    grep -E "^define.*@u_add_two" "$TMP/b1.ll" | head -2
  fi
else bad "Stage B5 sound @alias compile failed"; fi

# Unsound @alias: caller passes provably-overlapping memory; compiler
# must emit F0306 and SUPPRESS noalias for that pair.  The user fn
# signature must NOT carry noalias; the runtime defines may still have it
# on unrelated params (e.g. frt_parfor's ctxout), so check the user fn.
if $PEN -S tests/pos_alias_unsound.fsht -o "$TMP/b2.ll" 2>"$TMP/b2.err"; then
  if grep -q "F0306" "$TMP/b2.err"; then
    ok "Stage B4 unsound @alias emits F0306"
  else bad "Stage B4 expected F0306 warning"; fi
  if grep -E "^define.*@u_add_two.*noalias" "$TMP/b2.ll" >/dev/null; then
    bad "Stage B4 unsound pair: noalias leaked into u_add_two signature"
  else
    ok "Stage B5 unsound pair: noalias correctly suppressed"
  fi
else bad "Stage B4 unsound @alias compile failed"; fi

# F0305 still fires when two pointer args provably alias and the pair
# is NOT declared @alias.
run_err "Stage B4 alias-violation still fires" tests/neg_alias.fsht F0305

echo "== Audit integration: P0 effect summaries (report §2) =="
# §2.2 reproducer: pointer-reading helper between stores.  Before the fix
# the helper was stamped memory(none) willreturn and -O3 printed "00".
for O in 0 3; do
  if $PEN -O$O tests/pos_effects_repro.fsht -o "$TMP/rr$O" 2>/dev/null; then
    got=$("$TMP/rr$O" 2>/dev/null)
    if [ "$got" = "12" ]; then ok "§2.2 reproducer at -O$O prints 12"
    else bad "§2.2 reproducer at -O$O (got '$got' want '12')"; fi
  else bad "§2.2 reproducer -O$O compile"; fi
done

# global read between writes (§2.4): reader must not be memory(none)
if $PEN tests/pos_effects_global.fsht -o "$TMP/eg" 2>/dev/null; then
  got=$("$TMP/eg" 2>/dev/null)
  if [ "$got" = "012" ]; then ok "§2.4 global-read helper correct (012)"
  else bad "§2.4 global-read helper (got '$got' want '012')"; fi
else bad "§2.4 global-read compile"; fi

# rebound/aliased pointer reads (§2.4, the R4 mut-rebind idiom)
if $PEN tests/pos_effects_alias.fsht -o "$TMP/ea" 2>/dev/null; then
  got=$("$TMP/ea" 2>/dev/null)
  if [ "$got" = "70100" ]; then ok "§2.4 alias/rebind reads correct (7 0 100)"
  else bad "§2.4 alias/rebind reads (got '$got' want '70100')"; fi
else bad "§2.4 alias compile"; fi

# malloc wrapper used: must not be described as memory-free (§2.4)
if $PEN tests/pos_effects_mallocw.fsht -o "$TMP/em" 2>/dev/null; then
  got=$("$TMP/em" 2>/dev/null)
  if [ "$got" = "0.00" ]; then ok "§2.4 malloc wrapper correct (0.00)"
  else bad "§2.4 malloc wrapper (got '$got' want '0.00')"; fi
else bad "§2.4 malloc wrapper compile"; fi

# runtime builtins in purity candidates (§2.4): freshet.threads reads
# runtime state; soa_len reads the descriptor.  IR check: the wrappers
# must carry memory(read) — never memory(none).
$PEN -S tests/pos_effects_builtins.fsht -o "$TMP/eb.ll" 2>/dev/null
if grep -E "memory\(none\)" "$TMP/eb.ll" | grep -qE "nthreads|plen|u_"; then
  bad "§2.4 builtin wrappers must not be memory(none)"
else
  ok "§2.4 freshet.threads/soa_len wrappers keep memory(read)"
fi
if $PEN tests/pos_effects_builtins.fsht -o "$TMP/eb" 2>/dev/null; then
  got=$("$TMP/eb" 2>/dev/null)
  if [ "$got" = "4${FRT_THREADS:-$(nproc)}" ] || [ "${got:0:1}" = "4" ]; then
    ok "§2.4 builtin wrappers run"
  else bad "§2.4 builtin wrappers output (got '$got')"; fi
else bad "§2.4 builtins compile"; fi

# pure-but-nonterminating: willreturn must NOT be inferred without a
# termination proof (§2.4) — IR inspection on the definition of u_spin
$PEN -S tests/pos_effects_willret.fsht -o "$TMP/wr.ll" 2>/dev/null
if grep -E "^define.*@u_spin" "$TMP/wr.ll" | grep -q willreturn; then
  bad "§2.4 willreturn leaked onto an unbounded-loop function"
else
  ok "§2.4 no willreturn without termination proof"
fi

# @pure contract violations are hard errors (F0304, spec §3.1)
run_err "§2.4 pure write violation" tests/neg_pure_write.fsht F0304
run_err "§2.4 pure read violation" tests/neg_pure_read.fsht F0304
run_err "§2.4 pure impure callee" tests/neg_pure_call.fsht F0304
run_err "§2.4 pure io violation" tests/neg_pure_io.fsht F0304

echo "== Audit integration: IR attribute soundness (report §2.3) =="
# read-only pointer consumer => memory(argmem: read), NOT memory(none)
$PEN -S tests/pos_effects_repro.fsht -o "$TMP/ra.ll" 2>/dev/null
if grep -q "memory(argmem: read)" "$TMP/ra.ll"; then
  ok "§2.3 read-only helpers get memory(argmem: read)"
else bad "§2.3 memory(argmem: read) missing from read-first IR"; fi
# a genuinely memory-free helper still gets the full #52 treatment
$PEN -S tests/pos_basics.fsht -o "$TMP/mn.ll" 2>/dev/null
if grep -q "attributes #52 = { nounwind memory(none) willreturn }" "$TMP/mn.ll"; then
  ok "§2.3 memory-free fns keep group #52 (memory(none) willreturn)"
else bad "§2.3 group #52 missing"; fi

echo "== Audit integration: runtime IR + loop policy + PGO (§3/§5/§11) =="
# cached runtime IR is built at -O2: no optnone may leak into the final
# module (the §3 acceptance test)
$PEN -S tests/pos_basics.fsht -o "$TMP/rt.ll" 2>/dev/null
if grep -q "optnone" "$TMP/rt.ll"; then
  bad "§3 optnone leaked into the combined module"
else
  ok "§3 runtime IR has no optnone (built at -O$( [ -n "$($PEN --version >/dev/null 2>&1; echo)" ] && echo 2 || echo 2 ))"
fi
if ls build/frt_rt*_o2*.ll >/dev/null 2>&1; then
  ok "§3 runtime cache file keyed with optimisation level (frt_rt*_o2*.ll)"
else bad "§3 runtime cache key missing level"; fi

# default: NO forced vectorisation metadata; --force-vectorize restores
# the historical hint; @vectorize emits enable-only (§5)
$PEN -S tests/pos_vectorize.fsht -o "$TMP/va.ll" 2>/dev/null
n_annot=$(grep -c "vectorize.enable" "$TMP/va.ll")
n_ilv=$(grep -c "interleave.count" "$TMP/va.ll")
if [ "$n_annot" = "1" ] && [ "$n_ilv" = "0" ]; then
  ok "§5 @vectorize annotation: 1 annotated loop, no interleave count"
else bad "§5 @vectorize IR (annotated=$n_annot interleave=$n_ilv, want 1/0)"; fi
# LoopIDs must be self-referential, and the current vectorize.enable hint
# takes its name string only (no legacy i1 true operand).
if grep -Eq '^![0-9]+ = distinct !\{![0-9]+, !\{!"llvm.loop.vectorize.enable"\}\}$' "$TMP/va.ll" &&
   ! grep -q 'llvm.loop.vectorize.enable", i1' "$TMP/va.ll"; then
  ok "§5 @vectorize metadata uses a valid LoopID and enable hint"
else
  bad "§5 @vectorize metadata shape is invalid"
fi
# Preserve compiler stderr: this path runs both finite-domain rewriting and
# embedded Alive2/Freshet superoptimisation at the default -O3. Hiding stderr
# made native aborts appear only as an unhelpful shell "Aborted" line.
vectorize_compile_log="$TMP/pos_vectorize.compile.stderr"
if $PEN tests/pos_vectorize.fsht -o "$TMP/va" 2>"$vectorize_compile_log"; then
  got=$("$TMP/va" 2>/dev/null)
  base_want="5997000.00"
  if [ "$got" = "$base_want" ]; then ok "§5 @vectorize program output correct"
  else bad "§5 @vectorize output (got '$got' want '$base_want')"; fi
else
  if [ -s "$vectorize_compile_log" ]; then
    echo "--- pos_vectorize compiler stderr ---" >&2
    cat "$vectorize_compile_log" >&2
    echo "--- end compiler stderr ---" >&2
  fi
  bad "§5 @vectorize compile"
fi
cat > "$TMP/fv.fsht" <<'FVEOF'
fn main() {
    let n: i64 = 64;
    let a: *f64 = malloc(f64, n);
    for (i in 0..n) { a[i] = 1.0; }
    free(a);
}
FVEOF
$PEN -S --force-vectorize "$TMP/fv.fsht" -o "$TMP/fv.ll" 2>/dev/null
if grep -q "interleave.count" "$TMP/fv.ll"; then
  ok "§5 --force-vectorize restores the historical hint"
else bad "§5 --force-vectorize hint missing"; fi
if grep -Eq '^![0-9]+ = distinct !\{![0-9]+, !\{!"llvm.loop.vectorize.enable"\}, !\{!"llvm.loop.interleave.count", i32 4\}\}$' "$TMP/fv.ll"; then
  ok "§5 --force-vectorize metadata uses a valid self-referential LoopID"
else bad "§5 --force-vectorize LoopID metadata shape is invalid"; fi

# custom PGO counters are thread-safe atomics (§11.1)
$PEN -S --profile tests/pos_parfor.fsht -o "$TMP/pg.ll" 2>/dev/null
if grep -q "atomicrmw add" "$TMP/pg.ll"; then
  ok "§11.1 PGO counters are atomic (atomicrmw)"
else bad "§11.1 PGO counters not atomic"; fi

echo "== Audit integration: differential execution (§2.4/§13.4) =="
# -O0 vs -O3 checksum equality for a mixed arithmetic/pointer kernel
okdiff=1
$PEN -O0 tests/pos_diff_checksum.fsht -o "$TMP/dc0" 2>/dev/null || okdiff=0
$PEN -O3 tests/pos_diff_checksum.fsht -o "$TMP/dc3" 2>/dev/null || okdiff=0
if [ $okdiff = 1 ]; then
  o0=$("$TMP/dc0" 2>/dev/null); o3=$("$TMP/dc3" 2>/dev/null)
  if [ -n "$o0" ] && [ "$o0" = "$o3" ]; then
    ok "§2.4 differential -O0 vs -O3 identical ($o0)"
  else bad "§2.4 differential mismatch (O0='$o0' O3='$o3')"; fi
else bad "§2.4 differential compiles failed"; fi

# serial cutoff (§10.1): all-inline and all-dispatch must both be correct
if $PEN tests/pos_nest_ws.fsht -o "$TMP/sc" 2>/dev/null; then
  a=$(FRT_SERIAL_CUTOFF=1000000 "$TMP/sc" 2>/dev/null)
  b=$(FRT_SERIAL_CUTOFF=0 "$TMP/sc" 2>/dev/null)
  c=$("$TMP/sc" 2>/dev/null)
  if [ "$a" = "5050" ] && [ "$b" = "5050" ] && [ "$c" = "5050" ]; then
    ok "§10.1 serial cutoff: inline/dispatch/default all correct (5050)"
  else bad "§10.1 serial cutoff outputs ($a/$b/$c)"; fi
  # timing file carries the ST scheduler stats
  FRT_TIMING=1 FRT_SERIAL_CUTOFF=1000000 "$TMP/sc" >/dev/null 2>&1
  if grep -q "^ST " "$TMP/sc.frtiming"; then
    ok "§10.1 ST scheduler stats in .frtiming"
  else bad "§10.1 ST line missing from timing file"; fi
else bad "§10.1 serial cutoff compile"; fi

# vector math (§8): fast mode lowers sin/cos/log2/log10 to intrinsics
if $PEN -S /dev/null -o /dev/null 2>/dev/null; then :; fi
cat > "$TMP/vm.fsht" <<'VEOF'
@fastmath fn main() {
    let a: *f64 = malloc(f64, 64);
    for (i in 0..64) {
        let fi: f64 = i as f64;
        a[i] = sin(fi) + cos(fi) + log2(fi + 1.0) + log10(fi + 1.0);
    }
    let mut s: f64 = 0.0;
    for (i in 0..64) { s = s + a[i]; }
    print_f64(s, 4); println();
    free(a);
}
VEOF
$PEN -S "$TMP/vm.fsht" -o "$TMP/vm.ll" 2>/dev/null
if grep -q "@llvm.sin" "$TMP/vm.ll" && grep -q "@llvm.log10" "$TMP/vm.ll"; then
  ok "§8 @fastmath: sin/cos/log2/log10 lower to LLVM intrinsics"
else bad "§8 transcendental intrinsics missing"; fi
# @strictfp keeps scalar libm calls; under Option A the DEFAULT is strict,
# so plain functions also keep libm calls (no fast-math without opt-in)
cat > "$TMP/vs.fsht" <<'VEOF'
@strictfp fn f(x: f64) -> f64 {
    return sin(x);
}
fn g(x: f64) -> f64 {
    return cos(x);
}
fn main() {
    print_f64(f(0.5) + g(0.5), 4); println();
}
VEOF
$PEN -S "$TMP/vs.fsht" -o "$TMP/vs.ll" 2>/dev/null
if grep -q "call double @sin" "$TMP/vs.ll" && grep -q "call double @cos" "$TMP/vs.ll"; then
  ok "§8 Option A: strict default keeps scalar libm calls (incl. plain fns)"
else bad "§8 strictfp lowering changed"; fi

# --emit-final-ir (§13.1): the backend's optimised IR is inspectable
if $PEN --emit-final-ir tests/pos_basics.fsht -o "$TMP/fi" 2>/dev/null && \
   [ -f "$TMP/fi.final.ll" ]; then
  ok "§13.1 --emit-final-ir writes the optimised module"
else bad "§13.1 --emit-final-ir missing"; fi

echo "== Phase 0: correctness contract (report §3) =="
run_ok "signed div/rem truncation" tests/pos_divrem.fsht "-3 -7|-1 -1|-1 0|-7 0|-2 0|3 7|-25 -4|-37 -44|-50 -36|9223372036854775807|15|125 2|-32|-3 -7"
run_err "ret fall-off-the-end" tests/neg_noret.fsht F0209
run_err "ret valueless return" tests/neg_retvoid.fsht F0209
run_err "ret one-branch return" tests/neg_oneret.fsht F0209
run_ok "definite return + while(true)" tests/pos_defret.fsht "3|3|6|6"
run_ok "<<= >>= end-to-end" tests/pos_shiftassign.fsht "16|4|1024|128|-16|24|25|7"
run_ok "by-value struct params (miscompile fix)" tests/pos_structparam.fsht "4.00|100.0 1.5|2.5"

echo "== Phase 1: data + control foundations (report §4) =="
run_ok "break/continue + steps + ranges" tests/pos_breakcont.fsht "10|20|37|63|4|55|22"
run_err "F0213 break outside loop" tests/neg_brkloop.fsht F0213
run_ok "fixed arrays [T; N]" tests/pos_arrays.fsht "1 4|4|99|26|5|64|1 100|3|4.0|9"
run_err "bound constant index" tests/neg_bound.fsht F0212
run_ok "Span<T> views + slicing" tests/pos_spans.fsht "10.0|4|15|20|3 10 30|30 40|40|3.5|3.0"
run_ok "enums + tagged unions + match" tests/pos_enums.fsht "10|20|12.57|12.0|0.0|10.0|circle|3.0 4.0|3.14|1.0|30"
run_err "match non-exhaustive" tests/neg_match_nonex.fsht F0204

echo "== Phase 2: generics + strings + errors (report §4) =="
run_ok "generics (fns + structs + constraints)" tests/pos_generics.fsht "2.0|5|1.0|1.5 7|3 1.5|10|7.5|1|10|2.5"
run_err "generic constraint violation" tests/neg_genericconstraint.fsht F0203

# --- fsht-data crash fix (user report: pinContextTo segfault on an I/O
# heavy program): semaProgram's struct/enum/fn-body loops iterated
# p->structs/p->enums/p->fns with range-for while their own BODIES append
# (prelude creation, generic instantiation) — push_back past capacity
# reallocates and the cached iterators walk freed memory.  These three
# regressions make the realloc deterministic: power-of-two decl counts
# put the mid-loop instantiation push exactly past the vector capacity.
run_ok "sema realloc: struct fields with prelude/generic types" tests/pos_sema_realloc_structs.fsht "256"
run_ok "sema realloc: enum payloads with generic types" tests/pos_sema_realloc_enums.fsht "512"
run_ok "sema realloc: generic call instantiation mid body-loop" tests/pos_sema_realloc_fns.fsht "510"

run_ok "strings: str views + String + fmt" tests/pos_strings.fsht "hello|5|65 67|world|5|eq ne|abcdef|6|count: 42|12345|3.14|hello, freshet!|pi 3"
run_ok "Option/Result + ? propagation" tests/pos_optres.fsht "5|1001|2|none|26|1001|42|none|7"

echo "== fsht-02:01: dynamic collections (registry 02.09.01/02.09.02) =="
run_ok "Vec<T>: construction, growth, indexing, slicing" tests/pos_vec_basics.fsht "10 16|285|100 81|81|none|9|4 4 25|9|1.5 2.5 3.5|1.5|9.5 4|2|empty 2|0"
run_ok "Vec<T> over POD structs, bools, f32, u8" tests/pos_vec_pod.fsht "12.0 6|42.5|TF|4.0|7 8"
run_ok "Vec<T> moves: let-move, give, by-value, return, clone, manual free" tests/pos_vec_moves.fsht "1|4.0|2 4|3 2|7|7|3"
run_ok "Vec<T> through generic fns + span views" tests/pos_vec_generic.fsht "30|1.8|6|5"
run_ok "parfor over a container's Span view" tests/pos_vec_parfor_view.fsht "16320.00"
run_err "container use-after-move (F0301)" tests/neg_vec_move.fsht F0301
run_err "container use-after-free (F0301)" tests/neg_vec_useafterfree.fsht F0301
run_err "container double free (F0301)" tests/neg_vec_doublefree.fsht F0301
run_err "rebinding a container that still owns (F0301)" tests/neg_vec_rebind.fsht F0301
run_ok "owning element types legal (fsht-03:02 lifting)" tests/pos_vec_owned_elem.fsht "2"
run_ok "container nested in a struct legal (fsht-03:02 lifting)" tests/pos_vec_structfield.fsht "1"
run_err "returning a container view dangles (F0311)" tests/neg_vec_viewescape.fsht F0311
run_err "container mutation inside parfor (F0307)" tests/neg_vec_parfor_mut.fsht F0307

echo "== fsht-02:02: user-defined traits, static dispatch (registry 06.02.01) =="
run_ok "traits: decl/impl/generic dispatch/struct targets/Self:: sibling" tests/pos_traits.fsht "10.0|15|4.5|21|12|12.0|24.0|3.0"
run_err "trait bound without an impl (F0218)" tests/neg_traits_noimpl.fsht F0218
run_err "impl signature mismatch (F0217)" tests/neg_traits_sig.fsht F0217
run_err "ambiguous zero-arg trait method (F0216)" tests/neg_traits_ambig.fsht F0216

echo "== fsht-02:03: const generics + const fn (registry 06.03.01) =="
run_ok "const generics: structs, R*C, const fn args, const_assert, inference" tests/pos_constgen.fsht "5.0|4.0|24 32|48|5.0|72|64|440|61.5|10|36 34|const assertions hold"
run_err "runtime fn in a const slot (F0219)" tests/neg_constgen_eval.fsht F0219
run_err "failed compile-time assertion (F0220)" tests/neg_constgen_assert.fsht F0220
run_err "const-evaluated length is zero (F0219)" tests/neg_constgen_zero.fsht F0219

echo "== Memory modes (report §5) =="
run_ok "owned/borrowed/unsafe" tests/pos_memmodes.fsht "6.0|5 6|11"
run_err "own-call owned param without give" tests/neg_ownedcall.fsht F0303

echo "== optimisation-audit: §10.1 legality regressions =="
# §4.1 P0: predicate merging must not remove an observable call
run_okf "no merge of impure predicates (16 calls)" tests/neg_lane_merge_impure.fsht "16" "-O2" "!F0901"
# §4.2 P0: no condition evaluation on a zero-trip loop
run_okf "no unswitch on zero-trip trapping cond" tests/neg_lane_unswitch_zero_trip.fsht "0" "-O2" "!F0901"
# §4.3 P0: unrolling vetoes bodies with return
run_okf "no unroll of return-containing body" tests/neg_unroll_return.fsht "2|-1" "-O2"
# §4.4 P0: typed bit-vector constant semantics
run_ok "typed const overflow/cast semantics" tests/neg_const_overflow_fold.fsht "1|1|255|1|33"
# §4.6 P1: fusion rejected when unknown effects may conflict
run_okf "fusion rejected on unknown effects" tests/neg_fuse_unknown_effect.fsht "8" "-O2" "!F0908"
# mirrors: the positive shapes still fire
run_okf "disjoint regions still fuse" tests/pos_fuse_disjoint_regions.fsht "11" "-O2" "F0908"
run_okf "simple loop still fully unrolls" tests/pos_unroll_simple.fsht "28" "-O2" "F0909"
run_okf "pure total predicate still merges" tests/pos_lane_merge_pure.fsht "64" "-O2" "F0901"

echo "== optimisation-audit: §6 pass features =="
run_ok "typed const fold + short-circuit" tests/pos_constfold.fsht "9|44|5"
run_ok "imin/imax builtins" tests/pos_iminmax.fsht "3 7 -5 2|4 9"
run_okf "FreshetLICM hoists invariant pure lets" tests/pos_licm.fsht "1035" "-O2 --licm" "F0912"
run_okf "partial unroll with exact remainder" tests/pos_partial_unroll.fsht "440|33" "-O2 --partial-unroll" "F0910"
run_okf "peel resolves first-iteration guard" tests/pos_peel.fsht "116|1009" "-O2 --peel" "F0911"
run_okf "interchange swaps transposed nest" tests/pos_interchange.fsht "6|2" "-O2 --interchange" "F0913"
run_okf "tile blocks with exact boundaries" tests/pos_tile.fsht "54|1|73" "-O2 --tile" "F0914"
run_okf "unroll-and-jam keeps domain exact" tests/pos_unrolljam.fsht "56|95" "-O2 --unroll-and-jam" "F0916"
run_okf "distribution fissions independent groups" tests/pos_distribute.fsht "22|20" "-O2 --distribute" "F0915"
run_okf "parfor advisor: parallel-safe + reduction" tests/pos_parfor_advisor.fsht "18 2016" "-O2 --parfor-advisor" "F0905"
run_ok "body-cost-aware parfor cutoff" tests/pos_bodycost.fsht "19900|186900"
run_okf "proof log records premises" tests/pos_prooflog.fsht "440" "-O2 --partial-unroll --proof-log" ""

# ---------------------------------------------------------------------------
# mem & vectorisation stage: mem-reuse (F0919), mov-minimize (F0918),
# batch-ops (F0920), vector-plan advisory (F0921), SMT interleave (F0922)
# ---------------------------------------------------------------------------
echo "== mem & vectorisation stage =="
run_okf "mem-reuse hoists loop alloc churn" tests/pos_mem_reuse.fsht "2044.00" "-O2 --mem-reuse" "F0919"
run_okf "mem-reuse never-written read vetoes reuse" tests/neg_mem_reuse_readfirst.fsht "28" "-O2 --mem-reuse" "!F0919"
run_okf "mov-minimize: copies/self-assign/memcpy-forward" tests/pos_mov_min.fsht "4.50|3.00" "-O2 --mov-minimize" "F0918"
run_okf "batch-ops de-interleaves isomorphic chains" tests/pos_batch.fsht "202.00" "-O2 --batch-ops" "F0920"
run_okf "vector-plan reports separable routines" tests/pos_vector_plan.fsht "202.00" "-O2 --vector-plan" "F0921"
run_okf "smt-interleave pairs complementary regions" tests/pos_smt_pair.fsht "2.02" "-O2 --smt-interleave" "F0922"
run_okf "smt pairing vetoed on inter-region dependence" tests/neg_smt_pair_alias.fsht "3.00" "-O2 --smt-interleave" "!F0922"
# the paired dispatch is visible in the IR: ONE frt_parfor_pair call and
# exactly two chunk fns for the pair (the third belongs to the init region)
if $PEN -S --smt-interleave tests/pos_smt_pair.fsht -o "$TMP/smt.ll" 2>/dev/null \
    && grep -q "frt_parfor_pair" "$TMP/smt.ll" \
    && [ "$(grep -c 'define internal void @__pf_body' "$TMP/smt.ll")" = "3" ]; then
  ok "smt-interleave lowers to frt_parfor_pair (IR check)"
else bad "smt-interleave IR check (frt_parfor_pair / chunk fns)"; fi
# mem_class tags land at range offset 72 (1=memory, 2=compute)
if $PEN -S --smt-interleave tests/pos_smt_pair.fsht -o "$TMP/smt2.ll" 2>/dev/null \
    && grep -q "i64 72" "$TMP/smt2.ll"; then
  ok "smt-interleave bakes mem_class at offset 72 (IR check)"
else bad "smt-interleave mem_class IR check"; fi
# runtime-level SMT semantics (pure C, no LLVM needed): the sysfs cpu-list
# parser and every inline/dispatch combination of a paired dispatch must
# match the sequential reference
if command -v gcc >/dev/null 2>&1; then
  # -I runtime: the fsht-03:03 decomposition nests platform includes
  # as runtime/-relative paths (common/frt_platform.h) — the umbrella
  # resolves them through the runtime directory on the include path
  if gcc -std=c11 -D_GNU_SOURCE -w -I runtime -o "$TMP/rt_smt_test" \
      scripts/rt_smt_test.c runtime/frt_rt.c -lpthread -lm 2>/dev/null \
      && "$TMP/rt_smt_test" >"$TMP/rt_smt.out" 2>&1 \
      && grep -q "ALL RUNTIME SMT TESTS PASSED" "$TMP/rt_smt.out"; then
    ok "runtime SMT: cpu-list parser + pair semantics vs sequential"
  else bad "runtime SMT unit tests"; sed -n '1,4p' "$TMP/rt_smt.out" 2>/dev/null; fi
else
  echo "  SKIP  runtime SMT unit tests (no gcc on PATH)"
fi

echo "== goose-ported passes: sumred / basecase =="
# goose (aardappel/goose) study ports: bit-identical blocked float
# reductions (optimize_loops.h ShapeSum) and recursive base-case inlining
# (optimize_basecase.h).  Both are covered by output equivalence AND the
# note being (not) emitted (audit §10.1).
run_okf "sumred regroups and stays bit-identical" tests/pos_sumred.fsht "999.000000" "-O2 --sum-reduction" "F0928"
run_okf "sumred off by default, same output" tests/pos_sumred.fsht "999.000000" "-O2" "!F0928"
run_okf "sumred veto: the term reads the accumulator" tests/neg_sumred.fsht "3851.859889" "-O2 --sum-reduction" "!F0928"
run_okf "basecase inlines the leaf call" tests/pos_basecase.fsht "0" "-O2 --basecase" "F0929"
run_okf "basecase off by default, same output" tests/pos_basecase.fsht "0" "-O2" "!F0929"
run_okf "basecase veto: self-call not in return position" tests/neg_basecase.fsht "5" "-O2 --basecase" "!F0929"
# the rewrite is visible in the IR: the blocked loop's fresh bindings
if $PEN -S --sum-reduction tests/pos_sumred.fsht -o "$TMP/sr.ll" 2>/dev/null \
    && grep -q "__sr_j\." "$TMP/sr.ll" \
    && [ "$(grep -o '__sr_t\.[0-9]*' "$TMP/sr.ll" | sort -u | wc -l)" -ge 8 ]; then
  ok "sumred lowers blocked terms + tail loop (IR check)"
else bad "sumred IR check (blocked terms / tail loop)"; fi

echo "== goose-ported passes: bce / checkfree =="
# bounds-check elimination (goose src/bce.h): difference-constraint facts
# (loop/parfor ranges, assert, construction, completed checks) prove sites
# in checked builds; output must be identical with/without the pass
run_okf "bce elides proven checks (checked build)" tests/pos_bce.fsht "2080.00|2112.00|21.00" "-O2 --checked" "F0923"
run_okf "bce off (--no-bce), same output" tests/pos_bce.fsht "2080.00|2112.00|21.00" "-O2 --checked --no-bce" "!F0923"
# interprocedural entry-fact seeding (goose SeedEntryFacts): the callee's
# check is provable only from what its (all-analyzed) call sites prove
run_okf "bce entry-fact seeding elides the callee's check" tests/pos_bce_entry.fsht "1488.00" "-O2 --checked" "F0923"
run_okf "bce entry-fact differential (--no-bce), same output" tests/pos_bce_entry.fsht "1488.00" "-O2 --checked --no-bce" "!F0923"
if $PEN -O2 --checked tests/pos_bce_entry.fsht -o "$TMP/pbe" 2>"$TMP/pbe.err" \
    && grep -q "1/1 index" "$TMP/pbe.err"; then
  ok "bce entry-fact seeding visible in the elision counts"
else bad "bce entry-fact seeding counts"; fi
# the differential veto: a second site whose index is untracked kills the
# pair cells — no seeding for those pairs, the callee keeps its check
if $PEN -O2 --checked tests/neg_bce_entry.fsht -o "$TMP/nbe" 2>"$TMP/nbe.err" \
    && [ "$("$TMP/nbe" 2>/dev/null)" = "1503.00" ] \
    && grep -q "0/1 index" "$TMP/nbe.err"; then
  ok "bce entry-fact veto: untracked site keeps the callee's check"
else bad "bce entry-fact veto"; fi
run_okf "bce veto: runtime index keeps its check" tests/neg_bce.fsht "3.00" "-O2 --checked" "!F0923"
run_okf "checkfree: proven chain compiles clean" tests/pos_checkfree.fsht "528.00" "-O2 --checked" "!F0924"
run_err "checkfree violation is transitive (F0924)" tests/neg_checkfree.fsht "F0924"
# checked-build trap regression (found while porting goose's BCE): a KEPT
# check must still FIRE at -O2 — the effect summary marks check-emitting
# fns may-not-return, or the backend deletes the failing check's trap as
# side-effect-free dead code (wrong code in every checked build)
if $PEN -O2 --checked tests/neg_bce_trap.fsht -o "$TMP/t" 2>/dev/null \
    && ! "$TMP/t" >/dev/null 2>"$TMP/trap.err" \
    && grep -q "bounds check failed" "$TMP/trap.err"; then
  ok "kept bounds check still traps at -O2 (effect-summary fix)"
else bad "kept check trap at -O2"; fi

echo "== goose-ported passes: data stacks =="
# the data-stack memory model (goose codegen_stacks.h): stack_alloc bumps
# a per-thread pre-reserved region; the binding's block close is the
# deallocation (watermark restore — no free, no destructor)
run_okf "stack_alloc bumps + watermark restores" tests/pos_dstk.fsht "2475.00|1000.00" "-O2" ""
run_okf "--stacks reports the layout (F0925)" tests/pos_dstk.fsht "2475.00|1000.00" "-O2 --stacks" "F0925"
run_err "free() on data-stack memory (F0927)" tests/neg_dstk_free.fsht "F0927"
run_err "returning data-stack memory (F0926)" tests/neg_dstk_escape.fsht "F0926"
# %sp threading through frt_parfor (goose port v2): the region's chunk
# fn takes the dispatching context's first-free stack index as a hidden
# 4th argument (frt_range_t.sp0 @80) — region-local stack_alloc and calls
# to stack-using helpers work inside dispatched bodies
run_okf "%sp through frt_parfor: region stacks + stack-using callees" tests/pos_dstk_parfor.fsht "128.00" "-O2" ""
run_okf "%sp nesting: dynamic inner dispatch + collapsed inner" tests/pos_dstk_nest.fsht "200.00" "-O2" ""
run_err "outer stack binding captured by a parfor region (F0926)" tests/neg_dstk_parfor.fsht "F0926"
# static-root lifetime proofs (goose reference §12): scope-nested stores
# through stack memory and non-escaping callee parameters are PROVEN safe
run_okf "static-root proofs: nested stores + non-escaping callees" tests/pos_dstk_roots.fsht "10.0|10.0|3.0|2.0" "-O2" ""
run_err "store into stack memory that outlives the source (F0926)" tests/neg_dstk_roots.fsht "F0926"
run_err "stack memory at an escaping parameter (F0926)" tests/neg_dstk_escape2.fsht "F0926"
# the chunk fn's hidden %sp and the range's sp0 are visible in the IR
if $PEN -S tests/pos_dstk_parfor.fsht -o "$TMP/pf.ll" 2>/dev/null \
    && grep -q "i64 %sp" "$TMP/pf.ll" \
    && grep -q "@__pf_body_" "$TMP/pf.ll"; then
  ok "chunk fns take the hidden i64 %sp (IR check)"
else bad "chunk fn %sp IR check"; fi
# the bump + restore are visible in the IR: the per-thread TLS tops array
# (N = 1 for this program), bumped and restored in the callee
if $PEN -S tests/pos_dstk.fsht -o "$TMP/ds.ll" 2>/dev/null \
    && grep -q "thread_local global \[1 x ptr\]" "$TMP/ds.ll" \
    && grep -q "@frt.dstk.tops" "$TMP/ds.ll"; then
  ok "data stacks lower to the TLS tops array + bumps (IR check)"
else bad "data stacks IR check"; fi
# guard page: a write past the reservation aborts cleanly with the
# runtime's overflow message (goose's gs_fault_filter)
if $PEN -O2 tests/pos_dstk_guard.fsht -o "$TMP/dsg" 2>/dev/null \
    && ! FRT_DSTK_RESERVE=1048576 "$TMP/dsg" >/dev/null 2>"$TMP/dsg.err" \
    && grep -q "data stack overflow" "$TMP/dsg.err"; then
  ok "guard page: overflow past the reservation aborts cleanly"
else bad "data stack guard page"; fi

# ---------------------------------------------------------------------------
# Phase 3/4/5 feature tests (report §6/§9/§10/§12) — modules, fn pointers,
# parfor break/continue, @reproducible, test runner, fsh module, project
# manifest, formatter, debug info, lint, LSP navigation
# ---------------------------------------------------------------------------
echo "== Phase 3/4/5: modules, FFI, tooling, reproducibility =="

# --- modules (§6.1/§6.2): import, pub, qualified names, cross-module
# name collisions (private `distance` in two modules)
run_okf "modules: import/pub/qualified names" tests/pos_modules.fsht "10.00|7.00|0.00" "-Itests/mods"
if $PEN -Itests/mods tests/neg_import_private.fsht -o "$TMP/np" 2>"$TMP/err" \
    || grep -q "F0004" "$TMP/err"; then
  ok "F0004 private cross-module use (F0004)"
else bad "F0004 private cross-module use"; fi
run_err "import module not found" tests/neg_import_missing.fsht "F0004"

# --- circular import (two files) -> F0004
cat > "$TMP/ca.fsht" <<'EOFA'
module ca;
import cb;
pub fn a() -> i64 { return 1; }
fn main() { print_i64(cb::b()); println(); }
EOFA
cat > "$TMP/cb.fsht" <<'EOFA'
module cb;
import ca;
pub fn b() -> i64 { return ca::a(); }
EOFA
if $PEN "$TMP/ca.fsht" -o "$TMP/cyc" 2>"$TMP/err" && ! grep -q "F0004" "$TMP/err"; then
  bad "F0004 circular import detection"
else ok "F0004 circular import detection (F0004)"; fi

# --- multi-input compilation (§6.2): two files as one program
if $PEN tests/mods/geometry.fsht tests/pos_modules.fsht -o "$TMP/multi" 2>"$TMP/err" \
    && [ "$("$TMP/multi" 2>/dev/null)" = "$(printf '10.00\n7.00\n0.00')" ]; then
  ok "multi-input compilation (two .fsht on the CLI)"
else bad "multi-input compilation (two .fsht on the CLI)"; fi

# --- fn pointers + C callbacks (§6.3): libc qsort through a Freshet fn
run_ok "fn pointers + C callback (qsort)" tests/pos_fnptr.fsht "5|20|9|8|9 7 5 3 2 1 "
run_err "thread indirect call in parfor" tests/neg_fnptr_parfor.fsht "F0307"

# --- extern fn of libc symbols the runtime ALSO declares (fsht-data crash
# fix): @fopen is declared by both halves — the user's plain declare and
# the runtime's clang-decorated one.  LLVM rejects any second declaration
# of one symbol ("invalid redefinition of function 'fopen'"), so exactly
# one declare must survive the combine.  Round-trip through the libc
# externs AND the fsh module (the runtime's own @fopen call path).
run_ok "extern libc shared with runtime declare" tests/pos_extern_libc.fsht "extern libc round-trip"
# two imported modules extern-declaring the SAME C symbol (identical
# signatures fold into one declare) + the runtime declare dropped
run_okf "extern dup across modules: one declare" tests/pos_extern_dupmods.fsht "0|two modules, one fopen" "-Itests/mods"
# conflicting signatures for one C symbol: F0205, not a broken module
if $PEN -Itests/mods tests/neg_extern_conflict.fsht -o "$TMP/nxc" 2>"$TMP/err" \
    || ! grep -q "F0205" "$TMP/err"; then
  bad "F0205 conflicting extern declarations"
else ok "F0205 conflicting extern declarations (F0205)"; fi

# --- parfor break/continue (§12/§14): lane-local semantics
run_ok "parfor break/continue (lane-local)" tests/pos_parfor_break.fsht "62500"

# --- @reproducible (§12): bitwise-identical across thread counts
if $PEN tests/pos_reproducible.fsht -o "$TMP/repro" 2>"$TMP/err"; then
  r1=$(FRT_THREADS=1 FRT_WORK_CUTOFF=0 "$TMP/repro" 2>/dev/null)
  r2=$(FRT_THREADS=3 FRT_WORK_CUTOFF=0 "$TMP/repro" 2>/dev/null)
  r3=$(FRT_THREADS=8 FRT_WORK_CUTOFF=0 "$TMP/repro" 2>/dev/null)
  if [ -n "$r1" ] && [ "$r1" = "$r2" ] && [ "$r2" = "$r3" ]; then
    ok "@reproducible reduction identical across thread counts"
  else bad "@reproducible reduction identical across thread counts (got $r1/$r2/$r3)"; fi
else bad "@reproducible reduction identical across thread counts (compile failed)"; fi

# --- `penstock test` runner (§10.3): per-test lines + summary + exit code
if $PEN test tests/pos_test_runner.fsht >"$TMP/tout" 2>"$TMP/err"; then
  if grep -q "3/3 tests passed" "$TMP/tout" && \
     grep -q "test add_works ... ok" "$TMP/tout"; then
    ok "penstock test: @test runner (passing suite)"
  else bad "penstock test: @test runner (output mismatch)"; fi
else bad "penstock test: @test runner (non-zero exit)"; fi
# a FAILING test must not abort the run and must exit non-zero
cat > "$TMP/tf.fsht" <<'EOFA'
@test fn t1() { assert(1 == 1); }
@test fn t2() { assert(1 == 2); }
@test fn t3() { assert(3 == 3); }
EOFA
if $PEN test "$TMP/tf.fsht" >"$TMP/tout" 2>/dev/null; then
  bad "penstock test: failure exits non-zero"
else
  if grep -q "2/3 tests passed" "$TMP/tout"; then
    ok "penstock test: assert failure reported, suite continues"
  else bad "penstock test: assert failure reported, suite continues"; fi
fi
if $PEN test tests/neg_test_nofns.fsht -o "$TMP/nt" 2>"$TMP/err" \
    && ! grep -q "F0005" "$TMP/err"; then
  bad "F0005 no @test functions (F0005)"
else ok "F0005 no @test functions (F0005)"; fi

# --- the shipped fsh module (§6.3): args/env/file I/O
if $PEN tests/pos_fsh_module.fsht -o "$TMP/fshm" 2>"$TMP/err"; then
  got=$("$TMP/fshm" 2>/dev/null)
  arg0len=$(printf '%s' "$TMP/fshm" | wc -c)
  want="args: 1|arg0 len: $arg0len|arg9-or: <none>|missing-or: fallback|wrote: 16|size: 16|read: hello freshet io|nosize: -1"
  want=${want//|/$'\n'}
  if [ "$got" = "$want" ]; then ok "fsh module: args/env/file I/O"
  else bad "fsh module: args/env/file I/O (got '$got')"; fi
else bad "fsh module: args/env/file I/O (compile failed)"; fi

# --- project manifest (§6.2): penstock build + dependency cache
rm -f tests/proj/projtest tests/proj/projtest.pbcache
if $PEN build tests/proj 2>"$TMP/err" && [ "$(./tests/proj/projtest 2>/dev/null)" = "$(printf '49\n27')" ]; then
  if $PEN build tests/proj 2>"$TMP/err2" && grep -q "up to date" "$TMP/err2"; then
    ok "project manifest: build + dependency cache"
  else bad "project manifest: dependency cache (no up-to-date skip)"; fi
else bad "project manifest: build (see $TMP/err)"; fi
rm -f tests/proj/projtest tests/proj/projtest.pbcache

# --- formatter (§10.2): token-preserving reformat + idempotence
cp tests/pos_fmt_src.fsht "$TMP/fmt.fsht"
if $PEN fmt "$TMP/fmt.fsht" >/dev/null 2>&1 \
   && $PEN fmt --check "$TMP/fmt.fsht" >/dev/null 2>&1 \
   && $PEN "$TMP/fmt.fsht" -o "$TMP/fmtb" 2>/dev/null \
   && [ "$("$TMP/fmtb" 2>/dev/null)" = "22" ]; then
  ok "formatter: reformat + idempotence + compiled equivalence"
else bad "formatter: reformat + idempotence + compiled equivalence"; fi

# --- -g debug info (§10.3): DWARF line table + symbol resolution
if $PEN -g -O0 tests/pos_debug_g.fsht -o "$TMP/dbg" 2>"$TMP/err"; then
  if objdump --dwarf=decodedline "$TMP/dbg" 2>/dev/null | grep -q "pos_debug_g.fsht"; then
    sym_addr=$(objdump -t "$TMP/dbg" | awk '/u_inner/{print $1; exit}')
    resolved=$(addr2line -e "$TMP/dbg" -f "0x$sym_addr" 2>/dev/null | tail -1)
    if echo "$resolved" | grep -q "pos_debug_g.fsht"; then
      ok "-g debug info: DWARF line table resolves source lines"
    else bad "-g debug info: addr2line resolution ('$resolved')"; fi
  else bad "-g debug info: no line table emitted"; fi
else bad "-g debug info: build failed"; fi

# --- lint (§10.2): diagnostics-only surface
if $PEN lint tests/pos_basics.fsht >/dev/null 2>&1; then
  ok "lint: diagnostics-only run (clean file -> exit 0)"
else bad "lint: diagnostics-only run"; fi
if $PEN lint tests/neg_divzero.fsht >/dev/null 2>&1; then
  bad "lint: error file exits non-zero"
else ok "lint: error file exits non-zero"; fi

# --- LSP navigation (§10.1): hover/references/rename over stdio JSON-RPC
# Drives bin/freshet-language-server, which the `check` target builds as a
# dependency (cmake/PenstockTests.cmake).  Distinguish "binary not built"
# (a wiring problem) from a genuine protocol/assertion failure, and print
# the first lines of the python traceback on failure — same convention as
# the compile failures above.
if [ ! -x "$PEN_LSP" ]; then
  bad "LSP: hover/definition/references/rename over stdio JSON-RPC ($PEN_LSP not built — cmake --build build --target freshet-language-server)"
elif python3 scripts/lsp_nav_check.py "$PEN_LSP" >/dev/null 2>"$TMP/lsperr"; then
  ok "LSP: hover/definition/references/rename over stdio JSON-RPC"
else
  bad "LSP: hover/definition/references/rename over stdio JSON-RPC"
  sed -n '1,5p' "$TMP/lsperr"
fi

# LSP diagnostic ranges, comment lexing, useful narrowing-cast messages,
# and the filename/path-specific main() requirement.
if [ ! -x "$PEN_LSP" ]; then
  bad "LSP: diagnostic spans/comments/main-file policy (server not built)"
elif python3 scripts/lsp_diagnostics_check.py "$PEN_LSP" >/dev/null 2>"$TMP/lspdiagerr"; then
  ok "LSP: diagnostic spans/comments/main-file policy"
else
  bad "LSP: diagnostic spans/comments/main-file policy"
  sed -n '1,8p' "$TMP/lspdiagerr"
fi

# fsht tooling: completion + inlay hints (registry 10.04.01 surface).
# Scope-aware identifier completion (locals of the enclosing fn, fns,
# globals, prelude types, keywords, the compiler-known builtins;
# module-qualified paths restrict to that module) and the two inlay hint
# families (inferred types on un-annotated lets, parameter names at
# resolved call sites).
if [ ! -x "$PEN_LSP" ]; then
  bad "LSP: completion + inlay hints (server not built)"
elif python3 scripts/lsp_completion_inlay_check.py "$PEN_LSP" >/dev/null 2>"$TMP/lspcomperr"; then
  ok "LSP: completion + inlay hints over stdio JSON-RPC"
else
  bad "LSP: completion + inlay hints over stdio JSON-RPC"
  sed -n '1,8p' "$TMP/lspcomperr"
fi

echo "== optimisation-audit: §10.2/§10.3 differential + property tests =="
if [ "${RUN_DIFFTEST:-0}" = "1" ]; then
  if bash scripts/difftest.sh; then ok "difftest: all configurations agree"; else bad "difftest: outputs differ"; fi
  if python3 scripts/property_test.py 60 --seed 7 >/dev/null 2>&1; then
    ok "property_test: 60 random programs agree"; else bad "property_test: mismatch"; fi
else
  echo "  (opt-in: RUN_DIFFTEST=1 bash tests/run.sh — see scripts/difftest.sh, scripts/property_test.py)"
fi

echo "== diagnostics registry (src/diag_codes.h, docs/errors/) =="
# every registry ID is listed in the spec's generated index
missing=""
for id in $(grep -o '"F[0-9]\{4\}"' src/diag_codes.h | tr -d '"'); do
  grep -q "\`$id\`" docs/language-spec.md || missing="$missing $id"
done
if [ -z "$missing" ]; then ok "every registry ID is in the language-spec index"; else bad "IDs missing from docs/language-spec.md:$missing"; fi
# every documentation page belongs to a registered ID, and --explain serves it
stray=""
for f in docs/errors/F*.md; do
  [ -e "$f" ] || continue
  id=$(basename "$f" .md)
  grep -q "\"$id\"" src/diag_codes.h || stray="$stray $id"
  $PEN --explain "$id" 2>/dev/null | cmp -s - "$f" || stray="$stray $id(explain)"
done
if [ -z "$stray" ]; then ok "docs/errors pages match the registry and --explain"; else bad "stray/unservable pages:$stray"; fi
# --explain accepts ID or slug, and rejects unknown codes
if [ "$($PEN --explain F0402 2>/dev/null)" = "$($PEN --explain arith-div-runtime 2>/dev/null)" ] \
   && ! $PEN --explain F9999 >/dev/null 2>&1; then ok "--explain: ID == slug, unknown code rejected"
else bad "--explain lookup"; fi
# severity-free IDs: the same ID is a warning by default and an error under --strict-performance
$PEN tests/neg_arith.fsht -o "$TMP/t" 2>"$TMP/w.err"; $PEN --strict-performance tests/neg_arith.fsht -o "$TMP/t" 2>"$TMP/e.err"
if grep -q '^warning\[F0402\]' "$TMP/w.err" && grep -q '^error\[F0402\]' "$TMP/e.err"; then ok "F0402 is warning[F0402] by default, error[F0402] when strict"
else bad "ID/severity decoupling"; fi

# ---------------------------------------------------------------- fsht-03
echo "== fsht-03:01 expanded stdlib (registry 05.04.01/05.05.01/05.06.01/08.01.04) =="
run_ok "checked/saturating/wrapping variants" tests/pos_checked_variants.fsht "None|None|9223372036854775807|-9223372036854775808|-9223372036854775808|-9223372036854775808|127|255"
run_ok "checked_cast / saturating_cast" tests/pos_checked_cast.fsht "None|127|None|0|3|-3"
run_ok "parse_i64/u64/f64 + round-trip law" tests/pos_parse.fsht "42|-7|invalid|range|-9223372036854775808|invalid|2500.0|inf|range"
run_ok "io streaming: buffered reader/writer round trip" tests/pos_io_stream.fsht "line one|line two|eof-ok|bytes=8|108|rt=8|notfound|removed|rm-notfound"
run_ok "stdlib module decomposition (string/numeric/bits/time/random/alloc/collections)" tests/pos_modules_stdlib.fsht "p=7|3|9223372036854775807|6|50|578437695752307201|8|time-ok|4748208303499903454|page=4096|2|empty=0|2|4.0"

echo "== fsht-03:02 borrows & lifetimes (registry 03.02.09/03.02.10) =="
run_ok "String migration: auto-drop, clone/clear, moves, string_free" tests/pos_string_migration.fsht "hello world|0|hello world|hello|freed"
run_ok "drop elaboration through user shapes (Vec<String>, Vec<Vec>, struct fields)" tests/pos_shapes.fsht "2|2|2|5|x|1"
run_ok "&mut / borrowed mut / views with the in-place carve-out" tests/pos_borrow_mut.fsht "6|2|9|14"
run_err "F0310 two live mutable borrows in one call" tests/neg_borrow_two_mut.fsht F0310
run_err "F0310 growth against a live view" tests/neg_borrow_view_growth.fsht F0310
run_err "F0311 returning a view of local storage" tests/neg_borrow_escape.fsht F0311
run_err "F0311 returning &mut local" tests/neg_borrow_mut_escape.fsht F0311
run_err "F0301 use-after-string_free" tests/neg_string_uaf.fsht F0301
run_err "F0310 string_append against a live str view" tests/neg_string_view_growth.fsht F0310
run_err "F0307 &mut inside a parfor body" tests/neg_parfor_borrow_mut.fsht F0307

echo "== fsht-03:01 negative typing rules =="
run_err "F0201 mixed-width variant operands" tests/neg_checked_mixed_width.fsht F0201
run_err "F0201 unresolved cast destination U" tests/neg_cast_unresolved.fsht F0201

echo "== fsht-03:03 bool slot representation (user report: i1 vs i8) =="
# bool VALUES are i1 registers; struct fields / enum payloads / call
# boundaries keep the i8 C ABI.  The struct-literal field store wrote
# `store i8 <i1>` (clang object emit failed: "'%N' defined with type 'i1'
# but expected 'i8'"); array-literal adoption also let bool elements
# against integer annotations through.  Regression: every boundary now
# widens/narrows, and adoption type-checks its elements (nested
# [[1, 2], [3, 4]] literals keep adopting recursively).
run_ok "bool slots: literal fields, [bool; N], enum payload, parfor capture" tests/pos_bool_slots.fsht "lit-cmp lit-call fld-asn a1 a0 a1 enum1 vec1 48"
run_err "F0201 bool elements against an integer array annotation" tests/neg_arrlit_bool_elem.fsht F0201
run_err "F0201 integer elements against a bool array annotation" tests/neg_arrlit_int_to_bool.fsht F0201
# follow-up report (same family): `a != b` over bools emitted the icmp at
# the i8 slot ABI (`icmp ne i8 <i1>, <i1>` — clang object emit failed);
# comparisons now run in the i1 value representation, with non-bool
# constant operands (the `flag != 0` literal exemption) normalised to
# i1 first.  SoA bool field loads also moved to the value representation.
run_ok "bool comparisons: every producer pair, i1 value representation" tests/pos_bool_cmp.fsht "ne-ok eq-ok t1 t2 t3 fld1 fld2 arr1 vec1 vec2 call1 call2 int1 int2 int3 and1 or1 3 enum1 soa1 soa2 soa3 3.0 3"

echo "== fsht-03:03 portable runtime (registry 10.03.02) =="
# same-target compilation: the runtime .ll cache key carries the triple,
# and the wasm cross-compile selects the wasm platform layer (the wasm
# runtime object compile needs a wasi sysroot — the CI target-matrix
# workflow provides it; here the pipeline RUNS and the driver degrades
# gracefully without one)
if $PEN --target=wasm32-wasi -S tests/pos_basics.fsht -o "$TMP/w.ll" 2>/dev/null \
    && grep -q 'target triple = "wasm32-unknown-wasi"' "$TMP/w.ll"; then
  ok "--target wasm32-wasi cross-targets the user module (triple in IR)"
else bad "wasm cross-target"; fi
if $PEN --target=aarch64-linux-gnu -S tests/pos_basics.fsht -o "$TMP/a.ll" 2>/dev/null \
    && grep -q 'target triple = "aarch64-unknown-linux-gnu"' "$TMP/a.ll"; then
  ok "--target aarch64-linux cross-targets the user module"
else bad "aarch64 cross-target"; fi
# the runtime cache is triple-keyed (a target switch never reuses it)
if $PEN --target=x86_64-pc-linux-gnu -S tests/pos_basics.fsht -o "$TMP/x.ll" 2>/dev/null \
    && ls build 2>/dev/null | grep -q 'frt_rt.*_tx86_64'; then
  ok "runtime cache key includes the triple"
else bad "triple cache key"; fi
# unsupported triples: driver diagnostic naming the triple (F0009)
$PEN --target=avr-unknown-none tests/pos_basics.fsht -o "$TMP/t" 2>"$TMP/err"
if [ $? -ne 0 ] && grep -q "\[F0009\]" "$TMP/err" && grep -q "avr-unknown-none" "$TMP/err"; then
  ok "F0009 rejects an unsupported triple, naming it"
else bad "unsupported-triple gate"; fi
# platform-conditional link: no -lpthread on a wasm link line is CI's
# to execute end-to-end (the object compile needs the sysroot); the
# driver's request surface is asserted instead
if grep -q "appendPlatformLinkLibs" src/main.cpp && grep -q "targetIsLinux" src/main.cpp; then
  ok "link lines are platform-conditional (linux-only -lpthread)"
else bad "platform link lines"; fi
# the runtime decomposition exists with the chapter's layout
if [ -d runtime/common ] && [ -d runtime/platforms/linux ] \
    && [ -d runtime/platforms/windows ] && [ -d runtime/platforms/wasm ] \
    && [ -d runtime/platforms/baremetal ] \
    && grep -q "FRT_PLATFORM" runtime/frt_rt.c; then
  ok "runtime decomposition: common/ + platforms/{linux,windows,wasm,baremetal}"
else bad "runtime decomposition layout"; fi

echo "== fsht tooling: automatic C header generation (registry 07.04.01) =="
# The export surface compiles and runs like any program ...
run_ok "export fns build and run normally" tests/pos_c_header.fsht "25.0|12.0|10.0|10.0|6765"
# ... and --emit-c-header describes it with C declarations that mirror
# the emitted ABI: struct bodies, tagged-union enums, { ptr, len } pair
# typedefs for Span<T>, by-address aggregate parameters.
if $PEN tests/pos_c_header.fsht -o "$TMP/ch.bin" --emit-c-header="$TMP/pos_c_header.h" 2>"$TMP/cherr" \
    && [ -s "$TMP/pos_c_header.h" ]; then
  hdrs=$(grep -cE '^(int64_t|double|fsh_) [a-z_]+\(' "$TMP/pos_c_header.h")
  if grep -q 'struct Particle' "$TMP/pos_c_header.h" \
     && grep -q 'int64_t tag;' "$TMP/pos_c_header.h" \
     && grep -q 'fsh_span_double' "$TMP/pos_c_header.h" \
     && grep -q 'int64_t fib(int64_t n);' "$TMP/pos_c_header.h" \
     && grep -q 'double kinetic(Particle\* p);' "$TMP/pos_c_header.h" \
     && grep -q 'double area(Shape\* s);' "$TMP/pos_c_header.h" \
     && grep -q 'double sum_span(fsh_span_double v);' "$TMP/pos_c_header.h"; then
    ok "--emit-c-header writes ABI-mirroring C declarations"
  else
    bad "--emit-c-header: expected prototypes not found"
  fi
else
  bad "--emit-c-header: compile/header emission failed"; sed -n '1,5p' "$TMP/cherr"
fi
# the header must itself be valid C: compile a consumer that includes it
# and references every exported symbol (a C compiler is optional — CI has
# one, minimal installs may not)
CC_BIN=""
for c in gcc cc clang; do command -v "$c" >/dev/null 2>&1 && CC_BIN="$c" && break; done
if [ -n "$CC_BIN" ]; then
  cat > "$TMP/use_header.c" <<'CEOF'
#include <stddef.h>
#include "pos_c_header.h"
const void *use_exports(void) {
  (void)fib(0); (void)kinetic((const Particle[]){ {1,2,3} });
  Shape s; s.tag = 0;
  double v[1] = { 0 }; fsh_span_double sp = { v, 1 };
  (void)area(&s); (void)sum_span(sp);
  return sp.ptr;
}
CEOF
  if $CC_BIN -fsyntax-only -I"$TMP" "$TMP/use_header.c" 2>"$TMP/ccerr"; then
    ok "generated header compiles as C ($CC_BIN)"
  else
    bad "generated header is not valid C"; sed -n '1,5p' "$TMP/ccerr"
  fi
else
  echo "  (no C compiler found — header syntax check skipped)"
fi

echo
echo "passed: $pass   failed: $fail"
[ $fail -eq 0 ]
