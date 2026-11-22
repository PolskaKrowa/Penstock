// Freshet ownership analysis — per-function abstract state machine over
// pointer vars: Alive / MaybeFreed / Freed / Moved.
//   (all reported as F0301)
//   - owned pointer reaches scope end without free (leak)
//   - second free of the same pointer (double free)
//   - use after free / use after give()
//   - give() of a non-owned pointer
//
// fsht-03:02 (registry 03.02.09/03.02.10): the analysis now tracks the
// ownership-tracked String (moves, pre-emptive string_free — the
// ch. 24 §4 migration) and the Reader/Writer handles (use-after-close),
// and a second pass below (runBorrowAnalysis) enforces the borrow
// discipline: the F0310 exclusivity table (one writer or many readers,
// growth/release/move vs live views) and the F0311 escape rules
// (returning &mut local or a view of local storage; views of by-value
// container parameters).
#include "common.h"
#include <algorithm>
#include <climits>
#include <functional>

namespace freshet {

enum OwnState { OS_ALIVE, OS_MAYBE, OS_FREED, OS_MOVED, OS_NONE };

struct OwnInfo {
  std::string var;
  OwnState st = OS_NONE;
  bool owned = false;
  // fsht-02:01 (registry 02.09.02): the binding is an owned Vec<T> —
  // tracked for moves/invalidation like an owned pointer, but never for
  // scope leaks (irgen emits the scope-exit drop; the runtime keeps
  // drops idempotent through the nulled data pointer)
  bool container = false;
  // fsht-03:02: any owning binding (String per ch. 24 §4, Reader/Writer
  // handles per ch. 23 §4, owning user shapes per ch. 24 §5) — same
  // move/release tracking, never a scope leak (drops are automatic)
  bool owningShape = false;
  SrcLoc allocLoc, freeLoc;
  bool isParam = false;        // storage root is a parameter
  bool paramBorrowed = false;  // ... a `borrowed` parameter (caller owns)
};

// fsht-02:01 (+03:02): constructors producing a fresh owned value
static bool containerCtorCall(const std::string& n) {
  return n == "vec_new" || n == "vec_from_span" ||
         n == "vec_from_array" || n == "vec_clone" ||
         n == "string_new" || n == "string_from" || n == "string_clone" ||
         n == "str_cat" || n == "fmt_i64" || n == "fmt_f64";
}

// fsht-02:01 (+03:02): calls that release an owned binding's storage —
// pre-emptive drops that invalidate the binding (ch. 24 §4/§5, ch. 23 §4)
static bool containerReleaseCall(const std::string& n) {
  return n == "vec_free" || n == "string_free" ||
         n == "reader_close" || n == "writer_close";
}

// module-qualified calls keep the FULL source spelling in Expr::name
// ("io::reader_close") while the matchers above work on the bare name —
// normalise through qualModule (ch. 16 module paths) so a qualified
// io::reader_close(&mut r) invalidates r exactly like the bare builtin
static std::string bareCallName(const Expr* e) {
  if (!e) return "";
  if (!e->qualModule.empty() && e->name.size() > e->qualModule.size() + 2)
    return e->name.substr(e->qualModule.size() + 2);
  return e->name;
}

struct OwnCtx {
  std::map<std::string, OwnInfo> vars;
  std::vector<std::string> errors;
};

static const char* osName(OwnState s) {
  switch (s) {
    case OS_ALIVE: return "alive";
    case OS_MAYBE: return "maybe-freed";
    case OS_FREED: return "freed";
    case OS_MOVED: return "moved";
    default: return "n/a";
  }
}

// check an expression's identifier uses against state
static void checkExprUses(OwnCtx& ctx, Expr* e, SrcLoc at) {
  if (!e) return;
  // walk
  if (e->k == Expr::IDENT) {
    auto it = ctx.vars.find(e->name);
    if (it != ctx.vars.end()) {
      if (it->second.st == OS_FREED) {
        diag(Diag::Error, Code::Own,
             "use-after-free: '" + e->name + "' was freed at line " +
             std::to_string(it->second.freeLoc.line),
             at, "var=" + e->name + ",kind=uaf");
      } else if (it->second.st == OS_MOVED) {
        diag(Diag::Error, Code::Own,
             "use-after-move: '" + e->name + "' ownership was given away at line " +
             std::to_string(it->second.freeLoc.line),
             at, "var=" + e->name + ",kind=move");
      } else if (it->second.st == OS_MAYBE) {
        diag(Diag::Error, Code::Own,
             "possibly-freed: '" + e->name + "' may have been freed on a "
             "branch (strict mode requires provably-single flow)",
             at, "var=" + e->name + ",kind=maybe");
      }
    }
    return;
  }
  for (auto* a : e->args) checkExprUses(ctx, a, at);
  checkExprUses(ctx, e->lhs, at);
  checkExprUses(ctx, e->rhs, at);
  checkExprUses(ctx, e->base, at);
}

// apply free/move to a variable
static void applyFree(OwnCtx& ctx, const std::string& v, SrcLoc at,
                      bool isGive) {
  auto it = ctx.vars.find(v);
  if (it == ctx.vars.end()) return;
  OwnInfo& oi = it->second;
  const char* verb = isGive ? "give()" : "free()";
  if (oi.st == OS_FREED || oi.st == OS_MOVED) {
    diag(Diag::Error, Code::Own,
         std::string("double ") + (isGive ? "give" : "free") + ": '" + v +
         "' already " + osName(oi.st) + " at line " +
         std::to_string(oi.freeLoc.line),
         at,
         std::string("var=") + v + ",kind=double" + (isGive ? "move" : "free"));
    return;
  }
  if (oi.st == OS_MAYBE) {
    diag(Diag::Error, Code::Own,
         std::string(verb) + " of possibly-freed '" + v + "'",
         at, "var=" + v + ",kind=double");
    return;
  }
  if (!isGive && !oi.owned) {
    diag(Diag::Warning, Code::OwnSuspicious,
         "free() of borrowed pointer '" + v +
         "' — only the owner may free (received via give?)",
         at);
  }
  oi.st = isGive ? OS_MOVED : OS_FREED;
  oi.freeLoc = at;
}

static std::string rootVarName(Expr* e) {
  while (e && (e->k == Expr::INDEX || e->k == Expr::FIELD)) e = e->base;
  // fsht-02:01: container mutators take &v — unwrap the address-of
  if (e && e->k == Expr::UN && e->op == "&") e = e->rhs;
  return (e && e->k == Expr::IDENT) ? e->name : "";
}

// deferred frees collected during a block, applied at scope exit in
// reverse order.
struct Deferred {
  Expr* expr;
  SrcLoc loc;
};

static void ownStmt(OwnCtx& ctx, FnSema* fs, Stmt* s,
                    std::vector<Deferred>& defers);

static OwnCtx joinStates(const OwnCtx& a, const OwnCtx& b, SrcLoc at) {
  // merge after if/else: differing states -> OS_MAYBE
  OwnCtx out = a;
  for (auto& [v, oi] : out.vars) {
    auto it = b.vars.find(v);
    if (it == b.vars.end()) continue;
    const OwnInfo& other = it->second;
    if (oi.st != other.st) {
      if (oi.st == OS_FREED || other.st == OS_FREED ||
          oi.st == OS_MOVED || other.st == OS_MOVED)
        oi.st = OS_MAYBE;
      // else keep alive
    }
  }
  (void)at;
  return out;
}

static void ownBlock(OwnCtx& ctx, FnSema* fs, Stmt* b,
                     std::vector<Deferred>& defers, bool checkLeaks) {
  std::map<std::string, OwnInfo> entry = ctx.vars;
  // Only the defers registered INSIDE this block fire at its exit.  The
  // historical loop ran the WHOLE accumulated vector — including the
  // enclosing scopes' entries — and never removed them, so a top-level
  // `defer free(c)` fired once for every later sibling nested block and
  // AGAIN at the function end: a false F0301 double free.  Snapshot the
  // base, process only the new slice in reverse, then drop it (those
  // defers fired; they must not re-run at fn end).
  size_t base = defers.size();
  for (auto* st : b->stmts) ownStmt(ctx, fs, st, defers);
  // run defers in reverse (simulate: apply frees)
  for (size_t i = defers.size(); i > base; --i) {
    Deferred& de = defers[i - 1];
    Expr* dxe = de.expr;
    if (dxe->k == Expr::CALL && (bareCallName(dxe) == "free" || bareCallName(dxe) == "soa_free" ||
                                 bareCallName(dxe) == "vec_free")) {
      std::string v = rootVarName(dxe->args.empty() ? nullptr : dxe->args[0]);
      if (!v.empty()) {
        auto f = ctx.vars.find(v);
        if (f != ctx.vars.end() && f->second.st == OS_FREED) {
          diag(Diag::Error, Code::Own,
               "double free: '" + v + "' freed manually and again by defer",
               de.loc,
               "var=" + v + ",kind=double");
        } else if (f != ctx.vars.end() && f->second.st == OS_ALIVE) {
          f->second.st = OS_FREED;
          f->second.freeLoc = de.loc;
        }
      }
    }
  }
  defers.resize(base);   // the block's defers fired; outer ones remain
  if (checkLeaks) {
    for (auto& [v, oi] : ctx.vars) {
      if (!oi.owned) continue;
      // fsht-02:01/03:02: containers, Strings, handles and owning
      // shapes never leak their scope — the codegen emits the
      // scope-exit drop (the analysis tracks moves/frees only)
      if (oi.container || oi.owningShape) continue;
      auto before = entry.find(v);
      bool preExisting = before != entry.end();
      if (oi.st == OS_ALIVE && !preExisting) {
        diag(Diag::Error, Code::Own,
             "leak: owned '" + v + "' (malloc at line " +
             std::to_string(oi.allocLoc.line) +
             ") leaves scope without free/defer/give",
             b->stmts.empty() ? b->loc.file : b->stmts.back()->loc.file,
             b->loc.line, b->loc.col,
             "var=" + v + ",kind=leak");
      }
    }
  }
}

static void ownStmt(OwnCtx& ctx, FnSema* fs, Stmt* s,
                    std::vector<Deferred>& defers) {
  if (!s) return;
  switch (s->k) {
    case Stmt::LET: {
      checkExprUses(ctx, s->init, s->loc);
      OwnInfo oi;
      oi.var = s->var;
      oi.st = OS_ALIVE;
      // fsht-02:01: the binding's TYPE decides container tracking (the
      // ann type is resolved by sema before this pass)
      Type* letTy = s->annTy;
      // fsht-03:02: containers (Vec or String) and owning shapes/handles
      // all track moves and pre-emptive releases
      oi.container = letTy && isContainerType(letTy);
      oi.owningShape = letTy && (typeNeedsDrop(letTy) || isHandleType(letTy));
      bool owning = oi.container || oi.owningShape;
      oi.owned = (s->init->k == Expr::CALL && (bareCallName(s->init) == "malloc" ||
                                               bareCallName(s->init) == "soa_new")) ||
                 (owning && s->init->k == Expr::CALL &&
                  containerCtorCall(bareCallName(s->init))) ||
                 (owning && s->init->k == Expr::IDENT);
      oi.allocLoc = s->loc;
      // fsht-02:01/03:02 move-out invalidation: `let b = a;` with an
      // owning `a` MOVES the whole shape — mark the source binding moved
      // (uses of it after this line are F0301 use-after-move)
      if (owning && s->init->k == Expr::IDENT) {
        auto it = ctx.vars.find(s->init->name);
        if (it != ctx.vars.end() && it->second.container) {
          if (it->second.st == OS_ALIVE)
            applyFree(ctx, s->init->name, s->loc, true);
        }
      }
      ctx.vars[s->var] = oi;
      // let x = give-expr? not valid; give only in call args
      break;
    }
    case Stmt::ASSIGN: {
      checkExprUses(ctx, s->lhs, s->loc);
      checkExprUses(ctx, s->rhs, s->loc);
      std::string lv = rootVarName(s->lhs);
      if (s->lhs->k == Expr::IDENT && !lv.empty()) {
        // fsht-02:01: rebinding a container that still owns its buffer is
        // an error — the old allocation would leak (drops only see the
        // current slot contents); free it or move it out first
        {
          auto it = ctx.vars.find(lv);
          if (it != ctx.vars.end() && it->second.container &&
              it->second.st == OS_ALIVE && it->second.owned) {
            diag(Diag::Error, Code::Own,
                 "rebinding container '" + lv + "' while it still owns its "
                 "allocation — free it (vec_free) or move it first",
                 s->loc,
                 "var=" + lv + ",kind=rebind-container");
          }
        }
        // rebinding an owned var: old value must be freed first (or moved)
        auto it = ctx.vars.find(lv);
        if (it != ctx.vars.end() && it->second.owned &&
            (s->rhs->k != Expr::CALL ||
             (s->rhs->name != "malloc" && s->rhs->name != "soa_new"))) {
          diag(Diag::Warning, Code::OwnSuspicious,
               "rebinding owned '" + lv + "' without freeing the old value",
               s->loc);
        }
        OwnInfo oi;
        oi.var = lv;
        oi.st = OS_ALIVE;
        Type* asTy = s->lhs->ty;
        oi.container = asTy && isContainerType(asTy);
        oi.owningShape = asTy && (typeNeedsDrop(asTy) || isHandleType(asTy));
        bool owning = oi.container || oi.owningShape;
        oi.owned = (s->rhs->k == Expr::CALL && (bareCallName(s->rhs) == "malloc" ||
                                                bareCallName(s->rhs) == "soa_new")) ||
                   (owning && s->rhs->k == Expr::CALL &&
                    containerCtorCall(bareCallName(s->rhs))) ||
                   (owning && s->rhs->k == Expr::IDENT);
        oi.allocLoc = s->loc;
        // move-out invalidation for `b = a` owning sources
        if (owning && s->rhs->k == Expr::IDENT) {
          auto src = ctx.vars.find(s->rhs->name);
          if (src != ctx.vars.end() && src->second.container) {
            if (src->second.st == OS_ALIVE)
              applyFree(ctx, s->rhs->name, s->loc, true);
          }
        }
        ctx.vars[lv] = oi;
      } else if (s->lhs->k == Expr::INDEX || s->lhs->k == Expr::FIELD) {
        // writing through pointer is fine (borrow in place)
      }
      break;
    }
    case Stmt::RETURN: {
      checkExprUses(ctx, s->retVal, s->loc);
      // returning owned ptr transfers ownership out — no leak check at exit
      std::string rv = rootVarName(s->retVal);
      if (!rv.empty()) {
        auto it = ctx.vars.find(rv);
        if (it != ctx.vars.end() && it->second.owned)
          it->second.st = OS_MOVED;   // handed to caller
      }
      break;
    }
    case Stmt::DEFER: {
      checkExprUses(ctx, s->deferExpr, s->loc);
      defers.push_back({s->deferExpr, s->loc});
      break;
    }
    case Stmt::IF: {
      checkExprUses(ctx, s->cond, s->loc);
      OwnCtx a = ctx, b = ctx;
      std::vector<Deferred> da, db;
      ownBlock(a, fs, s->thenB, da, true);
      if (s->elseB) {
        if (s->elseB->k == Stmt::IF) {
          std::vector<Deferred> db2;
          ownStmt(b, fs, s->elseB, db2);
          db = db2;
        } else {
          ownBlock(b, fs, s->elseB, db, true);
        }
      }
      OwnCtx merged = joinStates(a, b, s->loc);
      ctx.vars = merged.vars;
      // defers: conservative — append both (dupes produce maybe-state)
      for (auto& d : da) defers.push_back(d);
      for (auto& d : db) defers.push_back(d);
      break;
    }
    case Stmt::WHILE: {
      checkExprUses(ctx, s->cond, s->loc);
      // one pass with entry state; frees inside loop -> maybe-freed after
      OwnCtx in = ctx;
      std::vector<Deferred> d2;
      ownBlock(in, fs, s->body, d2, false);
      OwnCtx m = joinStates(ctx, in, s->loc);
      ctx.vars = m.vars;
      break;
    }
    case Stmt::FOR: {
      if (s->forRange) {
        checkExprUses(ctx, s->rangeLo, s->loc);
        checkExprUses(ctx, s->rangeHi, s->loc);
      } else {
        checkExprUses(ctx, s->forInit, s->loc);
        checkExprUses(ctx, s->forCond, s->loc);
        checkExprUses(ctx, s->forStep, s->loc);
      }
      OwnCtx in = ctx;
      std::vector<Deferred> d2;
      ownBlock(in, fs, s->body, d2, false);
      OwnCtx m = joinStates(ctx, in, s->loc);
      ctx.vars = m.vars;
      break;
    }
    case Stmt::PARFOR: {
      checkExprUses(ctx, s->plo, s->loc);
      checkExprUses(ctx, s->phi, s->loc);
      // parfor body: same flow rules; owned vars inside body are per-iteration
      std::vector<Deferred> d2;
      ownBlock(ctx, fs, s->body, d2, true);
      // free/move of captured outer vars inside parfor → error (thread-safety
      // + ownership): deferred frees from parfor body dropped with error
      for (auto& d : d2) {
        diag(Diag::Error, Code::Thread,
             "defer inside parfor body — defers run per-iteration; move the "
             "allocation out of the loop",
             d.loc);
      }
      break;
    }
    case Stmt::EXPR: {
      checkExprUses(ctx, s->expr, s->loc);
      if (s->expr->k == Expr::CALL) {
        std::string bcn = bareCallName(s->expr);
        if (bcn == "free" || bcn == "soa_free" ||
            containerReleaseCall(bcn)) {
          std::string v = rootVarName(s->expr->args.empty() ? nullptr : s->expr->args[0]);
          if (!v.empty()) applyFree(ctx, v, s->loc, false);
        }
        // give() marks moved
        if (bcn == "give") {
          std::string v = rootVarName(s->expr->args.empty() ? nullptr : s->expr->args[0]);
          if (!v.empty()) applyFree(ctx, v, s->loc, true);
        }
        // user fn call with give args: mark moved
        FnDecl* callee = s->expr->callee;
        if (callee) {
          for (size_t i = 0; i < s->expr->args.size(); ++i) {
            // fsht-02:01: a by-value Vec<T> parameter is a move whether or
            // not the call site wraps it in give() — irgen nulls the
            // source binding after the call
            Type* pt = i < callee->params.size() ? callee->params[i].ty
                                                  : nullptr;
            bool containerArg = pt && pt->k == Type::STRUCT &&
                                (containerElemType(pt) || isHandleType(pt) ||
                                 typeNeedsDrop(pt));
            if (!s->expr->argGive[i] && !containerArg) continue;
            // fsht-03:02: borrowed / borrowed-mut parameters receive no
            // ownership — the caller keeps it
            if (callee->params[i].borrowed || callee->params[i].borrowedMut) continue;
            std::string v = rootVarName(s->expr->args[i]);
            if (v.empty()) continue;
            auto it = ctx.vars.find(v);
            if (it == ctx.vars.end()) continue;
            if (!it->second.owned && !it->second.container) {
              if (s->expr->argGive[i])
                diag(Diag::Error, Code::Own,
                     "give() of non-owned pointer '" + v + "'",
                     s->loc,
                     "var=" + v + ",kind=move-nonowned");
              continue;
            }
            applyFree(ctx, v, s->loc, true);
          }
        }
      }
      break;
    }
    case Stmt::BLOCK: {
      ownBlock(ctx, fs, s, defers, true);
      break;
    }
    // Phase 1d (report §4.3): every arm is a possible path — check each
    // against the ENTRY state and join, exactly like if/else.
    case Stmt::MATCH: {
      checkExprUses(ctx, s->matchOn, s->loc);
      OwnCtx joined = ctx;
      bool first = true;
      for (auto& arm : s->matchArms) {
        OwnCtx in = ctx;
        std::vector<Deferred> d2;
        ownBlock(in, fs, arm.body, d2, false);
        if (first) { joined = in; first = false; }
        else {
          OwnCtx m = joinStates(joined, in, s->loc);
          joined = m;
        }
      }
      ctx.vars = joined.vars;
      break;
    }
    // Phase 1a: leaf statements — nothing to track
    case Stmt::BREAK:
    case Stmt::CONTINUE:
      break;
    default: break;
  }
}

void runOwnershipAnalysis(Program* p) {
  (void)p;
  for (auto* fs : allFnSemas()) {
    // params that receive ownership: those with give at ALL call sites are
    // tracked conservatively: a param is owned if any caller gives it.
    // (checked at call sites; here we track param as borrowed so frees of
    // params produce F0302.)
    OwnCtx ctx;
    // fsht-03:02: seed the state machine AND the provenance table the
    // borrow analysis reads (params: borrowed = caller-owned storage —
    // returning views of them is legal; by-value/owned container params
    // drop at callee exit, so their views must not escape)
    for (auto& pm : fs->fn->params) {
      OwnInfo oi;
      oi.var = pm.name;
      oi.st = OS_ALIVE;
      oi.owned = false;
      oi.isParam = true;
      oi.paramBorrowed = pm.borrowed;
      Type* pt = pm.ty;
      oi.container = pt && isContainerType(pt);
      oi.owningShape = pt && (typeNeedsDrop(pt) || isHandleType(pt));
      ctx.vars[pm.name] = oi;
    }
    std::vector<Deferred> defers;
    // top-level body: leak check at function end
    for (auto* st : fs->fn->body->stmts) ownStmt(ctx, fs, st, defers);
    // function-end defers
    for (auto it = defers.rbegin(); it != defers.rend(); ++it) {
      Expr* de = it->expr;
      if (de->k == Expr::CALL && (bareCallName(de) == "free" || bareCallName(de) == "soa_free" ||
                                  bareCallName(de) == "vec_free")) {
        std::string v = rootVarName(de->args.empty() ? nullptr : de->args[0]);
        if (!v.empty()) {
          auto f = ctx.vars.find(v);
          if (f != ctx.vars.end() && f->second.st == OS_FREED) {
            diag(Diag::Error, Code::Own,
                 "double free: '" + v + "' freed manually and again by defer",
                 it->loc,
                 "var=" + v + ",kind=double");
          } else if (f != ctx.vars.end() && f->second.st == OS_ALIVE) {
            f->second.st = OS_FREED;
          }
        }
      }
    }
    // leaks: owned & alive at fn end (unless returned)
    for (auto& [v, oi] : ctx.vars) {
      // fsht-02:01/03:02: owning values drop at every fn exit
      if (oi.container || oi.owningShape) continue;
      if (oi.owned && oi.st == OS_ALIVE) {
        diag(Diag::Error, Code::Own,
             "leak: owned '" + v + "' (malloc at line " +
             std::to_string(oi.allocLoc.line) +
             ") escapes 'fn " + fs->fn->name + "' without free/give/return",
             fs->fn->loc.file, fs->fn->loc.line, 0,
             "var=" + v + ",kind=leak");
      }
    }
  }
}

// ============================================================ fsht-03:02
// registry 03.02.09/03.02.10 (ch. 24 §2/§3): the borrow discipline.
//
// A second per-function walk with the F0310/F0311 diagnostics:
//   * F0310 exclusivity — one writer or many readers:
//       - two live mutable borrows of the same place (f(&mut x, &mut x),
//         two `borrowed mut` aliases of one argument)
//       - a mutable borrow created while a live shared view of the place
//         exists, or a shared view used while a mutable borrow is live
//       - growth-mutators / releases / moves conflicting with a live
//         view (reallocation or transfer would dangle it); in-place
//         element writes do NOT invalidate views (ch. 24 §2)
//   * F0311 escape — a borrow/view must not outlive its storage:
//       - returning &mut local or a view of a local container/String
//       - returning a view of a BY-VALUE container parameter (the
//         callee-exit drop dangles it); views of `borrowed` parameters
//         stay legal (the caller owns them)
//       - storing a borrow of function-local storage into a global
//
// Lifetimes are lexical: a binding-held view/borrow lives from creation
// to its LAST USE (a per-block ordinal scan); an rvalue borrow passed
// to a call is live exactly that call.  One mutable borrow per storage
// root at a time (ch. 24 §8 defers disjoint-path proofs).
namespace {

struct ViewInfo {
  std::string root;      // the storage root's binding name
  bool mut;              // &mut binding vs shared view
  int expiry = 0;        // last-use ordinal in the declaring block
  SrcLoc loc;
};

struct BorrowWalk {
  FnDecl* fn = nullptr;
  FnSema* fs = nullptr;
  // the expression subtree currently being recorded as a BINDING (the
  // borrow/view creation of `let r = &mut x;`): its rvalue events were
  // consumed by the binding record itself, so checkExpr skips them
  Expr* suppressSubtree = nullptr;
  // binding-name -> view record (views & mut-borrow bindings alike)
  std::map<std::string, ViewInfo> views;
  // storage-root name -> the live rvalue mutable borrow of this stmt
  std::map<std::string, SrcLoc> rvalueMut;

