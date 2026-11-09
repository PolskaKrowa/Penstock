// Freshet — R4 source-level loop optimisation pass 2: cross-lane
// analysis for the vectoriser.
//
// Penstock marks every simple `for` loop with llvm.loop.vectorize.enable;
// LLVM's vectoriser turns iterations into SIMD lanes.  Two patterns
// waste vector throughput, and both are fixable at the source level
// where the compiler still knows what the user meant:
//
// 1. UNIFORM BRANCHES.  A branch whose condition does not depend on
//    the induction variable (and on nothing the loop writes) has the
//    SAME value in every lane, every iteration.  Inside the loop it
//    forces either masked execution (both paths computed for every
//    lane) or scalarisation.  The transform hoists the branch out and
//    specialises the loop per path — the condition is computed ONCE on
//    a scalar ("one lane reserved for the decision") and every lane of
//    the specialised loop does truly independent, branch-free work.
//
//        for (i in 0..n) { pre; if (flag) {A} else {Y}; post }
//   ->   if (flag) { for (i in 0..n) { pre; A; post } }
//        else     { for (i in 0..n) { pre; Y; post } }
//
//    Cost model (explicitly requested): the loop must carry enough
//    vectorisable work (laneWork >= 6 ops, >= 10 when an else arm
//    doubles the body), the specialised bodies must stay small
//    (<= ~600 nodes), and tiny constant trip counts are skipped
//    (clang unrolls those anyway).  Vector-light candidates are
//    reported with F0902 so the gate is visible, never silent.
//
// 2. RECOMPUTED PREDICATES.  Adjacent `if`s with canonically identical
//    conditions make the vectoriser emit the same vector compare +
//    branch twice per iteration.  When neither arm writes anything the
//    condition reads, they merge — the predicate is computed once and
//    REUSED instead of recomputed (throughput freed for useful lanes).
//
//        if (c) {A} ; if (c) {B}      ->      if (c) { A; B }
//
// Both transforms are pure AST restructures (shared statements, no new
// bindings), run post-sema pre-irgen, skipped on --profile builds.
#include "opt_util.h"
#include <algorithm>

