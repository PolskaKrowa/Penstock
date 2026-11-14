// Freshet — mem & vectorisation stage, part 1 (post-sema, pre-irgen):
//
//   --mem-reuse     allocation churn elimination (F0919)
//   --mov-minimize  redundant value-move elimination (F0918)
//   --smt-interleave  memory/compute parfor pairing (F0922, marks only —
//                     IRgen lowers the pair to frt_parfor_pair)
//
// Like every Phase-D transform these are OPT-IN source-level AST
// rewrites: they run post-sema/post-checks/pre-irgen at -O2+, reuse or
// remove existing sema'd nodes, introduce no new bindings (except the
// hoisted churn buffer, which keeps its original name), and emit one
// N-note + one --proof-log line per applied rewrite.
//
// ------------------------------------------------------------------ mem-reuse
// Two allocation-churn shapes are removed:
//
//  L (loop churn).  A sequential loop (for/while — never parfor: its
//  iterations are concurrent and would share the hoisted buffer) whose
//  body allocates a fresh buffer each iteration and frees it before the
//  iteration ends:
//
//      for (k in 0..n) {            let b = malloc(T, m);      // m k-invariant
//        let b = malloc(T, m);  ->   for (k in 0..n) {
//        ...uses b...;                    ...uses b...;        // b reused
//        free(b);                     }
//      }                             free(b);
//
//  Legality premises (all recorded in the proof log):
//    * the buffer never escapes the iteration: every access to b inside
//      the body is a DIRECT b[...] / b[...].f index (or soa_len(b)); a
//      bare b passed to any call, &b[..], give, or a rebinding LET that
//      mentions b vetoes the rewrite (the callee could retain the
//      pointer past the free);
//    * no assignment rebinds b inside the body;
//    * the allocation count m is loop-invariant (reads nothing the loop
//      writes, does not mention the induction variable);
//    * every read of b[i] has a matching write of b[i] (same canonical
//      index) somewhere in the body — a pure sanity gate: elements that
//      are only ever read carry malloc's unspecified contents in BOTH
//      forms (fresh malloc is not zeroed), so defined behaviour — a
//      function of WRITTEN contents only — is preserved;
//    * the body holds no give and no unknown-memory effects.
//
//  S (free/alloc fuse).  A free(p) IMMEDIATELY followed by a fresh
//  binding of the identical shape (same type, same count expression):
//
//      free(p);  let q = malloc(T, m);   ->   let q = p;      // mov-min
//                                                                      // forwards q
//  Premises: p's declaration is `let p = malloc(T, m)` with canonically
//  identical T/m, q is a fresh immutable binding whose later accesses
//  obey the same escape rules as L, and nothing escapes between the two
//  statements (they are adjacent, so nothing can).
//
// ------------------------------------------------------------------ mov-min
// Three mov shapes are removed (the "expensive mov" family — scalar
// copies lower to store+load pairs, aggregate copies lower to
// llvm.memcpy, and mis-grouped scalar ops lower to lane-crossing
// shuffles in the vectoriser, which the --batch-ops pass addresses):
//
//  1. self-assignments `x = x` (and `s.f = s.f` on non-pointer locals):
//     a load+store (memcpy for aggregates) back into the same slot.
//  2. dead movs `let a = b;` where a is never used.
//  3. mov forwarding `let a = b;` (b a bare local/param): every use of
//     a is rewritten to b and the binding dropped.  Chained movs are
//     forwarded across rounds.  Gates: a is immutable and declared
//     exactly once, b likewise, no global shadows either name; for
//     aggregate a (struct/enum/array/soa) additionally no assignment
//     through a or b anywhere in the fn and no call may mention either
//     (the by-address ABI would hand out the aliased storage); span/str
//     pairs may be passed to non-owning calls (the borrowed contract).
//     Scalar/pointer movs need no extra gate: mutability diagnostics
//     (F0208) already prove no write goes through them.
//
// ------------------------------------------------------------------ smt pair
// --smt-interleave classifies every parfor body by its memory/compute
// census and pairs ADJACENT parfors with complementary classes:
//
//      parfor (i in 0..n) { gather: b[i] = src[p[i]]; }   // memory-bound
//      parfor (i in 0..n) { x[i] = heavy(x[i]); }         // compute-bound
//    -> one frt_parfor_pair dispatch: the runtime queues the first
//       region's chunks on the memory lane and the second's on the
//       compute lane, so on an SMT core one hardware thread streams
//       memory while its sibling keeps the shared ALU busy.
//
//  Premises: regions are adjacent, outermost (depth 0), not
//  @reproducible (fixed-block schedule stays untouched), fully
//  independent (no root written by one is read or written by the other;
//  the later region's bounds read nothing the earlier writes; no
//  unknown-memory effects on either side), and their classes are
//  complementary (memory+compute, memory+balanced, balanced+compute).
//  Each region's chunk partition and reduction combine order are
//  unchanged — only the execution overlap differs.
#include "opt_util.h"
#include "loop_analysis.h"
#include <algorithm>
#include <set>

