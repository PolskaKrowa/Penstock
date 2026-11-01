// Freshet strict checks — F0401/F0402 (slow arithmetic), F0403 (strided/wasteful
// access), F0404 (R2: field straddling cache line), F0407 (R2:
// struct padding), F0408 (R2: AoS->SoA advice), F0305, F0405/F0406 (loops).
#include "common.h"
#include <algorithm>

namespace freshet {

// ---------------------------------------------------------------- helpers
static const uint64_t CACHELINE = 64;

struct AccessInfo {
  std::string rootVar;
  Expr* indexExpr = nullptr;
  SrcLoc loc;
};

// find root var of an lvalue chain: v[i].f.g -> v ; also collect soa field use
static Expr* rootOf(Expr* e) {
  while (e && (e->k == Expr::INDEX || e->k == Expr::FIELD)) e = e->base;
  return e;
}

static void forEachExpr(Expr* e, const std::function<void(Expr*)>& f) {
  if (!e) return;
  f(e);
  for (auto* a : e->args) forEachExpr(a, f);
  forEachExpr(e->lhs, f);
  forEachExpr(e->rhs, f);
  forEachExpr(e->base, f);
  if (e->k == Expr::CAST) forEachExpr(e->base, f);
}

static void forEachStmt(Stmt* s, const std::function<void(Stmt*)>& f) {
  if (!s) return;
  f(s);
  switch (s->k) {
    case Stmt::LET: case Stmt::EXPR: case Stmt::DEFER:
    case Stmt::BREAK: case Stmt::CONTINUE:   // Phase 1a: leaf statements
      break;
    case Stmt::ASSIGN:
      break;
    case Stmt::RETURN:
      break;
    case Stmt::IF:
      forEachStmt(s->thenB, f);
      forEachStmt(s->elseB, f);
      break;
    case Stmt::MATCH:    // Phase 1d: walk every arm body
      for (auto& arm : s->matchArms) forEachStmt(arm.body, f);
      break;
    case Stmt::WHILE: case Stmt::FOR: case Stmt::PARFOR:
      forEachStmt(s->body, f);
      break;
    case Stmt::BLOCK:
      for (auto* st : s->stmts) forEachStmt(st, f);
      break;
    default: break;
  }
}

static void stmtExprs(Stmt* s, const std::function<void(Expr*)>& f) {
  switch (s->k) {
    case Stmt::LET: case Stmt::EXPR: case Stmt::DEFER:
    case Stmt::RETURN:
      forEachExpr(s->init ? s->init : (s->expr ? s->expr :
                    (s->deferExpr ? s->deferExpr : s->retVal)), f);
      break;
    case Stmt::MATCH:   // Phase 1d: scrutinee + bindings' payload exprs
      forEachExpr(s->matchOn, f);
      break;
    case Stmt::ASSIGN: forEachExpr(s->lhs, f); forEachExpr(s->rhs, f); break;
    case Stmt::IF: forEachExpr(s->cond, f); break;
    case Stmt::WHILE: forEachExpr(s->cond, f); break;
    case Stmt::FOR:
      if (s->forRange) { forEachExpr(s->rangeLo, f); forEachExpr(s->rangeHi, f); }
      else { forEachExpr(s->forInit, f); forEachExpr(s->forCond, f);
             forEachExpr(s->forStep, f); }
      break;
    case Stmt::PARFOR:
      forEachExpr(s->plo, f); forEachExpr(s->phi, f);
      break;
    case Stmt::BLOCK: break;
    default: break;
  }
}

// ---------------------------------------------------------------- F0401/F0402 ARITH
// report §7: performance diagnostics are WARNINGS by default — a slow but
// correct program compiles.  `--strict-performance` (or `@hpc_strict` on
// the enclosing function) restores the historical hard errors for the HPC
// workflow.  Division by a literal ZERO stays a hard error in every mode:
// that is a correctness issue, not a performance preference.
static bool perfStrict(FnDecl* fn) {
  return g_opts.strictPerformance || (fn && fn->hpcStrict);
}
static void checkArith(FnDecl* fn, Expr* e) {
  if (e->k != Expr::BIN) return;
  const std::string& op = e->op;
  if (op != "/" && op != "%") return;
  // float division is allowed (pipelined); integer div/mod is the slow path
  if (!e->ty || !e->ty->isInt()) return;
  Expr* divisor = e->rhs;
  if (divisor->constant && divisor->k == Expr::INT) {
    uint64_t d = divisor->ival;
    if (d == 0) {
      diag(Diag::Error, Code::ArithDivZero, "division by zero",
           e->loc.file, e->loc.line, e->loc.col);
      return;
    }
    if ((d & (d - 1)) != 0) {
      diag(perfStrict(fn) ? Diag::Error : Diag::Warning,
           Code::ArithDivConst,
           "integer division by non-power-of-2 constant " + std::to_string(d) +
           " — costs ~20-40 cycles; strength-reduce it (e.g. x / 8, or "
           "multiply by the reciprocal constant yourself)"
           + std::string(perfStrict(fn) ? "" :
                         " (--strict-performance makes this an error)"),
           e->loc.file, e->loc.line, e->loc.col,
           "divisor=" + std::to_string(d));
      return;
    }
  } else {
    diag(perfStrict(fn) ? Diag::Error : Diag::Warning,
         Code::ArithDivRuntime,
         "integer division by runtime value — latency 20-40 cycles and "
         "not pipelined; hoist to a constant power-of-2, or restructure"
         + std::string(perfStrict(fn) ? "" :
                       " (--strict-performance makes this an error)"),
         e->loc.file, e->loc.line, e->loc.col, "divisor=runtime");
  }
}

// ---------------------------------------------------------------- F0403
// p[i * K] with constant K: bytes touched per element = K * elemsize
static void checkStrided(FnSema* fs, Expr* e) {
  if (e->k != Expr::INDEX) return;
  Expr* idx = e->rhs;
  Type* bt = e->base->ty;
  if (!bt || bt->k != Type::PTR) return;
  uint64_t elem = typeSizeOf(bt->pointee);
  if (elem < 8) return;   // small elements: hardware handles fine
  // idx == C * k or idx == k * C
  auto mulConst = [&](Expr* x, uint64_t* c) -> Expr* {
    if (x->k != Expr::BIN || x->op != "*") return nullptr;
    if (x->lhs->constant && x->lhs->k == Expr::INT) { *c = x->lhs->ival; return x->rhs; }
    if (x->rhs->constant && x->rhs->k == Expr::INT) { *c = x->rhs->ival; return x->lhs; }
    return nullptr;
  };
  uint64_t c = 0;
  Expr* k = mulConst(idx, &c);
  if (!k || c <= 1) {
    // also p[i + C] patterns: unit stride with offset — fine
    return;
  }
  uint64_t bytes = c * elem;
  double eff = (double)elem / (double)alignUp(bytes, CACHELINE);
  if (bytes >= CACHELINE) {
    // report §7: warning unless the strict HPC policy is active
    bool strict = perfStrict(fs ? fs->fn : nullptr);
    diag(strict ? Diag::Error : Diag::Warning,
         Code::CacheStride,
         "strided access: " + std::to_string(c) + " x " +
         std::to_string(elem) + "B = " + std::to_string(bytes) +
         "B per element — every touch burns a full cache line (" +
         std::string(eff < 0.15 ? "13%" : "25%") + " efficiency); unit-stride "
         "or blocked traversal recommended",
         e->loc.file, e->loc.line, e->loc.col,
         "stride_bytes=" + std::to_string(bytes));
  } else {
    diag(Diag::Warning, Code::CacheStride,
         "strided access " + std::to_string(bytes) + "B/element (" +
         std::to_string((int)(eff * 100)) + "% line efficiency)",
         e->loc.file, e->loc.line, e->loc.col,
         "stride_bytes=" + std::to_string(bytes));
  }
}

// ------------------------------------------------- R2: struct layout checks
// Called once per program: report layouts; per loop: AoS->SoA + straddle.
struct SoaAdvice {
  std::string fnName;
  int loopLine;
  std::string rootVar;
  StructDef* sdef;
  std::set<std::string> fieldsUsed;
  uint64_t bytesTouched = 0, bytesNeeded = 0;
};

static std::vector<SoaAdvice> g_soaAdvice;

static void reportStructLayouts(Program* p) {
  for (auto* sd : p->structs) {
    // F0407: if padding could be eliminated by reordering
    if (sd->padBytes > 0) {
      // simulate best order (size-descending within align groups)
      std::vector<uint64_t> sizes, aligns;
      for (auto& f : sd->fields) {
        sizes.push_back(typeSizeOf(f.ty));
        aligns.push_back(typeAlignOf(f.ty));
      }
      std::vector<size_t> order(sizes.size());
      for (size_t i = 0; i < order.size(); ++i) order[i] = i;
      std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return aligns[a] > aligns[b] || (aligns[a] == aligns[b] && sizes[a] > sizes[b]);
      });
      uint64_t off = 0, maxa = 1;
      for (size_t i : order) {
        off = alignUp(off, aligns[i]);
        off += sizes[i];
        maxa = std::max(maxa, aligns[i]);
      }
      uint64_t best = alignUp(off, maxa);
      if (best < sd->size) {
        diag(Diag::Warning, Code::LayoutPad,
             "struct " + sd->name + ": " + std::to_string(sd->size) +
             "B with " + std::to_string(sd->padBytes) +
             "B padding; declaring fields largest-first shrinks it to " +
             std::to_string(best) + "B (" +
             std::to_string(sd->size - best) + "B saved per element)",
             sd->loc.file, sd->loc.line, sd->loc.col,
             "struct=" + sd->name + ",size=" + std::to_string(sd->size) +
             ",optimal=" + std::to_string(best));
      }
    }
  }
}

