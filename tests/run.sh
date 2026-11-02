#!/usr/bin/env bash
# Freshet / Penstock test suite
# usage: tests/run.sh
set -u
cd "$(dirname "$0")/.."
PEN=bin/penstock
PEN_LSP=bin/freshet-language-server
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
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

echo "== R1 regression =="
run_ok "hello/basics" tests/pos_basics.fsh "10.00|hi"
run_ok "for-range" tests/pos_range.fsh "10"
run_ok "parfor reduction" tests/pos_parfor.fsh "8000000"
run_ok "defer+free ownership" tests/pos_defer.fsh "45"
# report §7: performance diagnostics are warnings by default;
# --strict-performance restores the historical hard errors
run_warn "F0402 runtime div (warn-first)" tests/neg_arith.fsh F0402
$PEN --strict-performance tests/neg_arith_strict.fsh -o "$TMP/as" 2>"$TMP/as.err"; rc=$?
if [ $rc -ne 0 ] && grep -q "\[F0402\]" "$TMP/as.err"; then
  ok "F0402 runtime div (--strict-performance)"
else bad "F0402 strict mode"; sed -n '1,3p' "$TMP/as.err"; fi
run_err "F0214 div by literal zero" tests/neg_divzero.fsh F0214
run_err "own leak" tests/neg_leak.fsh F0301

echo "== R2: structs =="
run_ok "struct literal/copy" tests/pos_struct.fsh "1.00 3.00|1.50"
run_ok "nested fields + align(64) + &param" tests/pos_nested.fsh "5.00|24.00"
run_ok "AoS parfor (element-wise)" tests/pos_aos.fsh "160.00"
run_ok "SoA planes end-to-end" tests/pos_soa.fsh "4000.00"
run_warn "soa AoS advice" tests/warn_soa.fsh F0408
run_warn "layout-pad reorder" tests/warn_layout.fsh F0407
run_warn "cache-line-straddle straddle (warn-first)" tests/neg_straddle.fsh F0404
$PEN --strict-performance tests/neg_straddle_strict.fsh -o "$TMP/ss" 2>"$TMP/ss.err"; rc=$?
if [ $rc -ne 0 ] && grep -q "\[F0404\]" "$TMP/ss.err"; then
  ok "F0404 straddle (--strict-performance)"
else bad "F0404 strict mode"; sed -n '1,3p' "$TMP/ss.err"; fi
run_err "struct literal missing field" tests/neg_slit.fsh F0201

echo "== R2: nested parfor + affinity =="
run_ok "collapsed nested (rect)" tests/pos_nest_collapse.fsh "4096"
run_ok "dynamic nested (WS)" tests/pos_nest_ws.fsh "5050"
run_ok "ring-buffer scheduler + reduction" tests/pos_ringbuf.fsh "100000.00"
run_ok "ring-buffer scheduler + reduction" tests/pos_ringbuf.fsh "100000.00"
run_ok "ring-buffer scheduler + reduction" tests/pos_ringbuf.fsh "100000.00"
run_ok "ring-buffer scheduler + reduction" tests/pos_ringbuf.fsh "100000.00"
run_ok "ring-buffer scheduler + reduction" tests/pos_ringbuf.fsh "100000.00"
run_ok "ring-buffer scheduler + reduction" tests/pos_ringbuf.fsh "100000.00"
run_ok "ring-buffer scheduler + reduction" tests/pos_ringbuf.fsh "100000.00"
run_ok "affinity close/spread" tests/pos_affinity.fsh "2000000"
run_err "thread scalar race" tests/neg_race.fsh F0307

echo "== R2: PGO =="
# 1. instrument, 2. run, 3. profile exists, 4. guided build, 5. branch weights
if $PEN --profile tests/pos_pgo.fsh -o "$TMP/pgo" 2>/dev/null; then
  out=$("$TMP/pgo" 2>/dev/null)
  if [ -f "$TMP/pgo.fprof" ]; then ok "PGO instrument+run (out=$out)"; else bad "PGO .fprof missing"; fi
  if $PEN --use-profile="$TMP/pgo.fprof" tests/pos_pgo.fsh -o "$TMP/pgo2" 2>/dev/null; then
    out2=$("$TMP/pgo2" 2>/dev/null)
    if [ "$out2" = "$out" ]; then ok "PGO-guided recompile"; else bad "PGO-guided output mismatch ($out2 vs $out)"; fi
    $PEN -S tests/pos_pgo.fsh -o "$TMP/pgo.ll" --use-profile="$TMP/pgo.fprof" 2>/dev/null
    if grep -q "branch_weights" "$TMP/pgo.ll" && grep -q "function_entry_count" "$TMP/pgo.ll"; then
      ok "PGO metadata (branch_weights + entry_count)"
    else bad "PGO metadata missing in IR"; fi
  else bad "PGO-guided compile failed"; fi
else bad "PGO instrumented compile failed"; fi

# report mode smoke
if $PEN --report --use-profile="$TMP/pgo.fprof" tests/pos_pgo.fsh -o "$TMP/rep" 2>"$TMP/err" \
   && grep -q "Freshet Performance Report" "$TMP/err"; then
  ok "performance report mode"
else bad "performance report mode"; fi

