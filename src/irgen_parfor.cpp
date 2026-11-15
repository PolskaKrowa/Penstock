// Freshet IRgen — parfor lowering.
// Per region:
//   * chunk function __pf_body_N(lo, hi, ctx): loop over [lo,hi) with
//     body; reduction accumulators are plain allocas whose live range is
//     the chunk fn — SROA + loop-rotate turn them into loop-carried phis,
//     and the fast-math flags let the vectorizer rewrite the reduction.
//   * ctx block: [ptr-to-captured-var x nshared][64B acc slot per reduction]
//   * caller: %frt.range + call frt_parfor(range, ctxout, MAXCTX), then a
//     combine+free loop over the returned chunk contexts.
// R2 nested parfor:
//   * rectangular (inner bounds independent of outer var) => inner is
//     serialized inside the outer chunk fn (collapse: one parallel region,
//     inner loop vectorizes);
//   * dynamic bounds => real nested dispatch: chunk fn calls frt_parfor
//     itself; the pool's workers execute/steal tasks; the calling worker
//     helps until the nested region drains (no blocking waits -> no
//     deadlock).
#include "irgen.h"
#include "dstk.h"

namespace freshet {

extern IrGen* G;

static const int MAXCTX = 64;

// optimisation-audit §6.12 / Phase F: compact per-iteration body-cost
// class.  The historical serial cutoff only knew the ITERATION COUNT —
// 256 cheap iterations and 256 expensive iterations made the same
// dispatch decision.  The estimate below is a static arithmetic/memory/
// call census of one iteration; the runtime turns it into an estimated
// total (trip_count * body_cost) and runs small-work regions inline.
// The weights mirror the profitability model's documented approximation.
static int64_t estimateParforBodyCost(Stmt* body) {
  int64_t ops = 0, mem = 0, calls = 0;
  std::function<void(Expr*)> walkE = [&](Expr* e) {
    if (!e) return;
    if (e->k == Expr::CALL) {
      ++calls;
    } else if (e->k == Expr::INDEX || e->k == Expr::FIELD) {
      if (e->k == Expr::INDEX || e->ptrDeref) ++mem;
    } else if (e->k == Expr::BIN) {
      ++ops;
    }
    for (auto* a : e->args) walkE(a);
    walkE(e->lhs);
    walkE(e->rhs);
    walkE(e->base);
  };
  std::function<void(Stmt*)> walkS = [&](Stmt* s) {
    if (!s) return;
    switch (s->k) {
      case Stmt::LET: walkE(s->init); break;
      case Stmt::ASSIGN: walkE(s->lhs); walkE(s->rhs); break;
      case Stmt::RETURN: walkE(s->retVal); break;
      case Stmt::DEFER: walkE(s->deferExpr); break;
      case Stmt::EXPR: walkE(s->expr); break;
      case Stmt::IF: walkE(s->cond); walkS(s->thenB); walkS(s->elseB); break;
      case Stmt::WHILE: walkE(s->cond); walkS(s->body); break;
      case Stmt::FOR:
        if (s->forRange) { walkE(s->rangeLo); walkE(s->rangeHi); }
        else { walkE(s->forInit); walkE(s->forCond); walkE(s->forStep); }
        walkS(s->body);
        break;
      case Stmt::PARFOR:
        walkE(s->plo); walkE(s->phi);
        walkS(s->body);
        break;
      case Stmt::BLOCK:
        for (auto* st : s->stmts) walkS(st);
        break;
      default: break;
    }
  };
  walkS(body);
  // cost units: ~1 per cheap op, 2 per memory op, 16 per call
  return 1 + ops + 2 * mem + 16 * calls;
}

// identifiers referenced in body that live in the enclosing function.
// bodyDeclared: names introduced inside the body (LET/FOR ivar/inner pvar).
static void collectCaptured(Stmt* body, const std::string& pvar,
                            std::set<std::string>& bodyDeclared,
                            std::set<std::string>& reductionVars,
                            std::vector<std::string>& captured) {
  std::set<std::string> seen;
  std::function<void(Expr*)> walkE = [&](Expr* e) {
    if (!e) return;
    if (e->k == Expr::IDENT && e->sym) {
      const std::string& n = e->name;
      if (n != pvar && !bodyDeclared.count(n) && !reductionVars.count(n) &&
          !seen.count(n)) {
        seen.insert(n);
        captured.push_back(n);
      }
    }
    for (auto* a : e->args) walkE(a);
    walkE(e->lhs);
    walkE(e->rhs);
    walkE(e->base);
  };
  std::function<void(Stmt*)> walkS = [&](Stmt* s) {
    if (!s) return;
    switch (s->k) {
      case Stmt::LET:
        bodyDeclared.insert(s->var);
        walkE(s->init);
        break;
      case Stmt::ASSIGN: walkE(s->lhs); walkE(s->rhs); break;
      case Stmt::RETURN: walkE(s->retVal); break;
      case Stmt::DEFER: walkE(s->deferExpr); break;
      case Stmt::EXPR: walkE(s->expr); break;
      case Stmt::IF: walkE(s->cond); walkS(s->thenB); walkS(s->elseB); break;
      case Stmt::WHILE: walkE(s->cond); walkS(s->body); break;
      case Stmt::FOR:
        bodyDeclared.insert(s->ivar);
        if (s->forRange) { walkE(s->rangeLo); walkE(s->rangeHi); }
        else { walkE(s->forInit); walkE(s->forCond); walkE(s->forStep); }
        walkS(s->body);
        break;
      case Stmt::PARFOR: {
        bodyDeclared.insert(s->pvar);   // inner pvar is loop-local
        // NOTE: inner reduction vars stay capturable (they must be shadow
        // vars of the OUTER chunk fn so the inner region can reference them)
        walkE(s->plo); walkE(s->phi);
        walkS(s->body);
        break;
      }
      case Stmt::BLOCK:
        for (auto* st : s->stmts) walkS(st);
        break;
      default: break;
    }
  };
  walkS(body);
}

static std::string neutralConst(const std::string& op, Type* t) {
  if (op == "*" && t->isFloat())
    return t->k == Type::F32 ? "0x3F800000" : "0x3FF0000000000000";
  if (op == "min") return t->k == Type::F32 ? "0x7F800000" : "0x7FF0000000000000";
  if (op == "max") return t->k == Type::F32 ? "0xFF800000" : "0xFFF0000000000000";
  if (op == "*") return "1";
  if (t->isFloat()) return "0.0";
  return "0";
}

static void combineOp(const std::string& op, Type* t, const std::string& acc,
                      const std::string& v, const std::string& r,
                      std::ostringstream& o, const std::string& fflag) {
  std::string ty = t->k == Type::F32 ? "float" : "double";
  if (t->isFloat()) {
    if (op == "+") o << "  " << r << " = fadd " << fflag << " " << ty << " " << acc << ", " << v << "\n";
    else if (op == "-") o << "  " << r << " = fsub " << fflag << " " << ty << " " << acc << ", " << v << "\n";
    else if (op == "*") o << "  " << r << " = fmul " << fflag << " " << ty << " " << acc << ", " << v << "\n";
    else if (op == "min")
      o << "  " << r << " = call " << fflag << " " << ty << " @llvm.fmin."
        << (t->k == Type::F32 ? "f32" : "f64") << "(" << ty << " " << acc
        << ", " << ty << " " << v << ")\n";
    else if (op == "max")
      o << "  " << r << " = call " << fflag << " " << ty << " @llvm.fmax."
        << (t->k == Type::F32 ? "f32" : "f64") << "(" << ty << " " << acc
        << ", " << ty << " " << v << ")\n";
  } else {
    std::string ity = G->llvmType(t);
    if (op == "+" || op == "-" || op == "*") {
      const char* iop = op == "+" ? "add" : (op == "-" ? "sub" : "mul");
      o << "  " << r << " = " << iop << " " << ity << " " << acc << ", " << v << "\n";
    } else {
      std::string c = op == "min" ? (t->isSigned() ? "slt" : "ult")
                                  : (t->isSigned() ? "sgt" : "ugt");
      std::string cm = G->t();
      o << "  " << cm << " = icmp " << c << " " << ity << " " << v << ", " << acc << "\n";
      o << "  " << r << " = select i1 " << cm << ", " << ity << " " << v
        << ", " << ity << " " << acc << "\n";
    }
  }
}

// serial loop for COLLAPSED inner parfor (runs inside outer chunk fn)
static void genCollapsedInner(Stmt* pf) {
  std::string lo = genExpr(pf->plo);
  if (pf->plo->ty && pf->plo->ty->bits() < 64)
    lo = genCastInline(pf->plo, Type::simple(Type::I64), lo);
  std::string hi = genExpr(pf->phi);
  if (pf->phi->ty && pf->phi->ty->bits() < 64)
    hi = genCastInline(pf->phi, Type::simple(Type::I64), hi);
  std::string iA = G->varAllocas[pf->pvar];
  if (iA.empty()) {
    iA = "%" + pf->pvar + ".addr";
    G->out << "  " << iA << " = alloca i64\n";
    G->varAllocas[pf->pvar] = iA;
  }
  G->out << "  store i64 " << lo << ", ptr " << iA << "\n";
  int hdr = G->bb++, body = G->bb++, latch = G->bb++, exit = G->bb++;
  G->out << "  br label %" << G->l(hdr) << "\n";
  G->terminate();
  G->out << G->l(hdr) << ":\n";
  G->curLabelTerminated = false;
  std::string cv = G->t(), cnd = G->t();
  G->out << "  " << cv << " = load i64, ptr " << iA << "\n";
  G->out << "  " << cnd << " = icmp slt i64 " << cv << ", " << hi << "\n";
  // §5 policy: no forced vectorise metadata by default (@vectorize on
  // the parfor or --force-vectorize opt back in)
  int lm = G->vectorizeLoopMeta(pf->vectorizeHint);
  G->out << "  br i1 " << cnd << ", label %" << G->l(body) << ", label %"
         << G->l(exit)
         << (lm >= 0 ? ", !llvm.loop !" + std::to_string(lm) : "") << "\n";
  G->terminate();
  G->out << G->l(body) << ":\n";
  G->curLabelTerminated = false;
  // Phase 3c (report §12/§14): break/continue inside a parfor body are
  // lane-local — push loop targets so BREAK/CONTINUE jump to this loop's
  // latch (continue) / exit (break: this chunk stops, others continue)
  G->loopStack.push_back({exit, latch, deferStackDepth(), true,
                             G->dstkRestores.size()});
  genStmt(pf->body);
  G->loopStack.pop_back();
  if (!G->curLabelTerminated)
    G->out << "  br label %" << G->l(latch) << "\n";
  G->out << G->l(latch) << ":\n";
  G->curLabelTerminated = false;
  std::string cv2 = G->t(), nv = G->t();
  G->out << "  " << cv2 << " = load i64, ptr " << iA << "\n";
  G->out << "  " << nv << " = add i64 " << cv2 << ", 1\n";
  G->out << "  store i64 " << nv << ", ptr " << iA << "\n";
  G->out << "  br label %" << G->l(hdr) << "\n";
  G->out << G->l(exit) << ":\n";
  G->curLabelTerminated = false;
}

// New genParfor for irgen_parfor.cpp — replaces lines 230..587
// (from `void genParfor(Stmt* pf) {` through the closing `}` before
// `} // namespace freshet`).
// ---- mem & vectorisation stage: one parfor region's caller-side pieces
struct PfRegion {
  Stmt* pf = nullptr;
  int N = 0;
  std::string rng, ctxout, nchunks;
  std::vector<std::pair<std::string, std::string>> reds;   // (op, var)
  std::set<std::string> redVars;
  std::set<std::string> bodyDeclared;
  std::vector<std::string> captured;
  uint64_t accBase = 0, ctxSize = 64;
  std::string affinity = "close";
};

// caller-context emission: lo/hi, ctx template, range struct (incl. the
// §6.12 body-cost class and the mem-stage memory/compute class at 72)
static void parforPrepRegion(Stmt* pf, int N, FnDecl* fn, FnSema* fs,
                             const std::function<Type*(const std::string&)>& tyOf,
                             PfRegion* r) {
  r->pf = pf;
  r->N = N;
  for (auto& c : pf->clauses) {
    if (c.k == ParForClause::REDUCTION) r->reds.push_back({c.redOp, c.var});
    if (c.k == ParForClause::AFFINITY) r->affinity = c.sval;
  }
  for (auto& red : r->reds) r->redVars.insert(red.second);
  collectCaptured(pf->body, pf->pvar, r->bodyDeclared, r->redVars,
                  r->captured);
  r->accBase = alignUp(8 * r->captured.size(), 64);
  r->ctxSize = (r->captured.empty() && r->reds.empty())
                   ? 64
                   : r->accBase + 64 * r->reds.size();

  std::string lo = genExpr(pf->plo);
  if (pf->plo->ty && pf->plo->ty->bits() < 64)
    lo = genCastInline(pf->plo, Type::simple(Type::I64), lo);
  std::string hi = genExpr(pf->phi);
  if (pf->phi->ty && pf->phi->ty->bits() < 64)
    hi = genCastInline(pf->phi, Type::simple(Type::I64), hi);

  // ctx template
  std::string tmpl = G->t();
  G->out << "  " << tmpl << " = alloca i8, i64 " << r->ctxSize
         << ", align 64\n";
  for (size_t k = 0; k < r->captured.size(); ++k) {
    std::string s = G->t();
    G->out << "  " << s << " = getelementptr i8, ptr " << tmpl << ", i64 "
           << (8 * k) << "\n";
    std::string srcA = G->varAllocas.count(r->captured[k])
                           ? G->varAllocas[r->captured[k]] : "";
    if (srcA.empty()) {
      // Phase 3a: captured GLOBAL — use the collision-aware IR symbol
      GlobalVar* cg = nullptr;
      for (auto* g : G->prog->globals)
        if (g->name == r->captured[k]) { cg = g; break; }
      srcA = cg ? ("@" + irGlobalSymbol(cg)) : ("@" + r->captured[k]);
    }
    std::string pv = G->t();
    G->out << "  " << pv << " = getelementptr "
           << G->llvmType(tyOf(r->captured[k])) << ", ptr " << srcA
           << ", i64 0\n";
    G->out << "  store ptr " << pv << ", ptr " << s << "\n";
  }
  for (size_t k = 0; k < r->reds.size(); ++k) {
    Type* at = tyOf(r->reds[k].second);
    std::string s = G->t();
    G->out << "  " << s << " = getelementptr i8, ptr " << tmpl << ", i64 "
           << (r->accBase + 64 * k) << "\n";
    G->out << "  store " << G->llvmType(at) << " "
           << neutralConst(r->reds[k].first, at) << ", ptr " << s << "\n";
  }

  // range struct
  r->rng = G->t();
  G->out << "  " << r->rng << " = alloca %frt.range\n";
  {
    std::string s = G->t();
    G->out << "  " << s << " = getelementptr i8, ptr " << r->rng
           << ", i64 0\n";
    G->out << "  store ptr @__pf_body_" << N << ", ptr " << s << "\n";
  }
  auto setI64 = [&](int off, const std::string& v) {
    std::string s = G->t();
    G->out << "  " << s << " = getelementptr i8, ptr " << r->rng
           << ", i64 " << off << "\n";
    G->out << "  store i64 " << v << ", ptr " << s << "\n";
  };
  auto setI32 = [&](int off, const std::string& v) {
    std::string s = G->t();
    G->out << "  " << s << " = getelementptr i8, ptr " << r->rng
           << ", i64 " << off << "\n";
    G->out << "  store i32 " << v << ", ptr " << s << "\n";
  };
  setI64(8, lo);
  setI64(16, hi);
  // Phase 5 (report §12): @reproducible — deterministic-schedule flag at
  // range offset 44 (see frt_range_t.repro).
  if (pf->reproducible) setI32(44, "1");
  std::string chunkVal = "0";
  bool hasUserChunk = false;
  for (auto& c : pf->clauses)
    if (c.k == ParForClause::CHUNK && c.e) {
      hasUserChunk = true;
      chunkVal = genExpr(c.e);
      if (!c.e->ty || c.e->ty->bits() != 64)
        chunkVal = genCastInline(c.e, Type::simple(Type::I64), chunkVal);
    }
  if (pf->reproducible && !hasUserChunk) chunkVal = "8192";  // FRT_REPRO_BLOCK
  setI64(24, chunkVal);
  std::string thrVal = "0";
  for (auto& c : pf->clauses)
    if (c.k == ParForClause::THREADS && c.e) {
      thrVal = genExpr(c.e);
      if (!c.e->ty || c.e->ty->bits() != 32)
        thrVal = genCastInline(c.e, Type::simple(Type::I32), thrVal);
    }
  setI32(32, thrVal);
  setI32(36, r->affinity == "spread" ? "1" : "0");
  setI32(40, std::to_string((int)r->ctxSize));
  uint64_t tag = hashName(fn->name + ":" + std::to_string(pf->loc.line));
  setI64(48, std::to_string(tag));
  G->parforTags.push_back({tag, fn->name, pf->loc.line});
  // §6.12 (optimisation-audit): per-iteration body-cost class at offset
  // 64.  0 = legacy IR (pure iteration-count cutoff); the estimator
  // always returns >= 1 for a real body.
  setI64(64, std::to_string(estimateParforBodyCost(pf->body)));
  {
    std::string s = G->t();
    G->out << "  " << s << " = getelementptr i8, ptr " << r->rng
           << ", i64 56\n";
    G->out << "  store ptr " << tmpl << ", ptr " << s << "\n";
  }
  // mem & vectorisation stage: memory/compute census class at offset 72
  // (0 = legacy/unclassified; the runtime routes class-tagged chunks to
  // SMT sibling lanes under FRT_SMT_INTERLEAVE=1)
  setI32(72, std::to_string(pf->smtMemClass));

  // %sp threading (goose port v2): the dispatching context's first-free
  // data-stack index rides in the range at offset 80; the runtime copies
  // it into every task and passes it as the chunk fn's hidden 4th
  // argument.  Each executing thread — a pool worker or the dispatching
  // thread helping — addresses the region's stacks at [sp0, sp0+need) of
  // its OWN per-thread tops array, so concurrent chunks never collide
  // and the caller's live stacks [.., sp0) are disjoint by construction.
  {
    const dstk::RegionInfo* ri = dstk::regionInfoOf(pf);
    size_t inUse = G->dstkRestores.size();
    std::string sp0 = "0";
    if (ri && ri->need > 0) {
      if (G->curSpExpr != "0" && inUse == 0) {
        sp0 = G->curSpExpr;
      } else if (G->curSpExpr != "0" || inUse > 0) {
        sp0 = G->t();
        G->out << "  " << sp0 << " = add i64 " << G->curSpExpr << ", "
               << inUse << "\n";
      }
    }
    setI64(80, sp0);
  }

  // ctxout array (the dispatch call fills it)
  r->ctxout = G->t();
  G->out << "  " << r->ctxout << " = alloca [" << MAXCTX << " x ptr]\n";
}

// chunk fn emission (own buffer, fresh numbering) — same instruction
// sequence as the historical single-region lowering
static void parforEmitChunkFn(const PfRegion& r, FnDecl* fn, FnSema* fs,
                              const std::function<Type*(const std::string&)>& tyOf) {
  Stmt* pf = r.pf;
  std::string callerText = G->out.str();
  int callerTmp = G->tmp, callerBb = G->bb;
  G->out.str("");
  G->out.clear();

  G->curFnIsChunkFn = true;
  G->tmp = 1;
  G->bb = 1;
  auto saveAllocas = G->varAllocas;
  G->varAllocas.clear();   // fresh symbol map for the chunk fn
  // %sp threading (goose port v2): the chunk fn carries its own stack
  // context — the caller's pending restores and sp expression must not
  // leak in, and the chunk fn's must not leak out
  auto saveRestores = G->dstkRestores;
  auto saveSpExpr = G->curSpExpr;
  G->dstkRestores.clear();
  const dstk::RegionInfo* ri = dstk::regionInfoOf(pf);
  bool regionSp = ri && ri->need > 0 && dstk::g_total > 0;
  G->curSpExpr = regionSp ? "%sp" : "0";

  // hidden 4th arg i64 %sp (goose port v2): the dispatching context's
  // first-free stack index, threaded through frt_range_t.sp0 — every
  // chunk fn has the SAME signature, so the runtime holds one fn-ptr
  // type; regions that use no stacks simply ignore it
  G->out << "define internal void @__pf_body_" << r.N
         << "(i64 %lo, i64 %hi, ptr %ctx, i64 %sp) #50 {\n";
  G->out << "entry:\n";
  G->curLabelTerminated = false;

  // shadow allocas for captured vars + init loads from ctx pointers
  for (size_t k = 0; k < r.captured.size(); ++k) {
    Type* vt = tyOf(r.captured[k]);
    G->out << "  %" << r.captured[k] << ".sh = alloca "
           << (vt->k == Type::STRUCT ? G->structTypes[vt->sdef->name]
                                     : G->llvmType(vt))
           << ", align "
           << (vt->k == Type::STRUCT ? typeAlignOf(vt) : 8) << "\n";
    G->varAllocas[r.captured[k]] = "%" + r.captured[k] + ".sh";
    std::string s = G->t();
    G->out << "  " << s << " = getelementptr i8, ptr %ctx, i64 " << (8 * k)
           << "\n";
    std::string p = G->t();
    G->out << "  " << p << " = load ptr, ptr " << s << "\n";
    std::string v = G->t();
    G->out << "  " << v << " = load " << G->llvmType(vt) << ", ptr " << p
           << "\n";
    G->out << "  store " << G->llvmType(vt) << " " << v << ", ptr %"
           << r.captured[k] << ".sh\n";
  }
  // body-declared locals: allocas now (types from sema)
  for (auto& n : r.bodyDeclared) {
    if (n == pf->pvar) continue;
    if (G->varAllocas.count(n)) continue;   // captured shadow already
    Type* vt = tyOf(n);
    if (!vt) continue;
    if (vt->k == Type::STRUCT) {
      G->out << "  %" << n << ".bd = alloca " << G->structTypes[vt->sdef->name]
             << ", align " << typeAlignOf(vt) << "\n";
      G->varAllocas[n] = "%" + n + ".bd";
    } else {
      G->out << "  %" << n << ".bd = alloca " << G->llvmType(vt) << "\n";
      G->varAllocas[n] = "%" + n + ".bd";
    }
  }
  // reduction accumulator allocas (64B slots, register-promotable)
  for (size_t k = 0; k < r.reds.size(); ++k) {
    const std::string& var = r.reds[k].second;
    Type* at = tyOf(var);
    G->out << "  %" << var << ".acc = alloca " << G->llvmType(at)
           << ", align 64\n";
    G->varAllocas[var] = "%" + var + ".acc";
    std::string s = G->t();
    G->out << "  " << s << " = getelementptr i8, ptr %ctx, i64 "
           << (r.accBase + 64 * k) << "\n";
    std::string v = G->t();
    G->out << "  " << v << " = load " << G->llvmType(at) << ", ptr " << s
           << "\n";
    G->out << "  store " << G->llvmType(at) << " " << v << ", ptr %" << var
           << ".acc\n";
  }
  // induction var
  G->out << "  %" << pf->pvar << ".addr = alloca i64\n";
  G->varAllocas[pf->pvar] = "%" + pf->pvar + ".addr";
  G->out << "  store i64 %lo, ptr %" << pf->pvar << ".addr\n";

  // %sp threading (goose port v2): per-thread lazy stack init at the
  // chunk fn entry — a POOL WORKER may be the first stack user on its
  // thread (its tops array is still all-null), so every stack-using
  // chunk fn checks and initializes before its first bump (same shape
  // as emitFunction's entry check in irgen_stmt.cpp)
  if (regionSp) {
    std::string ixr = G->t();
    G->out << "  " << ixr << " = add i64 %sp, 0\n";
    std::string slot = G->t();
    G->out << "  " << slot << " = getelementptr [@@DSTKN@@ x ptr], ptr "
           << "@frt.dstk.tops, i64 0, i64 " << ixr << "\n";
    std::string v = G->t();
    G->out << "  " << v << " = load ptr, ptr " << slot << "\n";
    std::string okr = G->t();
    G->out << "  " << okr << " = icmp ne ptr " << v << ", null\n";
    int cont = G->bb++, dinit = G->bb++;
    G->out << "  br i1 " << okr << ", label %" << G->l(cont)
           << ", label %" << G->l(dinit) << "\n";
    G->terminate();
    G->out << G->l(dinit) << ":\n";
    G->curLabelTerminated = false;
    G->out << "  call void @frt_dstk_init(ptr @frt.dstk.tops, i32 @@DSTKN@@)\n";
    G->out << "  br label %" << G->l(cont) << "\n";
    G->terminate();
    G->out << G->l(cont) << ":\n";
    G->curLabelTerminated = false;
  }

  // loop
  int hdr = G->bb++, body = G->bb++, latch = G->bb++, exit = G->bb++;
  G->out << "  br label %" << G->l(hdr) << "\n";
  G->terminate();
  G->out << G->l(hdr) << ":\n";
  G->curLabelTerminated = false;
  std::string cv = G->t(), cnd = G->t();
  G->out << "  " << cv << " = load i64, ptr %" << pf->pvar << ".addr\n";
  G->out << "  " << cnd << " = icmp slt i64 " << cv << ", %hi\n";
  // §5 policy: no forced vectorise metadata by default.  (parfor
  // iterations are independent BY LANGUAGE CONTRACT, so the loop is
  // vectorisable — but forcing it would override the backend's
  // profitability model; LLVM's default vectoriser decides.)
  int lm = G->vectorizeLoopMeta(pf->vectorizeHint);
  G->out << "  br i1 " << cnd << ", label %" << G->l(body) << ", label %"
         << G->l(exit)
         << (lm >= 0 ? ", !llvm.loop !" + std::to_string(lm) : "") << "\n";
  G->terminate();
  G->out << G->l(body) << ":\n";
  G->curLabelTerminated = false;
  // Phase 3c (report §12/§14): break/continue directly inside the parfor
  // body — lane-local semantics.  break -> this chunk's remaining
  // iterations are skipped (accs still written back at the exit block);
  // continue -> run the latch / next iteration.
  G->loopStack.push_back({exit, latch, deferStackDepth(), true,
                             G->dstkRestores.size()});
  // Phase 5: @reproducible pins strict FP for the accumulator chain
  if (pf->reproducible) ++G->forceStrictFp;
  genStmt(pf->body);
  if (pf->reproducible) --G->forceStrictFp;
  G->loopStack.pop_back();
  if (!G->curLabelTerminated)
    G->out << "  br label %" << G->l(latch) << "\n";
  G->out << G->l(latch) << ":\n";
  G->curLabelTerminated = false;
  std::string cv2 = G->t(), nv = G->t();
  G->out << "  " << cv2 << " = load i64, ptr %" << pf->pvar << ".addr\n";
  G->out << "  " << nv << " = add i64 " << cv2 << ", 1\n";
  G->out << "  store i64 " << nv << ", ptr %" << pf->pvar << ".addr\n";
  G->out << "  br label %" << G->l(hdr) << "\n";
  G->out << G->l(exit) << ":\n";
  G->curLabelTerminated = false;
  // accs back to ctx
  for (size_t k = 0; k < r.reds.size(); ++k) {
    const std::string& var = r.reds[k].second;
    Type* at = tyOf(var);
    std::string s = G->t();
    G->out << "  " << s << " = getelementptr i8, ptr %ctx, i64 "
           << (r.accBase + 64 * k) << "\n";
    std::string v = G->t();
    G->out << "  " << v << " = load " << G->llvmType(at) << ", ptr %" << var
           << ".acc\n";
    G->out << "  store " << G->llvmType(at) << " " << v << ", ptr " << s
           << "\n";
  }
  G->out << "  ret void\n";
  G->out << "}\n\n";

  G->chunkFns.push_back(G->out.str());

  // restore caller stream & state (numbering continues past the chunk fn)
  G->out.str(callerText);
  G->out.clear();
  G->out.seekp(0, std::ios::end);
  G->varAllocas = saveAllocas;
  G->dstkRestores = saveRestores;
  G->curSpExpr = saveSpExpr;
  G->curFnIsChunkFn = false;
  G->tmp = callerTmp;
  G->bb = callerBb;
}

// caller-context combine+free loop over the region's chunk contexts
static void parforEmitCombine(const PfRegion& r,
                              const std::function<Type*(const std::string&)>& tyOf,
                              const std::string& nchunksReg) {
  Stmt* pf = r.pf;
  std::string ci = G->t();
  G->out << "  " << ci << " = alloca i32\n";
  G->out << "  store i32 0, ptr " << ci << "\n";
  int chdr = G->bb++, cbody = G->bb++, clatch = G->bb++, cdone = G->bb++;
  G->out << "  br label %" << G->l(chdr) << "\n";
  G->terminate();
  G->out << G->l(chdr) << ":\n";
  G->curLabelTerminated = false;
  std::string cc = G->t(), cndc = G->t();
  G->out << "  " << cc << " = load i32, ptr " << ci << "\n";
  G->out << "  " << cndc << " = icmp slt i32 " << cc << ", " << nchunksReg
         << "\n";
  G->out << "  br i1 " << cndc << ", label %" << G->l(cbody) << ", label %"
         << G->l(cdone) << "\n";
  G->terminate();
  G->out << G->l(cbody) << ":\n";
  G->curLabelTerminated = false;
  std::string cci = G->t();
  G->out << "  " << cci << " = sext i32 " << cc << " to i64\n";
  std::string cp = G->t();
  G->out << "  " << cp << " = getelementptr [" << MAXCTX << " x ptr], ptr "
         << r.ctxout << ", i64 0, i64 " << cci << "\n";
  std::string ctxp = G->t();
  G->out << "  " << ctxp << " = load ptr, ptr " << cp << "\n";
  for (size_t k = 0; k < r.reds.size(); ++k) {
    const std::string& var = r.reds[k].second;
    Type* at = tyOf(var);
    std::string s = G->t();
    G->out << "  " << s << " = getelementptr i8, ptr " << ctxp << ", i64 "
           << (r.accBase + 64 * k) << "\n";
    std::string v = G->t();
    G->out << "  " << v << " = load " << G->llvmType(at) << ", ptr " << s
           << "\n";
    std::string accA = G->varAllocas[var];
    std::string accv = G->t();
    G->out << "  " << accv << " = load " << G->llvmType(at) << ", ptr "
           << accA << "\n";
    std::string res = G->t();
    // Phase 5: @reproducible combines run strict-FP in chunk order
    if (pf->reproducible) ++G->forceStrictFp;
    combineOp(r.reds[k].first, at, accv, v, res, G->out,
              pf->reproducible ? "" : G->fflag());
    if (pf->reproducible) --G->forceStrictFp;
    G->out << "  store " << G->llvmType(at) << " " << res << ", ptr "
           << accA << "\n";
  }
  G->out << "  call void @frt_free(ptr " << ctxp << ")\n";
  G->out << "  br label %" << G->l(clatch) << "\n";
  G->out << G->l(clatch) << ":\n";
  G->curLabelTerminated = false;
  std::string cnv = G->t();
  G->out << "  " << cnv << " = add i32 " << cc << ", 1\n";
  G->out << "  store i32 " << cnv << ", ptr " << ci << "\n";
  G->out << "  br label %" << G->l(chdr) << "\n";
  G->out << G->l(cdone) << ":\n";
  G->curLabelTerminated = false;
}

void genParfor(Stmt* pf) {
  // collapsed inner (rectangular): serialized inside the outer chunk fn
  if (pf->collapsed && G->curFnIsChunkFn) {
    genCollapsedInner(pf);
    return;
  }
  // paired partner (mem & vectorisation stage): the first region's
  // genParfor already emitted the shared frt_parfor_pair dispatch, both
  // chunk fns and both combine loops
  if (pf->smtPaired) return;

  FnDecl* saveFn = G->curFn;
  FnSema* saveFs = G->curFs;
  auto saveAllocas = G->varAllocas;
  bool saveChunk = G->curFnIsChunkFn;

  FnSema* fs = G->curFs;
  std::map<std::string, Type*> varTypes;
  for (auto& sym : fs->allSyms) varTypes[sym.name] = sym.ty;
  for (auto* g : G->prog->globals)
    if (!varTypes.count(g->name)) varTypes[g->name] = g->ty;
  auto tyOf = [&](const std::string& n) -> Type* {
    if (varTypes.count(n)) return varTypes[n];
    return Type::ptr(Type::simple(Type::U8));
  };

  // ---------------- caller dispatch part
  PfRegion A;
  parforPrepRegion(pf, G->parforCounter++, saveFn, fs, tyOf, &A);

  PfRegion B;
  const bool paired = pf->smtPairWith != nullptr;
  if (paired)
    parforPrepRegion(pf->smtPairWith, G->parforCounter++, saveFn, fs, tyOf,
                     &B);

  if (paired) {
    // mem & vectorisation stage: one dispatch covering both regions.
    // The runtime queues the first region's chunks on the memory lane
    // and the second's on the compute lane, then the caller helps until
    // both drain — on an SMT core one hardware thread streams memory
    // while its sibling keeps the shared ALU busy.  The two regions were
    // proven independent at the source level; each region's chunk
    // partition and combine order are unchanged.  The return packs the
    // two chunk counts as (nB << 32) | nA.
    std::string packed = G->t();
    G->out << "  " << packed << " = call i64 @frt_parfor_pair(ptr " << A.rng
           << ", ptr " << A.ctxout << ", ptr " << B.rng << ", ptr "
           << B.ctxout << ", i32 " << MAXCTX << ")\n";
    A.nchunks = G->t();
    G->out << "  " << A.nchunks << " = trunc i64 " << packed << " to i32\n";
    std::string shifted = G->t();
    G->out << "  " << shifted << " = lshr i64 " << packed << ", 32\n";
    B.nchunks = G->t();
    G->out << "  " << B.nchunks << " = trunc i64 " << shifted << " to i32\n";
  } else {
    A.nchunks = G->t();
    G->out << "  " << A.nchunks << " = call i32 @frt_parfor(ptr " << A.rng
           << ", ptr " << A.ctxout << ", i32 " << MAXCTX << ")\n";
  }

  // ---------------- chunk fns (separate buffers)
  parforEmitChunkFn(A, saveFn, fs, tyOf);
  if (paired) parforEmitChunkFn(B, saveFn, fs, tyOf);

  // restore caller state (numbering continues past the dispatch part)
  G->varAllocas = saveAllocas;
  G->curFnIsChunkFn = saveChunk;
  G->curFn = saveFn;
  G->curFs = saveFs;

  // ---------------- combine + free loops
  parforEmitCombine(A, tyOf, A.nchunks);
  if (paired) parforEmitCombine(B, tyOf, B.nchunks);
}


} // namespace freshet
