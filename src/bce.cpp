// Freshet — bounds-check elimination (BCE), ported from the BCE pass of
// the Goose programming language (aardappel/goose, src/bce.h — its largest
// optimisation pass, ~4.6k lines), reduced to Freshet's by-value world.
//
// Goose proves, at every bounds-checked index/slice site, that the emitted
// check cannot fire, and marks the site so codegen omits it: "memory
// safety without the checks' cost".  The solver is a flow-sensitive
// difference-constraint system:
//
//   * FACTS are `l <= r + c` over three base kinds — the constant zero,
//     integer VARIABLES, and the LENGTHS of span/str/soa bindings — plus
//     per-type range axioms for narrow integers (u8..u32, i8..i32) that
//     the query graph edges in for free.  A TERM is `base + off` (offs
//     must be "small", < 2^32, like goose's SmallOff).
//   * GENERATIONS: every base carries a generation; a kill (an assignment
//     to the variable, a re-binding of the span, a call whose footprint
//     writes globals) opens a NEW generation, and every fact stays pinned
//     to the generation it was proven about.  A stale fact can never
//     prove anything about a new value, because (x, g) and (x, g') are
//     DIFFERENT nodes in the query graph — goose's central soundness
//     trick, kept verbatim.
//   * QUERIES run Bellman-Ford with saturating arithmetic (positive
//     overflow saturates to unknown — a proof can only be weakened by
//     saturation, never faked) from the queried base over the fact graph
//     plus the type axioms.
//   * JOINS (if/else, match arms) meet two flows: a fact survives only
//     when BOTH flows state it about the SAME pinned generation, at the
//     weaker constant; generations take the max (a differing generation
//     means the merged value is unknown — and its facts died above).
//
// INTERPROCEDURAL ENTRY-FACT SEEDING (goose bce.h SeedEntryFacts/sites):
//   * a callee's INTERFACE is the tuple of its integer params and its
//     span/str/soa params' LENGTHS (goose's InterfaceOf bases; base 0 is
//     the constant zero);
//   * at every DIRECT call site the caller's judge walk records, into a
//     per-callee table, the constraint `param_x <= param_y + c` its own
//     solver proved about the ACTUAL arguments at that point (goose's
//     `record`: DistsFrom over the caller's graph, offset-adjusted),
//     keeping the WEAKEST constraint any site states (max over c);
//   * once every site has been judged, the callee's own walk is SEEDED
//     at entry with that table — every check inside it can then be
//     elided under a precondition every caller already establishes
//     ("grants a specialisation, at entry, what all of its call sites
//     proved about its parameters");
//   * seeding is skipped UNLESS all sites were analysed (seen == counted)
//     and nothing reaches the function any other way — its value taken,
//     an export, a @test entry, an unresolvable call (goose's
//     opaquesites) — or the arity mismatches; callers are judged FIRST
//     (reverse post-order over the direct-call graph — goose's
//     calleesfirst.rbegin()), so every table is complete before its
//     callee runs; recursion inside a cycle leaves seen < counted for
//     someone in it, which just skips that seeding;
//   * seeded facts are pinned to the callee's ENTRY generations: a
//     re-assigned param or a re-bound span opens a new generation and
//     the entry fact about it dies with the old one — the central
//     soundness trick carries through unchanged.
//
// Fact sources (goose's, adapted):
//   * loop / parfor iteration ranges: `for (i in lo..hi)` and
//     `parfor (i in lo..hi)` add `lo <= i <= hi-1` from the terms captured
//     at loop entry (bounds are evaluated once, so the pinned facts are
//     exactly the loop's snapshot — they survive kills of lo/hi inside
//     the body); every base the body writes is havoced first, so
//     per-iteration re-assignments cannot leak a previous iteration's
//     value; after a break-free loop, `i >= hi` holds;
//   * `assert(c)` and the branches of `if` / `while` conditions
//     (comparisons over integer terms become facts; `&&`/`||`/`!` fold
//     like goose's CondFacts);
//   * construction: `let s = span(p, n)` proves `len(s) == n`,
//     `let t = s[lo..hi]` proves `len(t) == hi-lo`, and an integer `let
//     x = <term>` proves `x == <term>` (goose's RecordSet);
//   * COMPLETED CHECKS (goose's CheckedFacts): past any index site —
//     elided (proven) or kept (trapped) — the program continues only with
//     `0 <= idx <= len-1`, so a second index with the same expression is
//     proven by the first.
//
// Elision rule (sound against the EMITTED code, not just the source): the
// index check is `icmp ult i64 idx, len` and the slice check is
// `ule lo, hi && ule hi, len` — all UNSIGNED compares.  We prove the
// SIGNED chain `0 <= idx <= len-1` (resp. `0 <= lo <= hi <= len`); a
// proven chain bounds idx by len-1 <= 2^63-1, so the high bit is clear
// and the unsigned compare is unfailable.  An unprovable site keeps its
// check — the pass never rewrites the tree, it only stops emitting checks
// that cannot fire.
//
// Freshet simplifications versus goose (all precision-losing, never
// soundness-losing — goose's Places/aliasing/UltOf machinery exists for
// its reference-typed world; Freshet is by-value, so a variable's value
// changes ONLY through assignment to its name, and a length changes only
// through re-binding the span/str/soa):
//   * no BK_TMP one-shot temporaries and no Lin scaled bases (4*i): terms
//     are variables, `.len`/`soa_len`, constants and x±smallconst in
//     64-bit arithmetic only;
//   * a binding whose address is taken (`&x`) is untracked entirely;
//   * calls kill only GLOBAL bases (locals are by-value): a callee
//     footprint that writesGlobal/writesUnknown (or an unknown indirect
//     callee) kills every global base; locals never die at calls;
//   * SoA element indexing (`s[i].f`) currently emits NO check in IRgen,
//     so it is out of scope here (an existing gap, not one introduced by
//     this pass).
//
// Surfaces:
//   * `--bce` (default on from -O1, like goose; `--no-bce` is the
//     differential-testing escape hatch) runs the solver and marks sites;
//   * `--bce-lines` prints goose's per-line report
//     (`bce-line: <file>:<line>: N elided, M kept`);
//   * `@checkfree fn` errors (F0924) when any check the pass would keep
//     survives in the function or anything it (transitively) calls —
//     goose: "no bounds check runs while a checkfree function does".
#include "common.h"
#include "opt_util.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace freshet {
namespace {

using namespace optutil;

constexpr int64_t BCE_INF = INT64_MAX;
constexpr int64_t BCE_CCAP = int64_t(1) << 60;    // fact/constant clamp
constexpr int64_t BCE_OFFCAP = int64_t(1) << 32;  // "small" term offsets
constexpr size_t BCE_MAXFACTS = 200;              // FIFO-capped fact table

enum Bk : uint8_t { BK_ZERO, BK_VAR, BK_LEN };

struct Base {
  uint8_t k = BK_ZERO;
  int id = 0;
  int gen = 0;
  bool operator==(const Base& o) const {
    return k == o.k && id == o.id && gen == o.gen;
  }
};

struct Fact { Base l, r; int64_t c = 0; };  // l <= r + c
struct Term { bool ok = false; Base b; int64_t off = 0; };  // b + off

static int64_t satAdd(int64_t a, int64_t b) {
  if (a == BCE_INF || b == BCE_INF) return BCE_INF;
  __int128 s = (__int128)a + b;
  if (s > BCE_CCAP) return BCE_INF;  // positive overflow weakens to unknown
  if (s < -BCE_CCAP) return -BCE_CCAP;
  return (int64_t)s;
}
static int64_t satSub(int64_t a, int64_t b) {
  if (a == BCE_INF) return BCE_INF;
  __int128 d = (__int128)a - b;
  if (d > BCE_CCAP) return BCE_INF;
  if (d < -BCE_CCAP) return -BCE_CCAP;
  return (int64_t)d;
}
static bool smallOff(int64_t v) { return v > -BCE_OFFCAP && v < BCE_OFFCAP; }

static Base zeroBase() { return Base{BK_ZERO, 0, 0}; }

// one kept-check record for @checkfree enforcement
struct Kept { SrcLoc loc; std::string why; };

struct BCE {
  Program* prog;
  FnDecl* fn;

