// Freshet compiler — shared AST / Sema structures.

#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <unordered_map>
#include <memory>
#include <cassert>
#include <functional>
#include "diag_codes.h"

namespace freshet {

#define FRESHET_VERSION "0.1.0-INDEV"  // report integration: Phases 0-2 + §5/§7/§8 policies

// ---------------------------------------------------------------- diagnostics
struct Diag {
  enum Severity { Error, Warning, Note };
  Severity sev;
  std::string code;      // stable public ID ("F0402"), see diag_codes.h
  std::string msg;
  std::string file;
  int line, col;
  std::string extra;     // machine-readable detail for report merging
};
inline std::vector<Diag> g_diags;
inline void diag(Diag::Severity s, Code code, const std::string& msg,
                 const std::string& file = "", int line = 0, int col = 0,
                 const std::string& extra = "") {
  g_diags.push_back({s, info(code).id, msg, file, line, col, extra});
}
inline int errorCount() {
  int n = 0; for (auto& d : g_diags) if (d.sev == Diag::Error) ++n; return n;
}

// ---------------------------------------------------------------- source info
// Stage E (report §3.2 + §4.3): SrcLoc now carries byte offsets and an end
// position, in addition to the historical start line/column.  This is
// needed for:
//   - LSP `Range` requires (start, end) byte positions, not just start.
//   - Quick-fix edits must know the span to replace, not just where to
//     insert a diagnostic.
//   - Source maps for #include / #define expansions need byte offsets to
//     splice edits back into the original buffer.
//
// The lexer is updated (lexer.cpp) to fill these new fields.  All existing
// code that constructs a SrcLoc gets the historical defaults
// (startOffset=endOffset=0, endLine=line, endCol=col) so behaviour is
// preserved when offsets are not yet populated.
//
// Encoding contract: offsets are BYTE offsets into the source snapshot
// the lexer consumed.  LSP character positions are 16-bit code units
// (UTF-16 by default); the LSP server is responsible for converting
// between byte offsets and the negotiated character encoding.
struct SrcLoc {
  std::string file;
  int line = 0, col = 0;
  // byte offset of the first character of the span
  uint64_t startOffset = 0;
  // byte offset ONE PAST the last character of the span (so end-start == length)
  uint64_t endOffset = 0;
  // end position; defaults to (line, col) for back-compat with single-point spans
  int endLine = 0, endCol = 0;
};
struct Token {
  enum K {
    EOFTOK, IDENT, INT, FLT, STR, KW, PUNCT,
  };
  K kind = EOFTOK;
  std::string text;      // ident/keyword/punct spelling, or literal lexeme
  std::string sval;      // string literal value (decoded)
  uint64_t ival = 0; double fval = 0;
  bool isflt = false;
  SrcLoc loc;
};

// ---------------------------------------------------------------- types
struct StructDef;
struct EnumDef;
struct Type {
  enum K { U8, U16, U32, U64, USIZE, I8, I16, I32, I64, F32, F64, BOOL,
           VOID, PTR, STRUCT, SOA, STR, ARRAY, SPAN, ENUM,
           // Phase 3b (report §6.3): function-pointer types —
           // fn(Params) -> Ret, first-class values, C-ABI compatible
           FNPTR };
  K k = VOID;
  Type* pointee = nullptr;   // PTR / SPAN (element) / ARRAY (element)
  StructDef* sdef = nullptr; // STRUCT / SOA
  EnumDef* edef = nullptr;   // ENUM
  uint64_t arrLen = 0;       // ARRAY: length is part of the type
  static Type* simple(K k) {
    static Type cache[32];
    static bool init = false;
    if (!init) { for (int i = 0; i < 32; ++i) cache[i].k = (K)i; init = true; }
    return &cache[k];
  }
  static Type* ptr(Type* p) {
    static std::vector<std::unique_ptr<Type>> pool;
    for (auto& t : pool) if (t->k == PTR && t->pointee == p) return t.get();
    pool.push_back(std::make_unique<Type>());
    pool.back()->k = PTR; pool.back()->pointee = p;
    return pool.back().get();
  }
  static Type* structOf(StructDef* s) {
    static std::vector<std::unique_ptr<Type>> pool;
    for (auto& t : pool) if (t->k == STRUCT && t->sdef == s) return t.get();
    pool.push_back(std::make_unique<Type>());
    pool.back()->k = STRUCT; pool.back()->sdef = s;
    return pool.back().get();
  }
  static Type* soaOf(StructDef* s) {
    static std::vector<std::unique_ptr<Type>> pool;
    for (auto& t : pool) if (t->k == SOA && t->sdef == s) return t.get();
    pool.push_back(std::make_unique<Type>());
    pool.back()->k = SOA; pool.back()->sdef = s;
    return pool.back().get();
  }
  // Phase 1b: fixed-size array [T; N] — the length is part of the type.
  static Type* arrayOf(Type* elem, uint64_t n) {
    static std::vector<std::unique_ptr<Type>> pool;
    for (auto& t : pool)
      if (t->k == ARRAY && t->pointee == elem && t->arrLen == n) return t.get();
    pool.push_back(std::make_unique<Type>());
    pool.back()->k = ARRAY; pool.back()->pointee = elem; pool.back()->arrLen = n;
    return pool.back().get();
  }
  // Phase 1c: Span<T> — a borrowed (ptr, len) view over contiguous T.
  // Representation is a first-class { ptr, i64 } aggregate.
  static Type* spanOf(Type* elem) {
    static std::vector<std::unique_ptr<Type>> pool;
    for (auto& t : pool) if (t->k == SPAN && t->pointee == elem) return t.get();
    pool.push_back(std::make_unique<Type>());
    pool.back()->k = SPAN; pool.back()->pointee = elem;
    return pool.back().get();
  }
  static Type* enumOf(EnumDef* e) {
    static std::vector<std::unique_ptr<Type>> pool;
    for (auto& t : pool) if (t->k == ENUM && t->edef == e) return t.get();
    pool.push_back(std::make_unique<Type>());
    pool.back()->k = ENUM; pool.back()->edef = e;
    return pool.back().get();
  }
  // Phase 3b: fn(P...) -> R — interned by structural identity (the
  // parameter list is stored in the parallel vector `fnParams`, the
  // return type reuses `pointee`).
  static Type* fnptrOf(const std::vector<Type*>& params, Type* ret);
  std::vector<Type*> fnParams;             // FNPTR: parameter types
  bool isInt() const { return k==U8||k==U16||k==U32||k==U64||k==USIZE||
                              k==I8||k==I16||k==I32||k==I64; }
  bool isSigned() const { return k==I8||k==I16||k==I32||k==I64; }
  bool isFloat() const { return k==F32||k==F64; }
  bool isNum() const { return isInt()||isFloat(); }
  bool isPtr() const { return k==PTR; }
  // pair aggregates (report §4.1/§4.5): { ptr, i64 } representation —
  // Span<T> and the UTF-8 str view
  bool isPair() const { return k==SPAN || k==STR; }
  // value aggregates passed by pointer in the ABI (struct, enum, array,
  // span, str): a variable slot holds the whole aggregate
  bool isAggregate() const {
    return k==STRUCT || k==ENUM || k==ARRAY || k==SPAN || k==STR || k==SOA;
  }
  // Phase 3b: fn pointers are scalar-sized pointer values
  bool isFnPtr() const { return k == FNPTR; }
  int bits() const {
    switch (k) { case U8: case I8: case BOOL: return 8; case U16: case I16: return 16;
      case U32: case I32: case F32: return 32; default: return 64; }
  }
};

// struct field
struct Field {
  std::string name; Type* ty = nullptr; std::string tyName; int idx = 0;
  SrcLoc loc2;
};

// fn parameter (moved ahead of EnumVariant: variant payloads reuse it).
// Phase MEM (report §5.3): parameter ownership modes — `owned data: *u8`
// / `borrowed data: Span<f64>` state the contract where the function is
// declared instead of inferring it from call sites.
struct Param {
  std::string name; Type* ty = nullptr; std::string tyName;
  bool owned = false;          // `owned` mode: caller must give(); callee frees
  bool borrowed = false;       // `borrowed` mode: no free/give, no mutation
  SrcLoc loc;
};

struct StructDef {
  std::string name;
  std::vector<Field> fields;
  uint64_t alignOverride = 0;    // align(N) attribute
  SrcLoc loc;
  // Phase 3a (report §6.1): owning module + pub visibility
  std::string module;
  bool isPub = false;
  // Phase 2a: generic struct template support (monomorphised in sema)
  bool isGeneric = false;
  std::vector<std::string> tparams;   // <T, U, ...>
  // concrete type arguments (parallel to the originating template's
  // tparams; empty for non-instantiated structs) — drives inference at
  // generic call sites
  std::vector<Type*> targs;
  // computed layout
  std::vector<uint64_t> offsets, sizes;
  uint64_t size = 0, align = 0, padBytes = 0;
  int id = 0;                    // llvm type number
};

// ---------------------------------------------------------------- enums
struct Program;   // declared below; the prelude factory needs it
// Phase 1d (report §4.3): C-style enums and payload-carrying tagged
// unions.  An enum value is a first-class aggregate:
//     { i64 tag, [paySize x i8] payload }   (payload aligned to payAlign)
// Simple enums (no variant has fields) still use the same layout — the
// payload is zero-sized — so match lowering is uniform.
struct EnumVariant {
  std::string name;
  std::vector<Param> fields;      // payload fields (positional)
  SrcLoc loc;
  int idx = 0;
  // computed payload layout (per-variant mini-struct)
  std::vector<uint64_t> offsets, sizes;
  uint64_t paySize = 0, payAlign = 1;
};
struct EnumDef {
  std::string name;
  std::vector<EnumVariant> variants;
  SrcLoc loc;
  // Phase 3a: owning module + pub visibility
  std::string module;
  bool isPub = false;
  bool isGeneric = false;
  std::vector<std::string> tparams;
  // source spelling for qualified access (`Option.None` after
  // instantiation): the template's name, not the mangled instance name
  std::string templateName;
  // computed layout
  uint64_t size = 0, align = 8, paySize = 0, payAlign = 1;
  int id = 0;
};

// registry + layout (types.cpp)
EnumDef* findEnumDef(const std::string& name);
void registerEnum(EnumDef* ed);
void computeEnumLayout(EnumDef* e);
std::vector<EnumDef*> allEnumDefs();
// prelude types (generics.cpp): String struct + Option/Result enums
StructDef* ensurePreludeString(Program* prog);

// ---------------------------------------------------------------- symbol info
struct Expr; struct Stmt; struct FnDecl;
struct Sym {
  enum K { VAR, PARAM, FN, GLOBAL, STRUCT } k;
  std::string name;
  Type* ty = nullptr;
  bool mut = false;
  bool owned = false;       // owns heap memory (malloc result / give arg)
  FnDecl* fn = nullptr;
  StructDef* sdef = nullptr;
  int scope = 0;
  SrcLoc declLoc;
};

struct FnSema {
  FnDecl* fn = nullptr;
  std::vector<std::map<std::string, Sym>> scopes;
  int scopeDepth = 0;
  std::vector<Sym> allSyms;
  struct LoopInfo { std::string var; Stmt* loop; };
  std::vector<LoopInfo> loops;
  int parforDepth = 0;
  // Phase MEM §5.1: nesting depth of unsafe scopes (fn @unsafe counts
  // from 1).  Inside an unsafe scope, bounds checks are skipped and the
  // ownership diagnostics are downgraded to warnings.
  int unsafeDepth = 0;
  std::map<std::string, std::vector<SrcLoc>> parforWrites;
  std::map<std::string, int> varDepth;
  std::set<std::string> reductionVars;
  std::map<Stmt*, std::string> stmtWrites;  // ASSIGN stmt -> root var name