  bool rootIsLocalContainer(const std::string& name) {
    auto it = views.find(name);
    (void)it;
    // params of borrowed kind belong to the caller
    for (auto& pm : fn->params)
      if (pm.name == name) return !pm.borrowed;
    // locals are function-owned storage
    return true;
  }

  static std::string baseRoot(Expr* e) {
    while (e && (e->k == Expr::INDEX || e->k == Expr::FIELD)) e = e->base;
    if (e && e->k == Expr::UN && e->op == "&") e = e->rhs;
    while (e && (e->k == Expr::INDEX || e->k == Expr::FIELD)) e = e->base;
    return (e && e->k == Expr::IDENT) ? e->name : "";
  }

  // is `e` a view creation rooted at a place?
  static Expr* viewRootExpr(Expr* e) {
    if (!e) return nullptr;
    if (e->k == Expr::SLICE) {
      Expr* b = e->base;
      if (b && b->k == Expr::UN && b->op == "&") b = b->rhs;
      return b;
    }
    if (e->k == Expr::CALL) {
      const std::string& n = e->name;
      if ((n == "vec_slice" || n == "str_of" || n == "span") &&
          !e->args.empty()) {
        Expr* a = e->args[0];
        if (a && a->k == Expr::UN && a->op == "&") a = a->rhs;
        return a;
      }
    }
    return nullptr;
  }