  // ---- interprocedural entry facts (goose SeedEntryFacts) -------------
  // per-callee site table shared by every per-fn solver of one program
  // run (owned by runBcePass):
  struct SiteRec {
    bool viable = true;
    int seen = 0;
    std::vector<int64_t> c;  // n*n; INT64_MIN = no data, BCE_INF = dead
  };
  std::map<FnDecl*, SiteRec>* sites = nullptr;
  std::map<FnDecl*, int>* nsites = nullptr;          // census counts
  std::set<FnDecl*>* opaqueSites = nullptr;          // reached opaquely
  std::map<FnDecl*, std::vector<std::pair<bool, size_t>>>* ifaces = nullptr;
  bool seeded = false;

  // ---- flow state ---------------------------------------------------
  std::vector<Fact> facts;
  std::map<std::pair<uint8_t, int>, int> gens;  // (kind,id) -> current gen
  int nextGen = 1;

  // ---- interning + prescan ------------------------------------------
  // IRgen materialises ONE alloca per NAME (irgen_stmt.cpp dedupes
  // redeclared names), so bases are interned by NAME too — every same-named
  // VAR/PARAM denotes the same storage, which is exactly what makes fused
  // loops (whose merged bodies reference each source loop's own ivar Sym*)
  // and shadowed re-lets sound to reason about by name.
  std::map<std::string, int> ids;               // "L:name"/"G:name" -> id
  std::map<int, Type*> idTypes;                 // id -> declared type
  int nextId = 1;
  std::set<int> addrof;                         // ids of address-taken bases
  std::set<int> globalIds;

  // ---- reporting -----------------------------------------------------
  int idxElided = 0, idxTotal = 0, slElided = 0, slTotal = 0;
  std::map<std::pair<std::string, int>, std::pair<int, int>> lineOut;
  std::vector<Kept> kept;

  explicit BCE(Program* p, FnDecl* f) : prog(p), fn(f) {}
  explicit BCE(Program* p, FnDecl* f,
               std::map<FnDecl*, SiteRec>* st,
               std::map<FnDecl*, int>* ns,
               std::set<FnDecl*>* op,
               std::map<FnDecl*, std::vector<std::pair<bool, size_t>>>* ifc)
      : prog(p), fn(f), sites(st), nsites(ns), opaqueSites(op),
        ifaces(ifc) {}

  // ------------------------------------------------------------- interning
  static std::string lkey(const std::string& n) { return "L:" + n; }
  static std::string gkey(const std::string& n) { return "G:" + n; }
  int idOf(const std::string& key, Type* ty = nullptr) {
    auto it = ids.find(key);
    int id = it != ids.end() ? it->second : (ids[key] = nextId++);
    if (ty && !idTypes.count(id)) idTypes[id] = ty;
    return id;
  }
  int genOf(uint8_t k, int id) const {
    auto it = gens.find({k, id});
    return it == gens.end() ? 0 : it->second;
  }
  Base cur(uint8_t k, int id) const { return Base{k, id, genOf(k, id)}; }
  void bump(uint8_t k, int id) { gens[{k, id}] = ++nextGen; }

  void killKey(const std::string& key, Type* ty) {
    int id = idOf(key, ty);
    if (ty && ty->isInt()) bump(BK_VAR, id);
    if (ty && (ty->k == Type::SPAN || ty->k == Type::STR ||
               ty->k == Type::SOA))
      bump(BK_LEN, id);
  }
  void killGlobals() {
    for (int id : globalIds) {
      bump(BK_VAR, id);
      bump(BK_LEN, id);
    }
  }
  void havocAll() {
    for (auto& kv : gens) bump(kv.first.first, kv.first.second);
  }

  // --------------------------------------------------------------- facts
  void addFact(const Base& l, const Base& r, int64_t c) {
    if (c >= BCE_CCAP) return;  // a useless bound
    for (auto& f : facts)
      if (f.l == l && f.r == r) {
        if (c < f.c) f.c = c;  // keep the strongest per (l, r)
        return;
      }
    facts.push_back({l, r, c});
    if (facts.size() > BCE_MAXFACTS) facts.erase(facts.begin());
  }