// For loops over arrays of structs: collect fields touched in body.
static void checkSoaLoop(FnDecl* fn, Stmt* loop) {
  // find indexing patterns root[i].field  (or root[i].field.g...)
  std::map<std::string, SoaAdvice> perVar;
  auto collect = [&](Expr* e) {
    if (e->k != Expr::FIELD) return;
    // base chain: ... [i] . f (maybe more fields)
    Expr* idx = e->base;
    std::vector<std::string> path;
    Expr* node = e;
    while (node->k == Expr::FIELD) {
      path.push_back(node->name);
      node = node->base;
    }
    if (node->k != Expr::INDEX) return;
    // element struct type
    Type* bt = node->base->ty;
    if (!bt) return;
    StructDef* sd = nullptr;
    if (bt->k == Type::SOA) {
      // soa access — already transformed; no advice needed
      bt = nullptr;
      return;
    }
    if (bt->k == Type::PTR && bt->pointee && bt->pointee->k == Type::STRUCT)
      sd = bt->pointee->sdef;
    else if (bt->k == Type::PTR && bt->pointee && bt->pointee->k == Type::PTR)
      return;  // **struct: deeper pattern, skip
    if (!sd) return;
    Expr* root = rootOf(node);
    if (!root || root->k != Expr::IDENT) return;
    SoaAdvice& adv = perVar[root->name];
    if (adv.sdef == nullptr) {
      adv.sdef = sd;
      adv.rootVar = root->name;
      adv.fnName = fn->name;
      adv.loopLine = loop->loc.line;
    } else if (adv.sdef != sd) {
      return;
    }
    // record top-level field
    adv.fieldsUsed.insert(path.back());
  };
  // walk loop body exprs (excluding bounds)
  std::function<void(Stmt*)> walkBody = [&](Stmt* s) {
    if (!s) return;
    stmtExprs(s, collect);
    switch (s->k) {
      case Stmt::IF: walkBody(s->thenB); walkBody(s->elseB); break;
      case Stmt::WHILE: case Stmt::FOR: case Stmt::PARFOR: walkBody(s->body); break;
      case Stmt::BLOCK: for (auto* st : s->stmts) walkBody(st); break;
      default: break;
    }
  };
  walkBody(loop->body);

  for (auto& [v, adv] : perVar) {
    uint64_t needed = 0;
    for (auto& f : adv.sdef->fields) {
      if (adv.fieldsUsed.count(f.name)) {
        // bytes actually consumed per element: the field size
        needed += adv.sdef->sizes[f.idx];
      }
    }
    // per-element bytes fetched: one struct per iteration (elements amortize
    // cache lines; a struct larger than 64B spans multiple lines)
    adv.bytesTouched = adv.sdef->size;
    adv.bytesNeeded = needed;
    double eff = (double)adv.bytesNeeded / (double)adv.bytesTouched;
    if (eff < 0.5) {
      diag(Diag::Warning, Code::Soa,
           "AoS->SoA: loop at " + std::to_string(adv.loopLine) +
           " reads " + std::to_string(adv.bytesNeeded) + "B of " +
           std::to_string(adv.bytesTouched) + "B fetched per element of '" +
           adv.rootVar + "' (" + std::to_string((int)(eff * 100)) +
           "% cache efficiency); declare it soa(" + adv.sdef->name +
           ") to store fields in separate planes",
           loop->loc.file, loop->loc.line, loop->loc.col,
           "var=" + adv.rootVar + ",struct=" + adv.sdef->name +
           ",needed=" + std::to_string(adv.bytesNeeded) +
           ",touched=" + std::to_string(adv.bytesTouched));
      g_soaAdvice.push_back(adv);
    }
    // F0404: a used field straddles a 64B boundary and the array base
    // is not guaranteed 64-aligned
    for (auto& f : adv.sdef->fields) {
      if (!adv.fieldsUsed.count(f.name)) continue;
      uint64_t off = adv.sdef->offsets[f.idx];
      uint64_t sz = adv.sdef->sizes[f.idx];
      if (sz >= CACHELINE) continue;
      if (adv.sdef->align >= CACHELINE) continue;  // base aligned: never straddles
      if ((off % CACHELINE) + sz > CACHELINE) {
        // report §7: warning unless the strict HPC policy is active
        bool strict = perfStrict(fn);
        diag(strict ? Diag::Error : Diag::Warning,
             Code::CacheLineStraddle,
             "hot field " + f.name + " of struct " + adv.sdef->name +
             " (offset " + std::to_string(off) + ", size " +
             std::to_string(sz) + ") straddles a 64-byte cache line — "
             "half of accesses touch two lines; declare struct " +
             adv.sdef->name + " align(64) or reorder fields",
             loop->loc.file, loop->loc.line, loop->loc.col,
             "struct=" + adv.sdef->name + ",field=" + f.name);
      }
    }
  }
}

