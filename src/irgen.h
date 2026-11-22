// Freshet IRgen shared state.
#pragma once
#include "common.h"
#include <fstream>
#include <sstream>

namespace freshet {

struct FnProf {
  uint64_t hash = 0;
  uint64_t count = 0;      // from loaded profile
  bool loaded = false;
};
struct BrProf {
  uint64_t hash = 0;
  uint64_t taken = 0, notTaken = 0;
  bool loaded = false;
  SrcLoc loc;
  int siteIdx = -1;
};

struct IrGen {
  Program* prog = nullptr;
  std::ostringstream out;    // buffered; written to file at end

  // ---------------- emission state (current function)
  FnDecl* curFn = nullptr;
  FnSema* curFs = nullptr;
  int tmp = 0;
  int bb = 0;              // current label number
  std::vector<int> bbStack;
  std::string curLabel;
  bool curLabelTerminated = false;
  int dbgCounter = 0;
  int loopMetaBase = 0;
  int profMetaBase = 0;
  std::map<std::string, int> varSlots;     // var -> alloca number
  std::map<std::string, std::string> varAllocas; // var -> alloca reg
  // Scope-aware variable slots: a REDECLARED name (a match-arm binding
  // whose block ended, a later same-name let) gets its OWN alloca — the
  // historical name-keyed dedupe reused the first binding's slot, so
  // `Some(v) => …` (i32) followed by `let mut v: Vec<i64>` memcpy'd 24
  // bytes into a 4-byte slot and smashed the stack.  Keyed by
  // name@declLoc (every declare() copies declLoc into allSyms; resolved
  // IDENTs carry the same declLoc through e->sym).  varAllocas keeps the
  // name-keyed view — the parfor machinery swaps it for chunk-fn shadow
  // registers (symAllocas is cleared alongside it there), and unresolved
  // recovery shapes fall back to it.
  std::map<std::string, std::string> symAllocas;
  std::string slotOf(const std::string& name, const SrcLoc& declLoc) {
    std::string k = name + "@" + std::to_string(declLoc.startOffset) +
                     ":" + std::to_string(declLoc.line);
    auto it = symAllocas.find(k);
    if (it != symAllocas.end()) return it->second;
    auto n = varAllocas.find(name);
    return n == varAllocas.end() ? "" : n->second;
  }
  std::vector<std::string> deferStack;     // IR text of deferred calls
  int parforCounter = 0;

  // ---- data stacks (goose port: bump-pointer scope memory) -------------
  // per-function emission state: the current stack-index expression base
  // ("%sp" for a stack-using fn, "0" for entries and plain callers) and
  // the live bump-restores (slot expression, base value) of the scopes
  // currently open — BLOCK ends, break/continue and returns emit the
  // watermark restores from this stack (see irgen_stmt.cpp).
  std::string curSpExpr = "0";
  std::vector<std::pair<std::string, std::string>> dstkRestores;

  // Phase 1a (report §4.4): break/continue targets + the defer-stack
  // depth at loop entry (defers registered inside the body run before the
  // branch leaves the loop)
  struct LoopCtx {
    int breakBB = -1;
    int continueBB = -1;
    size_t deferBase = 0;
    bool isParfor = false;
    // data stacks (goose port): live bump-restores at loop entry — a
    // break/continue restores the watermarks of scopes opened INSIDE the
    // loop, never the loop's own enclosing bumps
    size_t dstkBase = 0;
    // fsht-02:01: container drops registered at loop entry — break /
    // continue flush the body-scoped drops before branching out
    size_t dropBase = 0;
  };
  std::vector<LoopCtx> loopStack;

  // fast-math (report §8 Option A): strict IEEE-754-like per-operation
  // semantics are the DEFAULT; `@fastmath` on the fn or `--fast-math`
  // globally opts into the relaxed contract. `@strictfp` always wins.
  // Phase 5 (report §12): @reproducible reductions pin strict FP inside
  // their region (forceStrictFp > 0) so the accumulator chain is never
  // reassociated — bitwise-identical results for any thread count.
  int forceStrictFp = 0;
  std::string fflag() const {
    if (forceStrictFp > 0) return "";
    bool fast = g_opts.fastMathGlobal || (curFn && curFn->fastmath);
    if (curFn && curFn->strictfp) fast = false;
    return fast ? "fast" : "";
  }