  Sym* lookup(const std::string& n) {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
      auto f = it->find(n);
      if (f != it->end()) return &f->second;
    }
    return nullptr;
  }
  void push() { scopes.emplace_back(); ++scopeDepth; }
  void pop() { if (!scopes.empty()) { scopes.pop_back(); --scopeDepth; } }
  void declare(Sym s) {
    s.scope = scopeDepth;
    if (!scopes.empty()) scopes.back()[s.name] = s;
    allSyms.push_back(s);
  }
};

// cross-TU sema accessors
struct GlobalVar;
FnDecl* lookupFn(const std::string& name);
GlobalVar* lookupGlobal(const std::string& name);
StructDef* findStructDef(const std::string& name);
void registerStruct(StructDef* sd);
uint64_t typeSizeOf(Type* t);
uint64_t typeAlignOf(Type* t);
Type* typeExpr(FnSema* fs, Expr* e);
struct FnSema* semaForFn(FnDecl* f);
std::vector<FnSema*>& allFnSemas();

// ---------------------------------------------------------------- AST

struct Expr {
  enum K { INT, FLT, STR, BOOL, IDENT, CALL, BIN, UN, INDEX, FIELD, CAST, NILQ, SLIT,
           ARRLIT,   // Phase 1b: [a, b, c] fixed-array literal
           SLICE,    // Phase 1c: base[lo..hi] — span/str/array view
           TRYQ };   // Phase 2c: expr? — Option/Result propagation
  K k;
  SrcLoc loc;
  // literals
  uint64_t ival = 0; double fval = 0; std::string sval; bool bval = false;
  std::string name;                       // IDENT / CALL callee / FIELD name
  std::vector<Expr*> args;                // CALL
  std::vector<bool> argGive;              // CALL: give(p) per arg
  std::vector<std::string> slitFields;    // SLIT: field names
  std::string op;                         // BIN/UN operator
  Expr *lhs = nullptr, *rhs = nullptr, *base = nullptr;
  // SLICE: lo/hi reuse lhs/rhs
  Type* castTy = nullptr;
  std::string castTyName;             // CAST: binder encoding of castTy
  // sema results
  Type* ty = nullptr;
  bool constant = false;                  // foldable
  bool isLvalue = false;
  bool soaIndex = false;                  // INDEX whose base is soa(T)
  bool ptrDeref = false;                  // FIELD through *Struct (auto-deref)
  Type* mallocElem = nullptr;             // malloc/soa_new element type
  Sym* sym = nullptr;                     // resolved IDENT
  GlobalVar* globalRef = nullptr;         // resolved global
  FnDecl* callee = nullptr;               // resolved user call
  int fieldIdx = -1;                      // resolved FIELD
  // Phase 1d: enum variant construction (IDENT `A` / CALL `A(...)` or
  // qualified `Enum.Variant`), resolved during sema
  EnumDef* enumRef = nullptr;
  int variantIdx = -1;
  // Phase 3a: `mod::name` — the module qualifier of a qualified path
  // (empty for plain names). `name` keeps the FULL source spelling
  // ("geometry::distance") so diagnostics quote what the user wrote.
  std::string qualModule;
  // Phase 3b: indirect call through a function-pointer variable —
  // resolved to the fnptr-typed Sym instead of a FnDecl
  Sym* fnPtrSym = nullptr;
  // Phase 3b: fn-value reference (IDENT naming a function, not a call)
  FnDecl* fnValue = nullptr;
  Expr(K kk, SrcLoc l) : k(kk), loc(l) {}
};

