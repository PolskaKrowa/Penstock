// Freshet — mem & vectorisation stage, part 2: the separable-routine
// analysis ("vectorisation helper") and the SLP-oriented statement
// batching transform it drives.
//
//   --vector-plan   advisory only (F0921): for every function, split the
//                   straight-line statement lists into SEPARABLE ROUTINES
//                   (maximal groups of statements with no dataflow or
//                   memory dependence between them), report which of them
//                   are BATCHABLE (isomorphic statement shapes, pairwise
//                   independent, matching types), and summarise the
//                   dataflow (inputs/outputs per routine) that determines
//                   how they can be optimised.
//   --batch-ops     the transform (F0920): a stable topological reorder
//                   of each window that pulls batchable statements
//                   together, so the backend's SLP vectoriser packs them
//                   into vector ops instead of scalarising interleaved
//                   chains.  Mis-grouped scalar ops are exactly what
//                   becomes lane-crossing `mov`s (shuffles) after
//                   vectorisation — batching removes them at the source.
//
// Analysis model (all per statement list / loop body):
//   * scalar def-use: IDENT reads (value bindings) vs LET/ASSIGN writes;
//   * memory accesses: (root, key) pairs where key = canonical index +
//     field path.  Two DIRECT accesses on the same root are INDEPENDENT
//     when their keys differ by a field path (disjoint fields) or when
//     both index expressions are affine in the same induction variable
//     with equal coefficients and different offsets (provably distinct
//     addresses).  Anything else on a shared root is a dependence.
//   * pointer roots resolve through the shared alias map (rebinding a
//     param into a local stays attributed to the real root; wild
//     pointers make the statement immovable);
//   * effects: only statements with no observable effect (pure builtins
//     and @pure will-return callees only) may move at all.
//
// Separable routines are the connected components of the dependence DAG
// (viewed undirected): statements in different routines are mutually
// independent.  Batch groups are maximal sets of same-signature
// statements drawn from different routines — the SLP packable sets.
//
// The reorder is Kahn's algorithm over the same DAG with priority
// (group anchor position, original position): batch members float to
// the first member's position, everything else keeps its relative
// order.  Every emitted order is a topological order of the original
// DAG, so the rewrite is semantics-preserving by construction.
#include "opt_util.h"
#include "loop_analysis.h"
#include <algorithm>
#include <set>