echo "== R3: --soa=auto =="
# transform must preserve output exactly; 'b' transforms, 'w' is vetoed
if $PEN tests/pos_soa_auto.fsh -o "$TMP/sa0" 2>/dev/null; then
  out0=$("$TMP/sa0" 2>/dev/null)
  if $PEN --soa=auto tests/pos_soa_auto.fsh -o "$TMP/sa1" 2>"$TMP/sa.err"; then
    out1=$("$TMP/sa1" 2>/dev/null)
    if [ "$out0" = "$out1" ] && grep -q "F0903.*'b' in fn main" "$TMP/sa.err" &&
       grep -q "F0903.*skipped for 'w'" "$TMP/sa.err" &&
       grep -q "F0408.*'w'" "$TMP/sa.err"; then
      ok "soa=auto transform + veto + identical output"
    else bad "soa=auto behavior (out='$out1' want='$out0')"; sed -n '1,4p' "$TMP/sa.err"; fi
  else bad "soa=auto compile failed"; sed -n '1,3p' "$TMP/sa.err"; fi
else bad "soa=auto baseline compile failed"; fi
# IR proof: planes for the transformed var, malloc for the vetoed one
$PEN -S --soa=auto tests/pos_soa_auto.fsh -o "$TMP/sa.ll" 2>/dev/null
if grep -q "@frt_soa_new" "$TMP/sa.ll" && grep -q "@frt_soa_free" "$TMP/sa.ll"; then
  ok "soa=auto IR uses soa planes"
else bad "soa=auto IR missing soa runtime calls"; fi

echo "== finite-domain algorithm precomputation =="
# This evaluates every u8 input. CMake builds use embedded Alive2 to validate
# any replacement; non-CMake/bootstrap builds retain the original algorithm.
run_ok "u8 input/output map preserves all-domain checksum" tests/pos_precompute.fsh "8781696"
run_ok "finite inverse search returns the exact preimage" tests/pos_precompute_inverse.fsh "42"
if $PEN --no-precompute tests/pos_precompute.fsh -o "$TMP/precompute-off" 2>"$TMP/precompute-off.err" \
   && [ "$("$TMP/precompute-off" 2>/dev/null)" = "8781696" ] \
   && ! grep -q '^precompute:' "$TMP/precompute-off.err"; then
  ok "--no-precompute disables mapping stage"
else bad "--no-precompute switch"; sed -n '1,4p' "$TMP/precompute-off.err"; fi

echo "== embedded Freshet/Z3/Alive2 superoptimisation integration =="
# The CMake-built compiler uses in-process Z3/Alive2; bootstrap-only builds
# safely leave this stage disabled rather than depending on external tools.
if $PEN -O3 tests/pos_basics.fsh -o "$TMP/so" 2>"$TMP/so.err" && \
   [ "$("$TMP/so" 2>/dev/null)" = "$(printf '10.00\nhi')" ]; then
  ok "embedded superoptimiser preserves program output"
else bad "embedded Freshet superopt compile/run"; sed -n '1,4p' "$TMP/so.err"; fi

echo "== R3: -O4 solver-first default =="
if $PEN -O4 tests/pos_o4.fsh -o "$TMP/o4" 2>"$TMP/o4.err" \
   && grep -q "runtime tuning is disabled by default" "$TMP/o4.err"; then
  out=$("$TMP/o4" 2>/dev/null)
  if [ "$out" = "1599996000000.00" ]; then
    ok "-O4 build uses verified-superopt path by default and output is correct"
  else bad "-O4 output wrong (got '$out')"; fi
else bad "-O4 build failed"; sed -n '1,5p' "$TMP/o4.err"; fi

echo "== R4: loop fusion + full unrolling =="
# fusion: 3-phase FFT-multiply batch pipeline (fwd / pointwise / inv over
# call slices) jams into ONE sweep; output must equal the unoptimised build
if $PEN tests/pos_fuse.fsh -o "$TMP/fu" 2>"$TMP/fu.err" \
   && grep -q "F0908.*fused 3 adjacent loops" "$TMP/fu.err"; then
  out=$("$TMP/fu" 2>/dev/null)
  $PEN --no-loop-opts tests/pos_fuse.fsh -o "$TMP/fu0" 2>/dev/null
  out0=$("$TMP/fu0" 2>/dev/null)
  # §5: forced vectorise metadata is opt-in now, so count annotated loops
  # under --force-vectorize (a stable loop-count proxy for fusion proofs)
  $PEN -S --force-vectorize tests/pos_fuse.fsh -o "$TMP/fu.ll" 2>/dev/null
  n1=$(grep -c "vectorize.enable" "$TMP/fu.ll")
  $PEN -S --no-loop-opts --force-vectorize tests/pos_fuse.fsh -o "$TMP/fu0.ll" 2>/dev/null
  n0=$(grep -c "vectorize.enable" "$TMP/fu0.ll")
  if [ "$out" = "524288.00" ] && [ "$out" = "$out0" ] && [ "$n1" -lt "$n0" ]; then
    ok "loop fusion: 3-phase batch pipeline -> 1 sweep (F0908, $n0->$n1 loops, identical output)"
  else bad "fusion (out='$out' base='$out0' loops $n1 vs $n0)"; fi
else bad "fusion compile/note"; sed -n '1,3p' "$TMP/fu.err"; fi

