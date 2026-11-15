// Freshet — the data-stack memory model, ported from the Goose programming
// language (aardappel/goose): there is no heap for scratch memory.  A
// `stack_alloc(T, n)` allocates n elements of T from the function's DATA
// STACK — a per-thread, pre-reserved bump-pointer region — and the
// allocation dies when its scope ends: IRgen emits a watermark restore
// (`store base, tops[k]`) at the closing brace of the binding's block, in
// reverse declaration order, on every exit path (fall-through, break,
// continue, return, defers-first).  Deallocation is a pointer store: no
// malloc, no free, no destructors — the O(1) teardown that is one of the
// pillars of goose's benchmark wins.
//
// STACK INDICES (goose codegen_stacks.h's AllocStk/SpTop/BoundStacks):
// every live `stack_alloc` binding holds a distinct index into a
// per-thread array of stacks; a binding's index is released when its block
// ends (sequentially scoped, like goose's "locals with disjoint lifetimes
// reuse stacks").  A function that uses stacks (directly or through a
// callee) takes a hidden `i64 %sp` parameter and addresses its stacks at
// `%sp + k`, and a CALLER PASSES its own first-free index at the call —
// so a callee's stacks sit above everything the caller has in use, and
// the program's total N is the max over the call graph (own, offset +
// callee need), computed here to a fixpoint.  goose's §7.8 rule holds: a
// call into a recursive cycle is made with NOTHING in use — a stack live
// across a recursive self-call is F0927 (both activations would collide
// on the same indices; a static N cannot bound the recursion depth).
//
// %sp THROUGH frt_parfor (goose's worker programs, v2): a dispatched
// parfor body is lowered to a chunk function `__pf_body_N(lo, hi, ctx,
// %sp)` whose hidden %sp is the DISPATCHING context's first-free index,
// threaded through frt_range_t.sp0 at offset 80 (the runtime copies it
// into every task and passes it at invocation, exactly like a caller does
// at a call).  Each executing thread — a pool worker, or the dispatching
// thread helping — addresses the region's stacks at [sp0, sp0+need) of
// its OWN per-thread tops array, so concurrent chunks never collide and
// the caller's live stacks [.., sp0) are disjoint by construction.  The
// region is a node of the need fixpoint like a function: the enclosing
// function's need >= dispatch offset + region need, and a region that
// (transitively) re-enters its own dispatch context while a stack is live
// is F0927 (unbounded need — the same §7.8 rule over the fn->region->fn
// edge cycle).  A stack binding of the ENCLOSING context referenced
// inside a dispatched body is F0926 (region capture): the value would be
// copied into a chunk function that other threads execute — a
// cross-thread reference into the dispatching thread's private region
// (goose: workers are separate programs with private stacks).  A
// COLLAPSED (rectangular, serialized) inner parfor runs inside the same
// chunk function and thread: its body is part of the enclosing walk and
// uses the enclosing stacks freely.
//
// STATIC-ROOT LIFETIME PROOFS (goose reference.md §12, "a slice carries
// a static root"): every value that may reference data-stack memory
// carries a ROOT SET — the `let` bindings whose stack_alloc regions it
// points into (a copy/span/cast propagates them).  A store of a rooted
// value is judged by the DESTINATION's roots, not rejected wholesale:
//   * into a local binding / local value storage: LEGAL iff the
//     destination is lexically nested within (or equal to) EVERY source
//     root's scope — the destination provably dies before the memory it
//     now points at ("a slice can neither outlive that variable");
//   * through a rooted pointer (into stack memory): LEGAL iff the
//     destination memory's own roots die before-or-with every source
//     root — the stored reference becomes unreachable no later than the
//     memory it points at dies;
//   * into a global, through an unrooted (heap/param/unknown) pointer,
//     or out of the function (return / give / owned argument) is F0926 —
//     the destination provably MAY outlive the source.
// Scope chains are the walk-time BLOCK stacks, so the proof is the exact
// lexical nesting IRgen emits restores for.
//
// INTERPROCEDURAL NON-ESCAPE SUMMARIES (goose's callee root rules): for
// every function and every parameter, a fixpoint computes whether a
// reference passed at that parameter can escape the call — returned,
// stored into a global, stored through a pointer into caller memory,
// captured by a dispatched region, give()n, or forwarded to a callee
// parameter that may escape (recursion converges from "no escape").
// Passing stack memory at a parameter whose summary says "no escape" is
// PROVEN safe (the callee may only copy the reference into locals that
// die with the call); a parameter that may escape is F0926 (arg-escape).
// Parameters are by-value in Freshet (structs memcpy'd, spec §4), so
// storing into a struct/array VALUE parameter is local and safe.  extern
// (imported) functions are trusted like goose §7.10: they may write
// THROUGH what they are handed, but reach nothing else — they do not
// escape references.  Calls through function pointers have no summary:
// every rooted argument at one is F0926.
//
// The escape analysis is name-based taint over root sets, matching
// IRgen's name-keyed storage; a block-scoped binding's roots die with
// its block, and re-assignment re-roots the binding (an unrooted value
// clears it).  A name shadowed by a body-local binding is conservatively
// treated as the outer binding (the proof can only be lost, never faked).
#include "common.h"
#include "dstk.h"
#include "opt_util.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace freshet {

