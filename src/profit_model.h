// Freshet — shared profitability model for every source-level transform
// (optimisation-audit §8).
//
// DESIGN RULE (audit §8.2): every pass separates
//   1. LEGALITY    — a proof/conservative argument that semantics are
//                    preserved (effect summaries, dependence checks,
//                    typed constant evaluation);
//   2. PROFITABILITY — an estimate that the transform improves the
//                    user-selected objective.
// This file owns ONLY (2).  A legal transform may be unprofitable; an
// attractive benchmark can never make an illegal transform acceptable.
//
// The model is deliberately a documented approximation: Penstock drives
// an external Clang/LLVM, so TargetTransformInfo is not linkable here.
// The feature weights below are the static estimates the audit's §8.1
// table asks for (trip counts, ops/iter, bytes/iter, stride/contiguity,
// code size), with target geometry (cache line, L1/L2, vector width)
// overridable on the command line and refinable by PGO later.
#pragma once
#include "common.h"
#include "opt_util.h"
#include <string>

namespace freshet {
namespace profit {

// ---- target geometry (CLI-overridable; defaults = commodity x86-64) ----
struct TargetProfile {
  int cacheLine = 64;     // bytes per cache line
  int l1Bytes = 32768;    // L1d size
  int l2Bytes = 262144;   // L2 size
  int vectorWidth = 8;    // elements of i64/f64 per vector register
};
TargetProfile& target();          // reads g_opts overrides lazily

// ---- per-loop cost features (audit §8.1 table) ----
struct BodyCost {
  int ops = 0;             // static arithmetic/bitwise ops per iteration
  int memOps = 0;          // loads + stores per iteration
  int calls = 0;           // calls per iteration
  int branches = 0;        // IF statements per iteration
  int nodes = 0;           // AST size (code-size proxy)
  int64_t bytesPerIter = 0;// estimated memory bytes touched per iteration
  int laneWork = 0;        // vectorisable work (from BodyInfo)

  int64_t latencyScore() const {
    // rough per-iteration latency estimate: cheap int ops 1, memory 4,
    // calls 16 (call overhead dominates)
    return (int64_t)ops + 4 * (int64_t)memOps + 16 * (int64_t)calls;
  }
};
BodyCost estimateBody(const optutil::BodyInfo& info);

// ---- shared decision helpers used by the loop transforms ----
// partial-unroll factor from body weight and trip count (2/4/8, or 0 =
// not worth it): bounded code growth (nodes*F <= 200), never unroll a
// trip <= 16 (the full unroller owns that range)
int chooseUnrollFactor(const BodyCost& c, int64_t trip);

// tile edge length for a 2-D nest touching `distinctStreams` arrays with
// `bytesPerInnerIter` bytes: pick the largest power-of-two edge whose
// working set (tile_i * tile_j * distinctStreams * elemBytes) still fits
// half of L1; falls back to (vectorWidth, cacheLine/elemBytes)
void chooseTileEdges(int64_t distinctStreams, int64_t elemBytes,
                     int* ti, int* tj);

// human-readable cost line for pass remarks (§10.4: static estimates are
// labelled [profit], never presented as measurements)
std::string describe(const BodyCost& c);

} // namespace profit
} // namespace freshet