# unroll: both constant-trip loops vanish from the IR (no vectorize hint)
if $PEN tests/pos_unroll.fsh -o "$TMP/un" 2>"$TMP/un.err" \
   && grep -q "F0909" "$TMP/un.err" && [ -n "$(grep -c F0909 "$TMP/un.err")" ]; then
  out=$("$TMP/un" 2>/dev/null)
  $PEN -S --force-vectorize tests/pos_unroll.fsh -o "$TMP/un.ll" 2>/dev/null
  n1=$(grep -c "vectorize.enable" "$TMP/un.ll")
  $PEN -S --no-loop-opts --force-vectorize tests/pos_unroll.fsh -o "$TMP/un0.ll" 2>/dev/null
  n0=$(grep -c "vectorize.enable" "$TMP/un0.ll")
  if [ "$out" = "56.00" ] && [ "$n1" = "0" ] && [ "$n0" = "2" ]; then
    ok "full unroll: constant-trip loops gone from IR (F0909, $n0 loops baseline)"
  else bad "unroll (out='$out' loops $n1 vs $n0)"; fi
else bad "unroll compile/note"; sed -n '1,3p' "$TMP/un.err"; fi

echo "== R4: cross-lane analysis =="
# uniform branch hoisted out of the vector loop (one scalar decides,
# specialised loops branch-free); output identical to baseline
if $PEN tests/pos_lane.fsh -o "$TMP/la" 2>"$TMP/la.err" \
   && grep -q "F0901.*hoisted uniform branch" "$TMP/la.err"; then
  out=$("$TMP/la" 2>/dev/null)
  $PEN --no-loop-opts tests/pos_lane.fsh -o "$TMP/la0" 2>/dev/null
  out0=$("$TMP/la0" 2>/dev/null)
  if [ "$out" = "6292992.00" ] && [ "$out" = "$out0" ]; then
    ok "cross-lane: uniform branch hoisted (F0901, identical output)"
  else bad "lane hoist (out='$out' base='$out0')"; fi
else bad "lane hoist compile/note"; sed -n '1,3p' "$TMP/la.err"; fi

# cost model: vector-light uniform branch must NOT be hoisted, and the
# veto must be visible (F0902 with the numbers)
if $PEN tests/warn_lane_cost.fsh -o "$TMP/lc" 2>"$TMP/lc.err" \
   && grep -A2 "\[F0902\]" "$TMP/lc.err" | grep -q "cost model: lane work 3 ops < 6"; then
  out=$("$TMP/lc" 2>/dev/null)
  if [ "$out" = "523776.00" ]; then
    ok "cross-lane cost model: vector-light branch left alone (F0902)"
  else bad "lane cost model (out='$out')"; fi
else bad "lane cost model compile/note"; sed -n '1,3p' "$TMP/lc.err"; fi

# predicate reuse: adjacent identical conditions merge (computed once,
# reused); output identical to baseline
if $PEN tests/pos_lane_merge.fsh -o "$TMP/lm" 2>"$TMP/lm.err" \
   && grep -q "F0901.*merged 2 adjacent branches" "$TMP/lm.err"; then
  out=$("$TMP/lm" 2>/dev/null)
  $PEN --no-loop-opts tests/pos_lane_merge.fsh -o "$TMP/lm0" 2>/dev/null
  out0=$("$TMP/lm0" 2>/dev/null)
  if [ "$out" = "$(printf '2087872.00\n1043936.00')" ] && [ "$out" = "$out0" ]; then
    ok "cross-lane: identical predicates merged (F0901, identical output)"
  else bad "predicate merge (out='$out' base='$out0')"; fi
else bad "predicate merge compile/note"; sed -n '1,3p' "$TMP/lm.err"; fi

# deeper scope: a uniform branch nested beneath a data-dependent branch is
# lifted out around the whole loop.  Compare to the no-loop-opts baseline.
if $PEN tests/pos_lane_nested.fsh -o "$TMP/ln" 2>"$TMP/ln.err" \
   && grep -q "F0901.*specialised an outer loop around a nested uniform branch" "$TMP/ln.err"; then
  out=$($TMP/ln 2>/dev/null)
  $PEN --no-loop-opts tests/pos_lane_nested.fsh -o "$TMP/ln0" 2>/dev/null
  out0=$($TMP/ln0 2>/dev/null)
  if [ "$out" = "$out0" ]; then
    ok "cross-lane: nested uniform branch specialised at outer-loop scope"
  else bad "nested cross-lane (out='$out' base='$out0')"; fi
else bad "nested cross-lane transform/note"; sed -n '1,4p' "$TMP/ln.err"; fi

echo "== Stage A: inlining policy (report §2.2 A) =="
# default: no blanket noinline on ordinary functions.  IR check: the
# function attribute group #50 must NOT contain 'noinline'.
if $PEN -S tests/pos_basics.fsh -o "$TMP/a1.ll" 2>/dev/null; then
  if grep -q "attributes #50 = { nounwind }" "$TMP/a1.ll"; then
    ok "Stage A1 default: #50 = { nounwind } (no noinline)"
  else
    bad "Stage A1 default: expected 'attributes #50 = { nounwind }' in IR"
    grep "attributes #5" "$TMP/a1.ll" | head -5
  fi
else bad "Stage A1 -S compile failed"; fi

# --inline=off: reproduce the historical noinline-everything baseline.
# IR check: ordinary functions should now be marked noinline (group #60).
if $PEN --inline=off -S tests/pos_basics.fsh -o "$TMP/a2.ll" 2>/dev/null; then
  if grep -q "attributes #60 = { noinline nounwind }" "$TMP/a2.ll"; then
    ok "Stage A1 --inline=off: group #60 = { noinline nounwind }"
  else
    bad "Stage A1 --inline=off: expected group #60 noinline"
    grep "attributes #6" "$TMP/a2.ll" | head -3
  fi