namespace dstk {
std::map<FnDecl*, Info> g_info;
std::map<const Stmt*, RegionInfo> g_regions;
std::map<const Expr*, int> g_site;
int g_total = 0;
}

namespace {

using namespace optutil;
using dstk::g_info;
using dstk::g_regions;
using dstk::g_site;
using dstk::g_total;

constexpr int DSTK_MAX = 4096;  // per-thread stack count cap

bool isStackAlloc(Expr* e) {
  return e && e->k == Expr::CALL && e->name == "stack_alloc";
}

// a param pseudo-root: "#p<i>" can never collide with a binding name
// (Freshet identifiers cannot start with '#')
inline std::string paramRoot(size_t i) { return "#p" + std::to_string(i); }
inline bool isParamRoot(const std::string& r) {
  return r.size() >= 2 && r[0] == '#' && r[1] == 'p';
}

using Roots = std::set<std::string>;  // stack-root bindings + "#p<i>" params

// =============================================================== layout
// The fn/region node graph: goose's stackcalls/stackown/stackneed with
// dispatched parfor bodies as peer nodes ("worker programs").  Nodes are
// addressed by INDEX — the recursive walk grows the vector, so no
// references may be held across a node creation.
struct LNode {
  FnDecl* fn = nullptr;        // null => a dispatched parfor region
  const Stmt* region = nullptr;
  int own = 0;
  bool uses = false;
  int need = 0;
  std::vector<std::pair<int, int>> edges;  // (node index, sp offset)
};

struct Layout {
  Program* prog;
  std::vector<LNode> nodes;
  std::map<FnDecl*, int> fnIdx;

  int mkFn(FnDecl* f) {
    auto it = fnIdx.find(f);
    if (it != fnIdx.end()) return it->second;
    int id = (int)nodes.size();
    nodes.push_back(LNode{f, nullptr, 0, false, 0, {}});
    fnIdx[f] = id;
    return id;
  }

  std::string nodeName(int id) const {
    const LNode& nd = nodes[id];
    if (nd.fn) return nd.fn->name;
    return "parfor region (line " + std::to_string(nd.region->loc.line) + ")";
  }

  void walkExpr(Expr* e, int nodeId, int next) {
    if (!e) return;
    if (e->k == Expr::CALL) {
      if (e->name == "stack_alloc") {
        diag(Diag::Error, Code::DstkMisuse,
             "stack_alloc must be the direct initializer of a `let` — the "
             "allocation is freed when the binding's block ends",
             e->loc, "kind=non-let");
      } else {
        for (auto* a : e->args) walkExpr(a, nodeId, next);
      }
      if (e->callee) {
        auto it = fnIdx.find(e->callee);
        if (it != fnIdx.end())
          nodes[nodeId].edges.push_back({it->second, next});
      }
      return;
    }
    walkExpr(e->lhs, nodeId, next);
    walkExpr(e->rhs, nodeId, next);
    walkExpr(e->base, nodeId, next);
    for (auto* a : e->args) walkExpr(a, nodeId, next);
  }