  // ---------------- module-level pools
  std::map<std::string, std::string> structTypes;
  std::map<std::string, std::string> strPool;
  std::function<std::string(const std::string&)> internStr;
  std::map<std::string, std::string> soaSizeConsts;   // struct name -> const def
  bool assertEmitted = false;
  std::vector<FnProf> fnProf;
  std::vector<BrProf> brProf;
  std::vector<std::string> strConsts;
  int numUserAttrs = 4;
  int attrBase = 5;
  // ---- audit §2.3: effect-derived attribute groups -------------------
  // Groups #50-#53/#60/#61 stay fixed (historical numbering; the runtime
  // .ll uses #0..#9 so concatenation never collides).  Every other
  // attribute combination discovered from the sema effect summaries
  // (strictfp x effects, inline-hint x effects, memory(argmem: read),
  // memory(none) without willreturn, ...) is allocated here on first
  // use from #62 up, so each emitted group is exactly as strong as the
  // proof behind it — never stronger.
  std::map<std::string, int> attrGroupIds;          // body -> group id
  std::vector<std::pair<std::string, int>> dynAttrGroups;  // print order
  int nextAttrGroup = 62;
  int attrGroupFor(FnDecl* f);         // canonical group for def + calls
  std::string fnAttrBody(FnDecl* f);  // canonical attr set of a function
  // §5 loop-hint policy shared by all loop lowerings: returns -1 when no
  // vectorisation metadata should be attached (default), a plain enable
  // hint id for @vectorize, or the historical enable+interleave-4 hint
  // id under --force-vectorize
  int vectorizeLoopMeta(bool userHint);
  std::vector<std::string> loopMetas;
  std::vector<std::string> profMetas;      // "!prof" metadata text
  std::vector<std::string> dbgNodes;       // DILocation text nodes
  int dbgCu = -1;
  // Phase 3c (report §10.3): dynamically-numbered DWARF metadata —
  // DILocations 300..889, DISubprograms 902+, DIFiles 1902+.  The
  // historical fixed "902 + fn->id" numbering capped at 32 fns and
  // collided with synthesized decls (test harness, generic instances).
  std::map<FnDecl*, int> dbgSubprogs;      // fn -> DISubprogram id
  std::map<std::string, int> dbgFiles;     // source path -> DIFile id
  int nextDbgSub = 902;
  int nextDbgFile = 1902;
  std::vector<std::pair<std::string,int>> pendingDbgLocs; // (instr comment, line)
  int curDbgLine = -1;
  bool curFnIsChunkFn = false;
  std::vector<std::string> chunkFns;       // deferred chunk fn texts
  std::vector<std::tuple<uint64_t,std::string,int>> parforTags; // (tag, fn, line)

  // ---------------- helpers
  std::string t() { return "%" + std::to_string(tmp++); }
  std::string l(int n) { return "bb" + std::to_string(n); }
  void nl() { out << "\n"; }
  void emitLabel(int n) {
    if (curLabelTerminated) {
      out << l(n) << ":\n";
      curLabel = l(n);
      curLabelTerminated = false;
    } else {
      out << "br label %" << l(n) << "\n";
      out << l(n) << ":\n";
      curLabel = l(n);
    }
  }
  void terminate() { curLabelTerminated = true; }
  // dbg location for current line
  std::string dbg(int line, int col = 0) {
    if (line <= 0) return "";
    int n = dbgNodeFor(line, col);
    if (n < 0) return "";
    return ", !dbg !" + std::to_string(n);
  }
  int dbgNodeFor(int line, int col);
  int dbgSubprogId(FnDecl* f);
  int dbgFileId(const std::string& path);
  void emitFunction(FnDecl* f, FnSema* fs);
  void emitProfileData();
  void emitHeader();
  void emitStructTypes();
  void emitGlobals();
  void emitRuntimeDecls();
  void emitFooter();
  std::string llvmType(Type* t);
  // value representation classes: SPAN/STR are first-class { ptr, i64 }
  // aggregates; STRUCT/ENUM/ARRAY values are ADDRESSES of their storage
  bool isPairValueTy(Type* t);
  bool isAddrValueTy(Type* t);
  // codegen type for SCALAR VARIABLE SLOTS: bool locals live as i1 in
  // their (i8-sized) slots — literal/condition bools are produced as i1,
  // so loads/stores must match (R4 fix; struct fields/params keep the
  // i8 C ABI)
  std::string scalarCodegenTy(Type* t) {
    return (t && t->k == Type::BOOL) ? std::string("i1") : llvmType(t);
  }
  std::string constOf(Type* t, Expr* e);
  std::string doubleToHex(double d);
  std::string floatToHex(float f);
  std::string esc(const std::string& s);
  std::string allocaFor(const std::string& name, Type* t);
};

extern IrGen* G;

// statement/expression lowering (irgen_expr.cpp / irgen_stmt.cpp)
std::string genExpr(Expr* e);            // rvalue -> register name
std::string genAddr(Expr* e);            // lvalue -> pointer register
void genStmt(Stmt* s);
void genParfor(Stmt* pf);
void emitDeferredAll();
// data stacks (goose port): emit the watermark restores of every scope
// opened at or after `from` (reverse declaration order) — see irgen_stmt.cpp
void emitDstkRestores(size_t from);
std::string genCall(Expr* e);
std::string genSoaFieldAddr(Expr* fld);
std::string genCastInline(Expr* fromExpr, Type* to, std::string v);
// depth of the defer stack (irgen_stmt.cpp) — parfor chunk fns push
// loop contexts whose deferBase must match the CURRENT depth
size_t deferStackDepth();
// fsht-02:01 (registry 02.09.02): container drop stack — same contract
// as the defer depth above; moves null the source binding's data field;
// pushContainerDrop registers a scope-exit drop for a Vec<T> slot
size_t containerDropDepth();
void nullContainerSource(Expr* src, Type* cty);
void pushContainerDrop(const std::string& alloca, Type* cty);
void emitContainerDropsAll();
// fsht-03:02 (ch. 24 §4): statement-local OWNING temporaries — a
// constructor result consumed by nothing (a view read, a discarded
// value) drops at its statement's end; a temporary whose bytes were
// copied out (into a binding, a callee's aggregate parameter, a vec
// element) transfers ownership and never drops here.  This closes the
// "a never-freed String can no longer leak" guarantee for temporaries.
void registerOwningTemp(const std::string& addr, Type* ty);
void markTempConsumed(const std::string& addr);
void flushTempDrops();
// fsht-03:02 (ch. 24 §5): deep-copy the elements of an already
// shallow-cloned container (vec_clone over owning element types)
void emitDeepCloneElems(const std::string& dst, Type* ty);

// profile loading (irgen_pgo.cpp)
void loadProfile(const std::string& path);
int profSiteFor(uint64_t hash, SrcLoc loc);
void profileFnLookup(FnDecl* f);
std::vector<std::pair<std::string, uint64_t>> profileTopFunctions();

} // namespace freshet