struct ParForClause {
  enum K { REDUCTION, THREADS, CHUNK, AFFINITY, COLLAPSE };
  K k;
  std::string redOp;      // "+", "*", min, max
  std::string var;        // reduction accumulator name
  Expr* e = nullptr;      // threads/chunk expr
  int ival = 0;           // collapse
  std::string sval;       // affinity: "close"|"spread"
  SrcLoc loc;
};

// Phase 1d: one `match` arm — `Variant => body`, `Variant(a, b) => body`,
// or `_ => body`. Bindings map payload fields positionally to fresh names.
struct MatchArm {
  bool wildcard = false;
  std::string enumName;                   // resolved enum (diagnostics)
  std::string variantName;                // raw spelling from source
  int variantIdx = -1;                    // resolved variant
  std::vector<std::string> bindings;
  Stmt* body = nullptr;
  SrcLoc loc;
};

struct Stmt {
  enum K { LET, ASSIGN, RETURN, DEFER, IF, WHILE, FOR, PARFOR, EXPR, BLOCK, EMPTY,
            BREAK,    // Phase 1a (report §4.4)
            CONTINUE, // Phase 1a
            MATCH };  // Phase 1d (report §4.3/§4.4)
  K k;
  SrcLoc loc;
  std::string tyName;   // late-bound struct/soa type name
  std::string op2;      // assignment operator ("=", "+=", ...)
  bool pDescending = false;  // parfor i in a..b by -1
  // LET
  std::string var; Type* annTy = nullptr; bool mut = false; bool hasAnn = false;
  Expr* init = nullptr;
  // ASSIGN
  Expr* lhs = nullptr; Expr* rhs = nullptr;
  // RETURN
  Expr* retVal = nullptr;
  // DEFER (stores a call or free)
  Expr* deferExpr = nullptr;
  // IF / WHILE
  Expr* cond = nullptr; Stmt* thenB = nullptr; Stmt* elseB = nullptr;
  // FOR: `for (i = a; i < b; i ± s)` or `for (i in a..b)`
  std::string ivar; Expr* forInit = nullptr; Expr* forCond = nullptr;
  Expr* forStep = nullptr; bool forRange = false; Expr* rangeLo = nullptr; Expr* rangeHi = nullptr;
  // PARFOR
  std::string pvar; Expr* plo = nullptr, *phi = nullptr;  // i in plo..phi
  std::vector<ParForClause> clauses;
  Stmt* body = nullptr;
  int depth = 0;            // nesting depth of parfor (0 = outermost)
  bool collapsed = false;   // R2: merged with inner parfor
  // §5 loop-hint policy: `@vectorize` written directly on this loop asks
  // the backend to vectorize (emits llvm.loop.vectorize.enable).  The
  // default emits NO forced metadata; --force-vectorize restores the
  // historical blanket hint globally for differential testing.
  bool vectorizeHint = false;
  // Phase 5 (report §12): @reproducible on a parfor — the reduction is
  // computed with a fixed block partition and strict-FP combines, so
  // the result is bitwise identical for any thread count / scheduler.
  bool reproducible = false;
  // Phase MEM (report §5.1): `unsafe { ... }` is represented as a BLOCK
  // with this flag — every existing AST walker already visits BLOCKs, so
  // no pass silently skips an unsafe scope.
  bool isUnsafe = false;
  // Phase 1d: MATCH
  Expr* matchOn = nullptr;               // scrutinee
  std::vector<MatchArm> matchArms;
  // BLOCK / EXPR
  std::vector<Stmt*> stmts; Expr* expr = nullptr;
  Stmt* parent = nullptr;
  // sema: enclosing loop stack info
  FnDecl* fn = nullptr;
  Stmt() = default;
  Stmt(K kk, SrcLoc l) : k(kk), loc(l) {}
};

