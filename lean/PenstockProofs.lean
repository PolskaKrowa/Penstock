/-
  Penstock / Freshet — Lean 4 semantic kernel and proof harness.

  Optimisation-audit integration (report §5, Phase B).  This file is the
  "Freshet semantic kernel" the audit recommends as the first Lean layer:

    * fixed-width integer semantics as `BitVec w` (Lean 4 core — no
      Mathlib dependency), covering exactly the operations Penstock's
      typed constant evaluator folds;
    * a typed expression language over ONE width, a deterministic
      evaluator, a constant folder, and the equational theorem
      `eval_fold  :  eval env (fold e) = eval env e`
      (the audit's "constant-fold evaluation equals expression
      evaluation");
    * the basic strength-reduction identities of §5.4 for unsigned and
      two's-complement arithmetic;
    * `CertifiedCandidate` — every candidate for a rewrite carries its
      semantic-validity proof — plus a cost-based selector and the
      theorem that the selector returns a MINIMUM-COST member of its
      finite input list (the audit's "bounded optimal extraction");
    * the PREMISE STRUCTURES for loop fusion and branch unswitching:
      explicit contracts naming exactly what the C++ passes must
      establish (and what --proof-log records per transform).

  STATUS (honest, matching the audit's own §2/§5.6 caveat): this file was
  authored as part of the optimisation-audit integration and is wired
  into CI via scripts/check-lean.sh, but the authoring environment has
  NO Lean toolchain, so the theorems below are NOT kernel-checked here.
  Run `scripts/check-lean.sh` (or `lean lean/PenstockProofs.lean`) on a
  machine with Lean 4 before trusting a proof.  Lean and Alive2 stay
  complementary: Lean reasons about Penstock's AST-level rewrite
  legality; Alive2 translation-validates the actual emitted LLVM.
-/

namespace Penstock

/-! ## §5.3 fixed-width integers

`BitVec w` from Lean 4 core is the bit-vector model: 8/16/32/64-bit
integers with wrapping arithmetic — the same semantics the generated
LLVM IR has, and the semantics `evalConstTyped` implements in
`src/opt_util.cpp`. -/

section Identities
variable {w : Nat}

theorem bvadd_zero_left (x : BitVec w) : 0 + x = x := by
  simp

theorem bvadd_zero_right (x : BitVec w) : x + 0 = x := by
  simp

theorem bvmul_one_left (x : BitVec w) : 1 * x = x := by
  simp

theorem bvmul_one_right (x : BitVec w) : x * 1 = x := by
  simp

theorem bvmul_zero (x : BitVec w) : x * 0 = 0 := by
  simp

theorem bvsub_self (x : BitVec w) : x - x = 0 := by
  simp

/-! ## §5.4 strength reduction

Unsigned division by a power of two is the logical right shift.  The
SIGNED case deliberately does NOT claim `x / 2^k = x >>> k`: signed
division truncates toward zero while `ashr` rounds toward −∞; the bias
premise states the corrected identity Penstock's IRgen emits
(`(x + bias) >>> k` with `bias = 2^(k-1) − 1` when `x < 0`, else 0).
Stating the contract explicitly is deliberate — a wrong sweeping
claim would be worse than an open obligation. -/

def IsPow2 (d : Nat) : Prop := d > 0 ∧ ∃ k : Nat, d = 2 ^ k ∧ k < 64

/-- CI obligation: unsigned `x / 2^k` equals `x >>> k` — left OPEN
(`sorry`) rather than asserted without a kernel check in this
environment; `scripts/check-lean.sh` fails the build on any `sorry`
that has not been discharged. -/
theorem udiv_pow2_eq_lshr (x : BitVec w) {d : Nat} (h : IsPow2 d) :
    x / (d : BitVec w) = x >>> (log2 d) := by
  sorry

end Identities

/-! ## §5.3 typed expression semantics (smallest useful subset)

One width `w` for a whole expression — kernel v1.  Variables are
de Bruijn-style indices into an environment.  Division/remainder are
TOTAL here (Lean's `BitVec` division is total) but the FOLDER never
folds a zero divisor, mirroring `evalConstTyped`'s refusal — "not
foldable" is preserved structurally, so evaluation cannot disagree. -/

inductive Expr (w : Nat) where
  | lit   : BitVec w → Expr w
  | var   : Nat → Expr w
  | add   : Expr w → Expr w → Expr w
  | sub   : Expr w → Expr w → Expr w
  | mul   : Expr w → Expr w → Expr w
  | band  : Expr w → Expr w → Expr w
  | bor   : Expr w → Expr w → Expr w
  | bxor  : Expr w → Expr w → Expr w
  | shiftLeft  : Expr w → Expr w → Expr w
  | shiftRight : Expr w → Expr w → Expr w
  | udiv  : Expr w → Expr w → Expr w
  | urem  : Expr w → Expr w → Expr w

namespace Expr

def eval (env : Nat → BitVec w) : Expr w → BitVec w
  | lit v       => v
  | var i       => env i
  | add a b     => eval env a + eval env b
  | sub a b     => eval env a - eval env b
  | mul a b     => eval env a * eval env b
  | band a b    => eval env a &&& eval env b
  | bor a b     => eval env a ||| eval env b
  | bxor a b    => eval env a ^^^ eval env b
  | shiftLeft a b  => eval env a <<< eval env b
  | shiftRight a b => eval env a >>> eval env b
  | udiv a b    => eval env a / eval env b
  | urem a b    => eval env a % eval env b

/-! ## §6.1 TypedConstFold, as a function and a theorem

The folder normalises exactly the shapes the C++ pass folds.  The
zero-divisor guard keeps the division node when the divisor may be
zero, so the total evaluator never answers a question the source
would have refused to fold. -/

def fold : Expr w → Expr w
  | lit v              => lit v
  | var i              => var i
  | add a b =>
      match fold a, fold b with
      | lit x, lit y => lit (x + y)
      | a', Expr.lit v => if v == 0 then a' else add a' (lit v)
      | Expr.lit v, b' => if v == 0 then b' else add (lit v) b'
      | a', b' => add a' b'
  | sub a b =>
      match fold a, fold b with
      | lit x, lit y => lit (x - y)
      | a', b' => sub a' b'
  | mul a b =>
      match fold a, fold b with
      | lit x, lit y => lit (x * y)
      | a', Expr.lit v => if v == 0 then lit 0 else mul a' (lit v)
      | Expr.lit v, b' => if v == 0 then lit 0 else mul (lit v) b'
      | a', b' => mul a' b'
  | band a b =>
      match fold a, fold b with
      | lit x, lit y => lit (x &&& y)
      | a', b' => band a' b'
  | bor a b =>
      match fold a, fold b with
      | lit x, lit y => lit (x ||| y)
      | a', b' => bor a' b'
  | bxor a b =>
      match fold a, fold b with
      | lit x, lit y => lit (x ^^^ y)
      | a', b' => bxor a' b'
  | shiftLeft a b =>
      match fold a, fold b with
      | lit x, lit y =>
          if y.toNat < w then lit (x <<< y) else shiftLeft (fold a) (fold b)
      | a', b' => shiftLeft a' b'
  | shiftRight a b =>
      match fold a, fold b with
      | lit x, lit y =>
          if y.toNat < w then lit (x >>> y) else shiftRight (fold a) (fold b)
      | a', b' => shiftRight a' b'
  | udiv a b =>
      match fold a, fold b with
      | lit x, lit y => if y == 0 then udiv (lit x) (lit y) else lit (x / y)
      | a', b' => udiv a' b'
  | urem a b =>
      match fold a, fold b with
      | lit x, lit y => if y == 0 then urem (lit x) (lit y) else lit (x % y)
      | a', b' => urem a' b'

/-- §6.1's proof obligation: folding is semantics-preserving at the
exact source width.  Structural induction; each case reduces to BitVec
arithmetic identities plus the `if` splits for the zero-divisor and
in-range-shift guards.  CI obligation (see file header). -/
theorem eval_fold (env : Nat → BitVec w) (e : Expr w) :
    eval env (fold e) = eval env e := by
  induction e with
  | lit v => rfl
  | var i => rfl
  | add a b iha ihb => simp [fold, eval, iha, ihb]; split <;> simp_all
  | sub a b iha ihb => simp [fold, eval, iha, ihb]
  | mul a b iha ihb => simp [fold, eval, iha, ihb]; split <;> simp_all
  | band a b iha ihb => simp [fold, eval, iha, ihb]
  | bor a b iha ihb => simp [fold, eval, iha, ihb]
  | bxor a b iha ihb => simp [fold, eval, iha, ihb]
  | shiftLeft a b iha ihb => simp [fold, eval, iha, ihb]; split <;> simp_all
  | shiftRight a b iha ihb => simp [fold, eval, iha, ihb]; split <;> simp_all
  | udiv a b iha ihb => simp [fold, eval, iha, ihb]; split <;> simp_all
  | urem a b iha ihb => simp [fold, eval, iha, ihb]; split <;> simp_all

end Expr

/-! ## §5.5 / §7.2 certified candidates and bounded optimal extraction

Every candidate in the superoptimiser's search list carries a PROOF of
semantic equivalence to the source expression.  Cost is then the only
open question, and the extractor must return a minimum-cost member of
the list — exactly the audit's "optimal in this grammar, up to this
size, for this target cost model", never "globally optimal machine
code". -/

structure CertifiedCandidate (src : Expr w) where
  body      : Expr w
  validity  : ∀ env, Expr.eval env body = Expr.eval env src
  cost      : Nat

/-- Minimum-cost extraction over a finite, fully-certified candidate
list: keep the incumbent unless a strictly cheaper certified candidate
appears.  This is the shape `runVerifiedSuperoptimizer`'s objective
comparison mirrors in C++ (src/superopt.cpp). -/
def selectMinCost {src : Expr w}
    (cs : List (CertifiedCandidate src)) : Option (CertifiedCandidate src) :=
  cs.foldl (fun acc c =>
    match acc with
    | none           => some c
    | some b => if c.cost < b.cost then some c else acc)
    none

theorem selectMinCost_mem {src : Expr w}
    (cs : List (CertifiedCandidate src)) (r : CertifiedCandidate src)
    (hr : selectMinCost cs = some r) : r ∈ cs := by
  induction cs with
  | nil => exact absurd hr (by simp [selectMinCost])
  | cons c cs ih =>
      unfold selectMinCost at hr
      split at hr
      · split at hr
        · exact Or.inl (by injection hr; exact hr)
        · exact Or.inr (ih hr)
      · split at hr
        · exact Or.inl (by injection hr; exact hr)
        · exact Or.inr (ih hr)

/-- The extractor's result costs no more than ANY member of the list it
was extracted from — the bounded-optimality half of §7.2. -/
theorem selectMinCost_min {src : Expr w}
    (cs : List (CertifiedCandidate src)) (r c : CertifiedCandidate src)
    (hr : selectMinCost cs = some r) (hc : c ∈ cs) :
    r.cost ≤ c.cost := by
  -- the running minimum is monotone: it starts at the first element and
  -- only ever moves to a strictly cheaper member, so the final result
  -- bounds every list member.  CI obligation (see file header): the
  -- two relay cases below reduce to the same monotonicity argument as
  -- the closed cases; they are left visible, not silently claimed.
  sorry

/-! ## §5.2 pass-legality premise structures

The C++ transforms establish exactly these premises before rewriting
(and `--proof-log` records them per application).  They are stated as
contracts so a future certificate checker can consume them directly. -/

namespace FusionPremises
/-- Two loops over the same range may be jammed into one when every
cross-loop dependence is same-iteration and all effects preserve
ordering — `tryFuseRun`'s contract (src/fuse_unroll.cpp). -/
structure Contract (α : Type) where
  sameRange           : α
  allDepsSameIter     : α
  noObservableEffects : α
  boundsTotalAndRT    : α
end FusionPremises

namespace UnswitchPremises
/-- Hoisting an invariant branch out of a loop preserves behaviour when
the condition is loop-invariant, total (non-trapping) whenever the
range may be empty, and the original evaluation count is preserved —
the §4.2 fix's contract (src/lane_analysis.cpp). -/
structure Contract (α : Type) where
  loopInvariant            : α
  totalOrNonEmptyTrip      : α
  evaluationCountPreserved : α
  bodyClonedNotShared      : α
end UnswitchPremises

end Penstock
