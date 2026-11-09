// Freshet --soa=auto — automatic AoS→SoA transformation (R3).
//
// Runs between parse and sema: the parser keeps struct/soa types as
// late-bound tyName strings ("*:Body", "soa:Body"), so rewriting a
// declaration from *Body to soa(Body) + malloc→soa_new + free→soa_free
// is a pure AST edit; sema then binds everything as if the user wrote
// soa() by hand.
//
// Policy (conservative — auto must never pessimize or change semantics):
//   candidate:  let v [: *S] = malloc(S, n)   not inside a parfor body
//   transform if:
//     - every use of v is one of:
//         * v[e].f...           (plane access; nested struct paths OK)
//         * free(v)             (incl. inside defer)
//       anything else (whole-element use v[e], &v[..], v passed to a
//       call, pointer arithmetic, rebinding, return) → veto
//     - some loop over v fetches < 50% of the bytes it consumes
//       (the F0408 threshold), and
//     - no loop over v fetches ≥ 60% of the struct (SoA would hurt it)
//     - sizeof(S) ≥ 32B and S has ≥ 2 fields
//     - v is not shadowed by another let of the same name in the fn
#include "common.h"
#include "opt_util.h"   // §6.11: tripCountOf for frequency weighting
#include <map>
#include <set>
#include <algorithm>