  // snapshot/restore for branches (goose: Flow copies + Meet)
  struct Flow { std::vector<Fact> facts; std::map<std::pair<uint8_t,int>, int> gens; };
  Flow snap() const { return Flow{facts, gens}; }
  void restore(const Flow& f) { facts = f.facts; gens = f.gens; }
  void meet(const Flow& other) {
    std::vector<Fact> out;
    for (auto& f : facts)
      for (auto& g : other.facts)
        if (f.l == g.l && f.r == g.r) {
          out.push_back({f.l, f.r, std::max(f.c, g.c)});
          break;
        }
    facts = out;
    if (facts.size() > BCE_MAXFACTS)
      facts.erase(facts.begin(), facts.end() - BCE_MAXFACTS);
    for (auto& kv : other.gens) {
      int& mine = gens[kv.first];
      mine = std::max(mine, kv.second);
    }
  }

  // -------------------------------------------------------------- queries
  using NKey = std::tuple<uint8_t, int, int>;
  static NKey key(const Base& b) { return {b.k, b.id, b.gen}; }

  // Bellman-Ford distances from `from` over facts + type axioms.  Edges
  // l->r with weight c encode `l <= r + c`; saturating adds mean an
  // overflow can only weaken a bound, never fabricate one.
  std::map<NKey, int64_t> dists(const Base& from) const {
    std::map<NKey, int64_t> d;
    d[key(from)] = 0;
    d[key(zeroBase())] = 0;
    auto touch = [&](const Base& b) {
      if (!d.count(key(b))) d[key(b)] = BCE_INF;
    };
    for (auto& f : facts) { touch(f.l); touch(f.r); }

    std::vector<Fact> edges = facts;
    // type axioms for narrow integer variables at every generation the
    // graph mentions (the var's value satisfied its type at any time)
    for (auto& kv : gens) {
      if (kv.first.first != BK_VAR) continue;
      auto tit = idTypes.find(kv.first.second);
      Type* ty = tit == idTypes.end() ? nullptr : tit->second;
      if (!ty || !ty->isInt() || ty->bits() >= 64) continue;
      int64_t mn = 0, mx = 0;
      if (ty->isSigned()) {
        mx = (int64_t(1) << (ty->bits() - 1)) - 1;
        mn = -(int64_t(1) << (ty->bits() - 1));
      } else {
        mx = (int64_t)((uint64_t(1) << ty->bits()) - 1);
        mn = 0;
      }
      for (int g = 0; g <= kv.second; ++g) {
        Base v{BK_VAR, kv.first.second, g};
        if (!d.count(key(v))) continue;  // only nodes the graph mentions
        edges.push_back({zeroBase(), v, -mn});  // mn <= v
        edges.push_back({v, zeroBase(), mx});   // v <= mx
      }
    }

    for (size_t iter = 0; iter <= edges.size(); ++iter) {
      bool changed = false;
      for (auto& e : edges) {
        auto dl = d.find(key(e.l));
        if (dl == d.end() || dl->second == BCE_INF) continue;
        int64_t nd = satAdd(dl->second, e.c);
        auto& dr = d[key(e.r)];
        if (nd < dr) { dr = nd; changed = true; }
      }
      if (!changed) break;
    }
    return d;
  }

  bool query(const Base& l, const Base& r, int64_t c) {
    auto d = dists(l);
    auto it = d.find(key(r));
    return it != d.end() && it->second <= c;
  }

  // ------------------------------------------------- entry-fact seeding
  // goose bce.h `record`: at one direct call site, what THIS (caller)
  // solver proved about the callee's interface — the actual arguments'
  // terms (int params) and their length terms (span/str/soa params) —
  // folded to pairwise `param_x <= param_y + c` constraints, weakest
  // (max) over every site into the callee's table cell
  void recordSite(Expr* e) {
    if (!sites || !e->callee) return;
    auto si = sites->find(e->callee);
    if (si == sites->end()) return;   // callee never judged: no table
    SiteRec& S = si->second;
    S.seen++;
    if (!S.viable) return;
    auto ii = ifaces->find(e->callee);
    if (ii == ifaces->end()) { S.viable = false; return; }
    auto& bases = ii->second;
    int n = (int)bases.size();
    if (n <= 2 || e->args.size() != e->callee->params.size()) {
      S.viable = false;   // arity mismatch / no pair to relate
      return;
    }
    if (S.c.empty()) S.c.assign((size_t)n * n, INT64_MIN);
    std::vector<Term> terms((size_t)n);
    terms[0] = Term{true, zeroBase(), 0};
    for (int k = 1; k < n; ++k) {
      // moved=false: Freshet args are copies, goose's move nuance is N/A
      size_t j = bases[k].second;
      Expr* arg = e->args[j];
      Type* at = arg ? arg->ty : nullptr;
      Type* pt = e->callee->params[j].ty;
      // a width-changing conversion at the boundary (trunc/sext/zext)
      // can move difference relations: that param carries no term here
      // (an untracked term KILLS the pair cells it touches, below)
      if (!bases[k].first && at && pt && at->isInt() && pt->isInt() &&
          at->bits() != pt->bits())
        continue;
      terms[k] = bases[k].first ? lenTermOf(arg) : termOf(arg);
    }
    for (int x = 0; x < n; ++x) {
      // goose: cv defaults to INF — a site whose term for x (or y) is
      // untracked, or proves no distance, must KILL the cell for that
      // pair, not leave another site's constraint behind
      std::map<NKey, int64_t> d;
      if (terms[x].ok) d = dists(terms[x].b);
      for (int y = 0; y < n; ++y) {
        if (x == y) continue;
        int64_t cv = BCE_INF;
        if (terms[x].ok && terms[y].ok) {
          auto it = d.find(key(terms[y].b));
          if (it != d.end() && it->second != BCE_INF)
            cv = satAdd(it->second, terms[x].off - terms[y].off);
        }
        int64_t& cell = S.c[(size_t)x * n + y];
        if (cv > cell) cell = cv;   // the weakest any site states
      }
    }
  }