  // `inChunk` mirrors IRgen's curFnIsChunkFn: inside a chunk function a
  // `collapsed`-flagged parfor is serialized into the current walk; in a
  // plain function body every parfor dispatches its own region.
  void walkStmt(Stmt* s, int nodeId, int& next, bool inChunk) {
    if (!s) return;
    LNode& nd = nodes[nodeId];
    switch (s->k) {
      case Stmt::LET: {
        if (isStackAlloc(s->init)) {
          for (auto* a : s->init->args) walkExpr(a, nodeId, next);
          if (next >= DSTK_MAX) {
            diag(Diag::Error, Code::DstkMisuse,
                 "too many live stack_alloc bindings in " + nodeName(nodeId) +
                     " — the per-thread stack array is capped at " +
                     std::to_string(DSTK_MAX),
                 s->loc, "kind=cap");
            return;
          }
          nd.uses = true;
          g_site[s->init] = next;
          ++next;   // the binding holds its index until its block ends
          nd.own = std::max(nd.own, next);
          return;
        }
        walkExpr(s->init, nodeId, next);
        return;
      }
      case Stmt::ASSIGN:
        walkExpr(s->lhs, nodeId, next);
        walkExpr(s->rhs, nodeId, next);
        return;
      case Stmt::RETURN: walkExpr(s->retVal, nodeId, next); return;
      case Stmt::DEFER: walkExpr(s->deferExpr, nodeId, next); return;
      case Stmt::IF:
        walkExpr(s->cond, nodeId, next);
        walkStmt(s->thenB, nodeId, next, inChunk);
        walkStmt(s->elseB, nodeId, next, inChunk);
        return;
      case Stmt::WHILE:
        walkExpr(s->cond, nodeId, next);
        walkStmt(s->body, nodeId, next, inChunk);
        return;
      case Stmt::FOR:
        walkExpr(s->forInit, nodeId, next);
        walkExpr(s->forCond, nodeId, next);
        walkExpr(s->forStep, nodeId, next);
        walkExpr(s->rangeLo, nodeId, next);
        walkExpr(s->rangeHi, nodeId, next);
        walkStmt(s->body, nodeId, next, inChunk);
        return;
      case Stmt::PARFOR: {
        walkExpr(s->plo, nodeId, next);
        walkExpr(s->phi, nodeId, next);
        for (auto& c : s->clauses) walkExpr(c.e, nodeId, next);
        if (inChunk && s->collapsed) {
          // serialized into this chunk fn — its scopes release indices
          // here, exactly like a block
          int save = next;
          walkStmt(s->body, nodeId, next, inChunk);
          next = save;
          return;
        }
        // dispatched region: a peer node whose stacks sit at the
        // dispatching context's first-free index (frt_range_t.sp0)
        int rid = (int)nodes.size();
        nodes.push_back(LNode{nullptr, s, 0, false, 0, {}});
        nodes[nodeId].edges.push_back({rid, next});
        int rnext = 0;
        walkStmt(s->body, rid, rnext, true);
        return;
      }
      case Stmt::MATCH:
        walkExpr(s->matchOn, nodeId, next);
        for (auto& arm : s->matchArms)
          walkStmt(arm.body, nodeId, next, inChunk);
        return;
      case Stmt::BLOCK: {
        int save = next;
        for (auto* st : s->stmts) walkStmt(st, nodeId, next, inChunk);
        walkExpr(s->expr, nodeId, next);
        next = save;   // the block's indices are released here
        return;
      }
      case Stmt::EXPR: walkExpr(s->expr, nodeId, next); return;
      default: return;
    }
  }
};

// ============================================ static-root lifetime proofs
// One function-or-region walk context: root sets, lexical scope chains,
// binding types, and (for the owning function) param-escape events.
struct RootWalk {
  Program* prog;
  FnDecl* fn = nullptr;         // the owning FUNCTION (escape attribution)
  const Stmt* region = nullptr; // non-null: this context is a region body
  bool inChunk = false;         // inside a chunk fn (serialized parfors ok)
  bool diagnose = false;        // emit F0926/F0927 (final walk only)
  std::vector<Stmt*> scope;     // enclosing BLOCK chain (frames are roots)
  std::map<std::string, Roots> roots;
  std::map<std::string, std::vector<Stmt*>> chain;  // name -> decl chain
  std::map<std::string, Type*> types;
  std::vector<bool>* esc = nullptr;                  // fn's escape bits
  const std::map<FnDecl*, std::vector<bool>>* escTab = nullptr;

  // ---- root derivation
  Roots rootsOf(Expr* e) const {
    Roots r;
    if (!e) return r;
    switch (e->k) {
      case Expr::IDENT: {
        auto it = roots.find(e->name);
        if (it != roots.end()) return it->second;
        return r;
      }
      case Expr::CAST:
        return rootsOf(e->base);
      case Expr::CALL:
        // span(tainted, n): the span points into the same stack memory
        if (e->name == "span" && !e->args.empty())
          return rootsOf(e->args[0]);
        return r;
      default:
        return r;
    }
  }

  static bool hasStackRoots(const Roots& r) {
    for (auto& s : r) if (!isParamRoot(s)) return true;
    return false;
  }
  static bool hasParamRoots(const Roots& r) {
    for (auto& s : r) if (isParamRoot(s)) return true;
    return false;
  }
  void escapeParams(const Roots& r) {
    if (!esc) return;
    for (auto& s : r)
      if (isParamRoot(s)) {
        size_t i = (size_t)atoll(s.c_str() + 2);
        if (esc->size() > i) (*esc)[i] = true;
      }
  }

  // dest chain must extend (nest within or equal) every src root's chain:
  // the destination dies before-or-with the memory it now points at
  static bool nestedInEq(const std::vector<Stmt*>& a,
                         const std::vector<Stmt*>& b) {
    if (a.size() < b.size()) return false;
    return std::equal(b.begin(), b.end(), a.begin());
  }
  bool destMayHold(const std::vector<Stmt*>& destChain, const Roots& src) {
    for (auto& r : src) {
      if (isParamRoot(r)) continue;  // the caller's scope outlives the call
      auto it = chain.find(r);
      if (it == chain.end()) return false;
      if (!nestedInEq(destChain, it->second)) return false;
    }
    return true;
  }

