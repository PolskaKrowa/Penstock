// Freshet — bit-identical blocked float sum reductions, ported from Goose
// (aardappel/goose, src/optimize_loops.h, LoopShaper::ShapeSum).
//
// Goose's sumred recognises a loop whose body is nothing but `s += e` on a
// float local `s` that `e` neither reads nor writes, and restates it so the
// C compiler's vectoriser can run: a block of term computations first
// (independent of each other and of the additions), then the additions to
// `s` one by one in the ORIGINAL order.  Floating-point addition is not
// associative, so the loop cannot be re-associated — but it does not have
// to be: every result stays bit for bit identical because the chain of
// additions to `s` is unchanged, and the terms are the same values however
// early they are computed.  Freshet ports the idea as a source-level
// rewrite (goose only marks the loop and lets codegen restate it):
//
//     for (i in lo..hi) { s += e; }
//   becomes
//     let __sr_trip: i64 = hi - lo;      // i64 WRAPPING arithmetic — the
//     let __sr_full: i64 = (__sr_trip / 8) * 8;   // control plane is ALWAYS
//     let __sr_fpos: i64 = imax(__sr_full, 0);    // i64, whatever the loop's
//     if (__sr_full > 0) {                         // own types are
//         for (__sr_j in 0 .. __sr_trip / 8) {
//             let __sr_t0: FT = e[i := lo + __sr_j*8 + 0];
//             ... through __sr_t7 ...
//             s = s + __sr_t0; ... s = s + __sr_t7;   // original order
//         }
//     }
//     for (i in lo + __sr_fpos .. hi) { s += e; }     // identical tail
//
// IRgen lowers a range loop with an i64 SIGNED compare whatever the bound
// types are, so the i64 control plane mirrors the loop's own iteration
// arithmetic exactly: hi - lo wraps as the i stepping would, and the guard
// `__sr_full > 0` (signed) is true exactly when the wrapped trip is a
// positive i64.
//
// LEGALITY PREMISES (all checked before rewriting; they also go into the
// --proof-log line):
//   P1  the accumulator `s` is a LOCAL f32/f64 binding (Sym::VAR/PARAM —
//       globals resolve through globalRef and never match), and its Sym*
//       is not the loop's induction variable;
//   P2  no `&s` (unary "&" on an IDENT with s's Sym*) anywhere in the
//       function — the only way a scalar local's storage can escape, so
//       with P3 nothing can observe the additions to s but the program's
//       own subsequent reads of s;
//   P3  e does not reference s (no IDENT with s's Sym* anywhere in e).
//       e MAY read/write memory and call functions: terms still evaluate
//       in the same i-order, and only the additions to s — which e cannot
//       observe — move.  An aborting term aborts at the same term either
//       way (the partial sum it leaves is a local nothing runs after an
//       abort), so every observable result is bit for bit identical;
//   P4  the loop is a plain ascending unit-step range loop (forRange, no
//       `by` step) whose bounds are 64-bit integers (i64/u64/usize — IRgen
//       lowers every range loop in i64, and the synthesized control plane
//       and index arithmetic are built in i64 to match); all same-named
//       references to the ivar inside e agree on one Sym*;
//   P5  the trip cannot wrap into a false positive: IRgen's loop compare
//       is SIGNED, so the only unsound corner is lo - hi > 2^63 (the
//       wrapped trip turns positive while the original loop runs zero
//       iterations).  Sound when lo is the INT constant 0 (then
//       lo - hi = -hi <= 2^63 always), or when both bounds have known
//       intervals with max(lo) - min(hi) <= 2^63.  Otherwise rejected.
//
// The fresh bindings (__sr_trip/full/fpos, __sr_j, __sr_t0..7) are
// appended to the function's sema symbol table exactly like the tiling
// pass's tile induction variables, so IRgen materialises their allocas;
// synthesized nodes carry full sema fields (types, syms, constants) and
// introduce no other bindings.  K = 8 is fixed (goose's vector register
// lane count; one 512/256-bit vector of f64/f32 terms per block).
#include "common.h"
#include "opt_util.h"
#include "loop_analysis.h"

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace freshet {
namespace {

using namespace optutil;

// the block size K (goose: one vector register of terms)
static constexpr int SR_K = 8;

// ------------------------------------------------------- node builders
// synthesized nodes carry full sema fields (ty, sym, constant) so IRgen
// lowers them like parser-produced ones; they introduce NO bindings
// except srFreshVar below (loop_transforms.cpp conventions).
static Expr* srInt(uint64_t v, Type* t, SrcLoc loc) {
  Expr* e = new Expr(Expr::INT, loc);
  e->ival = v;
  e->ty = t;
  e->constant = true;
  return e;
}

static Expr* srIdent(const std::string& name, Sym* sym, Type* t,
                     SrcLoc loc) {
  Expr* e = new Expr(Expr::IDENT, loc);
  e->name = name;
  e->sym = sym;
  e->ty = t;
  return e;
}

static Expr* srBin(const std::string& op, Expr* a, Expr* b, Type* t,
                   SrcLoc loc) {
  Expr* e = new Expr(Expr::BIN, loc);
  e->op = op;
  e->lhs = a;
  e->rhs = b;
  e->ty = t;
  return e;
}

// imax(a, b) — IRgen lowers the two-integer-argument call natively
// (icmp + select); no callee is involved.
static Expr* srImax(Expr* a, Expr* b, Type* t, SrcLoc loc) {
  Expr* e = new Expr(Expr::CALL, loc);
  e->name = "imax";
  e->args.push_back(a);
  e->args.push_back(b);
  e->argGive.push_back(false);
  e->argGive.push_back(false);
  e->ty = t;
  return e;
}

// ------------------------------------------------------------ walkers
static void srWalkExpr(Expr* e, const std::function<void(Expr*)>& f) {
  if (!e) return;
  f(e);
  for (auto* a : e->args) srWalkExpr(a, f);
  srWalkExpr(e->lhs, f);
  srWalkExpr(e->rhs, f);
  srWalkExpr(e->base, f);
}

static void srWalkStmt(Stmt* s, const std::function<void(Expr*)>& f) {
  if (!s) return;
  switch (s->k) {
    case Stmt::LET: srWalkExpr(s->init, f); break;
    case Stmt::ASSIGN: srWalkExpr(s->lhs, f); srWalkExpr(s->rhs, f); break;
    case Stmt::RETURN: srWalkExpr(s->retVal, f); break;
    case Stmt::DEFER: srWalkExpr(s->deferExpr, f); break;
    case Stmt::EXPR: srWalkExpr(s->expr, f); break;
    case Stmt::IF:
      srWalkExpr(s->cond, f);
      srWalkStmt(s->thenB, f);
      srWalkStmt(s->elseB, f);
      break;
    case Stmt::WHILE:
      srWalkExpr(s->cond, f);
      srWalkStmt(s->body, f);
      break;
    case Stmt::FOR:
      srWalkExpr(s->forInit, f);
      srWalkExpr(s->forCond, f);
      srWalkExpr(s->forStep, f);
      srWalkExpr(s->rangeLo, f);
      srWalkExpr(s->rangeHi, f);
      srWalkStmt(s->body, f);
      break;
    case Stmt::PARFOR:
      srWalkExpr(s->plo, f);
      srWalkExpr(s->phi, f);
      for (auto& c : s->clauses) srWalkExpr(c.e, f);
      srWalkStmt(s->body, f);
      break;
    case Stmt::MATCH:
      srWalkExpr(s->matchOn, f);
      for (auto& a : s->matchArms) srWalkStmt(a.body, f);
      break;
    case Stmt::BLOCK:
      for (auto* st : s->stmts) srWalkStmt(st, f);
      srWalkExpr(s->expr, f);
      break;
    default: break;
  }
}

static void srRepairParents(Stmt* s, Stmt* parent, FnDecl* fn) {
  if (!s) return;
  s->parent = parent;
  s->fn = fn;
  for (auto* child : s->stmts) srRepairParents(child, s, fn);
  srRepairParents(s->body, s, fn);
  srRepairParents(s->thenB, s, fn);
  srRepairParents(s->elseB, s, fn);
  for (auto& arm : s->matchArms) srRepairParents(arm.body, s, fn);
}

// fresh binding appended to the function's sema symbol table so IRgen
// emits its alloca (allocas are name-keyed from fs->allSyms); the
// returned heap Sym only annotates synthesized IDENTs (IRgen resolves
// by name through varAllocas).  Same convention as loop_transforms.cpp's
// freshLoopVar.
static Sym* srFreshVar(FnDecl* fn, const std::string& base, Type* ty,
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

// visit every statement list (BLOCK stmts) in the subtree — the only
// slots a rewrite can splice into (loop_transforms.cpp walkListsT)
static void srWalkLists(Stmt* s, FnDecl* fn,
                        const std::function<void(std::vector<Stmt*>&)>& v) {
  if (!s) return;
  if (s->k == Stmt::BLOCK) {
    v(s->stmts);
    for (auto* st : s->stmts) srWalkLists(st, fn, v);
    return;
  }
  switch (s->k) {
    case Stmt::IF:
      srWalkLists(s->thenB, fn, v);
      srWalkLists(s->elseB, fn, v);
      break;
    case Stmt::MATCH:
      for (auto& arm : s->matchArms) srWalkLists(arm.body, fn, v);
      break;
    case Stmt::WHILE: case Stmt::FOR: case Stmt::PARFOR:
      srWalkLists(s->body, fn, v);
      break;
    default: break;
  }
}

// ================================================================ match
// Recognise `for (i in lo..hi) { s += e; }` / `for (i in lo..hi) { s = s
// + e; }` (single statement, either bare or in a one-statement BLOCK),
// prove P1-P5 and rewrite the loop in place inside `list`.
static int srInList(std::vector<Stmt*>& list, FnDecl* fn) {
  int applied = 0;
  for (size_t i = 0; i < list.size(); ++i) {
    Stmt* S = list[i];
    if (!S || S->k != Stmt::FOR || !S->forRange) continue;
    if (S->forStep) continue;              // P4: unit-step ascending range
    if (S->ivar.empty() || !S->rangeLo || !S->rangeHi) continue;
    if (!S->body) continue;

    // ---- the single accumulating statement
    Stmt* a = nullptr;
    if (S->body->k == Stmt::ASSIGN) a = S->body;
    else if (S->body->k == Stmt::BLOCK && S->body->stmts.size() == 1 &&
             !S->body->expr)
      a = S->body->stmts[0];
    if (!a || a->k != Stmt::ASSIGN) continue;

    Sym* s = nullptr;      // the accumulator
    Expr* e = nullptr;     // the accumulated term
    if (a->op2 == "+=") {
      if (!a->lhs || a->lhs->k != Expr::IDENT || !a->lhs->sym) continue;
      s = a->lhs->sym;
      e = a->rhs;
    } else if (a->op2 == "=") {
      Expr* r = a->rhs;
      if (!r || r->k != Expr::BIN || r->op != "+") continue;
      if (!a->lhs || a->lhs->k != Expr::IDENT || !a->lhs->sym) continue;
      if (!r->lhs || r->lhs->k != Expr::IDENT || !r->lhs->sym ||
          r->lhs->sym != a->lhs->sym)
        continue;                          // `s = s + e`, same Sym both sides
      s = a->lhs->sym;
      e = r->rhs;
    } else {
      continue;
    }
    if (!e || !s->ty) continue;

    // ---- P1: local float accumulator, not the loop variable
    if (!(s->ty->k == Type::F32 || s->ty->k == Type::F64)) continue;
    if (s->k != Sym::VAR && s->k != Sym::PARAM) continue;

    // ---- the induction variable's Sym*, from references inside e (all
    // must share one Sym*; none is fine too — constant terms)
    std::set<Sym*> ivRefs;
    srWalkExpr(e, [&](Expr* x) {
      if (x->k == Expr::IDENT && x->name == S->ivar && x->sym)
        ivRefs.insert(x->sym);
    });
    if (ivRefs.size() > 1) continue;       // conflicting Syms — bail
    Sym* ivSym = ivRefs.empty() ? nullptr : *ivRefs.begin();
    if (ivSym == s) continue;              // P1: s is not the ivar

    // ---- P4: 64-bit integer bounds (IRgen lowers ranges in i64; the
    // synthesized control plane + index arithmetic are built in i64)
    if (!S->rangeLo->ty || !S->rangeHi->ty) continue;
    auto bits64 = [](Type* t) {
      return t && t->isInt() && t->bits() == 64;
    };
    if (!bits64(S->rangeLo->ty) || !bits64(S->rangeHi->ty)) continue;

    // ---- P5: the trip cannot wrap into a false positive (the loop
    // compare is signed; unsound only when lo - hi > 2^63)
    {
      bool sound = false;
      if (S->rangeLo->k == Expr::INT && S->rangeLo->constant &&
          S->rangeLo->ival == 0) {
        sound = true;                      // -hi <= 2^63 always
      } else {
        Interval loi = intervalOf(S->rangeLo);
        Interval hii = intervalOf(S->rangeHi);
        if (loi.known && hii.known) {
          __int128 dMax = (__int128)loi.hi - (__int128)hii.lo;
          sound = dMax <= (__int128(1) << 63);
        }
      }
      if (!sound) continue;
    }

    // ---- P2: no `&s` anywhere in the function
    bool addrof = false;
    srWalkStmt(fn->body, [&](Expr* x) {
      if (x->k == Expr::UN && x->op == "&" && x->rhs &&
          x->rhs->k == Expr::IDENT && x->rhs->sym == s)
        addrof = true;
    });
    if (addrof) continue;

    // ---- P3: e never references s
    bool refs = false;
    srWalkExpr(e, [&](Expr* x) {
      if (x->k == Expr::IDENT && x->sym == s) refs = true;
    });
    if (refs) continue;

    // ================================================== rewrite
    // The control plane is ALWAYS i64 (see the file header): the loop
    // lowers with an i64 signed compare, so trip/full/fpos/guard and the
    // term index arithmetic mirror the loop's own i64 iteration space.
    Type* T = Type::simple(Type::I64);
    Type* FT = s->ty;
    SrcLoc loc = S->loc;

    // fresh bindings (registered like the tiling pass's tile variables)
    std::string tripN, fullN, fposN, jN;
    Sym* tripSym = srFreshVar(fn, "__sr_trip", T, &tripN);
    Sym* fullSym = srFreshVar(fn, "__sr_full", T, &fullN);
    Sym* fposSym = srFreshVar(fn, "__sr_fpos", T, &fposN);
    Sym* jSym = srFreshVar(fn, "__sr_j", T, &jN);
    std::string tN[SR_K];
    Sym* tSym[SR_K];
    for (int k = 0; k < SR_K; ++k)
      tSym[k] = srFreshVar(fn, "__sr_t", FT, &tN[k]);

    auto mkLet = [&](const std::string& name, Type* ty, Expr* init) {
      Stmt* L = new Stmt(Stmt::LET, loc);
      L->var = name;
      L->annTy = ty;
      L->hasAnn = true;
      L->mut = false;
      L->init = init;
      L->fn = fn;
      return L;
    };

    // let __sr_trip: T = hi - lo;
    Stmt* letTrip = mkLet(tripN, T,
                          srBin("-", cloneExpr(S->rangeHi, {}),
                                cloneExpr(S->rangeLo, {}), T, loc));
    // let __sr_full: T = (__sr_trip / 8) * 8;
    Stmt* letFull = mkLet(
        fullN, T,
        srBin("*",
              srBin("/", srIdent(tripN, tripSym, T, loc),
                    srInt(SR_K, T, loc), T, loc),
              srInt(SR_K, T, loc), T, loc));
    // let __sr_fpos: T = imax(__sr_full, 0);
    Stmt* letFpos = mkLet(fposN, T,
                          srImax(srIdent(fullN, fullSym, T, loc),
                                 srInt(0, T, loc), T, loc));

    // if (__sr_full > 0) { for (__sr_j in 0 .. __sr_trip / 8) { ... } }
    Stmt* ifG = new Stmt(Stmt::IF, loc);
    ifG->cond = srBin(">", srIdent(fullN, fullSym, T, loc),
                      srInt(0, T, loc), Type::simple(Type::BOOL), loc);
    ifG->fn = fn;

    Stmt* mainL = new Stmt(Stmt::FOR, loc);
    mainL->forRange = true;
    mainL->ivar = jN;
    mainL->rangeLo = srInt(0, T, loc);
    mainL->rangeHi = srBin("/", srIdent(tripN, tripSym, T, loc),
                           srInt(SR_K, T, loc), T, loc);
    mainL->fn = fn;
    mainL->vectorizeHint = S->vectorizeHint;   // the term loop vectorises
    Stmt* mainBody = new Stmt(Stmt::BLOCK, loc);
    mainBody->fn = fn;
    mainL->body = mainBody;

    // let __sr_tk: FT = e[i := lo + __sr_j*8 + k];   (k = 0..7)
    for (int k = 0; k < SR_K; ++k) {
      std::map<Sym*, Expr*> sub;
      if (ivSym) {
        Expr* idx = srBin("+",
                          srBin("+", cloneExpr(S->rangeLo, {}),
                                srBin("*", srIdent(jN, jSym, T, loc),
                                      srInt(SR_K, T, loc), T, loc),
                                T, loc),
                          srInt((uint64_t)k, T, loc), T, loc);
        sub[ivSym] = idx;
      }
      mainBody->stmts.push_back(mkLet(tN[k], FT, cloneExpr(e, sub)));
    }
    // s = s + __sr_t0; ... s = s + __sr_t7;   (original order)
    for (int k = 0; k < SR_K; ++k) {
      Stmt* as = new Stmt(Stmt::ASSIGN, loc);
      as->op2 = "=";
      as->lhs = srIdent(s->name, s, FT, loc);
      as->rhs = srBin("+", srIdent(s->name, s, FT, loc),
                      srIdent(tN[k], tSym[k], FT, loc), FT, loc);
      as->fn = fn;
      mainBody->stmts.push_back(as);
    }

    Stmt* ifBody = new Stmt(Stmt::BLOCK, loc);
    ifBody->fn = fn;
    ifBody->stmts.push_back(mainL);
    ifG->thenB = ifBody;
    ifG->elseB = nullptr;

    // for (i in lo + __sr_fpos .. hi) { <original assign, cloned>; }
    Stmt* tailL = new Stmt(Stmt::FOR, loc);
    tailL->forRange = true;
    tailL->ivar = S->ivar;
    tailL->rangeLo = srBin("+", cloneExpr(S->rangeLo, {}),
                           srIdent(fposN, fposSym, T, loc), T, loc);
    tailL->rangeHi = cloneExpr(S->rangeHi, {});
    tailL->fn = fn;
    tailL->vectorizeHint = S->vectorizeHint;
    tailL->body = cloneStmt(a, {});

    // splice: 3 LETs + IF + tail FOR replace the original loop
    std::vector<Stmt*> replacement;
    replacement.push_back(letTrip);
    replacement.push_back(letFull);
    replacement.push_back(letFpos);
    replacement.push_back(ifG);
    replacement.push_back(tailL);
    for (auto* r : replacement) srRepairParents(r, S->parent, fn);
    list[i] = replacement[0];
    list.insert(list.begin() + i + 1, replacement.begin() + 1,
                replacement.end());
    i += replacement.size() - 1;           // skip what we inserted
    ++applied;
    diag(Diag::Note, Code::SumRed,
         "float sum reduction regrouped into blocks of 8 — terms are "
         "computed first (vectorisable) and folded in the original order, "
         "bit for bit",
         S->loc, "kind=sumred,terms=8");
    proofLogLine("sum-reduction",
                 "e-never-reads-or-writes-s;no-addrof-of-s;"
                 "lo-hi-typed-as-ivar;single-add-body;range-loop",
                 fn, S->loc);
  }
  return applied;
}

}  // namespace

int runFpReductionPass(Program* p) {
  int total = 0;
  for (auto* fn : p->fns) {
    if (!fn->body || fn->isGeneric || fn->imported) continue;
    srWalkLists(fn->body, fn, [&](std::vector<Stmt*>& list) {
      total += srInList(list, fn);
    });
  }
  return total;
}

}  // namespace freshet