  // goose SeedEntryFacts: grant the fn, at entry, what ALL of its call
  // sites proved about its parameters — only once every site has been
  // judged, and only when nothing reaches it any other way
  void seedEntry() {
    if (!sites || !nsites || !opaqueSites || !ifaces) return;
    if (opaqueSites->count(fn)) return;
    auto si = sites->find(fn);
    auto ni = nsites->find(fn);
    if (si == sites->end() || ni == nsites->end()) return;
    SiteRec& S = si->second;
    if (getenv("BCEDBG")) fprintf(stderr, "[bcedbg] seedEntry %s: viable=%d seen=%d nsites=%d opaque=%d\n", fn->name.c_str(), (int)S.viable, S.seen, ni->second, (int)opaqueSites->count(fn));
    if (!S.viable || S.seen == 0 || S.seen != ni->second) return;
    auto ii = ifaces->find(fn);
    if (ii == ifaces->end()) return;
    auto& bases = ii->second;
    int n = (int)bases.size();
    std::vector<Base> bs((size_t)n);
    bs[0] = zeroBase();
    for (int k = 1; k < n; ++k) {
      size_t j = bases[k].second;
      const auto& pm = fn->params[j];
      int id = idOf(lkey(pm.name), pm.ty);
      bs[k] = bases[k].first ? Base{BK_LEN, id, genOf(BK_LEN, id)}
                             : Base{BK_VAR, id, genOf(BK_VAR, id)};
    }
    for (int x = 0; x < n; ++x)
      for (int y = 0; y < n; ++y) {
        if (x == y) continue;
        int64_t cv = S.c[(size_t)x * n + y];
        if (getenv("BCEDBG")) fprintf(stderr, "[bcedbg] seed cell(%d,%d)=%lld\n", x, y, (long long)cv);
        if (cv == INT64_MIN || cv >= BCE_INF) continue;  // no relation
        addFact(bs[x], bs[y], cv);
      }
    seeded = true;
    if (getenv("BCEDBG")) fprintf(stderr, "[bcedbg] seeded %s\n", fn->name.c_str());
  }

  // ---------------------------------------------------------------- terms
  Term termOf(Expr* e) {
    Term t;
    if (!e || !e->ty || !e->ty->isInt()) return t;
    switch (e->k) {
      case Expr::INT: {
        TypedConst tc;
        if (!evalConstTyped(e, &tc)) return t;
        t.ok = true;
        t.b = zeroBase();
        t.off = tc.signedVal();  // the literal's signed value
        return t;
      }
      case Expr::IDENT: {
        if (e->sym && (e->sym->k == Sym::VAR || e->sym->k == Sym::PARAM)) {
          int id = idOf(lkey(e->sym->name), e->sym->ty);
          if (addrof.count(id)) return t;
          t.ok = true;
          t.b = cur(BK_VAR, id);
          return t;
        }
        if (e->globalRef) {
          int id = idOf(gkey(e->globalRef->name), e->globalRef->ty);
          if (addrof.count(id)) return t;
          t.ok = true;
          t.b = cur(BK_VAR, id);
          return t;
        }
        return t;
      }
      case Expr::FIELD: {
        // `.len` on a fixed array is the constant; on a span/str binding
        // it is that binding's length base
        if (e->name == "len" && e->base) {
          Type* bt = e->base->ty;
          if (bt && bt->k == Type::ARRAY) {
            t.ok = true;
            t.b = zeroBase();
            t.off = (int64_t)bt->arrLen;
            return t;
          }
          if (bt && (bt->k == Type::SPAN || bt->k == Type::STR) &&
              e->base->k == Expr::IDENT) {
            int id = -1;
            if (e->base->sym)
              id = idOf(lkey(e->base->sym->name), e->base->sym->ty);
            else if (e->base->globalRef)
              id = idOf(gkey(e->base->globalRef->name),
                        e->base->globalRef->ty);
            if (id < 0 || addrof.count(id)) return t;
            t.ok = true;
            t.b = cur(BK_LEN, id);
            return t;
          }
        }
        return t;
      }
      case Expr::CALL: {
        if (e->name == "soa_len" && e->args.size() == 1 &&
            e->args[0]->k == Expr::IDENT) {
          int id = -1;
          if (e->args[0]->sym)
            id = idOf(lkey(e->args[0]->sym->name), e->args[0]->sym->ty);
          else if (e->args[0]->globalRef)
            id = idOf(gkey(e->args[0]->globalRef->name),
                      e->args[0]->globalRef->ty);
          if (id < 0 || addrof.count(id)) return t;
          t.ok = true;
          t.b = cur(BK_LEN, id);
          return t;
        }
        return t;  // call results are untracked (v1; goose's entry-fact
                   // seeding via call-site meets is future work)
      }
      case Expr::CAST: {
        // same-width integer casts preserve the bit pattern; narrower
        // ones reinterpret (zext/sext) and are not carried
        if (e->base && e->base->ty && e->base->ty->isInt() &&
            e->base->ty->bits() == 64)
          return termOf(e->base);
        return t;
      }
      case Expr::BIN: {
        // 64-bit x ± smallconst only: sub-64-bit arithmetic wraps at its
        // own width and must not be reasoned about at i64
        if (!e->lhs || !e->rhs) return t;
        if (!(e->ty && e->ty->isInt() && e->ty->bits() == 64)) return t;
        if (!(e->lhs->ty && e->lhs->ty->isInt() && e->lhs->ty->bits() == 64))
          return t;
        if (!(e->rhs->ty && e->rhs->ty->isInt() && e->rhs->ty->bits() == 64))
          return t;
        if (e->op != "+" && e->op != "-") return t;
        Term a = termOf(e->lhs), b = termOf(e->rhs);
        if (!a.ok || !b.ok) return t;
        if (e->op == "+") {
          if (b.b.k == BK_ZERO) { t = a; t.off += b.off; }
          else if (a.b.k == BK_ZERO) { t = b; t.off += a.off; }
          else return t;  // var + var: no term (goose uses Lin bases)
        } else {
          if (b.b.k != BK_ZERO) return t;  // var - var: no term
          t = a;
          t.off -= b.off;
        }
        if (!smallOff(t.off)) t.ok = false;
        return t;
      }
      default:
        return t;
    }
  }

  // the length term of an INDEX/SLICE base (fixed arrays: the constant;
  // span/str/soa bindings: their length base)
  Term lenTermOf(Expr* base) {
    Term t;
    if (!base || !base->ty) return t;
    Type* bt = base->ty;
    if (bt->k == Type::ARRAY) {
      t.ok = true;
      t.b = zeroBase();
      t.off = (int64_t)bt->arrLen;
      return t;
    }
    if (bt->k == Type::SPAN || bt->k == Type::STR || bt->k == Type::SOA) {
      if (base->k == Expr::IDENT) {
        int id = -1;
        if (base->sym) id = idOf(lkey(base->sym->name), base->sym->ty);
        else if (base->globalRef)
          id = idOf(gkey(base->globalRef->name), base->globalRef->ty);
        if (id >= 0 && !addrof.count(id)) {
          t.ok = true;
          t.b = cur(BK_LEN, id);
        }
      } else if (bt->k == Type::SOA && base->k == Expr::CALL &&
                 base->name == "soa_new") {
        // soa_new(n, ...) leaves the length as an untracked call result
        return t;
      }
    }
    return t;
  }