  void diagF0310(const std::string& msg, SrcLoc at) {
    diag(Diag::Error, Code::BorrowExclusivity, msg, at, "kind=borrow");
  }
  void diagF0311(const std::string& msg, SrcLoc at) {
    diag(Diag::Error, Code::BorrowLifetime, msg, at, "kind=borrow");
  }

  void expireAt(int ordinal) {
    for (auto it = views.begin(); it != views.end();) {
      if (it->second.expiry < ordinal) it = views.erase(it);
      else ++it;
    }
  }

  // live shared views of `root` at this point
  std::vector<std::string> liveViewsOf(const std::string& root) {
    std::vector<std::string> out;
    for (auto& [name, v] : views)
      if (!v.mut && v.root == root) out.push_back(name);
    return out;
  }
  bool mutLiveOf(const std::string& root) {
    for (auto& [name, v] : views)
      if (v.mut && v.root == root) return true;
    return rvalueMut.count(root) > 0;
  }

  void checkMutBorrowCreate(const std::string& root, SrcLoc at) {
    for (auto& lv : liveViewsOf(root)) {
      diagF0310("exclusive `&mut " + root + "` taken while the view '" +
                lv + "' is still live — one writer or many readers "
                "(the view would dangle after the write)", at);
    }
    if (rvalueMut.count(root)) {
      diagF0310("two live mutable borrows of '" + root + "'", at);
    }
    // a binding-held &mut root: single mutable borrow per root
    for (auto& [name, v] : views)
      if (v.mut && v.root == root)
        diagF0310("two live mutable borrows of '" + root +
                 "' ('" + name + "' is still live)", at);
  }