// ---------------------------------------------------------------- F0305
// Stage B4 (report §1.3 + §2.2 B): resolve the spec/implementation contradiction.
//
// Language contract (now documented in docs/language-spec.md §3):
//   `@alias(a, b)` is a PROMISE by the caller that parameters `a` and `b`
//   do NOT overlap.  It is not a permit to pass overlapping memory.
//
// Therefore:
//   - Two pointer args that provably alias (same root var, same SoA plane,
//     same base address) and whose parameter pair is NOT declared @alias
//     => F0305 (the historical behaviour, kept).
//   - Two pointer args that provably alias and whose parameter pair IS
//     declared @alias => F0306: the call violates the user-given
//     no-overlap promise.  This is a warning rather than an error so a
//     program that compiles against the historical (buggy) checker can
//     still link, but the user is told loud and clear that the @alias
//     attribute on this call is unsound and must not be trusted by the
//     optimiser.
//   - Args that provably DON'T alias => fine, no diagnostic.
//   - Args whose root cannot be resolved (freshet.* results, arithmetic,
//     call return values) => conservatively fine: we cannot prove overlap
//     so we emit no diagnostic and (importantly) emit NO noalias fact to
//     LLVM either.
//
// Stage B5: when F0306 fires, the offending (fn, param-a, param-b)
// triple is recorded in g_unsoundAliasPairs so IRgen can suppress the
// corresponding `noalias` attribute on the function definition.  Only
// `@alias` declarations that have ZERO lying callers in this compilation
// unit are lowered to LLVM `noalias` — the report's principle of
// "lower only sound facts".
namespace {
struct AliasPairKey {
  FnDecl* fn;
  std::string a, b;
  bool operator<(const AliasPairKey& o) const {
    if (fn != o.fn) return fn < o.fn;
    if (a != o.a) return a < o.a;
    return b < o.b;
  }
};
static std::set<AliasPairKey> g_unsoundAliasPairs;
}

