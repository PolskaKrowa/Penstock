// Freshet IRgen — LLVM IR text emission (module scaffolding, types, globals,
// runtime decls, DWARF lines, PGO counters, attribute groups).
// User fns use attribute groups #50+ by convention, but no margin is relied
// on: the driver's combine step (main.cpp shiftAttrGroupIds) renumbers the
// runtime .ll's group ids above the user module's maximum before
// concatenation, so the two halves' `attributes #N` spaces can never
// overlap regardless of how either side allocates them.
#include "irgen.h"
#include "dstk.h"

namespace freshet {

IrGen* G = nullptr;
Options g_opts;
std::string g_backend;
std::string g_targetTriple;   // §12: "" = native-host default
std::string g_dataLayout;

// ---------------------------------------------------------------- runtime decls
static const char* RUNTIME_DECLS = R"(
declare i64 @frt_time_ns()
declare i64 @frt_time_ms()
declare i64 @frt_rdtsc()
declare ptr @frt_malloc(i64, i64)
declare void @frt_free(ptr)
declare void @frt_memset(ptr, i64, i64)
declare void @frt_memcpy(ptr, ptr, i64)
declare void @frt_print_str(ptr, i64)
declare void @frt_print_f64(double, i64)
declare void @frt_print_i64(i64)
declare void @frt_print_u64(i64)
declare void @frt_println()
declare void @frt_soa_free(ptr)
declare ptr @frt_soa_new(i64, i32, ptr)
declare void @frt_clflush(ptr)
declare void @frt_pause()
declare void @frt_assert(i1)
declare void @frt_init()
declare i32 @frt_parfor(ptr, ptr, i32)
; mem & vectorisation stage (--smt-interleave): paired dispatch of two
; independent parfor regions — the runtime queues the first region's
; chunks on the memory lane and the second's on the compute lane so an
; SMT core can overlap memory streaming with ALU work on the sibling
; hardware thread.  Returns (nB << 32) | nA packed into one i64.
declare i64 @frt_parfor_pair(ptr, ptr, ptr, ptr, i32)
; Phase 4 (report §10.3): @test runner harness (runtime/frt_rt.c).
; frt_test_run takes the { ptr, i64 } str view + the test fn as a C
; function pointer (the harness passes the @test fn by name).
declare void @frt_test_setup()
declare i32 @frt_test_run({ ptr, i64 }, ptr)
declare i32 @frt_test_summary()
; Phase 4 (report §4.7/§9): args / env / file I/O over str views — the
; Freshet-facing wrappers live in the shipped `fsh` module (modules/fsh.fsht)
declare i64 @frt_args_count()
declare { ptr, i64 } @frt_args_get(i64)
declare { ptr, i64 } @frt_env_get({ ptr, i64 })
declare i64 @frt_file_size({ ptr, i64 })
declare { ptr, i64 } @frt_file_read({ ptr, i64 })
declare i64 @frt_file_write({ ptr, i64 }, { ptr, i64 })
declare void @frt_prof_register(i32, ptr, ptr, i32, ptr, ptr)
; Phase 2b (report §4.5): string runtime support (runtime/frt_rt.c).
; String is the 24-byte { ptr, i64, i64 } struct; constructors write
; through an OUT pointer so the C ABI stays simple.
declare i32 @frt_str_eq(ptr, i64, ptr, i64)
declare void @frt_str_cat(ptr, ptr, i64, ptr, i64)
declare void @frt_string_init(ptr)
declare void @frt_string_from(ptr, ptr, i64)
declare void @frt_string_append(ptr, ptr, i64)
declare void @frt_string_free(ptr)
; fsht-02:01 (registry 02.09.01): dynamic-collection runtime.  Vec<T> is
; the { ptr, i64, i64 } struct; esz is the monomorphised element size.
; Constructors write through an OUT pointer (C ABI parity with String).
; frt_vec_pop/get/remove return i32 "found" and write the element
; through `out` — irgen builds the Option<T> from that flag.
declare void @frt_vec_init(ptr)
declare void @frt_vec_with_cap(ptr, i64, i64)
declare void @frt_vec_from(ptr, ptr, i64, i64)
declare void @frt_vec_clone(ptr, ptr, i64)
declare void @frt_vec_push(ptr, ptr, i64)
declare i32 @frt_vec_pop(ptr, ptr, i64)
declare i32 @frt_vec_get(ptr, i64, ptr, i64)
declare void @frt_vec_insert(ptr, i64, ptr, i64)
declare i32 @frt_vec_remove(ptr, i64, ptr, i64)
declare void @frt_vec_reserve(ptr, i64, i64)
declare void @frt_vec_truncate(ptr, i64)
declare void @frt_vec_resize(ptr, i64, ptr, i64)
declare void @frt_vec_clear(ptr)
declare void @frt_vec_free(ptr)
; fsht-03:01 (registry 05.04.01, ch. 23 §1): numeric text parsing — the
; runtime parses a str view and returns { value, err } with err
; 0 = Ok, 1 = Invalid, 2 = OutOfRange (the ParseError mapping).
declare { i64, i32 } @frt_parse_i64(ptr, i64)
declare { i64, i32 } @frt_parse_u64(ptr, i64)
declare { double, i32 } @frt_parse_f64(ptr, i64)
; fsht-03:01 (ch. 23 §5): random / alloc / numeric module minimum content
declare i64 @frt_sm64_next(ptr)
declare i64 @frt_page_size()
declare i64 @frt_gcd_i64(i64, i64)
; fsht-03:02 (ch. 24 §4): the String migration's new operations
declare void @frt_string_clone(ptr, ptr)
declare void @frt_string_clear(ptr)
; fsht-03:02 (ch. 24 §5): aliasing-safe in-place vec clone (deep element copy)
declare void @frt_vec_clone_aliased(ptr, ptr, i64)
; fsht-03:01 (registry 08.01.04, ch. 23 §2–§3): the checked/saturating
; variant family lowers onto the LLVM overflow intrinsics at the exact
; source width (checked_* also covers div/rem traps: divisor 0 and
; INT_MIN / -1).
declare { i8, i1 } @llvm.sadd.with.overflow.i8(i8, i8)
declare { i8, i1 } @llvm.ssub.with.overflow.i8(i8, i8)
declare { i8, i1 } @llvm.smul.with.overflow.i8(i8, i8)
declare { i8, i1 } @llvm.uadd.with.overflow.i8(i8, i8)
declare { i8, i1 } @llvm.usub.with.overflow.i8(i8, i8)
declare { i8, i1 } @llvm.umul.with.overflow.i8(i8, i8)
declare { i16, i1 } @llvm.sadd.with.overflow.i16(i16, i16)
declare { i16, i1 } @llvm.ssub.with.overflow.i16(i16, i16)
declare { i16, i1 } @llvm.smul.with.overflow.i16(i16, i16)
declare { i16, i1 } @llvm.uadd.with.overflow.i16(i16, i16)
declare { i16, i1 } @llvm.usub.with.overflow.i16(i16, i16)
declare { i16, i1 } @llvm.umul.with.overflow.i16(i16, i16)
declare { i32, i1 } @llvm.sadd.with.overflow.i32(i32, i32)
declare { i32, i1 } @llvm.ssub.with.overflow.i32(i32, i32)
declare { i32, i1 } @llvm.smul.with.overflow.i32(i32, i32)
declare { i32, i1 } @llvm.uadd.with.overflow.i32(i32, i32)
declare { i32, i1 } @llvm.usub.with.overflow.i32(i32, i32)
declare { i32, i1 } @llvm.umul.with.overflow.i32(i32, i32)
declare { i64, i1 } @llvm.sadd.with.overflow.i64(i64, i64)
declare { i64, i1 } @llvm.ssub.with.overflow.i64(i64, i64)
declare { i64, i1 } @llvm.smul.with.overflow.i64(i64, i64)
declare { i64, i1 } @llvm.uadd.with.overflow.i64(i64, i64)
declare { i64, i1 } @llvm.usub.with.overflow.i64(i64, i64)
declare { i64, i1 } @llvm.umul.with.overflow.i64(i64, i64)
; ch. 23 §5 bits/numeric content: byte swap, population count, |x|,
; and the min/max select intrinsics used by clamp/gcd-style saturation
declare i16 @llvm.bswap.i16(i16)
declare i32 @llvm.bswap.i32(i32)
declare i64 @llvm.bswap.i64(i64)
declare i8 @llvm.ctpop.i8(i8)
declare i16 @llvm.ctpop.i16(i16)
declare i32 @llvm.ctpop.i32(i32)
declare i64 @llvm.ctpop.i64(i64)
declare i8 @llvm.abs.i8(i8, i1 immarg)
declare i16 @llvm.abs.i16(i16, i1 immarg)
declare i32 @llvm.abs.i32(i32, i1 immarg)
declare i64 @llvm.abs.i64(i64, i1 immarg)
declare i8 @llvm.smax.i8(i8, i8)
declare i8 @llvm.smin.i8(i8, i8)
declare i8 @llvm.umax.i8(i8, i8)
declare i8 @llvm.umin.i8(i8, i8)
declare i16 @llvm.smax.i16(i16, i16)
declare i16 @llvm.smin.i16(i16, i16)
declare i16 @llvm.umax.i16(i16, i16)
declare i16 @llvm.umin.i16(i16, i16)
declare i32 @llvm.smax.i32(i32, i32)
declare i32 @llvm.smin.i32(i32, i32)
declare i32 @llvm.umax.i32(i32, i32)
declare i32 @llvm.umin.i32(i32, i32)
declare i64 @llvm.smax.i64(i64, i64)
declare i64 @llvm.smin.i64(i64, i64)
declare i64 @llvm.umax.i64(i64, i64)
declare i64 @llvm.umin.i64(i64, i64)
declare void @frt_fmt_f64(ptr, double, i64)
declare void @frt_fmt_i64(ptr, i64)
declare ptr @frt_str_to_cstr(ptr, i64)
; Phase 1c (report §4.1): bounds-check trap for checked builds
declare void @frt_bound_trap(i64, i64)
declare void @frt_dstk_init(ptr, i32)
declare void @llvm.prefetch.p0(ptr, i32, i32, i32)
declare void @llvm.memset.p0.i64(ptr, i8, i64, i1 immarg)
declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1 immarg)
declare double @llvm.sqrt.f64(double)
declare double @llvm.fabs.f64(double)
declare double @llvm.floor.f64(double)
declare double @llvm.ceil.f64(double)
declare double @llvm.round.f64(double)
declare double @llvm.pow.f64(double, double)
declare double @llvm.exp.f64(double)
declare double @llvm.log.f64(double)
declare float @llvm.sqrt.f32(float)
declare float @llvm.fabs.f32(float)
declare float @llvm.floor.f32(float)
declare float @llvm.ceil.f32(float)
declare float @llvm.round.f32(float)
declare float @llvm.pow.f32(float, float)
declare float @llvm.exp.f32(float)
declare float @llvm.log.f32(float)
declare double @sin(double)
declare double @cos(double)
declare double @tan(double)
declare double @log2(double)
declare double @log10(double)
declare float @llvm.sin.f32(float)
declare double @llvm.sin.f64(double)
declare float @llvm.cos.f32(float)
declare double @llvm.cos.f64(double)
declare float @llvm.log2.f32(float)
declare double @llvm.log2.f64(double)
declare float @llvm.log10.f32(float)
declare double @llvm.log10.f64(double)
)";

