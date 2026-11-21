// Freshet effect summaries (audit P0, report §2.3).
//
// Replaces the historical boolean purity walk in sema.cpp step 6.  That
// walk had two unsound behaviours:
//   1. it conflated "does not write memory" with "accesses no memory",
//      so `fn read_first(p: *i64) -> i64 { return p[0]; }` was marked
//      `memory(none) willreturn` and the generated executable could
//      return stale values at -O3 (the audit reproducer printed `00`
//      instead of `12`); and
//   2. `@pure` bypassed the analysis entirely (`f->isPure = f->userPure`)
//      although the language spec documents @pure as a contract whose
//      violations are rejected.
//
// This pass computes a conservative interprocedural effect lattice per
// function (reads/writes split into argument-reachable memory vs
// globals/heap/unknown, plus allocation, free, I/O, unknown calls and a
// termination proof) and uses it to:
//   - derive `isPure` ("no observable side effects", reads allowed) for
//     the parfor race checks and the R4 loop passes;
//   - drive sound LLVM attribute selection in IRgen (memory(none) only
//     when no memory is touched at all; memory(argmem: read) for
//     read-only pointer consumers; `willreturn` only when termination
//     is PROVED — see mayNotReturn below);
//   - validate every user `@pure` against the spec §3.1 contract
//     ("reads no memory and writes no memory") with F0304 errors.
//
// Soundness policy: when the analysis cannot decide, it grants the
// CONSERVATIVE (broader) summary.  Local allocas (scalars, structs by
// value) are not observable from outside and therefore do not count as
// memory effects — this matches how LLVM reasons about freshly created
// stack slots and keeps simple scalar helpers eligible for memory(none).
#include "common.h"
#include <functional>
#include <map>
#include <set>
#include <vector>