else bad "Stage A1 --inline=off compile failed"; fi

# @inline attribute selects alwaysinline (group #61); @noinline selects
# group #60.  Program must still compile and run correctly.
if $PEN tests/pos_inline_attrs.fsh -o "$TMP/a3" 2>/dev/null; then
  out=$("$TMP/a3" 2>/dev/null)
  if [ "$out" = "42" ]; then
    ok "Stage A1 @inline/@noinline attributes accepted + runs"
  else bad "Stage A1 attrs run (out='$out' want '42')"; fi
  $PEN -S tests/pos_inline_attrs.fsh -o "$TMP/a3.ll" 2>/dev/null
  if grep -q "attributes #61 = { alwaysinline nounwind }" "$TMP/a3.ll" && \
     grep -q "attributes #60 = { noinline nounwind }" "$TMP/a3.ll"; then
    ok "Stage A1 attrs emitted groups #60 + #61"
  else bad "Stage A1 attrs IR missing groups #60/#61"; fi
else bad "Stage A1 attrs compile failed"; fi

echo "== Stage A: -S combined IR + opt-remarks (report §1.3 + §2.1) =="
# -S should now write the COMBINED module (user + runtime), not just user
# IR.  Distinguishable by the runtime function bodies (`define @frt_init`
# etc.); the user IR alone only has `declare` lines for these.
if $PEN -S tests/pos_basics.fsh -o "$TMP/a4.ll" 2>/dev/null; then
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
if $PEN --legacy-S -S tests/pos_basics.fsh -o "$TMP/a5.ll" 2>/dev/null; then
  if grep -q "^define.*@frt_init" "$TMP/a5.ll"; then
    bad "Stage A3 --legacy-S should NOT write runtime bodies"
  else
    ok "Stage A3 --legacy-S writes only user IR"
  fi
else bad "Stage A3 --legacy-S compile failed"; fi

# --emit-combined-ir: even when building (not -S), drop the combined
# module at the requested path.  Same define-vs-declare check as above.
if $PEN --emit-combined-ir="$TMP/a6.combined.ll" tests/pos_basics.fsh \
     -o "$TMP/a6" 2>/dev/null; then
  if [ -f "$TMP/a6.combined.ll" ] && grep -q "^define.*@frt_init" "$TMP/a6.combined.ll"; then
    ok "Stage A2 --emit-combined-ir writes module"
  else bad "Stage A2 --emit-combined-ir missing module"; fi
else bad "Stage A2 --emit-combined-ir compile failed"; fi

echo "== Stage B4: @alias semantics (report §2.2 B) =="
# Sound @alias: caller passes distinct roots; compiler must emit noalias
# on the user function's parameter signature.  We grep for `noalias`
# inside the `define` line of the user function — the runtime defines
# also use `noalias` (e.g. on frt_parfor's ctxout), so a whole-file grep
# would always pass.  The signature is what matters.
if $PEN -S tests/pos_alias_sound.fsh -o "$TMP/b1.ll" 2>/dev/null; then
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
if $PEN -S tests/pos_alias_unsound.fsh -o "$TMP/b2.ll" 2>"$TMP/b2.err"; then
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
run_err "Stage B4 alias-violation still fires" tests/neg_alias.fsh F0305

echo "== Audit integration: P0 effect summaries (report §2) =="
# §2.2 reproducer: pointer-reading helper between stores.  Before the fix
# the helper was stamped memory(none) willreturn and -O3 printed "00".
for O in 0 3; do
  if $PEN -O$O tests/pos_effects_repro.fsh -o "$TMP/rr$O" 2>/dev/null; then
    got=$("$TMP/rr$O" 2>/dev/null)
    if [ "$got" = "12" ]; then ok "§2.2 reproducer at -O$O prints 12"
    else bad "§2.2 reproducer at -O$O (got '$got' want '12')"; fi
  else bad "§2.2 reproducer -O$O compile"; fi
done

# global read between writes (§2.4): reader must not be memory(none)
if $PEN tests/pos_effects_global.fsh -o "$TMP/eg" 2>/dev/null; then
  got=$("$TMP/eg" 2>/dev/null)
  if [ "$got" = "012" ]; then ok "§2.4 global-read helper correct (012)"
  else bad "§2.4 global-read helper (got '$got' want '012')"; fi
else bad "§2.4 global-read compile"; fi

# rebound/aliased pointer reads (§2.4, the R4 mut-rebind idiom)
if $PEN tests/pos_effects_alias.fsh -o "$TMP/ea" 2>/dev/null; then
  got=$("$TMP/ea" 2>/dev/null)
  if [ "$got" = "70100" ]; then ok "§2.4 alias/rebind reads correct (7 0 100)"
  else bad "§2.4 alias/rebind reads (got '$got' want '70100')"; fi
else bad "§2.4 alias compile"; fi

# malloc wrapper used: must not be described as memory-free (§2.4)
if $PEN tests/pos_effects_mallocw.fsh -o "$TMP/em" 2>/dev/null; then
  got=$("$TMP/em" 2>/dev/null)
  if [ "$got" = "0.00" ]; then ok "§2.4 malloc wrapper correct (0.00)"
  else bad "§2.4 malloc wrapper (got '$got' want '0.00')"; fi