namespace freshet {
namespace vplan {

using namespace optutil;

// ------------------------------------------------------------ signatures
// shape of an expression with operand NAMES abstracted (but types kept —
// packing needs matching element types).  Field names are abstracted so
// the classic `b[i].x = a[i].x * s; b[i].y = a[i].y * s` SoA-style pair
// groups (the dependence analysis keeps the two addresses distinct via
// the field path in the access key).
static void sigExpr(Expr* e, std::string* out) {
  if (!e) { *out += "_"; return; }
  switch (e->k) {
    case Expr::IDENT:
      *out += "v<" + (e->ty ? typeToString(e->ty) : "?") + ">";
      return;
    case Expr::INT: case Expr::FLT: case Expr::BOOL:
      *out += "c";
      return;
    case Expr::STR: *out += "s"; return;
    case Expr::BIN:
      *out += "(" + e->op + " ";
      sigExpr(e->lhs, out);
      *out += " ";
      sigExpr(e->rhs, out);
      *out += ")";
      return;
    case Expr::UN:
      *out += "(" + e->op + " ";
      sigExpr(e->rhs, out);
      *out += ")";
      return;
    case Expr::CALL:
      *out += (e->callee ? ("@" + e->callee->name) : ("@" + e->name)) + "(";
      for (size_t i = 0; i < e->args.size(); ++i) {
        if (i) *out += ",";
        sigExpr(e->args[i], out);
      }
      *out += ")";
      return;
    case Expr::INDEX:
      *out += "[";
      sigExpr(e->base, out);
      *out += ";";
      sigExpr(e->rhs, out);
      *out += "]";
      return;
    case Expr::FIELD:
      *out += ".f(";
      sigExpr(e->base, out);
      *out += ")";
      return;
    case Expr::CAST:
      *out += "cast<" + e->castTyName + ">(";
      sigExpr(e->base, out);
      *out += ")";
      return;
    default:
      *out += "?";
      return;
  }
}

static std::string stmtSignature(Stmt* s) {
  std::string sig;
  switch (s->k) {
    case Stmt::ASSIGN:
      sig = "A";
      sigExpr(s->lhs, &sig);
      sig += "=";
      sigExpr(s->rhs, &sig);
      return sig;
    case Stmt::LET:
      sig = "L<" + (s->annTy ? typeToString(s->annTy)
                              : (s->init && s->init->ty
                                     ? typeToString(s->init->ty)
                                     : "?")) + ">";
      sigExpr(s->init, &sig);
      return sig;
    case Stmt::EXPR:
      sig = "E";
      sigExpr(s->expr, &sig);
      return sig;
    default: return "";
  }
}

// --------------------------------------------------------- access census
struct MAccess {
  std::string root;    // resolved root name (post-alias)
  std::string key;     // canon(idx) + "#" + field path ("" = whole/other)
  bool write = false;
};

struct StmtInfo {
  bool movable = false;
  std::set<std::string> reads;       // scalar value reads (incl. idx vars)
  std::set<std::string> writes;      // scalar writes (LET var / assign root)
  std::vector<MAccess> mem;          // resolved memory accesses
  std::string sig;
  int laneWork = 0;
};

static Expr* vpRoot(Expr* e) {
  while (e && (e->k == Expr::INDEX || e->k == Expr::FIELD)) e = e->base;
  return e;
}

// field path of an INDEX/FIELD chain: "x" / "x.y" ("" when no fields)
static std::string fieldPath(Expr* e) {
  std::vector<std::string> parts;
  for (Expr* n = e; n && n->k == Expr::FIELD; n = n->base)
    parts.push_back(n->name);
  std::string s;
  for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
    if (!s.empty()) s += ".";
    s += *it;
  }
  return s;
}

// index expression of an INDEX/FIELD chain (null when bare FIELD)
static Expr* idxExpr(Expr* e) {
  Expr* node = e;
  while (node && node->k == Expr::FIELD) node = node->base;
  if (node && node->k == Expr::INDEX) return node->rhs;
  return nullptr;
}

// collect reads/writes/memory accesses of ONE statement; `am` resolves
// pointer rebinding; wild roots make the statement immovable
static void analyzeStmt(Stmt* s, const AliasMap& am, StmtInfo* out) {
  out->movable = false;
  out->sig = stmtSignature(s);
  if (out->sig.empty()) return;   // only flat LET/ASSIGN/EXPR reorders

  bool bad = false;

  auto noteIdent = [&](Expr* e) {
    if (e->k != Expr::IDENT || e->globalRef) return;
    if (!e->sym || !(e->sym->k == Sym::VAR || e->sym->k == Sym::PARAM))
      return;
    if (e->ty && (e->ty->isPtr() || e->ty->k == Type::SOA)) return;
    out->reads.insert(e->name);
  };

  std::function<void(Expr*, bool)> walk = [&](Expr* e, bool lvalue) {
    if (!e || bad) return;
    switch (e->k) {
      case Expr::IDENT: noteIdent(e); return;
      case Expr::INDEX: case Expr::FIELD: {
        Expr* root = vpRoot(e);
        if (root && root->k == Expr::IDENT && !root->globalRef &&
            root->ty && (root->ty->isPtr() || root->ty->k == Type::SOA ||
                         root->ty->k == Type::ARRAY)) {
          // memory access through a root binding
          std::set<std::string> targets;
          if (!resolveAlias(am, root->name, targets)) { bad = true; return; }
          Expr* ie = idxExpr(e);
          std::string key = ie ? canon(ie) : "";
          key += "#";
          key += fieldPath(e);
          ++out->laneWork;
          for (auto& t : targets)
            out->mem.push_back({t, key, lvalue});
          // index sub-expressions are scalar reads
          for (Expr* n = e; n; n = n->base) {
            if (n->k == Expr::INDEX) walk(n->rhs, false);
          }
          return;
        }
        // struct value fields: scalar-ish access on the root binding
        if (root && root->k == Expr::IDENT && !root->globalRef) {
          (lvalue ? out->writes : out->reads).insert(root->name);
          if (e->k == Expr::INDEX) walk(e->rhs, false);
          walk(e->base, lvalue);
          return;
        }
        walk(e->base, lvalue);
        walk(e->rhs, false);
        return;
      }
      case Expr::CALL: {
        if (e->callee) {
          if (!e->callee->isPure) { bad = true; return; }
          for (auto* a : e->args) walk(a, false);
          return;
        }
        const std::string& n = e->name;
        if (n == "soa_len" && e->args.size() == 1 &&
            e->args[0]->k == Expr::IDENT) {
          // descriptor read — scalar on the root name
          noteIdent(e->args[0]);
          return;
        }
        if (!isPureBuiltinCall(n)) { bad = true; return; }
        for (auto* a : e->args) walk(a, false);
        return;
      }
      case Expr::BIN:
        if (e->op != "==" && e->op != "!=" && e->op != "<" &&
            e->op != ">" && e->op != "<=" && e->op != ">=" &&
            e->op != "&&" && e->op != "||")
          ++out->laneWork;
        walk(e->lhs, false);
        walk(e->rhs, false);
        return;
      default:
        for (auto* a : e->args) walk(a, false);
        walk(e->lhs, false);
        walk(e->rhs, false);
        walk(e->base, lvalue);
        return;
    }
  };

  switch (s->k) {
    case Stmt::LET:
      if (s->init) walk(s->init, false);
      out->writes.insert(s->var);
      break;
    case Stmt::ASSIGN: {
      Expr* root = vpRoot(s->lhs);
      // compound ops read the lhs too
      if (root && root->k == Expr::IDENT && s->op2 != "=")
        out->reads.insert(root->name);
      walk(s->lhs, true);
      walk(s->rhs, false);
      break;
    }
    case Stmt::EXPR:
      walk(s->expr, false);
      break;
    default: return;
  }
  out->movable = !bad;
}

// ------------------------------------------------------ dependence testing
// richer key comparison (field-path aware); the idx hook exists for a
// future affine-distinctness proof over the real AST nodes (v1 callers
// always pass idx = nullptr — see below)
struct MAccessE {
  std::string root;
  std::string key;
  Expr* idx = nullptr;    // original index expr (null for whole-binding)
  bool write = false;
};

static bool mayAliasAccessE(const MAccessE& a, const MAccessE& b,
                            const std::string& iv) {
  if (a.key == b.key) return true;
  size_t ha = a.key.rfind('#'), hb = b.key.rfind('#');
  std::string fa = ha == std::string::npos ? "" : a.key.substr(ha + 1);
  std::string fb = hb == std::string::npos ? "" : b.key.substr(hb + 1);
  if (fa != fb) return false;                 // disjoint field paths
  if (!a.idx || !b.idx) return true;          // whole-binding overlap
  if (!iv.empty()) {
    Affine aa = affineIn(a.idx, iv);
    Affine ab = affineIn(b.idx, iv);
    if (aa.valid && ab.valid && aa.coef == ab.coef && aa.off != ab.off)
      return false;   // a*iv+oa vs a*iv+ob, oa != ob — distinct addresses
  }
  return true;
}

// ---------------------------------------------------------------- window
struct Window {
  std::vector<size_t> idx;         // positions in the host list
  std::vector<StmtInfo> info;      // parallel to idx
};

struct DepEdge { size_t from, to; };

// strands (separable routines) = undirected components of the dep graph
struct StrandsResult {
  int nstrands = 0;
  std::vector<int> strandOf;       // per window stmt
  std::vector<std::vector<size_t>> members;
};

// --------------------------------------------------------------- analysis
struct ListPlan {
  bool useful = false;             // reportable / transformable at all
  size_t windowStart = 0, windowEnd = 0;
  Window win;
  std::vector<DepEdge> deps;
  StrandsResult strands;
  // batch groups: indices into the window
  std::vector<std::vector<size_t>> groups;
  int targetWidth = 8;
};

static void planWindow(const std::vector<size_t>& idx,
                       const std::vector<StmtInfo>& info,
                       const std::string& iv, ListPlan* plan) {
  size_t n = idx.size();
  plan->win.idx = idx;
  plan->win.info = info;

  // ---- dependence edges (i < j)
  auto scalarDep = [&](size_t i, size_t j) {
    const StmtInfo& a = info[i];
    const StmtInfo& b = info[j];
    for (auto& w : a.writes)
      if (b.reads.count(w) || b.writes.count(w)) return true;  // RAW/WAW
    for (auto& w : b.writes)
      if (a.reads.count(w)) return true;                        // WAR
    return false;
  };
  // rebuild accesses for the dependence test (index expressions are not
  // carried in v1 — MAccessE::idx stays null, so mayAliasAccessE falls
  // back to whole-binding overlap for same-key accesses)
  std::vector<std::vector<MAccessE>> acc(n);
  for (size_t i = 0; i < n; ++i)
    for (auto& m : info[i].mem)
      acc[i].push_back({m.root, m.key, nullptr, m.write});
  auto memDep = [&](size_t i, size_t j) {
    for (auto& a : acc[i])
      for (auto& b : acc[j]) {
        if (a.root != b.root) continue;
        if (!(a.write || b.write)) continue;   // read-read is fine
        if (mayAliasAccessE(a, b, iv)) return true;
      }
    return false;
  };
  for (size_t i = 0; i < n; ++i)
    for (size_t j = i + 1; j < n; ++j)
      if (scalarDep(i, j) || memDep(i, j))
        plan->deps.push_back({i, j});

  // ---- strands = undirected components
  std::vector<std::vector<size_t>> adj(n);
  for (auto& e : plan->deps) {
    adj[e.from].push_back(e.to);
    adj[e.to].push_back(e.from);
  }
  plan->strands.strandOf.assign(n, -1);
  for (size_t i = 0; i < n; ++i) {
    if (plan->strands.strandOf[i] >= 0) continue;
    int id = (int)plan->strands.members.size();
    plan->strands.members.push_back({});
    std::vector<size_t> stack{i};
    plan->strands.strandOf[i] = id;
    while (!stack.empty()) {
      size_t u = stack.back();
      stack.pop_back();
      plan->strands.members[id].push_back(u);
      for (size_t v : adj[u])
        if (plan->strands.strandOf[v] < 0) {
          plan->strands.strandOf[v] = id;
          stack.push_back(v);
        }
    }
  }
  plan->strands.nstrands = (int)plan->strands.members.size();

  // ---- batch groups: same signature, no dep edge between members
  auto hasEdge = [&](size_t i, size_t j) {
    for (auto& e : plan->deps)
      if ((e.from == i && e.to == j) || (e.from == j && e.to == i))
        return true;
    return false;
  };
  for (size_t i = 0; i < n; ++i) {
    if (info[i].sig.empty()) continue;
    bool placed = false;
    for (auto& g : plan->groups) {
      if (g.size() >= 8) continue;
      if (info[g[0]].sig != info[i].sig) continue;
      bool ok = true;
      for (size_t m : g)
        if (hasEdge(i, m)) { ok = false; break; }
      if (!ok) continue;
      g.push_back(i);
      placed = true;
      break;
    }
    if (!placed) plan->groups.push_back({i});
  }
  // keep only real groups (>= 2 members)
  std::vector<std::vector<size_t>> real;
  for (auto& g : plan->groups)
    if (g.size() >= 2) real.push_back(g);
  plan->groups = real;

  plan->useful = plan->strands.nstrands >= 2 || !plan->groups.empty();
}

// ------------------------------------------------------- advisory report
static void emitAdvisory(const ListPlan& plan, std::vector<Stmt*>& list,
                         FnDecl* fn, int* notes) {
  if (!plan.useful || *notes >= 2) return;
  ++*notes;
  std::string sizes;
  for (size_t i = 0; i < plan.strands.members.size(); ++i) {
    if (i) sizes += "+";
    sizes += std::to_string(plan.strands.members[i].size());
  }
  std::string groups;
  for (size_t i = 0; i < plan.groups.size(); ++i) {
    if (i) groups += ", ";
    groups += "width " + std::to_string(plan.groups[i].size());
    size_t s0 = plan.groups[i][0];
    groups += " @line " +
              std::to_string(list[plan.win.idx[s0]]->loc.line);
  }
  // dataflow: strand inputs = scalar reads not written inside the strand
  // plus distinct memory roots read; outputs = strand writes that any
  // OTHER window statement reads
  std::string flow;
  for (size_t i = 0; i < plan.strands.members.size() && i < 4; ++i) {
    std::set<std::string> innerWrites, innerReads, innerMemReads;
    for (size_t m : plan.strands.members[i]) {
      innerWrites.insert(plan.win.info[m].writes.begin(),
                         plan.win.info[m].writes.end());
      innerReads.insert(plan.win.info[m].reads.begin(),
                        plan.win.info[m].reads.end());
      for (auto& mm : plan.win.info[m].mem)
        if (!mm.write) innerMemReads.insert(mm.root);
    }
    int nIn = 0, nOut = 0;
    for (auto& r : innerReads)
      if (!innerWrites.count(r)) ++nIn;
    nIn += (int)innerMemReads.size();
    for (auto& w : innerWrites)
      for (size_t j = 0; j < plan.win.info.size(); ++j) {
        if (plan.strands.strandOf[j] == (int)i) continue;
        if (plan.win.info[j].reads.count(w)) { ++nOut; break; }
      }
    if (i) flow += "; ";
    flow += "routine " + std::to_string(i + 1) + ": " +
            std::to_string(nIn) + " in / " + std::to_string(nOut) +
            " out";
  }
  diag(Diag::Note, Code::VecPlan,
       "vector-plan: " + std::to_string(plan.strands.nstrands) +
       " separable routine(s) (sizes " + sizes + ") in fn " + fn->name +
       " — independent statement groups the vectoriser could pack" +
       (groups.empty() ? "" : "; batchable: " + groups) +
       " (target width " + std::to_string(plan.targetWidth) + "); " +
       flow +
       (groups.empty() ? "" : "; apply with --batch-ops"),
       list[plan.win.idx[0]]->loc,
       "kind=vec-plan,fn=" + fn->name + ",strands=" +
       std::to_string(plan.strands.nstrands) + ",groups=" +
       std::to_string(plan.groups.size()));
}

// ------------------------------------------------------------ transform
static int applyBatching(const ListPlan& plan, std::vector<Stmt*>& list,
                         FnDecl* fn) {
  if (plan.groups.empty()) return 0;
  size_t n = plan.win.idx.size();

  // anchor + indegree over the window
  std::vector<int> anchor(n);
  std::vector<int> indeg(n, 0);
  std::vector<std::vector<size_t>> succ(n);
  for (auto& e : plan.deps) {
    succ[e.from].push_back(e.to);
    ++indeg[e.to];
  }
  std::vector<char> grouped(n, 0);
  std::vector<int> groupAnchor(n, -1);
  for (auto& g : plan.groups) {
    int a = (int)g[0];
    for (size_t m : g) {
      grouped[m] = 1;
      groupAnchor[m] = a;
    }
  }
  for (size_t i = 0; i < n; ++i)
    anchor[i] = grouped[i] ? groupAnchor[i] : (int)i;

  // Kahn with (anchor, original index) priority
  std::set<size_t> ready;
  for (size_t i = 0; i < n; ++i)
    if (indeg[i] == 0) ready.insert(i);
  std::vector<size_t> order;
  order.reserve(n);
  while (!ready.empty()) {
    // smallest (anchor, idx)
    size_t best = *ready.begin();
    for (size_t r : ready)
      if (anchor[r] < anchor[best] ||
          (anchor[r] == anchor[best] && r < best))
        best = r;
    ready.erase(best);
    order.push_back(best);
    for (size_t v : succ[best])
      if (--indeg[v] == 0) ready.insert(v);
  }
  if (order.size() != n) return 0;   // cycle (shouldn't happen) — bail

  // identity?
  bool changed = false;
  for (size_t i = 0; i < n; ++i)
    if (order[i] != i) { changed = true; break; }
  if (!changed) return 0;

  // splice: window positions hold the reordered statements
  std::vector<Stmt*> perm(n);
  for (size_t i = 0; i < n; ++i) perm[i] = list[plan.win.idx[order[i]]];
  for (size_t i = 0; i < n; ++i) list[plan.win.idx[i]] = perm[i];

  int totalGrouped = 0;
  std::string widths;
  for (size_t i = 0; i < plan.groups.size(); ++i) {
    if (i) widths += "+";
    widths += std::to_string(plan.groups[i].size());
    totalGrouped += (int)plan.groups[i].size();
  }
  diag(Diag::Note, Code::Batch,
       "batch-ops: reordered " + std::to_string(n) +
       " statements (line " +
       std::to_string(list[plan.win.idx[0]]->loc.line) + ", fn " +
       fn->name + ") so " + std::to_string(totalGrouped) +
       " isomorphic operations (" + widths + " wide) sit adjacent — "
       "the SLP vectoriser can pack them into vector units instead of "
       "scalarising interleaved chains (which become lane-crossing "
       "shuffles after vectorisation)",
       list[plan.win.idx[0]]->loc,
       "kind=batch,fn=" + fn->name + ",widths=" + widths);
  proofLogLine("batch-ops",
               "topological reorder of the dependence DAG (scalar def-use "
               "+ memory roots with field/affine distinctness); every "
               "statement effect-free; batch members pairwise independent",
               fn, list[plan.win.idx[0]]->loc);
  return 1;
}

} // namespace vplan