namespace freshet {

namespace {

// provenance of the root variable of a FIELD/INDEX chain
enum RootKind { RK_SCALAR_LOCAL, RK_PARAM, RK_LOCAL_PTR, RK_STACK,
                RK_GLOBAL, RK_UNKNOWN };

// names of this fn bound (directly or through copies/spans/casts) to
// stack_alloc memory — filled per-fn by collectStackRooted; single-
// threaded pass state like the rest of the walker
static std::set<std::string> g_stackRooted;

static void collectStackRooted(Stmt* s);

RootKind classifyRoot(FnSema* fs, Expr* root) {
  if (!root || root->k != Expr::IDENT) return RK_UNKNOWN;
  const std::string& n = root->name;
  // data stacks (goose port v2): a binding rooted at stack_alloc memory
  // is per-thread scratch — invisible outside the call — checked before
  // the generic pointer case below
  if (!n.empty() && g_stackRooted.count(n)) return RK_STACK;
  if (fs) {
    if (Sym* s = fs->lookup(n)) {
      if (s->k == Sym::PARAM) return RK_PARAM;
      // struct-by-value locals (and every scalar) are allocas; pointer /
      // soa locals denote memory whose provenance we cannot prove here
      Type* t = s->ty;
      if (t && (t->k == Type::PTR || t->k == Type::SOA)) return RK_LOCAL_PTR;
      return RK_SCALAR_LOCAL;
    }
  }
  if (root->globalRef || lookupGlobal(n)) return RK_GLOBAL;
  return RK_UNKNOWN;
}

// fill g_stackRooted from a fn body: LET names whose initializer is a
// stack_alloc, a cast of an already-rooted name, or a re-binding from
// one — in declaration order (copies follow their source).  A span LET
// (let s = span(p, n)) stays a VALUE binding: its construction is an
// unmodelled builtin (unknownCall below), which conservatively covers
// every access through it in the same fn.
static void collectStackRooted(Stmt* s) {
  if (!s) return;
  auto exprRoots = [&](Expr* e, const std::string& var) {
    if (!e || var.empty()) return;
    if (e->k == Expr::CALL && e->name == "stack_alloc") {
      g_stackRooted.insert(var);
    } else if (e->k == Expr::CAST) {
      Expr* b = e->base;
      if (b && b->k == Expr::IDENT && !b->name.empty() &&
          g_stackRooted.count(b->name))
        g_stackRooted.insert(var);
    }
  };
  switch (s->k) {
    case Stmt::LET: exprRoots(s->init, s->var); break;
    case Stmt::ASSIGN:
      // re-binding a rooted name from another rooted name keeps it rooted
      if (s->lhs && s->lhs->k == Expr::IDENT && s->rhs &&
          s->rhs->k == Expr::IDENT && !s->rhs->name.empty() &&
          g_stackRooted.count(s->rhs->name))
        g_stackRooted.insert(s->lhs->name);
      else if (s->lhs && s->lhs->k == Expr::IDENT)
        g_stackRooted.erase(s->lhs->name);   // unrooted rebinding
      break;
    default: break;
  }
  collectStackRooted(s->body);
  collectStackRooted(s->thenB);
  collectStackRooted(s->elseB);
  for (auto* st : s->stmts) collectStackRooted(st);
  for (auto& arm : s->matchArms) collectStackRooted(arm.body);
}

// root of a FIELD/INDEX chain (field-derefs and indexes are the memory
// accesses; plain by-value field chains bottom out in an alloca)
Expr* chainRoot(Expr* e, bool* memoryAccess) {
  while (e && (e->k == Expr::FIELD || e->k == Expr::INDEX)) {
    if (memoryAccess && (e->k == Expr::INDEX || e->ptrDeref)) *memoryAccess = true;
    e = e->base;
  }
  return e;
}

void readOfRoot(FnSema* fs, Expr* root, Effects& e) {
  switch (classifyRoot(fs, root)) {
    case RK_PARAM:        e.readsArg = true; break;
    case RK_LOCAL_PTR:    e.readsArg = e.readsOther = true; break;  // param or heap
    case RK_STACK:        e.readsStack = true; break;   // per-thread scratch
    case RK_GLOBAL:       e.readsOther = true; break;
    case RK_SCALAR_LOCAL: break;                                  // alloca load
    case RK_UNKNOWN:      e.readsArg = e.readsOther = true; break;
  }
}
void writeOfRoot(FnSema* fs, Expr* root, Effects& e) {
  switch (classifyRoot(fs, root)) {
    case RK_PARAM:        e.writesArg = true; break;
    case RK_LOCAL_PTR:    e.writesArg = e.writesOther = true; break;
    case RK_STACK:        e.writesStack = true; break;  // per-thread scratch
    case RK_GLOBAL:       e.writesOther = true; break;
    case RK_SCALAR_LOCAL: break;                                  // alloca store
    case RK_UNKNOWN:      e.writesArg = e.writesOther = true; break;
  }
}

// one read through a FIELD/INDEX chain whose root is `root`. Both
// memory reads and non-memory reads (e.g. a whole global in `g = g + 1`)
// funnel through the same root classification.
void readThrough(FnSema* fs, Expr* leaf, Effects& e) {
  bool mem = false;
  Expr* root = chainRoot(leaf, &mem);
  (void)mem;
  readOfRoot(fs, root, e);
}

// effect of the implicit bounds check a checked build emits at this
// site (see checkSiteEffect below).
// A bounds-check site (checked builds / -O0/-O1) may TRAP: the abort path
// writes stderr and does not return, exactly like `assert`.  A function
// whose effect summary omits this is attributed `willreturn` (and possibly
// stronger), which let the -O1+ backend delete a failing check's trap as
// side-effect-free dead code — wrong code in every checked build (found
// while testing the BCE port: an out-of-range index printed 0.00 instead
// of aborting at -O2).  Mirrors irgen_expr.cpp's emitBoundsCheck call
// sites; the BCE pass may later elide the check, which only loses the
// willreturn attribute (never unsound).
static void checkSiteEffect(Expr* e, Effects& eff) {
  if (g_opts.noBoundsCheck) return;
  if (!g_opts.checked && g_opts.optLevel > 1) return;
  Type* bt = e->base ? e->base->ty : nullptr;
  bool emits = false;
  if (e->k == Expr::SLICE) {
    emits = true;                      // 0 <= lo <= hi <= len is checked
  } else if (bt && (bt->k == Type::SPAN || bt->k == Type::STR)) {
    emits = true;
  } else if (bt && bt->k == Type::ARRAY) {
    // a constant index in range is proven in sema and never checked
    emits = !(e->rhs && e->rhs->k == Expr::INT && e->rhs->constant &&
              e->rhs->ival < bt->arrLen);
  }
  if (emits) {
    eff.io = true;
    eff.mayNotReturn = true;
  }
}

// effect of a call to a builtin, by name; unmodelled names get
// e.unknownCall (conservative).  `strictfp` selects the strict-FP
// lowering, where sin/cos/... stay scalar libm calls that may
// read/write errno (memory).
void builtinEffects(const std::string& n, bool strictfp, Effects& e) {
  // lowered to llvm.* intrinsics: memory-free
  static const std::set<std::string> kIntrinsicMath = {
    "sqrt", "fabs", "floor", "ceil", "round", "fmin", "fmax", "pow",
    "exp", "log",
    "imin", "imax",   // icmp + select — memory-free (audit §6.7)
  };
  if (kIntrinsicMath.count(n)) return;
  // fast FP (the language default) lowers these to llvm.sin/.cos/.log2/
  // .log10 intrinsics; @strictfp functions keep scalar libm calls that
  // may touch errno — model that as reading and writing other memory.
  if (n == "sin" || n == "cos" || n == "log2" || n == "log10") {
    if (!strictfp) return;
    e.readsOther = e.writesOther = true;
    return;
  }
  if (n == "tan") {                    // no LLVM intrinsic — always libm
    e.readsOther = e.writesOther = true;
    return;
  }
  if (n == "malloc" || n == "soa_new") { e.allocates = true; return; }
  if (n == "free" || n == "soa_free")  { e.freesMem = true; return; }
  // fsht-02:01 (registry 02.09.01): dynamic-collection builtins —
  // constructors allocate; growth mutators may realloc (alloc + write
  // through the container root); in-place mutators write through the
  // container root; vec_free frees.  The read-only accessors classify
  // through the argument root in the call-site walker (below), exactly
  // like soa_len/soa_field_ptr.
  if (n == "vec_new" || n == "vec_from_span" || n == "vec_from_array" ||
      n == "vec_clone") { e.allocates = true; return; }
  if (n == "vec_push" || n == "vec_insert" || n == "vec_resize" ||
      n == "vec_reserve") {
    e.allocates = true;
    e.writesArg = true;
    return;
  }
  if (n == "vec_pop" || n == "vec_remove" || n == "vec_truncate" ||
      n == "vec_clear") { e.writesArg = true; return; }
  if (n == "vec_free") { e.freesMem = true; return; }
  if (n == "vec_len" || n == "vec_capacity" || n == "vec_is_empty" ||
      n == "vec_get" || n == "vec_slice")
    return;   // reads classified through the argument root
  // data stacks (goose port v2): the bump loads and stores the
  // thread-local stack-top array — per-thread scratch that no other
  // thread, iteration or caller can observe (the %sp-threaded parfor
  // dispatch gives every executing thread its own stacks), so it is
  // NOT "other" memory for the observable-impurity lattice; the
  // fn-attribute derivation still sees it (memory(readwrite), never
  // memory(none)) via readsStack/writesStack
  if (n == "stack_alloc") {
    e.readsStack = true;
    e.writesStack = true;
    return;
  }
  if (n == "print" || n == "print_f64" || n == "print_i64" ||
      n == "print_u64" || n == "println") { e.io = true; return; }
  if (n == "assert") {                  // abort path: writes stderr, may not return
    e.io = true;
    e.mayNotReturn = true;
    return;
  }
  if (n == "freshet.threads" || n == "freshet.time_ns" ||
      n == "freshet.time_ms" || n == "freshet.cycles") {
    e.readsOther = true;                // runtime / clock state
    return;
  }
  if (n == "freshet.pause") return;     // spin hint, no memory
  if (n == "freshet.clflush") { e.writesOther = true; return; }
  if (n == "sizeof" || n == "alignof" || n == "give") return;  // folded/marker
  // memcpy / memset / soa_len / soa_field_ptr / soa_new_sizes /
  // prefetch_r / prefetch_w depend on their argument roots — handled by
  // the call site walker.
  if (n == "memcpy" || n == "memset" || n == "soa_len" ||
      n == "soa_field_ptr" || n == "soa_new_sizes" ||
      n == "freshet.prefetch_r" || n == "freshet.prefetch_w")
    return;
  // fsht-03:01 (registry 08.01.04, ch. 23 §2–§3): the numeric variant
  // family, conversions, bits and abs/clamp lower to llvm.* intrinsics
  // and icmp/select chains — memory-free at every width
  if (n == "checked_add" || n == "checked_sub" || n == "checked_mul" ||
      n == "checked_div" || n == "checked_rem" ||
      n == "saturating_add" || n == "saturating_sub" || n == "saturating_mul" ||
      n == "wrapping_add" || n == "wrapping_sub" || n == "wrapping_mul" ||
      n == "int_min" || n == "int_max" ||
      n == "checked_cast" || n == "saturating_cast" ||
      n == "bswap16" || n == "bswap32" || n == "bswap64" ||
      n == "popcount" || n == "abs" || n == "clamp")
    return;
  // gcd calls the runtime helper — pure computation over its operands
  if (n == "gcd") return;
  // parse_i64/u64/f64: reads the str view's bytes (an argument root —
  // classified through the call-site walker like vec reads)
  if (n == "parse_i64" || n == "parse_u64" || n == "parse_f64") return;
  // now_ns/now_ms/page_size read runtime/clock state
  if (n == "now_ns" || n == "now_ms" || n == "page_size") {
    e.readsOther = true;
    return;
  }
  // sm64_next reads+writes through its &mut state root — argmem,
  // classified through the call-site walker
  if (n == "sm64_next") {
    e.readsArg = true;
    e.writesArg = true;
    return;
  }
  // fsht-03:02 (ch. 24 §4): the String migration's new builtins —
  // clone allocates a fresh buffer; clear writes through the root
  if (n == "string_clone") { e.allocates = true; return; }
  if (n == "string_clear") { e.writesArg = true; return; }
  e.unknownCall = true;                 // unmodelled name: be conservative
}

// does the expression pass memory roots (pointer values / lvalue chains)
// whose provenance is a LOCAL of the caller?  If a callee that
// reads/writes argument memory receives such a root, the callee may
// touch this function's own stack or heap, which is "other" memory, not
// argmem — recording only writesArg there would be unsound.
struct RootProbe { bool anyLocal = false, anyGlobal = false;
                   bool anyStack = false; };
void probeArgRoots(FnSema* fs, Expr* arg, RootProbe& p) {
  if (!arg) return;
  Expr* r = arg;
  if (r->k == Expr::UN && r->op == "&") r = r->rhs;
  bool mem = false;
  r = chainRoot(r, &mem);
  if (!r || r->k != Expr::IDENT) return;
  switch (classifyRoot(fs, r)) {
    case RK_SCALAR_LOCAL:
    case RK_LOCAL_PTR: p.anyLocal = true; break;
    case RK_GLOBAL:    p.anyGlobal = true; break;
    case RK_STACK:     p.anyStack = true; break;
    default: break;
  }
}

struct PureViolation {
  std::string msg;
  SrcLoc loc;
};

// effect walker for one function body.  `callees` supplies the current
// (fixed-point) summaries for user calls; `viol` collects @pure
// contract violations for the final diagnostics (only filled when the
// function is user-declared @pure).
struct EffectWalker {
  FnDecl* f;
  FnSema* fs;
  std::map<FnDecl*, Effects>* callees;
  std::vector<PureViolation>* viol;