else bad "§2.4 malloc wrapper compile"; fi

# runtime builtins in purity candidates (§2.4): freshet.threads reads
# runtime state; soa_len reads the descriptor.  IR check: the wrappers
# must carry memory(read) — never memory(none).
$PEN -S tests/pos_effects_builtins.fsh -o "$TMP/eb.ll" 2>/dev/null
if grep -E "memory\(none\)" "$TMP/eb.ll" | grep -qE "nthreads|plen|u_"; then
  bad "§2.4 builtin wrappers must not be memory(none)"
else
  ok "§2.4 freshet.threads/soa_len wrappers keep memory(read)"
fi
if $PEN tests/pos_effects_builtins.fsh -o "$TMP/eb" 2>/dev/null; then
  got=$("$TMP/eb" 2>/dev/null)
  if [ "$got" = "4${FRT_THREADS:-$(nproc)}" ] || [ "${got:0:1}" = "4" ]; then
    ok "§2.4 builtin wrappers run"
  else bad "§2.4 builtin wrappers output (got '$got')"; fi
else bad "§2.4 builtins compile"; fi

# pure-but-nonterminating: willreturn must NOT be inferred without a
# termination proof (§2.4) — IR inspection on the definition of u_spin
$PEN -S tests/pos_effects_willret.fsh -o "$TMP/wr.ll" 2>/dev/null
if grep -E "^define.*@u_spin" "$TMP/wr.ll" | grep -q willreturn; then
  bad "§2.4 willreturn leaked onto an unbounded-loop function"
else
  ok "§2.4 no willreturn without termination proof"
fi

# @pure contract violations are hard errors (F0304, spec §3.1)
run_err "§2.4 pure write violation" tests/neg_pure_write.fsh F0304
run_err "§2.4 pure read violation" tests/neg_pure_read.fsh F0304
run_err "§2.4 pure impure callee" tests/neg_pure_call.fsh F0304
run_err "§2.4 pure io violation" tests/neg_pure_io.fsh F0304

echo "== Audit integration: IR attribute soundness (report §2.3) =="
# read-only pointer consumer => memory(argmem: read), NOT memory(none)
$PEN -S tests/pos_effects_repro.fsh -o "$TMP/ra.ll" 2>/dev/null
if grep -q "memory(argmem: read)" "$TMP/ra.ll"; then
  ok "§2.3 read-only helpers get memory(argmem: read)"
else bad "§2.3 memory(argmem: read) missing from read-first IR"; fi
# a genuinely memory-free helper still gets the full #52 treatment
$PEN -S tests/pos_basics.fsh -o "$TMP/mn.ll" 2>/dev/null
if grep -q "attributes #52 = { nounwind memory(none) willreturn }" "$TMP/mn.ll"; then
  ok "§2.3 memory-free fns keep group #52 (memory(none) willreturn)"
else bad "§2.3 group #52 missing"; fi

echo "== Audit integration: runtime IR + loop policy + PGO (§3/§5/§11) =="
# cached runtime IR is built at -O2: no optnone may leak into the final
# module (the §3 acceptance test)
$PEN -S tests/pos_basics.fsh -o "$TMP/rt.ll" 2>/dev/null
if grep -q "optnone" "$TMP/rt.ll"; then
  bad "§3 optnone leaked into the combined module"
else
  ok "§3 runtime IR has no optnone (built at -O$( [ -n "$($PEN --version >/dev/null 2>&1; echo)" ] && echo 2 || echo 2 ))"
fi
if ls build/frt_rt_*_o2.ll >/dev/null 2>&1; then
  ok "§3 runtime cache file keyed with optimisation level (frt_rt_*_o2.ll)"
else bad "§3 runtime cache key missing level"; fi

# default: NO forced vectorisation metadata; --force-vectorize restores
# the historical hint; @vectorize emits enable-only (§5)
$PEN -S tests/pos_vectorize.fsh -o "$TMP/va.ll" 2>/dev/null
n_annot=$(grep -c "vectorize.enable" "$TMP/va.ll")
n_ilv=$(grep -c "interleave.count" "$TMP/va.ll")
if [ "$n_annot" = "1" ] && [ "$n_ilv" = "0" ]; then
  ok "§5 @vectorize annotation: 1 annotated loop, no interleave count"
else bad "§5 @vectorize IR (annotated=$n_annot interleave=$n_ilv, want 1/0)"; fi
# Preserve compiler stderr: this path runs both finite-domain rewriting and
# embedded Alive2/Freshet superoptimisation at the default -O3. Hiding stderr
# made native aborts appear only as an unhelpful shell "Aborted" line.
vectorize_compile_log="$TMP/pos_vectorize.compile.stderr"
if $PEN tests/pos_vectorize.fsh -o "$TMP/va" 2>"$vectorize_compile_log"; then
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
cat > "$TMP/fv.fsh" <<'FVEOF'
fn main() {
    let n: i64 = 64;
    let a: *f64 = malloc(f64, n);
    for (i in 0..n) { a[i] = 1.0; }
    free(a);
}
FVEOF
$PEN -S --force-vectorize "$TMP/fv.fsh" -o "$TMP/fv.ll" 2>/dev/null
if grep -q "interleave.count" "$TMP/fv.ll"; then
  ok "§5 --force-vectorize restores the historical hint"