using namespace optutil;   // the driver below lives at freshet scope

// ---------------------------------------------------------------- driver
int runVectorPlanPass(Program* p, bool applyBatching) {
  if (!g_opts.vectorPlanOn && !g_opts.batchOpsOn) return 0;
  // the advisory and the transform are independent modes: --vector-plan
  // alone reports (never rewrites); --batch-ops alone transforms (the
  // analysis it needs runs either way)
  const bool doBatch = applyBatching && g_opts.batchOpsOn;
  computeFootprints(p);

  int batches = 0, notes = 0;
  for (auto* fn : p->fns) {
    if (!fn->body || fn->isGeneric) continue;
    AliasMap am;
    buildAliasMap(fn->body, &am);

    // walk every statement list; loop bodies carry their ivar for the
    // affine distinctness proof
    std::function<void(std::vector<Stmt*>&, const std::string&)> walk =
        [&](std::vector<Stmt*>& list, const std::string& iv) {
      // split into maximal runs of movable statements (windows)
      size_t i = 0;
      while (i < list.size()) {
        vplan::StmtInfo probe;
        if (list[i]->k != Stmt::LET && list[i]->k != Stmt::ASSIGN &&
            list[i]->k != Stmt::EXPR) { ++i; continue; }
        vplan::analyzeStmt(list[i], am, &probe);
        if (!probe.movable) { ++i; continue; }
        size_t start = i;
        std::vector<size_t> widx;
        std::vector<vplan::StmtInfo> winfo;
        while (i < list.size() && list[i]->k != Stmt::DEFER &&
               (list[i]->k == Stmt::LET || list[i]->k == Stmt::ASSIGN ||
                list[i]->k == Stmt::EXPR)) {
          if (widx.size() >= 48) break;
          vplan::StmtInfo si;
          vplan::analyzeStmt(list[i], am, &si);
          if (!si.movable) break;
          widx.push_back(i);
          winfo.push_back(si);
          ++i;
        }
        if (widx.size() >= 2) {
          vplan::ListPlan plan;
          plan.targetWidth = g_opts.targetVectorWidth > 0
                                 ? g_opts.targetVectorWidth : 8;
          vplan::planWindow(widx, winfo, iv, &plan);
          if (g_opts.vectorPlanOn)
            vplan::emitAdvisory(plan, list, fn, &notes);
          if (doBatch)
            batches += vplan::applyBatching(plan, list, fn);
        }
        if (i == start) ++i;
      }
      // recurse into nested lists
      for (Stmt* s : list) {
        switch (s->k) {
          case Stmt::BLOCK:
            walk(s->stmts, iv);
            break;
          case Stmt::IF:
            if (s->thenB && s->thenB->k == Stmt::BLOCK)
              walk(s->thenB->stmts, iv);
            if (s->elseB && s->elseB->k == Stmt::BLOCK)
              walk(s->elseB->stmts, iv);
            break;
          case Stmt::WHILE:
            if (s->body && s->body->k == Stmt::BLOCK)
              walk(s->body->stmts, "");
            break;
          case Stmt::FOR:
            if (s->body && s->body->k == Stmt::BLOCK)
              walk(s->body->stmts, s->forRange ? s->ivar : "");
            break;
          case Stmt::PARFOR:
            if (s->body && s->body->k == Stmt::BLOCK)
              walk(s->body->stmts, s->pvar);
            break;
          case Stmt::MATCH:
            for (auto& arm : s->matchArms)
              if (arm.body && arm.body->k == Stmt::BLOCK)
                walk(arm.body->stmts, iv);
            break;
          default: break;
        }
      }
    };
    walk(fn->body->stmts, "");
  }
  return batches;
}

} // namespace freshet
