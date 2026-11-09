// Freshet — R4 source-level loop optimisation pass 1: loop fusion
// ("fusing functions together") + full unrolling.
//
// Runs post-sema / post-checks, pre-irgen.  All transforms are
// semantics-preserving AST rewrites that reuse or deep-clone existing
// nodes (sema fields copied verbatim); no new bindings are introduced,
// so IRgen's name-keyed allocas stay valid.
//
// FUSION jams adjacent `for (iv in lo..hi)` loops over the identical
// range into ONE loop:
//
//   for (k in 0..nb) { fwd(&re[k*L], &im[k*L], L); }     // phase 1
//   for (k in 0..nb) { pw(&re[k*L], &im[k*L], &br[k*L], L); }  // phase 2
//   for (k in 0..nb) { inv(&re[k*L], &im[k*L], &or[k*L], L); } // phase 3
//
// becomes one sweep — the per-batch-element working set stays hot in
// cache across the forward / pointwise / inverse phases while the
// batching loop itself is retained.  (LLVM's loop passes cannot do
// this: they never fuse across function boundaries, and its
// dependence tests cannot see through our call slices.)
//
// Safety, three tiers (checked per pair of fused bodies, all pairs
// i<j in a run):
//   * veto: any body may return, print, assert, dispatch a parfor,
//     give ownership, or hold unknown memory effects we cannot place;
//   * tier 1: no flow/anti conflicts at all (fully independent loops);
//   * tier 2: conflicting roots touched only through direct
//     root[<affine-in-ivar, literal-coef>] indexing whose canonical
//     index expression matches on both sides → same address only at
//     the same iteration (injectivity of the affine map);
//   * tier 3: conflicting roots touched only through call slices
//     &r[iv*C] where the callee's footprint is provably bounded by its
//     count parameter (summary analysis), and the call-site count
//     argument IS C → slices tile the array exactly; reads at
//     iv*C + j with j an inner loop var bounded by C stay inside the
//     same iteration's slice.
// Bounds of later loops must not read anything an earlier body writes,
// and bounds must be side-effect-free (soa_len / @pure / math allowed).
//
// UNROLLING fully unrolls constant-trip `for (iv in A..B)` loops
// (trip <= 16, body*trip cost-capped): the induction variable folds
// into integer literals, GEP indices become constants, and the
// per-iteration alloca traffic disappears before clang ever sees the
// IR.
#include "opt_util.h"
#include <algorithm>

