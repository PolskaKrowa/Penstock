// Freshet source-level loop optimisation — shared analysis utilities (R4).
// See opt_util.h for the interface contract.
#include "opt_util.h"
#include <algorithm>
#include <functional>

namespace freshet {
namespace optutil {

// ------------------------------------------------------------------ canon
// Commutativity is now decided by the TYPED arithmetic contract
// (optimisation-audit §4.5), not by operator spelling alone:
//   + * & | ^   — fixed-width two's-complement integers only: total,
//                 wrapping, operand order provably irrelevant;
//   == !=       — any type: comparison result never depends on order;
//   + * on floats — NOT commutative keys under strict FP: NaN payload
//                 propagation can differ between a+b and b+a;
//   && ||       — NOT commutative keys: operand order interacts with
//                 traps/divergence in short-circuit evaluation.
static bool isCommutativeKey(const std::string& op, Type* ty) {
  if (op == "==" || op == "!=") return true;
  bool isFloat = ty && ty->isFloat();
  if (isFloat) return false;
  return op == "+" || op == "*" || op == "&" || op == "|" || op == "^";
}

// typed literal rendering for canonical keys: the mathematical value
// under the literal's signedness PLUS a type tag, so 5:u8, 5:i64 and
// 5:u64 never collide (the old key printed (int64_t)e->ival, which
// reinterpreted u64 values above INT64_MAX as negative).
static std::string canonIntOf(const TypedConst& c, Type* ty) {
  std::string v = c.isSigned ? std::to_string(c.signedVal())
                              : std::to_string(c.unsignedVal());
  if (ty && ty->isInt()) return v + ":" + typeToString(ty);
  return v;
}

// canon with induction-variable normalisation ("$IV")
static std::string canonImpl(Expr* e, const std::string& iv) {
  if (!e) return "<nil>";
  if (e->k == Expr::IDENT && !iv.empty() && e->name == iv) return "$IV";
  switch (e->k) {
    case Expr::INT: {
      TypedConst c;
      if (evalConstTyped(e, &c)) return canonIntOf(c, e->ty);
      return std::to_string(e->ival);   // untyped (pre-sema) fallback
    }
    case Expr::FLT: {
      char buf[64];
      snprintf(buf, sizeof buf, "f%a", e->fval);
      return buf;
    }
    case Expr::BOOL: return e->bval ? "true" : "false";
    case Expr::STR:  return "\"" + e->sval + "\"";
    case Expr::NILQ: return "nil";
    case Expr::IDENT: return e->name;
    case Expr::CALL: {
      std::string s = "(" + e->name;
      for (auto* a : e->args) s += " " + canonImpl(a, iv);
      return s + ")";
    }
    case Expr::SLIT: {
      std::string s = "(slit";
      for (auto& f : e->slitFields) s += " " + f;
      return s + ")";
    }
    // Phase 1b/1c/2c: new expression forms — canonicalise structurally
    case Expr::ARRLIT: {
      std::string s = "(arr";
      for (auto* a : e->args) s += " " + canonImpl(a, iv);
      return s + ")";
    }
    case Expr::SLICE:
      return "(slice " + canonImpl(e->base, iv) + " " + canonImpl(e->lhs, iv) +
             " " + canonImpl(e->rhs, iv) + ")";
    case Expr::TRYQ:
      return "(try " + canonImpl(e->base, iv) + ")";
    case Expr::BIN: {
      // typed, wrapping constant folding in the KEY (the old code used
      // host int64_t subtraction — UB on overflow and wrong for u64)
      if (e->lhs && e->lhs->constant && e->lhs->k == Expr::INT &&
          e->rhs && e->rhs->constant && e->rhs->k == Expr::INT) {
        TypedConst c;
        if (evalConstTyped(e, &c)) return canonIntOf(c, e->ty);
      }
      std::string a = canonImpl(e->lhs, iv), b = canonImpl(e->rhs, iv);
      if (isCommutativeKey(e->op, e->ty) && a > b) std::swap(a, b);
      return "(" + a + e->op + b + ")";
    }
    case Expr::UN:
      if (e->op == "-" && e->rhs && e->rhs->constant &&
          e->rhs->k == Expr::INT) {
        TypedConst c;
        if (evalConstTyped(e, &c)) return canonIntOf(c, e->ty);
      }
      return "(" + e->op + " " + canonImpl(e->rhs, iv) + ")";
    case Expr::INDEX:
      return "(" + canonImpl(e->base, iv) + "[" + canonImpl(e->rhs, iv) + "])";
    case Expr::FIELD: return "(" + canonImpl(e->base, iv) + "." + e->name + ")";
    case Expr::CAST:
      return "(as " + typeToString(e->castTy) + " " + canonImpl(e->base, iv) + ")";
  }
  return "?";
}

std::string canon(Expr* e) { return canonImpl(e, ""); }
std::string canonN(Expr* e, const std::string& iv) {
  return canonImpl(e, iv);
}

bool sameExpr(Expr* a, Expr* b) {
  if (a == b) return true;
  if (!a || !b) return false;
  if (a->k != b->k) return false;
  switch (a->k) {
    case Expr::INT:  return a->ival == b->ival;
    case Expr::FLT:  return a->fval == b->fval;
    case Expr::BOOL: return a->bval == b->bval;
    case Expr::STR:  return a->sval == b->sval;
    case Expr::IDENT: return a->name == b->name;
    case Expr::CALL:
      if (a->name != b->name || a->args.size() != b->args.size()) return false;
      for (size_t i = 0; i < a->args.size(); ++i)
        if (!sameExpr(a->args[i], b->args[i])) return false;
      return true;
    case Expr::SLIT:
      if (a->slitFields != b->slitFields || a->args.size() != b->args.size())
        return false;
      for (size_t i = 0; i < a->args.size(); ++i)
        if (!sameExpr(a->args[i], b->args[i])) return false;
      return true;
    case Expr::BIN: case Expr::UN:
      if (a->op != b->op) return false;
      return sameExpr(a->lhs, b->lhs) && sameExpr(a->rhs, b->rhs);
    case Expr::INDEX:
      return sameExpr(a->base, b->base) && sameExpr(a->rhs, b->rhs);
    case Expr::FIELD: return a->name == b->name && sameExpr(a->base, b->base);
    case Expr::CAST:
      return typesEquivalent(a->castTy, b->castTy) &&
             sameExpr(a->base, b->base);
    case Expr::NILQ: return true;
    case Expr::ARRLIT:
      if (a->args.size() != b->args.size()) return false;
      for (size_t i = 0; i < a->args.size(); ++i)
        if (!sameExpr(a->args[i], b->args[i])) return false;
      return true;
    case Expr::SLICE:
      return sameExpr(a->base, b->base) && sameExpr(a->lhs, b->lhs) &&
             sameExpr(a->rhs, b->rhs);
    case Expr::TRYQ: return sameExpr(a->base, b->base);
  }
  return false;
}

// forward: shared AST walker defined below (used by the legality
// predicates, which live above it)
static void walkExpr(Expr* e, const std::function<void(Expr*)>& f);

bool evalConstInt(Expr* e, int64_t* out) {
  // exact wrapper over the typed evaluator: the historical version
  // negated int64_t directly (UB on INT64_MIN) and treated a narrowing
  // cast as the identity (audit §4.4).
  TypedConst c;
  if (!evalConstTyped(e, &c)) return false;
  if (c.isSigned) { *out = c.signedVal(); return true; }
  uint64_t u = c.unsignedVal();
  if (u > (uint64_t)INT64_MAX) return false;   // not representable exactly
  *out = (int64_t)u;
  return true;
}

// ------------------------------------------------ typed constant evaluation
static void tyWidthOf(Type* t, int* w, bool* sgn) {
  if (t && t->k == Type::BOOL) { *w = 1; *sgn = false; return; }
  if (t && t->isInt()) { *w = (int)t->bits(); *sgn = t->isSigned(); }
  else { *w = 64; *sgn = true; }    // historical default: i64
}

bool evalConstTyped(Expr* e, TypedConst* out) {
  out->valid = false;
  if (!e) return false;
  int W = 64; bool S = true;
  tyWidthOf(e->ty, &W, &S);
  if (W < 1 || W > 64) return false;
  uint64_t mask = W == 64 ? ~0ull : ((1ull << W) - 1);
  auto mk = [&](uint64_t bits) {
    out->bits = bits & mask; out->width = W; out->isSigned = S;
    out->valid = true; return true;
  };
  switch (e->k) {
    case Expr::INT:
      return mk(e->ival);
    case Expr::BOOL:
      out->width = 1; out->isSigned = false;
      out->bits = e->bval ? 1 : 0;
      out->valid = true;
      return true;
    case Expr::UN: {
      TypedConst a;
      if (e->op == "-") {
        if (!evalConstTyped(e->rhs, &a) || a.width > 64) return false;
        return mk((~a.bits + 1) & mask);      // wrapping two's complement
      }
      if (e->op == "!") {
        if (!evalConstTyped(e->rhs, &a) || a.width != 1) return false;
        return mk(a.bits == 0 ? 1 : 0);
      }
      return false;
    }
    case Expr::CAST: {
      // narrowing = truncation to the TARGET width; widening keeps the
      // zero-extended bit pattern (signedness applies at interpretation)
      if (!e->castTy || !e->castTy->isInt()) return false;
      TypedConst a;
      if (!evalConstTyped(e->base, &a) || !a.valid) return false;
      int NW = (int)e->castTy->bits();
      bool NS = e->castTy->isSigned();
      if (NW < 1 || NW > 64) return false;
      uint64_t nm = NW == 64 ? ~0ull : ((1ull << NW) - 1);
      out->bits = a.bits & nm; out->width = NW; out->isSigned = NS;
      out->valid = true;
      return true;
    }
    case Expr::BIN: {
      TypedConst a, b;
      // operands are evaluated at the node's width so mixed-width
      // promotion is not modelled (sema rejects mixed-width arithmetic
      // except through explicit casts, which the CAST case handles)
      if (!evalConstTyped(e->lhs, &a) || !a.valid) return false;
      if (!evalConstTyped(e->rhs, &b) || !b.valid) return false;
      const std::string& op = e->op;
      if (op == "+") return mk(a.bits + b.bits);
      if (op == "-") return mk(a.bits - b.bits);
      if (op == "*") return mk(a.bits * b.bits);
      if (op == "&") return mk(a.bits & b.bits);
      if (op == "|") return mk(a.bits | b.bits);
      if (op == "^") return mk(a.bits ^ b.bits);
      if (op == "<<" || op == ">>") {
        // only a constant amount below the width is exactly modelled
        // (LLVM shifts by >= width produce poison)
        if (b.width > 32) return false;
        int64_t amt = b.isSigned ? b.signedVal() : (int64_t)b.unsignedVal();
        if (amt < 0 || amt >= W) return false;
        if (op == "<<") return mk(a.bits << (int)amt);
        if (S) {
          // arithmetic shift on the sign-extended value, truncated back
          int64_t v = a.signedVal();
          return mk((uint64_t)(v >> (int)amt));
        }
        return mk(a.bits >> (int)amt);
      }
      if (op == "/" || op == "%") {
        // never fold a possibly-trapping division: require a non-zero
        // constant divisor; reject the INT_MIN/-1 overflow and the
        // most-negative divisor outright (cheap + always safe)
        if (b.width > 64) return false;
        uint64_t dbits = b.unsignedVal();
        if (dbits == 0) return false;
        if (S && dbits == (1ull << (W - 1))) return false;
        if (!S) {
          uint64_t x = a.unsignedVal(), d = dbits;
          if (op == "/") return mk(x / d);
          return mk(x % d);
        }
        // signed division truncates toward zero (LLVM sdiv / C99)
        int64_t x = a.signedVal();
        uint64_t dRaw = dbits & mask;
        int64_t d = (int64_t)(W == 64 ? dRaw
                          : (dRaw >> (W - 1)) ? (dRaw | (~0ull << (W - 1))) : dRaw);
        if (d == 0) return false;
        if (x == INT64_MIN && d == -1 && W == 64) return false;  // poison
        if (op == "/") return mk((uint64_t)(x / d));
        return mk((uint64_t)(x % d));
      }
      if (op == "==" || op == "!=" || op == "<" || op == "<=" ||
          op == ">" || op == ">=") {
        if (a.width != b.width && !(a.width <= 64 && b.width <= 64))
          return false;
        bool res;
        if (S) {
          int64_t x = a.signedVal(), y = b.signedVal();
          if (op == "==") res = x == y;
          else if (op == "!=") res = x != y;
          else if (op == "<") res = x < y;
          else if (op == "<=") res = x <= y;
          else if (op == ">") res = x > y;
          else res = x >= y;
        } else {
          uint64_t x = a.unsignedVal(), y = b.unsignedVal();
          if (op == "==") res = x == y;
          else if (op == "!=") res = x != y;
          else if (op == "<") res = x < y;
          else if (op == "<=") res = x <= y;
          else if (op == ">") res = x > y;
          else res = x >= y;
        }
        out->width = 1; out->isSigned = false; out->bits = res ? 1 : 0;
        out->valid = true;
        return true;
      }
      if (op == "&&" || op == "||") {
        if (a.width != 1 || b.width != 1) return false;
        bool x = a.bits != 0, y = b.bits != 0;
        bool res = op == "&&" ? (x && y) : (x || y);
        out->width = 1; out->isSigned = false; out->bits = res ? 1 : 0;
        out->valid = true;
        return true;
      }
      return false;
    }
    default:
      return false;
  }
}

TripInfo tripCountOf(Stmt* loop) {
  TripInfo t;
  if (!loop || !loop->forRange) return t;
  TypedConst lo, hi;
  if (!loop->rangeLo || !loop->rangeHi) return t;
  if (!evalConstTyped(loop->rangeLo, &lo) || !lo.valid) return t;
  if (!evalConstTyped(loop->rangeHi, &hi) || !hi.valid) return t;
  t.known = true;
  // mathematical distance in 128 bits: never overflows, never wraps —
  // the historical `hi - lo` could overflow int64_t BEFORE the bounds
  // check ran (audit §4.4)
  __int128 hiM = hi.isSigned ? (__int128)hi.signedVal()
                             : (__int128)hi.unsignedVal();
  __int128 loM = lo.isSigned ? (__int128)lo.signedVal()
                             : (__int128)lo.unsignedVal();
  __int128 d = hiM - loM;
  if (d <= 0) { t.empty = true; t.trip = 0; return t; }
  if (d > (__int128)INT64_MAX) { t.saturated = true; t.trip = INT64_MAX; return t; }
  t.trip = (int64_t)d;
  return t;
}

// ------------------------------------------- predicate legality predicates
// Mirrors effects.cpp's builtinEffects memory model (the single source
// of truth for what a builtin does) — audit §4.1 asked for exactly this
// unification instead of a second, drifting allow-list.
bool isRTBuiltinCall(const std::string& n, bool strictfp) {
  static const std::set<std::string> memFree = {
    "sqrt", "fabs", "floor", "ceil", "round", "fmin", "fmax", "pow",
    "exp", "log", "imin", "imax",
  };
  if (memFree.count(n)) return true;
  // sin/cos/log2/log10 are memory-free llvm.* intrinsics ONLY under
  // fast-math; @strictfp keeps scalar libm calls that touch errno
  if (n == "sin" || n == "cos" || n == "log2" || n == "log10")
    return !strictfp;
  // immutable runtime facts (thread count) and header reads are RT;
  // clocks, clflush, pause, io, alloc, free are not
  if (n == "freshet.threads" || n == "soa_len") return true;
  return false;
}

bool exprReferentiallyTransparent(Expr* e) {
  if (!e) return false;
  // strict FP is the LANGUAGE DEFAULT (audit report §8 Option A): builtins
  // like sin/cos are errno-touching libm calls there, so they are NOT
  // referentially transparent unless fast-math was explicitly enabled.
  const bool strictfp = !g_opts.fastMathGlobal;
  bool ok = true;
  walkExpr(e, [&](Expr* x) {
    if (!ok || x->k != Expr::CALL) return;
    if (x->callee) {
      const Effects& ef = x->callee->eff;
      if (!x->callee->isPure || ef.mayNotReturn || ef.io || ef.allocates ||
          ef.freesMem || ef.unknownCall)
        ok = false;
    } else if (!isRTBuiltinCall(x->name, strictfp)) {
      ok = false;
    }
  });
  return ok;
}

bool exprTotalAndPure(Expr* e) {
  if (!e) return false;
  if (!exprReferentiallyTransparent(e)) return false;
  bool ok = true;
  walkExpr(e, [&](Expr* x) {
    if (!ok) return;
    switch (x->k) {
      case Expr::INDEX: case Expr::FIELD:
        // a dereference can trap (checked OOB abort / null); on a
        // zero-trip loop the source would never have evaluated it
        if (x->k == Expr::INDEX || x->ptrDeref) ok = false;
        break;
      case Expr::BIN: {
        const std::string& op = x->op;
        if (op == "/" || op == "%") {
          TypedConst d;
          if (!x->rhs || !evalConstTyped(x->rhs, &d) || !d.valid ||
              d.unsignedVal() == 0)
            ok = false;              // divisor not provably non-zero
        } else if (op == "<<" || op == ">>") {
          TypedConst d;
          int W = x->ty && x->ty->isInt() ? (int)x->ty->bits() : 64;
          if (!x->rhs || !evalConstTyped(x->rhs, &d) || !d.valid ||
              d.width > 32 || d.signedVal() < 0 || d.signedVal() >= W)
            ok = false;              // shift amount not provably in-range
        }
        break;
      }
      case Expr::CALL:
        if (x->callee) {
          if (x->callee->eff.mayNotReturn) ok = false;
        } else if (x->name == "soa_len") {
          ok = false;                // dereference-like header read
        }
        break;
      default: break;
    }
  });
  return ok;
}

bool isPureBuiltinCall(const std::string& n) {
  static const std::set<std::string> pure = {
    "sqrt", "fabs", "floor", "ceil", "round", "fmin", "fmax", "pow",
    "exp", "log", "sin", "cos", "tan", "log2", "log10", "freshet.threads",
    "imin", "imax",
  };
  return pure.count(n) != 0;
}

static void walkExpr(Expr* e, const std::function<void(Expr*)>& f) {
  if (!e) return;
  f(e);
  for (auto* a : e->args) walkExpr(a, f);
  walkExpr(e->lhs, f);
  walkExpr(e->rhs, f);
  walkExpr(e->base, f);
}

void collectIdents(Expr* e, std::set<std::string>& out) {
  walkExpr(e, [&](Expr* x) {
    if (x->k == Expr::IDENT) out.insert(x->name);
  });
}

static Expr* rootOfChain(Expr* e) {
  while (e && (e->k == Expr::INDEX || e->k == Expr::FIELD)) e = e->base;
  return e;
}

void exprReadRoots(Expr* e, std::set<std::string>& out) {
  walkExpr(e, [&](Expr* x) {
    if (x->k == Expr::INDEX || x->k == Expr::FIELD) {
      Expr* r = rootOfChain(x);
      if (r && r->k == Expr::IDENT) out.insert(r->name);
    }
    if (x->k == Expr::CALL) {
      for (auto* a : x->args) {
        Expr* r = (a->k == Expr::UN && a->op == "&") ? rootOfChain(a->rhs)
                                                     : rootOfChain(a);
        if (r && r->k == Expr::IDENT) out.insert(r->name);
        else if (a->k == Expr::IDENT) out.insert(a->name);
      }
    }
  });
}

bool hasImpureCall(Expr* e) {
  bool impure = false;
  walkExpr(e, [&](Expr* x) {
    if (impure || x->k != Expr::CALL) return;
    if (x->callee) {
      if (!x->callee->isPure) impure = true;
    } else if (!isPureBuiltinCall(x->name)) {
      impure = true;
    }
  });
  return impure;
}

bool affineIvarLiteral(Expr* e, const std::string& iv) {
  if (!e || iv.empty()) return false;
  Expr* x = e;
  while (x->k == Expr::CAST && x->castTy && x->castTy->isInt()) x = x->base;
  if (x->k == Expr::IDENT) return x->name == iv;
  if (x->k == Expr::BIN && x->op == "*") {
    Expr *l = x->lhs, *r = x->rhs;
    if (l->k == Expr::IDENT && l->name == iv && r->constant &&
        r->k == Expr::INT && r->ival != 0)
      return true;
    if (r->k == Expr::IDENT && r->name == iv && l->constant &&
        l->k == Expr::INT && l->ival != 0)
      return true;
  }
  return false;
}

// --------------------------------------------------------------- cloning
Expr* cloneExpr(const Expr* e, const std::map<Sym*, Expr*>& substBySym) {
  if (!e) return nullptr;
  if (e->k == Expr::IDENT && e->sym) {
    auto it = substBySym.find(e->sym);
    if (it != substBySym.end()) return it->second;
  }
  Expr* n = new Expr(e->k, e->loc);
  n->ival = e->ival; n->fval = e->fval; n->sval = e->sval; n->bval = e->bval;
  n->name = e->name; n->op = e->op;
  n->lhs = cloneExpr(e->lhs, substBySym);
  n->rhs = cloneExpr(e->rhs, substBySym);
  n->base = cloneExpr(e->base, substBySym);
  for (auto* a : e->args) n->args.push_back(cloneExpr(a, substBySym));
  n->argGive = e->argGive;
  n->slitFields = e->slitFields;
  n->castTy = e->castTy;
  n->ty = e->ty;
  n->constant = e->constant;
  n->isLvalue = e->isLvalue;
  n->soaIndex = e->soaIndex;
  n->ptrDeref = e->ptrDeref;
  n->mallocElem = e->mallocElem;
  n->sym = e->sym;
  n->globalRef = e->globalRef;
  n->callee = e->callee;
  n->fieldIdx = e->fieldIdx;
  return n;
}

Stmt* cloneStmt(const Stmt* s, const std::map<Sym*, Expr*>& substBySym) {
  if (!s) return nullptr;
  Stmt* n = new Stmt();
  n->k = s->k;
  n->loc = s->loc;
  n->tyName = s->tyName;
  n->op2 = s->op2;
  n->pDescending = s->pDescending;
  n->var = s->var; n->annTy = s->annTy; n->mut = s->mut; n->hasAnn = s->hasAnn;
  n->init = cloneExpr(s->init, substBySym);
  n->lhs = cloneExpr(s->lhs, substBySym);
  n->rhs = cloneExpr(s->rhs, substBySym);
  n->retVal = cloneExpr(s->retVal, substBySym);
  n->deferExpr = cloneExpr(s->deferExpr, substBySym);
  n->cond = cloneExpr(s->cond, substBySym);
  n->thenB = cloneStmt(s->thenB, substBySym);
  n->elseB = cloneStmt(s->elseB, substBySym);
  n->ivar = s->ivar;
  n->forInit = cloneExpr(s->forInit, substBySym);
  n->forCond = cloneExpr(s->forCond, substBySym);
  n->forStep = cloneExpr(s->forStep, substBySym);
  n->forRange = s->forRange;
  n->rangeLo = cloneExpr(s->rangeLo, substBySym);
  n->rangeHi = cloneExpr(s->rangeHi, substBySym);
  n->pvar = s->pvar;
  n->plo = cloneExpr(s->plo, substBySym);
  n->phi = cloneExpr(s->phi, substBySym);
  n->clauses = s->clauses;
  for (auto& c : n->clauses)
    if (c.e) c.e = cloneExpr(c.e, substBySym);
  n->body = cloneStmt(s->body, substBySym);
  n->depth = s->depth;
  n->collapsed = s->collapsed;
  n->expr = cloneExpr(s->expr, substBySym);
  for (auto* st : s->stmts) n->stmts.push_back(cloneStmt(st, substBySym));
  n->parent = s->parent;
  n->fn = s->fn;
  return n;
}

// ------------------------------------------------------------- aliasing
void buildAliasMap(Stmt* body, AliasMap* m) {
  // 3 sweeps: enough for v2 = v1; v3 = v2 chains (union is monotone)
  for (int sweep = 0; sweep < 3; ++sweep) {
    std::function<void(Stmt*)> walk = [&](Stmt* s) {
      if (!s) return;
      switch (s->k) {
        case Stmt::LET: {
          if (!s->init) break;
          // only pointer/soa-typed bindings alias memory
          if (s->annTy && !(s->annTy->isPtr() || s->annTy->k == Type::SOA))
            break;
          bool hasCall = false;
          walkExpr(s->init, [&](Expr* x) {
            if (x->k == Expr::CALL) hasCall = true;
          });
          if (hasCall) {
            // malloc/soa_new results are fresh memory (own root)
            if (s->init->k == Expr::CALL &&
                (s->init->name == "malloc" || s->init->name == "soa_new")) {
              if (!m->alias.count(s->var) && !m->wild.count(s->var))
                m->alias[s->var] = {};   // fresh: never propagates
            } else {
              m->wild.insert(s->var);     // call result: unknown target
              m->alias.erase(s->var);
            }
            break;
          }
          std::set<std::string> ids;
          collectIdents(s->init, ids);
          if (!ids.empty()) {
            auto& tgt = m->alias[s->var];
            if (m->wild.count(s->var)) break;   // already wild
            for (auto& i : ids) tgt.insert(i);
          }
          break;
        }
        case Stmt::ASSIGN: {
          // scalar pointer rebinding: v = <expr>
          if (!s->lhs || s->lhs->k != Expr::IDENT || !s->lhs->ty) break;
          if (!(s->lhs->ty->isPtr() || s->lhs->ty->k == Type::SOA)) break;
          const std::string& v = s->lhs->name;
          if (s->rhs) {
            bool hasCall = false;
            walkExpr(s->rhs, [&](Expr* x) {
              if (x->k == Expr::CALL) hasCall = true;
            });
            if (hasCall) {
              if (s->rhs->k == Expr::CALL &&
                  (s->rhs->name == "malloc" || s->rhs->name == "soa_new")) {
                if (!m->alias.count(v) && !m->wild.count(v))
                  m->alias[v] = {};
              } else {
                m->wild.insert(v);
                m->alias.erase(v);
              }
              break;
            }
            std::set<std::string> ids;
            collectIdents(s->rhs, ids);
            if (!ids.empty()) {
              if (m->wild.count(v)) break;
              auto& tgt = m->alias[v];
              for (auto& i : ids) tgt.insert(i);
            }
          }
          break;
        }
        case Stmt::IF:
          walk(s->thenB);
          walk(s->elseB);
          break;
        case Stmt::WHILE: case Stmt::FOR: case Stmt::PARFOR:
          walk(s->body);
          break;
        case Stmt::BLOCK:
          for (auto* st : s->stmts) walk(st);
          break;
        default: break;
      }
    };
    walk(body);
  }
}

bool resolveAlias(const AliasMap& m, const std::string& name,
                  std::set<std::string>& out, int depth) {
  if (m.wild.count(name)) return false;
  auto it = m.alias.find(name);
  if (it == m.alias.end()) {
    out.insert(name);   // not an alias: its own root
    return true;
  }
  if (depth > 6) return false;   // cycle guard: treat as wild
  for (const auto& t : it->second) {
    if (t == name) continue;
    if (!resolveAlias(m, t, out, depth + 1)) return false;
  }
  return true;
}

// ------------------------------------------------------------- footprints
static std::map<FnDecl*, FnFootprint> g_footprints;
static bool g_footprintsDone = false;

enum BuiltinMem {
  BM_PURE = 0, BM_FREE, BM_MEMCPY, BM_MEMSET, BM_SOA_FREE, BM_ALLOC,
  BM_PRINT, BM_LEN,
};

static BuiltinMem classifyBuiltin(const std::string& n) {
  if (isPureBuiltinCall(n)) return BM_PURE;
  if (n == "free" || n == "soa_free") return BM_FREE;
  if (n == "memcpy") return BM_MEMCPY;
  if (n == "memset") return BM_MEMSET;
  if (n == "malloc" || n == "soa_new") return BM_ALLOC;
  if (n == "soa_len" || n == "soa_field_ptr") return BM_LEN;
  if (n == "print" || n == "print_f64" || n == "print_i64" ||
      n == "print_u64" || n == "println" || n == "assert")
    return BM_PRINT;
  if (n.rfind("freshet.", 0) == 0) return BM_PURE;
  return BM_PURE;   // unknown non-user name: assume pure (registered in
                    // sema as error otherwise; user fns carry callee)
}

static int paramIndexOf(FnDecl* f, const std::string& n) {
  for (size_t i = 0; i < f->params.size(); ++i)
    if (f->params[i].name == n) return (int)i;
  return -1;
}

// Is `idx` (an index expression into a parameter) provably confined to
// [0, CP) where CP is a parameter of `f`?  Legal shapes:
//   - a bare loop var of an enclosing `for (v in 0..CP)` (CP an ident)
//   - a top-level sum with exactly one such loop-var term (+ literal 0)
// Returns the bound param index, or -1 when not provable.
static int confinedBy(FnDecl* f, Expr* idx, const std::vector<LoopCtx>& loops) {
  if (!idx) return -1;
  std::vector<Expr*> terms;
  std::function<void(Expr*)> flatten = [&](Expr* x) {
    if (x->k == Expr::BIN && x->op == "+") {
      flatten(x->lhs);
      flatten(x->rhs);
      return;
    }
    terms.push_back(x);
  };
  flatten(idx);
  int nVars = 0, bound = -1;
  for (Expr* t : terms) {
    int64_t c = 0;
    if (t->constant && evalConstInt(t, &c) && c == 0) continue;
    bool found = false;
    if (t->k == Expr::IDENT) {
      for (auto& lc : loops) {
        if (lc.var != t->name || !lc.loop || !lc.loop->forRange) continue;
        int64_t lo = 0;
        Expr* hi = lc.loop->rangeHi;
        if (!hi || hi->k != Expr::IDENT) continue;
        if (!evalConstInt(lc.loop->rangeLo, &lo) || lo != 0) continue;
        int cp = paramIndexOf(f, hi->name);
        if (cp < 0) continue;
        if (bound == -1) bound = cp;
        else if (bound != cp) return -1;
        found = true;
        break;
      }
    }
    if (!found) return -1;
    ++nVars;
  }
  if (nVars > 1) return -1;
  return bound;
}

// merge a bound value into per-param bound state:
//   -1 = permanently unbounded, -2 = unset, >=0 = bounded by that param
static void mergeBound(int& slot, int cp) {
  if (slot == -1) return;
  if (cp < 0) { slot = -1; return; }
  if (slot == -2) { slot = cp; return; }
  if (slot != cp) slot = -1;
}

void computeFootprints(Program* p) {
  if (g_footprintsDone) return;
  g_footprintsDone = true;
  for (auto* f : p->fns) {
    if (!f->body) continue;
    FnFootprint& fp = g_footprints[f];
    size_t np = f->params.size();
    fp.paramWrites.assign(np, false);
    fp.paramReads.assign(np, false);
    fp.writeBound.assign(np, -2);
    fp.readBound.assign(np, -2);
  }

  bool changed = true;
  int guard = 0;
  while (changed && guard++ < (int)p->fns.size() + 4) {
    changed = false;
    for (auto* f : p->fns) {
      if (!f->body) continue;
      FnFootprint& F = g_footprints[f];
      bool lc_ = false;
      auto touch = [&](auto& b) { if (!b) { b = true; lc_ = true; } };
      auto touchU = touch;

      // pointer rebinds (let mut w = p) resolve through this map
      AliasMap am;
      buildAliasMap(f->body, &am);

      // classify one memory target of a resolved root; returns true if the
      // root was placed, false when it denoted unknown memory
      auto placeRoot = [&](const std::string& rn, bool isWrite, Expr* idx,
                           const std::vector<LoopCtx>& loops) -> bool {
        std::set<std::string> rs;
        if (!resolveAlias(am, rn, rs)) {
          if (isWrite) touchU(F.writesUnknown);
          else touchU(F.readsUnknown);
          return false;
        }
        bool placed = false;
        for (const auto& t : rs) {
          int pi = paramIndexOf(f, t);
          if (pi >= 0) {
            placed = true;
            if (isWrite) {
              touch(F.paramWrites[pi]);
              mergeBound(F.writeBound[pi], confinedBy(f, idx, loops));
            } else {
              touch(F.paramReads[pi]);
              mergeBound(F.readBound[pi], confinedBy(f, idx, loops));
            }
            continue;
          }
          if (lookupGlobal(t)) {
            placed = true;
            if (isWrite) touchU(F.writesGlobal);
            else touchU(F.readsGlobal);
          }
          // otherwise: callee-local fresh memory — cannot escape
        }
        return placed;
      };

      // classify one memory-access expression node in f's body
      std::function<void(Expr*, const std::vector<LoopCtx>&)> readExpr =
          [&](Expr* e, const std::vector<LoopCtx>& loops) {
        if (!e) return;
        switch (e->k) {
          case Expr::INDEX: case Expr::FIELD: {
            Expr* r = rootOfChain(e);
            if (r && r->k == Expr::IDENT) {
              Expr* node = e;
              while (node && node->k == Expr::FIELD) node = node->base;
              Expr* idx = (node && node->k == Expr::INDEX) ? node->rhs : nullptr;
              placeRoot(r->name, false, idx, loops);
            }
            readExpr(e->base, loops);
            if (e->k == Expr::INDEX) readExpr(e->rhs, loops);
            return;
          }
          case Expr::IDENT: {
            if (e->globalRef) touch(F.readsGlobal);
            return;
          }
          case Expr::CALL: {
            if (e->callee) {
              FnDecl* g = e->callee;
              auto it = g_footprints.find(g);
              if (it != g_footprints.end()) {
                FnFootprint& G = it->second;
                if (G.writesGlobal) touchU(F.writesGlobal);
                if (G.readsGlobal) touchU(F.readsGlobal);
                if (G.writesUnknown) touchU(F.writesUnknown);
                for (size_t j = 0; j < g->params.size(); ++j) {
                  if (j >= e->args.size()) break;
                  bool give = j < e->argGive.size() && e->argGive[j];
                  if (!G.paramWrites[j] && !G.paramReads[j] && !give)
                    continue;
                  Expr* a = e->args[j];
                  if (a->k == Expr::UN && a->op == "&") a = a->rhs;
                  Expr* r = (a->k == Expr::INDEX) ? rootOfChain(a) : a;
                  if (r && r->k != Expr::IDENT) {
                    touchU(F.readsUnknown);
                    if (G.paramWrites[j] || give) touchU(F.writesUnknown);
                    continue;
                  }
                  if (!r) {
                    touchU(F.readsUnknown);
                    continue;
                  }
                  // alias-resolve the arg's root (let mut w = p; g(w))
                  std::set<std::string> rs;
                  if (!resolveAlias(am, r->name, rs)) {
                    touchU(F.writesUnknown);
                    touchU(F.readsUnknown);
                    continue;
                  }
                  for (const auto& t : rs) {
                    int pi = paramIndexOf(f, t);
                    if (pi < 0) {
                      if (lookupGlobal(t)) {
                        if (G.paramWrites[j] || give) touchU(F.writesGlobal);
                        if (G.paramReads[j]) touchU(F.readsGlobal);
                      }
                      continue;   // callee-local fresh memory
                    }
                    if (G.paramWrites[j]) {
                      touch(F.paramWrites[pi]);
                      if (G.writeBound[j] >= 0 &&
                          (size_t)G.writeBound[j] < e->args.size()) {
                        Expr* ext = e->args[G.writeBound[j]];
                        if (ext && ext->k == Expr::IDENT) {
                          int cp = paramIndexOf(f, ext->name);
                          mergeBound(F.writeBound[pi], cp);
                        } else {
                          mergeBound(F.writeBound[pi], -1);
                        }
                      } else {
                        mergeBound(F.writeBound[pi], -1);
                      }
                    }
                    if (G.paramReads[j]) {
                      touch(F.paramReads[pi]);
                      if (G.readBound[j] >= 0 &&
                          (size_t)G.readBound[j] < e->args.size()) {
                        Expr* ext = e->args[G.readBound[j]];
                        if (ext && ext->k == Expr::IDENT) {
                          int cp = paramIndexOf(f, ext->name);
                          mergeBound(F.readBound[pi], cp);
                        } else {
                          mergeBound(F.readBound[pi], -1);
                        }
                      } else {
                        mergeBound(F.readBound[pi], -1);
                      }
                    }
                    // give transfer: callee takes ownership → may free
                    if (give) {
                      touchU(F.writesUnknown);
                      if (F.writeBound[pi] != -1) { F.writeBound[pi] = -1; lc_ = true; }
                    }
                  }
                }
              }
            } else {
              switch (classifyBuiltin(e->name)) {
                case BM_FREE: case BM_SOA_FREE: {
                  if (!e->args.empty()) {
                    Expr* a = e->args[0];
                    if (a->k == Expr::UN && a->op == "&") a = a->rhs;
                    Expr* r = (a->k == Expr::INDEX) ? rootOfChain(a) : a;
                    if (r && r->k == Expr::IDENT) {
                      std::set<std::string> rs;
                      if (!resolveAlias(am, r->name, rs))
                        touchU(F.writesUnknown);
                      else if (rs.empty()) { /* fresh local: own free */ }
                      else for (const auto& t : rs) {
                        int pi = paramIndexOf(f, t);
                        if (pi >= 0) {
                          touchU(F.writesUnknown);
                          touch(F.paramWrites[pi]);
                          F.writeBound[pi] = -1;
                        } else if (lookupGlobal(t)) touchU(F.writesGlobal);
                      }
                    } else touchU(F.writesUnknown);
                  }
                  break;
                }
                case BM_MEMCPY: case BM_MEMSET: {
                  if (e->args.size() >= 2) {
                    Expr* d = e->args[0];
                    if (d->k == Expr::UN && d->op == "&") d = d->rhs;
                    Expr* r = (d->k == Expr::INDEX) ? rootOfChain(d) : d;
                    if (r && r->k == Expr::IDENT) {
                      std::set<std::string> rs;
                      if (!resolveAlias(am, r->name, rs))
                        touchU(F.writesUnknown);
                      else if (rs.empty()) { /* fresh */ }
                      else for (const auto& t : rs) {
                        int pi = paramIndexOf(f, t);
                        if (pi >= 0) {
                          touch(F.paramWrites[pi]);
                          F.writeBound[pi] = -1;
                        } else if (lookupGlobal(t)) touchU(F.writesGlobal);
                      }
                    } else touchU(F.writesUnknown);
                    if (e->name == "memcpy") {
                      Expr* sr = e->args[1];
                      if (sr->k == Expr::UN && sr->op == "&") sr = sr->rhs;
                      Expr* rr = (sr->k == Expr::INDEX) ? rootOfChain(sr) : sr;
                      if (rr && rr->k == Expr::IDENT) {
                        std::set<std::string> rs2;
                        if (resolveAlias(am, rr->name, rs2))
                          for (const auto& t : rs2) {
                            int pj = paramIndexOf(f, t);
                            if (pj >= 0) touch(F.paramReads[pj]);
                            else if (lookupGlobal(t)) touchU(F.readsGlobal);
                          }
                      }
                    }
                  }
                  break;
                }
                case BM_LEN: {
                  if (!e->args.empty() && e->args[0]->k == Expr::IDENT) {
                    int pi = paramIndexOf(f, e->args[0]->name);
                    if (pi >= 0) touch(F.paramReads[pi]);
                  }
                  break;
                }
                default: break;
              }
            }
            for (auto* a : e->args) readExpr(a, loops);
            return;
          }
          case Expr::BIN:
            readExpr(e->lhs, loops);
            readExpr(e->rhs, loops);
            return;
          case Expr::UN:
            readExpr(e->rhs, loops);
            return;
          case Expr::CAST:
            readExpr(e->base, loops);
            return;
          case Expr::SLIT:
            for (auto* a : e->args) readExpr(a, loops);
            return;
          default: return;
        }
      };

      std::function<void(Stmt*, const std::vector<LoopCtx>&)> walk =
          [&](Stmt* s, const std::vector<LoopCtx>& loops) {
        if (!s) return;
        switch (s->k) {
          case Stmt::LET:
            readExpr(s->init, loops);
            break;
          case Stmt::ASSIGN: {
            Expr* node = s->lhs;
            while (node && node->k == Expr::FIELD) node = node->base;
            if (node && node->k == Expr::INDEX && node->base &&
                node->base->k == Expr::IDENT) {
              placeRoot(node->base->name, true, node->rhs, loops);
            } else {
              Expr* r = rootOfChain(s->lhs);
              if (r && r->k == Expr::IDENT) {
                // scalar/struct write: whole-target write of that name
                std::set<std::string> rs;
                if (!resolveAlias(am, r->name, rs)) {
                  touchU(F.writesUnknown);
                } else {
                  for (const auto& t : rs) {
                    int pi = paramIndexOf(f, t);
                    if (pi >= 0) {
                      touch(F.paramWrites[pi]);
                      F.writeBound[pi] = -1;
                    } else if (lookupGlobal(t)) {
                      touchU(F.writesGlobal);
                    }
                  }
                }
              } else {
                touchU(F.writesUnknown);
              }
            }
            readExpr(s->rhs, loops);
            break;
          }
          case Stmt::RETURN:
            readExpr(s->retVal, loops);
            break;
          case Stmt::DEFER:
            readExpr(s->deferExpr, loops);
            break;
          case Stmt::EXPR:
            readExpr(s->expr, loops);
            break;
          case Stmt::IF:
            readExpr(s->cond, loops);
            walk(s->thenB, loops);
            walk(s->elseB, loops);
            break;
          case Stmt::WHILE:
            readExpr(s->cond, loops);
            walk(s->body, loops);
            break;
          case Stmt::PARFOR:
            walk(s->body, loops);
            break;
          case Stmt::FOR: {
            std::vector<LoopCtx> nl = loops;
            if (s->forRange && !s->ivar.empty())
              nl.push_back({s->ivar, s, canon(s->rangeHi)});
            readExpr(s->forInit, loops);
            readExpr(s->forCond, loops);
            readExpr(s->forStep, loops);
            walk(s->body, nl);
            break;
          }
          case Stmt::BLOCK:
            for (auto* st : s->stmts) walk(st, loops);
            break;
          default: break;
        }
      };
      walk(f->body, {});
      if (lc_) changed = true;
    }
  }

  // a declared-pure function must not write; if the walk found writes,
  // demote (should not happen — sema validates @pure).
  for (auto* f : p->fns) {
    if (!f->body || !f->isPure) continue;
    FnFootprint& F = g_footprints[f];
    bool anyWrite = F.writesGlobal || F.writesUnknown;
    for (char w : F.paramWrites) anyWrite = anyWrite || w;
    if (anyWrite) f->isPure = false;
  }
}

FnFootprint* footprintOf(FnDecl* f) {
  auto it = g_footprints.find(f);
  return it == g_footprints.end() ? nullptr : &it->second;
}

// ------------------------------------------------------------- accesses
// decompose a pointer-PASSING call argument into (root, idxExpr):
//   &r[E], r + E, E + r   (r a pointer ident)  → slice at r[E]
//   bare r                                        → whole array
// (plain r[E] args are element reads, handled by the DIRECT path)
static bool addrOfArg(Expr* a, std::string* root, Expr** idx, bool* whole) {
  *whole = false;
  if (a->k == Expr::UN && a->op == "&") {
    a = a->rhs;
    if (a->k == Expr::INDEX && a->base && a->base->k == Expr::IDENT) {
      *root = a->base->name;
      *idx = a->rhs;
      return true;
    }
    if (a->k == Expr::IDENT) {   // &scalar / &struct: whole-binding
      *root = a->name;
      *idx = nullptr;
      *whole = true;
      return true;
    }
    return false;
  }
  if (a->k == Expr::BIN && a->op == "+") {
    Expr *l = a->lhs, *r = a->rhs;
    if (l->k == Expr::IDENT && l->ty && l->ty->isPtr()) {
      *root = l->name;
      *idx = r;
      return true;
    }
    if (r->k == Expr::IDENT && r->ty && r->ty->isPtr()) {
      *root = r->name;
      *idx = l;
      return true;
    }
    return false;
  }
  if (a->k == Expr::IDENT && a->ty && a->ty->isPtr()) {
    *root = a->name;
    *idx = nullptr;
    *whole = true;
    return true;
  }
  return false;
}

// classify a DIRECT root[<idx>] access: canonical idx, affine-literal flag,
// and tier-3 slice shape (iv*C + inner-loop vars bounded by C).
static void classifyDirect(Access* acc, Expr* idx, const std::string& iv,
                          const std::vector<LoopCtx>& loops) {
  acc->idx = canonN(idx, iv);
  if (iv.empty()) return;
  std::set<std::string> names;
  collectIdents(idx, names);
  acc->hasIvar = names.count(iv) != 0;
  acc->affineLit = affineIvarLiteral(idx, iv);
  if (!acc->hasIvar) return;
  // tier-3 decomposition: idx = (iv*C)? + remainder, remainder terms must
  // be inner loop vars whose bound canon == C
  std::vector<Expr*> terms;
  std::function<void(Expr*)> flatten = [&](Expr* x) {
    if (x->k == Expr::BIN && x->op == "+") { flatten(x->lhs); flatten(x->rhs); return; }
    terms.push_back(x);
  };
  terms.clear();
  flatten(idx);
  std::string cCanon;
  int varTerms = 0;
  bool bad = false;
  for (Expr* t : terms) {
    if (t->k == Expr::BIN && t->op == "*") {
      Expr *l = t->lhs, *r = t->rhs;
      Expr* cexpr = nullptr;
      if (l->k == Expr::IDENT && l->name == iv) cexpr = r;
      else if (r->k == Expr::IDENT && r->name == iv) cexpr = l;
      if (cexpr && cCanon.empty()) {
        cCanon = canon(cexpr);
        ++varTerms;
        continue;
      }
    }
    if (t->k == Expr::IDENT && t->name == iv) {
      if (cCanon.empty()) { cCanon = "1"; ++varTerms; continue; }
    }
    if (t->k == Expr::IDENT) {
      bool matched = false;
      for (auto& lc : loops)
        if (lc.var == t->name && lc.hiCanon == cCanon) { matched = true; break; }
      if (matched) continue;
    }
    bad = true;
    break;
  }
  if (!bad && varTerms == 1 && !cCanon.empty()) {
    acc->t3 = true;
    acc->t3c = cCanon;   // slice multiplier for tier-3 matching
  }
}

void collectBody(Stmt* body, const std::string& iv, FnDecl* fn, BodyInfo* out) {
  std::vector<LoopCtx> noLoops;

  // pointer rebinds in this body resolve through an alias map so that
  // `let mut w = re; w[j] = ...` is attributed to the real root `re`
  AliasMap am;
  buildAliasMap(body, &am);

  auto placeAccess = [&](const std::string& rn, const Access& a) {
    std::set<std::string> rs;
    if (!resolveAlias(am, rn, rs)) {
      if (a.isWrite) out->unknownWrite = true;
      else out->unknownRead = true;
      return;
    }
    for (const auto& t : rs) {
      if (a.isWrite) out->writes[t].push_back(a);
      else out->reads[t].push_back(a);
    }
  };

  // read classification of one expression (context-free parts)
  std::function<void(Expr*, const std::vector<LoopCtx>&)> readAcc =
      [&](Expr* e, const std::vector<LoopCtx>& loops) {
    if (!e) return;
    switch (e->k) {
      case Expr::INDEX: case Expr::FIELD: {
        Expr* r = rootOfChain(e);
        if (r && r->k == Expr::IDENT) {
          if (r->ty && r->ty->k == Type::SOA) out->escapedNames.insert(r->name);
          Expr* node = e;
          while (node && node->k == Expr::FIELD) node = node->base;
          Access a{};
          a.isWrite = false;
          a.loc = e->loc;
          if (node && node->k == Expr::INDEX && node->rhs) {
            a.kind = Access::DIRECT;
            classifyDirect(&a, node->rhs, iv, loops);
          } else {
            a.kind = Access::SCALAR;   // struct value in its own storage
          }
          placeAccess(r->name, a);
        }
        // fresh-call bases (malloc(...)[i]) cannot conflict: skipped
        readAcc(e->base, loops);
        if (e->k == Expr::INDEX) readAcc(e->rhs, loops);
        return;
      }
      case Expr::IDENT: {
        // binding reads: scalar/struct values are storage; pointers are not
        if (e->ty && (e->ty->isPtr() || e->ty->k == Type::SOA)) return;
        if (e->globalRef) {
          Access a{};
          a.kind = Access::SCALAR;
          a.loc = e->loc;
          out->reads[e->name].push_back(a);
        } else if (e->sym && (e->sym->k == Sym::VAR || e->sym->k == Sym::PARAM)) {
          if (e->ty && e->ty->k == Type::STRUCT) {
            Access a{};
            a.kind = Access::SCALAR;
            a.loc = e->loc;
            out->reads[e->name].push_back(a);
          } else {
            Access a{};
            a.kind = Access::SCALAR;
            a.loc = e->loc;
            out->reads[e->name].push_back(a);
          }
        }
        return;
      }
      case Expr::UN: {
        if (e->op == "&") {
          Expr* r = rootOfChain(e->rhs);
          if (r && r->k == Expr::IDENT) out->escapedNames.insert(r->name);
        }
        readAcc(e->rhs, loops);
        return;
      }
      case Expr::CALL: {
        for (size_t j = 0; j < e->args.size(); ++j) {
          if (j < e->argGive.size() && e->argGive[j]) out->hasGive = true;
        }
        if (e->callee) {
          FnFootprint* G = footprintOf(e->callee);
          if (!G) {
            out->unknownWrite = true;
            out->unknownRead = true;
          } else {
            if (G->writesGlobal) out->unknownWrite = true;
            if (G->readsGlobal) out->unknownRead = true;
            if (G->writesUnknown) { out->unknownWrite = true; out->unknownRead = true; }
            if (G->readsUnknown) out->unknownRead = true;
          }
          FnDecl* g = e->callee;
          for (size_t j = 0; j < g->params.size() && j < e->args.size(); ++j) {
            bool writes = G && G->paramWrites[j];
            bool reads = G && G->paramReads[j];
            if (!writes && !reads) continue;
            // soa descriptor arg: any plane reachable → wildcard
            if (e->args[j]->ty && e->args[j]->ty->k == Type::SOA) {
              Access a{};
              a.kind = Access::WILDCARD;
              a.isWrite = writes;
              a.loc = e->loc;
              if (e->args[j]->k == Expr::IDENT) {
                placeAccess(e->args[j]->name, a);
              } else {
                out->unknownWrite = out->unknownWrite || writes;
                out->unknownRead = out->unknownRead || reads;
              }
              continue;
            }
            std::string root;
            Expr* idx = nullptr;
            bool whole = false;
            if (!addrOfArg(e->args[j], &root, &idx, &whole)) {
              // element read passed by value / struct copy: handled as
              // DIRECT read by the INDEX walker; if it is something odd,
              // be conservative for writers
              if (writes) out->unknownWrite = true;
              continue;
            }
            Access a{};
            a.isWrite = writes;
            a.loc = e->loc;
            if (whole) {
              a.kind = Access::WHOLE;
            } else {
              // slice: idx must be iv*C or bare iv; else wildcard
              std::string cCanon;
              bool okShape = false;
              if (idx) {
                if (idx->k == Expr::IDENT && idx->name == iv) {
                  cCanon = "1";
                  okShape = true;
                } else if (idx->k == Expr::BIN && idx->op == "*") {
                  Expr *l = idx->lhs, *r = idx->rhs;
                  if (l->k == Expr::IDENT && l->name == iv) { cCanon = canon(r); okShape = true; }
                  else if (r->k == Expr::IDENT && r->name == iv) { cCanon = canon(l); okShape = true; }
                }
              }
              if (!okShape) {
                a.kind = Access::WILDCARD;
              } else {
                a.kind = Access::SLICE;
                a.idx = cCanon;
                a.hasIvar = true;
                int bnd = writes ? G->writeBound[j] : G->readBound[j];
                if (bnd >= 0 && (size_t)bnd < e->args.size() &&
                    canon(e->args[bnd]) == cCanon)
                  a.bounded = true;
              }
            }
            if (writes) {
              placeAccess(root, a);          // a.isWrite == true
            }
            if (reads) {
              Access b = a;
              b.isWrite = false;
              placeAccess(root, b);
            }
          }
        } else {
          // builtins with memory effects
          switch (classifyBuiltin(e->name)) {
            case BM_FREE: case BM_SOA_FREE: {
              if (!e->args.empty()) {
                std::string root;
                Expr* idx;
                bool whole;
                if (addrOfArg(e->args[0], &root, &idx, &whole)) {
                  Access a{};
                  a.kind = Access::WILDCARD;
                  a.isWrite = true;
                  a.loc = e->loc;
                  placeAccess(root, a);
                } else out->unknownWrite = true;
              }
              break;
            }
            case BM_MEMCPY: case BM_MEMSET: {
              if (e->args.size() >= 2) {
                std::string root;
                Expr* idx;
                bool whole;
                if (addrOfArg(e->args[0], &root, &idx, &whole)) {
                  Access a{};
                  a.kind = whole ? Access::WHOLE : Access::WILDCARD;
                  a.isWrite = true;
                  a.loc = e->loc;
                  placeAccess(root, a);
                } else out->unknownWrite = true;
                if (e->name == "memcpy" &&
                    addrOfArg(e->args[1], &root, &idx, &whole)) {
                  Access a{};
                  a.kind = whole ? Access::WHOLE : Access::WILDCARD;
                  a.isWrite = false;
                  a.loc = e->loc;
                  placeAccess(root, a);
                }
              }
              break;
            }
            case BM_LEN: {
              if (!e->args.empty() && e->args[0]->k == Expr::IDENT) {
                Access a{};
                a.kind = Access::SCALAR;   // descriptor field read
                a.isWrite = false;
                a.loc = e->loc;
                placeAccess(e->args[0]->name, a);
              }
              break;
            }
            case BM_PRINT:
              out->hasPrint = true;
              break;
            default: break;
          }
        }
        for (auto* a : e->args) readAcc(a, loops);
        return;
      }
      case Expr::BIN:
        readAcc(e->lhs, loops);
        readAcc(e->rhs, loops);
        return;
      case Expr::CAST:
        readAcc(e->base, loops);
        return;
      case Expr::SLIT:
        for (auto* a : e->args) readAcc(a, loops);
        return;
      default: return;
    }
  };

  std::function<void(Stmt*, const std::vector<LoopCtx>&)> walk =
      [&](Stmt* s, const std::vector<LoopCtx>& loops) {
    if (!s) return;
    out->nodes += 4;
    switch (s->k) {
      case Stmt::LET: {
        if (s->init) {
          walkExpr(s->init, [&](Expr* x) {
            if (x->k == Expr::BIN && x->op != "==" && x->op != "!=" &&
                x->op != "<" && x->op != ">" && x->op != "<=" && x->op != ">=" &&
                x->op != "&&" && x->op != "||")
              out->laneWork += 1;
            if (x->k == Expr::INDEX || x->k == Expr::FIELD) out->laneWork += 1;
            out->nodes += 1;
          });
          readAcc(s->init, loops);
        }
        Access a{};
        a.kind = Access::SCALAR;
        a.isWrite = true;
        a.loc = s->loc;
        out->writes[s->var].push_back(a);
        break;
      }
      case Stmt::ASSIGN: {
        out->laneWork += 1;   // store
        walkExpr(s->rhs, [&](Expr* x) {
          if (x->k == Expr::BIN && x->op != "==" && x->op != "!=" &&
              x->op != "<" && x->op != ">" && x->op != "<=" && x->op != ">=" &&
              x->op != "&&" && x->op != "||")
            out->laneWork += 1;
          if (x->k == Expr::INDEX || x->k == Expr::FIELD) out->laneWork += 1;
          out->nodes += 1;
        });
        Expr* node = s->lhs;
        while (node && node->k == Expr::FIELD) node = node->base;
        if (node && node->k == Expr::INDEX && node->base &&
            node->base->k == Expr::IDENT) {
          Access a{};
          a.kind = Access::DIRECT;
          a.isWrite = true;
          a.loc = s->loc;
          classifyDirect(&a, node->rhs, iv, loops);
          placeAccess(node->base->name, a);
        } else {
          Expr* r = rootOfChain(s->lhs);
          if (r && r->k == Expr::IDENT) {
            Access a{};
            a.isWrite = true;
            a.loc = s->loc;
            if (s->lhs->k == Expr::IDENT) a.kind = Access::SCALAR;
            else a.kind = Access::WILDCARD;   // field of scalar struct
            out->writes[r->name].push_back(a);
          } else {
            out->unknownWrite = true;
          }
        }
        readAcc(s->rhs, loops);
        break;
      }
      case Stmt::RETURN:
        out->hasReturn = true;
        readAcc(s->retVal, loops);
        break;
      case Stmt::DEFER:
        readAcc(s->deferExpr, loops);
        break;
      case Stmt::EXPR:
        walkExpr(s->expr, [&](Expr* x) {
          if (x->k == Expr::BIN && x->op != "==" && x->op != "!=" &&
              x->op != "<" && x->op != ">" && x->op != "<=" && x->op != ">=" &&
              x->op != "&&" && x->op != "||")
            out->laneWork += 1;
          if (x->k == Expr::INDEX || x->k == Expr::FIELD) out->laneWork += 1;
          out->nodes += 1;
        });
        readAcc(s->expr, loops);
        break;
      case Stmt::IF: {
        out->branches += 1;
        out->nodes += 2;
        readAcc(s->cond, loops);
        walk(s->thenB, loops);
        walk(s->elseB, loops);
        break;
      }
      case Stmt::MATCH: {   // Phase 1d: every arm is a possible path
        out->branches += (int)s->matchArms.size();
        out->nodes += 2;
        readAcc(s->matchOn, loops);
        for (auto& arm : s->matchArms) walk(arm.body, loops);
        break;
      }
      case Stmt::BREAK:      // Phase 1a: fusion must not break trip counts —
        out->hasBreak = true;// conservatively veto shapes containing them
        break;
      case Stmt::CONTINUE:
        out->hasBreak = true;
        break;
      case Stmt::WHILE:
        out->branches += 1;
        readAcc(s->cond, loops);
        walk(s->body, loops);
        break;
      case Stmt::PARFOR: {
        out->hasParfor = true;
        readAcc(s->plo, loops);
        readAcc(s->phi, loops);
        for (auto& c : s->clauses) readAcc(c.e, loops);
        walk(s->body, loops);
        break;
      }
      case Stmt::FOR: {
        out->branches += 1;
        std::vector<LoopCtx> nl = loops;
        if (s->forRange && !s->ivar.empty() && s->ivar != iv)
          nl.push_back({s->ivar, s, canon(s->rangeHi)});
        readAcc(s->rangeLo, loops);
        readAcc(s->rangeHi, loops);
        readAcc(s->forInit, loops);
        readAcc(s->forCond, loops);
        readAcc(s->forStep, loops);
        walk(s->body, nl);
        break;
      }
      case Stmt::BLOCK:
        for (auto* st : s->stmts) walk(st, loops);
        break;
      default: break;
    }
  };

  walk(body, noLoops);
  (void)fn;
}


// ------------------------------------------------------------- proof log
void proofLogLine(const std::string& pass, const std::string& premises,
                  FnDecl* fn, SrcLoc loc) {
  if (!g_opts.proofLogOn) return;
  static FILE* f = nullptr;
  static bool tried = false;
  if (!tried) {
    tried = true;
    std::string path = g_opts.proofLogPath.empty()
                           ? g_opts.output + ".prooflog"
                           : g_opts.proofLogPath;
    f = fopen(path.c_str(), "a");
  }
  if (!f) return;
  fprintf(f, "{\"pass\":\"%s\",\"fn\":\"%s\",\"file\":\"%s\",\"line\":%d,"
             "\"premises\":\"%s\"}\n",
          pass.c_str(), fn ? fn->name.c_str() : "", loc.file.c_str(),
          loc.line, premises.c_str());
  fflush(f);
}

} // namespace optutil
} // namespace freshet