namespace freshet {
namespace memstage {

using namespace optutil;

// ---------------------------------------------------------- declaration census
// name -> how many bindings declare it in this function (params, lets,
// loop ivars, parfor pvars, match-arm payload bindings).  Shadowing or
// redeclaration makes every name-based rewrite below unsafe, so any
// count != 1 vetoes.
struct Census {
  std::map<std::string, int> decls;
  std::set<std::string> muts;      // names declared mut anywhere
  std::set<std::string> params;
};

static void censusExpr(Expr* e, Census* c) {
  if (!e) return;
  for (auto* a : e->args) censusExpr(a, c);
  censusExpr(e->lhs, c);
  censusExpr(e->rhs, c);
  censusExpr(e->base, c);
}

static void censusStmt(Stmt* s, Census* c) {
  if (!s) return;
  switch (s->k) {
    case Stmt::LET:
      ++c->decls[s->var];
      if (s->mut) c->muts.insert(s->var);
      censusExpr(s->init, c);
      break;
    case Stmt::ASSIGN:
      censusExpr(s->lhs, c);
      censusExpr(s->rhs, c);
      break;
    case Stmt::RETURN: censusExpr(s->retVal, c); break;
    case Stmt::DEFER:  censusExpr(s->deferExpr, c); break;
    case Stmt::EXPR:   censusExpr(s->expr, c); break;
    case Stmt::IF:
      censusExpr(s->cond, c);
      censusStmt(s->thenB, c);
      censusStmt(s->elseB, c);
      break;
    case Stmt::WHILE:
      censusExpr(s->cond, c);
      censusStmt(s->body, c);
      break;
    case Stmt::FOR:
      ++c->decls[s->ivar];
      censusExpr(s->forInit, c);
      censusExpr(s->forCond, c);
      censusExpr(s->forStep, c);
      censusExpr(s->rangeLo, c);
      censusExpr(s->rangeHi, c);
      censusStmt(s->body, c);
      break;
    case Stmt::PARFOR:
      ++c->decls[s->pvar];
      censusExpr(s->plo, c);
      censusExpr(s->phi, c);
      for (auto& cl : s->clauses) censusExpr(cl.e, c);
      censusStmt(s->body, c);
      break;
    case Stmt::MATCH:
      censusExpr(s->matchOn, c);
      for (auto& arm : s->matchArms) {
        for (auto& b : arm.bindings) ++c->decls[b];
        censusStmt(arm.body, c);
      }
      break;
    case Stmt::BLOCK:
      for (auto* st : s->stmts) censusStmt(st, c);
      break;
    default: break;
  }
}

static void buildCensus(FnDecl* fn, Census* c) {
  for (auto& p : fn->params) {
    ++c->decls[p.name];
    c->params.insert(p.name);
    if (p.owned || p.borrowed) { /* modes do not affect the census */ }
  }
  censusStmt(fn->body, c);
}

// is `name` (as used by an IDENT node with a VAR/PARAM sym and no
// globalRef) a globally unique local in this fn?
static bool uniqueLocal(const Census& c, const std::string& name) {
  auto it = c.decls.find(name);
  return it != c.decls.end() && it->second == 1;
}

static bool globalExists(Program* p, const std::string& name) {
  for (auto* g : p->globals)
    if (g->name == name) return true;
  return false;
}

// --------------------------------------------------------- generic walking
static void walkExprNodesM(Expr* e, const std::function<void(Expr*)>& f) {
  if (!e) return;
  f(e);
  for (auto* a : e->args) walkExprNodesM(a, f);
  walkExprNodesM(e->lhs, f);
  walkExprNodesM(e->rhs, f);
  walkExprNodesM(e->base, f);
}

// root IDENT of an lvalue/rvalue chain (through INDEX/FIELD), or null
static Expr* rootOfChainM(Expr* e) {
  while (e && (e->k == Expr::INDEX || e->k == Expr::FIELD)) e = e->base;
  return e;
}

// ---------------------------------------------------------------- mem-reuse
// is `e` a `malloc(T, n)` / `soa_new(S, n)` call?
static bool isAllocCall(Expr* e, std::string* kind, Expr** countArg) {
  if (!e || e->k != Expr::CALL || e->callee) return false;
  if (e->name != "malloc" && e->name != "soa_new") return false;
  if (e->args.size() != 2) return false;
  *kind = e->name;
  *countArg = e->args[1];
  return true;
}

// is `s` an EXPR/DEFER statement that is exactly free(p)/soa_free(p) on
// the bare local `name`?
static bool isFreeStmt(Stmt* s, const std::string& name, Expr** freeArg) {
  Expr* e = nullptr;
  if (s->k == Stmt::EXPR) e = s->expr;
  else if (s->k == Stmt::DEFER) e = s->deferExpr;
  if (!e || e->k != Expr::CALL || e->callee) return false;
  if (e->name != "free" && e->name != "soa_free") return false;
  if (e->args.size() != 1) return false;
  Expr* a = e->args[0];
  if (!a || a->k != Expr::IDENT || a->name != name) return false;
  if (a->globalRef) return false;
  *freeArg = a;
  return true;
}

// every way the buffer `name` may be touched inside `body`:
//   DIRECT index accesses and soa_len are fine; anything else vetoes.
// `allowedFree` points at the one free() statement that is expected.
static bool bufferAccessesConfined(Stmt* body, const std::string& name,
                                   Stmt* allowedFree,
                                   bool* sawMatchingWrite,
                                   std::set<std::string>* writtenIdx) {
  struct V {
    bool bad = false;
    bool sawWrite = false;
    std::set<std::string> writes;
    const std::string& name;
    Stmt* allowedFree;

    void expr(Expr* e, bool lvalueRoot) {
      if (!e || bad) return;
      if (e->k == Expr::IDENT) {
        if (e->name == name && !e->globalRef && e->sym &&
            (e->sym->k == Sym::VAR || e->sym->k == Sym::PARAM)) {
          // bare mention outside a free arg / index base = escape
          if (!lvalueRoot) bad = true;
        }
        return;
      }
      if (e->k == Expr::INDEX || e->k == Expr::FIELD) {
        Expr* root = rootOfChainM(e);
        if (root && root->k == Expr::IDENT && root->name == name &&
            !root->globalRef) {
          if (e->k == Expr::FIELD && e->ptrDeref) { bad = true; return; }
          if (lvalueRoot) {
            // write through b[...]: record the canonical index
            Expr* node = e;
            while (node && node->k == Expr::FIELD) node = node->base;
            if (node && node->k == Expr::INDEX && node->rhs) {
              writes.insert(canon(node->rhs));
              sawWrite = true;
            } else {
              bad = true;   // whole-struct write through b — not confined
              return;
            }
          }
          expr(e->rhs, false);
          return;   // base handled by the root check above
        }
        // fallthrough: keep walking children
        expr(e->base, lvalueRoot);
        expr(e->rhs, false);
        return;
      }
      if (e->k == Expr::CALL) {
        // soa_len(b) reads the descriptor only — allowed
        if (!e->callee && e->name == "soa_len" && e->args.size() == 1 &&
            e->args[0]->k == Expr::IDENT && e->args[0]->name == name) {
          return;
        }
        for (auto* a : e->args) expr(a, false);
        return;
      }
      for (auto* a : e->args) expr(a, false);
      expr(e->lhs, false);
      expr(e->rhs, false);
      expr(e->base, false);
    }
    void stmt(Stmt* s) {
      if (!s || bad) return;
      switch (s->k) {
        case Stmt::LET:
          // `let c = b` (bare alias) leaks the pointer; `let c = b[0]`
          // is a plain element read and stays allowed via expr()
          expr(s->init, false);
          break;
        case Stmt::ASSIGN: {
          Expr* root = rootOfChainM(s->lhs);
          if (root && root->k == Expr::IDENT && root->name == name &&
              !root->globalRef) {
            if (s->op2 != "=") { bad = true; return; }  // b[i] += .. (reads!)
            expr(s->lhs, true);
          } else {
            expr(s->lhs, false);
          }
          expr(s->rhs, false);
          break;
        }
        case Stmt::RETURN: expr(s->retVal, false); break;
        case Stmt::DEFER:
          if (s != allowedFree) expr(s->deferExpr, false);
          break;
        case Stmt::EXPR:
          if (s != allowedFree) expr(s->expr, false);
          break;
        case Stmt::IF:
          expr(s->cond, false);
          stmt(s->thenB);
          stmt(s->elseB);
          break;
        case Stmt::WHILE:
          expr(s->cond, false);
          stmt(s->body);
          break;
        case Stmt::FOR:
          expr(s->forInit, false);
          expr(s->forCond, false);
          expr(s->forStep, false);
          expr(s->rangeLo, false);
          expr(s->rangeHi, false);
          stmt(s->body);
          break;
        case Stmt::PARFOR:
          expr(s->plo, false);
          expr(s->phi, false);
          for (auto& cl : s->clauses) expr(cl.e, false);
          stmt(s->body);
          break;
        case Stmt::MATCH:
          expr(s->matchOn, false);
          for (auto& arm : s->matchArms) stmt(arm.body);
          break;
        case Stmt::BLOCK:
          for (auto* st : s->stmts) stmt(st);
          break;
        default: break;
      }
    }
  };
  V v{false, false, {}, name, allowedFree};
  v.stmt(body);
  *sawMatchingWrite = v.sawWrite;
  *writtenIdx = v.writes;
  return !v.bad;
}

// read-index sanity gate (QUALITY gate, not soundness — see the header
// contract): every DIRECT read b[idx] must either (a) have some write
// b[idx'] with idx' == idx somewhere in the body, or (b) be covered by a
// nested loop whose ONLY b-accesses are DIRECT writes (a fill loop).
// Order is deliberately NOT required: reading an element before its
// write consumes unspecified memory in BOTH forms (fresh malloc is not
// zeroed), so defined behaviour — a function of WRITTEN contents — is
// preserved either way.  The gate exists to catch pure-garbage shapes
// (reads of elements nobody ever writes) where reuse would silently
// change which garbage is observed across iterations.
static bool readsHaveMatchingWrites(Stmt* body, const std::string& name) {
  std::set<std::string> readsIdx, writesIdx;
  bool fillLoop = false;
  // per-subtree classification: does this statement subtree read b or
  // write b outside of a DIRECT index?
  struct Acc { bool read = false; bool write = false; bool odd = false; };
  std::function<Acc(Stmt*)> classify = [&](Stmt* s) -> Acc {
    Acc a;
    if (!s) return a;
    switch (s->k) {
      case Stmt::LET: case Stmt::RETURN: case Stmt::DEFER:
      case Stmt::EXPR: case Stmt::IF: case Stmt::WHILE:
      case Stmt::MATCH: {
        std::vector<Expr*> exprs;
        if (s->k == Stmt::LET) exprs.push_back(s->init);
        if (s->k == Stmt::RETURN) exprs.push_back(s->retVal);
        if (s->k == Stmt::DEFER) exprs.push_back(s->deferExpr);
        if (s->k == Stmt::EXPR) exprs.push_back(s->expr);
        if (s->k == Stmt::IF || s->k == Stmt::WHILE)
          exprs.push_back(s->cond);
        if (s->k == Stmt::MATCH) exprs.push_back(s->matchOn);
        for (Expr* e : exprs)
          walkExprNodesM(e, [&](Expr* x) {
            if (x->k != Expr::INDEX && x->k != Expr::FIELD) return;
            Expr* root = rootOfChainM(x);
            if (!root || root->k != Expr::IDENT || root->name != name)
              return;
            Expr* node = x;
            while (node && node->k == Expr::FIELD) node = node->base;
            if (node && node->k == Expr::INDEX && node->rhs)
              readsIdx.insert(canon(node->rhs));
            else
              a.odd = true;
            a.read = true;
          });
        if (s->k == Stmt::IF) {
          Acc t = classify(s->thenB), e2 = classify(s->elseB);
          a.read |= t.read || e2.read;
          a.write |= t.write || e2.write;
          a.odd |= t.odd || e2.odd;
        }
        if (s->k == Stmt::WHILE) {
          Acc b = classify(s->body);
          a.read |= b.read; a.write |= b.write; a.odd |= b.odd;
        }
        if (s->k == Stmt::MATCH)
          for (auto& arm : s->matchArms) {
            Acc b = classify(arm.body);
            a.read |= b.read; a.write |= b.write; a.odd |= b.odd;
          }
        return a;
      }
      case Stmt::ASSIGN: {
        walkExprNodesM(s->lhs, [&](Expr* x) {
          if (x->k != Expr::INDEX && x->k != Expr::FIELD) return;
          Expr* root = rootOfChainM(x);
          if (!root || root->k != Expr::IDENT || root->name != name)
            return;
          Expr* node = x;
          while (node && node->k == Expr::FIELD) node = node->base;
          if (node && node->k == Expr::INDEX && node->rhs)
            writesIdx.insert(canon(node->rhs));
          else
            a.odd = true;
          a.write = true;
        });
        walkExprNodesM(s->rhs, [&](Expr* x) {
          if (x->k != Expr::INDEX && x->k != Expr::FIELD) return;
          Expr* root = rootOfChainM(x);
          if (!root || root->k != Expr::IDENT || root->name != name)
            return;
          Expr* node = x;
          while (node && node->k == Expr::FIELD) node = node->base;
          if (node && node->k == Expr::INDEX && node->rhs)
            readsIdx.insert(canon(node->rhs));
          else
            a.odd = true;
          a.read = true;
        });
        return a;
      }
      case Stmt::FOR: case Stmt::PARFOR: {
        Acc b = classify(s->body);
        // a loop whose b-accesses are writes only (and nothing odd) is
        // a FILL loop: every element it touches is (re)written each
        // iteration; reads elsewhere are considered covered
        if (b.write && !b.read && !b.odd) fillLoop = true;
        a.read |= b.read; a.write |= b.write; a.odd |= b.odd;
        return a;
      }
      case Stmt::BLOCK: {
        for (auto* st : s->stmts) {
          Acc b = classify(st);
          a.read |= b.read; a.write |= b.write; a.odd |= b.odd;
        }
        return a;
      }
      default: return a;
    }
  };
  classify(body);
  for (auto& r : readsIdx)
    if (!writesIdx.count(r) && !fillLoop) return false;
  return true;
}

// Pattern L: try to hoist one alloc/free churn out of the loop at
// list[li].  Returns true when the rewrite was applied.
static bool tryLoopChurn(std::vector<Stmt*>& list, size_t li, FnDecl* fn,
                         const Census& cens, Program* prog) {
  Stmt* loop = list[li];
  if (loop->k != Stmt::FOR && loop->k != Stmt::WHILE) return false;
  std::vector<Stmt*>* bodyList = stmtsOf(loop->body);
  if (!bodyList || bodyList->empty()) return false;

  // candidate: a top-level LET whose init is malloc/soa_new
  size_t letPos = (size_t)-1;
  Stmt* letStmt = nullptr;
  std::string allocKind;
  Expr* countArg = nullptr;
  for (size_t k = 0; k < bodyList->size(); ++k) {
    Stmt* s = (*bodyList)[k];
    if (s->k != Stmt::LET || !s->init) continue;
    std::string kind;
    Expr* cnt = nullptr;
    if (!isAllocCall(s->init, &kind, &cnt)) continue;
    if (!uniqueLocal(cens, s->var)) continue;   // shadowed/redeclared
    if (globalExists(prog, s->var)) continue;
    letPos = k;
    letStmt = s;
    allocKind = kind;
    countArg = cnt;
    break;
  }
  if (!letStmt) return false;
  const std::string buf = letStmt->var;

  // the matching free: a top-level EXPR/DEFER free(buf) after the LET
  size_t freePos = (size_t)-1;
  Stmt* freeStmt = nullptr;
  Expr* freeArg = nullptr;
  (void)freeArg;   // debug aid: the arg node is reused in the rewrite
  for (size_t k = letPos + 1; k < bodyList->size(); ++k) {
    Stmt* s = (*bodyList)[k];
    Expr* arg = nullptr;
    if (isFreeStmt(s, buf, &arg)) {
      freePos = k;
      freeStmt = s;
      freeArg = arg;
      break;
    }
  }
  if (!freeStmt) return false;
  // the free must target the same allocator family as the alloc
  {
    const std::string& freeName = freeStmt->k == Stmt::EXPR
                                      ? freeStmt->expr->name
                                      : freeStmt->deferExpr->name;
    const std::string wantFree = allocKind == "malloc" ? "free" : "soa_free";
    if (freeName != wantFree) return false;
  }

  // no reassignment of the buffer POINTER anywhere in the body (writes
  // THROUGH it — buf[j] = ... — are the normal use and stay allowed)
  bool rebound = false;
  std::function<void(Stmt*)> scanRebind = [&](Stmt* s) {
    if (!s || rebound) return;
    if (s->k == Stmt::ASSIGN && s->lhs && s->lhs->k == Expr::IDENT &&
        s->lhs->name == buf)
      rebound = true;
    for (auto* st : s->stmts) scanRebind(st);
    scanRebind(s->body); scanRebind(s->thenB); scanRebind(s->elseB);
    if (s->k == Stmt::MATCH)
      for (auto& arm : s->matchArms) scanRebind(arm.body);
  };
  scanRebind(loop->body);
  if (rebound) return false;

  // escape confinement + matching write census
  bool sawWrite = false;
  std::set<std::string> writtenIdx;
  if (!bufferAccessesConfined(loop->body, buf, freeStmt, &sawWrite,
                              &writtenIdx))
    return false;
  if (!sawWrite) return false;   // buffer never even written — no churn win

  // read/write index sanity gate
  if (!readsHaveMatchingWrites(loop->body, buf)) return false;

  // loop-wide effect vetoes: give / unknown memory (unknown calls could
  // retain or observe the buffer through captured state)
  BodyInfo info;
  collectBody(loop->body, loop->k == Stmt::FOR ? loop->ivar : "", fn, &info);
  if (info.hasGive || info.unknownWrite || info.unknownRead) return false;

  // allocation count must be loop-invariant
  std::set<std::string> cntNames;
  collectIdents(countArg, cntNames);
  std::set<std::string> wr = writeRoots(info);
  if (loop->k == Stmt::FOR) wr.insert(loop->ivar);
  for (auto& n : cntNames)
    if (wr.count(n)) return false;

  // ---- rewrite
  // 1. drop the free statement (and the LET) from the body
  bodyList->erase(bodyList->begin() + freePos);
  bodyList->erase(bodyList->begin() + letPos);
  // 2. re-insert the LET before the loop, and a free after it
  list.insert(list.begin() + li, letStmt);
  letStmt->parent = loop->parent;
  size_t loopPos = li + 1;
  Stmt* fin = new Stmt(Stmt::EXPR, freeStmt->loc);
  fin->fn = fn;
  fin->parent = loop->parent;
  fin->expr = freeStmt->k == Stmt::EXPR ? freeStmt->expr
                                        : freeStmt->deferExpr;
  list.insert(list.begin() + loopPos + 1, fin);
  delete freeStmt;   // the wrapper node (its expr moved into `fin`)

  diag(Diag::Note, Code::MemReuse,
       "mem-reuse: hoisted the per-iteration " + allocKind + " of '" + buf +
       "' out of the loop (line " + std::to_string(loop->loc.line) +
       ", fn " + fn->name + ") — one allocation + one free instead of one "
       "per iteration; the buffer is provably confined to each iteration "
       "(direct indexing only, no escapes)",
       loop->loc,
       "kind=loop-churn,fn=" + fn->name + ",var=" + buf);
  proofLogLine("mem-reuse",
               "loop-churn: buffer '" + buf + "' confined (direct accesses "
               "only, no escape/give/unknown), count loop-invariant, "
               "reads have matching writes, sequential loop (not parfor)",
               fn, loop->loc);
  return true;
}

// Pattern S: free(p) immediately followed by LET q = malloc(same shape).
static bool tryFreeAllocFuse(std::vector<Stmt*>& list, FnDecl* fn,
                             const Census& cens, Program* prog) {
  for (size_t i = 0; i + 1 < list.size(); ++i) {
    Stmt* fr = list[i];
    if (fr->k != Stmt::EXPR) continue;
    Expr* fe = fr->expr;
    if (!fe || fe->k != Expr::CALL || fe->callee) continue;
    if (fe->name != "free" && fe->name != "soa_free") continue;
    if (fe->args.size() != 1 || !fe->args[0] ||
        fe->args[0]->k != Expr::IDENT) continue;
    Expr* pArg = fe->args[0];
    if (pArg->globalRef || !uniqueLocal(cens, pArg->name)) continue;
    const std::string p = pArg->name;

    Stmt* qLet = list[i + 1];
    if (qLet->k != Stmt::LET || qLet->mut || !qLet->init) continue;
    std::string kind;
    Expr* cnt = nullptr;
    if (!isAllocCall(qLet->init, &kind, &cnt)) continue;
    if (!uniqueLocal(cens, qLet->var)) continue;
    if (globalExists(prog, qLet->var)) continue;
    if ((kind == "malloc") != (fe->name == "free")) continue;

    // p's own declaration must be an alloc of canonically identical shape
    Stmt* pDecl = nullptr;
    for (size_t k = 0; k < list.size(); ++k) {
      Stmt* s = list[k];
      if (s->k == Stmt::LET && s->var == p && s->init) { pDecl = s; break; }
    }
    if (!pDecl || pDecl->k != Stmt::LET || !pDecl->init) continue;
    std::string pKind;
    Expr* pCnt = nullptr;
    if (!isAllocCall(pDecl->init, &pKind, &pCnt)) continue;
    if (pKind != kind) continue;
    if (!sameExpr(pCnt, cnt)) continue;
    if (pDecl->init->args[0]->k == Expr::IDENT &&
        qLet->init->args[0]->k == Expr::IDENT &&
        pDecl->init->args[0]->name != qLet->init->args[0]->name)
      continue;
    if (!sameExpr(pDecl->init->args[0], qLet->init->args[0])) continue;

    // q's accesses from here on must stay confined; q's own free in
    // the tail (if any) is the expected statement
    bool sawWrite = false;
    std::set<std::string> writtenIdx;
    Stmt* qFree = nullptr;
    for (size_t k = i + 1; k < list.size(); ++k) {
      Expr* fa = nullptr;
      if (isFreeStmt(list[k], qLet->var, &fa)) { qFree = list[k]; break; }
    }
    Stmt restBody(Stmt::BLOCK, qLet->loc);
    // walk the tail without copying: temporarily point the BLOCK at the
    // remaining statements (cleared before it goes out of scope)
    restBody.stmts.assign(list.begin() + i + 1, list.end());
    bool confined = bufferAccessesConfined(&restBody, qLet->var, qFree,
                                           &sawWrite, &writtenIdx);
    restBody.stmts.clear();
    if (!confined) continue;

    // ---- rewrite: drop the free, q aliases p
    qLet->init = pArg;   // `let q = p;` — mov-min will forward it
    list.erase(list.begin() + i);
    diag(Diag::Note, Code::MemReuse,
         "mem-reuse: fused free(" + p + ") with the following identical " +
         kind + " (line " + std::to_string(fr->loc.line) + ", fn " +
         fn->name + ") — the freed block is reused for '" + qLet->var +
         "' instead of returning it to the allocator",
         fr->loc,
         "kind=free-alloc-fuse,fn=" + fn->name + ",var=" + p);
    proofLogLine("mem-reuse",
                 "free-alloc-fuse: identical type+count, adjacent, q "
                 "immutable and confined (direct accesses only)",
                 fn, fr->loc);
    return true;
  }
  return false;
}

// ---------------------------------------------------------------- mov-min
// replace every IDENT denoting `from` with a fresh clone of `tmpl`
static void redirectExpr(Expr*& e, const std::string& from,
                         const Expr* tmpl) {
  if (!e) return;
  if (e->k == Expr::IDENT && e->name == from && !e->globalRef && e->sym &&
      (e->sym->k == Sym::VAR || e->sym->k == Sym::PARAM)) {
    Expr* n = new Expr(Expr::IDENT, tmpl->loc);
    n->name = tmpl->name;
    n->ty = tmpl->ty;
    n->sym = tmpl->sym;
    n->constant = tmpl->constant;
    n->isLvalue = false;   // mov-forwarded uses are reads (writes vetoed)
    e = n;
    return;
  }
  for (auto*& a : e->args) redirectExpr(a, from, tmpl);
  redirectExpr(e->lhs, from, tmpl);
  redirectExpr(e->rhs, from, tmpl);
  redirectExpr(e->base, from, tmpl);
}

static void redirectStmt(Stmt* s, const std::string& from, const Expr* tmpl) {
  if (!s) return;
  switch (s->k) {
    case Stmt::LET: redirectExpr(s->init, from, tmpl); break;
    case Stmt::ASSIGN:
      redirectExpr(s->lhs, from, tmpl);
      redirectExpr(s->rhs, from, tmpl);
      break;
    case Stmt::RETURN: redirectExpr(s->retVal, from, tmpl); break;
    case Stmt::DEFER:  redirectExpr(s->deferExpr, from, tmpl); break;
    case Stmt::EXPR:   redirectExpr(s->expr, from, tmpl); break;
    case Stmt::IF:
      redirectExpr(s->cond, from, tmpl);
      redirectStmt(s->thenB, from, tmpl);
      redirectStmt(s->elseB, from, tmpl);
      break;
    case Stmt::WHILE:
      redirectExpr(s->cond, from, tmpl);
      redirectStmt(s->body, from, tmpl);
      break;
    case Stmt::FOR:
      redirectExpr(s->forInit, from, tmpl);
      redirectExpr(s->forCond, from, tmpl);
      redirectExpr(s->forStep, from, tmpl);
      redirectExpr(s->rangeLo, from, tmpl);
      redirectExpr(s->rangeHi, from, tmpl);
      redirectStmt(s->body, from, tmpl);
      break;
    case Stmt::PARFOR:
      redirectExpr(s->plo, from, tmpl);
      redirectExpr(s->phi, from, tmpl);
      for (auto& cl : s->clauses) redirectExpr(cl.e, from, tmpl);
      redirectStmt(s->body, from, tmpl);
      break;
    case Stmt::MATCH:
      redirectExpr(s->matchOn, from, tmpl);
      for (auto& arm : s->matchArms) redirectStmt(arm.body, from, tmpl);
      break;
    case Stmt::BLOCK:
      for (auto* st : s->stmts) redirectStmt(st, from, tmpl);
      break;
    default: break;
  }
}

// count IDENT uses of a unique local in a subtree
static int countUses(Stmt* body, const std::string& name) {
  int n = 0;
  std::function<void(Expr*)> walkE = [&](Expr* e) {
    if (!e) return;
    if (e->k == Expr::IDENT && e->name == name && !e->globalRef && e->sym &&
        (e->sym->k == Sym::VAR || e->sym->k == Sym::PARAM))
      ++n;
    for (auto* a : e->args) walkE(a);
    walkE(e->lhs); walkE(e->rhs); walkE(e->base);
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
        walkE(s->forInit); walkE(s->forCond); walkE(s->forStep);
        walkE(s->rangeLo); walkE(s->rangeHi); walkS(s->body);
        break;
      case Stmt::PARFOR:
        walkE(s->plo); walkE(s->phi);
        for (auto& cl : s->clauses) walkE(cl.e);
        walkS(s->body);
        break;
      case Stmt::MATCH:
        walkE(s->matchOn);
        for (auto& arm : s->matchArms) walkS(arm.body);
        break;
      case Stmt::BLOCK:
        for (auto* st : s->stmts) walkS(st);
        break;
      default: break;
    }
  };
  walkS(body);
  return n;
}

// any assignment whose lvalue root is one of `names`?
static bool anyWriteThrough(Stmt* body, const std::set<std::string>& names) {
  bool found = false;
  std::function<void(Stmt*)> walk = [&](Stmt* s) {
    if (!s || found) return;
    if (s->k == Stmt::ASSIGN) {
      Expr* root = rootOfChainM(s->lhs);
      if (root && root->k == Expr::IDENT && !root->globalRef &&
          names.count(root->name))
        found = true;
    }
    for (auto* st : s->stmts) walk(st);
    walk(s->body); walk(s->thenB); walk(s->elseB);
    if (s->k == Stmt::MATCH)
      for (auto& arm : s->matchArms) walk(arm.body);
  };
  walk(body);
  return found;
}

// any call that could observe/retain the binding's storage?
//   - a bare IDENT argument passes the WHOLE aggregate by address
//     (the by-address ABI hands the callee the aliased storage) — veto;
//   - a give-marked argument transfers ownership — veto;
//   - field/index chains rooted at the binding (`print_f64(b.x, 2)`)
//     load a scalar and pass a VALUE — safe.
static bool anyCallMentioning(Stmt* body, const std::set<std::string>& names,
                              bool ignoreGiveMarkers) {
  bool found = false;
  std::function<void(Stmt*)> walk = [&](Stmt* s) {
    if (!s || found) return;
    std::vector<Expr*> exprs;
    if (s->k == Stmt::LET) exprs.push_back(s->init);
    else if (s->k == Stmt::ASSIGN) { exprs.push_back(s->lhs); exprs.push_back(s->rhs); }
    else if (s->k == Stmt::RETURN) exprs.push_back(s->retVal);
    else if (s->k == Stmt::DEFER) exprs.push_back(s->deferExpr);
    else if (s->k == Stmt::EXPR) exprs.push_back(s->expr);
    else if (s->k == Stmt::IF) { exprs.push_back(s->cond); walk(s->thenB); walk(s->elseB); }
    else if (s->k == Stmt::WHILE) { exprs.push_back(s->cond); walk(s->body); }
    else if (s->k == Stmt::FOR) {
      exprs.push_back(s->forInit); exprs.push_back(s->forCond);
      exprs.push_back(s->forStep); exprs.push_back(s->rangeLo);
      exprs.push_back(s->rangeHi);
      walk(s->body);
    } else if (s->k == Stmt::PARFOR) {
      exprs.push_back(s->plo); exprs.push_back(s->phi);
      for (auto& cl : s->clauses) exprs.push_back(cl.e);
      walk(s->body);
    } else if (s->k == Stmt::MATCH) {
      exprs.push_back(s->matchOn);
      for (auto& arm : s->matchArms) walk(arm.body);
    } else if (s->k == Stmt::BLOCK) {
      for (auto* st : s->stmts) walk(st);
    }
    for (Expr* e : exprs)
      walkExprNodesM(e, [&](Expr* x) {
        if (found || x->k != Expr::CALL) return;
        for (size_t j = 0; j < x->args.size() && !found; ++j) {
          Expr* arg = x->args[j];
          bool give = j < x->argGive.size() && x->argGive[j];
          if (give && ignoreGiveMarkers) continue;
          bool bare = arg && arg->k == Expr::IDENT && !arg->globalRef &&
                      names.count(arg->name);
          bool giveMentions = false;
          if (give) {
            walkExprNodesM(arg, [&](Expr* y) {
              if (y->k == Expr::IDENT && !y->globalRef &&
                  names.count(y->name))
                giveMentions = true;
            });
          }
          if (bare || giveMentions) found = true;
        }
      });
  };
  walk(body);
  return found;
}

// one mov-minimise round over a statement list; returns rewrites applied
static int movMinList(std::vector<Stmt*>& list, FnDecl* fn, const Census& cens,
                      Program* prog) {
  int applied = 0;

  // ---- 1. self-assignments (x = x, s.f = s.f on non-pointer locals)
  for (size_t i = 0; i < list.size(); ++i) {
    Stmt* s = list[i];
    if (s->k != Stmt::ASSIGN || s->op2 != "=" || !s->lhs || !s->rhs)
      continue;
    if (!sameExpr(s->lhs, s->rhs)) continue;
    // no calls anywhere in either side (a call would be removed too)
    bool hasCall = false;
    walkExprNodesM(s->lhs, [&](Expr* x) { if (x->k == Expr::CALL) hasCall = true; });
    if (hasCall) continue;
    // root must be a local slot, never a pointer dereference (which
    // could trap on null) — aggregate slots (memcpy-to-self) are fine
    Expr* root = rootOfChainM(s->lhs);
    if (!root || root->k != Expr::IDENT || root->globalRef) continue;
    // an INDEX anywhere in the chain can trap (bounds checks / null
    // bases), so only IDENT and pure FIELD chains are removable
    bool idxInChain = false;
    for (Expr* n = s->lhs; n; n = n->base) {
      if (n->k != Expr::INDEX && n->k != Expr::FIELD) break;
      if (n->k == Expr::INDEX) { idxInChain = true; break; }
    }
    if (idxInChain) continue;
    if (root->ty && (root->ty->isPtr() || root->ty->k == Type::SOA)) {
      // b = b (pointer/soa self-copy) is still a pure local load+store
      if (s->lhs->k != Expr::IDENT || s->rhs->k != Expr::IDENT) continue;
    }
    list.erase(list.begin() + i);
    --i;
    ++applied;
    diag(Diag::Note, Code::MovMin,
         "mov-min: removed self-assignment (line " +
         std::to_string(s->loc.line) + ", fn " + fn->name +
         ") — a load+store (memcpy for aggregates) back into the same slot",
         s->loc, "kind=self-assign,fn=" + fn->name);
    proofLogLine("mov-min", "self-assign: identical lhs/rhs, no calls, "
                 "local slot root", fn, s->loc);
  }

  // ---- 2/3. mov forwarding: `let a = b` with a immutable, b a local
  for (size_t i = 0; i < list.size(); ++i) {
    Stmt* let = list[i];
    if (let->k != Stmt::LET || !let->init) continue;
    if (let->init->k != Expr::IDENT) continue;
    Expr* src = let->init;
    if (src->globalRef || !src->sym ||
        !(src->sym->k == Sym::VAR || src->sym->k == Sym::PARAM))
      continue;
    if (src->name == let->var) continue;
    if (let->mut) continue;
    if (!uniqueLocal(cens, let->var) || !uniqueLocal(cens, src->name))
      continue;
    if (globalExists(prog, let->var) || globalExists(prog, src->name))
      continue;
    if (!let->init->ty) continue;

    Type* ty = let->init->ty;
    bool aggregate = ty->k == Type::STRUCT || ty->k == Type::ENUM ||
                     ty->k == Type::ARRAY || ty->k == Type::SOA ||
                     ty->k == Type::SPAN || ty->k == Type::STR;
    bool pairLike = ty->k == Type::SPAN || ty->k == Type::STR;

    if (aggregate) {
      // no writes through either binding, anywhere in the fn
      std::set<std::string> both{let->var, src->name};
      if (anyWriteThrough(fn->body, both)) continue;
      // by-address ABI types may never reach a call; borrowed pair
      // values may (non-owning argument positions only)
      bool addrABI = !pairLike;
      if (addrABI && anyCallMentioning(fn->body, both, false)) continue;
      if (!addrABI && anyCallMentioning(fn->body, both, true)) continue;
    }

    int uses = countUses(fn->body, let->var);
    // (uses counted over the whole fn: the binding is unique, so every
    //  use is this LET's)
    if (uses > 0)
      redirectStmt(fn->body, let->var, src);
    list.erase(list.begin() + i);
    --i;
    ++applied;
    diag(Diag::Note, Code::MovMin,
         "mov-min: " + std::string(uses == 0 ? "removed dead mov" : "forwarded mov") +
         " `let " + let->var + " = " + src->name + "` (line " +
         std::to_string(let->loc.line) + ", fn " + fn->name + ") — " +
         std::to_string(uses) + " use(s) now read the source binding "
         "directly; the copy" +
         std::string(ty->isAggregate() ? " (a whole-aggregate memcpy)" : "") +
         " disappears before the backend sees it",
         let->loc,
         "kind=mov-forward,fn=" + fn->name + ",uses=" + std::to_string(uses));
    proofLogLine("mov-min",
                 "mov-forward: unique immutable bindings" +
                 std::string(aggregate ? ", aggregate: no writes through "
                  "either, no (owning) call mentions" : "") +
                 ", no shadowing globals",
                 fn, let->loc);
  }
  return applied;
}

// ---------------------------------------------------------- smt interleave
// memory/compute census class: 1 = memory-bound, 2 = compute-bound,
// 3 = balanced, 0 = trivial/unknown
void walkExprNodesStmt(Stmt* s, const std::function<void(Expr*)>& f);

int classifyParforMemClass(Stmt* body) {
  int64_t mem = 0, ops = 0;
  walkExprNodesStmt(body, [&](Expr* x) {
    if (x->k == Expr::INDEX || (x->k == Expr::FIELD && x->ptrDeref)) ++mem;
    else if (x->k == Expr::BIN && x->op != "==" && x->op != "!=" &&
             x->op != "<" && x->op != ">" && x->op != "<=" && x->op != ">=")
      ++ops;
  });
  if (mem + ops == 0) return 0;
  if (mem * 2 > ops + 1) return 1;
  if (ops > mem * 3 + 2) return 2;
  return 3;
}

} // namespace memstage