  // ---- classification of an assignment destination's base binding
  struct DstBase {
    bool found = false;
    bool isGlobal = false;
    std::string name;
    Type* ty = nullptr;
    bool isRef = false;
  };
  DstBase classifyDst(Expr* lhs) {
    DstBase d;
    Expr* b = lhs;
    bool deref = false;
    while (b) {
      if (b->k == Expr::FIELD || b->k == Expr::INDEX) {
        deref = deref || b->ptrDeref;
        b = b->base;
      } else if (b->k == Expr::CAST) {
        b = b->base;
      } else {
        break;
      }
    }
    if (!b || b->k != Expr::IDENT) return d;
    d.found = true;
    d.name = b->name;
    if (b->globalRef) {
      d.isGlobal = true;
      d.ty = b->globalRef->ty;
    } else {
      auto it = types.find(b->name);
      d.ty = it != types.end() ? it->second : (b->ty ? b->ty : nullptr);
    }
    Type* t = d.ty;
    d.isRef = deref || (t && (t->isPtr() || t->k == Type::SPAN ||
                              t->k == Type::STR || t->k == Type::SOA));
    return d;
  }

  void diagEscape(const std::string& msg, SrcLoc loc, const std::string& k) {
    if (diagnose) diag(Diag::Error, Code::DstkEscape, msg, loc, k);
  }
  void diagMisuse(const std::string& msg, SrcLoc loc, const std::string& k) {
    if (diagnose) diag(Diag::Error, Code::DstkMisuse, msg, loc, k);
  }

  // a callee parameter's escape summary; imported externs are trusted
  // (goose §7.10), everything else without a body escapes conservatively
  bool paramEscapes(FnDecl* callee, size_t i) const {
    if (!callee) return true;                       // indirect/fn-ptr call
    if (callee->imported) return false;             // trusted extern
    if (callee->isGeneric || !callee->body) return true;
    if (!escTab) return true;
    auto it = escTab->find(callee);
    if (it == escTab->end() || it->second.size() <= i) return true;
    return it->second[i];
  }

  // =============================== expressions (kill/propagation events)
  void walkExpr(Expr* e) {
    if (!e) return;
    switch (e->k) {
      case Expr::CALL: {
        for (auto* a : e->args) walkExpr(a);
        if (e->name == "free" && !e->args.empty() &&
            hasStackRoots(rootsOf(e->args[0])))
          diagMisuse("free() on stack_alloc memory — data-stack memory is "
                     "freed by its scope, never by free()",
                     e->loc, "kind=free-of-stack");
        if (e->name == "give" && !e->args.empty()) {
          Roots r = rootsOf(e->args[0]);
          if (hasStackRoots(r))
            diagEscape("stack_alloc memory give()n — ownership transfer "
                       "moves it to a callee that outlives the owning "
                       "scope",
                       e->loc, "kind=give");
          escapeParams(r);   // a param-rooted reference given away escapes
        }
        if (e->callee || e->fnPtrSym) {
          for (size_t i = 0; i < e->args.size(); ++i) {
            Roots r = rootsOf(e->args[i]);
            if (r.empty()) continue;
            bool owned = e->argGive.size() > i && e->argGive[i];
            if (owned) {
              if (hasStackRoots(r))
                diagEscape("stack_alloc memory passed as an owned argument "
                           "of " +
                               (e->callee ? e->callee->name
                                          : std::string("a callee")) +
                               " — the callee outlives the owning scope",
                           e->loc, "kind=owned-arg");
              escapeParams(r);
            } else if (paramEscapes(e->callee, i)) {
              if (hasStackRoots(r))
                diagEscape("stack memory passed to " +
                               (e->callee ? e->callee->name
                                          : std::string("a function pointer")) +
                               " at a parameter that may let it outlive "
                               "its owning scope (its body returns it, "
                               "stores it into a global or into caller "
                               "memory, or captures it in a parfor)",
                           e->loc, "kind=arg-escape");
              escapeParams(r);
            }
            // else PROVEN: the callee cannot store the reference anywhere
            // that outlives the call — passing stack memory is safe
          }
        }
        return;
      }
      default: {
        walkExpr(e->lhs);
        walkExpr(e->rhs);
        walkExpr(e->base);
        for (auto* a : e->args) walkExpr(a);
        return;
      }
    }
  }

  // =============================== statements
  void bindName(const std::string& n, Expr* init, Type* annTy) {
    if (n.empty()) return;
    chain[n] = scope;
    types[n] = annTy ? annTy : (init && init->ty ? init->ty : nullptr);
    if (isStackAlloc(init)) {
      Roots self;
      self.insert(n);   // the binding IS the static root (goose RootsOf)
      roots[n] = self;
    } else {
      roots[n] = rootsOf(init);
    }
  }