// ---------------------------------------------------------------- effects
// §2.3 (audit P0): a compact function-effect lattice.  "argmem" is
// memory reachable from pointer/soa/struct-pointer PARAMETERS; "other"
// covers globals, heap and anything of unknown provenance.  Reads and
// writes are tracked separately because LLVM's memory(none) means "no
// memory access at all", which is NOT the same as "does not write" —
// a function returning p[0] is read-only (memory(argmem: read)), not
// read-none.  Local allocas (scalars, structs by value) are not
// observable from outside and do not count as memory effects, matching
// how LLVM reasons about freshly created stack objects.
struct Effects {
  bool readsArg = false;     // reads through parameter-reachable memory
  bool readsOther = false;   // reads globals / heap / unknown memory
  bool writesArg = false;    // writes through parameter-reachable memory
  bool writesOther = false;  // writes globals / heap / unknown memory
  bool allocates = false;   // malloc / soa_new
  bool freesMem = false;    // free / soa_free
  bool io = false;           // print* / println — externally visible output
  bool unknownCall = false;  // call whose effects we cannot describe
  bool mayNotReturn = false; // loops (while / c-for / parfor), assert, recursion

  bool anyRead() const { return readsArg || readsOther; }
  bool anyWrite() const { return writesArg || writesOther; }
  // "pure" in the Freshet observable sense used by the parfor checks
  // and the R4 loop passes: no writes, no I/O, no allocation, no
  // unknown calls.  Reads are permitted.
  bool semanticallyPure() const {
    return !anyWrite() && !io && !allocates && !freesMem && !unknownCall;
  }
  void merge(const Effects& o) {
    readsArg |= o.readsArg;     readsOther |= o.readsOther;
    writesArg |= o.writesArg;  writesOther |= o.writesOther;
    allocates |= o.allocates;  freesMem |= o.freesMem;
    io |= o.io;                 unknownCall |= o.unknownCall;
    mayNotReturn |= o.mayNotReturn;
  }
};

// Compute interprocedural effect summaries + validate @pure contracts
// (effects.cpp).  Fills FnDecl::eff, derives FnDecl::isPure, and emits
// F0304 errors for user-declared @pure functions whose bodies violate
// the documented "reads no memory and writes no memory" contract
// (spec §3.1).  A @pure promise is a contract now, not a bypass.
struct Program;
void computeEffects(Program* p);

struct FnDecl {
  std::string name;
  std::vector<Param> params;
  Type* ret = Type::simple(Type::VOID);
  std::string retTyName;
  Stmt* body = nullptr;
  bool strictfp = false;    // @strictfp: disable fast-math
  // Policy §8 Option A: fast-math is now OPT-IN. The language default is
  // IEEE-754-like strict per-operation semantics; `@fastmath` on a fn (or
  // `--fast-math` globally) re-enables the relaxed contract.
  bool fastmath = false;    // @fastmath
  bool userPure = false;    // @pure
  bool exported = false;    // export fn
  bool imported = false;    // extern fn declaration, implemented outside Freshet
  // Policy §7: `@hpc_strict` — this function is checked with the strict
  // performance policy (perf diagnostics become errors) even when the
  // compilation is not in --strict-performance mode.
  bool hpcStrict = false;   // @hpc_strict
  // Phase MEM §5.1: `@unsafe fn` — the whole body is an unsafe scope
  // (bounds checks off, ownership diagnostics downgraded to warnings).
  bool unsafeFn = false;    // @unsafe
  // Phase 2a: generic function template (monomorphised per call site)
  bool isGeneric = false;
  std::vector<std::string> tparams;;
  std::vector<std::string> tconstraints;   // parallel to tparams ("" = none)
  // instantiated bodies rebind their type strings through this map
  // (empty for ordinary functions and templates)
  std::map<std::string, Type*> instTMap;
  // --- inline hints (Stage A1): explicit user control over the LLVM inliner.
  // Defaults: `inline_default` policy (LLVM decides by cost).  `@inline`
  // adds a strong `alwaysinline` hint; `@noinline` adds `noinline`.
  // Both override the global --inline= flag for this function only.
  bool userInline = false;    // @inline attribute present
  bool userNoinline = false;  // @noinline attribute present
  std::vector<std::pair<std::string,std::string>> aliasPairs; // @alias(a,b)
  SrcLoc loc;
  int id = 0;
  // sema results
  bool computedPure = false;
  bool isPure = false;   // derived: no observable side effects (reads OK)
  Type* ty = nullptr;
  int profIdx = -1;         // PGO function counter slot
  // Phase 3a (report §6.1): owning module + pub visibility
  std::string module;
  bool isPub = false;
  // Phase 4 (report §10.3): @test — zero-arg test function run by
  // `penstock test`
  bool isTest = false;
  // §2.3 effect summary: conservative interprocedural memory/effect
  // lattice computed by effects.cpp (fixed point).  This replaces the
  // historical boolean purity walk, which conflated "read-only" with
  // "read-none" and let a pointer-reading helper be marked
  // memory(none) — a wrong-code hazard at -O3.
  Effects eff;
};