  // the length a constructor builds (for `let s = ...` len facts)
  Term ctorLenTerm(Expr* init) {
    Term t;
    if (!init) return t;
    if (init->k == Expr::CALL && init->name == "span") {
      if (init->args.size() == 2) return termOf(init->args[1]);
      if (init->args.size() == 1 && init->args[0]->ty &&
          init->args[0]->ty->k == Type::ARRAY) {
        t.ok = true;
        t.b = zeroBase();
        t.off = (int64_t)init->args[0]->ty->arrLen;
      }
      return t;
    }
    if (init->k == Expr::SLICE) {
      Term lot = init->lhs ? termOf(init->lhs) : Term{true, zeroBase(), 0};
      Term hit = init->rhs ? termOf(init->rhs) : lenTermOf(init->base);
      if (!lot.ok || !hit.ok) return t;
      if (hit.b == lot.b) {
        t.ok = true;
        t.b = hit.b;
        t.off = hit.off - lot.off;
      } else if (lot.b.k == BK_ZERO) {
        t.ok = true;
        t.b = hit.b;
        t.off = hit.off - lot.off;
      }
      if (t.ok && !smallOff(t.off)) t.ok = false;
      return t;
    }
    return t;
  }

  // -------------------------------------------------------- cond facts
  void condFacts(Expr* c, bool truth) {
    if (!c) return;
    if (c->k == Expr::UN && c->op == "!") { condFacts(c->rhs, !truth); return; }
    if (c->k == Expr::BIN) {
      if (c->op == "&&" && truth) {
        condFacts(c->lhs, true);
        condFacts(c->rhs, true);
        return;
      }
      if (c->op == "||" && !truth) {
        condFacts(c->lhs, false);
        condFacts(c->rhs, false);
        return;
      }
      static const char* cmps[] = {"<", "<=", ">", ">=", "==", "!="};
      bool isCmp = false;
      for (auto* o : cmps) if (c->op == o) isCmp = true;
      if (!isCmp) return;
      Term a = termOf(c->lhs), b = termOf(c->rhs);
      if (!a.ok || !b.ok) return;
      // x <= y + d  (as a fact over the terms' bases)
      auto le = [&](const Term& x, const Term& y, int64_t d) {
        addFact(x.b, y.b, satAdd(satSub(y.off, x.off), d));
      };
      if (c->op == "<") { if (truth) le(a, b, -1); else le(b, a, 0); }
      else if (c->op == "<=") { if (truth) le(a, b, 0); else le(b, a, -1); }
      else if (c->op == ">") { if (truth) le(b, a, -1); else le(a, b, 0); }
      else if (c->op == ">=") { if (truth) le(b, a, 0); else le(a, b, -1); }
      else if (c->op == "==" && truth) { le(a, b, 0); le(b, a, 0); }
      // `!=` bounds nothing at i64 (goose's strict-vs-side rule)
    }
  }

  // ------------------------------------------------------------ judging
  void recordLine(SrcLoc loc, bool elided) {
    auto& e = lineOut[{loc.file, loc.line}];
    if (elided) ++e.first;
    else ++e.second;
  }

  bool emitsIndexCheck(Expr* e) const {
    Type* bt = e->base ? e->base->ty : nullptr;
    if (!bt) return false;
    if (bt->k == Type::ARRAY) {
      // constants in range are already proven in sema and never checked
      if (e->rhs && e->rhs->k == Expr::INT && e->rhs->constant &&
          e->rhs->ival < bt->arrLen)
        return false;
      return true;
    }
    return bt->k == Type::SPAN || bt->k == Type::STR;
  }

  void judgeIndex(Expr* e, const Term& lt) {
    if (!emitsIndexCheck(e)) return;
    Term it = termOf(e->rhs);
    ++idxTotal;
    recordLine(e->loc, false);
    bool ok = false;
    if (lt.ok && it.ok) {
      // 0 <= idx  AND  idx <= len - 1  (signed — see the file header)
      ok = query(zeroBase(), it.b, it.off) &&
           query(it.b, lt.b, satSub(satSub(lt.off, it.off), 1));
    }
    if (ok) {
      e->bceSafe = true;
      ++idxElided;
      recordLine(e->loc, true);
    } else {
      std::string why = !it.ok
          ? "the index is not a tracked 64-bit integer term"
          : !lt.ok
              ? "the length is not a tracked span/str/array binding here"
              : !query(zeroBase(), it.b, it.off)
                  ? "nothing shows 0 <= the index"
                  : "nothing shows the index < the length (an assert on "
                    "the span's length would settle it)";
      kept.push_back({e->loc, why});
    }
    // completed-check facts: past this site (elided or trapped) the
    // program continues only with 0 <= idx <= len-1
    if (it.ok) addFact(zeroBase(), it.b, it.off);
    if (it.ok && lt.ok)
      addFact(it.b, lt.b, satAdd(satSub(lt.off, it.off), -1));
  }

  void judgeSlice(Expr* e, const Term& lt) {
    Term lot = e->lhs ? termOf(e->lhs) : Term{true, zeroBase(), 0};
    Term hit = e->rhs ? termOf(e->rhs) : lt;  // hi defaults to full length
    ++slTotal;
    recordLine(e->loc, false);
    bool ok = false;
    if (lt.ok && lot.ok && hit.ok) {
      // 0 <= lo <= hi <= len (signed implies the emitted ule chain)
      ok = query(zeroBase(), lot.b, lot.off) &&
           query(lot.b, hit.b, satSub(hit.off, lot.off)) &&
           query(hit.b, lt.b, satSub(lt.off, hit.off));
    }
    if (ok) {
      e->bceSafe = true;
      ++slElided;
      recordLine(e->loc, true);
    } else {
      kept.push_back({e->loc, "the slice bounds are not provable here"});
    }
    if (lot.ok) addFact(zeroBase(), lot.b, lot.off);
    if (lot.ok && hit.ok) addFact(lot.b, hit.b, satSub(hit.off, lot.off));
    if (hit.ok && lt.ok) addFact(hit.b, lt.b, satSub(lt.off, hit.off));
  }

