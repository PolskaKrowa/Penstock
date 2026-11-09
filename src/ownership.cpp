// Freshet ownership analysis — per-function abstract state machine over
// pointer vars: Alive / MaybeFreed / Freed / Moved.
//   (all reported as F0301)
//   - owned pointer reaches scope end without free (leak)
//   - second free of the same pointer (double free)
//   - use after free / use after give()
//   - give() of a non-owned pointer
#include "common.h"
#include <algorithm>

namespace freshet {

enum OwnState { OS_ALIVE, OS_MAYBE, OS_FREED, OS_MOVED, OS_NONE };

struct OwnInfo {
  std::string var;
  OwnState st = OS_NONE;
  bool owned = false;
  SrcLoc allocLoc, freeLoc;
};

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
  return (e && e->k == Expr::IDENT) ? e->name : "";
}

// process a statement list; returns state map after the block.
// defers collected: freed at scope exit in reverse order.
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
  for (auto* st : b->stmts) ownStmt(ctx, fs, st, defers);
  // run defers in reverse (simulate: apply frees)
  for (auto it = defers.rbegin(); it != defers.rend(); ++it) {
    Expr* de = it->expr;
    if (de->k == Expr::CALL && (de->name == "free" || de->name == "soa_free")) {
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
          f->second.freeLoc = it->loc;
        }
      }
    }
  }
  if (checkLeaks) {
    for (auto& [v, oi] : ctx.vars) {
      if (!oi.owned) continue;
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
      oi.owned = (s->init->k == Expr::CALL && (s->init->name == "malloc" ||
                                               s->init->name == "soa_new"));
      oi.allocLoc = s->loc;
      ctx.vars[s->var] = oi;
      // let x = give-expr? not valid; give only in call args
      break;
    }
    case Stmt::ASSIGN: {
      checkExprUses(ctx, s->lhs, s->loc);
      checkExprUses(ctx, s->rhs, s->loc);
      std::string lv = rootVarName(s->lhs);
      if (s->lhs->k == Expr::IDENT && !lv.empty()) {
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
        oi.owned = (s->rhs->k == Expr::CALL && (s->rhs->name == "malloc" ||
                                                s->rhs->name == "soa_new"));
        oi.allocLoc = s->loc;
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
        if (s->expr->name == "free" || s->expr->name == "soa_free") {
          std::string v = rootVarName(s->expr->args.empty() ? nullptr : s->expr->args[0]);
          if (!v.empty()) applyFree(ctx, v, s->loc, false);
        }
        // give() marks moved
        if (s->expr->name == "give") {
          std::string v = rootVarName(s->expr->args.empty() ? nullptr : s->expr->args[0]);
          if (!v.empty()) applyFree(ctx, v, s->loc, true);
        }
        // user fn call with give args: mark moved
        FnDecl* callee = s->expr->callee;
        if (callee) {
          for (size_t i = 0; i < s->expr->args.size(); ++i) {
            if (!s->expr->argGive[i]) continue;
            std::string v = rootVarName(s->expr->args[i]);
            if (v.empty()) continue;
            auto it = ctx.vars.find(v);
            if (it == ctx.vars.end() || !it->second.owned) {
              diag(Diag::Error, Code::Own,
                   "give() of non-owned pointer '" + v + "'",
                   s->loc,
                   "var=" + v + ",kind=move-nonowned");
            } else {
              applyFree(ctx, v, s->loc, true);
            }
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
    for (auto& sym : fs->allSyms) {
      if (sym.k != Sym::PARAM) continue;
      OwnInfo oi;
      oi.var = sym.name;
      oi.st = OS_ALIVE;
      oi.owned = false;
      ctx.vars[sym.name] = oi;
    }
    std::vector<Deferred> defers;
    // top-level body: leak check at function end
    for (auto* st : fs->fn->body->stmts) ownStmt(ctx, fs, st, defers);
    // function-end defers
    for (auto it = defers.rbegin(); it != defers.rend(); ++it) {
      Expr* de = it->expr;
      if (de->k == Expr::CALL && (de->name == "free" || de->name == "soa_free")) {
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

} // namespace freshet