struct GlobalVar {
  std::string name; Type* ty = nullptr; std::string tyName; bool mut = false;
  Expr* init = nullptr; SrcLoc loc; bool exported = false;
  // Phase 3a: owning module + pub visibility
  std::string module;
  bool isPub = false;
};

// Phase 3a (report §6.1): `import geometry;` — resolved by the module
// loader (modules.cpp) BEFORE sema; the merged Program carries every
// loaded module's decls with their originating `module` tags.
struct ImportDecl {
  std::string name;
  SrcLoc loc;
};

struct Program {
  std::vector<FnDecl*> fns;
  std::vector<StructDef*> structs;
  std::vector<EnumDef*> enums;
  std::vector<GlobalVar*> globals;
  std::vector<std::string> linkLibs;   // #link
  std::string file;
  // ---- Phase 3a: module identity ----------------------------------
  // `module geometry;` at the top of the entry file (or the file stem
  // when absent).  Empty for the legacy single-file compilation mode.
  std::string moduleName;
  std::vector<ImportDecl> imports;     // imports of the ENTRY module
  // per-module import sets (filled by the loader for every loaded
  // module; entry module key = moduleName)
  std::map<std::string, std::set<std::string>> moduleImports;
};

// ---------------------------------------------------------------- driver opts
// Inline policy (Stage A1 — see report §2.2 A).  The historical behaviour
// was a blanket `noinline` on attribute groups #50/#51/#52 for ordinary,
// strict-FP and inferred-pure functions.  That blocked the LLVM inliner
// for nearly every internal helper, which is the wrong default for a
// performance-oriented HPC compiler.
//
//   default       — let LLVM decide by its normal cost/legality rules
//   conservative — like default, but mark recursive / very large
//                  functions noinline automatically (future)
//   off          — never emit inline/noinline hints AND mark every user
//                  function `noinline` (preserves the historical baseline
//                  for differential benchmarking)
//
// Per-function `@inline` / `@noinline` attributes always override the
// global policy.
enum class InlinePolicy { Default, Conservative, Off };
inline const char* inlinePolicyName(InlinePolicy p) {
  switch (p) {
    case InlinePolicy::Default:      return "default";
    case InlinePolicy::Conservative: return "conservative";
    case InlinePolicy::Off:          return "off";
  }
  return "default";
}

struct Options {
  std::string input, output = "a.out";
  // Phase 3a (report §6.2): multi-file compilation — every .fsh input
  // is a module of the same program (imports resolve the rest).
  std::vector<std::string> inputs;
  bool report = false;            // performance report mode
  bool profile = false;           // R2: instrument for profiling
  std::string useProfile;         // R2: path to .fprof
  int optLevel = 3;
  std::string march = "native";
  bool keepIr = false;
  std::string irOut;              // if set, stop after IR emit
  bool emitObj = false;
  bool run = false;
  std::string runArgs;
  int threads = 0;                // default parfor threads (0 = auto)
  bool soaAuto = false;           // R3: --soa=auto transform (warn-only default)
  int evoBudgetSec = 10;          // optional legacy -O4 runtime tuning budget
  unsigned evoSeed = 0;           // R3: 0 = derived from time+pid
  int superoptBudgetSec = 0;      // 0 = level-dependent default (2s -O3, 10s -O4)
  bool noSuperopt = false;        // --no-superopt disables embedded IR rewriting
  bool noPrecompute = false;      // --no-precompute disables verified finite-domain LUT rewriting
  bool evoRuntimeTune = false;    // opt-in legacy execute-and-time runtime tuning
  bool noLoopOpts = false;        // R4: --no-loop-opts disables fusion/unroll/lane passes
  std::vector<std::string> includeDirs;

  // ---- Stage A1: inlining policy.  Default lets LLVM decide; `--inline=off`
  // reproduces the historical noinline-everything baseline.
  InlinePolicy inlinePolicy = InlinePolicy::Default;

  // ---- Stage A2: optimisation observability loop.
  // --emit-combined-ir writes the same module that Clang optimises (user IR
  // + stripped runtime IR) to <file>, so optimisation analysis can run on
  // the real artefact rather than the user-IR-only `-S` output.
  //
  // --save-temps keeps every intermediate artefact: preprocessor output,
  // user .ll, combined .ll, clang opt-remarks YAML, and the final binary.
  //
  // --opt-remarks[=category,...] passes -Rpass*/-fsave-optimization-record
  // through to Clang and prints a per-loop summary keyed by source range.
  //   categories: vectorize | inline | all (default: all)
  std::string emitCombinedIr;
  bool saveTemps = false;
  std::string optRemarks;     // "" = off, "all" by default if --opt-remarks alone
  bool optRemarksEnabled = false;

