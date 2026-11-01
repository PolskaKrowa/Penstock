// Freshet — typed constant folding, range/induction analysis, parfor
// advisor.  See loop_analysis.h for the contracts.
#include "loop_analysis.h"
#include <algorithm>
#include <functional>

namespace freshet {

using namespace optutil;

// ------------------------------------------------------------- helpers
static void walkExprLA(Expr* e, const std::function<void(Expr*)>& f) {
  if (!e) return;
  f(e);
  for (auto* a : e->args) walkExprLA(a, f);
  walkExprLA(e->lhs, f);
  walkExprLA(e->rhs, f);
  walkExprLA(e->base, f);
}

static void walkStmtExprs(Stmt* s, const std::function<void(Expr*)>& f);

static void walkStmts(std::vector<Stmt*>& list,
                      const std::function<void(Stmt*)>& f) {
  for (auto* st : list) f(st);
}

static void walkStmtExprs(Stmt* s, const std::function<void(Expr*)>& f) {
  if (!s) return;
  switch (s->k) {
    case Stmt::LET: walkExprLA(s->init, f); break;
    case Stmt::ASSIGN: walkExprLA(s->lhs, f); walkExprLA(s->rhs, f); break;
    case Stmt::RETURN: walkExprLA(s->retVal, f); break;
    case Stmt::DEFER: walkExprLA(s->deferExpr, f); break;
    case Stmt::EXPR: walkExprLA(s->expr, f); break;
    case Stmt::IF:
      walkExprLA(s->cond, f);
      walkStmtExprs(s->thenB, f);
      walkStmtExprs(s->elseB, f);
      break;
    case Stmt::WHILE:
      walkExprLA(s->cond, f);
      walkStmtExprs(s->body, f);
      break;
    case Stmt::FOR:
      walkExprLA(s->forInit, f); walkExprLA(s->forCond, f);
      walkExprLA(s->forStep, f);
      walkExprLA(s->rangeLo, f); walkExprLA(s->rangeHi, f);
      walkStmtExprs(s->body, f);
      break;
    case Stmt::PARFOR:
      walkExprLA(s->plo, f); walkExprLA(s->phi, f);
      for (auto& c : s->clauses) walkExprLA(c.e, f);
      walkStmtExprs(s->body, f);
      break;
    case Stmt::MATCH:
      walkExprLA(s->matchOn, f);
      for (auto& c : s->clauses) walkExprLA(c.e, f);
      for (auto& arm : s->matchArms) walkStmtExprs(arm.body, f);
      break;
    case Stmt::BLOCK:
      for (auto* st : s->stmts) walkStmtExprs(st, f);
      break;
    default: break;
  }
}

// ---------------------------------------------------------------- folding
// replace `e` IN PLACE with the typed constant `c` (children detached;
// the node keeps its own type, which is the type the value was computed
// at — ival holds the zero-extended bit pattern, exactly how the parser
// and sema represent literals)
static void mutateToConst(Expr* e, const TypedConst& c) {
  e->lhs = e->rhs = e->base = nullptr;
  e->args.clear();
  e->argGive.clear();
  e->slitFields.clear();
  e->name.clear();
  e->op.clear();
  e->castTy = nullptr;
  e->callee = nullptr;
  e->sym = nullptr;
  e->globalRef = nullptr;
  e->enumRef = nullptr;
  e->variantIdx = -1;
  e->isLvalue = false;
  if (c.width == 1 && (!e->ty || e->ty->k == Type::BOOL)) {
    e->k = Expr::BOOL;
    e->bval = c.bits != 0;
  } else {
    e->k = Expr::INT;
    e->ival = c.unsignedVal();
  }
  e->constant = true;
}

static bool foldExpr(Expr* e, int* count) {
  if (!e) return false;
  // fold children first (bottom-up)
  for (auto* a : e->args) foldExpr(a, count);
  foldExpr(e->lhs, count);
  foldExpr(e->rhs, count);
  foldExpr(e->base, count);

  // §6.1: short-circuit folding that does not evaluate the unreachable
  // operand — `false && X` / `true || X` are the value regardless of X
  // (even when X would trap or diverge)
  if (e->k == Expr::BIN && (e->op == "&&" || e->op == "||")) {
    TypedConst a;
    if (e->lhs && e->lhs->constant && evalConstTyped(e->lhs, &a) &&
        a.width == 1) {
      bool decide = (e->op == "&&") ? (a.bits == 0) : (a.bits != 0);
      if (decide) {
        TypedConst r;
        r.width = 1; r.isSigned = false; r.valid = true;
        r.bits = a.bits;
        mutateToConst(e, r);
        ++*count;
        return true;
      }
    }
  }

  TypedConst c;
  if (e->k == Expr::BIN || e->k == Expr::UN || e->k == Expr::CAST) {
    if (e->constant && e->ty && evalConstTyped(e, &c)) {
      // don't "fold" what is already a bare literal
      bool already = (e->k == Expr::INT) || (e->k == Expr::BOOL);
      if (!already) {
        // comparison/boolean results carry the node's bool type; integer
        // results require a resolved integer type (IRgen's literal path
        // dereferences e->ty — an untyped node is left unfolded), and
        // NARROW results are left unfolded as well: the evaluator models
        // truncating casts exactly for the ANALYSES, but materialising a
        // fresh narrow INT literal would reach IRgen's width-blind
        // literal path (add i64) and mismatch a u8/i8/i16 store
        if (c.width == 1) {
          if (e->ty->k == Type::BOOL) { mutateToConst(e, c); ++*count; return true; }
          return false;   // width-1 result on a non-bool node: not worth guessing
        }
        if (e->ty->isInt() && e->ty->bits() == 64) {
          mutateToConst(e, c);
          ++*count;
          return true;
        }
      }
    }
  }
  return false;
}

// §6.2 (modest subset): propagate immutable constant bindings whose name
// is unique in the function — this is what lets `let n = 64; for (i in
// 0..n)` feed exact trip counts to the unroll/tile passes without a
// full SCCP.  Mut bindings and shadowed names are left alone.
static bool bindingIsUnique(FnDecl* fn, const std::string& name) {
  if (lookupGlobal(name)) return false;
  for (auto& p : fn->params)
    if (p.name == name) return false;
  bool seen = false;
  std::function<void(Stmt*)> walk = [&](Stmt* s) {
    if (!s) return;
    if (s->k == Stmt::LET && s->var == name) {
      if (seen) { seen = false; return; }   // redeclared: not unique
      seen = true;
    }
    switch (s->k) {
      case Stmt::IF: walk(s->thenB); walk(s->elseB); break;
      case Stmt::WHILE: case Stmt::FOR: case Stmt::PARFOR: walk(s->body); break;
      case Stmt::MATCH: for (auto& a : s->matchArms) walk(a.body); break;
      case Stmt::BLOCK: for (auto* st : s->stmts) walk(st); break;
      default: break;
    }
  };
  walk(fn->body);
  return seen;   // declared exactly once
}

static int propagateImmutableConsts(FnDecl* fn) {
  // collect non-mut constant lets with unique names
  std::map<std::string, Expr*> consts;
  std::function<void(Stmt*)> collect = [&](Stmt* s) {
    if (!s) return;
    if (s->k == Stmt::LET && !s->mut && s->init && s->init->constant &&
        (s->init->k == Expr::INT || s->init->k == Expr::BOOL) &&
        bindingIsUnique(fn, s->var))
      consts[s->var] = s->init;
    switch (s->k) {
      case Stmt::IF: collect(s->thenB); collect(s->elseB); break;
      case Stmt::WHILE: case Stmt::FOR: case Stmt::PARFOR: collect(s->body); break;
      case Stmt::MATCH: for (auto& a : s->matchArms) collect(a.body); break;
      case Stmt::BLOCK: for (auto* st : s->stmts) collect(st); break;
      default: break;
    }
  };
  collect(fn->body);
  if (consts.empty()) return 0;
  int n = 0;
  walkStmtExprs(fn->body, [&](Expr* e) {
    if (e->k != Expr::IDENT || !e->sym) return;
    // only replace READS (rvalue uses): lvalue IDENT targets are the
    // binding itself and can never be a constant
    if (e->isLvalue) return;
    auto it = consts.find(e->name);
    if (it == consts.end()) return;
    Expr* v = it->second;
    if (v->k == Expr::INT) {
      e->k = Expr::INT;
      e->ival = v->ival;
      e->bval = false;
    } else {
      e->k = Expr::BOOL;
      e->bval = v->bval;
    }
    e->lhs = e->rhs = e->base = nullptr;
    e->args.clear(); e->argGive.clear(); e->slitFields.clear();
    e->name.clear(); e->op.clear(); e->castTy = nullptr;
    e->callee = nullptr; e->sym = nullptr; e->globalRef = nullptr;
    e->enumRef = nullptr; e->variantIdx = -1;
    e->constant = true;
    ++n;
  });
  return n;
}

int runTypedConstFoldPass(Program* p) {
  int folded = 0, propagated = 0;
  for (auto* fn : p->fns) {
    if (!fn->body || fn->isGeneric) continue;
    propagated += propagateImmutableConsts(fn);
    walkStmtExprs(fn->body, [&](Expr* e) { foldExpr(e, &folded); });
  }
  if (folded + propagated > 0) {
    diag(Diag::Note, Code::ConstFold,
         "typed constant folding: " + std::to_string(folded) +
         " expression(s) folded at exact source width, " +
         std::to_string(propagated) + " immutable binding use(s) propagated",
         "", 0, 0,
         "folded=" + std::to_string(folded) +
         ",propagated=" + std::to_string(propagated));
  }
  return folded + propagated;
}

// ------------------------------------------------------ affine / interval
Affine affineIn(Expr* e, const std::string& iv) {
  Affine r{};
  if (!e || iv.empty()) return r;
  Expr* x = e;
  while (x->k == Expr::CAST && x->castTy && x->castTy->isInt()) x = x->base;
  if (x->k == Expr::IDENT && x->name == iv) { r.valid = true; r.coef = 1; r.off = 0; return r; }
  int64_t c = 0;
  if (x->constant && x->k == Expr::INT && evalConstInt(x, &c)) {
    r.valid = true; r.coef = 0; r.off = c;
    return r;
  }
  if (x->k == Expr::BIN) {
    Affine a = affineIn(x->lhs, iv), b = affineIn(x->rhs, iv);
    const std::string& op = x->op;
    if (op == "+" && a.valid && b.valid) {
      // int64 addition of coef/off could overflow for huge constants;
      // refuse rather than wrap (§4.4 discipline)
      if (!__builtin_add_overflow(a.coef, b.coef, &r.coef) &&
          !__builtin_add_overflow(a.off, b.off, &r.off)) { r.valid = true; return r; }
      return r;
    }
    if (op == "-" && a.valid && b.valid) {
      if (!__builtin_sub_overflow(a.coef, b.coef, &r.coef) &&
          !__builtin_sub_overflow(a.off, b.off, &r.off)) { r.valid = true; return r; }
      return r;
    }
    if (op == "*" && a.valid && b.valid) {
      // one side must be a pure constant
      if (a.coef == 0) {
        if (!__builtin_mul_overflow(a.off, b.coef, &r.coef) &&
            !__builtin_mul_overflow(a.off, b.off, &r.off)) { r.valid = true; return r; }
      } else if (b.coef == 0) {
        if (!__builtin_mul_overflow(b.off, a.coef, &r.coef) &&
            !__builtin_mul_overflow(b.off, a.off, &r.off)) { r.valid = true; return r; }
      }
      return r;
    }
  }
  if (x->k == Expr::UN && x->op == "-") {
    Affine a = affineIn(x->rhs, iv);
    if (a.valid && a.coef != INT64_MIN && a.off != INT64_MIN) {
      r.valid = true; r.coef = -a.coef; r.off = -a.off;
      return r;
    }
  }
  return r;
}

Interval intervalOf(Expr* e) {
  Interval r;
  if (!e) return r;
  if (e->k == Expr::INT) {
    int64_t v = 0;
    if (evalConstInt(e, &v)) { r.known = true; r.lo = r.hi = v; }
    return r;
  }
  if (e->k == Expr::CAST && e->castTy && e->castTy->isInt()) {
    // a cast result is always inside the target type's range — a sound
    // bound even when the operand is unknown
    Type* t = e->castTy;
    if (t->isSigned()) {
      switch (t->bits()) {
        case 8: r.lo = -128; r.hi = 127; break;
        case 16: r.lo = -32768; r.hi = 32767; break;
        case 32: r.lo = INT64_C(-2147483648); r.hi = INT64_C(2147483647); break;
        default: r.lo = INT64_MIN; r.hi = INT64_MAX; break;
      }
    } else {
      switch (t->bits()) {
        case 8: r.lo = 0; r.hi = 255; break;
        case 16: r.lo = 0; r.hi = 65535; break;
        case 32: r.lo = 0; r.hi = INT64_C(4294967295); break;
        default: return r;   // u64: full range is uninformative
      }
    }
    r.known = true;
    // tighten when the operand itself has an interval
    Interval b = intervalOf(e->base);
    if (b.known) {
      r.lo = std::max(r.lo, b.lo);
      r.hi = std::min(r.hi, b.hi);
      if (r.lo > r.hi) { r = Interval{}; }   // empty after clamping
    }
    return r;
  }
  if (e->k == Expr::UN && e->op == "-") {
    Interval b = intervalOf(e->rhs);
    if (b.known && b.lo != INT64_MIN) {
      r.known = true;
      r.lo = (b.hi == INT64_MIN) ? INT64_MAX : -b.hi;
      r.hi = -b.lo;
    }
    return r;
  }
  if (e->k == Expr::BIN && (e->op == "+" || e->op == "-")) {
    Interval a = intervalOf(e->lhs), b = intervalOf(e->rhs);
    if (!a.known || !b.known) return r;
    int64_t lo1, hi1, lo2, hi2;
    if (e->op == "+") {
      if (__builtin_add_overflow(a.lo, b.lo, &lo1)) return r;
      if (__builtin_add_overflow(a.hi, b.hi, &hi1)) return r;
    } else {
      if (__builtin_sub_overflow(a.lo, b.hi, &lo1)) return r;
      if (__builtin_sub_overflow(a.hi, b.lo, &hi1)) return r;
    }
    (void)lo2; (void)hi2;
    r.known = true; r.lo = lo1; r.hi = hi1;
    return r;
  }
  if (e->k == Expr::CALL && (e->name == "imin" || e->name == "imax") &&
      e->args.size() == 2 && !e->callee) {
    Interval a = intervalOf(e->args[0]), b = intervalOf(e->args[1]);
    if (!a.known || !b.known) return r;
    r.known = true;
    if (e->name == "imin") {
      r.lo = std::min(a.lo, b.lo);
      r.hi = std::min(a.hi, b.hi);
    } else {
      r.lo = std::max(a.lo, b.lo);
      r.hi = std::max(a.hi, b.hi);
    }
    return r;
  }
  return r;
}

// ------------------------------------------------------ dependence preds
bool iterationsIndependent(const BodyInfo& b) {
  if (b.unknownRead || b.unknownWrite) return false;
  std::set<std::string> w = writeRoots(b), rd = readRootsOf(b);
  for (auto& x : w)
    if (rd.count(x)) return false;
  return true;
}

bool noObservableEffects(const BodyInfo& b) {
  return !b.hasPrint && !b.hasReturn && !b.hasBreak && !b.hasParfor &&
         !b.hasGive;
}

// ------------------------------------------------------ parfor advisor
static bool isSimpleRangeFor(Stmt* s) {
  return s->k == Stmt::FOR && s->forRange && !s->ivar.empty();
}

int runParforAdvisorPass(Program* p) {
  computeFootprints(p);
  int advised = 0, reductions = 0;
  for (auto* fn : p->fns) {
    if (!fn->body || fn->isGeneric) continue;
    std::function<void(Stmt*)> walk = [&](Stmt* s) {
      if (!s) return;
      if (isSimpleRangeFor(s)) {
        BodyInfo info;
        collectBody(s->body, s->ivar, fn, &info);
        if (!info.hasParfor && noObservableEffects(info) &&
            !info.unknownRead && !info.unknownWrite && !info.writes.empty()) {
          // every write must be an injective per-iteration index function
          bool injective = true;
          for (auto& kv : info.writes)
            for (auto& a : kv.second)
              if (!(a.kind == Access::DIRECT && a.affineLit)) injective = false;
          // same-root read+write is still PARALLEL-SAFE when every access
          // pair is same-iteration (identical canonical affine index) —
          // the tier-2 dependence shape the fusion pass proves
          bool sameIterationOnly = true;
          if (injective) {
            for (auto& kv : info.writes) {
              auto rit = info.reads.find(kv.first);
              if (rit == info.reads.end()) continue;
              for (auto& w : kv.second) {
                for (auto& r : rit->second)
                  if (!(r.kind == Access::DIRECT && r.affineLit &&
                        r.idx == w.idx)) sameIterationOnly = false;
              }
            }
          }
          if (injective &&
              (iterationsIndependent(info) || sameIterationOnly)) {
            ++advised;
            diag(Diag::Note, Code::ParAdvise,
                 "loop (line " + std::to_string(s->loc.line) + ", fn " +
                 fn->name + ") appears PARALLEL-SAFE: per-iteration writes "
                 "are disjoint injective index functions, every same-root "
                 "read is same-iteration, and the body has no observable "
                 "effects — consider `parfor`",
                 s->loc.file, s->loc.line, s->loc.col,
                 "kind=parallel-safe,fn=" + fn->name);
          } else {
            // accumulate-into-scalar shape: reduction candidate
            bool scalarOnly = true;
            for (auto& kv : info.writes)
              for (auto& a : kv.second)
                if (a.kind != Access::SCALAR) scalarOnly = false;
            if (scalarOnly) {
              ++reductions;
              diag(Diag::Note, Code::ParAdvise,
                   "loop (line " + std::to_string(s->loc.line) + ", fn " +
                   fn->name + ") writes only scalar accumulators — a "
                   "REDUCTION candidate: `parfor (i in ..., reduce(+: acc))` "
                   "preserves the arithmetic when the operator is "
                   "associative (floating-point reassociation stays an "
                   "explicit language mode)",
                   s->loc.file, s->loc.line, s->loc.col,
                   "kind=reduction-candidate,fn=" + fn->name);
            }
          }
        }
      }
      switch (s->k) {
        case Stmt::IF: walk(s->thenB); walk(s->elseB); break;
        case Stmt::WHILE: case Stmt::FOR: case Stmt::PARFOR: walk(s->body); break;
        case Stmt::MATCH: for (auto& a : s->matchArms) walk(a.body); break;
        case Stmt::BLOCK: for (auto* st : s->stmts) walk(st); break;
        default: break;
      }
    };
    walk(fn->body);
  }
  return advised + reductions;
}

} // namespace freshet
