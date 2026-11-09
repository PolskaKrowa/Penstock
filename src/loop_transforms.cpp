// Freshet — Phase D source-level loop transforms.  See loop_transforms.h
// for the pass contracts; each transform below documents its exact
// legality premises (they also go into --proof-log lines at runtime).
#include "loop_transforms.h"
#include "opt_util.h"
#include "loop_analysis.h"
#include "profit_model.h"
#include <algorithm>
#include <cstdlib>
#include <functional>

namespace freshet {

using namespace optutil;


// ================================================================ helpers
static bool isInt64KindT(Type* t) {
  return t && t->isInt() && t->bits() == 64;
}

static Type* rangeIvarTyT(Stmt* loop) {
  if (!loop->rangeLo || !loop->rangeHi) return nullptr;
  Type* t = commonType(loop->rangeLo->ty, loop->rangeHi->ty);
  if (!t) return nullptr;
  if (t->k == Type::USIZE) t = Type::simple(Type::U64);
  return t;
}

static void walkExprNodesT(Expr* e, const std::function<void(Expr*)>& f) {
  if (!e) return;
  f(e);
  for (auto* a : e->args) walkExprNodesT(a, f);
  walkExprNodesT(e->lhs, f);
  walkExprNodesT(e->rhs, f);
  walkExprNodesT(e->base, f);
}

static int countNodes(Expr* e) {
  int n = 0;
  walkExprNodesT(e, [&](Expr*) { ++n; });
  return n;
}

static bool containsCall(Expr* e) {
  bool has = false;
  walkExprNodesT(e, [&](Expr* x) { if (x->k == Expr::CALL) has = true; });
  return has;
}

// the ivar's Sym*, taken from a body reference (same trick as the
// fusion pass: declaration scopes are popped, body refs carry the sym)
static Sym* ivarSymOfT(Stmt* body, const std::string& ivar) {
  Sym* found = nullptr;
  std::function<void(Stmt*)> walk = [&](Stmt* s) {
    if (!s || found) return;
    switch (s->k) {
      case Stmt::LET: case Stmt::EXPR: case Stmt::RETURN: case Stmt::DEFER: {
        Expr* e = s->init ? s->init : (s->expr ? s->expr :
                  (s->deferExpr ? s->deferExpr : s->retVal));
        walkExprNodesT(e, [&](Expr* x) {
          if (found || !(x->k == Expr::IDENT && x->name == ivar)) return;
          if (x->sym && (x->sym->k == Sym::VAR || x->sym->k == Sym::PARAM))
            found = x->sym;
        });
        break;
      }
      case Stmt::ASSIGN: {
        walkExprNodesT(s->lhs, [&](Expr* x) {
          if (found || !(x->k == Expr::IDENT && x->name == ivar)) return;
          if (x->sym) found = x->sym;
        });
        walkExprNodesT(s->rhs, [&](Expr* x) {
          if (found || !(x->k == Expr::IDENT && x->name == ivar)) return;
          if (x->sym) found = x->sym;
        });
        break;
      }
      case Stmt::IF: {
        walkExprNodesT(s->cond, [&](Expr* x) {
          if (found || !(x->k == Expr::IDENT && x->name == ivar)) return;
          if (x->sym) found = x->sym;
        });
        walk(s->thenB);
        walk(s->elseB);
        break;
      }
      case Stmt::WHILE:
        walkExprNodesT(s->cond, [&](Expr* x) {
          if (found || !(x->k == Expr::IDENT && x->name == ivar)) return;
          if (x->sym) found = x->sym;
        });
        walk(s->body);
        break;
      case Stmt::FOR: case Stmt::PARFOR:
        walk(s->body);
        break;
      case Stmt::MATCH:
        walkExprNodesT(s->matchOn, [&](Expr* x) {
          if (found || !(x->k == Expr::IDENT && x->name == ivar)) return;
          if (x->sym) found = x->sym;
        });
        for (auto& arm : s->matchArms) walk(arm.body);
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

static bool writesIvarT(Stmt* body, const std::string& ivar) {
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
      case Stmt::MATCH:
        for (auto& arm : s->matchArms) walk(arm.body);
        break;
      case Stmt::IF: walk(s->thenB); walk(s->elseB); break;
      case Stmt::WHILE: case Stmt::FOR: case Stmt::PARFOR: walk(s->body); break;
      case Stmt::BLOCK: for (auto* st : s->stmts) walk(st); break;
      default: break;
    }
  };
  walk(body);
  return writes;
}

static void repairParentsT(Stmt* s, Stmt* parent, FnDecl* fn) {
  if (!s) return;
  s->parent = parent;
  s->fn = fn;
  for (auto* child : s->stmts) repairParentsT(child, s, fn);
  repairParentsT(s->body, s, fn);
  repairParentsT(s->thenB, s, fn);
  repairParentsT(s->elseB, s, fn);
  for (auto& arm : s->matchArms) repairParentsT(arm.body, s, fn);
}

// all binding names introduced INSIDE a subtree (let vars + loop vars):
// nothing defined inside the loop may be referenced by a hoisted
// expression
static void collectBindings(Stmt* s, std::set<std::string>& out) {
  if (!s) return;
  if (s->k == Stmt::LET) out.insert(s->var);
  if (s->k == Stmt::FOR && !s->ivar.empty()) out.insert(s->ivar);
  if (s->k == Stmt::PARFOR && !s->pvar.empty()) out.insert(s->pvar);
  switch (s->k) {
    case Stmt::IF: collectBindings(s->thenB, out); collectBindings(s->elseB, out); break;
    case Stmt::WHILE: case Stmt::FOR: case Stmt::PARFOR: collectBindings(s->body, out); break;
    case Stmt::MATCH: for (auto& a : s->matchArms) collectBindings(a.body, out); break;
    case Stmt::BLOCK: for (auto* st : s->stmts) collectBindings(st, out); break;
    default: break;
  }
}

// is `name` bound exactly once in fn (and not a param/global)?
static bool nameUniqueInFn(FnDecl* fn, const std::string& name) {
  if (lookupGlobal(name)) return false;
  for (auto& p : fn->params)
    if (p.name == name) return false;
  int count = 0;
  std::function<void(Stmt*)> walk = [&](Stmt* s) {
    if (!s) return;
    if (s->k == Stmt::LET && s->var == name) ++count;
    switch (s->k) {
      case Stmt::IF: walk(s->thenB); walk(s->elseB); break;
      case Stmt::WHILE: case Stmt::FOR: case Stmt::PARFOR: walk(s->body); break;
      case Stmt::MATCH: for (auto& a : s->matchArms) walk(a.body); break;
      case Stmt::BLOCK: for (auto* st : s->stmts) walk(st); break;
      default: break;
    }
  };
  walk(fn->body);
  return count == 1;
}

// ---------------------------------------------------- expression builders
// synthesized nodes carry full sema fields (ty, sym, constant) so IRgen
// lowers them like parser-produced ones; they introduce NO bindings
// except freshLoopVar below.
static Expr* mkInt(uint64_t v, Type* t, SrcLoc loc) {
  Expr* e = new Expr(Expr::INT, loc);
  e->ival = v;
  e->ty = t;
  e->constant = true;
  return e;
}

static Expr* mkIdent(const std::string& name, Sym* sym, Type* t, SrcLoc loc) {
  Expr* e = new Expr(Expr::IDENT, loc);
  e->name = name;
  e->sym = sym;
  e->ty = t;
  return e;
}

static Expr* mkBin(const std::string& op, Expr* a, Expr* b, Type* t,
                   SrcLoc loc) {
  Expr* e = new Expr(Expr::BIN, loc);
  e->op = op;
  e->lhs = a;
  e->rhs = b;
  e->ty = t;
  return e;
}

static Expr* mkImin(Expr* a, Expr* b, Type* t, SrcLoc loc) {
  Expr* e = new Expr(Expr::CALL, loc);
  e->name = "imin";
  e->args.push_back(a);
  e->args.push_back(b);
  e->ty = t;
  return e;
}

// fresh induction variable for tiling: appended to the function's sema
// symbol table so IRgen emits its alloca (allocas are name-keyed from
// fs->allSyms); the returned heap Sym only annotates synthesized IDENTs
// (IRgen resolves by name through varAllocas).
static Sym* freshLoopVar(FnDecl* fn, const std::string& base, Type* ty,
                         std::string* nameOut) {
  static int counter = 0;
  FnSema* fs = semaForFn(fn);
  std::set<std::string> taken;
  if (fs)
    for (auto& s : fs->allSyms) taken.insert(s.name);
  std::string nm;
  do {
    nm = base + "." + std::to_string(counter++);
  } while (taken.count(nm) || lookupGlobal(nm));
  *nameOut = nm;
  Sym* heapSym = new Sym();
  heapSym->k = Sym::VAR;
  heapSym->name = nm;
  heapSym->ty = ty;
  if (fs) {
    Sym vs;
    vs.k = Sym::VAR;
    vs.name = nm;
    vs.ty = ty;
    fs->allSyms.push_back(vs);
  }
  return heapSym;
}

// ------------------------------------------------------------- walker
static void walkListsT(Stmt* s, FnDecl* fn,
                       const std::function<void(std::vector<Stmt*>&)>& visit) {
  if (!s) return;
  if (s->k == Stmt::BLOCK) {
    visit(s->stmts);
    for (auto* st : s->stmts) walkListsT(st, fn, visit);
    return;
  }
  switch (s->k) {
    case Stmt::IF:
      walkListsT(s->thenB, fn, visit);
      walkListsT(s->elseB, fn, visit);
      break;
    case Stmt::MATCH:
      for (auto& arm : s->matchArms) walkListsT(arm.body, fn, visit);
      break;
    case Stmt::WHILE: case Stmt::FOR: case Stmt::PARFOR:
      walkListsT(s->body, fn, visit);
      break;
    default: break;
  }
}

static bool isSimpleRangeForT(Stmt* s) {
  return s->k == Stmt::FOR && s->forRange && !s->ivar.empty();
}

// ================================================================ LICM
// §6.4 FreshetLICM.  Premises for hoisting `let x = E;` above the loop:
//   P1  x is immutable and its name is unique in the function (no
//       shadowing, so the name-keyed alloca cannot be captured);
//   P2  E mentions no binding defined inside the loop (incl. the ivar
//       and nested ivars) — nothing loop-local is in scope outside;
//   P3  E is referentially transparent (no I/O, alloc, free, unknown
//       call, divergence) — evaluating it once vs per-iteration is
//       unobservable;
//   P4  nothing the loop writes is read by E (value identical at every
//       evaluation point);
//   P5  the loop is provably non-empty, or E is total (non-trapping) —
//       hoisting must not introduce an evaluation on a zero-trip loop.
static int licmInList(std::vector<Stmt*>& list, FnDecl* fn, int* budget) {
  int applied = 0;
  for (size_t i = 0; i < list.size() && *budget > 0; ++i) {
    Stmt* loop = list[i];
    if (!isSimpleRangeForT(loop)) continue;
    if (!isInt64KindT(rangeIvarTyT(loop))) continue;
    TripInfo ti = tripCountOf(loop);
    BodyInfo info;
    collectBody(loop->body, loop->ivar, fn, &info);
    if (info.hasReturn || info.hasBreak) continue;
    std::set<std::string> innerBindings;
    collectBindings(loop->body, innerBindings);
    std::set<std::string> wr = writeRoots(info);
    auto* bodyList = stmtsOf(loop->body);
    if (!bodyList) continue;

    for (int hoisted = 0; hoisted < 2 && *budget > 0;) {
      int foundIdx = -1;
      for (size_t k = 0; k < bodyList->size(); ++k) {
        Stmt* L = (*bodyList)[k];
        if (L->k != Stmt::LET || L->mut || !L->init) continue;
        if (!nameUniqueInFn(fn, L->var)) continue;
        Expr* E = L->init;
        if (!exprReferentiallyTransparent(E)) continue;
        std::set<std::string> ids;
        collectIdents(E, ids);
        bool ok = true;
        for (auto& n : ids) {
          if (n == loop->ivar || innerBindings.count(n)) { ok = false; break; }
          if (wr.count(n)) { ok = false; break; }
          if (info.unknownWrite && info.escapedNames.count(n)) { ok = false; break; }
        }
        if (!ok) continue;
        std::set<std::string> rr;
        exprReadRoots(E, rr);
        for (auto& r : rr)
          if (wr.count(r)) { ok = false; break; }
        if (!ok) continue;
        if (!(ti.known && !ti.empty) && !exprTotalAndPure(E)) continue;
        // profitability (§6.4/§8): hoist only when the init carries real
        // work — a call or >= 3 nodes; tiny expressions are left to
        // LLVM's own LICM once the loop reaches IR
        if (countNodes(E) < 3 && !containsCall(E)) continue;
        foundIdx = (int)k;
        break;
      }
      if (foundIdx < 0) break;
      Stmt* L = (*bodyList)[foundIdx];
      bodyList->erase(bodyList->begin() + foundIdx);
      list.insert(list.begin() + i, L);
      L->parent = loop->parent;
      L->fn = fn;
      ++applied;
      ++hoisted;
      --*budget;
      diag(Diag::Note, Code::Licm,
           "hoisted loop-invariant binding '" + L->var + "' (line " +
           std::to_string(L->loc.line) + ", fn " + fn->name +
           ") out of the range loop — the pure, non-trapping initializer "
           "is computed once instead of per iteration",
           L->loc,
           "kind=licm,fn=" + fn->name + ",var=" + L->var);
      proofLogLine("licm",
                   "P1:immutable+unique-name;P2:no-inner-bindings;"
                   "P3:referentially-transparent;P4:no-loop-write-of-reads;"
                   "P5:" +
                       std::string(ti.known && !ti.empty
                                       ? "trip>=1-proven"
                                       : "total-non-trapping"),
                   fn, L->loc);
    }
  }
  return applied;
}

// ========================================================= PartialUnroll
// §6.5 (constant-trip v1).  The main loop runs [lo, lo+evenTrip) with F
// copies of the body whose ivar references are rewritten to i, i+1, ...,
// i+F-1; the remainder loop [lo+evenTrip, hi) keeps the original body —
// the iteration domain is partitioned EXACTLY (the Lean kernel's
// partition theorem models this premise).  Constant bounds only: the
// trip distance is computed with __int128-checked arithmetic and the
// rewritten bounds are exact literals, so no runtime bound arithmetic is
// synthesized (dynamic-trip partial unrolling needs the interval
// analysis to prove hi>=lo and is deliberately deferred).
static int partialUnrollInList(std::vector<Stmt*>& list, FnDecl* fn) {
  int applied = 0;
  for (size_t i = 0; i < list.size(); ++i) {
    Stmt* s = list[i];
    if (!isSimpleRangeForT(s)) continue;
    TripInfo ti = tripCountOf(s);
    if (!ti.known || ti.empty || ti.saturated) continue;
    int64_t trip = ti.trip;
    if (trip <= 16 || trip > 4096) continue;
    Type* ivTy = rangeIvarTyT(s);
    if (!isInt64KindT(ivTy)) continue;
    if (writesIvarT(s->body, s->ivar)) continue;
    BodyInfo info;
    collectBody(s->body, s->ivar, fn, &info);
    if (info.hasReturn || info.hasBreak) continue;   // control flow in copies
    profit::BodyCost pc = profit::estimateBody(info);
    int F = profit::chooseUnrollFactor(pc, trip);
    if (F < 2) continue;
    int64_t evenTrip = trip - (trip % F);

    TypedConst loC;
    if (!evalConstTyped(s->rangeLo, &loC) || !loC.valid) continue;
    __int128 loM = loC.isSigned ? (__int128)loC.signedVal()
                                : (__int128)loC.unsignedVal();
    Sym* ivSym = ivarSymOfT(s->body, s->ivar);

    // ---- main loop: block-index form.  The ivar is REUSED as the block
    // index t in [0, evenTrip/F); copy k executes the body for the
    // ORIGINAL ivar value lo + t*F + k, so the union of executed
    // iteration values is exactly [lo, lo+evenTrip) — each value ONCE.
    // (The block index starts at 0 regardless of lo: the value
    // reconstruction adds lo back explicitly, which keeps non-zero
    // lower bounds exact — found by the §10.3 property tests.)
    Stmt* mainL = new Stmt(Stmt::FOR, s->loc);
    mainL->forRange = true;
    mainL->ivar = s->ivar;
    mainL->rangeLo = mkInt(0, ivTy, s->loc);
    mainL->rangeHi = mkInt((uint64_t)(evenTrip / F), ivTy, s->loc);
    mainL->fn = fn;
    mainL->depth = s->depth;
    mainL->vectorizeHint = s->vectorizeHint;
    mainL->parent = s->parent;
    Stmt* mainBody = new Stmt(Stmt::BLOCK, s->loc);
    mainBody->fn = fn;
    mainBody->parent = mainL;
    mainL->body = mainBody;
    for (int k = 0; k < F; ++k) {
      std::map<Sym*, Expr*> sub;
      if (ivSym) {
        Expr* t = mkIdent(s->ivar, ivSym, ivTy, s->loc);
        // value = lo + t*F + k   (computed in 128 bits, fits by trip check)
        Expr* val = mkBin("+",
                          mkBin("*", t, mkInt((uint64_t)F, ivTy, s->loc),
                                ivTy, s->loc),
                          mkInt((uint64_t)(loM + (__int128)k), ivTy, s->loc),
                          ivTy, s->loc);
        sub[ivSym] = val;
      }
      Stmt* copy;
      if (s->body && s->body->k == Stmt::BLOCK) {
        copy = cloneStmt(s->body, sub);
      } else {
        copy = new Stmt(Stmt::BLOCK, s->loc);   // scope for defers
        copy->fn = fn;
        copy->stmts.push_back(cloneStmt(s->body, sub));
      }
      mainBody->stmts.push_back(copy);
    }
    repairParentsT(mainL, s->parent, fn);

    std::vector<Stmt*> replacement;
    replacement.push_back(mainL);

    // ---- remainder loop: [lo+evenTrip, hi), original body
    if (evenTrip != trip) {
      Stmt* remL = new Stmt(Stmt::FOR, s->loc);
      remL->forRange = true;
      remL->ivar = s->ivar;
      remL->rangeLo = mkInt((uint64_t)(loM + evenTrip), ivTy, s->loc);
      remL->rangeHi = s->rangeHi;     // shared constant bound
      remL->fn = fn;
      remL->depth = s->depth;
      remL->vectorizeHint = s->vectorizeHint;
      remL->parent = s->parent;
      remL->body = cloneStmt(s->body, {});
      repairParentsT(remL, s->parent, fn);
      replacement.push_back(remL);
    }

    // splice: replace s with mainL (+ remL)
    list[i] = replacement[0];
    if (replacement.size() > 1)
      list.insert(list.begin() + i + 1, replacement[1]);
    ++applied;
    diag(Diag::Note, Code::PartialUnroll,
         "partially unrolled constant " + std::to_string(trip) +
         "-trip loop by " + std::to_string(F) + " (line " +
         std::to_string(s->loc.line) + ", fn " + fn->name +
         ") — the main loop runs " + std::to_string(evenTrip / F) +
         " block iteration(s) of " + std::to_string(F) +
         " body copies (i -> i*" + std::to_string(F) + "+k); a " + std::to_string(trip - evenTrip) +
         "-iteration remainder loop preserves the exact domain",
         s->loc,
         "kind=partial-unroll,fn=" + fn->name +
         ",trip=" + std::to_string(trip) + ",factor=" + std::to_string(F));
    proofLogLine("partial-unroll",
                 "constant-trip:" + std::to_string(trip) +
                 ";domain-partition:main[" + std::to_string(evenTrip) +
                 "]+remainder[" + std::to_string(trip - evenTrip) +
                 "];no-return,no-break;factor=" + std::to_string(F),
                 fn, s->loc);
  }
  return applied;
}

// ================================================================ Peel
// §6.5 LoopPeel (first-iteration guard removal).  Recognised shapes,
// with V(k) the condition value at iteration k (iv = lo + k):
//   iv == lo / iv <= lo / lo >= iv    ->  V(0)=true,  V(k>=1)=false
//   iv >  lo / lo <  iv               ->  V(0)=false, V(k>=1)=true
// (injectivity of the range gives V(k>=1) unconditionally).  The peeled
// copy replaces the IF by the arm the guard selects at k=0; the
// remainder loop starts at lo+1 with the guard replaced by the arm it
// provably selects there.
enum class PeelVal { Unknown, True, False };

static PeelVal peelShape(Expr* cond, const std::string& iv, __int128 loM,
                         PeelVal* rest) {
  *rest = PeelVal::Unknown;
  if (!cond || cond->k != Expr::BIN) return PeelVal::Unknown;
  const std::string& op = cond->op;
  auto stripCasts = [](Expr* x) {
    while (x && x->k == Expr::CAST && x->castTy && x->castTy->isInt())
      x = x->base;
    return x;
  };
  Expr* l = stripCasts(cond->lhs);
  Expr* r = stripCasts(cond->rhs);
  bool lIv = l && l->k == Expr::IDENT && l->name == iv;
  bool rIv = r && r->k == Expr::IDENT && r->name == iv;
  if (!lIv && !rIv) return PeelVal::Unknown;
  Expr* other = lIv ? r : l;
  TypedConst kc;
  if (!other || !(other->constant && other->k == Expr::INT) ||
      !evalConstTyped(other, &kc))
    return PeelVal::Unknown;
  __int128 k = kc.isSigned ? (__int128)kc.signedVal()
                           : (__int128)kc.unsignedVal();
  bool ivLeft = lIv;
  // normalise to "iv OP k"
  std::string norm = op;
  if (!ivLeft) {
    // mirror the comparison so the ivar is on the left
    if (op == "<") norm = ">";
    else if (op == ">") norm = "<";
    else if (op == "<=") norm = ">=";
    else if (op == ">=") norm = "<=";
  }
  if (k == loM) {
    if (norm == "==" || norm == "<=" ) {
      // iv == lo (true at 0), iv <= lo (true only at 0)
      *rest = PeelVal::False;
      return PeelVal::True;
    }
    if (norm == ">") {          // iv > lo: false at 0, true for k>=1
      *rest = PeelVal::True;
      return PeelVal::False;
    }
    if (norm == ">=") {         // iv >= lo: true everywhere — useless
      return PeelVal::Unknown;
    }
    if (norm == "<") {          // iv < lo: false everywhere
      *rest = PeelVal::False;
      return PeelVal::False;
    }
  }
  return PeelVal::Unknown;
}

// replace the IF at `loc` with `arm` inside a statement list
static bool replaceIfAt(std::vector<Stmt*>& list, const SrcLoc& loc,
                        Stmt* arm) {
  for (auto*& s : list) {
    if (s && s->k == Stmt::IF && s->loc.line == loc.line &&
        s->loc.col == loc.col &&
        (loc.startOffset == 0 || s->loc.startOffset == loc.startOffset)) {
      s = arm ? arm : new Stmt(Stmt::BLOCK, s->loc);
      return true;
    }
  }
  return false;
}

static bool replaceIfRef(Stmt*& s, const SrcLoc& loc, Stmt* arm) {
  if (!s) return false;
  if (s->k == Stmt::IF && s->loc.line == loc.line &&
      s->loc.col == loc.col &&
      (loc.startOffset == 0 || s->loc.startOffset == loc.startOffset)) {
    s = arm ? arm : new Stmt(Stmt::BLOCK, s->loc);
    return true;
  }
  if (auto* sl = stmtsOf(s)) {
    if (replaceIfAt(*sl, loc, arm)) return true;
    for (auto* st : *sl)
      if (replaceIfRef(st, loc, arm)) return true;
    return false;
  }
  switch (s->k) {
    case Stmt::IF:
      if (replaceIfRef(s->thenB, loc, arm)) return true;
      return replaceIfRef(s->elseB, loc, arm);
    case Stmt::WHILE: case Stmt::FOR: case Stmt::PARFOR:
      return replaceIfRef(s->body, loc, arm);
    case Stmt::MATCH:
      for (auto& a : s->matchArms)
        if (replaceIfRef(a.body, loc, arm)) return true;
      return false;
    default: return false;
  }
}

static int peelInList(std::vector<Stmt*>& list, FnDecl* fn) {
  int applied = 0;
  for (size_t i = 0; i < list.size(); ++i) {
    Stmt* s = list[i];
    if (!isSimpleRangeForT(s)) continue;
    TripInfo ti = tripCountOf(s);
    if (!ti.known || ti.empty || ti.saturated || ti.trip < 2) continue;
    Type* ivTy = rangeIvarTyT(s);
    if (!isInt64KindT(ivTy)) continue;
    if (writesIvarT(s->body, s->ivar)) continue;
    BodyInfo info;
    collectBody(s->body, s->ivar, fn, &info);
    if (info.hasReturn || info.hasBreak) continue;
    TypedConst loC;
    if (!evalConstTyped(s->rangeLo, &loC)) continue;
    __int128 loM = loC.isSigned ? (__int128)loC.signedVal()
                                : (__int128)loC.unsignedVal();
    Sym* ivSym = ivarSymOfT(s->body, s->ivar);

    // find the peelable guard IF in the body list
    auto* bodyList = stmtsOf(s->body);
    if (!bodyList) continue;
    Stmt* guard = nullptr;
    PeelVal first = PeelVal::Unknown, rest = PeelVal::Unknown;
    for (auto* st : *bodyList) {
      if (!st || st->k != Stmt::IF || !st->cond) continue;
      PeelVal r2;
      PeelVal f = peelShape(st->cond, s->ivar, loM, &r2);
      if (f != PeelVal::Unknown && r2 != PeelVal::Unknown &&
          f != r2) {   // must differ for peeling to pay
        guard = st;
        first = f;
        rest = r2;
        break;
      }
    }
    if (!guard) continue;

    // ---- peeled copy: ivar -> lo, guard -> the arm it selects at k=0
    std::map<Sym*, Expr*> sub0;
    if (ivSym) sub0[ivSym] = mkInt((uint64_t)loM, ivTy, s->loc);
    Stmt* peeled = cloneStmt(s->body, sub0);
    Stmt* firstArm = (first == PeelVal::True) ? guard->thenB : guard->elseB;
    replaceIfRef(peeled, guard->loc, firstArm ? firstArm : nullptr);

    // ---- remainder loop: [lo+1, hi), guard -> the arm it selects at k>=1
    Stmt* remL = new Stmt(Stmt::FOR, s->loc);
    remL->forRange = true;
    remL->ivar = s->ivar;
    remL->rangeLo = mkInt((uint64_t)(loM + 1), ivTy, s->loc);
    remL->rangeHi = s->rangeHi;
    remL->fn = fn;
    remL->depth = s->depth;
    remL->vectorizeHint = s->vectorizeHint;
    remL->parent = s->parent;
    remL->body = cloneStmt(s->body, {});
    Stmt* restArm = (rest == PeelVal::True) ? guard->thenB : guard->elseB;
    replaceIfRef(remL->body, guard->loc, restArm ? restArm : nullptr);
    repairParentsT(remL, s->parent, fn);

    // ---- splice: peeled statements, then the remainder loop
    std::vector<Stmt*> peeledStmts;
    if (auto* pl = stmtsOf(peeled)) peeledStmts = *pl;
    else peeledStmts.push_back(peeled);
    list.erase(list.begin() + i);
    list.insert(list.begin() + i, remL);
    for (auto it = peeledStmts.rbegin(); it != peeledStmts.rend(); ++it) {
      (*it)->parent = s->parent;
      (*it)->fn = fn;
      list.insert(list.begin() + i, *it);
    }
    ++applied;
    diag(Diag::Note, Code::Peel,
         "peeled the first iteration of the constant " +
         std::to_string(ti.trip) + "-trip loop (line " +
         std::to_string(s->loc.line) + ", fn " + fn->name +
         ") — the first-iteration guard is resolved statically in the "
         "peeled copy and removed from the steady-state loop",
         s->loc,
         "kind=peel,fn=" + fn->name + ",trip=" + std::to_string(ti.trip));
    proofLogLine("peel",
                 "constant-trip:" + std::to_string(ti.trip) +
                 ";guard-value@0:" +
                 (first == PeelVal::True ? "true" : "false") +
                 ";guard-value@k>=1:" +
                 (rest == PeelVal::True ? "true" : "false") +
                 ";ivar-injective-over-range",
                 fn, s->loc);
    // skip past what we inserted
    i += peeledStmts.size();
  }
  return applied;
}

// ========================================================= Interchange
// access vote collection for the stride heuristic: every array access
// (read or write — this is a PROFITABILITY estimate, not legality)
// votes on whether the CURRENT inner loop tracks the contiguous
// dimension.  Two-level root[e1][e2]: e1 tracks the outer loop (row
// selector) and e2 the inner loop => contiguous; the transposed pattern
// votes strided.  Single-level root[idx]: idx tracking the inner loop is
// contiguous.
struct AccessVote { int contiguous = 0; int strided = 0; };

static void voteExpr(Expr* e, const std::string& outerIv,
                     const std::string& innerIv, AccessVote* v) {
  if (!e) return;
  if (e->k == Expr::INDEX) {
    // this node is the TOP of its index chain: vote once for the whole
    // access, then recurse into the index EXPRESSIONS only — never into
    // an INDEX base (that node belongs to this same access; visiting it
    // separately would double-count `g[j][i]` as a fake `g[j]` access)
    if (e->rhs) {
      if (e->base && e->base->k == Expr::INDEX && e->base->rhs) {
        Affine a1o = affineIn(e->base->rhs, outerIv);
        Affine a1i = affineIn(e->base->rhs, innerIv);
        Affine a2o = affineIn(e->rhs, outerIv);
        Affine a2i = affineIn(e->rhs, innerIv);
        bool firstOuter = a1o.valid && a1o.coef != 0;
        bool firstInner = a1i.valid && a1i.coef != 0;
        bool secondOuter = a2o.valid && a2o.coef != 0;
        bool secondInner = a2i.valid && a2i.coef != 0;
        if (firstOuter && secondInner) ++v->contiguous;    // row-major
        else if (firstInner && secondOuter) ++v->strided;  // transposed
        else if (firstInner && !secondOuter && !secondInner) ++v->strided;
      } else {
        Affine io = affineIn(e->rhs, outerIv);
        Affine ii = affineIn(e->rhs, innerIv);
        bool o = io.valid && io.coef != 0;
        bool n = ii.valid && ii.coef != 0;
        if (n && !o) ++v->contiguous;
        else if (o && !n) ++v->strided;
      }
      voteExpr(e->rhs, outerIv, innerIv, v);
    }
    if (e->base && e->base->k != Expr::INDEX)
      voteExpr(e->base, outerIv, innerIv, v);   // call/paren bases
    return;
  }
  for (auto* a : e->args) voteExpr(a, outerIv, innerIv, v);
  voteExpr(e->lhs, outerIv, innerIv, v);
  voteExpr(e->rhs, outerIv, innerIv, v);
  voteExpr(e->base, outerIv, innerIv, v);
}

static void collectVotes(Stmt* body, const std::string& outerIv,
                         const std::string& innerIv, AccessVote* v) {
  std::function<void(Stmt*)> walkS = [&](Stmt* s) {
    if (!s) return;
    switch (s->k) {
      case Stmt::LET: voteExpr(s->init, outerIv, innerIv, v); break;
      case Stmt::ASSIGN:
        voteExpr(s->lhs, outerIv, innerIv, v);
        voteExpr(s->rhs, outerIv, innerIv, v);
        break;
      case Stmt::RETURN: voteExpr(s->retVal, outerIv, innerIv, v); break;
      case Stmt::DEFER: voteExpr(s->deferExpr, outerIv, innerIv, v); break;
      case Stmt::EXPR: voteExpr(s->expr, outerIv, innerIv, v); break;
      case Stmt::IF:
        voteExpr(s->cond, outerIv, innerIv, v);
        walkS(s->thenB);
        walkS(s->elseB);
        break;
      case Stmt::WHILE:
        voteExpr(s->cond, outerIv, innerIv, v);
        walkS(s->body);
        break;
      case Stmt::FOR: case Stmt::PARFOR: walkS(s->body); break;
      case Stmt::MATCH:
        voteExpr(s->matchOn, outerIv, innerIv, v);
        for (auto& a : s->matchArms) walkS(a.body);
        break;
      case Stmt::BLOCK: for (auto* st : s->stmts) walkS(st); break;
      default: break;
    }
  };
  walkS(body);
}

// shared legality for the perfect-nest reordering transforms
// (interchange / tile / unroll-and-jam).  Premises:
//   R1  perfect rectangular nest (inner bounds never mention the outer
//       ivar; both loops are simple 64-bit range loops);
//   R2  bound evaluation counts change in BOTH directions, so every
//       bound must be referentially transparent AND total;
//   R3  iterations are independent (no root is both read and written;
//       no unknown memory) — any iteration order is legal;
//   R4  no observable effects (no print/return/break/parfor/give);
//   R5  no bound reads anything the body writes (bounds now evaluate
//       between inner iterations).
static Stmt* perfectInner(Stmt* outer, FnDecl* fn, BodyInfo* ibOut) {
  auto* obl = stmtsOf(outer->body);
  Stmt* inner = nullptr;
  if (obl && obl->size() == 1 && (*obl)[0]->k == Stmt::FOR)
    inner = (*obl)[0];
  else if (outer->body && outer->body->k == Stmt::FOR)
    inner = outer->body;
  if (!inner || !inner->forRange || inner->ivar.empty()) return nullptr;
  if (inner->ivar == outer->ivar) return nullptr;
  if (!isInt64KindT(rangeIvarTyT(outer))) return nullptr;
  if (!isInt64KindT(rangeIvarTyT(inner))) return nullptr;
  std::set<std::string> innerNames;
  collectIdents(inner->rangeLo, innerNames);
  collectIdents(inner->rangeHi, innerNames);
  if (innerNames.count(outer->ivar)) return nullptr;
  if (!exprTotalAndPure(outer->rangeLo) || !exprTotalAndPure(outer->rangeHi))
    return nullptr;                                     // R2 (gains evals)
  if (!exprTotalAndPure(inner->rangeLo) || !exprTotalAndPure(inner->rangeHi))
    return nullptr;                                     // R2
  BodyInfo ib;
  collectBody(inner->body, inner->ivar, fn, &ib);
  if (ib.hasReturn || ib.hasBreak) return nullptr;
  if (!iterationsIndependent(ib)) return nullptr;
  if (!noObservableEffects(ib)) return nullptr;
  std::set<std::string> wr = writeRoots(ib);
  std::set<std::string> br;
  exprReadRoots(outer->rangeLo, br);
  exprReadRoots(outer->rangeHi, br);
  exprReadRoots(inner->rangeLo, br);
  exprReadRoots(inner->rangeHi, br);
  for (auto& r : br)
    if (wr.count(r)) return nullptr;
  *ibOut = ib;
  return inner;
}

static int interchangeInList(std::vector<Stmt*>& list, FnDecl* fn) {
  int applied = 0;
  for (size_t i = 0; i < list.size(); ++i) {
    Stmt* outer = list[i];
    if (!isSimpleRangeForT(outer)) continue;
    BodyInfo ib;
    Stmt* inner = perfectInner(outer, fn, &ib);
    if (!inner) continue;

    // ---- profitability (§6.6): stride votes on real array accesses
    AccessVote v;
    collectVotes(inner->body, outer->ivar, inner->ivar, &v);
    if (v.strided == 0 || v.contiguous > 0) continue;   // nothing to gain

    // ---- rewrite: swap the nest (body MOVED, not cloned)
    Stmt* newOuter = new Stmt(Stmt::FOR, inner->loc);
    newOuter->forRange = true;
    newOuter->ivar = inner->ivar;
    newOuter->rangeLo = inner->rangeLo;
    newOuter->rangeHi = inner->rangeHi;
    newOuter->fn = fn;
    newOuter->depth = outer->depth;
    newOuter->vectorizeHint = inner->vectorizeHint;
    newOuter->parent = outer->parent;

    Stmt* newInner = new Stmt(Stmt::FOR, outer->loc);
    newInner->forRange = true;
    newInner->ivar = outer->ivar;
    newInner->rangeLo = outer->rangeLo;
    newInner->rangeHi = outer->rangeHi;
    newInner->fn = fn;
    newInner->depth = outer->depth + 1;
    newInner->vectorizeHint = outer->vectorizeHint;
    newInner->body = inner->body;
    newInner->parent = newOuter;
    inner->body->parent = newInner;

    newOuter->body = newInner;
    list[i] = newOuter;
    ++applied;
    diag(Diag::Note, Code::Interchange,
         "interchanged a perfect rectangular loop nest (lines " +
         std::to_string(outer->loc.line) + "/" +
         std::to_string(inner->loc.line) + ", fn " + fn->name +
         ") — the strided inner traversal (" + std::to_string(v.strided) +
         " transposed access[es]) becomes contiguous after the swap",
         outer->loc,
         "kind=interchange,fn=" + fn->name +
         ",strided=" + std::to_string(v.strided));
    proofLogLine("interchange",
                 "R1:perfect-rectangular;R2:bounds-RT+total;"
                 "R3:iterations-independent;R4:no-observable-effects;"
                 "R5:bounds-read-nothing-body-writes;profit:strided=" +
                 std::to_string(v.strided),
                 fn, outer->loc);
  }
  return applied;
}

// ================================================================= Tile
// §6.7 LoopTile.  Perfect rectangular nest only; bounds must be provably
// inside [0, 2^62] so the synthesized ceil-shift arithmetic cannot wrap
// (interval proof — audit §4.4 discipline: no unprovable arithmetic).
// Tile edges are powers of two from the target profile (or --tile=I,J).
// Boundary tiles are EXACT: the inner high bound is imin(tileEnd, R).
static int tileInList(std::vector<Stmt*>& list, FnDecl* fn) {
  int applied = 0;
  for (size_t i = 0; i < list.size(); ++i) {
    Stmt* outer = list[i];
    if (!isSimpleRangeForT(outer)) continue;
    BodyInfo ib;
    Stmt* inner = perfectInner(outer, fn, &ib);
    if (!inner) continue;

    // interval proof that both ranges are within [0, 2^62]: keeps the
    // synthesized (R + T-1) >> k and __tile*T arithmetic exact
    auto safeRange = [](Stmt* loop) -> bool {
      Interval lo = intervalOf(loop->rangeLo);
      Interval hi = intervalOf(loop->rangeHi);
      if (!lo.known || !hi.known) return false;
      if (lo.lo < 0 || hi.hi > ((int64_t)1 << 62) || hi.lo < lo.lo) return false;
      return true;
    };
    if (!safeRange(outer) || !safeRange(inner)) continue;

    // tile sizes
    int TI = g_opts.tileI, TJ = g_opts.tileJ;
    if (TI <= 0 || TJ <= 0) {
      int64_t streams = (int64_t)(ib.reads.size() + ib.writes.size());
      profit::chooseTileEdges(streams < 1 ? 2 : streams, 8, &TI, &TJ);
    }
    // round to powers of two (the ceil-div synthesis needs it)
    auto pow2 = [](int x) {
      int p = 1;
      while (p < x) p <<= 1;
      return p;
    };
    TI = pow2(TI);
    TJ = pow2(TJ);
    int ki = 0, kj = 0;
    while ((1 << ki) < TI) ++ki;
    while ((1 << kj) < TJ) ++kj;

    Type* bt = outer->rangeHi->ty && outer->rangeHi->ty->isInt()
                   ? outer->rangeHi->ty
                   : Type::simple(Type::I64);
    Type* i64 = Type::simple(Type::I64);
    SrcLoc loc = outer->loc;

    // fresh tile induction variables (appended to the symbol table)
    std::string niName, njName;
    Sym* niSym = freshLoopVar(fn, "__tile_i", i64, &niName);
    Sym* njSym = freshLoopVar(fn, "__tile_j", i64, &njName);

    // nI = (R + TI-1) >> ki  (exact for R in [0, 2^62], power-of-two TI)
    Expr* R = cloneExpr(outer->rangeHi, {});
    Expr* nI = mkBin(">>",
                     mkBin("+", R, mkInt((uint64_t)(TI - 1), bt, loc), bt, loc),
                     mkInt((uint64_t)ki, bt, loc), bt, loc);
    Expr* C = cloneExpr(inner->rangeHi, {});
    Expr* nJ = mkBin(">>",
                     mkBin("+", C, mkInt((uint64_t)(TJ - 1), bt, loc), bt, loc),
                     mkInt((uint64_t)kj, bt, loc), bt, loc);

    auto tileLo = [&](Sym* sym, const std::string& name, int T) {
      return mkBin("*", mkIdent(name, sym, i64, loc),
                   mkInt((uint64_t)T, i64, loc), i64, loc);
    };
    auto tileHi = [&](Sym* sym, const std::string& name, int T, Expr* bound) {
      Expr* lo1 = tileLo(sym, name, T);
      Expr* end1 = mkBin("+", lo1, mkInt((uint64_t)T, i64, loc), i64, loc);
      return mkImin(end1, bound, i64, loc);
    };

    // innermost two loops keep the user's ivars and body (MOVED)
    Stmt* jLoop = new Stmt(Stmt::FOR, inner->loc);
    jLoop->forRange = true;
    jLoop->ivar = inner->ivar;
    jLoop->rangeLo = tileLo(njSym, njName, TJ);
    jLoop->rangeHi = tileHi(njSym, njName, TJ, cloneExpr(inner->rangeHi, {}));
    jLoop->fn = fn;
    jLoop->vectorizeHint = inner->vectorizeHint || outer->vectorizeHint;
    jLoop->body = inner->body;
    jLoop->body->parent = jLoop;

    Stmt* iLoop = new Stmt(Stmt::FOR, outer->loc);
    iLoop->forRange = true;
    iLoop->ivar = outer->ivar;
    iLoop->rangeLo = tileLo(niSym, niName, TI);
    iLoop->rangeHi = tileHi(niSym, niName, TI, cloneExpr(outer->rangeHi, {}));
    iLoop->fn = fn;
    iLoop->body = jLoop;
    jLoop->parent = iLoop;

    Stmt* jjLoop = new Stmt(Stmt::FOR, inner->loc);
    jjLoop->forRange = true;
    jjLoop->ivar = njName;
    jjLoop->rangeLo = mkInt(0, bt, loc);
    jjLoop->rangeHi = nJ;
    jjLoop->fn = fn;
    jjLoop->body = iLoop;
    iLoop->parent = jjLoop;

    Stmt* iiLoop = new Stmt(Stmt::FOR, outer->loc);
    iiLoop->forRange = true;
    iiLoop->ivar = niName;
    iiLoop->rangeLo = mkInt(0, bt, loc);
    iiLoop->rangeHi = nI;
    iiLoop->fn = fn;
    iiLoop->depth = outer->depth;
    iiLoop->body = jjLoop;
    jjLoop->parent = iiLoop;
    repairParentsT(iiLoop, outer->parent, fn);

    list[i] = iiLoop;
    ++applied;
    diag(Diag::Note, Code::Tile,
         "tiled a perfect rectangular nest (lines " +
         std::to_string(outer->loc.line) + "/" +
         std::to_string(inner->loc.line) + ", fn " + fn->name +
         ") into " + std::to_string(TI) + "x" + std::to_string(TJ) +
         " blocks — boundary tiles are imin-clamped so the iteration "
         "domain is exact; the tile working set targets half of L1 (" +
         std::to_string(profit::target().l1Bytes / 1024) + " KiB)",
         outer->loc,
         "kind=tile,fn=" + fn->name + ",TI=" + std::to_string(TI) +
         ",TJ=" + std::to_string(TJ));
    proofLogLine("tile",
                 "R1..R5 as interchange;bounds-in-[0,2^62]:proven;"
                 "domain:exact-imin-clamped-boundaries;TI=" +
                 std::to_string(TI) + ";TJ=" + std::to_string(TJ),
                 fn, outer->loc);
  }
  return applied;
}

// ========================================================== Distribution
// §6.8 LoopDistribution (fission).  Splits a loop into two loops over
// the same range when the statement groups are provably independent:
//   D1  no statement group's writes touch any root the other group
//       reads OR writes (flow/anti/output all covered);
//   D2  no unknown memory effects anywhere (nothing unplaceable);
//   D3  no observable effects (order of iterations across groups would
//       change otherwise);
//   D4  bounds are RT+total (they are evaluated twice after the split).
static bool groupsConflict(const BodyInfo& x, const BodyInfo& y) {
  if (x.unknownRead || x.unknownWrite || y.unknownRead || y.unknownWrite)
    return true;
  std::set<std::string> wx = writeRoots(x), wy = writeRoots(y);
  for (auto& w : wx)
    if (y.reads.count(w) || y.writes.count(w)) return true;
  for (auto& w : wy)
    if (x.reads.count(w) || x.writes.count(w)) return true;
  return false;
}

static int distributeInList(std::vector<Stmt*>& list, FnDecl* fn) {
  int applied = 0;
  for (size_t i = 0; i < list.size(); ++i) {
    Stmt* s = list[i];
    if (!isSimpleRangeForT(s)) continue;
    if (!isInt64KindT(rangeIvarTyT(s))) continue;
    auto* bodyList = stmtsOf(s->body);
    if (!bodyList || bodyList->size() < 2) continue;
    BodyInfo wb;
    collectBody(s->body, s->ivar, fn, &wb);
    if (!noObservableEffects(wb)) continue;               // D3
    if (wb.unknownRead || wb.unknownWrite) continue;     // D2
    if (wb.hasReturn || wb.hasBreak) continue;
    if (!exprTotalAndPure(s->rangeLo) || !exprTotalAndPure(s->rangeHi))
      continue;                                           // D4

    // profitability (§6.8): fission pays when the fused body is big
    // enough that register pressure / vectorisation granularity matters
    if (wb.nodes < 24) continue;

    std::vector<BodyInfo> per(bodyList->size());
    for (size_t k = 0; k < bodyList->size(); ++k)
      collectBody((*bodyList)[k], s->ivar, fn, &per[k]);

    // choose the split closest to the middle that is legal
    int best = -1;
    size_t n = bodyList->size();
    for (size_t p = 1; p < n; ++p) {
      bool ok = true;
      for (size_t a = 0; a < p && ok; ++a)
        for (size_t b = p; b < n; ++b)
          if (groupsConflict(per[a], per[b])) { ok = false; break; }
      if (!ok) continue;
      if (best < 0 || llabs((long long)(p * 2 - (long long)n)) <
                         llabs((long long)(best * 2 - (long long)n)))
        best = (int)p;
    }
    if (best < 1) continue;

    // ---- rewrite: two loops over the same range (bounds shared: D4)
    Stmt* block = new Stmt(Stmt::BLOCK, s->loc);
    block->fn = fn;
    block->parent = s->parent;
    for (int half = 0; half < 2; ++half) {
      Stmt* L = new Stmt(Stmt::FOR, s->loc);
      L->forRange = true;
      L->ivar = s->ivar;
      L->rangeLo = s->rangeLo;
      L->rangeHi = s->rangeHi;
      L->fn = fn;
      L->depth = s->depth;
      L->vectorizeHint = s->vectorizeHint;
      L->parent = block;
      Stmt* body = new Stmt(Stmt::BLOCK, s->loc);
      body->fn = fn;
      body->parent = L;
      size_t from = half == 0 ? 0 : (size_t)best;
      size_t to = half == 0 ? (size_t)best : n;
      for (size_t k = from; k < to; ++k) {
        (*bodyList)[k]->parent = body;
        (*bodyList)[k]->fn = fn;
        body->stmts.push_back((*bodyList)[k]);
      }
      L->body = body;
      block->stmts.push_back(L);
    }
    list[i] = block;
    ++applied;
    diag(Diag::Note, Code::Distribute,
         "distributed a " + std::to_string(n) + "-statement loop (line " +
         std::to_string(s->loc.line) + ", fn " + fn->name +
         ") into two independent loops (split before statement " +
         std::to_string(best + 1) + ") — no cross-group flow/anti/output "
         "dependence exists, so each phase can vectorise and schedule "
         "independently",
         s->loc,
         "kind=distribute,fn=" + fn->name + ",split=" + std::to_string(best));
    proofLogLine("distribute",
                 "D1:no-cross-group-dependence;D2:no-unknown-effects;"
                 "D3:no-observable-effects;D4:bounds-RT+total;"
                 "split=" + std::to_string(best),
                 fn, s->loc);
  }
  return applied;
}

// ======================================================== UnrollAndJam
// §6.9 UnrollAndJam (outer x2, inner jammed).  Perfect rectangular nest
// with CONSTANT outer trip in (16, 512].  Premises: R1..R5 (as
// interchange) plus
//   J1  every WRITE access in the inner body is affine in exactly one
//       induction variable (or a two-level i-then-j index), so the
//       copies' address functions never alias across the jam — combined
//       with R3 (reads disjoint from writes), the per-iteration
//       interleaving cannot reorder two effects on the same address;
//   J2  the outer trip is a known constant, so the main/remainder
//       partition is exact (same partition premise as partial unroll).
static int unrollJamInList(std::vector<Stmt*>& list, FnDecl* fn) {
  int applied = 0;
  for (size_t i = 0; i < list.size(); ++i) {
    Stmt* outer = list[i];
    if (!isSimpleRangeForT(outer)) continue;
    BodyInfo ib;
    Stmt* inner = perfectInner(outer, fn, &ib);
    if (!inner) continue;
    TripInfo ti = tripCountOf(outer);
    if (!ti.known || ti.empty || ti.saturated) continue;
    if (ti.trip <= 16 || ti.trip > 512) continue;
    if (writesIvarT(inner->body, inner->ivar)) continue;
    int64_t evenTrip = ti.trip - (ti.trip % 2);

    // J1: writes must be affine in one ivar (or 2-level i/j)
    bool jamSafe = true;
    {
      std::function<void(Stmt*)> walkS = [&](Stmt* s) {
        if (!s || !jamSafe) return;
        switch (s->k) {
          case Stmt::ASSIGN: {
            walkExprNodesT(s->lhs, [&](Expr* x) {
              if (!jamSafe || x->k != Expr::INDEX || !x->rhs) return;
              if (x->base && x->base->k == Expr::INDEX && x->base->rhs) {
                // two-level write root[e1][e2]: EITHER orientation is
                // jam-safe — the address is an injective function of the
                // (i, j) pair, so the two copies (i vs i+1) never hit the
                // same address except at equal j, where the jammed body
                // preserves the copy0-before-copy1 order
                Affine a1o = affineIn(x->base->rhs, outer->ivar);
                Affine a1i = affineIn(x->base->rhs, inner->ivar);
                Affine a2o = affineIn(x->rhs, outer->ivar);
                Affine a2i = affineIn(x->rhs, inner->ivar);
                bool e1o = a1o.valid && a1o.coef != 0;
                bool e1i = a1i.valid && a1i.coef != 0;
                bool e2o = a2o.valid && a2o.coef != 0;
                bool e2i = a2i.valid && a2i.coef != 0;
                bool rowMajor = e1o && e2i;
                bool transposed = e1i && e2o;
                if (!rowMajor && !transposed) jamSafe = false;
              } else {
                Affine io = affineIn(x->rhs, outer->ivar);
                Affine ii = affineIn(x->rhs, inner->ivar);
                bool o = io.valid && io.coef != 0;
                bool n = ii.valid && ii.coef != 0;
                if (!o && !n) jamSafe = false;   // not affine in either ivar
              }
            });
            break;
          }
          case Stmt::IF: walkS(s->thenB); walkS(s->elseB); break;
          case Stmt::WHILE: case Stmt::FOR: case Stmt::PARFOR:
            walkS(s->body);
            break;
          case Stmt::MATCH: for (auto& a : s->matchArms) walkS(a.body); break;
          case Stmt::BLOCK: for (auto* st : s->stmts) walkS(st); break;
          default: break;
        }
      };
      walkS(inner->body);
    }
    if (!jamSafe) continue;

    TypedConst loC;
    if (!evalConstTyped(outer->rangeLo, &loC)) continue;
    __int128 loM = loC.isSigned ? (__int128)loC.signedVal()
                                : (__int128)loC.unsignedVal();
    Type* oTy = rangeIvarTyT(outer);
    Sym* iSym = ivarSymOfT(inner->body, outer->ivar);
    SrcLoc loc = outer->loc;

    // substitution for the two unrolled copies: the outer ivar is reused
    // as the block index t in [0, evenTrip/2); copy 0 executes iteration
    // lo + t*2, copy 1 executes lo + t*2 + 1 — the union is exactly
    // [lo, lo+evenTrip), each value once (same partition premise as
    // partial unroll; lo is added back explicitly so non-zero lower
    // bounds stay exact — found by the §10.3 property tests)
    std::map<Sym*, Expr*> sub0, sub1;
    if (iSym) {
      Expr* t0 = mkIdent(outer->ivar, iSym, oTy, loc);
      Expr* v0 = mkBin("+", mkBin("*", t0, mkInt(2, oTy, loc), oTy, loc),
                       mkInt((uint64_t)loM, oTy, loc), oTy, loc);
      sub0[iSym] = v0;
      Expr* t1 = mkIdent(outer->ivar, iSym, oTy, loc);
      Expr* v1 = mkBin("+", mkBin("*", t1, mkInt(2, oTy, loc), oTy, loc),
                       mkInt((uint64_t)(loM + 1), oTy, loc), oTy, loc);
      sub1[iSym] = v1;
    }

    // ---- main loop [lo, lo+evenTrip): jammed inner loop
    Stmt* jammedInner = new Stmt(Stmt::FOR, inner->loc);
    jammedInner->forRange = true;
    jammedInner->ivar = inner->ivar;
    jammedInner->rangeLo = inner->rangeLo;    // shared (RT+total: R2)
    jammedInner->rangeHi = inner->rangeHi;
    jammedInner->fn = fn;
    jammedInner->vectorizeHint = inner->vectorizeHint ||
                                 outer->vectorizeHint;
    Stmt* jamBody = new Stmt(Stmt::BLOCK, inner->loc);
    jamBody->fn = fn;
    jamBody->parent = jammedInner;
    jamBody->stmts.push_back(cloneStmt(inner->body, sub0));
    jamBody->stmts.push_back(cloneStmt(inner->body, sub1));
    jammedInner->body = jamBody;

    Stmt* mainL = new Stmt(Stmt::FOR, loc);
    mainL->forRange = true;
    mainL->ivar = outer->ivar;   // reused as the block index t in [0, n/2)
    mainL->rangeLo = mkInt(0, oTy, loc);
    mainL->rangeHi = mkInt((uint64_t)(evenTrip / 2), oTy, loc);
    mainL->fn = fn;
    mainL->depth = outer->depth;
    mainL->vectorizeHint = false;   // jammed body, not the outer loop
    mainL->body = jammedInner;
    jammedInner->parent = mainL;
    repairParentsT(mainL, outer->parent, fn);

    std::vector<Stmt*> replacement;
    replacement.push_back(mainL);
    if (evenTrip != ti.trip) {
      Stmt* remL = new Stmt(Stmt::FOR, loc);
      remL->forRange = true;
      remL->ivar = outer->ivar;
      remL->rangeLo = mkInt((uint64_t)(loM + evenTrip), oTy, loc);
      remL->rangeHi = outer->rangeHi;
      remL->fn = fn;
      remL->depth = outer->depth;
      remL->body = cloneStmt(inner, {});
      // inner->body is MOVED into jamBody clones; remainder gets a clone
      repairParentsT(remL, outer->parent, fn);
      replacement.push_back(remL);
    }

    list[i] = replacement[0];
    if (replacement.size() > 1)
      list.insert(list.begin() + i + 1, replacement[1]);
    ++applied;
    diag(Diag::Note, Code::UnrollJam,
         "unroll-and-jammed a perfect nest (lines " +
         std::to_string(outer->loc.line) + "/" +
         std::to_string(inner->loc.line) + ", fn " + fn->name +
         ") — the outer constant " + std::to_string(ti.trip) +
         "-trip loop runs 2 iterations per step and the inner loops are "
         "jammed into one, reusing each row across adjacent iterations",
         outer->loc,
         "kind=unroll-and-jam,fn=" + fn->name +
         ",trip=" + std::to_string(ti.trip));
    proofLogLine("unroll-and-jam",
                 "R1..R5 as interchange;J1:writes-affine-single-ivar;"
                 "J2:constant-trip-partition:" +
                 std::to_string(evenTrip) + "+1",
                 fn, outer->loc);
  }
  return applied;
}

// ================================================================ driver
LoopXformCounts runLoopTransformPasses(Program* p) {
  computeFootprints(p);
  LoopXformCounts c;
  // refresh the target profile from CLI overrides
  profit::TargetProfile& tp = profit::target();
  if (g_opts.targetCacheLine > 0) tp.cacheLine = g_opts.targetCacheLine;
  if (g_opts.targetL1 > 0) tp.l1Bytes = g_opts.targetL1;
  if (g_opts.targetL2 > 0) tp.l2Bytes = g_opts.targetL2;
  if (g_opts.targetVectorWidth > 0) tp.vectorWidth = g_opts.targetVectorWidth;

  for (auto* fn : p->fns) {
    if (!fn->body || fn->isGeneric) continue;
    // Order rationale (audit §9 pipeline + Phase D dependency analysis):
    //   LICM first (hoist before anything duplicates bodies), then the
    //   STRUCTURAL reshapes that need the original perfect nest
    //   (interchange, tiling, unroll-and-jam), then first-iteration
    //   peeling (needs the original guard shape), and only then partial
    //   unrolling (which rewrites nests into main+remainder pairs and
    //   would hide the shapes the reshapes look for) and distribution.
    int licmBudget = 8;   // code-motion bound per function
    if (g_opts.licmOn)
      walkListsT(fn->body, fn, [&](std::vector<Stmt*>& list) {
        c.licm += licmInList(list, fn, &licmBudget);
      });
    if (g_opts.interchangeOn)
      walkListsT(fn->body, fn, [&](std::vector<Stmt*>& list) {
        c.interchange += interchangeInList(list, fn);
      });
    if (g_opts.tileOn)
      walkListsT(fn->body, fn, [&](std::vector<Stmt*>& list) {
        c.tile += tileInList(list, fn);
      });
    if (g_opts.unrollJamOn)
      walkListsT(fn->body, fn, [&](std::vector<Stmt*>& list) {
        c.unrollJam += unrollJamInList(list, fn);
      });
    if (g_opts.peelOn)
      walkListsT(fn->body, fn, [&](std::vector<Stmt*>& list) {
        c.peel += peelInList(list, fn);
      });
    if (g_opts.partialUnrollOn)
      walkListsT(fn->body, fn, [&](std::vector<Stmt*>& list) {
        c.partialUnroll += partialUnrollInList(list, fn);
      });
    if (g_opts.distributeOn)
      walkListsT(fn->body, fn, [&](std::vector<Stmt*>& list) {
        c.distribute += distributeInList(list, fn);
      });
  }
  return c;
}

} // namespace freshet