  // ---- Stage A3: -S now also emits the combined IR by default (the IR
  // Clang actually optimises).  Set false to preserve the historical
  // behaviour of writing only user IR.
  bool emitCombinedOnS = true;

  // ---- audit §2–§15 integration --------------------------------------
  // §3: runtime IR optimisation level for the cached frt_rt.ll build.
  //   2 (default): no accidental optnone/noinline, canonical bodies the
  //               final pipeline can still re-optimise
  //   0:          debug runtime (historical -O0 behaviour)
  //   1..3:       explicit override; part of the cache key
  int runtimeOpt = 2;

  // §5: restore the historical forced vectorize.enable + interleave 4
  // on every simple loop (differential testing baseline).  Default off.
  bool forceVectorize = false;

  // §8.2: vector math library selection.  "none" (default) keeps the
  // portable LLVM lowering; "libmvec" appends -fveclib=libmvec
  // -fno-math-errno to the backend so the loop vectoriser can replace
  // llvm.sin/cos/log2/log10 calls with glibc's vector routines.
  // Only meaningful with the default fast-FP semantics.
  std::string veclib = "none";

  // §11.2: LLVM IR-based PGO (in addition to the thread-safe custom
  // counters, which remain the source-level approximation).
  bool llvmPgoGenerate = false;      // --llvm-pgo-generate: -fprofile-generate
  std::string llvmPgoUse;           // --llvm-pgo-use=<f>: profraw/dir or profdata
  std::string llvmProfdata;         // --llvm-profdata=<exe>: override tool path

  // §13.1: drop the final OPTIMISED IR next to the binary (runs the
  // backend once more with -S -emit-llvm at the requested level).
  std::string emitFinalIr;          // "" off, "<auto>" -> <out>.final.ll

  // §12: target configuration.  "" = native host: the triple + data
  // layout are then scraped from the backend clang's own default
  // (main.cpp probes it on every build); otherwise the requested clang
  // target triple, normalised through the same backend probe.
  std::string targetTriple;
  bool portable = false;            // --portable: baseline x86-64, no -march=native

  // ---- report §7: performance-diagnostics policy -----------------------
  // Default: correctness diagnostics are errors, performance diagnostics
  // are warnings — a slow-but-correct program compiles.  `--strict-
  // performance` (or `@hpc_strict` on a function) restores the historical
  // hard errors for the perf family (F0401-F0406).
  bool strictPerformance = false;

  // ---- report §8 Option A: floating-point default ----------------------
  // Default: strict IEEE-754-like per-operation semantics.  `--fast-math`
  // opts the whole compilation into the relaxed contract (per-function
  // `@fastmath` composes on top).
  bool fastMathGlobal = false;

  // ---- report §4.1/§4.5: bounds-check policy ----------------------------
  // `--checked` (or -O0/-O1) bounds-checks Span and fixed-array indexing;
  // `--no-bounds-check` disables checks everywhere (escape hatch).
  // Compile-time-provable indices are never checked in any mode.
  bool checked = false;
  bool noBoundsCheck = false;

  // ---- optimisation-audit integration (Phase C/D/E) ---------------------
  // New source-level transforms are OPT-IN (audit §11-D: "each pass
  // should start behind an opt-in flag until benchmarks show a robust
  // improvement on more than one workload"); every one also has a
  // --no-* escape hatch for differential testing (§10.2).
  bool licmOn = false;            // --licm: FreshetLICM (§6.4)
  bool partialUnrollOn = false;   // --partial-unroll (§6.5)
  bool peelOn = false;            // --peel: loop peeling (§6.5)
  bool interchangeOn = false;     // --interchange (§6.6)
  bool tileOn = false;            // --tile[=I[,J]] (§6.7)
  int tileI = 0, tileJ = 0;       // 0 = choose from the target profile
  bool distributeOn = false;      // --distribute: loop fission (§6.8)
  bool unrollJamOn = false;       // --unroll-and-jam (§6.9)
  bool parforAdvisorOn = false;   // --parfor-advisor: advice only (§6.10)
  bool soaAosoa = false;          // --soa=aosoa: advisory only (§6.11)

  // §10.4 pass remarks / §5.2 proof artefacts:
  // --proof-log[=<file>] records, per APPLIED source-level transform, the
  // legality premises it relied on (one JSON-ish line each).  The Lean
  // kernel + Alive2 remain the real verification gates; the log makes the
  // pass contract auditable and diffable in CI.
  bool proofLogOn = false;
  std::string proofLogPath;       // "" = <out>.prooflog

  // §7.1 superoptimiser objective: latency | size | balanced
  std::string superoptObjective = "latency";

  // §8 target profile overrides (defaults in profit_model.cpp)
  int targetCacheLine = 0;        // 0 = default (64)
  int targetL1 = 0;               // 0 = default (32768)
  int targetL2 = 0;               // 0 = default (262144)
  int targetVectorWidth = 0;      // 0 = default (8)