// ---------------------------------------------------------------- types
std::string IrGen::llvmType(Type* t) {
  if (!t) return "void";
  switch (t->k) {
    case Type::U8: case Type::I8: case Type::BOOL: return "i8";
    case Type::U16: case Type::I16: return "i16";
    case Type::U32: case Type::I32: return "i32";
    case Type::U64: case Type::USIZE: case Type::I64: return "i64";
    case Type::F32: return "float";
    case Type::F64: return "double";
    case Type::VOID: return "void";
    // Span<T> and str are first-class { ptr, i64 } aggregates (report
    // §4.1/§4.5): cheap to pass by value, length always travels with the
    // pointer
    case Type::PTR: case Type::SOA: return "ptr";
    // Phase 3b: fn pointers are opaque pointer values (LLVM 15+ opaque
    // pointers) — the C ABI passes them as plain pointers
    case Type::FNPTR: return "ptr";
    case Type::STR: case Type::SPAN: return "{ ptr, i64 }";
    case Type::ARRAY:
      return "[" + std::to_string(t->arrLen) + " x " + llvmType(t->pointee) + "]";
    case Type::STRUCT: return "%struct." + t->sdef->name;
    case Type::ENUM: return "%enum." + t->edef->name;
  }
  return "ptr";
}

// struct/enum/ARRAY values are addresses; SPAN/STR values are first-class
// pair aggregates
bool IrGen::isPairValueTy(Type* t) { return t && (t->k == Type::SPAN || t->k == Type::STR); }
bool IrGen::isAddrValueTy(Type* t) {
  return t && (t->k == Type::STRUCT || t->k == Type::ENUM || t->k == Type::ARRAY);
}

