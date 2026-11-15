// Freshet — recursive base-case inlining, ported from Goose (aardappel/
// goose, src/optimize_basecase.h, BaseCaseInliner, SetupLeading form only).
//
// Goose's optimizer offers every non-inlined call the base case of a
// self-recursive function whose body STARTS with `if c { return e; }`
// (c and e reading nothing but the parameters, globals and their own
// bindings): the call is rewritten to
//
//     { let p1 = a1; ...; if c[p := t] { return e[p := t]; }
//       return f(t...); }
//
// so the leaf call of a recursion tree returns without building a frame —
// half the calls of a complete tree walk disappear.  Freshet ports the
// SetupLeading form as a source-level rewrite of every RETURN statement
// whose value is exactly a direct self-call (goose rewrites the call
// expression wherever it appears; Freshet's `if` is a statement, not an
// expression, so only return position is sound here).
//
// LEGALITY PREMISES (checked before rewriting; they also go into the
// --proof-log line):
//   P1  f is directly self-recursive: some CALL in the body has
//       callee == f and no enumRef (an instantiated generic copy is a
//       separate FnDecl and recurses on itself, not the template);
//   P2  the body is a BLOCK whose first statement is `if c { return e; }`
//       — elseB == nullptr, thenB a RETURN (its retVal e may be null for
//       a bare `return;`) or a BLOCK with exactly that one statement;
//   P3  BaseOK(c) and BaseOK(e): the subtrees contain ONLY int/float/
//       bool/str literals, IDENTs resolving to a PARAM of f (sym->k ==
//       Sym::PARAM && sym->fn == f) or a global (globalRef), BIN, UN,
//       CAST, INDEX (reads) and FIELD (reads) — no calls (a call could
//       observe the parameters or diverge), no match/try/slice/array-/
//       struct-literal expressions, no fn values, no locals (nothing
//       else of the callee is in scope at the call site);
//   P4  gates: nodes(c) + nodes(e) < 100, self-call sites <= 8 and
//       sites * size < 800 (goose's inliner size rule — the base case is
//       duplicated once per site), and the body contains no DEFER
//       anywhere (a defer is bound to the block it is written in, which
//       the copy is not in);
//   P5  no give() anywhere at the rewritten sites (any argGive flag on
//       the call) and no owned parameters (ownership must not move into
//       an immutable re-binding).
//
// SOUNDNESS: the bindings are evaluated exactly where the call's
// arguments were (immutable LETs, in argument order, at the statement
// slot the RETURN sits in), and nothing runs between them and c/e — the
// original callee would have executed nothing before its first statement
// `if c { return e; }` either — so c and e see the same state the callee
// would have, and the recursing arm passes the very bindings on, so each
// argument is evaluated exactly once, before the test, whichever arm
// runs.
//
// The fresh __bc_p* bindings are appended to the function's sema symbol
// table exactly like the tiling pass's induction variables, so IRgen
// materialises their allocas; synthesized nodes carry full sema fields
// and introduce no other bindings.
#include "common.h"
#include "opt_util.h"

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace freshet {
namespace {

using namespace optutil;

// ------------------------------------------------------------ walkers
static void bcWalkExpr(Expr* e, const std::function<void(Expr*)>& f) {
  if (!e) return;
  f(e);
  for (auto* a : e->args) bcWalkExpr(a, f);
  bcWalkExpr(e->lhs, f);
  bcWalkExpr(e->rhs, f);
  bcWalkExpr(e->base, f);
}

static void bcWalkStmt(Stmt* s, const std::function<void(Expr*)>& f) {
  if (!s) return;
  switch (s->k) {
    case Stmt::LET: bcWalkExpr(s->init, f); break;
    case Stmt::ASSIGN: bcWalkExpr(s->lhs, f); bcWalkExpr(s->rhs, f); break;
    case Stmt::RETURN: bcWalkExpr(s->retVal, f); break;
    case Stmt::DEFER: bcWalkExpr(s->deferExpr, f); break;
    case Stmt::EXPR: bcWalkExpr(s->expr, f); break;
    case Stmt::IF:
      bcWalkExpr(s->cond, f);
      bcWalkStmt(s->thenB, f);
      bcWalkStmt(s->elseB, f);
      break;
    case Stmt::WHILE:
      bcWalkExpr(s->cond, f);
      bcWalkStmt(s->body, f);
      break;
    case Stmt::FOR:
      bcWalkExpr(s->forInit, f);
      bcWalkExpr(s->forCond, f);
      bcWalkExpr(s->forStep, f);
      bcWalkExpr(s->rangeLo, f);
      bcWalkExpr(s->rangeHi, f);
      bcWalkStmt(s->body, f);
      break;
    case Stmt::PARFOR:
      bcWalkExpr(s->plo, f);
      bcWalkExpr(s->phi, f);
      for (auto& c : s->clauses) bcWalkExpr(c.e, f);
      bcWalkStmt(s->body, f);
      break;
    case Stmt::MATCH:
      bcWalkExpr(s->matchOn, f);
      for (auto& a : s->matchArms) bcWalkStmt(a.body, f);
      break;
    case Stmt::BLOCK:
      for (auto* st : s->stmts) bcWalkStmt(st, f);
      bcWalkExpr(s->expr, f);
      break;
    default: break;
  }
}

static int bcCountNodes(Expr* e) {
  int n = 0;
  bcWalkExpr(e, [&](Expr*) { ++n; });
  return n;
}

// direct self-call CALL nodes anywhere in the subtree (callee == f, no
// enumRef) — goose's SelfSites
static int bcSelfCalls(Stmt* s, FnDecl* f) {
  int n = 0;
  bcWalkStmt(s, [&](Expr* x) {
    if (x->k == Expr::CALL && x->callee == f && !x->enumRef) ++n;
  });
  return n;
}

static bool bcHasDefer(Stmt* s) {
  bool has = false;
  std::function<void(Stmt*)> walk = [&](Stmt* st) {
    if (!st || has) return;
    if (st->k == Stmt::DEFER) has = true;
    switch (st->k) {
      case Stmt::IF: walk(st->thenB); walk(st->elseB); break;
      case Stmt::MATCH:
        for (auto& a : st->matchArms) walk(a.body);
        break;
      case Stmt::WHILE: case Stmt::FOR: case Stmt::PARFOR:
        walk(st->body);
        break;
      case Stmt::BLOCK:
        for (auto* x : st->stmts) walk(x);
        break;
      default: break;
    }
  };
  walk(s);
  return has;
}

// P3: the subtree reads only params of f, globals and literals — no
// calls (a call could observe the parameters or diverge), no fn values,
// no locals, no composite constructors, nothing that jumps out.
// Params are matched by NAME + Sym::PARAM: sema does not set Sym::fn on
// parameter symbols, and Freshet has no nested functions, so a PARAM
// symbol referenced inside f's body is one of f's own.
static bool bcIsParamOf(const std::string& name, FnDecl* f) {
  for (auto& p : f->params)
    if (p.name == name) return true;
  return false;
}

static bool bcBaseOK(Expr* e, FnDecl* f) {
  if (!e) return true;
  switch (e->k) {
    case Expr::INT: case Expr::FLT: case Expr::BOOL: case Expr::STR:
      return true;
    case Expr::IDENT:
      return (e->sym && e->sym->k == Sym::PARAM &&
              bcIsParamOf(e->sym->name, f)) ||
             (e->globalRef != nullptr);
    case Expr::BIN:
      return bcBaseOK(e->lhs, f) && bcBaseOK(e->rhs, f);
    case Expr::UN:
      return bcBaseOK(e->rhs, f);
    case Expr::CAST:
      return bcBaseOK(e->base, f);
    case Expr::INDEX:    // reads (an index expression is a load)
      return bcBaseOK(e->base, f) && bcBaseOK(e->rhs, f);
    case Expr::FIELD:    // reads
      return bcBaseOK(e->base, f);
    default:
      return false;
  }
}

static void bcRepairParents(Stmt* s, Stmt* parent, FnDecl* fn) {
  if (!s) return;
  s->parent = parent;
  s->fn = fn;
  for (auto* child : s->stmts) bcRepairParents(child, s, fn);
  bcRepairParents(s->body, s, fn);
  bcRepairParents(s->thenB, s, fn);
  bcRepairParents(s->elseB, s, fn);
  for (auto& arm : s->matchArms) bcRepairParents(arm.body, s, fn);
}

// fresh binding appended to the function's sema symbol table so IRgen
// emits its alloca (loop_transforms.cpp freshLoopVar conventions)
static Sym* bcFreshVar(FnDecl* fn, const std::string& base, Type* ty,
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

// ================================================================ pass
static int bcRewriteFn(FnDecl* f) {
  if (!f->body) return 0;
  FnSema* fs = semaForFn(f);
  if (!fs) return 0;

  // P5: owned params — ownership must not move into a re-binding
  for (auto& p : f->params)
    if (p.owned) return 0;

  // P1: directly self-recursive
  int sites = bcSelfCalls(f->body, f);
  if (sites <= 0) return 0;

  // P4: a defer is bound to the block it is written in, which a copy of
  // the base case into a call site is not in
  if (bcHasDefer(f->body)) return 0;

  // P2: the body starts with `if c { return e; }` (no else)
  if (f->body->k != Stmt::BLOCK || f->body->stmts.empty()) return 0;
  Stmt* ife = f->body->stmts[0];
  if (!ife || ife->k != Stmt::IF || ife->elseB) return 0;
  Stmt* ret = nullptr;
  if (ife->thenB && ife->thenB->k == Stmt::RETURN) {
    ret = ife->thenB;
  } else if (ife->thenB && ife->thenB->k == Stmt::BLOCK &&
             ife->thenB->stmts.size() == 1 && !ife->thenB->expr &&
             ife->thenB->stmts[0] &&
             ife->thenB->stmts[0]->k == Stmt::RETURN) {
    ret = ife->thenB->stmts[0];
  }
  if (!ret) return 0;
  Expr* c = ife->cond;
  Expr* e = ret->retVal;             // may be null: bare `return;`
  if (!c) return 0;

  // P3: c and e read only params of f, globals and literals
  if (!bcBaseOK(c, f) || !bcBaseOK(e, f)) return 0;

  // P4: gates (goose inliner size rule against what the rewrite copies)
  int size = bcCountNodes(c) + bcCountNodes(e);
  if (size >= 100) return 0;
  if (sites > 8) return 0;
  if ((int64_t)sites * (int64_t)size >= 800) return 0;

  // ---- the parameters' stable Syms (sema declares them into ownedSyms;
  // allSyms is the by-value index; Sym::fn is NOT set on params — match
  // by name).  The substitution replaces IDENTs by Sym*, so every param c
  // or e names must resolve to one of these.
  std::vector<Sym*> paramSym(f->params.size(), nullptr);
  for (auto& os : fs->ownedSyms) {
    if (os->k != Sym::PARAM) continue;
    for (size_t i = 0; i < f->params.size(); ++i)
      if (f->params[i].name == os->name) paramSym[i] = os.get();
  }
  for (size_t i = 0; i < f->params.size(); ++i) {
    if (!paramSym[i]) {
      // not found as a stable sym: only usable if c/e never names it
      bool named = false;
      auto check = [&](Expr* x) {
        if (named || x->k != Expr::IDENT || !x->sym) return;
        if (x->sym->k == Sym::PARAM && x->sym->name == f->params[i].name)
          named = true;
      };
      bcWalkExpr(c, check);
      bcWalkExpr(e, check);
      if (named) return 0;          // P3: unresolvable param — bail
    }
  }

  // ---- rewrite every RETURN slot whose value is exactly a direct
  // self-call f(a1..an) with no give() on any argument
  int applied = 0;
  std::function<void(Stmt*&)> rewriteSlot = [&](Stmt*& slot) {
    if (!slot) return;
    // a qualifying site?
    bool isSite = slot->k == Stmt::RETURN && slot->retVal &&
                  slot->retVal->k == Expr::CALL &&
                  slot->retVal->callee == f && !slot->retVal->enumRef &&
                  slot->retVal->args.size() == f->params.size();
    if (isSite) {
      Expr* call = slot->retVal;
      bool gives = false;
      for (bool g : call->argGive) gives = gives || g;   // P5: no give()
      if (!gives) {
        SrcLoc loc = slot->loc;
        // let __bc_p1 = a1; ... let __bc_pn = an;   (immutable, arg order)
        std::vector<Stmt*> lets;
        std::map<Sym*, Expr*> subst;
        std::vector<Expr*> argIdents;
        for (size_t i = 0; i < f->params.size(); ++i) {
          std::string nm;
          Sym* bSym = bcFreshVar(f, "__bc_p" + std::to_string(i + 1),
                                 f->params[i].ty, &nm);
          Stmt* L = new Stmt(Stmt::LET, loc);
          L->var = nm;
          L->annTy = f->params[i].ty;
          L->hasAnn = true;
          L->mut = false;
          L->init = call->args[i];       // moved: the call re-binds below
          L->fn = f;
          lets.push_back(L);
          Expr* id = new Expr(Expr::IDENT, loc);
          id->name = nm;
          id->ty = f->params[i].ty;
          id->sym = bSym;
          argIdents.push_back(id);
          if (paramSym[i]) subst[paramSym[i]] = id;
        }
        // if (c[p := __bc_p...]) { return e[p := __bc_p...]; }
        Stmt* guard = new Stmt(Stmt::IF, loc);
        guard->cond = cloneExpr(c, subst);
        guard->fn = f;
        Stmt* thenB = new Stmt(Stmt::BLOCK, loc);
        thenB->fn = f;
        Stmt* baseRet = new Stmt(Stmt::RETURN, ret->loc);
        baseRet->retVal = e ? cloneExpr(e, subst) : nullptr;  // bare return
        baseRet->fn = f;
        thenB->stmts.push_back(baseRet);
        guard->thenB = thenB;
        guard->elseB = nullptr;
        // return f(__bc_p1, ..., __bc_pn);   (the original call node,
        // re-bound to the bindings — goose re-uses the Call node too)
        call->args = argIdents;
        Stmt* recRet = new Stmt(Stmt::RETURN, loc);
        recRet->retVal = call;
        recRet->fn = f;
        // splice: { lets...; if ...; return f(...); }
        Stmt* blk = new Stmt(Stmt::BLOCK, loc);
        blk->fn = f;
        for (auto* L : lets) blk->stmts.push_back(L);
        blk->stmts.push_back(guard);
        blk->stmts.push_back(recRet);
        bcRepairParents(blk, slot->parent, f);
        slot = blk;
        ++applied;
        diag(Diag::Note, Code::Basecase,
             "recursive base case inlined at call site — the leaf call "
             "returns without a frame",
             loc, "kind=basecase");
        proofLogLine("basecase",
                     "leading-if-base;cond-ret-read-params-globals-only;"
                     "args-bound-once-in-order;no-give;no-defer",
                     f, loc);
        return;         // never descend into the replacement
      }
    }
    // recurse into child slots
    switch (slot->k) {
      case Stmt::IF:
        rewriteSlot(slot->thenB);
        rewriteSlot(slot->elseB);
        break;
      case Stmt::MATCH:
        for (auto& arm : slot->matchArms) rewriteSlot(arm.body);
        break;
      case Stmt::WHILE: case Stmt::FOR: case Stmt::PARFOR:
        rewriteSlot(slot->body);
        break;
      case Stmt::BLOCK:
        for (auto*& st : slot->stmts) rewriteSlot(st);
        break;
      default: break;
    }
  };
  rewriteSlot(f->body);
  return applied;
}

}  // namespace

int runBasecasePass(Program* p) {
  int total = 0;
  for (auto* f : p->fns) {
    if (!f->body || f->imported || f->isGeneric) continue;
    total += bcRewriteFn(f);
  }
  return total;
}

}  // namespace freshet