  // ---- Phase 3+ driver surface ------------------------------------------
  // Phase 3a: module search path additions (`-I` covers both #include
  // and import resolution; the builtin modules dir is always searched
  // last — that is where the shipped `fsh` module lives)
  // Phase 3c/4: -g / --debug emits DWARF source-level debug info
  bool debugInfo = false;
  // Phase 4 (report §10.3): `penstock test` — run @test fns via the
  // generated test harness (replaces main)
  bool testMode = false;
  // Phase 4 (report §10.2): `penstock lint` — diagnostics-only run
  bool lintMode = false;
  // Phase 4 (report §10.2): `penstock fmt [--check]`
  bool fmtMode = false;
  bool fmtCheck = false;
  // Phase 3 (report §6.2): project manifest (penstock.toml) — empty = none
  std::string projectFile;
  // project build: force recompilation even when the cache is fresh
  bool forceRebuild = false;
  // project target type (from the manifest): bin | static-lib | shared-lib
  std::string projectTarget = "bin";
  // project-declared link libraries
  std::vector<std::string> projectLinkLibs;
  // project-declared module search dirs (relative to the manifest)
  std::vector<std::string> projectSrcDirs;
  // manifest location + [project] name (drives the default output path:
  // <manifest dir>/<project name>)
  std::string projectDir;
  std::string projectName;
};

extern Options g_opts;

// resolved backend description, e.g. "LLVM 22.1.8 (tools/llvm22)" —
// filled in by main() once the toolchain is located; shown by --report.
extern std::string g_backend;

// §12 target configuration, resolved by main() before irgen: the
// canonical triple / data layout probed from the backend clang — its
// own default for native builds, the normalised --target/--portable
// triple otherwise.  Empty (= probe failed) falls back to the built-in
// x86_64-pc-linux-gnu pair in irgen.cpp.
extern std::string g_targetTriple;
extern std::string g_dataLayout;

// helpers shared across phases ------------------------------------------
inline uint64_t alignUp(uint64_t v, uint64_t a) { return a ? ((v + a - 1) / a) * a : v; }
std::string typeToString(Type* t);

// lexer
int lexFile(const std::string& path, const std::string& src,
            std::vector<Token>& toks);
// preprocessor
bool preprocess(const std::string& path, std::string& out,
                std::vector<std::string>& linkLibs);
uint64_t hashName(const std::string& s);
// parser
Program* parseTokens(std::vector<Token>& toks, const std::string& file);
// types
void computeStructLayout(StructDef* s);
Type* resolveTypeName(const std::string& name);
bool typesEquivalent(Type* a, Type* b);
bool typesCompatible(Type* a, Type* b);   // with implicit conversion rules
Type* commonType(Type* a, Type* b);
// sema
bool semaProgram(Program* p);
// sema accessors for cross-TU phases (sema_expr.cpp / generics.cpp)
void queueGenericInstance(FnDecl* inst);
Program* currentProgram();
// contextual resolution helpers (sema_expr.cpp)
void tryResolveVariantExpr(FnSema* fs, Expr* e, Type* expected);
void tryAdoptStructLiteral(Expr* e, Type* expected);
// generic-instance scope: the current function's T -> concrete map
Type* tparamTypeInScope(const std::string& name);
// generics (Phase 2a): monomorphisation helpers (generics.cpp)
StructDef* instantiateStruct(Program* prog, StructDef* tmpl,
                             const std::vector<Type*>& args, SrcLoc loc);
EnumDef* instantiateEnum(Program* prog, EnumDef* tmpl,
                         const std::vector<Type*>& args, SrcLoc loc);
FnDecl* instantiateGenericFn(Program* prog, FnDecl* tmpl,
                             const std::vector<Type*>& argTypes, SrcLoc loc);
Type* bindTypeEncoding(Program* prog, const std::string& enc, SrcLoc loc,
                       const std::map<std::string, Type*>* tmap);
// checks (strict)
void runStrictChecks(Program* p);
void runParforChecks(Program* p);
// ownership
void runOwnershipAnalysis(Program* p);
// Stage B5: consult after runStrictChecks().  Returns true iff the given
// @alias pair on `fn` was violated by at least one caller in this
// compilation unit (F0306).  IRgen suppresses the `noalias` LLVM
// parameter attribute on unsound pairs.
bool aliasPairIsUnsound(FnDecl* fn, const std::string& a, const std::string& b);
// R3: --soa=auto pre-sema AoS->SoA transform (returns vars transformed)
int applySoaAutoTransform(Program* p);
// R3: -O4 stochastic evolution engine (evo.cpp)
struct EvoWinner {
  bool improved = false;
  int clangOpt = 3;              // winning clang -O level
  std::string march = "native";  // winning -march
  int threads = 0;               // baked FRT_TUNED_THREADS (0 = leave auto)
  int pin = 0;                   // baked FRT_TUNED_AFFINITY
  long chunk = 0;                // baked FRT_TUNED_CHUNK (0 = heuristic)
  // §10.1 / §11.3: runtime serial cutoff (parfor regions no larger than
  // this many iterations execute inline instead of dispatching to the
  // pool).  -1 = not tuned (default heuristic); 0 = always dispatch.
  long serialCutoff = -1;
  bool veclib = false;           // §8: winner used -fveclib=libmvec
  double baseMs = 0, bestMs = 0; // re-validated medians
  int evals = 0, compiles = 0, generations = 0;
};
bool evoOptimize(const std::string& clangBin, const std::string& finalLl,
                 const std::string& outBinary, int budgetSec, unsigned seed,
                 EvoWinner* out);
// Verified superoptimisation: the Freshet engine proposes bounded LLVM IR
// rewrites; embedded Z3 and Alive2 must prove each rewrite before installation.
bool runVerifiedSuperoptimizer(const std::string& llvmRoot,
                               const std::string& finalLl, int optLevel,
                               int budgetSec);
// Exhaustive input/output-map synthesis for bounded, pure integer functions.
// Candidate lookup-table rewrites are installed only after Alive2 verifies them.
bool runPrecomputeRewriter(const std::string& clang, const std::string& llvmRoot,
                           const std::string& finalLl, int optLevel,
                           int verifyBudgetSec);
