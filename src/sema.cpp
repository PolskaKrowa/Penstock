// Freshet Sema — symbol tables, declaration binding (tyName encodings are
// resolved through generics.cpp's binder), literal type adoption. Effect /
// purity analysis lives in effects.cpp.
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
// fsht-02:03: const-parameter values of the current generic instantiation
// (parallel to g_fnTMap) — array lengths `[T; N * 2]` and const arguments
// inside the instance body evaluate against it.
static std::map<std::string, uint64_t>* g_fnCMap = nullptr;

Type* tparamTypeInScope(const std::string& name) {
  if (g_fnTMap) {
    auto it = g_fnTMap->find(name);
    if (it != g_fnTMap->end()) return it->second;
  }
  return nullptr;
}

// fsht-02:03: const-parameter value of the current instantiation
uint64_t* constParamValueInScope(const std::string& name) {
  if (g_fnCMap) {
    auto it = g_fnCMap->find(name);
    if (it != g_fnCMap->end()) return &it->second;
  }
  return nullptr;
}

static Type* bindType(Type* placeholder, const std::string& tyName, SrcLoc loc) {
  if (tyName.empty()) return placeholder;
  if (g_fnTMap) {
    auto it = g_fnTMap->find(tyName);
    if (it != g_fnTMap->end()) return it->second;
  }
  // fsht-02:03 (and a latent Phase-2a fix): thread the substitution maps
  // INTO the binder so nested shapes (`Pair<T, f64>` fields, `[T; N]`
  // lengths) resolve against the current instantiation, not just the
  // top-level bare names handled above.
  Type* t = bindTypeEncoding(S->prog, tyName, loc, g_fnTMap, g_fnCMap);
  return t ? t : placeholder;
}

// ---------------------------------------------------------------- expressions
Type* typeExpr(FnSema* fs, Expr* e);   // in sema_expr.cpp
void tryResolveVariantExpr(FnSema* fs, Expr* e, Type* expected);  // sema_expr.cpp