// Returns true iff the given @alias pair on `fn` has been violated by at
// least one caller in this compilation unit.  IRgen consults this before
// emitting the `noalias` parameter attribute (Stage B5).
bool aliasPairIsUnsound(FnDecl* fn, const std::string& a, const std::string& b) {
  return g_unsoundAliasPairs.count({fn, a, b}) > 0 ||
         g_unsoundAliasPairs.count({fn, b, a}) > 0;
}

static void checkAlias(FnDecl* fn, Expr* e) {
  if (e->k != Expr::CALL) return;
  FnDecl* callee = e->callee;
  if (!callee) return;
  // collect alias-declared pairs (symmetric for lookup convenience)
  std::set<std::pair<std::string, std::string>> aliasDeclared;
  for (auto& pr : callee->aliasPairs) {
    aliasDeclared.insert({pr.first, pr.second});
    aliasDeclared.insert({pr.second, pr.first});
  }
  for (size_t i = 0; i < callee->params.size(); ++i) {
    for (size_t j = i + 1; j < callee->params.size(); ++j) {
      Type* ti = callee->params[i].ty;
      Type* tj = callee->params[j].ty;
      if (!ti || !tj || !ti->isPtr() || !tj->isPtr()) continue;
      bool declared = aliasDeclared.count({callee->params[i].name,
                                           callee->params[j].name});
      Expr* ai = e->args[i];
      Expr* aj = e->args[j];
      if (!ai || !aj) continue;
      // root vars
      Expr* ri = rootOf(ai);
      Expr* rj = rootOf(aj);
      auto rootName = [](Expr* r) -> std::string {
        if (r && r->k == Expr::IDENT) return r->name;
        if (r && r->k == Expr::CALL) return r->name;  // freshet.* results
        return "";
      };
      std::string ni = rootName(ri);
      std::string nj = rootName(rj);
      if (ni.empty() || nj.empty()) continue;      // unknown: cannot decide
      if (ni != nj) continue;      // different roots: provably distinct by
                                   // language contract; @alias would be a
                                   // tautology here but is not a lie
      // same root: soa planes of different fields never alias
      auto planeField = [](Expr* a) -> std::string {
        Expr* r = rootOf(a);
        // soa access path: s[i].f -> plane f
        Expr* node = a;
        std::string fld;
        while (node && (node->k == Expr::FIELD || node->k == Expr::INDEX)) {
          if (node->k == Expr::FIELD) fld = node->name;
          node = node->base;
        }
        if (node && node->k == Expr::IDENT) {
          // check the root's type is soa
          return fld;  // caller verifies soa-ness
        }
        return "";
      };
      // determine if root is soa
      bool rootIsSoa = false;
      if (ri && ri->k == Expr::IDENT && ri->sym && ri->sym->ty &&
          ri->sym->ty->k == Type::SOA)
        rootIsSoa = true;
      if (rj && rj->k == Expr::IDENT && rj->sym && rj->sym->ty &&
          rj->sym->ty->k == Type::SOA)
        rootIsSoa = true;
      if (rootIsSoa) {
        std::string fi = planeField(ai), fj = planeField(aj);
        if (!fi.empty() && !fj.empty() && fi != fj) continue;  // distinct planes
      }
      // same root, same plane: provably same address => alias
      if (declared) {
        // user promised no-overlap but the call violates it -> record so
        // IRgen does NOT lower this @alias pair to a noalias attribute
        g_unsoundAliasPairs.insert({callee,
            callee->params[i].name, callee->params[j].name});
        diag(Diag::Warning, Code::AliasLie,
             "call to " + callee->name + ": args " + std::to_string(i + 1) +
             " and " + std::to_string(j + 1) + " alias the same memory '" + ni +
             "' but the function declares @alias(" +
             callee->params[i].name + "," + callee->params[j].name +
             ") — the no-overlap promise is broken on this call site; "
             "either remove @alias from the declaration or change the "
             "callers.  The optimiser will NOT be told these args are "
             "noalias for this compilation unit.",
             e->loc.file, e->loc.line, e->loc.col,
             "root=" + ni + ",declared=" + callee->params[i].name + "," +
             callee->params[j].name);
      } else {
        diag(Diag::Error, Code::AliasViolation,
             "call to " + callee->name + ": args " + std::to_string(i + 1) +
             " and " + std::to_string(j + 1) + " alias the same memory '" + ni +
             "' — parameters are noalias by contract; if the call is "
             "intentional you must declare @alias(" +
             callee->params[i].name + "," + callee->params[j].name +
             ") on the function AND ensure EVERY caller passes "
             "non-overlapping memory",
             e->loc.file, e->loc.line, e->loc.col,
             "root=" + ni);
      }
    }
  }
}