std::string IrGen::constOf(Type* t, Expr* e) {
  auto foldInt = [&](Expr* x) -> int64_t {
    if (x->k == Expr::INT) return (int64_t)x->ival;
    if (x->k == Expr::UN && x->op == "-" && x->rhs->k == Expr::INT)
      return -(int64_t)x->rhs->ival;
    if (x->k == Expr::BOOL) return x->bval ? 1 : 0;
    return 0;
  };
  auto foldFlt = [&](Expr* x) -> double {
    if (x->k == Expr::FLT) return x->fval;
    if (x->k == Expr::INT) return (double)x->ival;
    if (x->k == Expr::UN && x->op == "-" && x->rhs->k == Expr::FLT)
      return -x->rhs->fval;
    if (x->k == Expr::UN && x->op == "-" && x->rhs->k == Expr::INT)
      return -(double)x->rhs->ival;
    return 0;
  };
  if (t->k == Type::F64) {
    double d = foldFlt(e);
    if (d == 0.0) return "0.000000e+00";
    return doubleToHex(d);
  }
  if (t->k == Type::F32) {
    float f = (float)foldFlt(e);
    if (f == 0.0f) return "0.000000e+00";
    return floatToHex(f);
  }
  if (t->k == Type::BOOL)
    return e->k == Expr::BOOL ? (e->bval ? "true" : "false") : "false";
  return std::to_string(foldInt(e));
}