namespace freshet {

// ------------------------------------------------------------ shallow layout
// Pre-sema the struct registry is empty and field Type*s may be null
// (struct-typed fields are late-bound), so compute sizes from names.
namespace soaauto {

struct ShallowLayout {
  std::vector<uint64_t> sizes;   // per field
  uint64_t size = 0;             // struct size
  bool ok = false;
};

static uint64_t scalarSize(const std::string& n) {
  static const std::map<std::string, uint64_t> sz = {
    {"u8",1},{"i8",1},{"bool",1},{"u16",2},{"i16",2},{"u32",4},{"i32",4},
    {"f32",4},{"u64",8},{"usize",8},{"i64",8},{"f64",8},{"void",0},
  };
  auto it = sz.find(n);
  return it == sz.end() ? (uint64_t)-1 : it->second;
}

static ShallowLayout layoutOf(StructDef* sd, std::map<std::string, StructDef*>& byName,
                              std::set<std::string>& visiting);

static bool fieldSizeAlign(const Field& f, std::map<std::string, StructDef*>& byName,
                           std::set<std::string>& visiting, uint64_t* size,
                           uint64_t* align) {
  if (f.ty && f.ty->k != Type::STRUCT && f.ty->k != Type::SOA) {
    if (f.ty->k == Type::PTR) { *size = 8; *align = 8; return true; }
    uint64_t s = scalarSize(typeToString(f.ty));
    if (s == (uint64_t)-1) return false;
    *size = s; *align = s < 8 ? s : 8;
    return true;
  }
  if (f.tyName.empty()) return false;
  if (f.tyName.rfind("soa:", 0) == 0 || f.tyName.rfind("*:", 0) == 0)
    { *size = 8; *align = 8; return true; }   // ptr/soa field: word-sized
  uint64_t s = scalarSize(f.tyName);
  if (s != (uint64_t)-1) { *size = s; *align = s < 8 ? s : 8; return true; }
  auto it = byName.find(f.tyName);
  if (it == byName.end()) return false;
  ShallowLayout sl = layoutOf(it->second, byName, visiting);
  if (!sl.ok) return false;
  *size = sl.size;
  *align = sl.size < 8 ? sl.size : 8;
  return true;
}

static ShallowLayout layoutOf(StructDef* sd, std::map<std::string, StructDef*>& byName,
                              std::set<std::string>& visiting) {
  ShallowLayout out;
  if (!visiting.insert(sd->name).second) return out;   // cycle → give up
  uint64_t off = 0, maxa = 1;
  for (auto& f : sd->fields) {
    uint64_t s = 0, a = 1;
    if (!fieldSizeAlign(f, byName, visiting, &s, &a)) { visiting.erase(sd->name); return out; }
    off = alignUp(off, a);
    out.sizes.push_back(s);
    off += s;
    maxa = std::max(maxa, a);
  }
  if (sd->alignOverride) maxa = std::max(maxa, sd->alignOverride);
  out.size = alignUp(off, maxa);
  out.ok = out.sizes.size() == sd->fields.size();
  visiting.erase(sd->name);
  return out;
}

} // namespace soaauto

using namespace soaauto;

// ------------------------------------------------------------ expr utilities
static void linkTree(Expr* root, Expr* parent, std::map<Expr*, Expr*>& par) {
  if (!root) return;
  par[root] = parent;
  auto link = [&](Expr* c) { linkTree(c, root, par); };
  for (auto* a : root->args) link(a);
  link(root->lhs); link(root->rhs); link(root->base);
}

struct FnInfo {
  std::map<Expr*, Expr*> par;     // parent of every expr node
  std::vector<Stmt*> loops;       // WHILE/FOR/PARFOR anywhere in fn
  std::vector<Stmt*> lets;        // LET stmts outside parfor bodies
  std::vector<Stmt*> allLets;     // LET stmts everywhere (shadow check)
};

static void walkStmtBuild(Stmt* s, FnInfo& A, int pfDepth) {
  if (!s) return;
  switch (s->k) {
    case Stmt::LET:
      linkTree(s->init, nullptr, A.par);   // init may use candidate vars
      A.allLets.push_back(s);
      if (pfDepth == 0) A.lets.push_back(s);
      break;
    case Stmt::ASSIGN: linkTree(s->lhs, nullptr, A.par); linkTree(s->rhs, nullptr, A.par); break;
    case Stmt::RETURN: linkTree(s->retVal, nullptr, A.par); break;
    // Phase 1d: match arms are arbitrary uses — veto candidates touched
    // by them (conservative: the transform only fires on provable shapes)
    case Stmt::MATCH:
      linkTree(s->matchOn, nullptr, A.par);
      for (auto& arm : s->matchArms) walkStmtBuild(arm.body, A, pfDepth);
      break;
    case Stmt::DEFER: linkTree(s->deferExpr, nullptr, A.par); break;
    case Stmt::IF:
      linkTree(s->cond, nullptr, A.par);
      walkStmtBuild(s->thenB, A, pfDepth); walkStmtBuild(s->elseB, A, pfDepth);
      break;
    case Stmt::WHILE:
      linkTree(s->cond, nullptr, A.par); A.loops.push_back(s);
      walkStmtBuild(s->body, A, pfDepth);
      break;
    case Stmt::FOR:
      if (s->forRange) { linkTree(s->rangeLo, nullptr, A.par); linkTree(s->rangeHi, nullptr, A.par); }
      else { linkTree(s->forInit, nullptr, A.par); linkTree(s->forCond, nullptr, A.par); linkTree(s->forStep, nullptr, A.par); }
      A.loops.push_back(s); walkStmtBuild(s->body, A, pfDepth);
      break;
    case Stmt::PARFOR:
      linkTree(s->plo, nullptr, A.par); linkTree(s->phi, nullptr, A.par);
      for (auto& c : s->clauses) linkTree(c.e, nullptr, A.par);
      A.loops.push_back(s); walkStmtBuild(s->body, A, pfDepth + 1);
      break;
    case Stmt::EXPR: linkTree(s->expr, nullptr, A.par); break;
    case Stmt::BLOCK: for (auto* st : s->stmts) walkStmtBuild(st, A, pfDepth); break;
    default: break;
  }
}

// top-level field of a `v[...] . f ... . g` chain: the field directly
// above the INDEX (matches checks.cpp F0408 accounting)
static std::string topLevelFieldOf(Expr* fld, const std::string& v) {
  if (fld->k != Expr::FIELD) return "";
  Expr* node = fld;
  while (node->k == Expr::FIELD) node = node->base;
  if (node->k != Expr::INDEX) return "";
  if (!(node->base && node->base->k == Expr::IDENT && node->base->name == v))
    return "";
  Expr* bottom = fld;
  while (bottom->k == Expr::FIELD && bottom->base->k == Expr::FIELD)
    bottom = bottom->base;
  if (bottom->k == Expr::FIELD && bottom->base == node) return bottom->name;
  return "";
}

// run f on every expression node in a loop body (recursive, mirroring
// checks.cpp walkBody: bounds/init of the loop itself are excluded)
static void collectBodyNodes(Stmt* s, const std::function<void(Expr*)>& f) {
  if (!s) return;
  std::function<void(Expr*)> walk = [&](Expr* x) {
    if (!x) return;
    f(x);
    for (auto* a : x->args) walk(a);
    walk(x->lhs); walk(x->rhs); walk(x->base);
  };
  switch (s->k) {
    case Stmt::LET: walk(s->init); break;
    case Stmt::ASSIGN: walk(s->lhs); walk(s->rhs); break;
    case Stmt::RETURN: walk(s->retVal); break;
    case Stmt::DEFER: walk(s->deferExpr); break;
    case Stmt::IF:
      walk(s->cond);
      collectBodyNodes(s->thenB, f); collectBodyNodes(s->elseB, f);
      break;
    case Stmt::WHILE: case Stmt::FOR: case Stmt::PARFOR:
      collectBodyNodes(s->body, f);
      break;
    case Stmt::EXPR: walk(s->expr); break;
    case Stmt::BLOCK: for (auto* st : s->stmts) collectBodyNodes(st, f); break;
    default: break;
  }
}

// ------------------------------------------------------------ main pass
int applySoaAutoTransform(Program* p) {
  std::map<std::string, StructDef*> byName;
  for (auto* sd : p->structs) byName[sd->name] = sd;

  struct Cand {
    FnDecl* fn = nullptr;
    Stmt* let = nullptr;
    StructDef* sdef = nullptr;
    ShallowLayout layout;
    bool legal = true;
    std::string vetoReason;
    int nIndexed = 0;
    std::vector<Expr*> freeCalls;
    double minEff = 1e9, maxEff = 0;
    // optimisation-audit §6.11: frequency-weighted efficiency — loop trip
    // counts (exact for literal bounds) × per-loop access counts, not
    // just WHICH fields appear
    double weightedEff = 1.0;
    double weightSum = 0.0;
    bool anyLoop = false;
  };
  std::vector<Cand> cands;
  std::map<FnDecl*, FnInfo> fnInfo;

  // ---------- pass 1: parent maps + candidate discovery
  for (auto* fn : p->fns) {
    if (!fn->body || fn->isGeneric) continue;
    FnInfo& A = fnInfo[fn];
    for (auto* st : fn->body->stmts) walkStmtBuild(st, A, 0);

    for (Stmt* let : A.lets) {
      if (!let->init || let->init->k != Expr::CALL) continue;
      if (let->init->name != "malloc") continue;
      if (let->init->args.size() < 2) continue;
      Expr* tArg = let->init->args[0];
      if (!tArg || tArg->k != Expr::IDENT) continue;
      auto it = byName.find(tArg->name);
      if (it == byName.end()) continue;
      StructDef* sd = it->second;

      bool annOk = let->tyName.empty() || let->tyName == "*:" + sd->name;
      if (!annOk) continue;

      bool shadowed = false;
      for (Stmt* other : A.allLets)
        if (other != let && other->var == let->var) { shadowed = true; break; }
      if (shadowed) continue;

      Cand c;
      c.fn = fn; c.let = let; c.sdef = sd;
      std::set<std::string> visiting;
      c.layout = layoutOf(sd, byName, visiting);
      cands.push_back(c);
    }
  }

  // ---------- pass 2: classify every use of each candidate var
  for (auto& c : cands) {
    const std::string v = c.let->var;
    FnInfo& A = fnInfo[c.fn];
    auto deny = [&](const std::string& why) {
      if (c.legal) { c.legal = false; c.vetoReason = why; }
    };
    for (auto& [e, parent] : A.par) {
      if (!(e->k == Expr::IDENT && e->name == v)) continue;
      if (parent == nullptr) { deny("used as a bare value"); continue; }
      if (parent->k == Expr::CALL) {
        if (parent->name == "free" && parent->args.size() == 1 &&
            parent->args[0] == e) {
          c.freeCalls.push_back(parent);
          continue;
        }
        deny("passed to call '" + parent->name + "'");
        continue;
      }
      if (parent->k == Expr::INDEX && parent->base == e) {
        // v[...]
        Expr* gp = A.par.count(parent) ? A.par[parent] : nullptr;
        if (gp == nullptr) { deny("whole-element use v[...]"); continue; }
        if (gp->k == Expr::INDEX) { deny("chained index v[i][j]"); continue; }
        if (gp->k == Expr::UN) { deny("address-of / unary on v[...]"); continue; }
        if (gp->k == Expr::FIELD && gp->base == parent) {
          // v[i].f... — outer end of the FIELD chain must not feed
          // address-of or another index
          Expr* top = gp;
          bool chainBad = false;
          while (true) {
            Expr* pp = A.par.count(top) ? A.par[top] : nullptr;
            if (pp && pp->k == Expr::FIELD && pp->base == top) { top = pp; continue; }
            if (pp && (pp->k == Expr::UN || pp->k == Expr::INDEX)) { chainBad = true; }
            break;
          }
          if (chainBad) { deny("address-of / index of a field of v[...]"); continue; }
          ++c.nIndexed;
          continue;
        }
        deny("whole-element use v[...]");
        continue;
      }
      deny("used outside plane access");
    }
  }

  // ---------- pass 3: per-loop cache efficiency, FREQUENCY-WEIGHTED
  // (optimisation-audit §6.11: the historical metric counted only WHICH
  // fields a loop touches — a 10-iteration loop over 1 field and a
  // 10-million-iteration loop over the same field scored identically).
  // Each loop's weight is (trip count when the bounds are literals,
  // else 1) × (number of plane-style accesses in its body).
  for (auto& c : cands) {
    const std::string v = c.let->var;
    FnInfo& A = fnInfo[c.fn];
    double wSum = 0.0, wEffSum = 0.0;
    for (Stmt* loop : A.loops) {
      std::map<std::string, int> fieldHits;
      collectBodyNodes(loop->body, [&](Expr* e) {
        std::string f = topLevelFieldOf(e, v);
        if (!f.empty()) fieldHits[f]++;
      });
      if (fieldHits.empty()) continue;
      c.anyLoop = true;
      double trip = 1.0;   // unknown bounds -> neutral weight
      if (loop->k == Stmt::FOR && loop->forRange) {
        optutil::TripInfo ti = optutil::tripCountOf(loop);
        if (ti.known && !ti.empty && !ti.saturated) trip = (double)ti.trip;
      }
      int hits = 0;
      for (auto& kv : fieldHits) hits += kv.second;
      double weight = trip * (double)hits;
      uint64_t needed = 0;
      for (size_t i = 0; i < c.sdef->fields.size(); ++i)
        if (fieldHits.count(c.sdef->fields[i].name) && i < c.layout.sizes.size())
          needed += c.layout.sizes[i];
      double eff = c.layout.size ? (double)needed / (double)c.layout.size : 1.0;
      c.minEff = std::min(c.minEff, eff);
      c.maxEff = std::max(c.maxEff, eff);
      wSum += weight;
      wEffSum += weight * eff;
    }
    c.weightSum = wSum;
    c.weightedEff = wSum > 0.0 ? wEffSum / wSum : 1.0;
  }

  // ---------- pass 4: decide + rewrite (+ §6.11 AoSoA advisory)
  //
  // --soa=aosoa is ADVISORY ONLY: the frequency-weighted layout
  // comparison is reported, the code is never transformed (a real AoSoA
  // lowering needs new runtime container types; the audit's own next
  // step for SoA is exactly this measured cost model).
  int transformed = 0;
  for (auto& c : cands) {
    if (!c.anyLoop) continue;
    auto note = [&](const std::string& msg) {
      diag(Diag::Note, Code::SoaAuto, msg, c.let->loc);
    };
    auto aosoaAdvice = [&]() {
      // §6.11 middle band: neither pure AoS nor pure SoA is a clear
      // winner — a blocked AoSoA(S, tile) layout balances the two
      const char* verdict =
          c.weightedEff < 0.35 ? "SoA planes are the right layout"
          : c.weightedEff > 0.60 ? "AoS rows are the right layout"
          : "AoSoA(S, tile) is the likely winner";
      diag(Diag::Note, Code::SoaAosoa,
           "layout cost report for '" + c.let->var + "' (struct " +
           c.sdef->name + ", " + std::to_string(c.layout.size) +
           "B/element): frequency-weighted plane efficiency " +
           std::to_string((int)(c.weightedEff * 100)) + "% — " + verdict +
           "; restructure manually as soa(" + c.sdef->name +
           ") or a blocked AoSoA layout if this array is hot",
           c.let->loc,
           "var=" + c.let->var + ",struct=" + c.sdef->name +
           ",weighted-eff=" + std::to_string((int)(c.weightedEff * 100)));
    };
    if (g_opts.soaAosoa) { aosoaAdvice(); continue; }
    if (!(c.minEff < 0.5)) continue;   // not W-SOA-hot: leave alone
    if (!c.layout.ok || c.layout.size < 32 || c.sdef->fields.size() < 2) {
      note("auto AoS->SoA skipped for '" + c.let->var + "' in fn " + c.fn->name +
           ": struct " + c.sdef->name + " layout unsuitable for planes");
      continue;
    }
    if (!c.legal || c.nIndexed == 0) {
      note("auto AoS->SoA skipped for '" + c.let->var + "' in fn " + c.fn->name +
           ": " + (c.nIndexed == 0 ? "no plane-style indexing found"
                                   : c.vetoReason) +
           " — transform manually with soa(" + c.sdef->name + ")");
      continue;
    }
    if (c.maxEff >= 0.6) {
      note("auto AoS->SoA skipped for '" + c.let->var + "' in fn " + c.fn->name +
           ": another loop consumes " + std::to_string((int)(c.maxEff * 100)) +
           "% of struct " + c.sdef->name + " per element — planes would hurt it");
      aosoaAdvice();   // §6.11: the middle band is exactly this case
      continue;
    }

    // rewrite: *S -> soa(S), malloc -> soa_new, free -> soa_free
    c.let->tyName = "soa:" + c.sdef->name;
    c.let->annTy = Type::soaOf(nullptr);
    c.let->hasAnn = true;
    c.let->init->name = "soa_new";
    for (Expr* fc : c.freeCalls) fc->name = "soa_free";
    ++transformed;

    diag(Diag::Note, Code::SoaAuto,
         "auto AoS->SoA: '" + c.let->var + "' in fn " + c.fn->name + " (*" +
         c.sdef->name + " -> soa(" + c.sdef->name + ")): hot loop fetches " +
         std::to_string((int)(c.minEff * 100)) + "% of " +
         std::to_string(c.layout.size) + "B per element; fields now in planes",
         c.let->loc,
         "var=" + c.let->var + ",struct=" + c.sdef->name);
  }
  return transformed;
}

} // namespace freshet