// ---------------------------------------------------------------- F0405/F0406 LOOP
static void checkLoop(FnDecl* fn, FnSema* fs, Stmt* s) {
  if (s->k != Stmt::FOR || s->forRange) return;
  // stride must be constant ±1
  Expr* step = s->forStep;
  if (!step) return;
  // patterns: i += C | i = i + C | i = i - C | i -= C
  uint64_t stride = 0;
  bool ok = false;
  if (step->k == Expr::BIN && step->op == "=" &&
      step->rhs && step->rhs->k == Expr::BIN) {
    Expr* r = step->rhs;
    if ((r->op == "+" || r->op == "-") && r->rhs->constant &&
        r->rhs->k == Expr::INT) { stride = r->rhs->ival; ok = true; }
  } else if (step->k == Expr::BIN && (step->op == "+=" || step->op == "-=") &&
             step->rhs && step->rhs->constant && step->rhs->k == Expr::INT) {
    stride = step->rhs->ival;
    ok = true;
  } else if (step->k == Expr::BIN && step->op == "=" && step->rhs &&
             step->rhs->constant && step->rhs->k == Expr::INT) {
    stride = step->rhs->ival; ok = true;  // i = 5 etc (weird but constant)
  }
  if (!ok || stride != 1) {
    // report §7: non-unit strides compile (the lowering honours the step);
    // they are a performance note unless the strict HPC policy is active
    bool strict = perfStrict(fn);
    diag(strict ? Diag::Error : Diag::Warning,
         Code::LoopStride,
         "for-loop stride is not constant 1 — vectorization guarantees "
         "are lost; use a nested loop or manual blocking for other strides"
         + std::string(strict ? "" :
                       " (--strict-performance makes this an error)"),
         s->loc.file, s->loc.line, s->loc.col);
  }
  // induction var must not be assigned inside body
  bool writesIv = false;
  forEachStmt(s->body, [&](Stmt* st) {
    if (st->k != Stmt::ASSIGN) return;
    Expr* t = st->lhs;
    while (t->k == Expr::FIELD || t->k == Expr::INDEX) t = t->base;
    if (t->k == Expr::IDENT && t->name == s->ivar) writesIv = true;
  });
  if (writesIv) {
    bool strict = perfStrict(fn);
    diag(strict ? Diag::Error : Diag::Warning,
         Code::LoopIvWritten,
         "loop induction variable '" + s->ivar + "' modified in body — "
         "the loop still executes correctly, but cannot be vectorized",
         s->loc.file, s->loc.line, s->loc.col);
  }
}

// ---------------------------------------------------------------- driver
void runStrictChecks(Program* p) {
  reportStructLayouts(p);

  for (auto* fs : allFnSemas()) {
    FnDecl* fn = fs->fn;
    // every statement
    std::function<void(Stmt*)> walk = [&](Stmt* s) {
      if (!s) return;
      stmtExprs(s, [&](Expr* e) {
        checkArith(fn, e);
        checkStrided(fs, e);
        checkAlias(fn, e);
      });
      switch (s->k) {
        case Stmt::IF: walk(s->thenB); walk(s->elseB); break;
        case Stmt::MATCH:
          for (auto& arm : s->matchArms) walk(arm.body);
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
    walk(fn->body);
    // loop-specific checks
    forEachStmt(fn->body, [&](Stmt* s) {
      checkLoop(fn, fs, s);
      if (s->k == Stmt::FOR || s->k == Stmt::WHILE || s->k == Stmt::PARFOR) {
        checkSoaLoop(fn, s);
      }
    });
  }
}

const std::vector<SoaAdvice>& soaAdviceList() { return g_soaAdvice; }

} // namespace freshet