  void checkStore(Stmt* s) {
    Roots rhs = rootsOf(s->rhs);
    if (rhs.empty()) {
      // re-binding an unrooted value clears the old roots
      if (s->lhs && s->lhs->k == Expr::IDENT && !s->lhs->globalRef) {
        auto rit = roots.find(s->lhs->name);
        if (rit != roots.end()) rit->second.clear();
      }
      return;
    }
    if (s->lhs && s->lhs->k == Expr::IDENT) {
      if (s->lhs->globalRef) {
        if (hasStackRoots(rhs))
          diagEscape("stack_alloc memory stored into the global '" +
                         s->lhs->name + "' — a global outlives the scope "
                         "that owns the allocation",
                     s->loc, "kind=global-store");
        escapeParams(rhs);
        return;
      }
      // local binding: the destination must die before-or-with every
      // stack root of the value (static-root lifetime proof)
      std::vector<Stmt*> dc;
      auto cit = chain.find(s->lhs->name);
      if (cit != chain.end()) dc = cit->second;
      if (!destMayHold(dc, rhs)) {
        diagEscape("'" + s->lhs->name + "' would outlive the stack_alloc "
                   "scope(s) it is made to point at — the destination of "
                   "a stack reference must be lexically nested within "
                   "every root's scope",
                   s->loc, "kind=stored-into-outer-scope");
        return;
      }
      roots[s->lhs->name] = rhs;   // tracked onward
      return;
    }
    if (!s->lhs || s->lhs->k == Expr::IDENT) return;
    // FIELD / INDEX / DEREF destination
    DstBase d = classifyDst(s->lhs);
    if (!d.found) {
      if (hasStackRoots(rhs))
        diagEscape("stack_alloc memory stored through '" + s->lhs->name +
                       "' — the destination may outlive the scope that "
                       "owns the allocation",
                   s->loc, "kind=through-unknown");
      escapeParams(rhs);
      return;
    }
    if (d.isGlobal) {
      if (hasStackRoots(rhs))
        diagEscape("stack_alloc memory stored into the global '" + d.name +
                       "' — a global outlives the scope that owns the "
                       "allocation",
                   s->loc, "kind=global-store");
      escapeParams(rhs);
      return;
    }
    if (d.isRef) {
      // writing THROUGH a reference: the destination memory belongs to
      // the base's roots — param roots are caller memory (the stored
      // reference escapes there), stack roots must die before-or-with
      // every source root
      Roots destRoots;
      auto rit = roots.find(d.name);
      if (rit != roots.end()) destRoots = rit->second;
      if (hasParamRoots(destRoots)) {
        escapeParams(rhs);   // the reference now sits in caller memory
        return;
      }
      if (hasStackRoots(destRoots)) {
        // fn-local stack memory: the destination slot dies with its root
        for (auto& m : destRoots) {
          auto cit = chain.find(m);
          if (cit == chain.end() || !destMayHold(cit->second, rhs)) {
            diagEscape("stack_alloc memory stored through '" + d.name +
                           "' into memory that outlives the source scope — "
                           "the destination's root must die before-or-with "
                           "every root of the stored reference",
                       s->loc, "kind=through-pointer");
            return;
          }
        }
        return;   // the slot (and the reference in it) dies with its root
      }
      // unrooted reference: heap / param / unknown target
      if (hasStackRoots(rhs))
        diagEscape("stack_alloc memory stored through '" + d.name +
                       "' — the destination may outlive the scope that "
                       "owns the allocation (only provably scope-nested "
                       "data-stack destinations are allowed)",
                   s->loc, "kind=through-unknown");
      escapeParams(rhs);
      return;
    }
    // VALUE storage (array/struct binding): dies with its scope
    std::vector<Stmt*> dc;
    auto cit = chain.find(d.name);
    if (cit != chain.end()) dc = cit->second;
    if (!destMayHold(dc, rhs)) {
      diagEscape("'" + d.name + "' would outlive the stack_alloc scope(s) "
                 "stored into it — the destination must be lexically "
                 "nested within every root's scope",
                 s->loc, "kind=stored-into-outer-scope");
      return;
    }
    auto rit = roots.find(d.name);
    if (rit == roots.end()) roots[d.name] = rhs;
    else
      for (auto& r : rhs) rit->second.insert(r);
  }

  // deep IDENT scan of a dispatched body (region-capture check); a name
  // shadowed by a body-local binding is conservatively flagged too
  void captureCheck(Stmt* body) {
    std::set<std::string> names;
    std::function<void(Expr*)> we = [&](Expr* e) {
      if (!e) return;
      if (e->k == Expr::IDENT && !e->name.empty()) names.insert(e->name);
      we(e->lhs); we(e->rhs); we(e->base);
      for (auto* a : e->args) we(a);
    };
    std::function<void(Stmt*)> ws = [&](Stmt* s) {
      if (!s) return;
      ws(s->body); ws(s->thenB); ws(s->elseB);
      for (auto* st : s->stmts) ws(st);
      for (auto& arm : s->matchArms) ws(arm.body);
      switch (s->k) {
        case Stmt::LET: we(s->init); break;
        case Stmt::ASSIGN: we(s->lhs); we(s->rhs); break;
        case Stmt::RETURN: we(s->retVal); break;
        case Stmt::DEFER: we(s->deferExpr); break;
        case Stmt::IF: case Stmt::WHILE: we(s->cond); break;
        case Stmt::FOR:
          we(s->forInit); we(s->forCond); we(s->forStep);
          we(s->rangeLo); we(s->rangeHi); break;
        case Stmt::PARFOR:
          we(s->plo); we(s->phi);
          for (auto& c : s->clauses) we(c.e);
          break;
        case Stmt::MATCH: we(s->matchOn); break;
        case Stmt::EXPR: we(s->expr); break;
        default: break;
      }
    };
    ws(body);
    for (auto& n : names) {
      auto it = roots.find(n);
      if (it == roots.end() || it->second.empty()) continue;
      if (hasStackRoots(it->second))
        diagEscape("'" + n + "' points into data-stack memory of this "
                   "thread and is referenced inside a dispatched parfor "
                   "body — the chunk function may execute on another "
                   "thread, whose data stacks are private (goose: workers "
                   "are separate programs)",
                   body->loc, "kind=region-capture");
      escapeParams(it->second);   // param-rooted references are captured
    }
  }