using namespace optutil;   // the stage drivers below live at freshet scope

// walk every statement list in a subtree (shared by all sub-passes)
static void walkLists(std::vector<Stmt*>& list, FnDecl* fn,
                      const std::function<void(std::vector<Stmt*>&)>& f) {
  f(list);
  for (Stmt* s : list) {
    switch (s->k) {
      case Stmt::BLOCK:
        walkLists(s->stmts, fn, f);
        break;
      case Stmt::IF:
        if (s->thenB && s->thenB->k == Stmt::BLOCK)
          walkLists(s->thenB->stmts, fn, f);
        if (s->elseB && s->elseB->k == Stmt::BLOCK)
          walkLists(s->elseB->stmts, fn, f);
        break;
      case Stmt::WHILE: case Stmt::FOR: case Stmt::PARFOR:
        if (s->body && s->body->k == Stmt::BLOCK)
          walkLists(s->body->stmts, fn, f);
        break;
      case Stmt::MATCH:
        for (auto& arm : s->matchArms)
          if (arm.body && arm.body->k == Stmt::BLOCK)
            walkLists(arm.body->stmts, fn, f);
        break;
      default: break;
    }
  }
}

// expression walker over a whole statement subtree (helper for the
// census used by classifyParforMemClass)
namespace memstage {
void walkExprNodesStmt(Stmt* s, const std::function<void(Expr*)>& f) {
  if (!s) return;
  switch (s->k) {
    case Stmt::LET: walkExprNodesM(s->init, f); break;
    case Stmt::ASSIGN: walkExprNodesM(s->lhs, f); walkExprNodesM(s->rhs, f); break;
    case Stmt::RETURN: walkExprNodesM(s->retVal, f); break;
    case Stmt::DEFER: walkExprNodesM(s->deferExpr, f); break;
    case Stmt::EXPR: walkExprNodesM(s->expr, f); break;
    case Stmt::IF:
      walkExprNodesM(s->cond, f);
      walkExprNodesStmt(s->thenB, f);
      walkExprNodesStmt(s->elseB, f);
      break;
    case Stmt::WHILE:
      walkExprNodesM(s->cond, f);
      walkExprNodesStmt(s->body, f);
      break;
    case Stmt::FOR:
      walkExprNodesM(s->forInit, f); walkExprNodesM(s->forCond, f);
      walkExprNodesM(s->forStep, f); walkExprNodesM(s->rangeLo, f);
      walkExprNodesM(s->rangeHi, f);
      walkExprNodesStmt(s->body, f);
      break;
    case Stmt::PARFOR:
      walkExprNodesM(s->plo, f); walkExprNodesM(s->phi, f);
      for (auto& cl : s->clauses) walkExprNodesM(cl.e, f);
      walkExprNodesStmt(s->body, f);
      break;
    case Stmt::MATCH:
      walkExprNodesM(s->matchOn, f);
      for (auto& arm : s->matchArms) walkExprNodesStmt(arm.body, f);
      break;
    case Stmt::BLOCK:
      for (auto* st : s->stmts) walkExprNodesStmt(st, f);
      break;
    default: break;
  }
}
} // namespace memstage