  void checkSharedUse(const std::string& root, SrcLoc at) {
    if (rvalueMut.count(root))
      diagF0310("shared borrow of '" + root +
                "' used while a mutable borrow is live", at);
    for (auto& [name, v] : views)
      if (v.mut && v.root == root)
        diagF0310("shared use of '" + root + "' while the mutable borrow '" +
                  name + "' is live", at);
  }
};

// builtin growth-mutators (ch. 24 §2): reallocation or header mutation
static bool growthMutatorCall(const std::string& n) {
  return n == "vec_push" || n == "vec_insert" || n == "vec_resize" ||
         n == "vec_reserve" || n == "vec_truncate" || n == "vec_clear" ||
         n == "string_append";
}

static void walkBorrowBlock(BorrowWalk& W, Stmt* b, int baseOrdinal);

// check one statement's expressions for borrow events; `ordinal` is the
// statement's position in its block (views live to their expiry)
static void walkBorrowStmt(BorrowWalk& W, Stmt* s, int ordinal) {
  if (!s) return;
  std::function<void(Expr*, SrcLoc)> checkExpr = [&](Expr* e, SrcLoc at) {
    if (!e) return;
    if (W.suppressSubtree == e) return;   // recorded as a binding below
    // rvalue mutable borrows: &mut place
    if (e->k == Expr::UN && e->op == "&") {
      std::string root = BorrowWalk::baseRoot(e);
      if (!root.empty()) {
        if (e->borrowMut) {
          W.checkMutBorrowCreate(root, at);
          W.rvalueMut[root] = at;
        }
      }
    }
    // view creations rooted at a place
    if (Expr* vr = BorrowWalk::viewRootExpr(e)) {
      std::string root = BorrowWalk::baseRoot(vr);
      if (!root.empty()) W.checkSharedUse(root, at);
    }
    for (auto* a : e->args) checkExpr(a, at);
    checkExpr(e->lhs, at);
    checkExpr(e->rhs, at);
    checkExpr(e->base, at);
  };
  auto clearRvalue = [&]() { W.rvalueMut.clear(); };

  switch (s->k) {
    case Stmt::LET: {
      // view/mut-borrow binding creation
      if (s->init) {
        // a borrow/view BINDING records its creation once (the binding
        // IS that borrow) — suppress the same subtree's rvalue events
        bool isBorrowInit = (s->init->k == Expr::UN && s->init->op == "&") ||
                            BorrowWalk::viewRootExpr(s->init) != nullptr;
        if (isBorrowInit) W.suppressSubtree = s->init;
        if (Expr* vr = BorrowWalk::viewRootExpr(s->init)) {
          std::string root = BorrowWalk::baseRoot(vr);
          if (!root.empty()) {
            W.checkSharedUse(root, s->loc);
            // the view binding lives until its last use (block scan)
            W.views[s->var] = {root, false, ordinal, s->loc};
          }
        }
        // let m = &mut x;  — a binding-held mutable borrow
        if (s->init->k == Expr::UN && s->init->op == "&" &&
            s->init->borrowMut) {
          std::string root = BorrowWalk::baseRoot(s->init);
          if (!root.empty()) {
            W.checkMutBorrowCreate(root, s->loc);
            W.views[s->var] = {root, true, ordinal, s->loc};
          }
        }
        // a MOVE of an owning binding conflicts with its live views
        if (s->init->k == Expr::IDENT) {
          std::string src = s->init->name;
          for (auto& lv : W.liveViewsOf(src))
            W.diagF0310("moving '" + src + "' while the view '" + lv +
                        "' is still live — the transfer would dangle it",
                        s->loc);
        }
        checkExpr(s->init, s->loc);
        W.suppressSubtree = nullptr;
        clearRvalue();
      }
      break;
    }
    case Stmt::ASSIGN: {
      // writing THROUGH a place is the in-place carve-out: views stay
      // valid; writing TO a view root (rebinding) conflicts below
      if (s->lhs && s->lhs->k == Expr::IDENT) {
        std::string root = s->lhs->name;
        for (auto& lv : W.liveViewsOf(root))
          W.diagF0310("rebinding '" + root + "' while the view '" + lv +
                      "' is still live", s->loc);
        // storing a borrow of function-local storage into a global
        if (s->lhs->globalRef && s->rhs && s->rhs->k == Expr::UN &&
            s->rhs->op == "&") {
          std::string root = BorrowWalk::baseRoot(s->rhs);
          if (!root.empty() && W.rootIsLocalContainer(root))
            W.diagF0311("storing a borrow of the function-local '" + root +
                        "' into a global — the storage dies at fn exit",
                        s->loc);
        }
      }
      if (s->rhs && s->rhs->k == Expr::IDENT) {
        for (auto& lv : W.liveViewsOf(s->rhs->name))
          W.diagF0310("moving '" + s->rhs->name + "' while the view '" +
                      lv + "' is still live", s->loc);
      }
      checkExpr(s->lhs, s->loc);
      checkExpr(s->rhs, s->loc);
      clearRvalue();
      break;
    }
    case Stmt::RETURN: {
      if (s->retVal) {
        // F0311: returning &mut local
        if (s->retVal->k == Expr::UN && s->retVal->op == "&" &&
            s->retVal->borrowMut) {
          std::string root = BorrowWalk::baseRoot(s->retVal);
          if (!root.empty() && W.rootIsLocalContainer(root))
            W.diagF0311("returning `&mut " + root +
                        "` — the local's drop at fn exit dangles the borrow",
                        s->loc);
        }
        if (Expr* vr = BorrowWalk::viewRootExpr(s->retVal)) {
          std::string root = BorrowWalk::baseRoot(vr);
          if (!root.empty() && W.rootIsLocalContainer(root))
            W.diagF0311("returning a view of '" + root +
                        "' — the storage drops at fn exit (return the "
                        "owner, or take a `borrowed` parameter)", s->loc);
        }
        if (s->retVal->k == Expr::IDENT) {
          auto it = W.views.find(s->retVal->name);
          if (it != W.views.end() && W.rootIsLocalContainer(it->second.root))
            W.diagF0311("returning the view '" + s->retVal->name +
                        "' of '" + it->second.root +
                        "' — its storage drops at fn exit", s->loc);
        }
        // returning an owning binding while a view of it is live would
        // dangle the view at the return point
        if (s->retVal->k == Expr::IDENT) {
          for (auto& lv : W.liveViewsOf(s->retVal->name))
            W.diagF0310("moving '" + s->retVal->name +
                        "' out while the view '" + lv + "' is still live",
                        s->loc);
        }
        checkExpr(s->retVal, s->loc);
        clearRvalue();
      }
      break;
    }
    case Stmt::EXPR: {
      if (s->expr && s->expr->k == Expr::CALL) {
        const std::string n = bareCallName(s->expr);
        // two &mut of one root inside one call: f(&mut x, &mut x)
        std::map<std::string, int> mutCount;
        for (auto* a : s->expr->args)
          if (a && a->k == Expr::UN && a->op == "&" && a->borrowMut) {
            std::string root = BorrowWalk::baseRoot(a);
            if (!root.empty()) mutCount[root]++;
          }
        for (auto& [root, cnt] : mutCount)
          if (cnt > 1)
            W.diagF0310("two live mutable borrows of '" + root +
                        "' in one call", s->loc);
        // growth mutators / releases vs live views of the root
        if (growthMutatorCall(n) || containerReleaseCall(n)) {
          Expr* a0 = s->expr->args.empty() ? nullptr : s->expr->args[0];
          std::string root = BorrowWalk::baseRoot(a0);
          if (!root.empty()) {
            const char* verb = growthMutatorCall(n)
                ? "grows/rebinds" : "releases";
            for (auto& lv : W.liveViewsOf(root))
              W.diagF0310(std::string(n) + " " + verb + " '" + root +
                          "' while the view '" + lv + "' is still live — " +
                          (growthMutatorCall(n)
                           ? "reallocation would dangle it"
                           : "the release would dangle it"), s->loc);
          }
        }
        // by-value owning args are moves
        if (FnDecl* callee = s->expr->callee) {
          for (size_t i = 0; i < s->expr->args.size() &&
                             i < callee->params.size(); ++i) {
            Type* pt = callee->params[i].ty;
            if (!(pt && pt->k == Type::STRUCT &&
                  (containerElemType(pt) || isHandleType(pt) ||
                   typeNeedsDrop(pt))))
              continue;
            std::string root = BorrowWalk::baseRoot(s->expr->args[i]);
            if (root.empty()) continue;
            for (auto& lv : W.liveViewsOf(root))
              W.diagF0310("moving '" + root + "' into " + callee->name +
                          " while the view '" + lv + "' is still live",
                          s->loc);
          }
        }
      }
      checkExpr(s->expr, s->loc);
      clearRvalue();
      break;
    }
    case Stmt::DEFER: {
      checkExpr(s->deferExpr, s->loc);
      clearRvalue();
      break;
    }
    case Stmt::IF: {
      checkExpr(s->cond, s->loc);
      clearRvalue();
      // branches keep the entry state and rejoin (the join is the
      // enclosing sequence's next ordinal — approximate by one walk each
      // from a copy; views created inside a branch die with it)
      auto viewsBefore = W.views;
      walkBorrowBlock(W, s->thenB, ordinal);
      auto afterThen = W.views;
      W.views = viewsBefore;
      if (s->elseB) walkBorrowBlock(W, s->elseB, ordinal);
      // union: a view live in either branch stays live after (its expiry
      // was recorded against this ordinal; extend to keep it)
      for (auto& [name, v] : afterThen) {
        auto it = W.views.find(name);
        if (it == W.views.end()) W.views[name] = v;
        else it->second.expiry = std::max(it->second.expiry, v.expiry);
      }
      break;
    }
    case Stmt::WHILE:
    case Stmt::FOR: {
      // a view live across a loop must survive the WHOLE loop: extend
      // expiry conservatively (the loop body may re-enter)
      for (auto& [name, v] : W.views) v.expiry = INT_MAX;
      walkBorrowBlock(W, s->body, ordinal);
      break;
    }
    case Stmt::PARFOR: {
      // fsht-03:02 (ch. 24 §2): mutable borrows inside a dispatched
      // parfor body are F0307 (checked by the parfor pass); here the
      // body walk still tracks views for the outer sequence
      walkBorrowBlock(W, s->body, ordinal);
      break;
    }
    case Stmt::MATCH: {
      checkExpr(s->matchOn, s->loc);
      clearRvalue();
      auto viewsBefore = W.views;
      for (auto& arm : s->matchArms) {
        W.views = viewsBefore;
        walkBorrowBlock(W, arm.body, ordinal);
      }
      W.views = viewsBefore;
      break;
    }
    case Stmt::BLOCK: {
      walkBorrowBlock(W, s, ordinal);
      break;
    }
    default: break;
  }
}

// walk a block, numbering statements; views of THIS block expire at the
// end (lexical scoping: they cannot be referenced from outside)
static void walkBorrowBlock(BorrowWalk& W, Stmt* b, int baseOrdinal) {
  if (!b) return;
  auto snapshot = W.views;
  for (size_t i = 0; i < b->stmts.size(); ++i) {
    int ordinal = baseOrdinal + (int)i + 1;
    walkBorrowStmt(W, b->stmts[i], ordinal);
    W.expireAt(ordinal);
  }
  W.views = snapshot;   // nested-block views die with the block
}

} // namespace

void runBorrowAnalysis(Program* p) {
  (void)p;
  for (auto* fs : allFnSemas()) {
    if (!fs->fn || !fs->fn->body) continue;
    BorrowWalk W;
    W.fn = fs->fn;
    W.fs = fs;
    walkBorrowBlock(W, fs->fn->body, 0);
  }
}

} // namespace freshet