// fsht-02:01: is `e` a borrowed view (str / Span<T>) rooted at a LOCAL
// Vec<T> binding?  The provenance matters because the container drops at
// scope exit — returning such a view (or a binding that holds one) would
// dangle.  Views of PARAMS are fine (the caller owns them); String keeps
// its fsht-01:01 manual model, so str views of local Strings stay legal.
static Expr* containerViewRoot(FnSema* fs, Expr* e) {
  if (!e || !e->ty) return nullptr;
  if (e->ty->k != Type::SPAN && e->ty->k != Type::STR) return nullptr;
  Expr* root = e;
  if (root->k == Expr::CALL && (root->name == "str_of" ||
                                 root->name == "vec_slice") &&
      !root->args.empty())
    root = root->args[0];
  if (root->k == Expr::UN && root->op == "&") root = root->rhs;
  if (root->k == Expr::SLICE) root = root->base;
  if (!root || root->k != Expr::IDENT || !root->sym) return nullptr;
  if (root->sym->k != Sym::VAR) return nullptr;   // params: caller owns
  if (!isContainerType(root->sym->ty)) return nullptr;
  if (!containerElemType(root->sym->ty)) return nullptr;   // String: manual model
  (void)fs;
  return root;
}

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
    // fsht-03:01 (registry 08.01.04): a checked_cast/saturating_cast in
    // the initialiser infers its destination U from the annotation
    // (Option<U> unwraps; a plain numeric annotation IS U)
    trySetCastTarget(fs, s->init, t);
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
    // fsht-02:01: record view bindings whose provenance is a local
    // container — returning them dangles after the scope-exit drop
    if (it && (it->k == Type::SPAN || it->k == Type::STR) &&
        containerViewRoot(fs, s->init))
      fs->containerViewVars.insert(s->var);
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
  // ownership: malloc result / give-received.  stack_alloc results are
  // writable through immutable bindings like owned heap (the binding is
  // const, the scope exclusively owns the memory) — but they are NOT
  // heap-owned: free() on them is F0927 and they never leak (the
  // scope-exit watermark restore frees them).
  if (s->init->k == Expr::CALL && s->init->name == "malloc") sym.owned = true;
  if (s->init->k == Expr::CALL && s->init->name == "soa_new") sym.owned = true;
  if (s->init->k == Expr::CALL && s->init->name == "stack_alloc") sym.owned = true;
  // fsht-03:02 (ch. 24 §1): a binding initialised from `&mut place`
  // holds the exclusive borrow — writes through it are legal (F0310
  // governs the exclusivity, const-ness of the binding does not)
  if (s->init->k == Expr::UN && s->init->op == "&" && s->init->borrowMut)
    sym.mutBorrow = true;
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
  // induction var is a fresh immutable usize in body scope
  fs->push();
  Sym iv;
  iv.k = Sym::VAR; iv.name = s->pvar; iv.ty = Type::simple(Type::USIZE);
  iv.mut = false;
  iv.declLoc = s->loc;   // IRgen resolves the parfor var's slot by name@declLoc
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
        // reduction init-value check (+ needs 0, * needs 1) is done in
        // checks_parfor.cpp (F0308/F0309)
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

  // post-body race analysis (F0307) is done in checks_parfor.cpp, which
  // sees the fully typed body

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
      // fsht-03:01: `x = checked_cast(v);` infers U from the lhs type
      trySetCastTarget(fs, s->rhs, s->lhs->ty);
      Type* rt = typeExpr(fs, s->rhs);
      // fsht-02:01: assignments seed the same view-provenance record the
      // LET path keeps — a Span/str binding written from a local
      // container-rooted view must not be returned out of the fn
      if (rt && (rt->k == Type::SPAN || rt->k == Type::STR) &&
          s->lhs->k == Expr::IDENT && containerViewRoot(fs, s->rhs))
        fs->containerViewVars.insert(s->lhs->name);
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
            // is exclusively owned; a `&mut`-held borrow (ch. 24 §1) is
            // writable too — its exclusivity is F0310's business
            if (bs && !bs->mut && !bs->owned && !bs->mutBorrow &&
                bs->ty && bs->ty->k == Type::PTR) {
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
        // fsht-03:01: `return checked_cast(v);` infers U from the fn's
        // return type
        trySetCastTarget(fs, s->retVal, fs->fn->ret);
        Type* t = typeExpr(fs, s->retVal);
        // fsht-02:01: returning a borrowed view of a LOCAL container —
        // the container drops at every fn exit, so the view would dangle.
        // Views of params stay legal (the caller owns them); returning the
        // container itself moves it and is fine.
        {
          Expr* badRoot = containerViewRoot(fs, s->retVal);
          bool viaBinding = s->retVal->k == Expr::IDENT &&
                            fs->containerViewVars.count(s->retVal->name) > 0;
          if (badRoot || viaBinding) {
            // fsht-03:02 (03.02.10, ch. 15 ID-split): reclassified F0301 →
            // F0311 — the precise escape diagnostic
            diag(Diag::Error, Code::BorrowLifetime,
                 std::string("returning a borrowed view of local storage — "
                 "the owner drops at fn exit and the view dangles "
                 "(fsht-03:02's F0311); return the owner itself (a move) "
                 "or take a `borrowed` parameter"),
                 s->loc,
                 "kind=view-escape");
          }
        }
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
          // unique slot key: two matches binding the same NAME (Some(v)
          // then Ok(v)) used to share one declLoc-defaulted slot and the
          // second arm read the first's stale bytes
          bs.declLoc = arm.loc;
          bs.declLoc.startOffset += (int)i + 1;
          arm.bindingLocs.push_back(bs.declLoc);   // IRgen's store key
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
    case Stmt::FOR: {
      // declare induction var as mut local of bound type
      fs->push();
      Sym iv;
      iv.k = Sym::VAR; iv.name = s->ivar; iv.mut = true;
      iv.declLoc = s->loc;   // IRgen resolves loop-var slots by name@declLoc
      if (s->forRange) {
        Type* lt = typeExpr(fs, s->rangeLo);
        Type* ht = typeExpr(fs, s->rangeHi);
        iv.ty = (lt && ht) ? commonType(lt, ht) : Type::simple(Type::USIZE);
        if (iv.ty->k == Type::USIZE) iv.ty = Type::simple(Type::U64);
        // type the `by <step>` expression too: IRgen's latch lowers it
        // through genExpr(stepAmt) — an untyped literal used to crash the
        // compiler with a null-type deref (only `by -1` survived, because
        // the descending path never evaluates the step expression).
        if (s->forStep) typeExpr(fs, s->forStep);
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
  g_fnCMap = f->instCMap.empty() ? nullptr : &f->instCMap;   // fsht-02:03
  if (f->unsafeFn) fs->unsafeDepth = 1;
  for (auto& pm : f->params) {
    Sym s;
    s.k = Sym::PARAM; s.name = pm.name; s.ty = pm.ty; s.mut = false;
    s.declLoc = pm.loc;   // Phase 4: LSP goto-definition / rename
    // Phase MEM (§5.3): `owned` params carry ownership in (the existing
    // flow analysis already treats them as owned when any caller gives);
    // `borrowed` params never own.
    s.owned = pm.owned;
    // fsht-03:02 (registry 03.02.09): `borrowed mut` grants the callee
    // exclusive write access through the parameter
    if (pm.borrowedMut) s.mut = true;
    fs->varDepth[pm.name] = 0;
    fs->declare(s);
  }
  S->fnSemas.push_back(fs);
  // body statements
  for (auto* st : f->body->stmts) semaStmt(fs, st);

  g_fnTMap = nullptr;
  g_fnCMap = nullptr;
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

  // 1. register structs (two passes: names first, so declaration order is free)
  for (auto* sd : p->structs) registerStruct(sd);
  // 1b. register enums (Phase 1d); generic enum templates are registered
  // but NOT laid out (only instantiations have concrete layouts)
  for (auto* ed : p->enums) registerEnum(ed);

  // 1c. fsht-02:02 (registry 06.02.01): traits + impls.  Runs after the
  // struct/enum registries exist (impl targets may be generic-struct
  // INSTANTIATIONS like Vec<i64>, which the binder creates on demand)
  // and BEFORE fn-map registration (step 4 iterates p->fns by name — the
  // impl members must carry their mangled names by then).
  for (auto* td : p->traits) {
    pinContextTo(td->module);
    registerTrait(td);
    // duplicate method names inside one trait are unusable for the
    // impl-side check below
    for (size_t i = 0; i < td->methods.size(); ++i)
      for (size_t j = i + 1; j < td->methods.size(); ++j)
        if (td->methods[i].name == td->methods[j].name)
          diag(Diag::Error, Code::Dup,
               "trait " + td->name + " declares method '" +
               td->methods[i].name + "' twice", td->loc);
  }
  for (auto* im : p->impls) {
    pinContextTo(im->module);
    TraitDef* td = findTraitDef(im->traitName);
    if (!td) {
      diag(Diag::Error, Code::NoTraitImpl,
           "impl block names unknown trait '" + im->traitName + "'", im->loc);
      continue;
    }
    // resolve the target type — must be CONCRETE (blanket impls over
    // template parameters are deferred with the registry entry)
    Type* selfTy = bindType(nullptr, im->selfEnc, im->loc);
    bool targetOk = selfTy && selfTy->k != Type::VOID;
    if (targetOk && ((selfTy->k == Type::STRUCT && selfTy->sdef->isGeneric) ||
                     (selfTy->k == Type::ENUM && selfTy->edef->isGeneric))) {
      diag(Diag::Error, Code::TraitImplMismatch,
           "impl " + im->traitName + " for " + typeToString(selfTy) +
           " targets a generic TEMPLATE — blanket impls are not supported "
           "in the fsht-02:02 first cut; impl for a concrete instantiation "
           "instead", im->loc);
      targetOk = false;
    }
    if (!targetOk) selfTy = nullptr;
    im->selfTy = selfTy;
    registerImpl(im);
    // check + rename every member fn
    for (auto* f : im->fns) {
      TraitDef::Method* m = findTraitMethodIn(im->traitName, f->name);
      if (!m) {
        diag(Diag::Error, Code::TraitImplMismatch,
             "impl " + im->traitName + " for " +
             (selfTy ? typeToString(selfTy) : "<unresolved>") +
             " defines '" + f->name + "' which the trait does not declare",
             f->loc);
        continue;
      }
      // signature match: every param encoding (Self already substituted
      // by the parser) must equal the trait's encoding after the same
      // textual substitution; the return likewise
      bool sigOk = f->params.size() == m->params.size();
      if (sigOk)
        for (size_t i = 0; i < f->params.size(); ++i) {
          std::string want = substSelfInEnc(m->params[i].tyName, im->selfEnc);
          if (f->params[i].tyName != want) {
            diag(Diag::Error, Code::TraitImplMismatch,
                 "parameter " + std::to_string(i + 1) + " of " +
                 im->traitName + "::" + f->name + " is " +
                 (f->params[i].tyName.empty() ? "<unresolved>"
                  : f->params[i].tyName) +
                 ", the trait declares " +
                 (m->params[i].tyName.empty() ? "<unresolved>"
                  : m->params[i].tyName) + " (Self = " +
                 (selfTy ? typeToString(selfTy) : "?") + ")",
                 f->params[i].loc);
            sigOk = false;
          }
        }
      {
        std::string wantRet = substSelfInEnc(m->retEnc, im->selfEnc);
        if (f->retTyName != wantRet) {
          diag(Diag::Error, Code::TraitImplMismatch,
               "return type of " + im->traitName + "::" + f->name +
               " is " + (f->retTyName.empty() ? "void" : f->retTyName) +
               ", the trait declares " +
               (wantRet.empty() ? "void" : wantRet),
               f->loc);
          sigOk = false;
        }
      }
      if (!sigOk) continue;
      // all checks pass: assign the mangled direct-call name
      if (selfTy) {
        f->implSelfTy = selfTy;
        f->module = im->module;
        f->name = traitMethodMangle(im->traitName, selfTy, m->name);
      }
    }
    // duplicate impls of the same trait for the same type
    if (selfTy) {
      for (auto* other : allImpls())
        if (other != im && other->traitName == im->traitName &&
            other->selfTy == selfTy) {
          diag(Diag::Error, Code::Dup,
               "duplicate impl of " + im->traitName + " for " +
               typeToString(selfTy), im->loc);
          break;
        }
    }
  }

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
    // Phase 2a: validate declared constraints.  fsht-02:02: a
    // constraint list may mix builtins and user traits (`T: Numeric +
    // Hashable`); each part must be a builtin or a declared trait.
    if (f->isGeneric) {
      for (size_t i = 0; i < f->tconstraints.size(); ++i) {
        const std::string& c = f->tconstraints[i];
        std::string bad = firstUnknownConstraintPart(c);
        if (!bad.empty()) {
          diag(Diag::Error, Code::Generic,
               "unknown type constraint '" + bad + "' on " + f->tparams[i] +
               " (supported: Numeric, Int, Float, Signed and declared traits)",
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
std::vector<FnSema*>& allFnSemas() {
  // LSP error-recovery path: a document with parse errors never reaches
  // sema, so S is null (first analysis) or holds an EARLIER analysis's
  // state.  Callers must tolerate an empty registry.
  static std::vector<FnSema*> empty;
  return S ? S->fnSemas : empty;
}
FnSema* semaForFn(FnDecl* f) {
  // Same guard as allFnSemas(): analysis of a parse-error document skips
  // semaProgram entirely, and the language server still walks the
  // recovered AST for hover/completion/hints.  A null S used to crash
  // every navigation request on such documents (nullptr deref in
  // getFnSema's loop below).
  if (!S) return nullptr;
  return getFnSema(f);
}

} // namespace freshet