// R4: source-level loop optimisation passes (post-sema AST transforms,
// enabled at -O2+, skipped on --profile builds; notes F0908/F0909/
// F0901/F0902):
//   fuse_unroll.cpp  — adjacent-loop fusion (function fusion, incl. the
//                      call-slice FFT batch shape) + constant-trip full unroll
//   lane_analysis.cpp— cross-lane vectoriser analysis: uniform-branch
//                      unswitching + same-condition predicate reuse
int runFuseUnrollPasses(Program* p);
int runLaneAnalysisPass(Program* p);
// irgen
bool irgenProgram(Program* p, const std::string& outLl);
// report
void mergeAndPrintReport(Program* p, const std::string& irPath,
                         const std::string& binPath);

// ---------------------------------------------------------------- modules
// Phase 3a (report §6.1/§6.2) — src/modules.cpp:
//   * loadProgramWithInputs: preprocess+parse every input file, resolve
//     `import` declarations transitively (search: entry dir, -I dirs,
//     builtin modules dir), detect cycles, and merge everything into a
//     single Program whose decls carry their originating module tags.
//   * visibility: during sema the current module + its import set form
//     the lookup context; pub decls of imported modules are visible,
//     everything else is module-private.
//   * IR symbol naming: internal fns keep the historical u_<name> when
//     unique, and disambiguate to u_<module>$<name> on cross-module
//     name collisions (exported / extern / main keep their exact names).
Program* loadProgramWithInputs(const std::vector<std::string>& inputs,
                               std::vector<std::string>& linkLibs);
// in-memory variant for the LSP: the entry document's TEXT is analysed;
// imported modules are loaded from disk
Program* loadProgramWithImports(const std::string& entryPath,
                                const std::string& entryText,
                                std::vector<std::string>& linkLibs);
// lookup context (set by sema around each decl's binding / body typing)
void setModuleLookupContext(const std::string& currentModule,
                             const std::set<std::string>& imports);
const std::string& currentLookupModule();
// is `m` visible from the current lookup context?
bool moduleVisibleFrom(const std::string& m);
// sema finished: post-sema passes resolve without visibility filtering
void moduleLookupRelaxHook();
// true while sema is running (visibility filtering active)
bool moduleLookupIsStrict();
// register a freshly-created generic instance in the fn map so that
// concurrent call sites intern to ONE instance (no duplicate emission)
void registerGenericInstance(FnDecl* inst);
// push/pop the lookup context (used when a generic template is
// instantiated: its body binds under the TEMPLATE's module)
void pushModuleContext(const std::string& mod);
void popModuleContext();
// module-qualified registry lookups (types.cpp)
StructDef* findStructInModule(const std::string& name, const std::string& mod);
EnumDef* findEnumInModule(const std::string& name, const std::string& mod);
// registry helper (types.cpp / sema.cpp): pick the visible definition out
// of a same-named candidate list with precise F0004 diagnostics
template <typename D>
D* pickVisible(const std::vector<D*>& cands, const std::string& name,
               const std::string& kind, SrcLoc loc) {
  if (cands.empty()) return nullptr;
  bool strict = moduleLookupIsStrict();
  if (!strict) return cands[0];
  D* found = nullptr;
  int nvis = 0;
  for (auto* d : cands) {
    // visible: prelude, own module (pub or private), or pub decl of a
    // directly-imported module
    bool vis = d->module.empty() || d->module == currentLookupModule() ||
               (d->isPub && moduleVisibleFrom(d->module));
    if (vis) { if (!found) found = d; ++nvis; }
  }
  if (nvis == 1) return found;
  if (nvis > 1) {
    std::string mods;
    for (auto* d : cands)
      if (moduleVisibleFrom(d->module)) {
        if (!mods.empty()) mods += ", ";
        mods += d->module.empty() ? "<prelude>" : d->module;
      }
    diag(Diag::Error, Code::Import,
         "ambiguous " + kind + " '" + name + "' — defined by modules " + mods +
         "; qualify the name (mod::" + name + ") or remove one import",
         loc.file, loc.line, loc.col);
    return found;
  }
  // nothing visible — distinguish "private" from "not imported"
  D* d = cands[0];
  if (!d->isPub) {
    diag(Diag::Error, Code::Import,
         kind + " '" + name + "' is private to module '" +
         (d->module.empty() ? "<prelude>" : d->module) +
         "' — mark it `pub` to use it from " +
         (currentLookupModule().empty() ? std::string("this module")
                                        : "module '" + currentLookupModule() + "'"),
         loc.file, loc.line, loc.col);
    return nullptr;
  }
  diag(Diag::Error, Code::Import,
       kind + " '" + name + "' lives in module '" + d->module +
       "', which is not imported here (add: import " + d->module + ";)",
       loc.file, loc.line, loc.col);
  return nullptr;
}
// IR symbol for a function (collision-aware; see above)
std::string irFnSymbol(FnDecl* f);
// IR symbol for an internal global (collision-aware)
std::string irGlobalSymbol(GlobalVar* g);
// builtin modules dir (beside the binary: ../modules, or the installed
// share/penstock/modules)
std::string builtinModulesDir();
// parse one module file into a Program (shared by loader + LSP)
Program* parseModuleFile(const std::string& path,
                          std::vector<std::string>& linkLibs);

// Phase 3b: formatter (src/fmt.cpp) — returns true when the file was
// (or with --check, would be) reformatted
bool formatSourceFile(const std::string& path, bool checkOnly);

// Phase 3 (report §6.2): project manifest (penstock.toml) loader —
// returns false with diagnostics on parse errors
bool loadProjectManifest(const std::string& path);

// Phase 4 (report §10.3): test-runner harness synthesis — replaces the
// entry module's main with a generated main that runs every @test fn
// through the frt_test_* runtime. Returns false on @test misuse.
bool synthesizeTestMain(Program* p);

} // namespace freshet