  void violate(const std::string& msg, SrcLoc loc) {
    if (viol && viol->size() < 4) viol->push_back({msg, loc});
  }

  // ---- expressions ------------------------------------------------------
  void walkExpr(Expr* e, Effects& eff) {
    if (!e) return;
    switch (e->k) {
      case Expr::INT: case Expr::FLT: case Expr::STR: case Expr::BOOL:
      case Expr::NILQ:
        return;
      case Expr::IDENT: {
        if (e->globalRef || (fs && !fs->lookup(e->name) && lookupGlobal(e->name))) {
          eff.readsOther = true;
          if (viol) violate("reads global '" + e->name + "'", e->loc);
        }
        return;   // locals are alloca loads
      }
      case Expr::INDEX: {
        // p[i] / soa[i] — a load through the base (or a soa plane access)
        readThrough(fs, e, eff);
        if (viol) violate("reads memory through '" + rootName(e) + "'", e->loc);
        checkSiteEffect(e, eff);   // the bounds check may trap (like assert)
        walkExpr(e->rhs, eff);
        walkExpr(e->base, eff);
        return;
      }
      case Expr::FIELD: {
        if (e->ptrDeref) {              // p.f through *Struct — real memory
          readThrough(fs, e, eff);
          if (viol) violate("reads memory through '" + rootName(e) + "'", e->loc);
        }
        // plain struct field: alloca load — but the base chain may still
        // bottom out in a soa plane access (a[i].f)
        Expr* b = e->base;
        while (b && b->k == Expr::FIELD) {
          if (b->ptrDeref) {
            readThrough(fs, b, eff);
            if (viol) violate("reads memory through '" + rootName(b) + "'", b->loc);
          }
          b = b->base;
        }
        if (b && b->k == Expr::INDEX) {
          readThrough(fs, b, eff);
          if (viol) violate("reads memory through '" + rootName(b) + "'", b->loc);
        }
        walkExpr(e->base, eff);
        return;
      }
      case Expr::CAST:
        walkExpr(e->base, eff);
        return;
      case Expr::UN:
        walkExpr(e->rhs, eff);
        walkExpr(e->lhs, eff);
        return;
      case Expr::BIN:
        walkExpr(e->lhs, eff);
        walkExpr(e->rhs, eff);
        return;
      case Expr::ARRLIT:   // Phase 1b: array literal — element exprs walked
        for (auto* a : e->args) walkExpr(a, eff);
        return;
      case Expr::SLICE:     // Phase 1c: base[lo..hi] — reads the base
        readThrough(fs, e, eff);
        if (viol) violate("reads memory through '" + rootName(e) + "'", e->loc);
        checkSiteEffect(e, eff);   // the slice bounds check may trap
        walkExpr(e->base, eff);
        walkExpr(e->lhs, eff);
        walkExpr(e->rhs, eff);
        return;
      case Expr::TRYQ:      // Phase 2c: `expr?` — evaluates expr, may return
        walkExpr(e->base, eff);
        eff.mayNotReturn = true;   // early return on the error path
        return;
      case Expr::SLIT:
      case Expr::CALL:
        walkCall(e, eff);
        return;
    }
  }