namespace freshet {

using namespace optutil;

// ------------------------------------------------------------ helpers
static bool isInt64Kind(Type* t) { return t && t->isInt() && t->bits() == 64; }

static void walkExprNodes(Expr* e, const std::function<void(Expr*)>& f) {
  if (!e) return;
  f(e);
  for (auto* a : e->args) walkExprNodes(a, f);
  walkExprNodes(e->lhs, f);
  walkExprNodes(e->rhs, f);
  walkExprNodes(e->base, f);
}

// may the bounds be re-emitted (evaluated twice, one per specialised
// loop) without changing behaviour?  soa_len / @pure / pure math only.
static bool boundsPure(Expr* e) {
  bool ok = true;
  walkExprNodes(e, [&](Expr* x) {
    if (!ok || x->k != Expr::CALL) return;
    if (x->callee) {
      if (!x->callee->isPure) ok = false;
    } else if (x->name != "soa_len" && !isPureBuiltinCall(x->name)) {
      ok = false;
    }
  });
  return ok;
}

// is the IF condition loop-invariant (same value every iteration)?
static bool condInvariant(Expr* cond, const std::string& ivar,
                          const BodyInfo& info) {
  if (!cond) return false;
  std::set<std::string> names;
  collectIdents(cond, names);
  if (names.count(ivar)) return false;
  // side-effect-free condition: only pure math / @pure user fns
  bool pure = true;
  walkExprNodes(cond, [&](Expr* x) {
    if (!pure || x->k != Expr::CALL) return;
    if (x->callee) {
      if (!x->callee->isPure) pure = false;
    } else if (!isPureBuiltinCall(x->name)) {
      pure = false;
    }
  });
  if (!pure) return false;

  std::set<std::string> wr = writeRoots(info);
  // scalar operands must not be written anywhere in the body
  for (auto& n : names)
    if (wr.count(n)) return false;
  // memory operands (root[i] loads, call-slice reads) likewise
  std::set<std::string> rr;
  exprReadRoots(cond, rr);
  for (auto& r : rr)
    if (wr.count(r)) return false;
  // unknown memory writes anywhere in the body: only literals/pure
  // calls and never-escaped scalars are provably unaffected
  if (info.unknownWrite) {
    if (!rr.empty()) return false;
    for (auto& n : names)
      if (info.escapedNames.count(n)) return false;
  }
  return true;
}

// ---------------------------------------------------- predicate merging
// merge adjacent IFs with identical conditions inside a loop body list
//
// Legality contract (optimisation-audit §4.1, P0):
//   1. the shared condition is REFERENTIALLY TRANSPARENT — evaluating it
//      once instead of twice must not remove an observable call.  The
//      historical code merged `if (next_event()) {...}` pairs, silently
//      halving the event consumption.  RT here means: only memory-free
//      builtins (under the CURRENT FP mode) and pure, will-return user
//      functions; no I/O, allocation, free, unknown call or divergence.
//   2. (kept from the historical check) neither arm of the first IF may
//      write anything the condition reads, and no unknown memory write
//      may happen between the two evaluations — so the value the merged
//      predicate computes once is exactly what the second evaluation
//      would have computed.
static int mergeScan(std::vector<Stmt*>& list, FnDecl* fn) {
  int applied = 0;
  size_t i = 0;
  while (i + 1 < list.size()) {
    Stmt* a = list[i];
    Stmt* b = list[i + 1];
    if (a->k != Stmt::IF || b->k != Stmt::IF) { ++i; continue; }
    if (!a->cond || !b->cond || canon(a->cond) != canon(b->cond)) { ++i; continue; }
    // appending into an else-if chain is awkward; skip those
    if (a->elseB && a->elseB->k == Stmt::IF && b->elseB) { ++i; continue; }

    // §4.1 P0: a merged condition is evaluated ONCE where the source
    // evaluated it TWICE — only a referentially-transparent predicate
    // may be reused.  (canon equality makes checking one side enough,
    // but check both so future canon collisions cannot smuggle an
    // impure second condition past this gate.)
    if (!exprReferentiallyTransparent(a->cond) ||
        !exprReferentiallyTransparent(b->cond)) { ++i; continue; }

    // neither arm of the first IF may write anything the shared
    // condition reads — otherwise the second evaluation could differ
    BodyInfo ia, ib;
    collectBody(a->thenB, "", fn, &ia);
    if (a->elseB) collectBody(a->elseB, "", fn, &ib);
    std::set<std::string> condNames;
    collectIdents(a->cond, condNames);
    std::set<std::string> condRoots;
    exprReadRoots(a->cond, condRoots);
    bool bad = ia.unknownWrite || ib.unknownWrite;
    for (auto& n : condNames)
      if (ia.writes.count(n) || ib.writes.count(n)) bad = true;
    for (auto& r : condRoots)
      if (ia.writes.count(r) || ib.writes.count(r)) bad = true;
    // and the second IF's arms must not have escaped effects between
    // evaluations that the first arms' writes could disturb: order A;B
    // is preserved, only the predicate reuse needs the check above.
    if (bad) { ++i; continue; }

    // merge b into a
    Stmt* newThen = new Stmt(Stmt::BLOCK, a->loc);
    newThen->fn = fn;
    newThen->parent = a;
    if (a->thenB) { a->thenB->parent = newThen; newThen->stmts.push_back(a->thenB); }
    if (b->thenB) { b->thenB->parent = newThen; newThen->stmts.push_back(b->thenB); }
    a->thenB = newThen;
    if (b->elseB) {
      if (!a->elseB) {
        a->elseB = b->elseB;
        a->elseB->parent = a;
      } else {
        Stmt* newElse = new Stmt(Stmt::BLOCK, a->loc);
        newElse->fn = fn;
        newElse->parent = a;
        a->elseB->parent = newElse;
        newElse->stmts.push_back(a->elseB);
        b->elseB->parent = newElse;
        newElse->stmts.push_back(b->elseB);
        a->elseB = newElse;
      }
    }
    list.erase(list.begin() + (i + 1));
    ++applied;
    diag(Diag::Note, Code::Lane,
         "cross-lane: merged 2 adjacent branches with an identical "
         "condition (line " + std::to_string(b->loc.line) + ", fn " +
         fn->name + ") — the per-iteration predicate is computed once "
         "and reused instead of recomputed",
         b->loc,
         "kind=merge,fn=" + fn->name);
    // rescan at i: the merged IF may merge again with i+1
  }
  return applied;
}

// ------------------------------------------------------ branch hoisting
struct UnswitchStats { int applied = 0; int skippedNotes = 0; };

// The original pass only unswitched IFs that were immediate children of a
// loop body.  That misses a common algorithmic shape: a uniform mode flag
// tested in the inner loop of a multi-loop kernel.  Search the whole loop
// nest, but never hoist a predicate that mentions an induction variable of
// any nested loop on its path.  Specialising the *whole outer loop* then
// removes the repeated test from all the inner iterations in one rewrite.
static void findDeepInvariantIfs(Stmt* s, Stmt* outerLoop,
                                 const std::string& outerIvar,
                                 const BodyInfo& info, FnDecl* fn,
                                 std::set<std::string>& nestedIvars,
                                 const std::set<Stmt*>& directChildren,
                                 std::vector<Stmt*>& out) {
  if (!s) return;
  switch (s->k) {
    case Stmt::IF: {
      if (!directChildren.count(s) && s->cond &&
          condInvariant(s->cond, outerIvar, info)) {
        std::set<std::string> names;
        collectIdents(s->cond, names);
        bool mentionsNestedIV = false;
        for (const auto& n : nestedIvars)
          if (names.count(n)) { mentionsNestedIV = true; break; }
        if (!mentionsNestedIV) out.push_back(s);
      }
      findDeepInvariantIfs(s->thenB, outerLoop, outerIvar, info, fn,
                           nestedIvars, directChildren, out);
      findDeepInvariantIfs(s->elseB, outerLoop, outerIvar, info, fn,
                           nestedIvars, directChildren, out);
      break;
    }
    case Stmt::BLOCK:
      for (auto* child : s->stmts)
        findDeepInvariantIfs(child, outerLoop, outerIvar, info, fn,
                             nestedIvars, directChildren, out);
      break;
    case Stmt::FOR: case Stmt::PARFOR: {
      const std::string iv = s->k == Stmt::FOR ? s->ivar : s->pvar;
      bool inserted = !iv.empty() && s != outerLoop;
      if (inserted) nestedIvars.insert(iv);
      findDeepInvariantIfs(s->body, outerLoop, outerIvar, info, fn,
                           nestedIvars, directChildren, out);
      if (inserted) nestedIvars.erase(iv);
      break;
    }
    case Stmt::WHILE:
      findDeepInvariantIfs(s->body, outerLoop, outerIvar, info, fn,
                           nestedIvars, directChildren, out);
      break;
    default: break;
  }
}

static void repairParents(Stmt* s, Stmt* parent, FnDecl* fn) {
  if (!s) return;
  s->parent = parent;
  s->fn = fn;
  for (auto* child : s->stmts) repairParents(child, s, fn);
  repairParents(s->body, s, fn);
  repairParents(s->thenB, s, fn);
  repairParents(s->elseB, s, fn);
}

static bool replaceIfWithArm(Stmt*& s, const SrcLoc& loc,
                             const std::string& condition, bool takeThen) {
  if (!s) return false;
  if (s->k == Stmt::IF && s->loc.file == loc.file &&
      s->loc.line == loc.line && s->loc.col == loc.col &&
      (loc.startOffset == 0 || s->loc.startOffset == loc.startOffset) &&
      s->cond && canon(s->cond) == condition) {
    static const std::map<Sym*, Expr*> noSubst;
    Stmt* arm = takeThen ? s->thenB : s->elseB;
    Stmt* replacement = arm ? cloneStmt(arm, noSubst)
                           : new Stmt(Stmt::BLOCK, s->loc);
    s = replacement;
    return true;
  }
  for (auto*& child : s->stmts)
    if (replaceIfWithArm(child, loc, condition, takeThen)) return true;
  if (replaceIfWithArm(s->body, loc, condition, takeThen)) return true;
  if (replaceIfWithArm(s->thenB, loc, condition, takeThen)) return true;
  if (replaceIfWithArm(s->elseB, loc, condition, takeThen)) return true;
  return false;
}

static bool tryDeepUnswitch(std::vector<Stmt*>& list, size_t idx,
                            Stmt* loop, Stmt* cand, FnDecl* fn,
                            const BodyInfo& info, int* budget,
                            UnswitchStats* st) {
  // Duplicating a nested parallel region can increase scheduling and
  // synchronisation overhead enough to swamp the branch savings.  Keep the
  // broad rewrite to sequential loop nests; PARFOR is still analysed by the
  // other vector/lane passes and by LLVM's own cost model.
  if (info.hasParfor || info.hasReturn || info.hasGive ||
      info.hasBreak) return false;
  // §4.2 P0 (zero-trip): the deep rewrite hoists a condition out of a
  // WHOLE loop nest.  When the outer range may be empty, the source
  // program would never evaluate the condition at any nesting depth —
  // so hoisting is legal only if the condition is provably total
  // (non-trapping) as well as invariant, or the outer trip is provably
  // non-zero.  Same rule as the direct unswitcher above.
  {
    TripInfo ti = tripCountOf(loop);
    if (!(ti.known && !ti.empty) && !exprTotalAndPure(cand->cond))
      return false;
  }
  const int minWork = cand->elseB ? 10 : 6;
  if (info.laneWork < minWork || info.nodes * 2 > 600 || *budget <= 0) {
    if (st->skippedNotes < 2 && info.laneWork >= 1) {
      ++st->skippedNotes;
      diag(Diag::Note, Code::LaneSkip,
           "cross-lane: nested uniform branch (line " +
           std::to_string(cand->loc.line) + ", fn " + fn->name +
           ") not hoisted: lane work " + std::to_string(info.laneWork) +
           " ops, minimum " + std::to_string(minWork) +
           (info.nodes * 2 > 600 ? "; whole-loop clone exceeds size budget" : "") +
           (*budget <= 0 ? "; unswitch budget exhausted" : ""),
           cand->loc,
           "kind=deep-hoist-skip,fn=" + fn->name);
    }
    return false;
  }

  static const std::map<Sym*, Expr*> noSubst;
  const std::string cc = canon(cand->cond);
  Stmt* thenLoop = cloneStmt(loop, noSubst);
  Stmt* elseLoop = cloneStmt(loop, noSubst);
  if (!replaceIfWithArm(thenLoop, cand->loc, cc, true) ||
      !replaceIfWithArm(elseLoop, cand->loc, cc, false))
    return false; // don't commit a partial / ambiguous rewrite

  Stmt* wrapper = new Stmt(Stmt::IF, cand->loc);
  wrapper->cond = cand->cond;
  wrapper->thenB = thenLoop;
  wrapper->elseB = elseLoop; // required even when the source IF had no else
  wrapper->fn = fn;
  wrapper->parent = loop->parent;
  repairParents(wrapper, loop->parent, fn);
  list[idx] = wrapper;
  --*budget;
  ++st->applied;
  diag(Diag::Note, Code::Lane,
       "cross-lane: specialised an outer loop around a nested uniform branch "
       "(line " + std::to_string(cand->loc.line) + ", fn " + fn->name +
       ") — the condition is evaluated once for the loop nest, removing a "
       "per-inner-iteration branch from the vectorisable kernel",
       cand->loc,
       "kind=deep-hoist,fn=" + fn->name + ",lanework=" +
       std::to_string(info.laneWork));
  return true;
}

static void tryUnswitchLoop(std::vector<Stmt*>& list, size_t idx, FnDecl* fn,
                           int* budget, UnswitchStats* st) {
  Stmt* loop = list[idx];
  if (!(loop->k == Stmt::FOR && loop->forRange && !loop->ivar.empty()))
    return;
  Type* ivTy = nullptr;
  if (loop->rangeLo && loop->rangeHi && loop->rangeLo->ty &&
      loop->rangeHi->ty)
    ivTy = commonType(loop->rangeLo->ty, loop->rangeHi->ty);
  if (ivTy && ivTy->k == Type::USIZE) ivTy = Type::simple(Type::U64);
  if (!isInt64Kind(ivTy)) return;
  if (!boundsPure(loop->rangeLo) || !boundsPure(loop->rangeHi)) return;

  // tiny constant trip counts: clang unrolls these, hoisting is noise.
  // §4.4: the distance is computed with __int128-checked arithmetic — the
  // historical `hi - lo` could overflow int64_t before the comparison.
  // A provably-EMPTY constant loop is dead code: never unswitch it (the
  // rewrite would evaluate the condition on a zero-trip loop).
  TripInfo ti = tripCountOf(loop);
  if (ti.known && ti.empty) return;
  if (ti.known && !ti.saturated && ti.trip <= 4) return;
  // §4.2 P0: trip count provably >= 1  ->  the source evaluates the
  // condition at least once, so hoisting it cannot introduce an
  // evaluation that never happened.  Otherwise (dynamic bounds) the
  // condition must additionally be TOTAL (non-trapping) — evaluating
  // `1/d > 0` on a loop that may run zero iterations would abort where
  // the source program could not.  (A saturated distance also proves
  // non-emptiness: the loop runs more than int64 can count.)
  const bool nonEmpty = ti.known && !ti.empty;

  std::vector<Stmt*>* bodyList = stmtsOf(loop->body);
  BodyInfo info;
  collectBody(loop->body, loop->ivar, fn, &info);
  if (info.hasReturn || info.hasGive || info.hasBreak) return;

  // find the first direct-child IF with an invariant condition
  size_t ifPos = (size_t)-1;
  Stmt* cand = nullptr;
  if (bodyList) {
    for (size_t k = 0; k < bodyList->size(); ++k) {
      Stmt* s = (*bodyList)[k];
      if (s->k == Stmt::IF && condInvariant(s->cond, loop->ivar, info)) {
        ifPos = k;
        cand = s;
        break;
      }
    }
  } else if (loop->body && loop->body->k == Stmt::IF &&
             condInvariant(loop->body->cond, loop->ivar, info)) {
    ifPos = 0;
    cand = loop->body;
  }
  if (!cand) {
    std::set<Stmt*> directChildren;
    if (bodyList) {
      for (auto* child : *bodyList) directChildren.insert(child);
    } else if (loop->body) {
      directChildren.insert(loop->body);
    }
    std::vector<Stmt*> deepCandidates;
    std::set<std::string> nestedIvars;
    findDeepInvariantIfs(loop->body, loop, loop->ivar, info, fn,
                         nestedIvars, directChildren, deepCandidates);
    // Prefer the deepest condition: it eliminates the most repeated branch
    // checks while preserving the same scalar decision for the whole nest.
    if (deepCandidates.empty()) return;
    Stmt* deep = deepCandidates.back();
    (void)tryDeepUnswitch(list, idx, loop, deep, fn, info, budget, st);
    return;
  }

  // §4.2 P0 (zero-trip): dynamic-bounds loops may execute zero times; a
  // hoisted condition is evaluated unconditionally, so require totality
  // (non-trapping, non-diverging) unless the trip count is provably >= 1.
  if (!nonEmpty && !exprTotalAndPure(cand->cond)) {
    if (st->skippedNotes < 2) {
      ++st->skippedNotes;
      diag(Diag::Note, Code::LaneSkip,
           "cross-lane: uniform branch (line " +
           std::to_string(cand->loc.line) + ", fn " + fn->name +
           ") NOT hoisted — the loop range may be empty and the condition "
           "is not provably total (division/dereference/call could trap "
           "on a zero-trip loop the source would never execute)",
           cand->loc,
           "kind=hoist-skip-zero-trip,fn=" + fn->name);
    }
    return;
  }

  bool hasElse = cand->elseB != nullptr;

  // ---- cost model (explicit gate, reported when it vetoes)
  // lane work: arithmetic + memory ops per iteration that vectorise;
  // vector-light loops would not amortise the duplicated body.
  int minWork = hasElse ? 10 : 6;
  if (info.laneWork < minWork || info.nodes * 2 > 600 || *budget <= 0) {
    if (st->skippedNotes < 2 && info.laneWork >= 1) {
      ++st->skippedNotes;
      diag(Diag::Note, Code::LaneSkip,
           "cross-lane: uniform branch (line " +
           std::to_string(cand->loc.line) + ", fn " + fn->name +
           ") NOT hoisted out of the vector loop — cost model: lane "
           "work " + std::to_string(info.laneWork) + " ops < " +
           std::to_string(minWork) + " threshold (vector-light body; "
           "code duplication would cost more than the branch)" +
           (info.nodes * 2 > 600 ? "; body too large to duplicate" : "") +
           (*budget <= 0 ? "; unswitch budget exhausted" : ""),
           cand->loc,
           "kind=hoist-skip,fn=" + fn->name);
    }
    return;
  }

  // ---- rewrite
  std::vector<Stmt*> pre, post;
  if (bodyList) {
    for (size_t k = 0; k < ifPos; ++k) pre.push_back((*bodyList)[k]);
    for (size_t k = ifPos + 1; k < bodyList->size(); ++k)
      post.push_back((*bodyList)[k]);
  }

  auto makeLoop = [&](std::vector<Stmt*> stmts) -> Stmt* {
    Stmt* L = new Stmt(Stmt::FOR, loop->loc);
    L->forRange = true;
    L->ivar = loop->ivar;
    L->rangeLo = loop->rangeLo;   // shared; bounds are side-effect-free
    L->rangeHi = loop->rangeHi;
    L->fn = fn;
    L->depth = loop->depth;
    L->body = new Stmt(Stmt::BLOCK, loop->loc);
    L->body->fn = fn;
    L->body->parent = L;
    for (auto* s : stmts) {
      s->parent = L->body;
      L->body->stmts.push_back(s);
    }
    return L;
  };

  std::vector<Stmt*> thenStmts = pre;
  if (cand->thenB) thenStmts.push_back(cand->thenB);
  for (auto* s : post) thenStmts.push_back(s);
  std::vector<Stmt*> elseStmts = pre;
  if (hasElse) elseStmts.push_back(cand->elseB);
  for (auto* s : post) elseStmts.push_back(s);

  Stmt* nif = new Stmt(Stmt::IF, cand->loc);   // keeps loc: PGO hash stable
  nif->cond = cand->cond;                      // shared
  nif->thenB = makeLoop(thenStmts);
  nif->thenB->parent = nif;
  if (hasElse || !pre.empty() || !post.empty()) {
    nif->elseB = makeLoop(elseStmts);
    nif->elseB->parent = nif;
  }
  nif->fn = fn;
  list[idx] = nif;
  --*budget;
  ++st->applied;
  diag(Diag::Note, Code::Lane,
         "cross-lane: hoisted uniform branch (line " +
         std::to_string(cand->loc.line) + ", fn " + fn->name +
         ") out of the vector loop — the condition is evaluated once, "
         "scalar (one lane reserved for the decision); the specialised "
         "loops are branch-free so every lane does independent work",
         cand->loc,
         "kind=hoist,fn=" + fn->name + ",lanework=" +
         std::to_string(info.laneWork));
}

// ------------------------------------------------------------- driver
static void walkListsLaneOne(Stmt* s, FnDecl* fn, int loopDepth, int* budget,
                             UnswitchStats* st);

static void walkListsLane(std::vector<Stmt*>& list, FnDecl* fn, int loopDepth,
                          int* budget, UnswitchStats* st) {
  if (loopDepth > 0) st->applied += mergeScan(list, fn);

  for (size_t i = 0; i < list.size(); ++i) {
    Stmt* s = list[i];
    switch (s->k) {
      case Stmt::BLOCK:
        walkListsLane(s->stmts, fn, loopDepth, budget, st);
        break;
      case Stmt::MATCH:   // Phase 1d: arms stay in the same loop context
        for (auto& arm : s->matchArms)
          walkListsLane(arm.body->stmts, fn, loopDepth, budget, st);
        break;
      case Stmt::IF: {
        // branch lists stay inside the same loop context
        if (s->thenB && s->thenB->k == Stmt::BLOCK)
          walkListsLane(s->thenB->stmts, fn, loopDepth, budget, st);
        else if (s->thenB) walkListsLaneOne(s->thenB, fn, loopDepth, budget, st);
        if (s->elseB && s->elseB->k == Stmt::BLOCK)
          walkListsLane(s->elseB->stmts, fn, loopDepth, budget, st);
        else if (s->elseB)
          walkListsLaneOne(s->elseB, fn, loopDepth, budget, st);
        break;
      }
      case Stmt::WHILE: case Stmt::FOR: case Stmt::PARFOR: {
        if (s->body && s->body->k == Stmt::BLOCK)
          walkListsLane(s->body->stmts, fn, loopDepth + 1, budget, st);
        else if (s->body)
          walkListsLaneOne(s->body, fn, loopDepth + 1, budget, st);
        // unswitch a direct-child simple FOR loop in this list
        if (s->k == Stmt::FOR) tryUnswitchLoop(list, i, fn, budget, st);
        break;
      }
      default: break;
    }
  }
}

// helper: a loop/if body that is a single statement still contains
// nested lists (e.g. `if (c) { ... }` without braces is a BLOCK anyway;
// single-statement bodies only matter for loops wrapping loops)
static void walkListsLaneOne(Stmt* s, FnDecl* fn, int loopDepth, int* budget,
                             UnswitchStats* st) {
  if (!s) return;
  switch (s->k) {
    case Stmt::BLOCK: walkListsLane(s->stmts, fn, loopDepth, budget, st); break;
    case Stmt::MATCH:
      for (auto& arm : s->matchArms)
        walkListsLane(arm.body->stmts, fn, loopDepth, budget, st);
      break;
    case Stmt::IF:
      if (s->thenB && s->thenB->k == Stmt::BLOCK)
        walkListsLane(s->thenB->stmts, fn, loopDepth, budget, st);
      if (s->elseB && s->elseB->k == Stmt::BLOCK)
        walkListsLane(s->elseB->stmts, fn, loopDepth, budget, st);
      else if (s->elseB) walkListsLaneOne(s->elseB, fn, loopDepth, budget, st);
      break;
    case Stmt::WHILE: case Stmt::FOR: case Stmt::PARFOR:
      if (s->body && s->body->k == Stmt::BLOCK)
        walkListsLane(s->body->stmts, fn, loopDepth + 1, budget, st);
      else if (s->body)
        walkListsLaneOne(s->body, fn, loopDepth + 1, budget, st);
      break;
    default: break;
  }
}

int runLaneAnalysisPass(Program* p) {
  computeFootprints(p);
  int total = 0;
  for (auto* fn : p->fns) {
    if (!fn->body || fn->isGeneric) continue;
    int budget = 3;   // code-growth bound per function
    // two rounds: after a hoist, the specialised loops can expose
    // further uniform branches (bounded by the budget anyway)
    for (int round = 0; round < 2; ++round) {
      UnswitchStats st;
      walkListsLane(fn->body->stmts, fn, 0, &budget, &st);
      total += st.applied;
      if (st.applied == 0) break;
    }
  }
  return total;
}

} // namespace freshet
