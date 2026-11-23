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

#define FRESHET_VERSION "0.1.0-INDEV"  // report integration: Phases 0-5 + §5/§7/§8 policies

// ---------------------------------------------------------------- diagnostics
struct Diag {
  enum Severity { Error, Warning, Note };
  Severity sev;
  std::string code;      // stable public ID ("F0402"), see diag_codes.h
  std::string msg;
  std::string file;
  int line, col;
  int endLine = 0, endCol = 0; // exclusive end; 0 means point-only fallback
  std::string extra;     // machine-readable detail for report merging
};
inline std::vector<Diag> g_diags;
inline void diag(Diag::Severity s, Code code, const std::string& msg,
                 const std::string& file = "", int line = 0, int col = 0,
                 const std::string& extra = "") {
  g_diags.push_back({s, info(code).id, msg, file, line, col, 0, 0, extra});
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
// Source-aware diagnostic overload.  Unlike the legacy (file,line,col)
// form, this preserves the full token/AST span for editors and terminal
// renderers.  SrcLoc end positions are exclusive, matching LSP ranges.
inline void diag(Diag::Severity s, Code code, const std::string& msg,
                 const SrcLoc& loc, const std::string& extra = "") {
  g_diags.push_back({s, info(code).id, msg, loc.file, loc.line, loc.col,
                     loc.endLine, loc.endCol, extra});
}

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
  // fsht-03:02 (registry 03.02.09): `borrowed mut` — the callee gets
  // exclusive write access; the call site must pass a mutable borrow
  // (`&mut place`, or the legacy `&place` spelling at the mutation
  // builtins).  A bare pointer value or a shared borrow is F0303.
  bool borrowedMut = false;
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
  // fsht-02:03 (registry 06.03.01): parallel to tparams — true when the
  // parameter is `const N: usize` (a compile-time value) rather than a
  // type.  The list is sized lazily by the parser; missing entries mean
  // a type parameter.
  std::vector<bool> tparamIsConst;
  // concrete const arguments (values for the const positions of the
  // originating template's tparams, recorded on instances)
  std::vector<uint64_t> constTargs;
  // concrete type arguments (parallel to the originating template's
  // tparams; empty for non-instantiated structs) — drives inference at
  // generic call sites
  std::vector<Type*> targs;
  // fsht-02:01: for a monomorphised instance, the template it came from
  // (nullptr for directly-declared structs and for the templates
  // themselves).  Lets the container predicates recognise Vec<T>
  // instantiations structurally instead of by name mangling.
  StructDef* templateOf = nullptr;
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
  // fsht-02:03: const parameters of a generic enum template (parallel
  // to tparams, see StructDef::tparamIsConst)
  std::vector<bool> tparamIsConst;
  std::vector<uint64_t> constTargs;
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
std::vector<StructDef*> allStructDefs();
// prelude types (generics.cpp): String struct + Option/Result enums
StructDef* ensurePreludeString(Program* prog);

// ---------------------------------------------------------------- traits
// fsht-02:02 (registry 06.02.01) — user-defined traits with static
// dispatch through monomorphisation.  A trait declaration is a pure
// SIGNATURE set: no fields, no bodies, no code of its own.  `Self` in a
// method signature is the placeholder for the implementing type — the
// parser keeps the raw binder encoding "Self" and the impl machinery
// substitutes it textually (see traits.cpp).
struct FnDecl;   // impl members (full definition below)
struct Stmt;
struct Expr;     // const-eval expression trees
//
// The first cut is deliberately narrow, matching the registry's
// static-dispatch-first requirement:
//   * impls target CONCRETE types only (scalars, structs, enum types,
//     instantiated generic structs like Vec<i64>) — blanket impls over
//     template parameters are deferred with the registry entry
//   * dynamic dispatch (trait objects / vtables) is a later opt-in and
//     never the default
struct TraitDef {
  struct Method {
    std::string name;
    std::vector<Param> params;    // tyName encodings; "Self" placeholder
    std::string retEnc;           // "" = void
    SrcLoc loc;
  };
  std::string name;
  std::vector<Method> methods;
  SrcLoc loc;
  // Phase 3a conventions: owning module + pub visibility
  std::string module;
  bool isPub = false;
};

// One `impl Trait for Type { ... }` block.  The member functions are
// ordinary FnDecls (added to Program::fns); their FINAL names are the
// mangled `Trait$<type>$method` forms assigned by sema before fn-map
// registration, so every downstream pass (effects, checks, irgen, LLVM)
// sees plain direct calls — static dispatch by construction, no vtables.
struct ImplDef {
  std::string traitName;
  std::string selfEnc;         // binder encoding of the target type
  Type* selfTy = nullptr;      // resolved during sema
  std::vector<FnDecl*> fns;    // impl member fns (mangled in sema)
  SrcLoc loc;
  std::string module;
  bool isPub = false;
};

// traits.cpp
TraitDef* findTraitDef(const std::string& name);
void registerTrait(TraitDef* td);
std::vector<TraitDef*> allTraitDefs();
// impl table (traits.cpp)
void registerImpl(ImplDef* im);
std::vector<ImplDef*>& allImpls();
// declared trait method lookup (nullptr when the trait lacks the method)
TraitDef::Method* findTraitMethodIn(const std::string& trait,
                                    const std::string& method);
// textual `Self` -> target substitution over a binder encoding
std::string substSelfInEnc(const std::string& enc, const std::string& selfEnc);
// mangled impl function name: Trait$<mangled type>$method
std::string traitMethodMangle(const std::string& trait, Type* selfTy,
                              const std::string& method);
// find the impl of `trait` for exactly `selfTy` (nullptr when absent)
struct ImplDef* findImplFor(const std::string& trait, Type* selfTy);
// constraint satisfaction over `+`-separated parts: builtins (Numeric, Int,
// Float, Signed) or user traits (impl present for the type)
bool typeSatisfiesConstraints(Type* t, const std::string& cons);
// first malformed part of a constraint list ("" when well-formed)
std::string firstUnknownConstraintPart(const std::string& cons);
// resolve a `Prefix::method(...)` trait-method call.  `argTys` are the
// call's argument types (may contain nullptrs while untyped); preferSelf
// is the implementing type when the caller knows it (impl bodies, unique
// bound tparam).  Returns the impl FnDecl or nullptr after emitting the
// appropriate diagnostic (F0216/F0218) — callers fall through to other
// resolution paths when the prefix is not a trait/type at all.
FnDecl* resolveTraitMethodCall(const std::string& prefix,
                               const std::string& method,
                               const std::vector<Type*>& argTys,
                               Type* preferSelf, SrcLoc loc);
// rewrite `Trait::method` / `T::method` calls inside a freshly cloned
// generic instance body, where tmap supplies the concrete types.  Calls
// that cannot be resolved HERE are left for the instance body sema,
// which sees typed arguments and retries through the same resolver.
void rewriteTraitCallsInBody(Stmt* body, FnDecl* tmpl,
                             const std::map<std::string, Type*>& tmap,
                             Program* prog);

// ---------------------------------------------------------------- const eval
// fsht-02:03 (registry 06.03.01) — compile-time evaluation for const
// generics.  `const fn` bodies are interpreted over the u64 domain
// (usize values) with a step/depth budget; anything the interpreter
// cannot prove constant leaves the compile with F0219.
// Evaluate a const-expression ENCODING (see parser.cpp):
//   "5"              literal
//   "N"              const parameter name (looked up in cenv)
//   "(A + B)"        binary op over encodings (fully parenthesised)
//   "sq(3,4)"        call to a const fn with encoded arguments
bool evalConstEnc(const std::string& enc, const std::map<std::string, uint64_t>* cenv,
                  SrcLoc loc, uint64_t* out);
// interpret a `const fn` with concrete arguments (depth-capped)
bool evalConstFnCall(FnDecl* f, const std::vector<uint64_t>& args, uint64_t* out,
                     int depth = 0);
// evaluate a source expression tree under a constant environment
bool evalConstExpr(Expr* e, const std::map<std::string, uint64_t>& env,
                   uint64_t* out, SrcLoc loc);

// fsht-02:01 (registry 02.09.01/02.09.02) — dynamic collections.
// Vec<T> is a builtin GENERIC template struct (monomorphised per element
// type exactly like user generic structs) laid out as the triple
//   { data: *T, len: u64, cap: u64 }
// and the prelude String is the same shape over u8.  Both are OWNED
// containers: the ownership analysis tracks them (moves invalidate,
// rebinding while owning is an error) and irgen drops them at scope
// exit through the frt_vec_*/frt_string_* runtime (the runtime frees
// only a non-null data pointer and nulls it after every transfer or
// free, so drops are idempotent and moved-from bindings are inert).
StructDef* ensurePreludeVec(Program* prog);
StructDef* instantiateVecType(Program* prog, Type* elem, SrcLoc loc);
// prelude enum templates (Option/Result) for sema callers
EnumDef* ensurePreludeEnumType(const std::string& name);
// fsht-03:01 (registry 05.04.01/05.05.01): the parse/error prelude types.
// ParseError { Invalid, OutOfRange } backs parse_i64/u64/f64's Result
// payloads; IoError { NotFound, Permission, Eof, Busy, Other(i64) }
// backs the streaming-I/O Results.  Reader/Writer are the { handle: i64 }
// prelude structs of chapter 23 §4 — ownership-tracked handles (auto
// close at scope exit under chapter 24's container model).
EnumDef* ensurePreludeParseError(Program* prog);
EnumDef* ensurePreludeIoError(Program* prog);
StructDef* ensurePreludeReader(Program* prog);
StructDef* ensurePreludeWriter(Program* prog);
bool isHandleType(Type* t);               // prelude Reader or Writer
// container predicates (types.cpp): a struct type that is the prelude
// String or any Vec<T> instantiation
bool isContainerType(Type* t);
// true when `t` may legally appear as a Vec element type / inside a
// non-owning aggregate: scalars, str views, POD structs, arrays, enums,
// spans.  Containers (String, Vec<T>) are OWNED — legal as elements and
// inside user shapes since fsht-03:02 (registry 02.09.02 lifting, the
// drop elaboration of 03.02.10), but a type containing them needs shape
// drops / moves / deep clones (typeNeedsDrop, below).
bool isNonOwningType(Type* t);
// fsht-03:02 (registry 03.02.10, ch. 24 §5): does a value of this type
// own storage that must drop at scope exit / transfer on move / deep-
// copy on clone?  true for Vec<T> over owning elements, String, Reader/
// Writer handles, and any user struct/array/enum shape transitively
// containing one.  Pure scalars, str/Span views and POD shapes are
// plain copies (false).
bool typeNeedsDrop(Type* t);
// Vec<T> -> T; nullptr for String
Type* containerElemType(Type* t);

// ---------------------------------------------------------------- symbol info
struct Expr; struct Stmt; struct FnDecl;
struct Sym {
  enum K { VAR, PARAM, FN, GLOBAL, STRUCT } k;
  std::string name;
  Type* ty = nullptr;
  bool mut = false;
  bool owned = false;       // owns heap memory (malloc result / give arg)
  // fsht-03:02 (ch. 24 §1): the binding HOLDS a `&mut` exclusive borrow
  // — writes through it are legal even on an immutable binding (the
  // exclusivity is enforced by the F0310 borrow analysis, not by const)
  bool mutBorrow = false;
  FnDecl* fn = nullptr;
  StructDef* sdef = nullptr;
  int scope = 0;
  SrcLoc declLoc;
};

struct FnSema {
  FnDecl* fn = nullptr;
  // Identifier expressions retain Sym* annotations after semantic analysis
  // has left their lexical scope (the LSP uses them for hover/definition/
  // references).  Store symbols in stable heap allocations; the active scope
  // maps only index those allocations and may safely be discarded on pop().
  std::vector<std::map<std::string, Sym*>> scopes;
  std::vector<std::unique_ptr<Sym>> ownedSyms;
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
  // fsht-02:01: bindings holding a borrowed view (str / Span<T>) whose
  // provenance is a local Vec<T> — returning them out of the fn would
  // dangle after the container's scope-exit drop (the lifetime analysis
  // of fsht-03:02 is the full fix)
  std::set<std::string> containerViewVars;
  std::map<Stmt*, std::string> stmtWrites;  // ASSIGN stmt -> root var name

  Sym* lookup(const std::string& n) {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
      auto f = it->find(n);
      if (f != it->end()) return f->second;
    }
    return nullptr;
  }
  void push() { scopes.emplace_back(); ++scopeDepth; }
  void pop() { if (!scopes.empty()) { scopes.pop_back(); --scopeDepth; } }
  void declare(Sym s) {
    s.scope = scopeDepth;
    auto owned = std::make_unique<Sym>(std::move(s));
    Sym* stable = owned.get();
    ownedSyms.push_back(std::move(owned));
    if (!scopes.empty()) scopes.back()[stable->name] = stable;
    // Keep the historical by-value index for passes that iterate all symbols.
    allSyms.push_back(*stable);
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
// fsht-03:01 (registry 08.01.04): set checked_cast/saturating_cast's
// destination U from a context type (Option<U> unwraps to U); called at
// the binding/return/argument/assignment sites before init typing.
void trySetCastTarget(FnSema* fs, Expr* e, Type* expected);
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
  // fsht-03:02 (registry 03.02.09): `&mut place` — the exclusive
  // mutable-borrow spelling of the UN '&' node (`&place` stays the shared
  // spelling / the legacy argument form at the mutation builtins).
  bool borrowMut = false;
  // fsht-03:01 (registry 08.01.04): resolved destination type U of a
  // `checked_cast<T, U>(x)` / `saturating_cast<T, U>(x)` call — inferred
  // from the binding/context (sema_expr trySetCastTarget); nullptr while
  // unresolved (F0201 at typing time).
  Type* castTargetTy = nullptr;
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
  // BCE (goose port): set by the bounds-check elimination pass when
  // 0 <= idx <= len-1 (INDEX) or 0 <= lo <= hi <= len (SLICE) is proven at
  // this site; IRgen then emits no check.  Checked builds consume it;
  // unchecked builds ignore it (no check would be emitted anyway).
  bool bceSafe = false;
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
  // the binding's declLoc key (sema assigns arm.loc + payload index —
  // unique per match/arm/payload so two matches binding the same name
  // never share a slot); IRgen resolves the payload store through it
  std::vector<SrcLoc> bindingLocs;
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
  // the backend to vectorise (emits llvm.loop.vectorize.enable).  The
  // default emits NO forced metadata; --force-vectorize restores the
  // historical blanket hint globally for differential testing.
  bool vectorizeHint = false;
  // Phase 5 (report §12): @reproducible on a parfor — the reduction is
  // computed with a fixed block partition and strict-FP combines, so
  // the result is bitwise identical for any thread count / scheduler.
  bool reproducible = false;
  // ---- SMT memory/compute interleave (mem & vectorisation stage) ---------
  // Set by the --smt-interleave pairing pass on the FIRST region of a
  // legal adjacent pair: IRgen emits ONE frt_parfor_pair dispatch covering
  // both regions instead of two synchronous frt_parfor calls.  The partner
  // is marked smtPaired and generates no dispatch of its own.
  Stmt* smtPairWith = nullptr;    // this region dispatches together with...
  bool smtPaired = false;         // ...this region's dispatch lives elsewhere
  // memory/compute census class of the body (filled by the pairing pass;
  // IRgen bakes it into frt_range_t.mem_class at offset 72):
  //   0 = unknown, 1 = memory-bound, 2 = compute-bound, 3 = balanced
  int smtMemClass = 0;
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
  bool readsStack = false;   // reads the thread-local data-stack tops
  bool writesStack = false;  // data-stack bumps (goose port): per-thread
                             // scratch, invisible outside the call
  bool allocates = false;   // malloc / soa_new
  bool freesMem = false;    // free / soa_free
  bool io = false;           // print* / println — externally visible output
  bool unknownCall = false;  // call whose effects we cannot describe
  bool mayNotReturn = false; // loops (while / c-for / parfor), assert, recursion

  bool anyRead() const { return readsArg || readsOther; }
  bool anyWrite() const { return writesArg || writesOther; }
  // "pure" in the Freshet observable sense used by the parfor checks
  // and the R4 loop passes: no writes, no I/O, no allocation, no
  // unknown calls.  Reads are permitted.  Data-stack bumps are NOT
  // observable impurity (goose port v2): each thread owns its stacks,
  // so no iteration, thread or caller can see them — a stacks-only
  // function is pure, which is what makes stack-using helpers legal
  // inside parfor bodies under the %sp-threaded dispatch ABI.
  bool semanticallyPure() const {
    return !anyWrite() && !io && !allocates && !freesMem && !unknownCall;
  }
  void merge(const Effects& o) {
    readsArg |= o.readsArg;     readsOther |= o.readsOther;
    writesArg |= o.writesArg;  writesOther |= o.writesOther;
    readsStack |= o.readsStack; writesStack |= o.writesStack;
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
  // BCE (goose port): `@checkfree fn` — no bounds check may remain in
  // this function or anything it calls; the BCE pass raises F0924 on any
  // check site it cannot prove safe (goose: "no bounds check runs while a
  // checkfree function does").
  bool checkfree = false;   // @checkfree
  // Phase 2a: generic function template (monomorphised per call site)
  bool isGeneric = false;
  std::vector<std::string> tparams;
  std::vector<std::string> tconstraints;   // parallel to tparams ("" = none;
                                          // may be "TraitA+TraitB" — all parts
                                          // must hold; fsht-02:02)
  // fsht-02:03: parallel to tparams — true for `const N: usize` params
  std::vector<bool> tparamIsConst;
  // instantiated bodies rebind their type strings through this map
  // (empty for ordinary functions and templates)
  std::map<std::string, Type*> instTMap;
  // fsht-02:03: const-parameter values for instances (parallel to instTMap)
  std::map<std::string, uint64_t> instCMap;
  // fsht-02:02: this fn is a member of an `impl Trait for Type` block.
  // The parser stores the raw spelling; sema resolves the target type,
  // renames the fn to the mangled `Trait$<type>$method` form and records
  // both here for trait-call resolution inside impl bodies.
  bool isTraitImpl = false;
  std::string traitName;       // originating trait
  std::string implSelfEnc;     // parser-time encoding of the target type
  Type* implSelfTy = nullptr;  // sema-resolved target type
  // fsht-02:02: this fn is a `const fn` — its body is interpretable at
  // compile time for const-generic arguments and const_assert (registry
  // 06.03.01).  Const fns are ALSO ordinary runtime fns: they are sema'd,
  // emitted and callable normally.
  bool isConst = false;
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
  // fsht-02:02: trait declarations and impl blocks (parsed order)
  std::vector<TraitDef*> traits;
  std::vector<ImplDef*> impls;
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
  // Phase 3a (report §6.2): multi-file compilation — every .fsht input
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

  // fsht tooling surface (registry 07.04.01): automatic C header
  // generation from the `export fn` surface.  "" off, "<auto>" ->
  // <input-stem>.h; library targets (static-lib/shared-lib) default it
  // to "<auto>" so C consumers get a header with every build.
  std::string emitCHeader;

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

  // ---- mem & vectorisation stage (mov-min / mem-reuse / batch) ----------
  // Opt-in like every Phase-D transform (audit §11-D), each with a --no-*
  // differential hatch.  --mem-stage is the umbrella that turns the three
  // sub-passes on together.
  bool movMinOn = false;          // --mov-minimize: redundant mov removal
  bool memReuseOn = false;        // --mem-reuse: free/alloc churn reuse
  bool batchOpsOn = false;       // --batch-ops: SLP-oriented statement batching
  bool vectorPlanOn = false;      // --vector-plan: separable-routine advisory
  // ---- SMT memory/compute interleave --------------------------------------
  // Pair adjacent independent parfor regions with complementary
  // memory/compute character; the runtime co-schedules their chunks on the
  // two hardware threads of one physical core (one stream keeps the shared
  // ALU busy while the other waits on memory).
  bool smtInterleaveOn = false;   // --smt-interleave

  // ---- goose-ported passes (see the block above runMemStagePasses) ------
  // BCE follows goose's own policy (on by default, --no-bce as the
  // differential hatch): it never rewrites the tree, it only stops
  // emitting checks the solver has PROVEN cannot fire, so checked builds
  // become "memory safety without the checks' cost".
  bool bceOn = true;            // --bce (default on; --no-bce disables)
  bool bceLinesOn = false;      // --bce-lines: per-line elided/kept report
  bool dstkReportOn = false;    // --stacks: data-stack layout report
  bool sumRedOn = false;        // --sum-reduction (goose sumred)
  bool basecaseOn = false;      // --basecase (goose basecase)

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
  // and import resolution; the builtin modules dir — home of the shipped
  // `fsh` module — is searched after the -I dirs, with only the current
  // directory after it)
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
bool semaProgram(Program* p, bool requireMain = true);
// sema accessors for cross-TU phases (sema_expr.cpp / generics.cpp)
void queueGenericInstance(FnDecl* inst);
Program* currentProgram();
// contextual resolution helpers (sema_expr.cpp)
void tryResolveVariantExpr(FnSema* fs, Expr* e, Type* expected);
void tryAdoptStructLiteral(Expr* e, Type* expected);
// generic-instance scope: the current function's T -> concrete map
Type* tparamTypeInScope(const std::string& name);
// fsht-02:03: const-parameter value of the current instantiation
uint64_t* constParamValueInScope(const std::string& name);
// generics (Phase 2a): monomorphisation helpers (generics.cpp)
// fsht-02:03: constArgs carries const-generic values (parallel to the
// template's const parameter positions; empty for pure-type templates)
StructDef* instantiateStruct(Program* prog, StructDef* tmpl,
                             const std::vector<Type*>& args, SrcLoc loc,
                             const std::vector<uint64_t>& constArgs = {});
EnumDef* instantiateEnum(Program* prog, EnumDef* tmpl,
                         const std::vector<Type*>& args, SrcLoc loc,
                         const std::vector<uint64_t>& constArgs = {});
FnDecl* instantiateGenericFn(Program* prog, FnDecl* tmpl,
                             const std::vector<Type*>& argTypes, SrcLoc loc);
Type* bindTypeEncoding(Program* prog, const std::string& enc, SrcLoc loc,
                       const std::map<std::string, Type*>* tmap,
                       const std::map<std::string, uint64_t>* cenv = nullptr);
// checks (strict)
void runStrictChecks(Program* p);
void runParforChecks(Program* p);
// ownership
void runOwnershipAnalysis(Program* p);
// fsht-03:02 (registry 03.02.09/03.02.10): the borrow discipline —
// F0310 exclusivity + F0311 escape (ownership.cpp)
void runBorrowAnalysis(Program* p);
// fsht tooling surface (registry 07.04.01): automatic C header generation
// — a .h describing the C ABI of every `export fn` (cheader.cpp)
bool emitCHeader(Program* p, const std::string& path);

// Resolved backend LLVM major (0 = not probed yet).  main.cpp scrapes it
// from the backend clang --version once per invocation, before IRgen.
// The backend matrix spans LLVM 19-23, and a few intrinsic spellings
// changed across it — notably FP min/max: LLVM 19 spells them
// llvm.minnum/llvm.maxnum; LLVM 20+ renamed them llvm.fmin/llvm.fmax
// (19's parser treats the new spelling as an unknown external symbol
// and the link fails with "undefined reference to llvm.fmin.f64").
extern int g_backendLlvmMajor;
inline const char* llvmFminBase() {
  return g_backendLlvmMajor >= 20 ? "llvm.fmin" : "llvm.minnum";
}
inline const char* llvmFmaxBase() {
  return g_backendLlvmMajor >= 20 ? "llvm.fmax" : "llvm.maxnum";
}
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

// mem & vectorisation stage (mem_stage.cpp / vector_plan.cpp), post-sema
// pre-irgen like the R4 passes, all opt-in:
//   --mem-reuse   alloc-churn hoisting: a loop that malloc/soa_news a fresh
//                 buffer every iteration and frees it at iteration end
//                 reuses ONE allocation (reads must be covered by
//                 same-iteration writes); free->alloc pairs of identical
//                 shape fuse into a reuse.
//   --mov-minimize redundant value moves removed: self-assignments,
//                 dead `let a = b` copies, immutable mov forwarding
//                 (incl. aggregate copies that lower to memcpy), chained.
//   --batch-ops   SLP-oriented statement batching driven by the
//                 separable-routine analysis (vector_plan.cpp).
//   --vector-plan advisory: reports each function's separable routines,
//                 their batchability and dataflow (never rewrites).
//   --smt-interleave pairs adjacent independent parfor regions with
//                 complementary memory/compute character so the runtime
//                 can co-schedule them on SMT siblings of one core.
struct MemStageCounts {
  int memReuse = 0;
  int movMin = 0;
  int batched = 0;
  int vecPlanNotes = 0;
  int smtPairs = 0;
  int total() const {
    return memReuse + movMin + batched + vecPlanNotes + smtPairs;
  }
};
MemStageCounts runMemStagePasses(Program* p);   // mem-reuse + mov-minimize
int runVectorPlanPass(Program* p, bool applyBatching);  // B analysis (+A3)
int runSmtInterleavePass(Program* p);              // parfor pairing marks

// ------------------------------------------------------------- goose ports
// Passes adapted from the Goose programming language (aardappel/goose) —
// the memory-safe systems language with no heap allocator, garbage
// collector or lifetime annotations whose optimiser and data-stack memory
// model this compiler studied and ported where Penstock's LLVM-IR backend
// does not already provide the equivalent:
//   --bce           bounds-check elimination (goose src/bce.h, ~4.6k lines
//                   reduced to Freshet's simpler by-value world): a
//                   flow-sensitive difference-constraint solver (facts
//                   `l <= r + c` over zero / int vars / span-str-array
//                   lengths / one-shot temporaries, with generation
//                   pinning so kills cannot leak stale facts) proves
//                   0 <= idx <= len-1 at INDEX sites and
//                   0 <= lo <= hi <= len at SLICE sites, and marks them
//                   so IRgen emits no check.  Facts come from loop and
//                   parfor iteration ranges, `assert`, dominating
//                   conditions, span/slice construction and completed
//                   checks; assignments, address-taking and call effects
//                   (via the FnFootprint summaries) invalidate them.
//                   `@checkfree fn` rejects any check the pass keeps.
//                   (goose runs BCE once, last before codegen; so do we.)
//   --stacks        data-stack layout report (goose codegen_stacks.h
//                   StackReport): per-function own / with-callees stack
//                   counts for the stack_alloc memory model.
//   --sum-reduction bit-identical blocked float reductions (goose
//                   optimize_loops.h LoopShaper::ShapeSum): regroup
//                   `s += e` loops so a block of terms is computed first
//                   (independent, vectorisable) and folded into s one by
//                   one in the ORIGINAL order — every result bit for bit,
//                   because the addition chain is unchanged.
//   --basecase      recursive base-case inlining (goose
//                   optimize_basecase.h): a self-recursive fn whose body
//                   starts with `if c { return e; }` (c and e reading only
//                   params, globals and their own bindings) gets every
//                   tail `return f(a...)` rewritten to
//                     { let p... ; if c[p:=t] { return e[p:=t]; }
//                       return f(p...) }
//                   removing half the calls of a full tree walk.  (Only
//                   return-position self-calls: Freshet's `if` is a
//                   statement, not an expression like goose's.)
// Deliberately NOT ported (the clang -O3 backend already provides them on
// our .ll, where goose emits C and must do them itself): the goose
// inliner + constant folder (optimize.h — LLVM's inliner/instcombine),
// accumulator tail-recursion elimination (optimize_tre.h — LLVM
// tailcallelim), integer min/max chain regrouping (optimize_reassoc.h) and
// loop strip-mining marks (optimize_loops.h ShapeStrip — partial-unroll
// already emits the same register-friendly shape).
struct BceCounts {
  int idxElided = 0, idxTotal = 0;
  int slElided = 0, slTotal = 0;
  int entrySeeded = 0;   // fns granted interprocedural entry facts
};
BceCounts runBcePass(Program* p);   // bounds-check elimination + @checkfree
int runFpReductionPass(Program* p); // blocked FP sum reductions
int runBasecasePass(Program* p);    // recursive base-case inlining
int runDstkAnalysis(Program* p);    // data-stack layout + escape checking
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

// In-memory formatter surface for the language server (LSP
// textDocument/formatting + textDocument/rangeFormatting; report §10.2).
// `formatSourceText` returns the full formatted document; `*ok` (the
// pointer may be null) is false when the token-preserving safety gate
// refuses, in which case the returned text must NOT be applied.
std::string formatSourceText(const std::string& src, int tabSize,
                             bool insertSpaces, bool* ok);

// Formats ONLY the toplevel declarations intersecting [startOff, endOff)
// of `src`.  The result replaces exactly the byte span [start, end);
// bytes outside it are untouched.  `ok` is false when the selection covers
// no declaration or the safety gate refuses.
struct RangeFormatResult {
  size_t start = 0, end = 0;   // byte span of the document to replace
  std::string text;            // the replacement for those bytes
  bool ok = false;
};
RangeFormatResult formatSourceRange(const std::string& src, size_t startOff,
                                    size_t endOff, int tabSize,
                                    bool insertSpaces);

// Phase 3 (report §6.2): project manifest (penstock.toml) loader —
// returns false with diagnostics on parse errors
bool loadProjectManifest(const std::string& path);

// Phase 4 (report §10.3): test-runner harness synthesis — replaces the
// entry module's main with a generated main that runs every @test fn
// through the frt_test_* runtime. Returns false on @test misuse.
bool synthesizeTestMain(Program* p);

} // namespace freshet