  std::string rootName(Expr* e) {
    Expr* r = chainRoot(e, nullptr);
    return (r && r->k == Expr::IDENT) ? r->name : std::string("<expr>");
  }

  // walk an assignment TARGET for nested memory reads (e.g. the index
  // expression a[b[j]]), without recording the target itself as a load
  void walkLhs(Expr* e, Effects& eff) {
    if (!e) return;
    switch (e->k) {
      case Expr::INDEX: walkLhs(e->base, eff); walkExpr(e->rhs, eff); return;
      case Expr::FIELD: walkLhs(e->base, eff); return;
      case Expr::CAST:  walkLhs(e->base, eff); return;
      default:          return;   // ident / literal: address only, no load
    }
  }

  void walkCall(Expr* e, Effects& eff) {
    const std::string& n = e->name;
    // walk nested arg expressions first (indices, arithmetic, nested calls)
    for (auto* a : e->args) walkExpr(a, eff);

    FnDecl* callee = e->callee;
    if (callee && callees) {
      Effects c;
      auto found = callees->find(callee);
      if (found != callees->end()) {
        c = found->second;
      } else {
        // Imported C calls have no body summary in this translation unit.
        // Treat them as fully unknown; in particular, do not let an import
        // accidentally participate in a @pure / memory(none) optimisation.
        c.readsArg = true;
        c.readsOther = true;
        c.writesArg = true;
        c.writesOther = true;
        c.unknownCall = true;
        c.mayNotReturn = true;
      }
      // argument-root refinement: passing memory rooted in OUR locals or
      // globals to a callee that touches argument memory means the callee
      // may touch this function's own stack / heap / globals ("other"),
      // not just argmem — recording only the Arg flags would be unsound.
      // The Arg flags stay set as well (another argument may be genuine
      // param memory), which only ever over-claims effects.
      RootProbe p;
      for (auto* a : e->args) probeArgRoots(fs, a, p);
      if (p.anyLocal || p.anyGlobal) {
        if (c.readsArg)  c.readsOther = true;
        if (c.writesArg) c.writesOther = true;
      }
      // a data-stack-rooted argument (goose port v2): the callee's
      // argmem access on it touches THIS thread's per-thread stacks —
      // scratch, not observable "other" memory
      if (p.anyStack) {
        if (c.readsArg)  c.readsStack = true;
        if (c.writesArg) c.writesStack = true;
      }
      if (viol) {
        // @pure callers may only call functions that touch no memory
        // at all — reads violate the contract just like writes
        if (c.anyRead() || c.anyWrite() || c.io || c.allocates ||
            c.freesMem || c.unknownCall) {
          std::string what =
              (c.anyRead() && c.anyWrite()) ? "reads and writes memory" :
              c.anyRead() ? "reads memory" :
              c.anyWrite() ? "writes memory" :
              c.io ? "performs I/O" :
              c.allocates ? "allocates memory" :
              c.freesMem ? "frees memory" : "has unknown effects";
          violate("calls '" + callee->name + "', which " + what, e->loc);
        }
      }
      eff.merge(c);
      return;
    }

    // builtin (or unresolved) callee
    Effects be;
    builtinEffects(n, f ? f->strictfp : false, be);
    if (n == "memcpy" && e->args.size() >= 2) {
      // dst write, src read — classify through the argument roots
      RootProbe dummy;
      (void)dummy;
      writeThroughArg(e->args[0], eff);
      readThroughArg(e->args[1], eff);
      if (viol) {
        violate("copies memory through '" + rootName(e->args[0]) + "'", e->loc);
      }
      return;
    }
    if (n == "memset" && e->args.size() >= 1) {
      writeThroughArg(e->args[0], eff);
      if (viol) violate("writes memory through '" + rootName(e->args[0]) + "'", e->loc);
      return;
    }
    if ((n == "soa_len" || n == "soa_field_ptr" ||
         n == "freshet.prefetch_r" || n == "freshet.prefetch_w") &&
        !e->args.empty()) {
      readThroughArg(e->args[0], eff);
      if (n == "freshet.prefetch_w") writeThroughArg(e->args[0], eff);
      if (viol) violate("reads memory through '" + rootName(e->args[0]) + "'", e->loc);
      return;
    }
    // fsht-02:01: Vec accessors — reads (and, for mutators, writes)
    // classify through the container argument's root; the container arg
    // is `&v` for the &mut forms and a plain Vec value for readers.  The
    // builtin's own flags (allocates/freesMem from builtinEffects) merge
    // here too — this branch returns early.
    if (n.rfind("vec_", 0) == 0 && !e->args.empty() &&
        n != "vec_new") {
      eff.merge(be);
      Expr* a0 = e->args[0];
      if (a0->k == Expr::UN && a0->op == "&") a0 = a0->rhs;
      bool mutator = (n == "vec_push" || n == "vec_insert" ||
                      n == "vec_resize" || n == "vec_reserve" ||
                      n == "vec_pop" || n == "vec_remove" ||
                      n == "vec_truncate" || n == "vec_clear" ||
                      n == "vec_free");
      readThroughArg(a0, eff);
      if (mutator) writeThroughArg(a0, eff);
      if (viol) {
        if (be.allocates)
          violate("allocates memory ('" + n + "')", e->loc);
        else if (be.freesMem)
          violate("frees memory ('" + n + "')", e->loc);
        else
          violate(std::string(mutator ? "mutates" : "reads") +
                  " a container through '" + rootName(a0) + "'", e->loc);
      }
      return;
    }
    if (viol) {
      if (be.io) violate("performs I/O ('" + n + "')", e->loc);
      else if (be.allocates) violate("allocates memory ('" + n + "')", e->loc);
      else if (be.freesMem) violate("frees memory ('" + n + "')", e->loc);
      else if (be.anyRead() || be.anyWrite())
        violate("calls '" + n + "' (touches memory)", e->loc);
      else if (be.unknownCall) violate("calls unknown function '" + n + "'", e->loc);
    }
    eff.merge(be);
  }

