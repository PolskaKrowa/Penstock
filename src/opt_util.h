// Freshet source-level loop optimisation — shared analysis utilities (R4).
// Used by fuse_unroll.cpp (loop fusion + full unrolling) and
// lane_analysis.cpp (cross-lane vectoriser analysis: uniform-branch
// unswitching + same-condition predicate reuse).
//
// Everything here runs POST-sema: expressions carry resolved types
// (e->ty), symbols (e->sym), globals (e->globalRef) and callees
// (e->callee); IRgen addresses locals purely by NAME through
// varAllocas, so AST restructures that reuse or clone nodes (copying
// every sema field) and never introduce NEW bindings are safe.
#pragma once
#include "common.h"
#include <map>
#include <set>
#include <vector>
#include <string>

namespace freshet {
namespace optutil {

// ---------------------------------------------------------------- summaries
// Interprocedural memory footprint per user function.  "May" information,
// computed to a fixed point (handles recursion).  Used to decide:
//   - what memory a call inside a loop may touch (write roots / read
//     roots / globals / unknown), and
//   - whether writes+reads through a pointer PARAMETER are provably
//     confined to [base, base + count) where `count` is another
//     parameter — the proof shape that makes call-slice fusion (the
//     FFT batch pattern) sound.
struct FnFootprint {
  bool writesGlobal = false;     // stores to any global
  bool readsGlobal = false;      // loads from any global
  bool writesUnknown = false;    // free/memcpy/memset/soa_free/give-like
  bool readsUnknown = false;     // reads through undecomposable pointers
  std::vector<char> paramWrites;      // per param: may write through it
  std::vector<char> paramReads;       // per param: may read through it
  std::vector<int>  writeBound;       // per param: idx of count param, -1 = unbounded
  std::vector<int>  readBound;        // per param: idx of count param, -1 = unbounded
};

void computeFootprints(Program* p);                 // idempotent
FnFootprint* footprintOf(FnDecl* f);                // null if unknown callee

// ---------------------------------------------------------------- aliasing
// Freshet's only callee-writes-into-caller-memory idiom is rebinding a
// param into a mutable local (`let mut w: *T = p; w[j] = ...`).  A
// flow-insensitive alias map resolves such local pointer bindings back
// to the roots (params / globals / caller arrays) they may point into,
// so footprint and access collection stay sound.
struct AliasMap {
  std::map<std::string, std::set<std::string>> alias; // name -> target names
  std::set<std::string> wild;                         // name -> unknown memory
};
void buildAliasMap(Stmt* body, AliasMap* m);
// resolve `name` to the set of base roots it may denote; returns false
// when the name may alias unknown memory (caller must treat as wild)
bool resolveAlias(const AliasMap& m, const std::string& name,
                  std::set<std::string>& out, int depth = 0);

// ---------------------------------------------------------------- expressions
// canonical string form: commutative operands sorted so a+b == b+a
std::string canon(Expr* e);
// like canon(), but IDENTs matching the induction variable render as
// "$IV" — so index expressions from loops with different ivar NAMES
// (k, j, ...) compare equal when they denote the same per-iteration
// address function
std::string canonN(Expr* e, const std::string& iv);
bool sameExpr(Expr* a, Expr* b);                    // structural equality

// constant folding of integer literal shapes (INT, unary -, int casts).
// LEGACY narrow interface (kept for callers that only need small signed
// values).  It is now a thin, exact wrapper over the typed evaluator:
// unsigned values that do not fit int64_t return false ("not foldable")
// instead of silently reinterpreting the bit pattern.
bool evalConstInt(Expr* e, int64_t* out);

// ---------------------------------------------------------------- typed
// constants (optimisation-audit §4.4 / §6.1)
// Bit-vector constant with explicit width and signedness, mirroring the
// semantics of the generated LLVM IR:
//   - add/sub/mul wrap at the source width (no host-C++ UB anywhere);
//   - a narrowing cast TRUNCATES (it is not the identity on integers);
//   - a widening cast zero-extends the stored bits (signedness is applied
//     at interpretation points: comparisons, division, printing);
//   - division/remainder fold ONLY for a provably non-zero constant
//     divisor (and never for INT_MIN / -1, which is poison in LLVM);
//   - shifts fold only for a constant amount below the width;
//   - anything the model cannot decide exactly returns valid = false —
//     callers must treat that as "not foldable", never guess.
struct TypedConst {
  uint64_t bits = 0;      // zero-extended bit pattern
  int width = 0;          // 1..64 (0 => invalid)
  bool isSigned = false;
  bool valid = false;
  int64_t signedVal() const {          // sign-interpretation of `bits`
    if (width <= 0 || width > 64) return 0;
    uint64_t m = width == 64 ? ~0ull : ((1ull << width) - 1);
    uint64_t b = bits & m;
    if (width == 64) return (int64_t)b;
    int64_t signBit = (int64_t)((b >> (width - 1)) & 1);
    int64_t v = (int64_t)b;
    if (signBit) v -= ((int64_t)1 << width);   // two's-complement extend
    return v;
  }
  uint64_t unsignedVal() const {
    if (width <= 0 || width > 64) return 0;
    uint64_t m = width == 64 ? ~0ull : ((1ull << width) - 1);
    return bits & m;
  }
};
bool evalConstTyped(Expr* e, TypedConst* out);

// exact constant-trip information for a `for (iv in lo..hi)` loop,
// computed with __int128-checked distance arithmetic (never host-C++
// overflow, never signed wraparound): `hi - lo` in the OLD code could
// overflow before the bounds check ran (audit §4.4).
struct TripInfo {
  bool known = false;      // both bounds are typed constants
  bool empty = false;      // known and hi <= lo  (zero iterations)
  int64_t trip = 0;        // exact count when known && !saturated
  bool saturated = false;  // mathematical distance exceeds int64 range
};
TripInfo tripCountOf(Stmt* rangeLoop);

// referential transparency of a predicate (audit §4.1): the expression
// has no observable effect and, evaluated twice with no interleaved
// writes to what it reads, yields the same value — i.e. every call is
// to a pure, WILL-RETURN function or a memory-free builtin under the
// CURRENT floating-point mode (strict FP keeps sin/cos/... as errno-
// touching libm calls, so they are NOT referentially transparent there).
// Reads of memory are allowed; callers must separately prove no writes
// happen between the two evaluations.
bool isRTBuiltinCall(const std::string& n, bool strictfp);
bool exprReferentiallyTransparent(Expr* e);

// totality (non-trapping, non-diverging) of a predicate (audit §4.2):
// safe to evaluate on a ZERO-TRIP loop where the source program would
// never evaluate it.  Rejects division/remainder/shift by anything but a
// representable constant, memory dereferences, and every call that may
// not return.
bool exprTotalAndPure(Expr* e);

// is this call name an obviously pure, memory-free builtin?
bool isPureBuiltinCall(const std::string& n);       // sqrt, fabs, ..., freshet.threads

// all identifier names referenced by an expression (scalars)
void collectIdents(Expr* e, std::set<std::string>& out);
// memory roots READ by an expression (INDEX/FIELD loads, soa_len, call args)
void exprReadRoots(Expr* e, std::set<std::string>& out);
// does the expression contain any call that is not pure-and-memory-free?
bool hasImpureCall(Expr* e);

// affine in `iv`: iv appears exactly once, multiplied by a nonzero INT
// literal (or bare) — i.e. the address function is injective over the
// iteration variable (needed to prove "same canonical index string ==
// same iteration").
bool affineIvarLiteral(Expr* e, const std::string& iv);

// ---------------------------------------------------------------- cloning
// deep clone; `substBySym` replaces IDENT nodes whose sema Sym* matches
// (induction-variable renaming).  All sema-cached fields are copied.
Expr* cloneExpr(const Expr* e, const std::map<Sym*, Expr*>& substBySym);
Stmt* cloneStmt(const Stmt* s, const std::map<Sym*, Expr*>& substBySym);

// ---------------------------------------------------------------- accesses
// One memory access to a ROOT (a local pointer/array/soa var or a global)
// collected from a loop body:
struct Access {
  enum Kind { SCALAR,    // whole binding (no index): scalar var, bare ptr
              DIRECT,    // root[<idx>] / root[<idx>].f  (idx canonicalised)
              SLICE,     // &root[iv*C] / root + iv*C passed to a call,
                         // with the callee footprint bounded by C
              WHOLE,     // bare pointer passed to a call (whole array)
              WILDCARD } kind;
  std::string idx;       // DIRECT: canonical idx; SLICE: canonical C
  std::string t3c;       // DIRECT with tier-3 shape: canonical slice C
                         // ("" when not slice-shaped)
  bool hasIvar = false;  // idx mentions the loop induction var
  bool affineLit = false;// DIRECT: affine-in-ivar with literal coef
  bool t3 = false;       // DIRECT: slice-shaped (iv*C + inner-loop vars
                         // bounded by C) → tier-3 compatible
  bool bounded = false;  // SLICE: stride C == callee extent arg
  bool isWrite = false;
  SrcLoc loc;
};

struct LoopCtx {
  std::string var;       // loop variable name
  Stmt* loop = nullptr;  // the FOR stmt (for range checks)
  std::string hiCanon;   // canon(rangeHi)
};

struct BodyInfo {
  // root name -> accesses
  std::map<std::string, std::vector<Access>> reads, writes;
  bool unknownWrite = false;    // some call touches undecomposable memory
  bool unknownRead = false;     // ... on the read side
  bool hasReturn = false;
  bool hasBreak = false;   // Phase 1a: break/continue inside the body
  bool hasPrint = false;        // print*/assert: observable ordering
  bool hasParfor = false;
  bool hasGive = false;
  std::set<std::string> escapedNames;  // &x / x passed as pointer somewhere
  int nodes = 0;                // rough statement+expression size
  int laneWork = 0;             // arithmetic + memory ops (vectorisable work)
  int branches = 0;             // IF statements
};

// Collect reads/writes of a loop (or any statement) subtree.  `iv` is
// the induction variable of the loop being analysed ("" when the set is
// for a non-loop context); enclosing-loop bounds context enables the
// tier-3 slice decomposition `iv*C + j`.
void collectBody(Stmt* body, const std::string& iv, FnDecl* fn, BodyInfo* out);

// convenience: all write roots of a BodyInfo
inline std::set<std::string> writeRoots(const BodyInfo& b) {
  std::set<std::string> s;
  for (auto& kv : b.writes) s.insert(kv.first);
  return s;
}
inline std::set<std::string> readRootsOf(const BodyInfo& b) {
  std::set<std::string> s;
  for (auto& kv : b.reads) s.insert(kv.first);
  return s;
}

// statement list of a loop/if body ("{ ... }" bodies are BLOCKs, but a
// single statement is legal too)
inline std::vector<Stmt*>* stmtsOf(Stmt* body) {
  if (body && body->k == Stmt::BLOCK) return &body->stmts;
  return nullptr;
}

// §5.2/§10.4 proof artefact: append one line per APPLIED source-level
// transform, recording the legality premises it relied on.  Enabled with
// --proof-log[=<file>]; a no-op otherwise.  The Lean kernel and Alive2
// remain the verification gates — this log makes the pass contract
// auditable and diffable.
void proofLogLine(const std::string& pass, const std::string& premises,
                  FnDecl* fn, SrcLoc loc);

} // namespace optutil
} // namespace freshet