std::string IrGen::doubleToHex(double d) {
  char buf[32];
  union { double d; uint64_t u; } u;
  u.d = d;
  snprintf(buf, sizeof buf, "0x%016lX", u.u);
  return buf;
}
std::string IrGen::floatToHex(float f) {
  // LLVM textual IR fp constants are ALWAYS the 16-hex-digit double-width
  // encoding, even for float-typed values (the 8-digit 32-bit form is a
  // parse error since LLVM 15).  Every f32 value is exactly representable
  // as the double of the same value, so widen through double.
  return doubleToHex((double)f);
}

std::string IrGen::esc(const std::string& s) {
  std::string out;
  static const char* hex = "0123456789ABCDEF";
  for (unsigned char c : s) {
    if (c >= 32 && c < 127 && c != '"' && c != '\\') out += (char)c;
    else {
      out += '\\';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}

int IrGen::dbgSubprogId(FnDecl* f) {
  if (!f) return 902;
  auto it = dbgSubprogs.find(f);
  if (it != dbgSubprogs.end()) return it->second;
  int id = nextDbgSub++;
  if (id >= 1900) return 902;   // numbering guard
  dbgSubprogs[f] = id;
  return id;
}

int IrGen::dbgFileId(const std::string& path) {
  if (path.empty()) return 901;
  auto it = dbgFiles.find(path);
  if (it != dbgFiles.end()) return it->second;
  int id = nextDbgFile++;
  if (id >= 2900) return 901;   // numbering guard
  dbgFiles[path] = id;
  return id;
}

int IrGen::dbgNodeFor(int line, int col) {
  // DILocation nodes, numbered from 300; 890 is the guard (the 900+
  // range is reserved for the compile unit / files / subprograms)
  if (dbgNodes.size() >= 590) return -1;
  int sub = dbgSubprogId(curFn);
  dbgNodes.push_back("!DILocation(line: " + std::to_string(line) +
                     ", column: " + std::to_string(col ? col : 1) +
                     ", scope: !" + std::to_string(sub) + ")");
  return 300 + (int)dbgNodes.size() - 1;
}

// ------------------------------------------------- effect-derived attrs
// audit §2.3: derive the canonical LLVM attribute set of a function from
// its sema effect summary.  Only attributes the body PROVES are emitted:
//
//   memory(none)          — touches no memory at all (not just "no writes")
//   memory(argmem: ...)  — only memory reachable from pointer/soa params
//   memory(read) / etc.  — any memory, read and/or write
//   willreturn           — ONLY when the effect analysis proved termination
//                           (no while / arbitrary-condition for / parfor /
//                           assert / call cycles); NEVER inferred from
//                           effect-freedom alone (a side-effect-free loop
//                           can still run forever)
//
// Definition and call sites share this function, so the two always agree
// (audit §2.3 rule 2) — no stamping a group on a call that the callee's
// own definition does not carry.
std::string IrGen::fnAttrBody(FnDecl* f) {
  // inline hints compose WITH the effect attributes instead of
  // discarding them (a @pure @inline fn keeps both facts)
  bool wantNoinline = f->userNoinline ||
                      g_opts.inlinePolicy == InlinePolicy::Off;
  bool wantAlwaysInline = f->userInline && !f->userNoinline;

  const Effects& e = f->eff;
  std::string mem;
  if (!e.allocates && !e.freesMem && !e.io && !e.unknownCall) {
    // data-stack bumps (goose port) read/write the thread-local tops
    // array — real memory, so a stacks-only fn still claims
    // memory(readwrite), never memory(none)
    bool r = e.anyRead() || e.readsStack;
    bool w = e.anyWrite() || e.writesStack;
    bool other = e.readsOther || e.writesOther || e.readsStack ||
                 e.writesStack;
    if (!r && !w)            mem = "memory(none)";
    else if (!w)              mem = other ? "memory(read)" : "memory(argmem: read)";
    else if (!r)              mem = other ? "memory(write)" : "memory(argmem: write)";
    else                      mem = other ? "memory(readwrite)" : "memory(argmem: readwrite)";
  }
  // `willreturn` needs BOTH a termination proof and a describable
  // memory effect; functions with I/O / allocation / unknown calls keep
  // the conservative plain-nounwind groups.
  bool willret = !e.mayNotReturn && !mem.empty();

  std::string body;
  auto add = [&](const std::string& s) {
    if (!body.empty()) body += " ";
    body += s;
  };
  if (wantNoinline) add("noinline");
  if (wantAlwaysInline) add("alwaysinline");
  add("nounwind");
  if (f->strictfp) add("strictfp");
  if (!mem.empty()) {
    add(mem);
    if (willret) add("willreturn");
  }
  return body;
}

int IrGen::attrGroupFor(FnDecl* f) {
  // exported functions are ABI boundaries: plain #53, no memory claims
  // (external callers must not depend on our summaries)
  if (f->exported) return 53;
  std::string body = fnAttrBody(f);
  auto it = attrGroupIds.find(body);
  if (it != attrGroupIds.end()) return it->second;
  int id = nextAttrGroup++;
  attrGroupIds[body] = id;
  dynAttrGroups.push_back({body, id});
  return id;
}

// §5 loop-hint policy, shared by the FOR lowering (irgen_stmt.cpp) and
// the parfor chunk-fn / collapsed-inner loops (irgen_parfor.cpp):
//   * default: NO forced vectorisation metadata — LLVM's cost model
//     decides; a forced enable overrides profitability and the fixed
//     interleave 4 hurt small loops / register pressure / non-AVX2
//   * @vectorize on the loop: enable only, no interleave count
//   * --force-vectorize: historical blanket hint (enable + interleave 4)
// Returns -1 when no metadata should be attached.
int IrGen::vectorizeLoopMeta(bool userHint) {
  if (!userHint && !g_opts.forceVectorize) return -1;

  // LLVM loop metadata is a self-referential LoopID node.  The loop hint
  // itself is a child metadata node.  Current LLVM requires the
  // vectorize.enable hint to contain ONLY its name string (the historical
  // `i1 true` operand is rejected by the verifier with "Expecting only the
  // metadata name").  A no-argument enable hint is also understood by older
  // LoopVectorize implementations, so this form works across LLVM versions.
  const int id = 200 + static_cast<int>(loopMetas.size());
  std::string metadata = "distinct !{!" + std::to_string(id) +
                        ", !{!\"llvm.loop.vectorize.enable\"}";
  if (!userHint || g_opts.forceVectorize)
    metadata += ", !{!\"llvm.loop.interleave.count\", i32 4}";
  metadata += "}";
  loopMetas.push_back(metadata);
  return id;
}

// ---------------------------------------------------------------- driver
bool irgenProgram(Program* p, const std::string& outLl) {
  G = new IrGen();
  G->prog = p;
  // data stacks (goose port): the pre-irgen analysis (runDstkAnalysis)
  // already assigned every stack_alloc site its index and computed the
  // per-function needs/needsSp and the program total N
  (void)dstk::g_total;

  // Phase 3c (report §10.3): register a DISubprogram for EVERY function
  // up front — functions without branches (straight-line bodies) would
  // otherwise carry no debug metadata at all, and `gdb`/addr2line would
  // show nothing for them.
  for (auto* f : p->fns)
    if (!f->imported && !f->isGeneric)
      G->dbgSubprogId(f);

  // fixed attribute groups (bodies must match emitFooter exactly)
  G->attrGroupIds["nounwind"] = 50;
  G->attrGroupIds["nounwind strictfp"] = 51;
  G->attrGroupIds["nounwind memory(none) willreturn"] = 52;
  G->attrGroupIds["noinline nounwind"] = 60;
  G->attrGroupIds["alwaysinline nounwind"] = 61;

  // string interning (print literals -> @.strN pool)
  G->internStr = [](const std::string& s) -> std::string {
    auto it = G->strPool.find(s);
    if (it != G->strPool.end()) return it->second;
    std::string name = "@.str" + std::to_string(G->strPool.size());
    G->strPool[s] = name;
    return name;
  };

  // --- load profile if given
  if (!g_opts.useProfile.empty()) loadProfile(g_opts.useProfile);

  // --- struct layouts (generic TEMPLATES have unbound field types and
  // are never emitted — only their instantiations, which were laid out
  // at instantiation time, reach the IR)
  for (auto* sd : p->structs) {
    if (sd->isGeneric) continue;
    computeStructLayout(sd);
    G->structTypes[sd->name] = "%struct." + sd->name;
  }

  // --- PGO fn slots (hashes always; counts filled from profile)
  for (auto* f : p->fns) {
    FnProf fp;
    fp.hash = hashName(f->name + ":" + std::to_string(f->loc.line));
    f->profIdx = (int)G->fnProf.size();
    G->fnProf.push_back(fp);
  }
  if (!g_opts.useProfile.empty())
    for (auto* f : p->fns) profileFnLookup(f);

  auto& out = G->out;

  // ---------------- header
  out << "; Freshet (Penstock " << FRESHET_VERSION << ") generated LLVM IR\n";
  out << "source_filename = \"" << p->file << "\"\n";
  // §12: the module triple + data layout come from the resolved target
  // configuration (main.cpp probes the backend clang — its own default
  // triple for native builds — so the strings are exact for the LLVM
  // build in use and match the backend invocation).  The hard-coded
  // x86_64-pc-linux-gnu pair below is only the emergency fallback for a
  // failed probe.
  out << "target datalayout = \"" << (g_dataLayout.empty() ?
         "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
         : g_dataLayout) << "\"\n";
  out << "target triple = \"" << (g_targetTriple.empty() ?
         "x86_64-pc-linux-gnu" : g_targetTriple) << "\"\n\n";

  // ---------------- struct types + runtime range struct
  for (auto* sd : p->structs) {
    if (sd->isGeneric) continue;   // template; instances are emitted below
    out << "%struct." << sd->name << " = type { ";
    for (size_t i = 0; i < sd->fields.size(); ++i) {
      if (i) out << ", ";
      out << G->llvmType(sd->fields[i].ty);
    }
    out << " }\n";
  }
  // ---------------- enum types (Phase 1d, report §4.3)
  // an enum value is { i64 tag, [paySize x i8] payload } with the payload
  // aligned to payAlign (layout in types.cpp::computeEnumLayout)
  for (auto* ed : p->enums) {
    if (ed->isGeneric) continue;   // template; instances carry mangled names
    computeEnumLayout(ed);
    out << "%enum." << ed->name << " = type { i64, [" << ed->paySize
        << " x i8] }\n";
  }
  out << "%frt.range = type { ptr, i64, i64, i64, i32, i32, i32, i32, i64, ptr, i64, i32, i64 }\n"
         "  ; ^8th field i32 @44: Phase 5 @reproducible flag (matches\n"
         "  ;   frt_range_t.repro — 1 = fixed-block deterministic schedule)\n"
         "  ; ^11th field i64 @64: §6.12 per-iteration body-cost class (matches\n"
         "  ;   frt_range_t.body_cost in runtime/frt_rt.c — 0 = legacy IR)\n"
         "  ; ^12th field i32 @72: mem & vectorisation stage memory/compute\n"
         "  ;   census class (matches frt_range_t.mem_class — 0 = unknown,\n"
         "  ;   1 = memory-bound, 2 = compute-bound, 3 = balanced); drives\n"
         "  ;   the runtime's SMT sibling lane routing when\n"
         "  ;   FRT_SMT_INTERLEAVE=1\n"
         "  ; ^13th field i64 @80: the dispatching context's first-free data\n"
         "  ;   stack index (goose port: %sp threaded through frt_parfor).\n"
         "  ;   The runtime copies it into every task and passes it as the\n"
         "  ;   chunk function's hidden 4th argument — each executing thread\n"
         "  ;   addresses the region's stacks at [sp0, sp0+need) of its own\n"
         "  ;   per-thread tops array; 0 for regions that use no stacks\n";
  out << "@__frt_env_threads = external global i32\n";
  // data stacks (goose port): the per-thread array of stack tops.  The
  // size is the program total N — known only after every function is
  // emitted (a callee's stacks sit above its caller's), so the array is
  // emitted with a placeholder that is patched before the module is
  // written (the @@ARGS@@ PGO registration uses the same trick).
  if (dstk::g_total > 0)
    out << "@frt.dstk.tops = thread_local global [@@DSTKN@@ x ptr] "
           "zeroinitializer, align 8\n";
  out << "\n";

  // ---------------- globals
  for (auto* g : p->globals) {
    std::string linkage = g->exported ? "" : "internal";
    std::string init = g->init ? G->constOf(g->ty, g->init) : "0";
    if (g->ty->k == Type::PTR || g->ty->k == Type::FNPTR) init = "null";
    out << "@" << irGlobalSymbol(g) << " = " << linkage << " global "
        << G->llvmType(g->ty) << " " << init << ", align "
        << typeAlignOf(g->ty) << "\n";
  }
  out << "\n";

  // ---------------- runtime decls
  out << RUNTIME_DECLS << "\n";
  // Backend-matrix FP min/max (LLVM 19-23): LLVM 19 spells them
  // llvm.minnum/llvm.maxnum, LLVM 20+ llvm.fmin/llvm.fmax — emit the
  // resolved backend's spelling (g_backendLlvmMajor, common.h).
  out << "declare double @" << llvmFminBase() << ".f64(double, double)\n"
      << "declare double @" << llvmFmaxBase() << ".f64(double, double)\n"
      << "declare float @" << llvmFminBase() << ".f32(float, float)\n"
      << "declare float @" << llvmFmaxBase() << ".f32(float, float)\n";

  // ---------------- functions and imported C prototypes
  for (auto* f : p->fns) {
    if (f->imported) {
      out << "declare " << G->llvmType(f->ret) << " @" << f->name << "(";
      for (size_t i = 0; i < f->params.size(); ++i) {
        if (i) out << ", ";
        Type* pt = f->params[i].ty;
        if (pt && (pt->k == Type::STRUCT || pt->k == Type::SOA ||
                   pt->k == Type::ENUM || pt->k == Type::ARRAY)) out << "ptr";
        else out << G->llvmType(pt);
      }
      out << ")\n";
      continue;
    }
    if (f->isGeneric) continue;   // Phase 2a: template, instances only
    FnSema* fs = semaForFn(f);
    if (!fs) continue;
    G->emitFunction(f, fs);
  }

  // ---------------- parfor chunk fns
  for (auto& c : G->chunkFns) out << c;

  // ---------------- profile data globals + registration patch
  std::string body = out.str();
  // data stacks (goose port): patch the placeholder stack count now that
  // every function has been emitted (globals, GEPs and the lazy-init
  // call all reference it)
  if (dstk::g_total > 0) {
    const std::string ph = "@@DSTKN@@";
    const std::string n = std::to_string(dstk::g_total);
    size_t pos;
    while ((pos = body.find(ph)) != std::string::npos)
      body.replace(pos, ph.size(), n);
  }
  if (g_opts.profile) {
    out.str("");
    out.clear();
    G->emitProfileData();
    body += out.str();
    // patch the registration call in main
    std::string args = "(i32 " + std::to_string(G->fnProf.size()) +
        ", ptr @__frt_prof_fnhash, ptr @__frt_prof_fnptrs, i32 " +
        std::to_string(G->brProf.size()) +
        ", ptr @__frt_prof_brhash, ptr @__frt_prof_brptrs)";
    size_t pos = body.find("@frt_prof_register@@ARGS@@()");
    if (pos != std::string::npos)
      body.replace(pos, strlen("@frt_prof_register@@ARGS@@()"),
                   "@frt_prof_register" + args);
  }

  // ---------------- string pool / soa consts / metadata footer
  out.str(body);
  out.clear();
  out.seekp(0, std::ios::end);
  G->emitFooter();

  // write file
  std::ofstream f(outLl);
  if (!f.is_open()) {
    diag(Diag::Error, Code::Io, "cannot write IR file: " + outLl, p->file, 0, 0);
    return false;
  }
  f << G->out.str();
  f.close();
  return errorCount() == 0;
}

// ---------------------------------------------------------------- profile data
void IrGen::emitProfileData() {
  if (fnProf.empty() && brProf.empty()) return;
  if (!fnProf.empty()) {
    for (size_t i = 0; i < fnProf.size(); ++i)
      out << "@__frt_prof_fn_" << i << " = internal global i64 0, align 8\n";
    out << "@__frt_prof_fnhash = internal constant [" << fnProf.size()
        << " x i64] [";
    for (size_t i = 0; i < fnProf.size(); ++i) {
      if (i) out << ", ";
      out << "i64 " << fnProf[i].hash;
    }
    out << "]\n";
    out << "@__frt_prof_fnptrs = internal constant [" << fnProf.size()
        << " x ptr] [";
    for (size_t i = 0; i < fnProf.size(); ++i) {
      if (i) out << ", ";
      out << "ptr @__frt_prof_fn_" << i;
    }
    out << "]\n";
  }
  // branch profile arrays: emit even when empty — the registration call
  // references these unconditionally (even in programs with no if/while)
  {
    if (brProf.empty()) {
      out << "@__frt_prof_brhash = internal constant [0 x i64] zeroinitializer\n";
      out << "@__frt_prof_brptrs = internal constant [0 x ptr] zeroinitializer\n";
    } else {
    int nb = (int)brProf.size() * 2;
    for (int i = 0; i < nb; ++i)
      out << "@__frt_prof_br_" << i << " = internal global i64 0, align 8\n";
    out << "@__frt_prof_brhash = internal constant [" << nb << " x i64] [";
    for (int i = 0; i < nb; ++i) {
      if (i) out << ", ";
      out << "i64 " << (brProf[i / 2].hash ^ (i % 2 ? 0x5A5A5A5A5A5A5A5Aull : 0));
    }
    out << "]\n";
    out << "@__frt_prof_brptrs = internal constant [" << nb << " x ptr] [";
    for (int i = 0; i < nb; ++i) {
      if (i) out << ", ";
      out << "ptr @__frt_prof_br_" << i;
    }
    out << "]\n";
    }
  }
  out << "\n";
}

// ---------------------------------------------------------------- footer
void IrGen::emitFooter() {
  out << "\n; ---------------- constants\n";
  // string pool
  for (auto& [s, name] : strPool) {
    out << name << " = private unnamed_addr constant ["
        << (s.size() + 1) << " x i8] c\"" << esc(s) << "\\00\"\n";
  }
  // soa plane-size constants
  for (auto& [sn, def] : soaSizeConsts) {
    out << def << "\n";
  }
  out << "\n; ---------------- attributes\n";
  // audit §2.3: attribute groups are derived from the sema effect
  // summaries (effects.cpp).  Each group claims exactly what the body
  // proved — never more:
  //   #50  ordinary internal fn   — nounwind
  //   #51  strict-FP fn          — nounwind strictfp
  //   #52  read-none + proven-returning — nounwind memory(none) willreturn
  //   #53  exported ABI boundary — nounwind
  //   #60  explicit @noinline     — noinline nounwind
  //   #61  explicit @inline      — alwaysinline nounwind
  // Effect-specific combinations (strictfp x effects, inline x effects,
  // memory(argmem: read), memory(none) without a termination proof, ...)
  // are allocated dynamically from #62 upward.
  //
  // Historical fix note: the pre-audit #52 was stamped on every
  // inferred-pure function even when the body only PROVED "no writes",
  // and @pure declarations bypassed validation entirely — the audit's
  // §2.2 reproducer (`fn read_first(p){return p[0];}` called twice
  // around a store) printed `00` instead of `12` at -O3.
  out << "attributes #50 = { nounwind }\n";
  out << "attributes #51 = { nounwind strictfp }\n";
  out << "attributes #52 = { nounwind memory(none) willreturn }\n";
  out << "attributes #53 = { nounwind }\n";
  out << "attributes #60 = { noinline nounwind }\n";
  out << "attributes #61 = { alwaysinline nounwind }\n";
  for (auto& [body, id] : dynAttrGroups)
    out << "attributes #" << id << " = { " << body << " }\n";

  // metadata
  out << "\n; ---------------- metadata\n";
  for (size_t i = 0; i < profMetas.size(); ++i) {
    out << "!" << (100 + i) << " = " << profMetas[i] << "\n";
  }
  for (size_t i = 0; i < loopMetas.size(); ++i) {
    out << "!" << (200 + i) << " = " << loopMetas[i] << "\n";
  }
  for (size_t i = 0; i < dbgNodes.size(); ++i) {
    out << "!" << (300 + i) << " = " << dbgNodes[i] << "\n";
  }
  // compile unit + subprograms (DWARF line tables)
  // Phase 3c: subprograms + per-module DIFiles are numbered dynamically
  // (902+ / 1902+) — every function in every module gets one, including
  // generic instances and the synthesised test harness
  out << "!900 = distinct !DICompileUnit(language: DW_LANG_C99, file: !901, "
         "producer: \"Penstock " << FRESHET_VERSION << "\", isOptimized: true, "
         "emissionKind: FullDebug)\n";
  out << "!901 = !DIFile(filename: \"" << prog->file << "\", directory: \".\")\n";
  for (auto& [f, id] : dbgSubprogs) {
    int fileId = f->loc.file.empty() ? 901 : dbgFileId(f->loc.file);
    int line = std::max(1, f->loc.line);
    out << "!" << id << " = distinct !DISubprogram(name: \""
        << irFnSymbol(f) << "\", scope: !" << fileId << ", file: !"
        << fileId << ", line: " << line << ", type: !2950, scopeLine: "
        << line << ", unit: !900, spFlags: DISPFlagDefinition)\n";
  }
  // DIFiles are emitted AFTER the subprogram loop: dbgFileId() allocates
  // ids on demand while subprograms are written
  for (auto& [path, id] : dbgFiles)
    out << "!" << id << " = !DIFile(filename: \"" << path
        << "\", directory: \".\")\n";
  // FLUME PATCH (upstream bug): the fixed subroutine-type and module-flag
  // nodes used to sit at !940/!941/!950/!951 — INSIDE the dynamic DISubprogram
  // range that starts at !902, so any program with >= 39 functions emitted
  // duplicate metadata ids and clang rejected the module.  They now live
  // above the DIFile range (files: 1902..2899, guard 2900).
  out << "!2950 = !DISubroutineType(types: !2951)\n";
  out << "!2951 = !{null}\n";
  out << "!2955 = !{i32 7, !\"Dwarf Version\", i32 4}\n";
  out << "!2956 = !{i32 2, !\"Debug Info Version\", i32 3}\n";
  out << "!llvm.dbg.cu = !{!900}\n";
  out << "!llvm.module.flags = !{!2955, !2956}\n";
  out << "\n";
}

} // namespace freshet