  // read/write through a call argument that denotes memory
  void readThroughArg(Expr* a, Effects& eff) {
    Expr* r = a;
    if (r->k == Expr::UN && r->op == "&") r = r->rhs;
    if (r->k == Expr::INDEX || r->k == Expr::FIELD) { readThrough(fs, r, eff); return; }
    if (r->k == Expr::IDENT) { readOfRoot(fs, r, eff); return; }
    eff.readsArg = eff.readsOther = true;   // opaque expression: conservative
  }
  void writeThroughArg(Expr* a, Effects& eff) {
    Expr* r = a;
    if (r->k == Expr::UN && r->op == "&") r = r->rhs;
    if (r->k == Expr::INDEX || r->k == Expr::FIELD) { writeOfRoot(fs, chainRoot(r, nullptr), eff); return; }
    if (r->k == Expr::IDENT) { writeOfRoot(fs, r, eff); return; }
    eff.writesArg = eff.writesOther = true;
  }

  // ---- statements -------------------------------------------------------
  void walkStmt(Stmt* s, Effects& eff) {
    if (!s) return;
    switch (s->k) {
      case Stmt::LET:
        walkExpr(s->init, eff);
        return;
      case Stmt::ASSIGN: {
        // classify the write target
        bool mem = false;
        Expr* root = chainRoot(s->lhs, &mem);
        RootKind rk = classifyRoot(fs, root);
        if (mem || rk != RK_SCALAR_LOCAL) {
          writeOfRoot(fs, root, eff);
          if (viol) violate("writes memory through '" + rootName(s->lhs) + "'",
                           s->loc);
        }
        // walk the lvalue for nested reads (index expressions like
        // a[b[j]]), but do NOT record the store target itself as a load
        walkLhs(s->lhs, eff);
        walkExpr(s->rhs, eff);
        return;
      }
      case Stmt::RETURN:
        walkExpr(s->retVal, eff);
        return;
      case Stmt::DEFER:
        walkExpr(s->deferExpr, eff);
        return;
      case Stmt::EXPR:
        walkExpr(s->expr, eff);
        return;
      case Stmt::IF:
        walkExpr(s->cond, eff);
        walkStmt(s->thenB, eff);
        walkStmt(s->elseB, eff);
        return;
      case Stmt::MATCH: {   // Phase 1d: scrutinee read + arm bodies
        walkExpr(s->matchOn, eff);
        for (auto& arm : s->matchArms) walkStmt(arm.body, eff);
        return;
      }
      case Stmt::BREAK: case Stmt::CONTINUE:   // Phase 1a: no expressions
        return;
      case Stmt::WHILE:
        // arbitrary condition — cannot prove termination
        eff.mayNotReturn = true;
        walkExpr(s->cond, eff);
        walkStmt(s->body, eff);
        return;
      case Stmt::FOR:
        if (!s->forRange) eff.mayNotReturn = true;  // arbitrary condition
        // range-for terminates: i starts at lo, steps +1, stops at hi
        if (s->forRange) {
          walkExpr(s->rangeLo, eff);
          walkExpr(s->rangeHi, eff);
        } else {
          walkExpr(s->forInit, eff);
          walkExpr(s->forCond, eff);
          walkExpr(s->forStep, eff);
        }
        walkStmt(s->body, eff);
        return;
      case Stmt::PARFOR:
        // dispatches through the runtime: per-chunk context allocation,
        // queue/signalling, possibly a non-terminating body.  Model the
        // caller conservatively.
        eff.allocates = true;
        eff.readsOther = eff.writesOther = true;
        eff.mayNotReturn = true;
        walkExpr(s->plo, eff);
        walkExpr(s->phi, eff);
        for (auto& c : s->clauses) walkExpr(c.e, eff);
        // body effects execute inside the generated chunk function; the
        // caller itself only builds the context template.  Still walk it
        // so @pure callers get precise violation messages.
        {
          Effects bodyEff;
          walkStmt(s->body, bodyEff);
          if (viol && !bodyEff.semanticallyPure())
            violate("parfor body has side effects", s->loc);
        }
        return;
      case Stmt::BLOCK:
        for (auto* st : s->stmts) walkStmt(st, eff);
        return;
      case Stmt::EMPTY:
        return;
    }
  }
};

}  // namespace

void computeEffects(Program* p) {
  if (!p) return;
  std::vector<FnDecl*> fns;
  for (auto* f : p->fns)
    if (f->body && !f->isGeneric) fns.push_back(f);

  // ---- fixed point over the call lattice --------------------------------
  std::map<FnDecl*, Effects> cur;
  for (auto* f : fns) cur[f] = Effects{};

  bool changed = true;
  while (changed) {
    changed = false;
    for (auto* f : fns) {
      FnSema* fs = semaForFn(f);
      g_stackRooted.clear();
      collectStackRooted(f->body);
      EffectWalker w{f, fs, &cur, nullptr};
      Effects e;
      w.walkStmt(f->body, e);
      // fsht-02:01: a function with container bindings or container
      // parameters emits hidden scope-exit drop calls (irgen injects
      // them after this analysis) — the summary must never claim
      // memory(none) for such a function.  freesMem is the conservative
      // marker: it keeps the fn out of readnone groups without claiming
      // observable writes.
      {
        bool hasContainer = false;
        for (auto& sym : fs->allSyms)
          if (sym.k == Sym::PARAM || sym.k == Sym::VAR)
            if (isContainerType(sym.ty)) { hasContainer = true; break; }
        if (hasContainer) e.freesMem = true;
      }
      // a user @pure promise does NOT relax the summary — it is validated
      // separately below; if it holds, the inferred summary is already
      // clean, and if it does not hold we reject the program.
      Effects old = cur[f];
      if (memcmp(&old, &e, sizeof(Effects)) != 0) {
        cur[f] = e;
        changed = true;
      }
    }
  }

  // ---- call-cycle termination refinement --------------------------------
  // A function on a call cycle (self- or mutual recursion) may recurse
  // forever; the lattice above cannot see cycles, so mark them here.
  {
    std::map<FnDecl*, std::vector<FnDecl*>> edges;
    std::function<void(FnDecl*, Expr*)> collect = [&](FnDecl* from, Expr* e) {
      if (!e) return;
      if (e->k == Expr::CALL && e->callee && e->callee->body)
        edges[from].push_back(e->callee);
      for (auto* a : e->args) collect(from, a);
      collect(from, e->lhs);
      collect(from, e->rhs);
      collect(from, e->base);
    };
    std::function<void(FnDecl*, Stmt*)> collectS = [&](FnDecl* from, Stmt* s) {
      if (!s) return;
      switch (s->k) {
        case Stmt::LET: collect(from, s->init); break;
        case Stmt::ASSIGN: collect(from, s->lhs); collect(from, s->rhs); break;
        case Stmt::RETURN: collect(from, s->retVal); break;
        case Stmt::DEFER: collect(from, s->deferExpr); break;
        case Stmt::MATCH:   // Phase 1d
          collect(from, s->matchOn);
          for (auto& arm : s->matchArms) collectS(from, arm.body);
          break;
        case Stmt::BREAK: case Stmt::CONTINUE: break;   // Phase 1a
        case Stmt::EXPR: collect(from, s->expr); break;
        case Stmt::IF: collect(from, s->cond); collectS(from, s->thenB);
                       collectS(from, s->elseB); break;
        case Stmt::WHILE: collect(from, s->cond); collectS(from, s->body); break;
        case Stmt::FOR:
          if (s->forRange) { collect(from, s->rangeLo); collect(from, s->rangeHi); }
          else { collect(from, s->forInit); collect(from, s->forCond); collect(from, s->forStep); }
          collectS(from, s->body);
          break;
        case Stmt::PARFOR: collect(from, s->plo); collect(from, s->phi);
                           for (auto& c : s->clauses) collect(from, c.e);
                           collectS(from, s->body);
                           break;
        case Stmt::BLOCK: for (auto* st : s->stmts) collectS(from, st); break;
        default: break;
      }
    };
    for (auto* f : fns) collectS(f, f->body);

    for (auto* f : fns) {
      // DFS from f; if f is reachable via >= 1 edge, f sits on a cycle
      std::set<FnDecl*> visited;
      std::vector<FnDecl*> stack{f};
      while (!stack.empty()) {
        FnDecl* x = stack.back();
        stack.pop_back();
        for (FnDecl* next : edges[x]) {
          if (next == f) { cur[f].mayNotReturn = true; goto done; }
          if (!visited.count(next)) {
            visited.insert(next);
            stack.push_back(next);
          }
        }
      }
    done:;
    }
  }

  // ---- publish summaries + derive isPure --------------------------------
  for (auto* f : fns) {
    f->eff = cur[f];
    f->isPure = f->eff.semanticallyPure();
    f->computedPure = true;
  }

  // ---- @pure contract validation (spec §3.1) ---------------------------
  // "@pure: function reads no memory and writes no memory" — violations
  // are compile errors, as documented.  (Historical behaviour let @pure
  // bypass the analysis entirely, which turned an unvalidated user
  // promise into a memory(none) attribute — exactly the audit's §2.1
  // unsoundness.)
  for (auto* f : fns) {
    if (!f->userPure) continue;
    FnSema* fs = semaForFn(f);
    g_stackRooted.clear();
    collectStackRooted(f->body);
    std::vector<PureViolation> viol;
    EffectWalker w{f, fs, &cur, &viol};
    Effects e;   // recomputed only to drive the walker
    w.walkStmt(f->body, e);
    // callee-based violations came through `viol` during the walk;
    // effects themselves must be all-clear for the contract to hold:
    const Effects& eff = cur[f];
    bool clean = !eff.anyRead() && !eff.anyWrite() && !eff.io &&
                 !eff.allocates && !eff.freesMem && !eff.unknownCall;
    if (clean && viol.empty()) continue;
    std::string head = "@pure fn '" + f->name + "' violates its contract: " +
                       (clean ? std::string("body has effects the analysis could not localise")
                              : viol.empty() ? std::string("reads or writes memory")
                                             : viol.front().msg);
    diag(Diag::Error, Code::Pure, head, f->loc);
    for (size_t i = viol.empty() ? 0 : 1; i < viol.size() && i < 4; ++i)
      diag(Diag::Note, Code::Pure, viol[i].msg, viol[i].loc);
  }
}

}  // namespace freshet
