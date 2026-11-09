// Freshet parfor checks — F0307 (races, impure calls), reduction
// validation, R2: nested parfor analysis (collapse eligibility + work
// stealing) and affinity validation.
#include "common.h"

namespace freshet {

struct LoopCtx {
  Stmt* parfor;
  int depth;
  std::set<std::string> localVars;   // declared inside this parfor
  std::set<std::string> reductions;  // reduction accumulators
};

static bool isPureRuntimeCall(const std::string& n) {
  // runtime fns that are safe inside parfor
  static const std::set<std::string> pure = {
    "freshet.prefetch_r", "freshet.prefetch_w", "freshet.clflush",
    "freshet.cycles", "freshet.time_ns", "freshet.time_ms", "freshet.pause",
    "sqrt", "sin", "cos", "tan", "exp", "log", "log2", "log10", "pow",
    "fabs", "fmin", "fmax", "floor", "ceil", "round",
    "soa_len", "soa_field_ptr",
    "imin", "imax",   // icmp + select: memory-free (audit §6.7)
  };
  return pure.count(n) > 0;
}

static void forEachExprAdapter(Expr* e, const std::function<void(Expr*)>& f);
static void forEachStmtAdapter(Stmt* s, const std::function<void(Stmt*)>& f);

static bool exprImpureInParfor(FnSema* fs, Expr* e, Stmt* pf) {
  (void)fs; (void)pf;
  bool impure = false;
  forEachExprAdapter(e, [&](Expr* x) {
    if (x->k != Expr::CALL) return;
    const std::string& n = x->name;
    if (isPureRuntimeCall(n)) return;
    if (n == "print" || n == "print_f64" || n == "print_i64" ||
        n == "print_u64" || n == "println") {
      diag(Diag::Error, Code::Thread,
           "I/O call '" + n + "' inside parfor — serialize the loop or move "
           "the call out; parfor bodies must be pure",
           x->loc, "call=" + n);
      impure = true;
      return;
    }
    if (n == "malloc" || n == "soa_new" || n == "free" || n == "soa_free") {
      diag(Diag::Error, Code::Thread,
           "allocation call '" + n + "' inside parfor — allocator contention "
           "and non-deterministic layout; pre-allocate before the loop",
           x->loc, "call=" + n);
      impure = true;
      return;
    }
    if (n == "memcpy" || n == "memset") {
      // allowed only on parfor-local or plane-disjoint memory; conservative:
      // allowed (aliasing already checked at call sites)
      return;
    }
    if (n == "assert" || n == "give") return;
    FnDecl* f = lookupFn(n);
    if (f) {
      if (!f->isPure) {
        diag(Diag::Error, Code::Thread,
             "call to impure function '" + n + "' inside parfor — mark it "
             "@pure (only if truly pure) or hoist it out",
             x->loc, "call=" + n);
        impure = true;
      }
      return;
    }
    // Phase 3b: indirect call through a function pointer — the callee is
    // unknown at compile time, so the body cannot be proven pure
    if (x->fnPtrSym || (x->globalRef && x->globalRef->ty &&
                        x->globalRef->ty->isFnPtr())) {
      diag(Diag::Error, Code::Thread,
           "indirect call through a function pointer inside parfor — the "
           "callee is not provably pure; hoist the call out or call a "
           "known @pure function",
           x->loc, "call=" + n);
      impure = true;
    }
  });
  return impure;
}

// forEachExpr helper local to this file (checks.cpp has a static one)
static void forEachExprAdapter(Expr* e, const std::function<void(Expr*)>& f);

// ---------------------------------------------------------------- nested R2
// Returns true if inner parfor bounds are independent of outer var
// (rectangular — eligible for collapse).
static bool boundsIndependent(Stmt* inner, const std::string& outerVar) {
  bool indep = true;
  auto scan = [&](Expr* x) {
    if (x->k == Expr::IDENT && x->name == outerVar) indep = false;
  };
  if (inner->plo) forEachExprAdapter(inner->plo, scan);
  if (inner->phi) forEachExprAdapter(inner->phi, scan);
  return indep;
}

// Find parfor statements in a body (non-recursive into nested parfors unless
// wanted)
static void collectParfors(Stmt* body, std::vector<Stmt*>& out, bool nested) {
  if (!body) return;
  std::function<void(Stmt*)> walk = [&](Stmt* s) {
    if (!s) return;
    if (s->k == Stmt::PARFOR) {
      out.push_back(s);
      if (nested) walk(s->body);
      return;   // don't descend into a nested parfor's body again
    }
    switch (s->k) {
      case Stmt::IF: walk(s->thenB); walk(s->elseB); break;
      case Stmt::MATCH:   // Phase 1d
        for (auto& arm : s->matchArms) walk(arm.body);
        break;
      case Stmt::WHILE: case Stmt::FOR: walk(s->body); break;
      case Stmt::BLOCK: for (auto* st : s->stmts) walk(st); break;
      default: break;
    }
  };
  walk(body);
}

static void checkParfor(FnSema* fs, Stmt* pf) {
  // reduction init values: '+':0, '*':1, min/max: any
  for (auto& c : pf->clauses) {
    if (c.k != ParForClause::REDUCTION) continue;
    if (c.redOp == "+" || c.redOp == "-") {
      // find let-init of the accumulator
      bool foundZero = false;
      forEachStmtAdapter(fs->fn->body, [&](Stmt* s) {
        if (foundZero || s->k != Stmt::LET || s->var != c.var) return;
        if (s->init && s->init->constant &&
            ((s->init->k == Expr::INT && s->init->ival == 0) ||
             (s->init->k == Expr::FLT && s->init->fval == 0.0) ||
             s->init->k == Expr::UN))
          foundZero = true;
      });
      if (!foundZero)
        diag(Diag::Warning, Code::RedAddInit,
             "reduction(+" + c.var + ") should be initialized to 0 for "
             "deterministic parallel semantics",
             c.loc);
    }
    if (c.redOp == "*") {
      bool foundOne = false;
      forEachStmtAdapter(fs->fn->body, [&](Stmt* s) {
        if (foundOne || s->k != Stmt::LET || s->var != c.var) return;
        if (s->init && s->init->constant &&
            ((s->init->k == Expr::INT && s->init->ival == 1) ||
             (s->init->k == Expr::FLT && s->init->fval == 1.0)))
          foundOne = true;
      });
      if (!foundOne)
        diag(Diag::Error, Code::RedMulInit,
             "reduction(*" + c.var + ") must be initialized to 1",
             c.loc);
    }
  }

  // F0307 races: writes to vars captured from enclosing scopes
  // A var is safe if: (a) it's the reduction accumulator, (b) declared inside
  // the parfor body, (c) it's the parfor induction var.
  std::set<std::string> safe;
  for (auto& c : pf->clauses)
    if (c.k == ParForClause::REDUCTION) safe.insert(c.var);
  safe.insert(pf->pvar);

  // collapsed inner parfor: serialized inside the outer chunk fn, so the
  // ENCLOSING parfors' reduction vars are written by one thread at a time
  if (pf->collapsed) {
    Stmt* q = pf->parent;
    while (q) {
      if (q->k == Stmt::PARFOR) {
        safe.insert(q->pvar);
        for (auto& c : q->clauses)
          if (c.k == ParForClause::REDUCTION) safe.insert(c.var);
      }
      q = q->parent;
    }
  }

  // locals declared inside body
  std::function<void(Stmt*)> collectLocals = [&](Stmt* s) {
    if (!s) return;
    if (s->k == Stmt::LET || s->k == Stmt::FOR) safe.insert(s->var);
    if (s->k == Stmt::PARFOR) {
      safe.insert(s->pvar);
      for (auto& c : s->clauses)
        if (c.k == ParForClause::REDUCTION) safe.insert(c.var);
      // inner parfors have their own body locals
      collectLocals(s->body);
      return;
    }
    switch (s->k) {
      case Stmt::IF: collectLocals(s->thenB); collectLocals(s->elseB); break;
      case Stmt::WHILE: case Stmt::FOR: collectLocals(s->body); break;
      case Stmt::BLOCK: for (auto* st : s->stmts) collectLocals(st); break;
      default: break;
    }
  };
  collectLocals(pf->body);

  std::function<void(Stmt*)> checkWrites = [&](Stmt* s) {
    if (!s) return;
    if (s->k == Stmt::ASSIGN) {
      Expr* t = s->lhs;
      while (t->k == Expr::FIELD || t->k == Expr::INDEX) t = t->base;
      if (t->k == Expr::IDENT && !safe.count(t->name)) {
        // writes THROUGH a pointer/index base go to heap elements at
        // iteration-distinct indices — legal parfor data parallelism
        // (stride quality is F0403's job). Only scalar/struct captures
        // and the pointer VARIABLE itself race. soa() handles are plane
        // indirections: b[i].f writes plane[i] — disjoint per i.
        Sym* sy = fs ? fs->lookup(t->name) : nullptr;
        bool throughPtr = (s->lhs != t) && sy && sy->ty &&
                          (sy->ty->isPtr() || sy->ty->k == Type::SOA);
        if (!throughPtr) {
          diag(Diag::Error, Code::Thread,
               "write to captured variable '" + t->name + "' inside parfor — "
               "concurrent iterations race; make it a reduction, or copy to a "
               "loop-local, or declare the var inside the parfor",
               s->loc, "var=" + t->name);
        }
      }
    }
    switch (s->k) {
      case Stmt::IF: checkWrites(s->thenB); checkWrites(s->elseB); break;
      case Stmt::WHILE: case Stmt::FOR: checkWrites(s->body); break;
      case Stmt::PARFOR: return;   // inner parfor checked separately
      case Stmt::BLOCK: for (auto* st : s->stmts) checkWrites(st); break;
      default: break;
    }
  };
  checkWrites(pf->body);

  // impure calls
  std::function<void(Stmt*)> walkImpure = [&](Stmt* s) {
    if (!s) return;
    switch (s->k) {
      case Stmt::LET: case Stmt::EXPR: case Stmt::DEFER: case Stmt::RETURN:
      case Stmt::ASSIGN: {
        Expr* ex = nullptr;
        if (s->k == Stmt::LET) ex = s->init;
        else if (s->k == Stmt::EXPR) ex = s->expr;
        else if (s->k == Stmt::DEFER) ex = s->deferExpr;
        else if (s->k == Stmt::RETURN) ex = s->retVal;
        else { exprImpureInParfor(fs, s->rhs, pf); ex = s->lhs; }
        exprImpureInParfor(fs, ex, pf);
        break;
      }
      case Stmt::IF: walkImpure(s->thenB); walkImpure(s->elseB); break;
      case Stmt::WHILE: case Stmt::FOR: walkImpure(s->body); break;
      case Stmt::PARFOR: walkImpure(s->body); break;
      case Stmt::BLOCK: for (auto* st : s->stmts) walkImpure(st); break;
      default: break;
    }
  };
  walkImpure(pf->body);

  // R2: nested parfor analysis
  std::vector<Stmt*> inner;
  collectParfors(pf->body, inner, false);
  for (auto* in : inner) {
    bool rectangular = boundsIndependent(in, pf->pvar);
    bool innerHasReduction = false;
    for (auto& c : in->clauses)
      if (c.k == ParForClause::REDUCTION) innerHasReduction = true;
    bool outerHasReduction = false;
    for (auto& c : pf->clauses)
      if (c.k == ParForClause::REDUCTION) outerHasReduction = true;

    if (rectangular && (in->clauses.empty() || (!innerHasReduction && !outerHasReduction))) {
      in->collapsed = true;
      pf->collapsed = true;
      diag(Diag::Note, Code::Collapse,
           "nested parfor (lines " + std::to_string(pf->loc.line) + "," +
           std::to_string(in->loc.line) + ") is rectangular — compiler "
           "collapses the iteration spaces into one parallel region",
           in->loc);
    } else {
      // work-stealing path: inner tasks pushed to the global pool
      diag(Diag::Note, Code::NestedWs,
           "nested parfor (lines " + std::to_string(pf->loc.line) + "," +
           std::to_string(in->loc.line) + ") has dynamic bounds/reductions — "
           "inner chunks become stealable tasks in the global pool",
           in->loc);
    }
  }
}

// ---- shared traversal adapters (non-static so both this file sees them)
static void forEachExprAdapter(Expr* e, const std::function<void(Expr*)>& f) {
  if (!e) return;
  f(e);
  for (auto* a : e->args) forEachExprAdapter(a, f);
  forEachExprAdapter(e->lhs, f);
  forEachExprAdapter(e->rhs, f);
  forEachExprAdapter(e->base, f);
}
static void forEachStmtAdapter(Stmt* s, const std::function<void(Stmt*)>& f) {
  if (!s) return;
  f(s);
  switch (s->k) {
    case Stmt::IF: forEachStmtAdapter(s->thenB, f); forEachStmtAdapter(s->elseB, f); break;
    case Stmt::WHILE: case Stmt::FOR: case Stmt::PARFOR:
      forEachStmtAdapter(s->body, f); break;
    case Stmt::BLOCK: for (auto* st : s->stmts) forEachStmtAdapter(st, f); break;
    default: break;
  }
}

void runParforChecks(Program* p) {
  (void)p;
  for (auto* fs : allFnSemas()) {
    std::vector<Stmt*> parfors;
    collectParfors(fs->fn->body, parfors, true);   // including nested
    for (auto* pf : parfors) checkParfor(fs, pf);
  }
}

} // namespace freshet
