// Freshet IRgen — functions, statements, control flow, loops, parfor
// lowering (per-chunk context functions + runtime dispatch), R2: collapsed
// nested parfor (serialized inner, vectorizable) vs dynamic nested (work
// stealing), defer emission, PGO block counters.
#include "irgen.h"

namespace freshet {

extern IrGen* G;

// irgen_expr.cpp: shared arithmetic emitter + compound-op mapping
std::string emitArithOp(const std::string& op, Type* dt, const std::string& a,
                        const std::string& b, Expr* rhsExpr, SrcLoc loc);
std::string compoundBinaryOp(const std::string& op2);
std::string genVariantConstruct(Expr* e, std::vector<Expr*>* args);

static std::string userFnName(FnDecl* f) {
  // Phase 3a: collision-aware IR symbol (modules.cpp) — internal fns
  // keep the historical u_<name> while unique, disambiguating to
  // u_<module>$<name> when two modules define the same name
  return irFnSymbol(f);
}

// ---------------- PGO helpers
// §11.1: counter bumps are now atomic read-modify-writes.  The historical
// load/add/store triple raced when a counter site sat inside a parfor
// chunk fn executed by several workers — counts were lost, making
// training data non-reproducible.  seq_cst matches the C11 data-race-free
// guarantee the runtime's timing table already relies on; the training
// build's runtime cost is irrelevant next to correctness of the profile.
static void emitBump(int site, int edge) {
  // raw per-site counter globals (created in emitProfileData)
  std::string o = G->t();
  G->out << "  " << o << " = atomicrmw add ptr @__frt_prof_br_"
         << (site * 2 + edge) << ", i64 1 seq_cst\n";
}

// branch with optional profile metadata (from loaded .fprof)
static void emitCondBr(std::string cond, int tbb, int fbb, SrcLoc loc,
                       int profSite = -1) {
  std::string profAttr;
  if (profSite >= 0 && !g_opts.profile && !g_opts.useProfile.empty()) {
    BrProf& bp = G->brProf[profSite];
    uint64_t t = bp.taken, n = bp.notTaken;
    if (t + n == 0) { t = 50; n = 50; }   // no data: neutral hint
    G->profMetas.push_back("!{!\"branch_weights\", i32 " +
                           std::to_string((uint32_t)t) + ", i32 " +
                           std::to_string((uint32_t)n) + "}");
    int idx = 100 + (int)G->profMetas.size() - 1;
    profAttr = ", !prof !" + std::to_string(idx);
  }
  G->out << "  br i1 " << cond << ", label %" << G->l(tbb) << ", label %"
         << G->l(fbb) << profAttr << G->dbg(loc.line, loc.col) << "\n";
  G->terminate();
}

// loop-latch metadata id — thin wrapper over IrGen::vectorizeLoopMeta
// (shared with the parfor lowerings) implementing the audit §5 policy:
// no forced vectorisation metadata by default; @vectorize on the loop
// emits a plain enable hint; --force-vectorize restores the historical
// blanket enable + interleave 4 for differential testing.
// Returns -1 when no metadata should be attached.
static int loopMeta(bool userHint) {
  return G->vectorizeLoopMeta(userHint);
}

// ---------------- defer stack
struct DeferEntry {
  Expr* expr;
  SrcLoc loc;
};
static std::vector<DeferEntry> g_defers;

// irgen_parfor.cpp pushes LoopCtx entries whose deferBase must match the
// CURRENT defer depth (break/continue inside parfor bodies flush the
// body-registered defers before branching, exactly like ordinary loops)
size_t deferStackDepth() { return g_defers.size(); }

static void emitDeferCall(Expr* de) {
  // defers are free(p) / soa_free(p) / user fn calls
  genExpr(de);
}

// ================================================================ function
void IrGen::emitFunction(FnDecl* f, FnSema* fs) {
  curFn = f;
  curFs = fs;
  tmp = 1;
  bb = 1;
  g_defers.clear();
  varAllocas.clear();

  // PGO function counter
  int fnSite = f->profIdx;

  // ---- Stage A1: pick the attribute group for this function.
  //
  // audit §2.3: the group is derived from the sema effect summary via
  // IrGen::attrGroupFor — the same canonical body is used for the
  // definition here and for every call site (irgen_call.cpp), so the two
  // can never disagree.  Inline hints compose with the effect attributes
  // instead of discarding them; exported functions are ABI boundaries
  // and stay on plain #53.  `willreturn` is only present when the effect
  // analysis proved termination.
  //
  // Group numbering keeps the historical headroom (>=50) so the runtime
  // .ll (clang -O2, attribute groups #0..#9) cannot collide.
  std::string attrs = " #" + std::to_string(attrGroupFor(f));

  // function entry-count metadata (PGO)
  std::string profDef;
  if (!g_opts.useProfile.empty() && fnSite >= 0 &&
      G->fnProf[fnSite].loaded && G->fnProf[fnSite].count > 0) {
    G->profMetas.push_back("!{!\"function_entry_count\", i64 " +
                           std::to_string(G->fnProf[fnSite].count) + "}");
    profDef = " !prof !" + std::to_string(100 + (int)G->profMetas.size() - 1);
    if (G->fnProf[fnSite].count > 100000) attrs = " hot" + attrs;
  }

  bool isMain = (f->name == "main");
  std::string linkage = (f->exported || isMain) ? "" : "internal ";
  // C ABI: main returns i32 even when declared void
  bool mainI32 = isMain && f->ret->k == Type::VOID;
  std::string retTy = mainI32 ? "i32" : llvmType(f->ret);

  out << "define " << linkage << retTy << " @" << userFnName(f)
      << "(";
  // Stage B5 (report §2.2 B): emit per-parameter attributes for sound @alias
  // pairs.  For each pair (a, b) declared on the function:
  //   - if NO caller in this compilation unit passes overlapping memory
  //     (i.e. F0306 never fired) -> mark BOTH params `noalias`.
  //     This is sound: the language promises @alias means "no overlap" and
  //     we just verified every caller we can see keeps that promise.
  //   - if F0306 fired on at least one caller -> suppress `noalias`
  //     for this pair (we cannot lie to LLVM).  Other pairs on the same
  //     function are unaffected.
  //
  // Note: we mark the parameters individually rather than the whole
  // function with a `noalias` return attribute, because LLVM's
  // `noalias` on a parameter means "this pointer does not alias any
  // OTHER pointer that escapes the function".  Combined with the
  // Freshet ownership model (noescape pointers + SoA plane identity),
  // this is exactly the right shape: every pair marked @alias is a
  // pairwise no-overlap promise, and unmarked pairs default to the
  // conservative "may alias anything" behaviour.
  std::set<std::string> noaliasParams;
  for (auto& pr : f->aliasPairs) {
    if (!aliasPairIsUnsound(f, pr.first, pr.second)) {
      noaliasParams.insert(pr.first);
      noaliasParams.insert(pr.second);
    }
  }
  for (size_t i = 0; i < f->params.size(); ++i) {
    if (i) out << ", ";
    Type* pt = f->params[i].ty;
    // Parameter passing convention (spec §4 + report §3.2 fix):
    //   STRUCT/ENUM/ARRAY — by ADDRESS, the callee copies (the historical
    //     code emitted `%struct.X %param` in the definition while every
    //     call site passed `ptr`, silently miscompiling by-value struct
    //     parameters)
    //   SOA — the descriptor pointer, by value
    //   Span<T>/str — first-class { ptr, i64 } aggregate, by value
    //   scalars — by value
    if (pt && (pt->k == Type::STRUCT || pt->k == Type::ENUM ||
               pt->k == Type::ARRAY))
      out << "ptr";
    else
      out << llvmType(pt);
    // Stage B5: noalias on sound @alias pointer params.  We do NOT add
    // `nonnull` because Freshet does not yet have a null-pointer literal
    // (only `nil` for optionals, which lowers differently) — being
    // conservative here matches the report's "lower only sound facts".
    if (pt && pt->isPtr() && noaliasParams.count(f->params[i].name)) {
      out << " noalias";
    }
    out << " %" << f->params[i].name << "param";
  }
  // Phase 3c (report §10.3): attach the DISubprogram to the definition —
  // LLVM only emits DWARF for functions whose `define` carries `!dbg`
  out << ")" << attrs << profDef
      << " !dbg !" << dbgSubprogId(f) << " {\n";
  out << "entry:\n";
  curLabelTerminated = false;

  // PGO entry bump (§11.1: atomic — see emitBump)
  if (g_opts.profile && fnSite >= 0) {
    std::string o = G->t();
    out << "  " << o << " = atomicrmw add ptr @__frt_prof_fn_" << fnSite
        << ", i64 1 seq_cst\n";
  }

  if (isMain) {
    out << "  call void @frt_init()\n";
    if (g_opts.profile) {
      out << "  call void @frt_prof_register@@ARGS@@()\n";  // patched later
    }
  }

  // allocas for params + all locals
  for (auto& sym : fs->allSyms) {
    if (sym.k != Sym::PARAM && sym.k != Sym::VAR) continue;
    if (sym.name == f->name) continue;
    if (varAllocas.count(sym.name)) continue;   // dedupe (same name redeclared)
    std::string a = "%" + sym.name + ".addr";
    std::string ty = llvmType(sym.ty);
    if (sym.ty->k == Type::STRUCT) {
      // struct var: alloca struct
      out << "  " << a << " = alloca " << structTypes[sym.ty->sdef->name]
          << ", align " << typeAlignOf(sym.ty) << "\n";
    } else if (sym.ty->k == Type::ARRAY || sym.ty->k == Type::ENUM) {
      // Phase 1b/1d: array/enum locals get their full aggregate slot
      out << "  " << a << " = alloca " << ty << ", align "
          << typeAlignOf(sym.ty) << "\n";
    } else {
      uint64_t align = typeAlignOf(sym.ty);
      // pad hot accumulators to 64 to avoid false sharing between threads
      if (sym.ty->isNum() && fs->varDepth.count(sym.name) &&
          fs->varDepth[sym.name] == 0) {
        // top-level: parfor reductions run per-chunk; pad numeric muts
        // conservatively when written in any parfor
        out << "  ; (alignment widened for cache isolation)\n";
        align = 64;
      }
      out << "  " << a << " = alloca " << ty << ", align " << align << "\n";
    }
    varAllocas[sym.name] = a;
  }
  // store params into allocas
  for (auto& pm : f->params) {
    Type* pt = pm.ty;
    if (pt && (pt->k == Type::STRUCT || pt->k == Type::ENUM ||
               pt->k == Type::ARRAY)) {
      // by-address parameter: the callee sees its own copy (spec §4)
      out << "  call void @llvm.memcpy.p0.p0.i64(ptr " << varAllocas[pm.name]
          << ", ptr %" << pm.name << "param, i64 " << typeSizeOf(pt)
          << ", i1 false)\n";
    } else {
      // scalars, pointers, SOA descriptors (a ptr value) and pair
      // aggregates all store directly
      out << "  store " << llvmType(pt) << " %" << pm.name << "param, ptr "
          << varAllocas[pm.name] << "\n";
    }
  }

  // body
  for (auto* st : f->body->stmts) genStmt(st);

  // implicit return
  if (!curLabelTerminated) {
    emitDeferredAll();
    if (mainI32) {
      out << "  ret i32 0\n";
    } else if (f->ret->k == Type::VOID) {
      out << "  ret void\n";
    } else {
      // Phase 0b (report §3.2): a compiler must not guess that a missing
      // return value means zero. Definite-return analysis in sema makes
      // this unreachable for valid programs; the backend guard emits
      // `unreachable` (valid IR) instead of a silently wrong zero value.
      out << "  unreachable  ; non-void fn falls off the end (F0209 in sema)\n";
    }
    terminate();
  }
  out << "}\n\n";
}

// emit all pending defers (reverse order) — used at scope exit and
// returns. Phase 1a fix: this does NOT clear the stack — every exit
// path must run its own copy, and block-scope removal is handled by the
// BLOCK lowering (resize to dstart). The historical clear() made the
// SECOND return in a function silently skip its defers (leak on that
// path).
void emitDeferredAll() {
  for (auto it = g_defers.rbegin(); it != g_defers.rend(); ++it) {
    emitDeferCall(it->expr);
  }
}

// run the defers registered inside the current loop body (indices >=
// base) before a break/continue branches out
static void emitLoopDefers(size_t base) {
  for (size_t i = g_defers.size(); i > base; --i)
    emitDeferCall(g_defers[i - 1].expr);
}

// ================================================================ statements
// statements after a terminator in the same block are unreachable; the
// guard keeps them out of the IR entirely (this also fixes the historical
// dead-code-after-return shape that emitted instructions after `ret`)
void genStmt(Stmt* s) {
  if (!s) return;
  if (G->curLabelTerminated) return;
  switch (s->k) {
    case Stmt::EMPTY: break;
    case Stmt::LET: {
      if (s->init) {
        std::string v = genExpr(s->init);
        std::string a = G->varAllocas[s->var];
        Type* vt = s->init->ty;
        Type* dt = s->annTy;
        if (!dt || s->tyName != "") dt = vt;
        if (dt && (dt->k == Type::STRUCT || dt->k == Type::ENUM ||
                   dt->k == Type::ARRAY)) {
          // aggregate by address: copy the value's storage into the var
          G->out << "  call void @llvm.memcpy.p0.p0.i64(ptr " << a << ", ptr "
                 << v << ", i64 " << typeSizeOf(dt) << ", i1 false)\n";
        } else if (dt && dt->k == Type::SOA) {
          G->out << "  store ptr " << v << ", ptr " << a << "\n";
        } else if (dt && dt->isPair()) {
          // Span/str: value is a { ptr, i64 } register; cast pair types
          // (Span<u8> <-> str) element-wise when representations match
          if (vt && vt->isPair() && !typesEquivalent(vt, dt) && v.rfind("%", 0) == 0) {
            std::string p0 = G->t();
            G->out << "  " << p0 << " = extractvalue { ptr, i64 } " << v << ", 0\n";
            std::string l0 = G->t();
            G->out << "  " << l0 << " = extractvalue { ptr, i64 } " << v << ", 1\n";
            std::string v1 = G->t();
            G->out << "  " << v1 << " = insertvalue { ptr, i64 } poison, ptr "
                   << p0 << ", 0\n";
            std::string v2 = G->t();
            G->out << "  " << v2 << " = insertvalue { ptr, i64 } " << v1
                   << ", i64 " << l0 << ", 1\n";
            v = v2;
          }
          G->out << "  store { ptr, i64 } " << v << ", ptr " << a << "\n";
        } else {
          // report §3.1: cast whenever the representations can differ, even
          // for constants (see irgen_expr.cpp for the history)
          if (dt && vt && dt->isNum() && vt->isNum() &&
              !typesEquivalent(vt, dt))
            v = genCastInline(s->init, dt, v);
          G->out << "  store " << G->scalarCodegenTy(dt ? dt : vt) << " " << v
                 << ", ptr " << a << "\n";
        }
      }
      break;
    }
    case Stmt::ASSIGN: {
      // compound ops expand: a op= b  =>  a = a op b — via the SAME
      // emitArithOp the binary path uses (signedness, strength reduction,
      // shifts and sdiv/srem agree between the two spellings)
      if (s->op2 != "=") {
        std::string lhsA = genAddr(s->lhs);
        std::string lhsV = G->t();
        Type* lt = s->lhs->ty;
        if (lt->k == Type::STRUCT || lt->k == Type::ENUM ||
            lt->k == Type::ARRAY) {
          // struct compound not allowed (sema)
        } else {
          G->out << "  " << lhsV << " = load " << G->scalarCodegenTy(lt)
                 << ", ptr " << lhsA << "\n";
          std::string rhsV = genExpr(s->rhs);
          if (s->rhs->ty && !typesEquivalent(lt, s->rhs->ty))
            rhsV = genCastInline(s->rhs, lt, rhsV);
          std::string op = compoundBinaryOp(s->op2);
          std::string r = emitArithOp(op, lt, lhsV, rhsV, s->rhs, s->loc);
          G->out << "  store " << G->scalarCodegenTy(lt) << " " << r
                 << ", ptr " << lhsA << "\n";
        }
        break;
      }
      // plain assignment
      Type* lt = s->lhs->ty;
      if (lt && (lt->k == Type::STRUCT || lt->k == Type::ENUM ||
                 lt->k == Type::ARRAY)) {
        std::string src = genExpr(s->rhs);   // aggregate value = address
        std::string dst = genAddr(s->lhs);
        G->out << "  call void @llvm.memcpy.p0.p0.i64(ptr " << dst << ", ptr "
               << src << ", i64 " << typeSizeOf(lt) << ", i1 false)\n";
        break;
      }
      if (lt && lt->isPair()) {
        std::string v = genExpr(s->rhs);
        if (s->rhs->ty && s->rhs->ty->isPair() && !typesEquivalent(s->rhs->ty, lt)) {
          std::string p0 = G->t();
          G->out << "  " << p0 << " = extractvalue { ptr, i64 } " << v << ", 0\n";
          std::string l0 = G->t();
          G->out << "  " << l0 << " = extractvalue { ptr, i64 } " << v << ", 1\n";
          std::string v1 = G->t();
          G->out << "  " << v1 << " = insertvalue { ptr, i64 } poison, ptr "
                 << p0 << ", 0\n";
          std::string v2 = G->t();
          G->out << "  " << v2 << " = insertvalue { ptr, i64 } " << v1
                 << ", i64 " << l0 << ", 1\n";
          v = v2;
        }
        std::string a = genAddr(s->lhs);
        G->out << "  store { ptr, i64 } " << v << ", ptr " << a << "\n";
        break;
      }
      std::string v = genExpr(s->rhs);
      // report §3.1: always cast to the destination width when the
      // representations can differ — the historical constant exemption let
      // `e = 100;` on an i32 variable store an i64 register (malformed IR)
      if (s->rhs->ty && lt && s->rhs->ty->isNum() && lt->isNum() &&
          !typesEquivalent(s->rhs->ty, lt)) {
        v = genCastInline(s->rhs, lt, v);
      }
      std::string a = genAddr(s->lhs);
      G->out << "  store " << G->scalarCodegenTy(lt) << " " << v << ", ptr "
             << a << "\n";
      break;
    }
    case Stmt::RETURN: {
      bool mainI32 = (G->curFn->name == "main" &&
                      G->curFn->ret->k == Type::VOID);
      if (s->retVal) {
        Type* rt = G->curFn->ret;
        if (rt->k == Type::STRUCT || rt->k == Type::ENUM ||
            rt->k == Type::ARRAY) {
          // aggregate return: load the whole value and return it
          std::string src = genExpr(s->retVal);   // address
          std::string v = G->t();
          G->out << "  " << v << " = load " << G->llvmType(rt) << ", ptr "
                 << src << "\n";
          emitDeferredAll();
          G->out << "  ret " << G->llvmType(rt) << " " << v
                 << G->dbg(s->loc.line, s->loc.col) << "\n";
        } else if (rt->isPair()) {
          std::string v = genExpr(s->retVal);   // { ptr, i64 }
          emitDeferredAll();
          G->out << "  ret { ptr, i64 } " << v
                 << G->dbg(s->loc.line, s->loc.col) << "\n";
        } else {
          std::string v = genExpr(s->retVal);
          if (s->retVal->ty && !typesEquivalent(s->retVal->ty, rt) &&
              s->retVal->ty->isNum() && rt->isNum())
            v = genCastInline(s->retVal, rt, v);
          // bool RETURN ABI (mirror of genArg's bool-parameter widening):
          // bool VALUES are i1 registers (literals, comparisons, loads from
          // bool slots) while the function signature uses the i8 C ABI —
          // widen at the ret boundary.  A call result is already i8.
          if (rt->k == Type::BOOL && s->retVal->ty &&
              s->retVal->ty->k == Type::BOOL &&
              s->retVal->k != Expr::CALL) {
            std::string w = G->t();
            G->out << "  " << w << " = zext i1 " << v << " to i8\n";
            v = w;
          }
          emitDeferredAll();
          G->out << "  ret " << G->llvmType(rt) << " " << v
                 << G->dbg(s->loc.line, s->loc.col) << "\n";
        }
      } else {
        emitDeferredAll();
        std::string d = G->dbg(s->loc.line, s->loc.col);
        if (mainI32) G->out << "  ret i32 0" << d << "\n";
        else G->out << "  ret void" << d << "\n";
      }
      G->terminate();
      break;
    }
    // Phase 1a (report §4.4): loop exits. Defers registered inside the
    // loop body run BEFORE the branch leaves.
    case Stmt::BREAK: {
      if (!G->loopStack.empty()) {
        auto& loop = G->loopStack.back();
        emitLoopDefers(loop.deferBase);
        G->out << "  br label %" << G->l(loop.breakBB) << "\n";
      }
      G->terminate();
      break;
    }
    case Stmt::CONTINUE: {
      if (!G->loopStack.empty()) {
        auto& loop = G->loopStack.back();
        emitLoopDefers(loop.deferBase);
        G->out << "  br label %" << G->l(loop.continueBB) << "\n";
      }
      G->terminate();
      break;
    }
    case Stmt::DEFER: {
      g_defers.push_back({s->deferExpr, s->loc});
      break;
    }
    case Stmt::EXPR: {
      genExpr(s->expr);
      break;
    }
    case Stmt::BLOCK: {
      size_t dstart = g_defers.size();
      for (auto* st : s->stmts) genStmt(st);
      // block-scoped defers (skipped when control already left the block)
      size_t dend = g_defers.size();
      if (dend > dstart && !G->curLabelTerminated) {
        for (size_t i = dend; i > dstart; --i) {
          emitDeferCall(g_defers[i - 1].expr);
        }
        g_defers.resize(dstart);
      } else if (dend > dstart) {
        // defers already ran on the break/continue/return path; drop them
        g_defers.resize(dstart);
      }
      break;
    }
    case Stmt::IF: {
      std::string c = genExpr(s->cond);
      int thenBB = G->bb++;
      int elseBB = -1, joinBB = G->bb++;
      bool hasElse = s->elseB != nullptr;
      if (hasElse) elseBB = G->bb++;
      // PGO: two counter blocks (or profile-driven !prof on the br)
      int site = -1;
      uint64_t ifHash = hashName(G->curFn->name + ":" +
                                  std::to_string(s->loc.line) + ":if");
      if (g_opts.profile) {
        site = profSiteFor(ifHash, s->loc);
        int tcount = G->bb++;
        int fcount = G->bb++;
        G->out << "  br i1 " << c << ", label %" << G->l(tcount) << ", label %"
               << G->l(fcount) << "\n";
        G->terminate();
        G->out << G->l(tcount) << ":\n";
        G->curLabelTerminated = false;
        emitBump(site, 0);
        G->out << "  br label %" << G->l(thenBB) << "\n";
        G->out << G->l(fcount) << ":\n";
        emitBump(site, 1);
        G->out << "  br label %" << (hasElse ? G->l(elseBB) : G->l(joinBB))
               << "\n";
        G->out << G->l(thenBB) << ":\n";
        G->curLabelTerminated = false;
      } else {
        if (g_opts.useProfile.empty()) {
          G->out << "  br i1 " << c << ", label %" << G->l(thenBB)
                 << ", label %" << (hasElse ? G->l(elseBB) : G->l(joinBB))
                 << G->dbg(s->loc.line) << "\n";
          G->terminate();
        } else {
          site = profSiteFor(ifHash, s->loc);
          emitCondBr(c, thenBB, hasElse ? elseBB : joinBB, s->loc, site);
        }
        G->out << G->l(thenBB) << ":\n";
        G->curLabelTerminated = false;
      }
      genStmt(s->thenB);
      if (!G->curLabelTerminated) {
        G->out << "  br label %" << G->l(joinBB) << "\n";
        G->terminate();
      }
      if (hasElse) {
        G->out << G->l(elseBB) << ":\n";
        G->curLabelTerminated = false;
        genStmt(s->elseB);
        if (!G->curLabelTerminated) {
          G->out << "  br label %" << G->l(joinBB) << "\n";
          G->terminate();
        }
      }
      G->out << G->l(joinBB) << ":\n";
      G->curLabelTerminated = false;
      break;
    }
    case Stmt::WHILE: {
      int hdr = G->bb++;
      int body = G->bb++;
      int exit = G->bb++;
      // Phase 1a: break targets exit; continue re-tests the header
      G->loopStack.push_back({exit, hdr, g_defers.size(), false});
      G->out << "  br label %" << G->l(hdr) << "\n";
      G->terminate();
      G->out << G->l(hdr) << ":\n";
      G->curLabelTerminated = false;
      std::string c = genExpr(s->cond);
      G->out << "  br i1 " << c << ", label %" << G->l(body) << ", label %"
             << G->l(exit) << G->dbg(s->loc.line) << "\n";
      G->terminate();
      G->out << G->l(body) << ":\n";
      G->curLabelTerminated = false;
      genStmt(s->body);
      if (!G->curLabelTerminated)
        G->out << "  br label %" << G->l(hdr) << "\n";
      G->out << G->l(exit) << ":\n";
      G->curLabelTerminated = false;
      G->loopStack.pop_back();
      break;
    }
    case Stmt::FOR: {
      // both `for (i in a..b [by s])` and `for (i = a; i < b; i ± s)`
      std::string ivar = s->ivar;
      int hdr = G->bb++;
      int body = G->bb++;
      int latch = G->bb++;
      int exit = G->bb++;
      // Phase 1a: break targets exit; continue runs the latch (the step)
      G->loopStack.push_back({exit, latch, g_defers.size(), false});
      std::string iA = G->varAllocas[ivar];
      std::string lo, hi;
      // step analysis (report §7 follow-on: with F0405 downgraded to a
      // warning by default, the lowering must honour the actual step)
      int stepDir = 1;
      Expr* stepAmt = nullptr;
      bool descendingRange = false;
      if (s->forStep) {
        if (s->forStep->k == Expr::UN && s->forStep->op == "-" &&
            s->forStep->rhs->k == Expr::INT && s->forStep->rhs->ival == 1) {
          descendingRange = true;   // `by -1`
        } else if (s->forStep->k == Expr::BIN && s->forStep->op == "+=") {
          stepDir = 1; stepAmt = s->forStep->rhs;
        } else if (s->forStep->k == Expr::BIN && s->forStep->op == "-=") {
          stepDir = -1; stepAmt = s->forStep->rhs;
        } else if (s->forStep->k == Expr::BIN && s->forStep->op == "=" &&
                   s->forStep->rhs && s->forStep->rhs->k == Expr::BIN) {
          Expr* r = s->forStep->rhs;
          if (r->op == "+" || r->op == "-") {
            // i = i + C | i = i - C | i = C + i
            if (r->lhs->k == Expr::IDENT && r->lhs->name == ivar) {
              stepDir = (r->op == "+") ? 1 : -1;
              stepAmt = r->rhs;
            } else if (r->rhs->k == Expr::IDENT && r->rhs->name == ivar) {
              stepDir = (r->op == "+") ? 1 : -1;
              stepAmt = r->lhs;
            }
          }
        } else if (s->forStep->k == Expr::INT || s->forStep->k == Expr::IDENT ||
                   s->forStep->k == Expr::BIN) {
          // bare `by N` step expression on a range loop
          stepAmt = s->forStep;
        }
      }
      if (s->forRange) {
        lo = genExpr(s->rangeLo);
        hi = genExpr(s->rangeHi);
        if (s->rangeLo->ty && !typesEquivalent(s->rangeLo->ty, Type::simple(Type::I64)))
          lo = genCastInline(s->rangeLo, Type::simple(Type::I64), lo);
        if (s->rangeHi->ty && !typesEquivalent(s->rangeHi->ty, Type::simple(Type::I64)))
          hi = genCastInline(s->rangeHi, Type::simple(Type::I64), hi);
        // `for (i in 10..0 by -1)` starts at rangeLo (10) and iterates
        // while i > rangeHi (0), stepping -1 — the bounds keep their
        // source roles; only the comparison direction flips
        G->out << "  store i64 " << lo << ", ptr " << iA << "\n";
      } else {
        lo = genExpr(s->forInit);
        Type* it = s->forInit ? s->forInit->ty : nullptr;
        if (it && it->isNum() && !typesEquivalent(it, Type::simple(Type::I64)))
          lo = genCastInline(s->forInit, Type::simple(Type::I64), lo);
        G->out << "  store i64 " << lo << ", ptr " << iA << "\n";
      }
      G->out << "  br label %" << G->l(hdr) << "\n";
      G->terminate();
      G->out << G->l(hdr) << ":\n";
      G->curLabelTerminated = false;
      // condition
      if (s->forRange) {
        std::string cv = G->t();
        G->out << "  " << cv << " = load i64, ptr " << iA << "\n";
        std::string cnd = G->t();
        G->out << "  " << cnd << " = icmp " << (descendingRange ? "sgt" : "slt")
               << " i64 " << cv << ", " << hi << "\n";
        int lm = loopMeta(s->vectorizeHint);
        G->out << "  br i1 " << cnd << ", label %" << G->l(body) << ", label %"
               << G->l(exit)
               << (lm >= 0 ? ", !llvm.loop !" + std::to_string(lm) : "")
               << G->dbg(s->loc.line) << "\n";
      } else {
        std::string cond = genExpr(s->forCond);
        int lm = loopMeta(s->vectorizeHint);
        G->out << "  br i1 " << cond << ", label %" << G->l(body)
               << ", label %" << G->l(exit)
               << (lm >= 0 ? ", !llvm.loop !" + std::to_string(lm) : "")
               << G->dbg(s->loc.line) << "\n";
      }
      G->terminate();
      G->out << G->l(body) << ":\n";
      G->curLabelTerminated = false;
      genStmt(s->body);
      if (!G->curLabelTerminated)
        G->out << "  br label %" << G->l(latch) << "\n";
      G->out << G->l(latch) << ":\n";
      G->curLabelTerminated = false;
      // increment: i = i ± step (constant 1 when no step is written).
      // NOTE: the result temp is allocated AFTER the step value so the
      // explicit %N register numbering follows the emission order.
      std::string cv = G->t();
      G->out << "  " << cv << " = load i64, ptr " << iA << "\n";
      if (!stepAmt) {
        std::string nv = G->t();
        G->out << "  " << nv << " = " << (descendingRange ? "sub" : "add")
               << " i64 " << cv << ", 1\n";
        G->out << "  store i64 " << nv << ", ptr " << iA << "\n";
      } else {
        std::string amt = genExpr(stepAmt);
        if (stepAmt->ty && stepAmt->ty->bits() < 64)
          amt = genCastInline(stepAmt, Type::simple(Type::I64), amt);
        else if (stepAmt->ty && !typesEquivalent(stepAmt->ty, Type::simple(Type::I64)) && stepAmt->ty->isNum())
          amt = genCastInline(stepAmt, Type::simple(Type::I64), amt);
        std::string nv = G->t();
        G->out << "  " << nv << " = " << (stepDir < 0 ? "sub" : "add")
               << " i64 " << cv << ", " << amt << "\n";
        G->out << "  store i64 " << nv << ", ptr " << iA << "\n";
      }
      G->out << "  br label %" << G->l(hdr) << "\n";
      G->out << G->l(exit) << ":\n";
      G->curLabelTerminated = false;
      G->loopStack.pop_back();
      break;
    }
    // Phase 1d (report §4.3/§4.4): match — tag dispatch on the enum value
    case Stmt::MATCH: {
      std::string sv = genExpr(s->matchOn);   // enum value = address
      Type* st = s->matchOn->ty;
      EnumDef* ed = st->edef;
      std::string tagp = G->t();
      G->out << "  " << tagp << " = getelementptr %enum." << ed->name
             << ", ptr " << sv << ", i32 0, i32 0\n";
      std::string tag = G->t();
      G->out << "  " << tag << " = load i64, ptr " << tagp << "\n";
      int joinBB = G->bb++;
      // arm blocks + check chain
      std::vector<int> armBBs;
      std::vector<int> checkBBs;
      int nArms = (int)s->matchArms.size();
      for (int i = 0; i < nArms; ++i) {
        armBBs.push_back(G->bb++);
        checkBBs.push_back(G->bb++);
      }
      // chain of tag comparisons. The FIRST check continues the current
      // block (no label, no branch needed); later checks are branched to
      // by the previous comparison's false-edge.
      for (int i = 0; i < nArms; ++i) {
        const MatchArm& arm = s->matchArms[i];
        if (i > 0) {
          G->out << G->l(checkBBs[i]) << ":\n";
          G->curLabelTerminated = false;
        }
        if (arm.wildcard) {
          G->out << "  br label %" << G->l(armBBs[i]) << "\n";
          G->terminate();
        } else {
          std::string c = G->t();
          G->out << "  " << c << " = icmp eq i64 " << tag << ", "
                 << arm.variantIdx << "\n";
          int next = (i + 1 < nArms) ? checkBBs[i + 1] : joinBB;
          G->out << "  br i1 " << c << ", label %" << G->l(armBBs[i])
                 << ", label %" << G->l(next) << "\n";
          G->terminate();
        }
      }
      // arm bodies with payload bindings
      for (int i = 0; i < nArms; ++i) {
        const MatchArm& arm = s->matchArms[i];
        G->out << G->l(armBBs[i]) << ":\n";
        G->curLabelTerminated = false;
        if (!arm.wildcard && arm.variantIdx >= 0) {
          EnumVariant& v = ed->variants[arm.variantIdx];
          for (size_t b = 0; b < arm.bindings.size() && b < v.fields.size(); ++b) {
            std::string fp = G->t();
            G->out << "  " << fp << " = getelementptr i8, ptr " << sv
                   << ", i64 " << (8 + v.offsets[b]) << "\n";
            Type* ft = v.fields[b].ty;
            std::string dst = G->varAllocas[arm.bindings[b]];
            if (ft->k == Type::STRUCT || ft->k == Type::ENUM ||
                ft->k == Type::ARRAY) {
              G->out << "  call void @llvm.memcpy.p0.p0.i64(ptr " << dst
                     << ", ptr " << fp << ", i64 " << typeSizeOf(ft)
                     << ", i1 false)\n";
            } else if (ft->isPair()) {
              std::string val = G->t();
              G->out << "  " << val << " = load { ptr, i64 }, ptr " << fp
                     << "\n";
              G->out << "  store { ptr, i64 } " << val << ", ptr " << dst
                     << "\n";
            } else {
              std::string val = G->t();
              G->out << "  " << val << " = load " << G->llvmType(ft)
                     << ", ptr " << fp << "\n";
              G->out << "  store " << G->scalarCodegenTy(ft) << " " << val
                     << ", ptr " << dst << "\n";
            }
          }
        }
        genStmt(arm.body);
        if (!G->curLabelTerminated) {
          G->out << "  br label %" << G->l(joinBB) << "\n";
          G->terminate();
        }
      }
      G->out << G->l(joinBB) << ":\n";
      G->curLabelTerminated = false;
      break;
    }
    case Stmt::PARFOR:
      genParfor(s);
      break;
  }
}

} // namespace freshet