  void walkStmt(Stmt* s) {
    if (!s) return;
    switch (s->k) {
      case Stmt::LET: {
        walkExpr(s->init);
        bindName(s->var, s->init, s->hasAnn ? s->annTy : nullptr);
        return;
      }
      case Stmt::ASSIGN: {
        if (s->op2 != "=") {
          walkExpr(s->lhs);
          walkExpr(s->rhs);
        } else {
          walkExpr(s->rhs);
          walkExpr(s->lhs);
        }
        checkStore(s);
        return;
      }
      case Stmt::RETURN: {
        Roots r = rootsOf(s->retVal);
        walkExpr(s->retVal);
        if (hasStackRoots(r))
          diagEscape("stack_alloc memory returned from " + fn->name +
                     " — the allocation dies when its scope ends, and the "
                     "caller outlives that",
                     s->loc, "kind=return");
        escapeParams(r);
        return;
      }
      case Stmt::DEFER: walkExpr(s->deferExpr); return;
      case Stmt::IF:
        walkExpr(s->cond);
        walkStmt(s->thenB);
        walkStmt(s->elseB);
        return;
      case Stmt::WHILE:
        walkExpr(s->cond);
        walkStmt(s->body);
        return;
      case Stmt::FOR:
        walkExpr(s->forInit);
        walkExpr(s->forCond);
        walkExpr(s->forStep);
        walkExpr(s->rangeLo);
        walkExpr(s->rangeHi);
        walkStmt(s->body);
        return;
      case Stmt::PARFOR: {
        walkExpr(s->plo);
        walkExpr(s->phi);
        for (auto& c : s->clauses) walkExpr(c.e);
        if (inChunk && s->collapsed) {
          // serialized into this chunk fn: same thread, same stacks —
          // the body's own BLOCK opens/closes its scopes
          walkStmt(s->body);
          return;
        }
        // dispatched region: no binding of this context may be captured
        captureCheck(s->body);
        RootWalk rw;
        rw.prog = prog;
        rw.fn = fn;
        rw.region = s;
        rw.inChunk = true;
        rw.diagnose = diagnose;
        rw.esc = esc;
        rw.escTab = escTab;
        rw.scope = {s->body};
        rw.walkStmt(s->body);
        return;
      }
      case Stmt::MATCH:
        walkExpr(s->matchOn);
        for (auto& arm : s->matchArms) walkStmt(arm.body);
        return;
      case Stmt::BLOCK: {
        // block-scoped bindings: roots/chains/types die with the block
        auto save = snapshotScope();
        scope.push_back(s);
        for (auto* st : s->stmts) walkStmt(st);
        walkExpr(s->expr);
        restoreScope(save);
        return;
      }
      case Stmt::EXPR: walkExpr(s->expr); return;
      default: return;
    }
  }

  // scope snapshots: only the per-scope maps are saved/restored (escape
  // bits live in the caller's table)
  struct ScopeSave {
    std::map<std::string, Roots> roots;
    std::map<std::string, std::vector<Stmt*>> chain;
    std::map<std::string, Type*> types;
    std::vector<Stmt*> scope;
  };
  ScopeSave snapshotScope() const {
    ScopeSave r;
    r.roots = roots;
    r.chain = chain;
    r.types = types;
    r.scope = scope;
    return r;
  }
  void restoreScope(const ScopeSave& r) {
    roots = r.roots;
    chain = r.chain;
    types = r.types;
    scope = r.scope;
  }
};

// params are reference-typed when a rooted argument may be passed at
// them; their roots are the "#p<i>" pseudo-roots
void rootWalkInitParams(RootWalk& w, FnDecl* fn) {
  for (size_t i = 0; i < fn->params.size(); ++i) {
    const auto& pm = fn->params[i];
    w.chain[pm.name] = w.scope;
    w.types[pm.name] = pm.ty;
    Type* t = pm.ty;
    if (t && (t->isPtr() || t->k == Type::SPAN || t->k == Type::STR ||
              t->k == Type::SOA)) {
      Roots r;
      r.insert(paramRoot(i));
      w.roots[pm.name] = r;
    }
  }
}

}  // namespace