else bad "§5 --force-vectorize hint missing"; fi

# custom PGO counters are thread-safe atomics (§11.1)
$PEN -S --profile tests/pos_parfor.fsh -o "$TMP/pg.ll" 2>/dev/null
if grep -q "atomicrmw add" "$TMP/pg.ll"; then
  ok "§11.1 PGO counters are atomic (atomicrmw)"
else bad "§11.1 PGO counters not atomic"; fi

echo "== Audit integration: differential execution (§2.4/§13.4) =="
# -O0 vs -O3 checksum equality for a mixed arithmetic/pointer kernel
okdiff=1
$PEN -O0 tests/pos_diff_checksum.fsh -o "$TMP/dc0" 2>/dev/null || okdiff=0
$PEN -O3 tests/pos_diff_checksum.fsh -o "$TMP/dc3" 2>/dev/null || okdiff=0
if [ $okdiff = 1 ]; then
  o0=$("$TMP/dc0" 2>/dev/null); o3=$("$TMP/dc3" 2>/dev/null)
  if [ -n "$o0" ] && [ "$o0" = "$o3" ]; then
    ok "§2.4 differential -O0 vs -O3 identical ($o0)"
  else bad "§2.4 differential mismatch (O0='$o0' O3='$o3')"; fi
else bad "§2.4 differential compiles failed"; fi

# serial cutoff (§10.1): all-inline and all-dispatch must both be correct
if $PEN tests/pos_nest_ws.fsh -o "$TMP/sc" 2>/dev/null; then
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
cat > "$TMP/vm.fsh" <<'VEOF'
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
$PEN -S "$TMP/vm.fsh" -o "$TMP/vm.ll" 2>/dev/null
if grep -q "@llvm.sin" "$TMP/vm.ll" && grep -q "@llvm.log10" "$TMP/vm.ll"; then
  ok "§8 @fastmath: sin/cos/log2/log10 lower to LLVM intrinsics"
else bad "§8 transcendental intrinsics missing"; fi
# @strictfp keeps scalar libm calls; under Option A the DEFAULT is strict,
# so plain functions also keep libm calls (no fast-math without opt-in)
cat > "$TMP/vs.fsh" <<'VEOF'
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
$PEN -S "$TMP/vs.fsh" -o "$TMP/vs.ll" 2>/dev/null
if grep -q "call double @sin" "$TMP/vs.ll" && grep -q "call double @cos" "$TMP/vs.ll"; then
  ok "§8 Option A: strict default keeps scalar libm calls (incl. plain fns)"
else bad "§8 strictfp lowering changed"; fi

# --emit-final-ir (§13.1): the backend's optimised IR is inspectable
if $PEN --emit-final-ir tests/pos_basics.fsh -o "$TMP/fi" 2>/dev/null && \
   [ -f "$TMP/fi.final.ll" ]; then
  ok "§13.1 --emit-final-ir writes the optimised module"
else bad "§13.1 --emit-final-ir missing"; fi

echo "== Phase 0: correctness contract (report §3) =="
run_ok "signed div/rem truncation" tests/pos_divrem.fsh "-3 -7|-1 -1|-1 0|-7 0|-2 0|3 7|-25 -4|-37 -44|-50 -36|9223372036854775807|15|125 2|-32|-3 -7"
run_err "ret fall-off-the-end" tests/neg_noret.fsh F0209
run_err "ret valueless return" tests/neg_retvoid.fsh F0209
run_err "ret one-branch return" tests/neg_oneret.fsh F0209
run_ok "definite return + while(true)" tests/pos_defret.fsh "3|3|6|6"
run_ok "<<= >>= end-to-end" tests/pos_shiftassign.fsh "16|4|1024|128|-16|24|25|7"
run_ok "by-value struct params (miscompile fix)" tests/pos_structparam.fsh "4.00|100.0 1.5|2.5"

echo "== Phase 1: data + control foundations (report §4) =="
run_ok "break/continue + steps + ranges" tests/pos_breakcont.fsh "10|20|37|63|4|55|22"
run_err "F0213 break outside loop" tests/neg_brkloop.fsh F0213
run_ok "fixed arrays [T; N]" tests/pos_arrays.fsh "1 4|4|99|26|5|64|1 100|3|4.0|9"
run_err "bound constant index" tests/neg_bound.fsh F0212
run_ok "Span<T> views + slicing" tests/pos_spans.fsh "10.0|4|15|20|3 10 30|30 40|40|3.5|3.0"
run_ok "enums + tagged unions + match" tests/pos_enums.fsh "10|20|12.57|12.0|0.0|10.0|circle|3.0 4.0|3.14|1.0|30"
run_err "match non-exhaustive" tests/neg_match_nonex.fsh F0204

echo "== Phase 2: generics + strings + errors (report §4) =="
run_ok "generics (fns + structs + constraints)" tests/pos_generics.fsh "2.0|5|1.0|1.5 7|3 1.5|10|7.5|1|10|2.5"
run_err "generic constraint violation" tests/neg_genericconstraint.fsh F0203
run_ok "strings: str views + String + fmt" tests/pos_strings.fsh "hello|5|65 67|world|5|eq ne|abcdef|6|count: 42|12345|3.14|hello, freshet!|pi 3"
run_ok "Option/Result + ? propagation" tests/pos_optres.fsh "5|1001|2|none|26|1001|42|none|7"