// ------------------------------------------------------------ stage driver
MemStageCounts runMemStagePasses(Program* p) {
  MemStageCounts counts;
  if (!g_opts.memReuseOn && !g_opts.movMinOn) return counts;
  computeFootprints(p);
  for (auto* fn : p->fns) {
    if (!fn->body || fn->isGeneric) continue;
    memstage::Census cens;
    memstage::buildCensus(fn, &cens);

    // ---- mem-reuse (pattern L + pattern S)
    if (g_opts.memReuseOn) {
      for (int round = 0; round < 4; ++round) {
        int before = counts.memReuse;
        walkLists(fn->body->stmts, fn, [&](std::vector<Stmt*>& list) {
          for (size_t i = 0; i < list.size(); ++i)
            if (memstage::tryLoopChurn(list, i, fn, cens, p))
              ++counts.memReuse;
            // no ++i: the list grew by two, the hoisted LET is a no-op
            // for the scan, and the processed loop no longer matches
        });
        walkLists(fn->body->stmts, fn, [&](std::vector<Stmt*>& list) {
          if (memstage::tryFreeAllocFuse(list, fn, cens, p))
            ++counts.memReuse;
        });
        if (counts.memReuse == before) break;
      }
    }

    // ---- mov-minimise (2 rounds for chained movs)
    if (g_opts.movMinOn) {
      for (int round = 0; round < 2; ++round) {
        int roundApplied = 0;
        walkLists(fn->body->stmts, fn, [&](std::vector<Stmt*>& list) {
          roundApplied += memstage::movMinList(list, fn, cens, p);
        });
        counts.movMin += roundApplied;
        if (roundApplied == 0) break;
      }
    }
  }
  return counts;
}