int runDstkAnalysis(Program* p) {
  g_info.clear();
  g_regions.clear();
  g_site.clear();
  g_total = 0;

  // ================= pass 1: layout — site indices + the node graph
  Layout lay{p, {}, {}};
  for (auto* fn : p->fns) {
    if (!fn->body || fn->imported || fn->isGeneric) continue;
    lay.mkFn(fn);
  }
  for (auto& kv : lay.fnIdx) {
    int next = 0;
    lay.walkStmt(kv.first->body, kv.second, next, false);
  }
  if (g_site.empty()) return 0;

  // ================= need fixpoint: need = max(own, offset + callee need)
  for (auto& nd : lay.nodes) nd.need = nd.own;
  for (bool changed = true; changed;) {
    changed = false;
    for (auto& nd : lay.nodes)
      for (auto& e : nd.edges) {
        int v = e.second + lay.nodes[e.first].need;
        if (v > nd.need) {
          nd.need = v;
          changed = true;
        }
      }
  }

  // ================= §7.8: an edge with a live stack into its own
  // recursion cycle would demand unbounded indices (Tarjan over nodes)
  {
    std::map<int, int> index, low;
    std::set<int> onStack;
    std::vector<int> stk;
    std::map<int, std::set<int>> peers;
    int counter = 0;
    std::function<void(int)> strong = [&](int v) {
      index[v] = low[v] = counter++;
      stk.push_back(v);
      onStack.insert(v);
      for (auto& e : lay.nodes[v].edges) {
        int w = e.first;
        if (!index.count(w)) {
          strong(w);
          low[v] = std::min(low[v], low[w]);
        } else if (onStack.count(w)) {
          low[v] = std::min(low[v], index[w]);
        }
      }
      if (low[v] == index[v]) {
        std::set<int> scc;
        while (true) {
          int w2 = stk.back();
          stk.pop_back();
          onStack.erase(w2);
          scc.insert(w2);
          if (w2 == v) break;
        }
        if (scc.size() > 1) {
          for (int f2 : scc) {
            peers[f2].insert(scc.begin(), scc.end());
            peers[f2].erase(f2);
          }
        } else {
          for (auto& e : lay.nodes[v].edges)   // self-edge counts
            if (e.first == v) peers[v].insert(v);
        }
      }
    };
    for (size_t i = 0; i < lay.nodes.size(); ++i)
      if (!index.count((int)i)) strong((int)i);
    for (size_t i = 0; i < lay.nodes.size(); ++i)
      for (auto& e : lay.nodes[i].edges)
        if (e.second > 0 && peers[(int)i].count(e.first)) {
          LNode& from = lay.nodes[i];
          LNode& to = lay.nodes[e.first];
          diag(Diag::Error, Code::DstkMisuse,
               "a stack_alloc binding is live across the recursive call "
               "to " + lay.nodeName(e.first) +
                   " — both activations would collide on the same data "
                   "stacks (goose §7.8: a call into a recursive cycle is "
                   "made with nothing in use)",
               from.fn ? from.fn->loc : from.region->loc,
               from.fn ? "fn=" + from.fn->name : "kind=region-cycle");
        }
  }

  // ================= fill the shared tables + caps
  g_total = 0;
  for (size_t ni = 0; ni < lay.nodes.size(); ++ni) {
    LNode& nd = lay.nodes[ni];
    if (nd.own > DSTK_MAX) {
      diag(Diag::Error, Code::DstkMisuse,
           lay.nodeName((int)ni) + " holds " + std::to_string(nd.own) +
               " data stacks at once — above the per-thread cap of " +
               std::to_string(DSTK_MAX),
           nd.fn ? nd.fn->loc : nd.region->loc, "kind=cap");
      continue;
    }
    g_total = std::max(g_total, nd.need);
    if (nd.fn) {
      dstk::Info& info = g_info[nd.fn];
      info.uses = nd.uses;
      info.own = nd.own;
      info.need = nd.need;
      info.needsSp = nd.need > 0;
    } else {
      dstk::RegionInfo& ri = g_regions[nd.region];
      ri.uses = nd.uses;
      ri.own = nd.own;
      ri.need = nd.need;
    }
  }
  if (g_total > DSTK_MAX) {
    diag(Diag::Error, Code::DstkMisuse,
         "the program needs " + std::to_string(g_total) +
             " data stacks — above the per-thread cap of " +
             std::to_string(DSTK_MAX),
         p->file, 0, 0);
    g_total = 0;
  }

  // ================= entry/ABI restrictions (v1 rules, unchanged)
  for (auto& kv : g_info) {
    FnDecl* fn = kv.first;
    if (!kv.second.needsSp) continue;
    if (fn->exported)
      diag(Diag::Error, Code::DstkMisuse,
           "export fn " + fn->name +
               " uses data stacks — the hidden stack-index parameter "
               "changes the C ABI",
           fn->loc, "kind=export-abi");
    // a function VALUE of a stack-using fn would drop the hidden parameter
    std::function<void(Expr*)> fe = [&](Expr* e) {
      if (!e) return;
      if (e->k == Expr::IDENT && e->fnValue == fn)
        diag(Diag::Error, Code::DstkMisuse,
             "function value of " + fn->name +
                 " — a stack-using function's hidden stack-index "
                 "parameter cannot pass through a function pointer",
             e->loc, "kind=fn-value");
      fe(e->lhs); fe(e->rhs); fe(e->base);
      for (auto* a : e->args) fe(a);
    };
    std::function<void(Stmt*)> fsw = [&](Stmt* s) {
      if (!s) return;
      fsw(s->body); fsw(s->thenB); fsw(s->elseB);
      for (auto* st : s->stmts) fsw(st);
      for (auto& arm : s->matchArms) fsw(arm.body);
      switch (s->k) {
        case Stmt::LET: fe(s->init); break;
        case Stmt::ASSIGN: fe(s->lhs); fe(s->rhs); break;
        case Stmt::RETURN: fe(s->retVal); break;
        case Stmt::DEFER: fe(s->deferExpr); break;
        case Stmt::IF: case Stmt::WHILE: fe(s->cond); break;
        case Stmt::FOR:
          fe(s->forInit); fe(s->forCond); fe(s->forStep);
          fe(s->rangeLo); fe(s->rangeHi); break;
        case Stmt::PARFOR:
          fe(s->plo); fe(s->phi);
          for (auto& c : s->clauses) fe(c.e);
          break;
        case Stmt::MATCH: fe(s->matchOn); break;
        case Stmt::EXPR: fe(s->expr); break;
        default: break;
      }
    };
    for (auto* other : p->fns)
      if (other->body && !other->imported && !other->isGeneric)
        fsw(other->body);
  }

  // ================= pass 2: param-escape summaries (fixpoint, silent)
  std::map<FnDecl*, std::vector<bool>> esc;
  for (auto& kv : lay.fnIdx)
    esc[kv.first].assign(kv.first->params.size(), false);
  for (bool changed = true; changed;) {
    changed = false;
    for (auto& kv : lay.fnIdx) {
      FnDecl* fn = kv.first;
      std::vector<bool> proposed(fn->params.size(), false);
      RootWalk w;
      w.prog = p;
      w.fn = fn;
      w.inChunk = false;
      w.diagnose = false;          // the fixpoint walk is silent
      w.esc = &proposed;
      w.escTab = &esc;
      w.scope = {fn->body};
      rootWalkInitParams(w, fn);
      w.walkStmt(fn->body);
      auto& bits = esc[fn];
      for (size_t i = 0; i < proposed.size(); ++i)
        if (proposed[i] && !bits[i]) {
          bits[i] = true;
          changed = true;
        }
    }
  }

  // ================= pass 3: static-root proofs (final, diagnosing walk)
  for (auto& kv : lay.fnIdx) {
    FnDecl* fn = kv.first;
    RootWalk w;
    w.prog = p;
    w.fn = fn;
    w.inChunk = false;
    w.diagnose = true;
    w.esc = &esc[fn];   // events here feed nothing further; kept for shape
    w.escTab = &esc;
    w.scope = {fn->body};
    rootWalkInitParams(w, fn);
    w.walkStmt(fn->body);
  }

  // ================= report (--stacks): goose's StackReport
  if (g_opts.dstkReportOn) {
    for (auto& kv : g_info) {
      if (!kv.second.uses) continue;
      char extra[96];
      snprintf(extra, sizeof extra, "kind=layout,own=%d,need=%d",
               kv.second.own, kv.second.need);
      diag(Diag::Note, Code::DstkLayout,
           "fn " + kv.first->name + ": " +
               std::to_string(kv.second.own) + " own stack(s), " +
               std::to_string(kv.second.need) + " with callees",
           kv.first->loc, extra);
      proofLogLine("dstk", "scope-indices;sp-above-caller;watermark-restore",
                   kv.first, kv.first->loc);
    }
    for (auto& kv : g_regions) {
      if (!kv.second.uses) continue;
      char extra[96];
      snprintf(extra, sizeof extra, "kind=region,own=%d,need=%d",
               kv.second.own, kv.second.need);
      diag(Diag::Note, Code::DstkLayout,
           "parfor region (line " + std::to_string(kv.first->loc.line) +
               "): " + std::to_string(kv.second.own) +
               " own stack(s) in its chunk function, " +
               std::to_string(kv.second.need) +
               " with callees — %sp threaded through frt_parfor "
               "(range offset 80)",
           kv.first->loc, extra);
      proofLogLine("dstk", "region-chunk-sp;sp0-at-dispatch", nullptr,
                   kv.first->loc);
    }
    fprintf(stderr, "penstock: data stacks: N=%d per thread\n", g_total);
  }
  return (int)g_site.size();
}

}  // namespace freshet
