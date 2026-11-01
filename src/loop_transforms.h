// Freshet — Phase D source-level loop transforms (optimisation-audit
// §6.4–§6.9, §11-D):
//
//   FreshetLICM     --licm               hoist invariant pure lets
//   PartialUnroll   --partial-unroll     constant-trip, factor 2/4/8,
//                                        remainder loop preserved
//   LoopPeel        --peel               first-iteration guard removal
//   LoopInterchange --interchange        perfect rectangular nests
//   LoopTile        --tile[=I[,J]]       2-D blocking with exact
//                                        imin-clamped boundary tiles
//   LoopDistribution --distribute        2-way fission of independent
//                                        statement groups
//   UnrollAndJam    --unroll-and-jam     outer x2 + jammed inner bodies
//
// ALL of these are OPT-IN (audit §11-D) and run only at -O2+, not under
// --profile, and not with --no-loop-opts.  Every pass:
//   * proves LEGALITY first (effect summaries + dependence checks +
//     typed constant evaluation + totality for evaluation-count changes);
//   * consults the shared profitability model for the decision
//     parameters (audit §8: legality and profitability are separate);
//   * emits an N-* note saying what ran, and a --proof-log line listing
//     the premises it relied on (audit §5.2/§10.4);
//   * reuses or clones existing sema'd nodes — the only new bindings are
//     fresh, collision-checked tile induction variables, appended to the
//     function's symbol table so IRgen materialises their allocas.
#pragma once
#include "common.h"

namespace freshet {

struct LoopXformCounts {
  int licm = 0, partialUnroll = 0, peel = 0, interchange = 0;
  int tile = 0, distribute = 0, unrollJam = 0;
  int total() const {
    return licm + partialUnroll + peel + interchange + tile + distribute +
           unrollJam;
  }
};

LoopXformCounts runLoopTransformPasses(Program* p);

} // namespace freshet
