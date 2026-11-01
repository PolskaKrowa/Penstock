// Freshet — typed constant folding (audit §6.1) + range/induction
// analysis (§6.3) + automatic-parfor advisor (§6.10).
//
// Everything here runs POST-sema (types resolved, effects computed) and
// PRE-irgen, like the other source-level passes.  The const folder only
// ever replaces an expression with the EXACT typed value computed by
// optutil::evalConstTyped — the same bit-vector semantics the IR has.
#pragma once
#include "common.h"
#include "opt_util.h"

namespace freshet {

// §6.1 TypedConstFold: fold integer/boolean constant expressions in
// place (INT/BOOL literals keep their node type); short-circuit `&&`/`||`
// fold without evaluating the unreachable operand.  Also performs the
// modest immutable-binding propagation §6.2 asks for (non-mut `let`s
// whose unique-name binding holds a constant are substituted at uses) —
// deliberately NOT a full SCCP/GVN: LLVM owns that after IRgen.
int runTypedConstFoldPass(Program* p);

// §6.3 range/induction analysis ------------------------------------------------

// affine form `coef*iv + off` over ONE induction variable (a superset of
// optutil::affineIvarLiteral, which only recognised bare iv or iv*C)
struct Affine {
  bool valid = false;
  int64_t coef = 0;    // nonzero when the expression varies with iv
  int64_t off = 0;
};
Affine affineIn(Expr* e, const std::string& iv);

// closed interval for a scalar integer expression; `known` only when the
// whole expression is modelled exactly (saturated arithmetic => unknown)
struct Interval {
  bool known = false;
  int64_t lo = 0, hi = 0;
  bool contains(int64_t v) const { return known && v >= lo && v <= hi; }
};
Interval intervalOf(Expr* e);

// shared dependence predicates for the reordering transforms
// (interchange / tiling / distribution / unroll-and-jam):
//   iterationsIndependent — no root is both read and written by the body
//   and nothing touches unknown memory: with injective per-iteration
//   index functions, iterations may run in ANY order.
bool iterationsIndependent(const optutil::BodyInfo& b);
// reordering iterations is unobservable: no print/return/break/parfor/
// give anywhere in the body
bool noObservableEffects(const optutil::BodyInfo& b);

// §6.10 AutoParforAdvisor: analysis-only.  Emits F0905 notes for
// canonical `for` loops whose iterations are provably independent (and a
// separate reduction-candidate note for accumulate-into-scalar shapes).
// Never rewrites anything.
int runParforAdvisorPass(Program* p);

} // namespace freshet