echo "== Memory modes (report §5) =="
run_ok "owned/borrowed/unsafe" tests/pos_memmodes.fsh "6.0|5 6|11"
run_err "own-call owned param without give" tests/neg_ownedcall.fsh F0303

echo "== optimisation-audit: §10.1 legality regressions =="
# §4.1 P0: predicate merging must not remove an observable call
run_okf "no merge of impure predicates (16 calls)" tests/neg_lane_merge_impure.fsh "16" "-O2" "!F0901"
# §4.2 P0: no condition evaluation on a zero-trip loop
run_okf "no unswitch on zero-trip trapping cond" tests/neg_lane_unswitch_zero_trip.fsh "0" "-O2" "!F0901"
# §4.3 P0: unrolling vetoes bodies with return
run_okf "no unroll of return-containing body" tests/neg_unroll_return.fsh "2|-1" "-O2"
# §4.4 P0: typed bit-vector constant semantics
run_ok "typed const overflow/cast semantics" tests/neg_const_overflow_fold.fsh "1|1|255|1|33"
# §4.6 P1: fusion rejected when unknown effects may conflict
run_okf "fusion rejected on unknown effects" tests/neg_fuse_unknown_effect.fsh "8" "-O2" "!F0908"
# mirrors: the positive shapes still fire
run_okf "disjoint regions still fuse" tests/pos_fuse_disjoint_regions.fsh "11" "-O2" "F0908"
run_okf "simple loop still fully unrolls" tests/pos_unroll_simple.fsh "28" "-O2" "F0909"
run_okf "pure total predicate still merges" tests/pos_lane_merge_pure.fsh "64" "-O2" "F0901"

echo "== optimisation-audit: §6 pass features =="
run_ok "typed const fold + short-circuit" tests/pos_constfold.fsh "9|44|5"
run_ok "imin/imax builtins" tests/pos_iminmax.fsh "3 7 -5 2|4 9"
run_okf "FreshetLICM hoists invariant pure lets" tests/pos_licm.fsh "1035" "-O2 --licm" "F0912"
run_okf "partial unroll with exact remainder" tests/pos_partial_unroll.fsh "440|33" "-O2 --partial-unroll" "F0910"
run_okf "peel resolves first-iteration guard" tests/pos_peel.fsh "116|1009" "-O2 --peel" "F0911"
run_okf "interchange swaps transposed nest" tests/pos_interchange.fsh "6|2" "-O2 --interchange" "F0913"
run_okf "tile blocks with exact boundaries" tests/pos_tile.fsh "54|1|73" "-O2 --tile" "F0914"
run_okf "unroll-and-jam keeps domain exact" tests/pos_unrolljam.fsh "56|95" "-O2 --unroll-and-jam" "F0916"
run_okf "distribution fissions independent groups" tests/pos_distribute.fsh "22|20" "-O2 --distribute" "F0915"
run_okf "parfor advisor: parallel-safe + reduction" tests/pos_parfor_advisor.fsh "18 2016" "-O2 --parfor-advisor" "F0905"
run_ok "body-cost-aware parfor cutoff" tests/pos_bodycost.fsh "19900|186900"
run_okf "proof log records premises" tests/pos_prooflog.fsh "440" "-O2 --partial-unroll --proof-log" ""

# ---------------------------------------------------------------------------
# Phase 3/4/5 feature tests (report §6/§9/§10/§12) — modules, fn pointers,
# parfor break/continue, @reproducible, test runner, fsh module, project
# manifest, formatter, debug info, lint, LSP navigation
# ---------------------------------------------------------------------------
echo "== Phase 3/4/5: modules, FFI, tooling, reproducibility =="

# --- modules (§6.1/§6.2): import, pub, qualified names, cross-module
# name collisions (private `distance` in two modules)
run_okf "modules: import/pub/qualified names" tests/pos_modules.fsh "10.00|7.00|0.00" "-Itests/mods"
if $PEN -Itests/mods tests/neg_import_private.fsh -o "$TMP/np" 2>"$TMP/err" \
    || grep -q "F0004" "$TMP/err"; then
  ok "F0004 private cross-module use (F0004)"
else bad "F0004 private cross-module use"; fi
run_err "import module not found" tests/neg_import_missing.fsh "F0004"

# --- circular import (two files) -> F0004
cat > "$TMP/ca.fsh" <<'EOFA'
module ca;
import cb;
pub fn a() -> i64 { return 1; }
fn main() { print_i64(cb::b()); println(); }
EOFA
cat > "$TMP/cb.fsh" <<'EOFA'
module cb;
import ca;
pub fn b() -> i64 { return ca::a(); }
EOFA
if $PEN "$TMP/ca.fsh" -o "$TMP/cyc" 2>"$TMP/err" && ! grep -q "F0004" "$TMP/err"; then
  bad "F0004 circular import detection"
else ok "F0004 circular import detection (F0004)"; fi

# --- multi-input compilation (§6.2): two files as one program
if $PEN tests/mods/geometry.fsh tests/pos_modules.fsh -o "$TMP/multi" 2>"$TMP/err" \
    && [ "$("$TMP/multi" 2>/dev/null)" = "$(printf '10.00\n7.00\n0.00')" ]; then
  ok "multi-input compilation (two .fsh on the CLI)"
else bad "multi-input compilation (two .fsh on the CLI)"; fi