// ---------------------------------------------------------- smt pairing pass
int runSmtInterleavePass(Program* p) {
  if (!g_opts.smtInterleaveOn) return 0;
  computeFootprints(p);
  int pairs = 0;
  for (auto* fn : p->fns) {
    if (!fn->body || fn->isGeneric) continue;
    // walk every statement list; `insideParfor` tracks SMT-pair
    // eligibility: only OUTERMOST regions pair in v1 (a nested pair
    // would dispatch from inside an outer chunk fn — legal, but the
    // concurrency reasoning is subtle enough to stay conservative)
    std::function<void(std::vector<Stmt*>&, bool)> walkList =
        [&](std::vector<Stmt*>& list, bool insideParfor) {
      for (size_t i = 0; i + 1 < list.size(); ++i) {
        Stmt* A = list[i];
        Stmt* B = list[i + 1];
        if (A->k != Stmt::PARFOR || B->k != Stmt::PARFOR) continue;
        if (insideParfor) continue;   // outermost regions only (v1)
        // non-reproducible, not already paired, not a collapsed inner
        if (A->reproducible || B->reproducible) continue;
        if (A->collapsed || B->collapsed) continue;
        if (A->smtPairWith || B->smtPairWith || A->smtPaired || B->smtPaired)
          continue;

        // classify both bodies
        int clsA = memstage::classifyParforMemClass(A->body);
        int clsB = memstage::classifyParforMemClass(B->body);
        if (clsA == 0 || clsB == 0) continue;
        // complementary: one memory-heavy, one compute-heavy
        auto isMem = [](int c) { return c == 1; };
        auto isCmp = [](int c) { return c == 2; };
        if (!((isMem(clsA) && (isCmp(clsB) || clsB == 3)) ||
              (isMem(clsB) && (isCmp(clsA) || clsA == 3))))
          continue;

        // ---- region independence (concurrent execution is legal)
        BodyInfo ia, ib;
        collectBody(A->body, A->pvar, fn, &ia);
        collectBody(B->body, B->pvar, fn, &ib);
        if (ia.hasGive || ib.hasGive) continue;
        if (ia.unknownWrite || ia.unknownRead || ib.unknownWrite ||
            ib.unknownRead)
          continue;
        std::set<std::string> wa = writeRoots(ia), wb = writeRoots(ib);
        std::set<std::string> ra = readRootsOf(ia), rb = readRootsOf(ib);
        bool conflict = false;
        for (auto& r : wa)
          if (rb.count(r) || wb.count(r)) { conflict = true; break; }
        if (!conflict)
          for (auto& r : wb)
            if (ra.count(r)) { conflict = true; break; }
        if (conflict) continue;
        // B's bounds are evaluated before A finishes in the paired
        // dispatch — A must not write anything B's bounds read
        std::set<std::string> bB;
        memstage::walkExprNodesM(B->plo, [&](Expr* x) {
          if (x->k == Expr::IDENT && !x->globalRef) bB.insert(x->name);
        });
        memstage::walkExprNodesM(B->phi, [&](Expr* x) {
          if (x->k == Expr::IDENT && !x->globalRef) bB.insert(x->name);
        });
        std::set<std::string> bBr;
        exprReadRoots(B->plo, bBr);
        exprReadRoots(B->phi, bBr);
        bool bBad = !bBr.empty() && ia.unknownWrite;
        for (auto& r : bBr)
          if (wa.count(r)) bBad = true;
        for (auto& n : bB)
          if (wa.count(n)) bBad = true;
        if (bBad) continue;

        // ---- mark the pair
        A->smtPairWith = B;
        B->smtPaired = true;
        A->smtMemClass = clsA;
        B->smtMemClass = clsB;
        ++pairs;
        const char* clsName[] = {"?", "memory-bound", "compute-bound",
                                 "balanced"};
        diag(Diag::Note, Code::SmtPair,
             "smt-interleave: paired adjacent parfor regions (lines " +
             std::to_string(A->loc.line) + " + " +
             std::to_string(B->loc.line) + ", fn " + fn->name + ") — " +
             clsName[clsA] + " region runs on the memory lane while the " +
             clsName[clsB] + " region keeps the shared ALU busy on the "
             "sibling hardware thread of the same physical core "
             "(frt_parfor_pair)",
             B->loc,
             "kind=smt-pair,fn=" + fn->name + ",clsA=" +
             std::to_string(clsA) + ",clsB=" + std::to_string(clsB));
        proofLogLine("smt-interleave",
                     "adjacent outermost parfors, regions fully independent "
                     "(no shared write/read roots, later bounds read nothing "
                     "the earlier writes, no unknown effects), complementary "
                     "mem/compute classes, chunk partitions unchanged",
                     fn, B->loc);
        ++i;   // B is consumed
      }
      // recurse into nested lists (parfor bodies are "inside parfor")
      for (Stmt* s : list) {
        switch (s->k) {
          case Stmt::BLOCK:
            walkList(s->stmts, insideParfor);
            break;
          case Stmt::IF:
            if (s->thenB && s->thenB->k == Stmt::BLOCK)
              walkList(s->thenB->stmts, insideParfor);
            if (s->elseB && s->elseB->k == Stmt::BLOCK)
              walkList(s->elseB->stmts, insideParfor);
            break;
          case Stmt::WHILE: case Stmt::FOR:
            if (s->body && s->body->k == Stmt::BLOCK)
              walkList(s->body->stmts, insideParfor);
            break;
          case Stmt::PARFOR:
            if (s->body && s->body->k == Stmt::BLOCK)
              walkList(s->body->stmts, true);
            break;
          case Stmt::MATCH:
            for (auto& arm : s->matchArms)
              if (arm.body && arm.body->k == Stmt::BLOCK)
                walkList(arm.body->stmts, insideParfor);
            break;
          default: break;
        }
      }
    };
    walkList(fn->body->stmts, false);
    // classify every remaining parfor so the runtime can route its
    // chunks even when unpaired (nested regions benefit too)
    std::function<void(Stmt*)> classifyAll = [&](Stmt* s) {
      if (!s) return;
      if (s->k == Stmt::PARFOR) {
        if (s->smtMemClass == 0)
          s->smtMemClass = memstage::classifyParforMemClass(s->body);
        classifyAll(s->body);
        return;
      }
      for (auto* st : s->stmts) classifyAll(st);
      classifyAll(s->body);
      classifyAll(s->thenB);
      classifyAll(s->elseB);
      if (s->k == Stmt::MATCH)
        for (auto& arm : s->matchArms) classifyAll(arm.body);
    };
    classifyAll(fn->body);
  }
  return pairs;
}

} // namespace freshet