  // ------------------------------------------------------------- walking
  void walkExpr(Expr* e) {
    if (!e) return;
    switch (e->k) {
      case Expr::INDEX: {
        walkExpr(e->base);              // the span value is loaded first
        Term lt = lenTermOf(e->base);   // captured BEFORE the index walks
        walkExpr(e->rhs);
        if (lt.ok && lt.b.k != BK_ZERO &&
            lt.b.gen != genOf(lt.b.k, lt.b.id))
          lt.ok = false;                // the index walk killed the length
        judgeIndex(e, lt);
        return;
      }
      case Expr::SLICE: {
        walkExpr(e->base);
        Term lt = lenTermOf(e->base);
        walkExpr(e->lhs);
        walkExpr(e->rhs);
        if (lt.ok && lt.b.k != BK_ZERO &&
            lt.b.gen != genOf(lt.b.k, lt.b.id))
          lt.ok = false;
        judgeSlice(e, lt);
        return;
      }
      case Expr::CALL: {
        if (e->name == "assert" && e->args.size() == 1) {
          walkExpr(e->args[0]);
          condFacts(e->args[0], true);
          return;
        }
        for (auto* a : e->args) walkExpr(a);
        // entry-fact seeding (goose `record`): what this caller proved
        // about the callee's arguments HERE — before the call's kills
        if (e->callee) recordSite(e);
        if (e->callee) {
          // locals are by-value: only GLOBAL bases can die at a call
          FnFootprint* fp = footprintOf(e->callee);
          if (!fp || fp->writesGlobal || fp->writesUnknown) killGlobals();
        } else if (e->fnPtrSym) {
          killGlobals();  // unknown indirect callee
        }
        // builtins and variant constructors write no tracked base
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

  // bases a statement subtree may write (assign / compound-assign to a
  // name) — the loop-entry havoc set
  void collectWritten(Stmt* s, std::set<std::pair<uint8_t, int>>& out) {
    if (!s) return;
    if (s->k == Stmt::ASSIGN && s->lhs && s->lhs->k == Expr::IDENT) {
      Type* ty = nullptr;
      std::string key;
      if (s->lhs->sym) { key = lkey(s->lhs->sym->name); ty = s->lhs->sym->ty; }
      else if (s->lhs->globalRef) {
        key = gkey(s->lhs->globalRef->name);
        ty = s->lhs->globalRef->ty;
      }
      if (!key.empty()) {
        int id = idOf(key, ty);
        if (ty && ty->isInt()) out.insert({BK_VAR, id});
        if (ty && (ty->k == Type::SPAN || ty->k == Type::STR ||
                   ty->k == Type::SOA))
          out.insert({BK_LEN, id});
      }
    }
    if (s->k == Stmt::BREAK || s->k == Stmt::CONTINUE) return;
    for (auto* st : s->stmts) collectWritten(st, out);
    collectWritten(s->body, out);
    collectWritten(s->thenB, out);
    collectWritten(s->elseB, out);
    for (auto& arm : s->matchArms) collectWritten(arm.body, out);
  }

  static bool hasBreak(Stmt* s) {
    if (!s) return false;
    if (s->k == Stmt::BREAK) return true;
    for (auto* st : s->stmts) if (hasBreak(st)) return true;
    return hasBreak(s->body) || hasBreak(s->thenB) || hasBreak(s->elseB);
  }

  void havocWritten(const std::set<std::pair<uint8_t, int>>& w) {
    for (auto& kv : w) bump(kv.first, kv.second);
  }

  // statement list walk: LETs bind their (name-keyed) base to the
  // initializer's term — the base is bumped FIRST (the binding rewrites
  // the shared storage), then the new value's facts are stated
  void walkList(std::vector<Stmt*>& list) {
    for (size_t i = 0; i < list.size(); ++i) {
      Stmt* s = list[i];
      if (!s) continue;
      if (s->k == Stmt::LET) {
        walkExpr(s->init);
        if (s->var.empty()) continue;
        std::string key = lkey(s->var);
        int id = idOf(key);
        if (addrof.count(id)) continue;
        Type* bty = s->hasAnn && s->annTy ? s->annTy
                                          : (s->init ? s->init->ty : nullptr);
        if (bty) idTypes[id] = bty;
        // the binding rewrites the name's shared storage (IRgen allocas
        // are name-keyed) — open a fresh generation before the facts
        if (bty && bty->isInt()) bump(BK_VAR, id);
        if (bty && (bty->k == Type::SPAN || bty->k == Type::STR))
          bump(BK_LEN, id);
        if (s->init && s->init->ty && bty) {
          if (s->init->ty->isInt() && bty->isInt()) {
            // goose RecordSet: the binding equals its initializer's term
            Term t = termOf(s->init);
            if (t.ok) {
              Base b = cur(BK_VAR, id);
              addFact(b, t.b, t.off);
              addFact(t.b, b, -t.off);
            }
          } else if (s->init->ty->k == Type::SPAN ||
                     s->init->ty->k == Type::STR) {
            Term lt = ctorLenTerm(s->init);
            if (lt.ok) {
              Base b = cur(BK_LEN, id);
              addFact(b, lt.b, lt.off);
              addFact(lt.b, b, -lt.off);
            }
          }
        }
        continue;
      }
      walkStmt(s);
    }
  }

  void walkStmt(Stmt* s) {
    if (!s) return;
    switch (s->k) {
      case Stmt::LET: {
        // LETs inside a statement list are handled by walkList
        // (positional Sym resolution); a body reached through a bare
        // Stmt* pointer (no list) lands here
        walkExpr(s->init);
        return;
      }
      case Stmt::ASSIGN: {
        // IRgen's evaluation order: compound ops emit the lhs address and
        // load FIRST, plain assignment the rhs value first — the walk
        // matches so completed-check facts line up with the emitted checks
        if (s->op2 != "=") {
          walkExpr(s->lhs);
          walkExpr(s->rhs);
        } else {
          walkExpr(s->rhs);
          walkExpr(s->lhs);
        }
        if (s->lhs && s->lhs->k == Expr::IDENT) {
          if (s->lhs->sym)
            killKey(lkey(s->lhs->sym->name), s->lhs->sym->ty);
          else if (s->lhs->globalRef)
            killKey(gkey(s->lhs->globalRef->name), s->lhs->globalRef->ty);
        }
        return;
      }
      case Stmt::IF: {
        walkExpr(s->cond);
        Flow save = snap();
        condFacts(s->cond, true);
        walkStmt(s->thenB);
        Flow thenF = snap();
        restore(save);
        condFacts(s->cond, false);
        walkStmt(s->elseB);
        meet(thenF);
        return;
      }
      case Stmt::WHILE: {
        walkExpr(s->cond);
        std::set<std::pair<uint8_t, int>> written;
        collectWritten(s->body, written);
        havocWritten(written);          // per-iteration havoc
        condFacts(s->cond, true);       // holds at every body entry
        walkStmt(s->body);
        havocWritten(written);
        if (!hasBreak(s->body)) condFacts(s->cond, false);  // exit tested it
        return;
      }
      case Stmt::FOR: {
        if (s->forRange) {
          walkExpr(s->rangeLo);
          Term lot = termOf(s->rangeLo);
          walkExpr(s->rangeHi);
          Term hit = termOf(s->rangeHi);
          std::set<std::pair<uint8_t, int>> written;
          collectWritten(s->body, written);
          havocWritten(written);
          if (!s->ivar.empty()) {
            int iid = idOf(lkey(s->ivar));
            if (!addrof.count(iid)) {
              // fresh induction value: the loop rewrites the shared
              // storage each iteration (and any earlier loop of the same
              // name left exit facts behind)
              bump(BK_VAR, iid);
              Base ib = cur(BK_VAR, iid);
              // bounds evaluated once at entry; the facts are the loop's
              // snapshot (pinned generations) exactly like goose's for-facts
              if (lot.ok) addFact(lot.b, ib, -lot.off);      // lo <= i
              if (hit.ok) addFact(ib, hit.b, satSub(hit.off, 1));  // i < hi
            }
            walkStmt(s->body);
            havocWritten(written);
            if (!addrof.count(iid) && hit.ok && !hasBreak(s->body)) {
              // the exit tested !(i < hi): i >= hi
              addFact(hit.b, cur(BK_VAR, iid), -hit.off);
            }
          } else {
            walkStmt(s->body);
            havocWritten(written);
          }
        } else {
          // C-style for: no range facts (v1); plain havoc + walk
          walkExpr(s->forInit);
          walkExpr(s->forCond);
          walkExpr(s->forStep);
          std::set<std::pair<uint8_t, int>> written;
          collectWritten(s->body, written);
          havocWritten(written);
          walkStmt(s->body);
          havocWritten(written);
        }
        return;
      }
      case Stmt::PARFOR: {
        walkExpr(s->plo);
        Term lot = termOf(s->plo);
        walkExpr(s->phi);
        Term hit = termOf(s->phi);
        std::set<std::pair<uint8_t, int>> written;
        collectWritten(s->body, written);
        havocWritten(written);          // captured scalars are per-chunk
        if (!s->pvar.empty()) {
          int pid = idOf(lkey(s->pvar));
          if (!addrof.count(pid)) {
            bump(BK_VAR, pid);
            Base ib = cur(BK_VAR, pid);
            if (lot.ok) addFact(lot.b, ib, -lot.off);
            if (hit.ok) addFact(ib, hit.b, satSub(hit.off, 1));
          }
        }
        walkStmt(s->body);
        havocWritten(written);
        return;
      }
      case Stmt::MATCH: {
        walkExpr(s->matchOn);
        Flow save = snap();
        std::vector<Flow> outs;
        for (auto& arm : s->matchArms) {
          restore(save);
          walkStmt(arm.body);
          outs.push_back(snap());
        }
        outs.push_back(save);  // a non-exhaustive match falls through
        restore(save);
        for (size_t i = 1; i < outs.size(); ++i) meet(outs[i]);
        return;
      }
      case Stmt::BLOCK: {
        walkList(s->stmts);
        walkExpr(s->expr);
        return;
      }
      case Stmt::RETURN:
        walkExpr(s->retVal);
        return;  // the path ends here
      case Stmt::DEFER:
        // the deferred call runs LATER; applying its kills now is
        // conservative (more killing), never unsound
        walkExpr(s->deferExpr);
        return;
      case Stmt::BREAK:
      case Stmt::CONTINUE:
        havocAll();  // exits the straight-line path conservatively
        return;
      case Stmt::EXPR:
        walkExpr(s->expr);
        return;
      default:
        return;
    }
  }

  // -------------------------------------------------------------- prescan
  void prescan() {
    // address-taken bases: `&x` is the only way a by-value local's storage
    // escapes, so those bases are untracked entirely
    std::function<void(Expr*)> we = [&](Expr* e) {
      if (!e) return;
      if (e->k == Expr::UN && e->op == "&" && e->rhs &&
          e->rhs->k == Expr::IDENT) {
        if (e->rhs->sym)
          addrof.insert(idOf(lkey(e->rhs->sym->name), e->rhs->sym->ty));
        if (e->rhs->globalRef) {
          int gid = idOf(gkey(e->rhs->globalRef->name),
                         e->rhs->globalRef->ty);
          addrof.insert(gid);
          globalIds.insert(gid);
        }
      }
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
    ws(fn->body);
    // global bases (killed by unknown-effect calls)
    if (prog)
      for (auto* g : prog->globals)
        globalIds.insert(idOf(gkey(g->name), g->ty));
  }

  void run() {
    prescan();
    seedEntry();   // goose: entry facts before the first body walk
    walkStmt(fn->body);
    if (idxElided + slElided > 0) {
      char extra[144];
      snprintf(extra, sizeof extra,
               "kind=elided,idx=%d/%d,slice=%d/%d%s", idxElided, idxTotal,
               slElided, slTotal, seeded ? ",entry=1" : "");
      diag(Diag::Note, Code::Bce,
           std::to_string(idxElided + slElided) + " of " +
               std::to_string(idxTotal + slTotal) +
               " bounds checks proven safe — a proven check cannot fire, "
               "so it is not emitted",
           fn->loc, extra);
      proofLogLine("bce",
                   std::string("difference-constraints;generation-pinning;"
                               "type-axioms;loop-range-facts;cond-facts;"
                               "ctor-facts;completed-check-facts") +
                       (seeded ? ";entry-facts" : ""),
                   fn, fn->loc);
    }
  }
};

}  // namespace

BceCounts runBcePass(Program* p) {
  computeFootprints(p);
  BceCounts total;

  // ---- census (goose BuildCallGraph): direct call sites per callee,
  // opaque uses (fn values, exports, @test entries), each judged fn's
  // interface bases, and the direct-call graph shared with @checkfree
  std::map<FnDecl*, int> nsites;
  std::set<FnDecl*> opaqueSites;
  std::map<FnDecl*, std::vector<std::pair<bool, size_t>>> iface;
  std::map<FnDecl*, std::set<FnDecl*>> calls;
  std::vector<FnDecl*> judged;
  for (auto* fn : p->fns) {
    if (!fn->body || fn->imported || fn->isGeneric) continue;
    judged.push_back(fn);
    // entry points no analysed site covers: exports (external C callers)
    // and @test fns (the test runner passes their value to frt_test_run)
    if (fn->exported || fn->isTest) opaqueSites.insert(fn);
    // the interface: (isLen, param idx) with a LEADING ZERO marker at
    // slot 0 (goose's InterfaceOf: base 0 is the constant zero, params
    // follow) — SIZE_MAX marks the zero slot and is never dereferenced
    {
      std::vector<std::pair<bool, size_t>> bs;
      bs.push_back({false, SIZE_MAX});
      for (size_t i = 0; i < fn->params.size(); ++i) {
        Type* t = fn->params[i].ty;
        if (!t) continue;
        if (t->isInt()) bs.push_back({false, i});
        else if (t->k == Type::SPAN || t->k == Type::STR ||
                 t->k == Type::SOA)
          bs.push_back({true, i});
      }
      iface[fn] = std::move(bs);
    }
    std::function<void(Expr*)> ce = [&](Expr* e) {
      if (!e) return;
      if (e->k == Expr::CALL && e->callee) {
        calls[fn].insert(e->callee);
        FnDecl* g = e->callee;
        if (g->body && !g->imported && !g->isGeneric) {
          ++nsites[g];
        } else {
          opaqueSites.insert(g);   // unanalysable target: never seed it
        }
      }
      // a fn VALUE drops out of the direct-site census: its target is
      // opaque no matter where the pointer ends up called
      if (e->k == Expr::IDENT && e->fnValue) opaqueSites.insert(e->fnValue);
      ce(e->lhs); ce(e->rhs); ce(e->base);
      for (auto* a : e->args) ce(a);
    };
    std::function<void(Stmt*)> cs = [&](Stmt* s) {
      if (!s) return;
      cs(s->body); cs(s->thenB); cs(s->elseB);
      for (auto* st : s->stmts) cs(st);
      for (auto& arm : s->matchArms) cs(arm.body);
      switch (s->k) {
        case Stmt::LET: ce(s->init); break;
        case Stmt::ASSIGN: ce(s->lhs); ce(s->rhs); break;
        case Stmt::RETURN: ce(s->retVal); break;
        case Stmt::DEFER: ce(s->deferExpr); break;
        case Stmt::IF: case Stmt::WHILE: ce(s->cond); break;
        case Stmt::FOR:
          ce(s->forInit); ce(s->forCond); ce(s->forStep);
          ce(s->rangeLo); ce(s->rangeHi); break;
        case Stmt::PARFOR:
          ce(s->plo); ce(s->phi);
          for (auto& c : s->clauses) ce(c.e);
          break;
        case Stmt::MATCH: ce(s->matchOn); break;
        case Stmt::EXPR: ce(s->expr); break;
        default: break;
      }
    };
    cs(fn->body);
  }

  // ---- judge order: CALLERS FIRST (goose's calleesfirst.rbegin()) — a
  // fn's entry facts come from its call sites, which must have been
  // judged; recursion inside a cycle leaves someone's seen < counted,
  // which just skips that fn's seeding
  std::vector<FnDecl*> order;
  {
    std::set<FnDecl*> visited;
    std::function<void(FnDecl*)> visit = [&](FnDecl* f) {
      if (!visited.insert(f).second) return;
      auto cit = calls.find(f);
      if (cit != calls.end())
        for (FnDecl* g : cit->second) visit(g);
      order.push_back(f);
    };
    for (auto* fn : judged) visit(fn);
    std::reverse(order.begin(), order.end());
  }

  // ---- per-fn solver runs + kept-site records
  std::map<FnDecl*, BCE::SiteRec> sites;
  for (auto& kv : nsites) sites[kv.first] = BCE::SiteRec{};
  std::map<FnDecl*, std::vector<Kept>> keptByFn;
  std::set<FnDecl*> checkfreeFns;
  for (FnDecl* fn : order) {
    if (fn->checkfree) checkfreeFns.insert(fn);
    BCE b(p, fn, &sites, &nsites, &opaqueSites, &iface);
    b.run();
    total.idxElided += b.idxElided;
    total.idxTotal += b.idxTotal;
    total.slElided += b.slElided;
    total.slTotal += b.slTotal;
    total.entrySeeded += b.seeded ? 1 : 0;
    keptByFn[fn] = b.kept;
    if (g_opts.bceLinesOn)
      for (auto& kv : b.lineOut)
        fprintf(stderr, "bce-line: %s:%d: %lld elided, %lld kept\n",
                kv.first.first.c_str(), kv.first.second,
                (long long)kv.second.first, (long long)kv.second.second);
  }

  // ---- @checkfree enforcement (goose: "no bounds check runs while a
  // checkfree function does") — transitive over direct calls
  for (FnDecl* f : checkfreeFns) {
    // own body
    for (auto& k : keptByFn[f])
      diag(Diag::Error, Code::Checkfree,
           "checkfree fn " + f->name + " keeps a bounds check — " + k.why,
           k.loc, "fn=" + f->name);
    // transitive callees
    std::set<FnDecl*> seen;
    std::vector<FnDecl*> work{f};
    while (!work.empty()) {
      FnDecl* g = work.back();
      work.pop_back();
      for (FnDecl* h : calls[g]) {
        if (h->isGeneric || h->imported) continue;  // no Freshet checks
        if (!seen.insert(h).second) continue;
        work.push_back(h);
        for (auto& k : keptByFn[h])
          diag(Diag::Error, Code::Checkfree,
               "checkfree fn " + f->name + " calls " + h->name +
                   ", which keeps a bounds check — " + k.why,
               k.loc, "fn=" + f->name + ",callee=" + h->name);
      }
    }
  }
  return total;
}

}  // namespace freshet