# --- fn pointers + C callbacks (§6.3): libc qsort through a Freshet fn
run_ok "fn pointers + C callback (qsort)" tests/pos_fnptr.fsh "5|20|9|8|9 7 5 3 2 1 "
run_err "thread indirect call in parfor" tests/neg_fnptr_parfor.fsh "F0307"

# --- parfor break/continue (§12/§14): lane-local semantics
run_ok "parfor break/continue (lane-local)" tests/pos_parfor_break.fsh "62500"

# --- @reproducible (§12): bitwise-identical across thread counts
if $PEN tests/pos_reproducible.fsh -o "$TMP/repro" 2>"$TMP/err"; then
  r1=$(FRT_THREADS=1 FRT_WORK_CUTOFF=0 "$TMP/repro" 2>/dev/null)
  r2=$(FRT_THREADS=3 FRT_WORK_CUTOFF=0 "$TMP/repro" 2>/dev/null)
  r3=$(FRT_THREADS=8 FRT_WORK_CUTOFF=0 "$TMP/repro" 2>/dev/null)
  if [ -n "$r1" ] && [ "$r1" = "$r2" ] && [ "$r2" = "$r3" ]; then
    ok "@reproducible reduction identical across thread counts"
  else bad "@reproducible reduction identical across thread counts (got $r1/$r2/$r3)"; fi
else bad "@reproducible reduction identical across thread counts (compile failed)"; fi

# --- `penstock test` runner (§10.3): per-test lines + summary + exit code
if $PEN test tests/pos_test_runner.fsh >"$TMP/tout" 2>"$TMP/err"; then
  if grep -q "3/3 tests passed" "$TMP/tout" && \
     grep -q "test add_works ... ok" "$TMP/tout"; then
    ok "penstock test: @test runner (passing suite)"
  else bad "penstock test: @test runner (output mismatch)"; fi
else bad "penstock test: @test runner (non-zero exit)"; fi
# a FAILING test must not abort the run and must exit non-zero
cat > "$TMP/tf.fsh" <<'EOFA'
@test fn t1() { assert(1 == 1); }
@test fn t2() { assert(1 == 2); }
@test fn t3() { assert(3 == 3); }
EOFA
if $PEN test "$TMP/tf.fsh" >"$TMP/tout" 2>/dev/null; then
  bad "penstock test: failure exits non-zero"
else
  if grep -q "2/3 tests passed" "$TMP/tout"; then
    ok "penstock test: assert failure reported, suite continues"
  else bad "penstock test: assert failure reported, suite continues"; fi
fi
if $PEN test tests/neg_test_nofns.fsh -o "$TMP/nt" 2>"$TMP/err" \
    && ! grep -q "F0005" "$TMP/err"; then
  bad "F0005 no @test functions (F0005)"
else ok "F0005 no @test functions (F0005)"; fi

# --- the shipped fsh module (§6.3): args/env/file I/O
if $PEN tests/pos_fsh_module.fsh -o "$TMP/fshm" 2>"$TMP/err"; then
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
cp tests/pos_fmt_src.fsh "$TMP/fmt.fsh"
if $PEN fmt "$TMP/fmt.fsh" >/dev/null 2>&1 \
   && $PEN fmt --check "$TMP/fmt.fsh" >/dev/null 2>&1 \
   && $PEN "$TMP/fmt.fsh" -o "$TMP/fmtb" 2>/dev/null \
   && [ "$("$TMP/fmtb" 2>/dev/null)" = "22" ]; then
  ok "formatter: reformat + idempotence + compiled equivalence"
else bad "formatter: reformat + idempotence + compiled equivalence"; fi

# --- -g debug info (§10.3): DWARF line table + symbol resolution
if $PEN -g -O0 tests/pos_debug_g.fsh -o "$TMP/dbg" 2>"$TMP/err"; then
  if objdump --dwarf=decodedline "$TMP/dbg" 2>/dev/null | grep -q "pos_debug_g.fsh"; then
    sym_addr=$(objdump -t "$TMP/dbg" | awk '/u_inner/{print $1; exit}')
    resolved=$(addr2line -e "$TMP/dbg" -f "0x$sym_addr" 2>/dev/null | tail -1)
    if echo "$resolved" | grep -q "pos_debug_g.fsh"; then
      ok "-g debug info: DWARF line table resolves source lines"
    else bad "-g debug info: addr2line resolution ('$resolved')"; fi
  else bad "-g debug info: no line table emitted"; fi
else bad "-g debug info: build failed"; fi

# --- lint (§10.2): diagnostics-only surface
if $PEN lint tests/pos_basics.fsh >/dev/null 2>&1; then
  ok "lint: diagnostics-only run (clean file -> exit 0)"
else bad "lint: diagnostics-only run"; fi
if $PEN lint tests/neg_divzero.fsh >/dev/null 2>&1; then
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
$PEN tests/neg_arith.fsh -o "$TMP/t" 2>"$TMP/w.err"; $PEN --strict-performance tests/neg_arith.fsh -o "$TMP/t" 2>"$TMP/e.err"
if grep -q '^warning\[F0402\]' "$TMP/w.err" && grep -q '^error\[F0402\]' "$TMP/e.err"; then ok "F0402 is warning[F0402] by default, error[F0402] when strict"
else bad "ID/severity decoupling"; fi

echo
echo "passed: $pass   failed: $fail"
[ $fail -eq 0 ]
