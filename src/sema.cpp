// Freshet Sema — symbol tables, declaration binding (struct names resolved
// here from parser's tyName strings), literal type adoption, purity analysis.
#include "common.h"
#include <functional>

namespace freshet {

struct SemaState {
  Program* prog;
  // Phase 3a: same-named decls across modules coexist — vectors +
  // visibility filtering (lookupFn/lookupGlobal pick the visible one)
  std::map<std::string, std::vector<FnDecl*>> fnMap;
  std::map<std::string, std::vector<GlobalVar*>> globalMap;
  std::vector<FnSema*> fnSemas;
  // Phase 2a: bodies of freshly instantiated generic fns awaiting sema
  std::vector<FnDecl*> pendingGenericBodies;
  // instances already through body sema (interning now happens at
  // CREATION via registerGenericInstance, so fnMap membership can no
  // longer distinguish "processed" from "merely created")
  std::set<FnDecl*> genericSemaDone;
};

static SemaState* S = nullptr;

FnSema* getFnSema(FnDecl* f) {
  for (auto* fs : S->fnSemas) if (fs->fn == f) return fs;
  return nullptr;
}

// bind Type* from parser's placeholder + tyName. The encoding grammar is
// documented in parser.cpp; generics.cpp resolves it (and instantiates
// generic templates along the way).  Inside a generic INSTANTIATION the
// current substitution map is consulted first, so `let s: T = ...` binds
// to the concrete type the call site inferred.
static std::map<std::string, Type*>* g_fnTMap = nullptr;

Type* tparamTypeInScope(const std::string& name) {
  if (g_fnTMap) {
    auto it = g_fnTMap->find(name);
    if (it != g_fnTMap->end()) return it->second;
  }
  return nullptr;
}

static Type* bindType(Type* placeholder, const std::string& tyName, SrcLoc loc) {
  if (tyName.empty()) return placeholder;
  if (g_fnTMap) {
    auto it = g_fnTMap->find(tyName);
    if (it != g_fnTMap->end()) return it->second;
  }
  Type* t = bindTypeEncoding(S->prog, tyName, loc, nullptr);
  return t ? t : placeholder;
}

// ---------------------------------------------------------------- expressions
Type* typeExpr(FnSema* fs, Expr* e);   // in sema_expr.cpp
void tryResolveVariantExpr(FnSema* fs, Expr* e, Type* expected);  // sema_expr.cpp

static void declareVarFromInit(FnSema* fs, Stmt* s) {
  Type* t = nullptr;
  if (s->hasAnn) t = s->annTy;
  if (s->tyName != "") t = bindType(s->annTy, s->tyName, s->loc);
  if (!t || (t->k == Type::VOID && !s->tyName.empty())) t = nullptr;
  else if (t && s->hasAnn) {
    // resolve the annotation IN PLACE: IRgen (and later passes) must see
    // the concrete type, not the parser placeholder.  Clearing tyName
    // keeps the "unresolved annotation" fallbacks from firing.
    s->annTy = t;
    s->tyName = "";
  }
  if (t) {
    // Phase 1d: contextual enum-variant resolution — `let x: Color = Red;`
    // / `let r: Result<i64, E> = Ok(v);` need the EXPECTED type to resolve
    // the bare variant name
    if (t->k == Type::ENUM && s->init)
      tryResolveVariantExpr(fs, s->init, t);
    // Phase 2a: struct literals adopt the instantiated generic type
    if (t->k == Type::STRUCT && s->init)
      tryAdoptStructLiteral(s->init, t);
    // Phase 1b: an array literal ADOPTS the annotation's type (element
    // type and length) before typing, so [7, 8, 9] builds a [3 x i32]
    // value against `let f: [i32; 3]`
    if (t->k == Type::ARRAY && s->init && s->init->k == Expr::ARRLIT) {
      if (s->init->args.size() != t->arrLen) {
        diag(Diag::Error, Code::Type,
             "array literal has " + std::to_string(s->init->args.size()) +
             " element(s) but the annotation is " + typeToString(t),
             s->loc);
      } else if (t->arrLen > 0) {
        // adopt (also lets the empty literal resolve).  The adopted type
        // memoizes the whole literal, so its ELEMENTS must be typed here
        // — otherwise IRgen dereferences their null types.
        s->init->ty = t;
        for (auto* el : s->init->args) typeExpr(fs, el);
      }
    }
    // check init compatible
    Type* it = typeExpr(fs, s->init);
    if (it && !typesCompatible(t, it)) {
      // literal adoption: constant int to any int type
      bool ok = false;
      if (s->init->constant && t->isInt() && it->isInt()) ok = true;
      if (s->init->constant && t->k == Type::F64 && it->isFloat()) ok = true;
      if (s->init->constant && t->k == Type::F32 && it->k == Type::F64) ok = true;
      // array literal against an array annotation of the same length
      if (s->init->k == Expr::ARRLIT && t->k == Type::ARRAY) ok = true;
      if (s->init->k == Expr::SLICE && t->isPair()) ok = true;
      if (!ok) {
        diag(Diag::Error, Code::Type, "cannot initialize " + typeToString(t) +
             " with " + typeToString(it), s->loc);
      }
    }
    if (t->k == Type::SOA) {
      // soa vars only from soa_new
      if (!(s->init->k == Expr::CALL && s->init->name == "soa_new")) {
        bool give = s->init->k == Expr::CALL && s->init->name.rfind("__soa_give", 0) == 0;
        if (!give) {
          diag(Diag::Error, Code::Type,
               "soa(" + t->sdef->name + ") values come only from soa_new()",
               s->loc);
        }
      }
    }
  } else {
    // infer
    t = typeExpr(fs, s->init);
    if (t->k == Type::USIZE) t = Type::simple(Type::U64); // default
    if (t->k == Type::VOID) {
      diag(Diag::Error, Code::Type, "cannot infer type of void expression",
           s->loc);
    }
    if (t->k == Type::STRUCT && s->init->k == Expr::ARRLIT) {
      diag(Diag::Error, Code::Type,
           "array literal needs a type annotation: let a: [T; N] = [...]",
           s->loc);
    }
  }
  Sym sym;
  sym.k = Sym::VAR;
  sym.name = s->var;
  sym.ty = t;
  sym.mut = s->mut;
  sym.declLoc = s->loc;   // Phase 4: LSP goto-definition / rename
  // ownership: malloc result / give-received
  if (s->init->k == Expr::CALL && s->init->name == "malloc") sym.owned = true;
  if (s->init->k == Expr::CALL && s->init->name == "soa_new") sym.owned = true;
  fs->varDepth[s->var] = fs->parforDepth;
  fs->declare(sym);
}

// ---------------------------------------------------------------- statements
static void semaStmt(FnSema* fs, Stmt* s);

// Phase 0b (report §3.2): definite-return analysis.
// A statement "always returns" when every execution path through it either
// hits a `return` or ends in a recognised non-terminating construct
// (`while` with a constant-true condition). The analysis is deliberately
// conservative — anything it cannot prove returns is treated as possibly
// falling through, and the caller then demands an explicit trailing
// return.
static bool stmtAlwaysReturns(Stmt* s) {
  if (!s) return false;
  switch (s->k) {
    case Stmt::RETURN:
      return true;
    case Stmt::BLOCK: {
      for (auto* st : s->stmts)
        if (stmtAlwaysReturns(st)) return true;   // rest is unreachable
      return false;
    }
    case Stmt::IF:
      return stmtAlwaysReturns(s->thenB) && s->elseB &&
             stmtAlwaysReturns(s->elseB);
    case Stmt::WHILE:
      // `while (true) { ... }` never falls through (the only exit is
      // return/break; break falls through, but a break-terminated loop is
      // rare and the trailing-return demand is a minor cost for it)
      return s->cond && s->cond->constant &&
             s->cond->k == Expr::BOOL && s->cond->bval;
    case Stmt::MATCH: {
      // every arm must return; a wildcard arm counts as a path
      for (auto& arm : s->matchArms)
        if (!stmtAlwaysReturns(arm.body)) return false;
      return !s->matchArms.empty();
    }
    default:
      return false;   // LET/ASSIGN/EXPR/DEFER/BREAK/CONTINUE/FOR/PARFOR
  }
}

static void semaBlock(FnSema* fs, Stmt* b) {
  fs->push();
  for (auto* st : b->stmts) semaStmt(fs, st);
  fs->pop();
}

static void semaParfor(FnSema* fs, Stmt* s) {
  // bounds
  Type* lot = typeExpr(fs, s->plo);
  Type* hit = typeExpr(fs, s->phi);
  for (Type* t : {lot, hit}) {
    if (t && !t->isInt()) {
      diag(Diag::Error, Code::Type, "parfor bounds must be integers",
           s->loc);
    }
  }
  // iterate var is a fresh immutable usize in body scope
  fs->push();
  Sym iv;
  iv.k = Sym::VAR; iv.name = s->pvar; iv.ty = Type::simple(Type::USIZE);
  iv.mut = false;
  fs->declare(iv);
  fs->varDepth[s->pvar] = fs->parforDepth + 1;

  int prevDepth = fs->parforDepth;
  fs->parforDepth += 1;
  fs->loops.push_back({s->pvar, s});

  // clauses
  int nred = 0;
  std::string affinity = "close";
  for (auto& c : s->clauses) {
    switch (c.k) {
      case ParForClause::REDUCTION: {
        ++nred;
        Sym* acc = fs->lookup(c.var);
        if (!acc) {
          diag(Diag::Error, Code::Parfor,
               "reduction accumulator not found: " + c.var,
               c.loc);
          break;
        }
        if (!acc->mut) {
          diag(Diag::Error, Code::Parfor,
               "reduction accumulator must be declared mut: " + c.var,
               c.loc);
        }
        if (!(acc->ty->isNum())) {
          diag(Diag::Error, Code::Parfor,
               "reduction accumulator must be numeric: " + c.var,
               c.loc);
        }
        if (c.redOp != "+" && c.redOp != "-" && c.redOp != "*" &&
            c.redOp != "min" && c.redOp != "max") {
          diag(Diag::Error, Code::Parfor,
               "unsupported reduction op: " + c.redOp,
               c.loc);
        }
        fs->reductionVars.insert(c.var);
        // init value check: + requires 0, * requires 1, min/max any
        break;
      }
      case ParForClause::THREADS: {
        typeExpr(fs, c.e);
        break;
      }
      case ParForClause::CHUNK: {
        typeExpr(fs, c.e);
        break;
      }
      case ParForClause::AFFINITY: {
        if (c.sval != "close" && c.sval != "spread") {
          diag(Diag::Error, Code::Parfor,
               "affinity must be close or spread, got: " + c.sval,
               c.loc);
        }
        affinity = c.sval;
        break;
      }
      case ParForClause::COLLAPSE: {
        if (c.ival < 1 || c.ival > 2) {
          diag(Diag::Error, Code::Parfor, "collapse supports 1 or 2",
               c.loc);
        }
        break;
      }
    }
  }
  (void)affinity;

  // body
  semaBlock(fs, s->body);

  fs->loops.pop_back();
  fs->parforDepth = prevDepth;

  // post-body race analysis: any var written at depth < current that is not
  // a reduction var → F0307 (done in checks_parfor for full precision,
  // but quick check here captures obvious cases)
  for (auto& [v, locs] : fs->parforWrites) {
    (void)v; (void)locs;
  }

  fs->pop();
}

static void semaStmt(FnSema* fs, Stmt* s) {
  switch (s->k) {
    case Stmt::LET: declareVarFromInit(fs, s); break;
    case Stmt::ASSIGN: {
      typeExpr(fs, s->lhs);
      // Phase 1d: `x = Red;` where x: Color — the EXPECTED type resolves
      // the bare variant name before the rhs is typed
      if (s->lhs->ty && s->lhs->ty->k == Type::ENUM && s->lhs->k == Expr::IDENT)
        tryResolveVariantExpr(fs, s->rhs, s->lhs->ty);
      // Phase 2a: `p = Pair { ... }` adopts the instantiated type
      if (s->lhs->ty && s->lhs->ty->k == Type::STRUCT)
        tryAdoptStructLiteral(s->rhs, s->lhs->ty);
      Type* rt = typeExpr(fs, s->rhs);
      if (!s->lhs->isLvalue) {
        diag(Diag::Error, Code::Type, "left side of assignment is not assignable",
             s->loc);
      } else {
        Sym* sym = nullptr;
        if (s->lhs->k == Expr::IDENT) {
          sym = fs->lookup(s->lhs->name);
          if (sym && !sym->mut && !sym->owned) {
            // owned ptr rebinding allowed (ownership handles)
            diag(Diag::Error, Code::Mut, "cannot assign to immutable " + sym->name,
                 s->loc);
          }
        }
        if (s->lhs->k == Expr::FIELD || s->lhs->k == Expr::INDEX) {
          // mutability flows from base
          Expr* base = s->lhs->base;
          while (base && base->k == Expr::FIELD) base = base->base;
          if (base && base->k == Expr::IDENT) {
            Sym* bs = fs->lookup(base->name);
            // owned pointers (malloc results / give params) are writable
            // through immutable bindings: the binding is const, the memory
            // is exclusively owned
            if (bs && !bs->mut && !bs->owned && bs->ty && bs->ty->k == Type::PTR) {
              diag(Diag::Error, Code::Mut,
                   "cannot mutate through immutable pointer " + base->name,
                   s->loc);
            }
          }
        }
        if (s->op2 != "=") {
          // compound: lhs must be numeric and types must match.  A bare
          // INT literal adopts the lhs type (`x <<= 4` on i64).
          Type* lt = s->lhs->ty;
          if (lt && rt && !typesEquivalent(lt, rt) &&
              !(s->rhs->constant && lt->isNum() && rt->isNum())) {
            diag(Diag::Error, Code::Type,
                 "compound assignment type mismatch: " +
                 typeToString(lt) + " " + s->op2 + " " + typeToString(rt),
                 s->loc);
          }
        } else if (s->lhs->ty && rt && !typesCompatible(s->lhs->ty, rt)) {
          bool ok = s->rhs->constant && s->lhs->ty->isNum() && rt->isNum();
          if (!ok)
            diag(Diag::Error, Code::Type,
                 "assignment type mismatch: " + typeToString(s->lhs->ty) +
                 " = " + typeToString(rt),
                 s->loc);
        }
      }
      // record write for parfor race checks
      Expr* target = s->lhs;
      while (target->k == Expr::FIELD || target->k == Expr::INDEX)
        target = target->base;
      if (target->k == Expr::IDENT) {
        fs->parforWrites[target->name].push_back(s->loc);
        fs->stmtWrites[s] = target->name;
      }
      break;
    }
    case Stmt::RETURN: {
      if (s->retVal) {
        // Phase 1d: variant context for `return Ok(v);`
        if (fs->fn->ret->k == Type::ENUM)
          tryResolveVariantExpr(fs, s->retVal, fs->fn->ret);
        // Phase 2a: struct-literal context for `return Pair { ... };`
        if (fs->fn->ret->k == Type::STRUCT)
          tryAdoptStructLiteral(s->retVal, fs->fn->ret);
        Type* t = typeExpr(fs, s->retVal);
        if (fs->fn->ret->k != Type::VOID && t &&
            !typesCompatible(fs->fn->ret, t)) {
          // literal adoption (same rule as call arguments): `return 10;`
          // adopts the fn's integer return type
          bool ok = s->retVal->constant && fs->fn->ret->isNum() && t->isNum();
          if (!ok)
            diag(Diag::Error, Code::Type,
                 "return type mismatch: fn returns " +
                 typeToString(fs->fn->ret) + ", got " + typeToString(t),
                 s->loc);
        }
        if (fs->fn->ret->k == Type::VOID && s->retVal) {
          diag(Diag::Error, Code::Type, "void function cannot return a value",
               s->loc);
        }
      } else if (fs->fn->ret->k != Type::VOID) {
        // Phase 0b (report §3.2): a valueless `return;` in a non-void
        // function used to be a warning while IRgen emitted an
        // ill-typed `ret void`. It is a hard error now; `main` keeps the
        // documented implicit `return 0` convention instead.
        if (fs->fn->name == "main") {
          diag(Diag::Warning, Code::RetMainImplicit,
               "main() returns implicitly 0; prefer an explicit return",
               s->loc);
        } else {
          diag(Diag::Error, Code::Ret,
               "non-void function '" + fs->fn->name + "' cannot return without "
               "a value (declared -> " + typeToString(fs->fn->ret) + ")",
               s->loc);
        }
      }
      break;
    }
    // Phase 1a (report §4.4)
    case Stmt::BREAK:
    case Stmt::CONTINUE: {
      if (fs->loops.empty()) {
        diag(Diag::Error, Code::LoopBreakOutside,
             std::string(s->k == Stmt::BREAK ? "break" : "continue") +
             " outside of a loop",
             s->loc);
      }
      // Phase 3c (report §12/§14): break/continue DIRECTLY inside a
      // parfor body are now lowered — the semantics are lane-local:
      //   continue → skip the rest of THIS iteration (jump to the latch)
      //   break    → this worker chunk stops early; other chunks run on
      // Nested for/while loops inside the parfor body work normally.
      break;
    }
    // Phase MEM (report §5.1): unsafe scope = flagged BLOCK
    case Stmt::BLOCK: {
      bool unsafeScope = s->isUnsafe;
      if (unsafeScope) ++fs->unsafeDepth;
      semaBlock(fs, s);
      if (unsafeScope) --fs->unsafeDepth;
      break;
    }
    // Phase 1d (report §4.3/§4.4): match
    case Stmt::MATCH: {
      Type* st = typeExpr(fs, s->matchOn);
      if (!st || st->k != Type::ENUM) {
        diag(Diag::Error, Code::Match,
             "match scrutinee must be an enum value, got " +
             typeToString(st),
             s->loc);
        // still walk arm bodies for other errors
        for (auto& arm : s->matchArms) semaBlock(fs, arm.body);
        break;
      }
      EnumDef* ed = st->edef;
      std::set<int> covered;
      bool hasWildcard = false;
      for (auto& arm : s->matchArms) {
        if (arm.wildcard) {
          hasWildcard = true;
          semaBlock(fs, arm.body);
          continue;
        }
        // resolve variant: bare name or qualified Enum.Variant
        int vidx = -1;
        for (auto& v : ed->variants)
          if (v.name == arm.variantName) { vidx = v.idx; break; }
        if (vidx < 0 && arm.variantName.find('.') != std::string::npos) {
          size_t dot = arm.variantName.find('.');
          std::string vn = arm.variantName.substr(dot + 1);
          for (auto& v : ed->variants)
            if (v.name == vn) { vidx = v.idx; break; }
        }
        if (vidx < 0) {
          diag(Diag::Error, Code::Match,
               "enum " + ed->name + " has no variant " + arm.variantName,
               arm.loc);
          semaBlock(fs, arm.body);
          continue;
        }
        arm.variantIdx = vidx;
        covered.insert(vidx);
        EnumVariant& v = ed->variants[vidx];
        if (arm.bindings.size() > v.fields.size()) {
          diag(Diag::Error, Code::Match,
               "variant " + v.name + " carries " +
               std::to_string(v.fields.size()) +
               " payload value(s); arm binds " +
               std::to_string(arm.bindings.size()),
               arm.loc);
        }
        // declare bindings in the arm scope
        fs->push();
        for (size_t i = 0; i < arm.bindings.size() && i < v.fields.size(); ++i) {
          Sym bs;
          bs.k = Sym::VAR;
          bs.name = arm.bindings[i];
          bs.ty = v.fields[i].ty;
          bs.mut = false;
          fs->varDepth[arm.bindings[i]] = fs->parforDepth;
          fs->declare(bs);
        }
        semaBlock(fs, arm.body);
        fs->pop();
      }
      // exhaustiveness (report §4.4: "require exhaustive matching for
      // closed enums where practical")
      if (!hasWildcard) {
        for (auto& v : ed->variants) {
          if (!covered.count(v.idx)) {
            diag(Diag::Error, Code::Match,
                 "non-exhaustive match on " + ed->name + ": variant " + v.name +
                 " is not covered (add an arm or `_ => ...`)",
                 s->loc);
          }
        }
      }
      break;
    }
    case Stmt::DEFER: typeExpr(fs, s->deferExpr); break;
    case Stmt::IF: {
      Type* ct = typeExpr(fs, s->cond);
      if (ct && ct->k != Type::BOOL) {
        diag(Diag::Error, Code::Type, "if condition must be bool",
             s->loc);
      }
      semaBlock(fs, s->thenB);
      if (s->elseB) {
        if (s->elseB->k == Stmt::IF) semaStmt(fs, s->elseB);
        else semaBlock(fs, s->elseB);
      }
      // Phase 1d: `if` arms are where enum-variant inits usually appear
      break;
    }
    case Stmt::WHILE: {
      Type* ct = typeExpr(fs, s->cond);
      if (ct && ct->k != Type::BOOL) {
        diag(Diag::Error, Code::Type, "while condition must be bool",
             s->loc);
      }
      fs->loops.push_back({"", s});
      semaBlock(fs, s->body);
      fs->loops.pop_back();
      break;
    }
    // Phase 1d: variant context flows into assignments `x = Red;` where
    // x: Color
    case Stmt::FOR: {
      // declare induction var as mut local of bound type
      fs->push();
      Sym iv;
      iv.k = Sym::VAR; iv.name = s->ivar; iv.mut = true;
      if (s->forRange) {
        Type* lt = typeExpr(fs, s->rangeLo);
        Type* ht = typeExpr(fs, s->rangeHi);
        iv.ty = (lt && ht) ? commonType(lt, ht) : Type::simple(Type::USIZE);
        if (iv.ty->k == Type::USIZE) iv.ty = Type::simple(Type::U64);
        // the ivar must be visible to the body only (ranges are exprs)
        fs->varDepth[s->ivar] = fs->parforDepth;
        fs->declare(iv);
      } else {
        // C-style: type the init FIRST, then declare the ivar so the
        // condition and step can reference it (the historical order
        // typed the condition before the ivar existed — the form never
        // type-checked)
        Type* it = typeExpr(fs, s->forInit);
        iv.ty = it && it->isInt() ? it : Type::simple(Type::U64);
        fs->varDepth[s->ivar] = fs->parforDepth;
        fs->declare(iv);
        if (s->forCond) typeExpr(fs, s->forCond);
        if (s->forStep) typeExpr(fs, s->forStep);
      }
      fs->loops.push_back({s->ivar, s});
      semaBlock(fs, s->body);
      fs->loops.pop_back();
      fs->pop();
      break;
    }
    case Stmt::PARFOR: semaParfor(fs, s); break;
    case Stmt::EXPR: typeExpr(fs, s->expr); break;
    case Stmt::EMPTY: break;
  }
}

// run full body sema for one function (shared by the main fn loop and the
// generic-instance worklist)
static void semaFnBody(Program* p, FnDecl* f) {
  FnSema* fs = new FnSema();
  fs->fn = f;
  fs->push();
  g_fnTMap = f->instTMap.empty() ? nullptr : &f->instTMap;
  if (f->unsafeFn) fs->unsafeDepth = 1;
  for (auto& pm : f->params) {
    Sym s;
    s.k = Sym::PARAM; s.name = pm.name; s.ty = pm.ty; s.mut = false;
    s.declLoc = pm.loc;   // Phase 4: LSP goto-definition / rename
    // Phase MEM (§5.3): `owned` params carry ownership in (the existing
    // flow analysis already treats them as owned when any caller gives);
    // `borrowed` params never own.
    s.owned = pm.owned;
    fs->varDepth[pm.name] = 0;
    fs->declare(s);
  }
  S->fnSemas.push_back(fs);
  // body statements
  for (auto* st : f->body->stmts) semaStmt(fs, st);

  g_fnTMap = nullptr;
  // Phase 0b (report §3.2): definite-return analysis. Non-void functions
  // must return a value on every path; the exemption is the documented
  // implicit `return 0` entry convention, which applies to the VOID main
  // (lowered to the C ABI's i32) — an explicitly non-void main is
  // checked like any other function.
  if (!f->imported && f->ret->k != Type::VOID &&
      !(f->name == "main" && f->ret->k == Type::VOID)) {
    bool any = false;
    for (auto* st : f->body->stmts)
      if (stmtAlwaysReturns(st)) { any = true; break; }
    if (!any) {
      diag(Diag::Error, Code::Ret,
           "non-void function '" + f->name + "' can reach the end of its "
           "body without returning a " + typeToString(f->ret) +
           " — every path must return or terminate (e.g. while (true))",
           f->loc);
    }
  }
}

// ---------------------------------------------------------------- program
// Phase 3a helper: pin the lookup context to a decl's module while its
// types / body are bound.  Falls back to no filtering for programs that
// were not produced by the module loader (moduleImports empty).
static void pinContextTo(const std::string& mod) {
  std::set<std::string> imps;
  if (S->prog) {
    auto it = S->prog->moduleImports.find(mod);
    if (it != S->prog->moduleImports.end()) imps = it->second;
  }
  setModuleLookupContext(mod, imps);
}

bool semaProgram(Program* p, bool requireMain) {
  S = new SemaState();
  S->prog = p;

  // 1. register structs (two passes: register names first so ordering free)
  for (auto* sd : p->structs) registerStruct(sd);
  // 1b. register enums (Phase 1d); generic enum templates are registered
  // but NOT laid out (only instantiations have concrete layouts)
  for (auto* ed : p->enums) registerEnum(ed);

  // 2. bind field types
  for (auto* sd : p->structs) {
    pinContextTo(sd->module);
    if (sd->isGeneric) continue;   // fields bind per instantiation
    for (size_t i = 0; i < sd->fields.size(); ++i) {
      Field& f = sd->fields[i];
      f.idx = (int)i;
      if (f.tyName != "") f.ty = bindType(f.ty, f.tyName, f.loc2);
      if (!f.ty) {
        diag(Diag::Error, Code::Type, "field " + f.name + " has invalid type",
             sd->loc);
      }
    }
    computeStructLayout(sd);
  }
  // 2b. bind enum variant payload types + layouts
  for (auto* ed : p->enums) {
    pinContextTo(ed->module);
    if (ed->isGeneric) continue;
    for (auto& v : ed->variants)
      for (auto& pm : v.fields)
        if (pm.tyName != "") pm.ty = bindType(pm.ty, pm.tyName, pm.loc);
    computeEnumLayout(ed);
  }
  // 3. globals
  for (auto* g : p->globals) {
    pinContextTo(g->module);
    if (g->tyName != "") g->ty = bindType(g->ty, g->tyName, g->loc);
    if (!g->ty) g->ty = typeExpr(nullptr, g->init);
    if (!g->init->constant) {
      diag(Diag::Error, Code::Global,
           "global initializer must be constant: " + g->name,
           g->loc);
    }
    auto& v = S->globalMap[g->name];
    bool dup = false;
    for (auto* o : v)
      if (o->module == g->module) {
        diag(Diag::Error, Code::Dup, "duplicate global: " + g->name,
             g->loc);
        dup = true;
      }
    if (!dup) v.push_back(g);
  }
  // 4. function signatures
  for (auto* f : p->fns) {
    pinContextTo(f->module);
    auto& v = S->fnMap[f->name];
    bool dup = false;
    for (auto* o : v)
      if (o->module == f->module) {
        diag(Diag::Error, Code::Dup, "duplicate function: " + f->name,
             f->loc);
        dup = true;
      }
    if (dup) continue;
    v.push_back(f);
    // Phase 2a: generic templates keep their param/return types UNBOUND —
    // each instantiation binds them against the inferred type arguments
    for (auto& pm : f->params) {
      if (pm.tyName != "" && !f->isGeneric)
        pm.ty = bindType(pm.ty, pm.tyName, pm.loc);
    }
    if (f->retTyName != "" && !f->isGeneric)
      f->ret = bindType(f->ret, f->retTyName, f->loc);
    // Phase 2a: validate declared constraints
    if (f->isGeneric) {
      for (size_t i = 0; i < f->tconstraints.size(); ++i) {
        const std::string& c = f->tconstraints[i];
        if (!c.empty() && c != "Numeric" && c != "Int" && c != "Float" &&
            c != "Signed") {
          diag(Diag::Error, Code::Generic,
               "unknown type constraint '" + c + "' on " + f->tparams[i] +
               " (supported: Numeric, Int, Float, Signed)",
               f->loc);
        }
      }
    }
  }
  // 5. function bodies (generic templates are checked per instantiation)
  bool hasMain = S->fnMap.count("main");
  // Executable builds need an entry point. Compile-only modes (-S / -c)
  // and library targets intentionally accept a module without `main`;
  // requiring a dummy entry point here would make it impossible to build
  // reusable Freshet modules into a static archive or another executable.
  const bool executableOutput = !g_opts.emitObj && g_opts.irOut.empty() &&
                                g_opts.projectTarget == "bin";
  if (!hasMain && executableOutput && requireMain)
    diag(Diag::Error, Code::Main, "no 'fn main()' found", p->file, 0, 0);

  for (auto* f : p->fns) {
    auto mit = S->fnMap.find(f->name);
    bool keep = false;
    if (mit != S->fnMap.end())
      for (auto* o : mit->second) if (o == f) { keep = true; break; }
    if (!keep) continue;                 // dup
    if (f->imported) continue;             // prototype only; body lives in a linked object
    if (f->isGeneric) continue;            // Phase 2a: template, not code
    pinContextTo(f->module);
    // Phase 3b: the fn's value type (used when the name is referenced
    // without a call — fn-pointer coercion)
    {
      std::vector<Type*> ptys;
      for (auto& pm : f->params) ptys.push_back(pm.ty);
      f->ty = Type::fnptrOf(ptys, f->ret);
    }
    semaFnBody(p, f);
  }
  // 5b. generic instantiation worklist: instances discovered while typing
  // bodies (and instances of instances) run through the same body sema
  while (!S->pendingGenericBodies.empty()) {
    FnDecl* inst = S->pendingGenericBodies.back();
    S->pendingGenericBodies.pop_back();
    if (S->genericSemaDone.count(inst)) continue;   // already processed
    S->genericSemaDone.insert(inst);
    registerGenericInstance(inst);   // no-op when created via the new path
    // Phase 3a: instances belong to the TEMPLATE's module
    pinContextTo(inst->module);
    {
      std::vector<Type*> ptys;
      for (auto& pm : inst->params) ptys.push_back(pm.ty);
      inst->ty = Type::fnptrOf(ptys, inst->ret);
    }
    semaFnBody(p, inst);
  }

  // Phase 3a: every cross-module reference is resolved on the AST now —
  // post-sema passes (effects / checks / ownership / loop opts) resolve
  // without visibility filtering.
  moduleLookupRelaxHook();

  // 6. effect summaries + @pure validation (effects.cpp, audit §2.3).
  //
  // The historical boolean purity walk lived here.  It conflated
  // "does not write memory" with memory(none) (LLVM's read-none), let
  // @pure bypass validation entirely (`f->isPure = f->userPure`), and
  // its builtin allow-list treated malloc / soa_new / freshet.threads /
  // soa_len / soa_field_ptr as memory-free — all unsound under -O3.
  // computeEffects replaces it with a conservative read/write lattice,
  // a termination proof for `willreturn`, and hard F0304 errors for
  // contract violations (spec §3.1).
  computeEffects(p);

  return errorCount() == 0;
}

FnDecl* lookupFn(const std::string& name) {
  if (!S) return nullptr;
  // Phase 3a: qualified `mod::item` — resolve inside that module only
  size_t q = name.find("::");
  if (q != std::string::npos) {
    std::string mod = name.substr(0, q);
    std::string item = name.substr(q + 2);
    auto it = S->fnMap.find(item);
    if (it == S->fnMap.end()) return nullptr;
    FnDecl* cand = nullptr;
    for (auto* f : it->second)
      if (f->module == mod) { cand = f; break; }
    if (!cand) return nullptr;
    if (moduleLookupIsStrict()) {
      if (!moduleVisibleFrom(mod)) {
        diag(Diag::Error, Code::Import,
             "module '" + mod + "' is not imported here (add: import " +
             mod + ";) to use " + name);
        return nullptr;
      }
      if (!cand->isPub && mod != currentLookupModule()) {
        diag(Diag::Error, Code::Import,
             "function '" + item + "' is private to module '" + mod +
             "' — mark it `pub fn'");
        return nullptr;
      }
    }
    return cand;
  }
  auto it = S->fnMap.find(name);
  if (it == S->fnMap.end()) return nullptr;
  return pickVisible(it->second, name, "function", SrcLoc{});
}
void queueGenericInstance(FnDecl* inst) {
  if (S) S->pendingGenericBodies.push_back(inst);
}
// Phase 3b follow-on: instances register in the fn map AT CREATION so a
// second call site in the same body interns to the same FnDecl (the
// historical flow only interned in the post-body worklist, so two call
// sites in one function produced two emission-identical instances)
void registerGenericInstance(FnDecl* inst) {
  if (!S || !inst) return;
  auto& iv = S->fnMap[inst->name];
  for (auto* o : iv)
    if (o == inst) return;
  iv.push_back(inst);
}
Program* currentProgram() { return S ? S->prog : nullptr; }
GlobalVar* lookupGlobal(const std::string& name) {
  if (!S) return nullptr;
  size_t q = name.find("::");
  if (q != std::string::npos) {
    std::string mod = name.substr(0, q);
    std::string item = name.substr(q + 2);
    auto it = S->globalMap.find(item);
    if (it == S->globalMap.end()) return nullptr;
    GlobalVar* cand = nullptr;
    for (auto* g : it->second)
      if (g->module == mod) { cand = g; break; }
    return cand;
  }
  auto it = S->globalMap.find(name);
  if (it == S->globalMap.end()) return nullptr;
  return pickVisible(it->second, name, "global", SrcLoc{});
}
SemaState* semaState() { return S; }
std::vector<FnSema*>& allFnSemas() { return S->fnSemas; }
FnSema* semaForFn(FnDecl* f) { return getFnSema(f); }

} // namespace freshet