namespace freshet {

using namespace optutil;

// ------------------------------------------------------------ helpers
static void walkExprNodes(Expr* e, const std::function<void(Expr*)>& f) {
  if (!e) return;
  f(e);
  for (auto* a : e->args) walkExprNodes(a, f);
  walkExprNodes(e->lhs, f);
  walkExprNodes(e->rhs, f);
  walkExprNodes(e->base, f);
}

static bool isInt64Kind(Type* t) {
  return t && t->isInt() && t->bits() == 64;
}

// sema's ivar type for a range loop (commonType of the bounds, usize→u64)
static Type* rangeIvarTy(Stmt* loop) {
  if (!loop->rangeLo || !loop->rangeHi) return nullptr;
  Type* t = commonType(loop->rangeLo->ty, loop->rangeHi->ty);
  if (!t) return nullptr;
  if (t->k == Type::USIZE) t = Type::simple(Type::U64);
  return t;
}

// the ivar's Sym*, taken from a body reference (declaration scopes are
// popped; body references carry the resolved sym)
static Sym* ivarSymOf(Stmt* body, const std::string& ivar) {
  Sym* found = nullptr;
  std::function<void(Stmt*)> walk = [&](Stmt* s) {
    if (!s || found) return;
    switch (s->k) {
      case Stmt::LET: case Stmt::EXPR: case Stmt::RETURN: case Stmt::DEFER: {
        Expr* e = s->init ? s->init : (s->expr ? s->expr :
                  (s->deferExpr ? s->deferExpr : s->retVal));
        walkExprNodes(e, [&](Expr* x) {
          if (found || !(x->k == Expr::IDENT && x->name == ivar)) return;
          if (x->sym && (x->sym->k == Sym::VAR || x->sym->k == Sym::PARAM))
            found = x->sym;
        });
        break;
      }
      case Stmt::ASSIGN:
        walkExprNodes(s->lhs, [&](Expr* x) {
          if (found || !(x->k == Expr::IDENT && x->name == ivar)) return;
          if (x->sym) found = x->sym;
        });
        walkExprNodes(s->rhs, [&](Expr* x) {
          if (found || !(x->k == Expr::IDENT && x->name == ivar)) return;
          if (x->sym) found = x->sym;
        });
        break;
      case Stmt::IF:
        walkExprNodes(s->cond, [&](Expr* x) {
          if (found || !(x->k == Expr::IDENT && x->name == ivar)) return;
          if (x->sym) found = x->sym;
        });
        walk(s->thenB);
        walk(s->elseB);
        break;
      case Stmt::WHILE:
        walkExprNodes(s->cond, [&](Expr* x) {
          if (found || !(x->k == Expr::IDENT && x->name == ivar)) return;
          if (x->sym) found = x->sym;
        });
        walk(s->body);
        break;
      case Stmt::FOR: case Stmt::PARFOR:
        walk(s->body);
        break;
      case Stmt::BLOCK:
        for (auto* st : s->stmts) walk(st);
        break;
      default: break;
    }
  };
  walk(body);
  return found;
}

// is `ivar` assigned anywhere in the body? (F0406 already flags this
// for C-style loops; range loops still need the check for unrolling)
static bool writesIvar(Stmt* body, const std::string& ivar) {
  bool writes = false;
  std::function<void(Stmt*)> walk = [&](Stmt* s) {
    if (!s || writes) return;
    switch (s->k) {
      case Stmt::ASSIGN: {
        Expr* t = s->lhs;
        while (t && (t->k == Expr::FIELD || t->k == Expr::INDEX)) t = t->base;
        if (t && t->k == Expr::IDENT && t->name == ivar) writes = true;
        break;
      }
      case Stmt::MATCH:   // Phase 1d: arms are paths too
        for (auto& arm : s->matchArms) walk(arm.body);
        break;
      case Stmt::IF:
        walk(s->thenB);
        walk(s->elseB);
        break;
      case Stmt::WHILE:
        walk(s->body);
        break;
      case Stmt::FOR: case Stmt::PARFOR:
        walk(s->body);
        break;
      case Stmt::BLOCK:
        for (auto* st : s->stmts) walk(st);
        break;
      default: break;
    }
  };
  walk(body);
  return writes;
}

// bounds may contain soa_len / @pure / pure math, nothing else with effects
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

// ------------------------------------------------------- compatibility
// must every read in `rs` be covered by a same-iteration write in `ws`?
static bool readsCovered(const std::vector<Access>& ws,
                         const std::vector<Access>& rs) {
  for (const Access& r : rs) {
    bool covered = false;
    for (const Access& w : ws) {
      if (w.kind == Access::SCALAR || w.kind == Access::WHOLE ||
          w.kind == Access::WILDCARD)
        continue;
      if (r.kind == Access::SCALAR || r.kind == Access::WHOLE ||
          r.kind == Access::WILDCARD)
        continue;
      if (w.kind == Access::SLICE) {
        if (!w.bounded) continue;
        if (r.kind == Access::SLICE && r.bounded && r.idx == w.idx)
          { covered = true; break; }
        if (r.kind == Access::DIRECT && r.t3 && r.t3c == w.idx)
          { covered = true; break; }
        continue;
      }
      // w is DIRECT
      if (r.kind == Access::DIRECT &&
          w.idx == r.idx && w.affineLit && r.affineLit)
        { covered = true; break; }                       // tier 2
      if (r.kind == Access::SLICE && r.bounded &&
          w.t3 && w.t3c == r.idx)
        { covered = true; break; }                       // tier 3 (direct write)
      continue;
    }
    if (!covered) return false;
  }
  return true;
}

static bool bodyHasAnyRead(const BodyInfo& b) {
  if (b.unknownRead) return true;
  return !b.reads.empty();
}
static bool bodyHasAnyWrite(const BodyInfo& b) {
  if (b.unknownWrite) return true;
  return !b.writes.empty();
}

// ------------------------------------------------------------- fusion
struct FuseCand {
  Stmt* loop;
  BodyInfo info;
};

static bool tryFuseRun(std::vector<Stmt*>& list, size_t from, size_t end,
                       FnDecl* fn, int* applied) {
  // greedy prefix extension: [from, k) is the currently-legal run
  std::vector<FuseCand> run;
  Stmt* first = list[from];
  run.push_back({first, BodyInfo{}});
  collectBody(first->body, first->ivar, fn, &run.back().info);

  size_t lastLegal = from;   // exclusive end of the legal run
  for (size_t j = from + 1; j < end; ++j) {
    FuseCand cand{list[j], BodyInfo{}};
    collectBody(cand.loop->body, cand.loop->ivar, fn, &cand.info);
    // basic shape checks
    Stmt* prev = run.back().loop;
    Stmt* cur = cand.loop;
    if (!cur->forRange || cur->ivar.empty()) break;
    if (!sameExpr(prev->rangeLo, cur->rangeLo) ||
        !sameExpr(prev->rangeHi, cur->rangeHi))
      break;
    Type* itPrev = rangeIvarTy(prev);
    Type* itCur = rangeIvarTy(cur);
    if (!isInt64Kind(itPrev) || !isInt64Kind(itCur)) break;
    if (!boundsPure(cur->rangeLo) || !boundsPure(cur->rangeHi)) break;

    // rename feasibility (checked before including in the run so the
    // rewrite below cannot fail): different ivar names need a rebindable
    // sym + a 64-bit int ivar type on both sides
    if (cur->ivar != first->ivar) {
      Sym* curSym = ivarSymOf(cur->body, cur->ivar);
      Type* curIvTy = rangeIvarTy(cur);
      if (!curSym || !curIvTy || !isInt64Kind(curIvTy)) break;
    }

    // observable / structural vetoes
    if (cand.info.hasReturn || cand.info.hasPrint || cand.info.hasParfor ||
        cand.info.hasGive || cand.info.hasBreak)
      break;

    // unknown-flag conflicts vs every earlier body (§4.6, audit):
    // a boolean `unknownWrite` must force rejection whenever the other
    // loop has ANY effect that could be affected by it — not merely
    // reads.  The dependence classes considered explicitly:
    //   flow   : unknown write (either side) vs any read        -> reject
    //   anti   : unknown read   (either side) vs any write       -> reject
    //   output : unknown write (either side) vs any WRITE        -> reject
    //            (the historical test missed write-vs-unknown-write:
    //             jamming two loops that both write through
    //             undecomposable pointers can reorder the two writes
    //             to the same address and change the surviving value)
    //   escape : give/free-like transfers                        -> they
    //             mark writesUnknown in the callee footprint, so the
    //             write-side rules above already reject them
    //   input  : read-read is harmless — both loops keep reading on
    //            the same iteration, in the same order
    bool bad = false;
    for (auto& r : run) {
      if (cand.info.unknownWrite &&
          (bodyHasAnyRead(r.info) || bodyHasAnyWrite(r.info))) { bad = true; break; }
      if (r.info.unknownWrite &&
          (bodyHasAnyRead(cand.info) || bodyHasAnyWrite(cand.info))) { bad = true; break; }
      if (cand.info.unknownRead && bodyHasAnyWrite(r.info)) { bad = true; break; }
      if (r.info.unknownRead && bodyHasAnyWrite(cand.info)) { bad = true; break; }
    }
    if (bad) break;

    // later bounds must not read anything earlier bodies write
    std::set<std::string> br;
    exprReadRoots(cur->rangeLo, br);
    exprReadRoots(cur->rangeHi, br);
    for (auto& r : run) {
      std::set<std::string> wr = writeRoots(r.info);
      for (auto& x : br)
        if (wr.count(x)) { bad = true; break; }
      if (r.info.unknownWrite && !br.empty()) { bad = true; break; }
      if (bad) break;
    }
    if (bad) break;

    // flow / anti dependence checks vs every earlier body
    for (auto& r : run) {
      // flow: earlier writes, later reads
      for (auto& kv : r.info.writes) {
        auto it = cand.info.reads.find(kv.first);
        if (it == cand.info.reads.end()) continue;
        if (!readsCovered(kv.second, it->second)) { bad = true; break; }
      }
      if (bad) break;
      // anti: later writes, earlier reads
      for (auto& kv : cand.info.writes) {
        auto it = r.info.reads.find(kv.first);
        if (it == r.info.reads.end()) continue;
        if (!readsCovered(kv.second, it->second)) { bad = true; break; }
      }
      if (bad) break;
    }
    if (bad) break;

    run.push_back(std::move(cand));
    lastLegal = j;
  }

  if (run.size() < 2) return false;

  // size guard: avoid fusing huge bodies into one (i-cache / registers)
  int totalNodes = 0;
  for (auto& r : run) totalNodes += r.info.nodes;
  if (totalNodes > 1200) return false;

  // ---- rewrite: single loop over the first's range
  Stmt* firstL = run[0].loop;
  std::string ivar = firstL->ivar;

  Stmt* fused = new Stmt(Stmt::FOR, firstL->loc);
  fused->forRange = true;
  fused->ivar = ivar;
  fused->rangeLo = firstL->rangeLo;   // shared: side-effect-free bounds
  fused->rangeHi = firstL->rangeHi;
  fused->fn = fn;
  fused->depth = firstL->depth;
  // §5: if any loop in the fused run carried @vectorize, the fused loop
  // keeps the hint (the user asked for vectorised execution of that body)
  for (auto& r : run)
    if (r.loop->vectorizeHint) fused->vectorizeHint = true;
  fused->body = new Stmt(Stmt::BLOCK, firstL->loc);
  fused->body->fn = fn;
  fused->body->parent = fused;

  for (size_t idx = 0; idx < run.size(); ++idx) {
    Stmt* L = run[idx].loop;
    Stmt* body = L->body;
    if (L->ivar != ivar) {
      // rename: clone the body, substituting its ivar with an IDENT of
      // the fused ivar (sema fields copied; addressing is name-keyed).
      // Feasibility was proven during run extension.
      Sym* ivSym = ivarSymOf(body, L->ivar);
      Type* ivTy = rangeIvarTy(L);
      if (!ivSym || !ivTy || !isInt64Kind(ivTy)) return false;   // defensive
      Expr* repl = new Expr(Expr::IDENT, L->loc);
      repl->name = ivar;
      repl->ty = ivTy;
      repl->sym = ivSym;
      repl->isLvalue = false;
      std::map<Sym*, Expr*> subst{{ivSym, repl}};
      body = cloneStmt(body, subst);
    }
    body->parent = fused->body;
    fused->body->stmts.push_back(body);
  }

  // splice: replace the whole run [from..lastLegal] with the fused loop;
  // keep everything before and after intact
  list[from] = fused;
  list.erase(list.begin() + from + 1, list.begin() + lastLegal + 1);
  ++*applied;

  int line1 = firstL->loc.line, line2 = run.back().loop->loc.line;
  std::string rangeDesc = canon(firstL->rangeLo) + ".." + canon(firstL->rangeHi);
  diag(Diag::Note, Code::Fuse,
       "fused " + std::to_string(run.size()) + " adjacent loops (lines " +
       std::to_string(line1) + "-" + std::to_string(line2) + ") over [" +
       rangeDesc + "] in fn " + fn->name +
       " into one sweep — per-iteration data stays hot across phase "
       "boundaries; the batching loop is retained",
       firstL->loc,
       "loops=" + std::to_string(run.size()) + ",fn=" + fn->name);
  return true;
}

static int fuseInList(std::vector<Stmt*>& list, FnDecl* fn) {
  int applied = 0;
  size_t i = 0;
  while (i < list.size()) {
    Stmt* s = list[i];
    if (s->k == Stmt::FOR && s->forRange && !s->ivar.empty() &&
        rangeIvarTy(s) && isInt64Kind(rangeIvarTy(s)) &&
        boundsPure(s->rangeLo) && boundsPure(s->rangeHi)) {
      // maximal block of adjacent simple range loops starting at i
      size_t j = i + 1;
      while (j < list.size() &&
             list[j]->k == Stmt::FOR && list[j]->forRange &&
             !list[j]->ivar.empty())
        ++j;
      if (j - i >= 2 && tryFuseRun(list, i, j, fn, &applied))
        continue;   // list[i] is now the fused loop; rescan from i
    }
    ++i;
  }
  return applied;
}

// ------------------------------------------------------------ unrolling
// Clone-safety contract (optimisation-audit §4.3, P0) — what full
// unrolling may duplicate, stated explicitly:
//   hasReturn — VETOED: a constant-trip loop containing an early return
//               is not a straight-line body that can be copied; the
//               clones after the first would follow a terminating block
//               and Penstock's IR generator tracks block termination.
//               (Return-aware unrolling needs a real control-flow
//               representation; until then the fact that collectBody
//               records `hasReturn` is USED, not ignored.)
//   hasBreak  — VETOED (Phase 1a): early exits make the effective trip
//               count dynamic; unrolling would drop the early exits.
//   defer     — SAFE: each unrolled copy keeps its own BLOCK scope, so
//               per-iteration defers run at the end of their copy —
//               exactly as they ran at the end of their iteration.
//   hasGive   — SAFE: `give` consumes the value of ITS iteration's
//               expression; clones repeat the same statements in the
//               same order, so ownership transfers once per iteration.
//   hasParfor — SAFE: each clone becomes its own region with identical
//               semantics (trip <= 16 bounds the duplication).
static int unrollInList(std::vector<Stmt*>& list, FnDecl* fn) {
  int applied = 0;
  for (size_t i = 0; i < list.size(); ++i) {
    Stmt* s = list[i];
    if (!(s->k == Stmt::FOR && s->forRange)) continue;
    // §4.4: exact trip information via __int128-checked distance — the
    // historical `hi - lo` could overflow int64_t before the bounds
    // checks below ran
    TripInfo ti = tripCountOf(s);
    if (!ti.known || ti.empty || ti.saturated) continue;
    int64_t trip = ti.trip;
    if (trip < 1 || trip > 16) continue;
    Type* ivTy = rangeIvarTy(s);
    if (!isInt64Kind(ivTy)) continue;
    if (writesIvar(s->body, s->ivar)) continue;

    BodyInfo info;
    collectBody(s->body, "", fn, &info);
    if ((long long)info.nodes * trip > 200) continue;
    // Phase 1a (report §4.4): break/continue make the trip count DYNAMIC —
    // full unrolling would drop the early exits (and cloning statement
    // kinds the pass does not model would drop them from the IR entirely)
    if (info.hasBreak) continue;
    // optimisation-audit §4.3 (P0): veto bodies containing `return`
    if (info.hasReturn) continue;
    // no point unrolling an empty or single-statement trip-1 loop with
    // zero body weight? still fine — do it (removes loop-carried alloca)

    Sym* ivSym = ivarSymOf(s->body, s->ivar);
    std::map<Sym*, Expr*> subst;
    std::vector<Expr*> lits((size_t)trip);
    if (ivSym) {
      // §4.4: the induction-variable literal for iteration k is the
      // MATHEMATICAL value lo + k computed in 128 bits, then wrapped to
      // the ivar's width — never host-C++ `lo + k` arithmetic
      TypedConst loC;
      if (!evalConstTyped(s->rangeLo, &loC) || !loC.valid) continue;
      __int128 loM = loC.isSigned ? (__int128)loC.signedVal()
                                  : (__int128)loC.unsignedVal();
      for (int64_t k = 0; k < trip; ++k) {
        Expr* lit = new Expr(Expr::INT, s->loc);
        lit->ival = (uint64_t)(loM + (__int128)k);
        lit->ty = ivTy;
        lit->constant = true;
        lits[(size_t)k] = lit;
      }
    }

    Stmt* block = new Stmt(Stmt::BLOCK, s->loc);
    block->fn = fn;
    block->parent = s->parent;
    for (int64_t k = 0; k < trip; ++k) {
      std::map<Sym*, Expr*> sub;
      if (ivSym) sub[ivSym] = lits[(size_t)k];
      Stmt* copy;
      if (s->body && s->body->k == Stmt::BLOCK) {
        copy = cloneStmt(s->body, sub);
      } else {
        copy = new Stmt(Stmt::BLOCK, s->loc);   // scope for per-iteration defers
        copy->fn = fn;
        copy->parent = block;
        copy->stmts.push_back(cloneStmt(s->body, sub));
      }
      copy->parent = block;
      block->stmts.push_back(copy);
    }
    list[i] = block;
    ++applied;
    diag(Diag::Note, Code::Unroll,
         "fully unrolled constant " + std::to_string(trip) +
         "-trip loop (line " + std::to_string(s->loc.line) + ", fn " +
         fn->name + ") — induction variable folds into literal "
         "indices; constant-offset addressing reaches the backend",
         s->loc,
         "trip=" + std::to_string(trip) + ",fn=" + fn->name);
  }
  return applied;
}

// ------------------------------------------------------------- driver
static void walkLists(Stmt* s, FnDecl* fn,
                      const std::function<void(std::vector<Stmt*>&)>& visit) {
  if (!s) return;
  if (s->k == Stmt::BLOCK) {
    visit(s->stmts);
    for (auto* st : s->stmts) walkLists(st, fn, visit);
    return;
  }
  switch (s->k) {
    case Stmt::IF:
      walkLists(s->thenB, fn, visit);
      walkLists(s->elseB, fn, visit);
      break;
    case Stmt::MATCH:   // Phase 1d
      for (auto& arm : s->matchArms) walkLists(arm.body, fn, visit);
      break;
    case Stmt::WHILE: case Stmt::FOR: case Stmt::PARFOR:
      walkLists(s->body, fn, visit);
      break;
    default: break;
  }
}

int runFuseUnrollPasses(Program* p) {
  computeFootprints(p);
  int fused = 0, unrolled = 0;
  for (auto* fn : p->fns) {
    if (!fn->body || fn->isGeneric) continue;
    walkLists(fn->body, fn, [&](std::vector<Stmt*>& list) {
      fused += fuseInList(list, fn);
    });
    walkLists(fn->body, fn, [&](std::vector<Stmt*>& list) {
      unrolled += unrollInList(list, fn);
    });
  }
  return fused + unrolled;
}

} // namespace freshet
