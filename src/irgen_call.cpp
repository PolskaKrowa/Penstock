// Freshet IRgen — call lowering: user fns, builtins, math, intrinsics,
// soa_new/soa_free, profiling register.
#include "irgen.h"

namespace freshet {

extern IrGen* G;

static std::string genArg(Expr* a, Type* paramTy) {
  std::string v = genExpr(a);
  // bool parameters use the i8 C ABI at the call boundary while bool
  // VALUES are i1 registers (comparisons, literals) — widen here
  if (paramTy && paramTy->k == Type::BOOL && a->ty &&
      a->ty->k == Type::BOOL) {
    std::string w = G->t();
    G->out << "  " << w << " = zext i1 " << v << " to i8\n";
    return w;
  }
  if (paramTy && a->ty && !typesEquivalent(a->ty, paramTy)) {
    if ((a->ty->isNum() && paramTy->isNum()) ||
        (a->ty->k == Type::PTR && paramTy->k == Type::PTR))
      v = genCastInline(a, paramTy, v);
  }
  return v;
}

std::string genCall(Expr* e) {
  const std::string& n = e->name;

  // Phase 0d follow-on: sizeof/alignof fold to constants in sema
  if (n == "sizeof" || n == "alignof") {
    std::string r = G->t();
    G->out << "  " << r << " = add i64 0, " << e->ival << "  ; " << n
           << " folded\n";
    return r;
  }

  // ---------------- intrinsics
  if (n == "freshet.prefetch_r" || n == "freshet.prefetch_w") {
    std::string p = genExpr(e->args[0]);
    G->out << "  call void @llvm.prefetch.p0(ptr " << p << ", i32 "
           << (n.back() == 'r' ? 0 : 1) << ", i32 3, i32 1)\n";
    return "";
  }
  if (n == "freshet.clflush") {
    std::string p = genExpr(e->args[0]);
    G->out << "  call void @frt_clflush(ptr " << p << ")\n";
    return "";
  }
  if (n == "freshet.cycles") {
    std::string r = G->t();
    G->out << "  " << r << " = call i64 @frt_rdtsc()\n";
    return r;
  }
  if (n == "freshet.time_ns") {
    std::string r = G->t();
    G->out << "  " << r << " = call i64 @frt_time_ns()\n";
    return r;
  }
  if (n == "freshet.time_ms") {
    std::string r = G->t();
    G->out << "  " << r << " = call i64 @frt_time_ms()\n";
    return r;
  }
  if (n == "freshet.pause") {
    G->out << "  call void @frt_pause()\n";
    return "";
  }
  if (n == "freshet.threads") {
    // the runtime global is i32; the language type is usize (i64) —
    // sign-extend so the register type matches the declared type (the
    // historical code returned the raw i32 load, which broke any caller
    // that stored or returned the value)
    std::string r32 = G->t();
    G->out << "  " << r32 << " = load i32, ptr @__frt_env_threads\n";
    std::string r = G->t();
    G->out << "  " << r << " = sext i32 " << r32 << " to i64\n";
    return r;
  }

  // ---------------- memory
  if (n == "malloc") {
    Type* elem = e->mallocElem;
    uint64_t sz = typeSizeOf(elem);
    uint64_t al = std::max<uint64_t>(16, typeAlignOf(elem));
    std::string cnt = genExpr(e->args[1]);
    if (e->args[1]->ty && e->args[1]->ty->bits() < 64)
      cnt = genCastInline(e->args[1], Type::simple(Type::USIZE), cnt);
    std::string bytes = G->t();
    if (sz == 1)
      G->out << "  " << bytes << " = mul i64 " << cnt << ", 1\n";
    else
      G->out << "  " << bytes << " = mul i64 " << cnt << ", " << sz << "\n";
    std::string r = G->t();
    G->out << "  " << r << " = call ptr @frt_malloc(i64 " << bytes << ", i64 "
           << al << ")\n";
    return r;
  }
  if (n == "free") {
    std::string p = genExpr(e->args[0]);
    G->out << "  call void @frt_free(ptr " << p << ")\n";
    return "";
  }
  if (n == "memcpy" || n == "memset") {
    std::string d = genExpr(e->args[0]);
    if (n == "memcpy") {
      std::string s = genExpr(e->args[1]);
      std::string c = genExpr(e->args[2]);
      G->out << "  call void @llvm.memcpy.p0.p0.i64(ptr " << d << ", ptr " << s
             << ", i64 " << c << ", i1 false)\n";
    } else {
      std::string v = genExpr(e->args[1]);
      std::string c = genExpr(e->args[2]);
      G->out << "  call void @llvm.memset.p0.i64(ptr " << d << ", i8 " << v
             << ", i64 " << c << ", i1 false)\n";
    }
    return "";
  }

  // ---------------- soa
  if (n == "soa_new") {
    StructDef* sd = e->mallocElem->sdef;
    int nf = (int)sd->fields.size();
    // sizes global constant
    std::string gname = "@.soasizes." + sd->name;
    // (emitted once at footer — register here)
    if (!G->soaSizeConsts.count(sd->name)) {
      std::string body = gname + " = internal constant [" + std::to_string(nf) +
                         " x i64] [";
      for (size_t i = 0; i < sd->fields.size(); ++i) {
        if (i) body += ", ";
        body += "i64 " + std::to_string(typeSizeOf(sd->fields[i].ty));
      }
      body += "]";
      G->soaSizeConsts[sd->name] = body;
    }
    std::string cnt = genExpr(e->args[1]);
    if (e->args[1]->ty && e->args[1]->ty->bits() < 64)
      cnt = genCastInline(e->args[1], Type::simple(Type::USIZE), cnt);
    std::string r = G->t();
    G->out << "  " << r << " = call ptr @frt_soa_new(i64 " << cnt << ", i32 "
           << nf << ", ptr " << gname << ")\n";
    return r;
  }
  if (n == "soa_free") {
    std::string p = genExpr(e->args[0]);
    G->out << "  call void @frt_soa_free(ptr " << p << ")\n";
    return "";
  }
  if (n == "soa_len") {
    // desc: {i32 nf@0, i64 n@8, ptr planes@16}
    std::string d = genExpr(e->args[0]);
    std::string np = G->t();
    G->out << "  " << np << " = getelementptr i8, ptr " << d << ", i64 8\n";
    std::string r = G->t();
    G->out << "  " << r << " = load i64, ptr " << np << "\n";
    return r;
  }
  if (n == "soa_field_ptr") {
    StructDef* sd = nullptr;
    if (e->args[0]->ty && e->args[0]->ty->k == Type::SOA)
      sd = e->args[0]->ty->sdef;
    if (!sd || e->args[1]->k != Expr::STR) return "";
    std::string d = genExpr(e->args[0]);
    int fidx = -1;
    for (auto& f : sd->fields) if (f.name == e->args[1]->sval) fidx = f.idx;
    if (fidx < 0) return "";
    std::string pslot = G->t();
    G->out << "  " << pslot << " = getelementptr i8, ptr " << d << ", i64 "
           << (16 + 8 * fidx) << "\n";
    std::string r = G->t();
    G->out << "  " << r << " = load ptr, ptr " << pslot << "\n";
    return r;
  }

  // ---------------- I/O
  if (n == "print") {
    // Phase 2b: the argument is a full str VALUE ({ ptr, i64 }) —
    // literals, views and slices all take the same path
    std::string v = genExpr(e->args[0]);
    std::string p = G->t();
    G->out << "  " << p << " = extractvalue { ptr, i64 } " << v << ", 0\n";
    std::string l = G->t();
    G->out << "  " << l << " = extractvalue { ptr, i64 } " << v << ", 1\n";
    G->out << "  call void @frt_print_str(ptr " << p << ", i64 " << l << ")\n";
    return "";
  }
  if (n == "print_f64") {
    std::string v = genExpr(e->args[0]);
    if (e->args[0]->ty && e->args[0]->ty->k == Type::F32)
      v = genCastInline(e->args[0], Type::simple(Type::F64), v);
    std::string dg = genExpr(e->args[1]);
    G->out << "  call void @frt_print_f64(double " << v << ", i64 " << dg
           << ")\n";
    return "";
  }
  if (n == "print_i64" || n == "print_u64") {
    std::string v = genExpr(e->args[0]);
    if (e->args[0]->ty && e->args[0]->ty->bits() < 64)
      v = genCastInline(e->args[0], Type::simple(Type::I64), v);
    std::string callee = (n == "print_u64") ? "frt_print_u64" : "frt_print_i64";
    G->out << "  call void @" << callee << "(i64 " << v << ")\n";
    return "";
  }
  if (n == "println") {
    G->out << "  call void @frt_println()\n";
    return "";
  }
  if (n == "assert") {
    std::string v = genExpr(e->args[0]);
    std::string cnd;
    Type* at = e->args[0]->ty;
    if (at->isFloat()) {
      cnd = G->t();
      G->out << "  " << cnd << " = fcmp " << G->fflag() << " one "
             << G->llvmType(at) << " " << v << ", 0.0\n";
    } else if (at->k == Type::BOOL) {
      cnd = v;
    } else {
      cnd = G->t();
      G->out << "  " << cnd << " = icmp ne " << G->llvmType(at) << " " << v
             << ", 0\n";
    }
    G->out << "  call void @frt_assert(i1 " << cnd << ")\n";
    return "";
  }

  // ---------------- integer min/max (audit §6.7): icmp + select,
  // memory-free, exact for the operand type's signedness
  if ((n == "imin" || n == "imax") && e->args.size() == 2) {
    Type* rt = e->ty && e->ty->isInt() ? e->ty : Type::simple(Type::I64);
    std::string a = genExpr(e->args[0]);
    if (e->args[0]->ty && !typesEquivalent(e->args[0]->ty, rt))
      a = genCastInline(e->args[0], rt, a);
    std::string b = genExpr(e->args[1]);
    if (e->args[1]->ty && !typesEquivalent(e->args[1]->ty, rt))
      b = genCastInline(e->args[1], rt, b);
    std::string ty = G->llvmType(rt);
    bool s = rt->isSigned();
    // the PREDICATE encodes the direction; the select is always
    // (cnd ? a : b):  imin -> cnd = a < b, imax -> cnd = a > b
    const char* pred =
        n == "imin" ? (s ? "slt" : "ult") : (s ? "sgt" : "ugt");
    std::string cnd = G->t();
    G->out << "  " << cnd << " = icmp " << pred << " " << ty << " " << a
           << ", " << b << "\n";
    std::string r = G->t();
    G->out << "  " << r << " = select i1 " << cnd << ", " << ty << " " << a
           << ", " << ty << " " << b << "  ; " << n << "\n";
    return r;
  }

  // ---------------- math via llvm intrinsics / libm
  static const std::map<std::string, std::string> llvmMath = {
    {"sqrt", "llvm.sqrt"}, {"fabs", "llvm.fabs"}, {"floor", "llvm.floor"},
    {"ceil", "llvm.ceil"}, {"round", "llvm.round"}, {"fmin", "llvm.fmin"},
    {"fmax", "llvm.fmax"}, {"pow", "llvm.pow"}, {"exp", "llvm.exp"},
    {"log", "llvm.log"},
  };
  auto it = llvmMath.find(n);
  if (it != llvmMath.end() && e->args.size() >= 1) {
    Type* at = e->args[0]->ty;
    if (at && (at->k == Type::F32 || at->k == Type::F64)) {
      std::string ty = G->llvmType(at);
      std::string a = genExpr(e->args[0]);
      std::string b;
      if (e->args.size() == 2) b = genExpr(e->args[1]);
      std::string r = G->t();
      if (e->args.size() == 2)
        G->out << "  " << r << " = call " << G->fflag() << " " << ty << " @"
               << it->second << "." << (at->k == Type::F32 ? "f32" : "f64")
               << "(" << ty << " " << a << ", " << ty << " " << b << ")\n";
      else
        G->out << "  " << r << " = call " << G->fflag() << " " << ty << " @"
               << it->second << "." << (at->k == Type::F32 ? "f32" : "f64")
               << "(" << ty << " " << a << ")\n";
      return r;
    }
  }
  // ---------------- transcendental math (§8, Option A)
  // sin/cos/log2/log10 lower to LLVM intrinsics ONLY under fast-math
  // (@fastmath on the fn or --fast-math globally) — the language default
  // is strict IEEE-754-like semantics, and libm calls preserve the exact
  // reference behaviour (errno visibility included).  f32 stays f32.  tan
  // has no LLVM intrinsic and keeps the scalar libm call in all modes.
  static const std::map<std::string, std::string> llvmTrans = {
    {"sin", "llvm.sin"}, {"cos", "llvm.cos"},
    {"log2", "llvm.log2"}, {"log10", "llvm.log10"},
  };
  auto tt = llvmTrans.find(n);
  bool fastMathOn = (G->curFn && G->curFn->fastmath) || g_opts.fastMathGlobal;
  if (tt != llvmTrans.end() && e->args.size() == 1 && fastMathOn &&
      !(G->curFn && G->curFn->strictfp)) {
    Type* at = e->args[0]->ty;
    if (at && (at->k == Type::F32 || at->k == Type::F64)) {
      std::string ty = G->llvmType(at);
      std::string a = genExpr(e->args[0]);
      std::string r = G->t();
      G->out << "  " << r << " = call " << G->fflag() << " " << ty << " @"
             << tt->second << "." << (at->k == Type::F32 ? "f32" : "f64")
             << "(" << ty << " " << a << ")\n";
      return r;
    }
  }
  // libm-only fns (double): tan in all modes; sin/cos/log2/log10 under
  // @strictfp only
  static const std::set<std::string> libm1 = {"sin", "cos", "tan", "log2", "log10"};
  if (libm1.count(n) && e->args.size() == 1) {
    std::string a = genExpr(e->args[0]);
    if (e->args[0]->ty && e->args[0]->ty->k == Type::F32) {
      a = genCastInline(e->args[0], Type::simple(Type::F64), a);
      std::string r0 = G->t();
      G->out << "  " << r0 << " = call double @" << n << "(double " << a
             << ")\n";
      std::string r = G->t();
      G->out << "  " << r << " = fptrunc double " << r0 << " to float\n";
      return r;
    }
    std::string r = G->t();
    G->out << "  " << r << " = call double @" << n << "(double " << a << ")\n";
    return r;
  }

  // ---------------- Phase 1c (report §4.1): span construction
  if (n == "span") {
    if (e->args.size() == 2) {
      // span(p, len) -> { ptr, i64 }
      std::string p = genExpr(e->args[0]);
      std::string lv = genExpr(e->args[1]);
      if (e->args[1]->ty && e->args[1]->ty->bits() < 64)
        lv = genCastInline(e->args[1], Type::simple(Type::I64), lv);
      std::string v1 = G->t();
      G->out << "  " << v1 << " = insertvalue { ptr, i64 } poison, ptr " << p
             << ", 0\n";
      std::string v2 = G->t();
      G->out << "  " << v2 << " = insertvalue { ptr, i64 } " << v1 << ", i64 "
             << lv << ", 1\n";
      return v2;
    }
    // span(arr) -> view over the array's storage
    std::string a = genAddr(e->args[0]);
    if (a.empty()) a = genExpr(e->args[0]);
    Type* at = e->args[0]->ty;
    std::string v1 = G->t();
    G->out << "  " << v1 << " = insertvalue { ptr, i64 } poison, ptr " << a
           << ", 0\n";
    std::string v2 = G->t();
    G->out << "  " << v2 << " = insertvalue { ptr, i64 } " << v1 << ", i64 "
           << at->arrLen << ", 1\n";
    return v2;
  }

  // ---------------- Phase 2b (report §4.5): strings
  if (n == "str_eq") {
    std::string va = genExpr(e->args[0]);
    std::string pa = G->t(), la = G->t();
    G->out << "  " << pa << " = extractvalue { ptr, i64 } " << va << ", 0\n";
    G->out << "  " << la << " = extractvalue { ptr, i64 } " << va << ", 1\n";
    std::string vb = genExpr(e->args[1]);
    std::string pb = G->t(), lb = G->t();
    G->out << "  " << pb << " = extractvalue { ptr, i64 } " << vb << ", 0\n";
    G->out << "  " << lb << " = extractvalue { ptr, i64 } " << vb << ", 1\n";
    std::string r32 = G->t();
    G->out << "  " << r32 << " = call i32 @frt_str_eq(ptr " << pa << ", i64 "
           << la << ", ptr " << pb << ", i64 " << lb << ")\n";
    std::string r = G->t();
    G->out << "  " << r << " = icmp ne i32 " << r32 << ", 0\n";
    return r;
  }
  if (n == "str_to_cstr") {
    std::string v = genExpr(e->args[0]);
    std::string p = G->t(), l = G->t();
    G->out << "  " << p << " = extractvalue { ptr, i64 } " << v << ", 0\n";
    G->out << "  " << l << " = extractvalue { ptr, i64 } " << v << ", 1\n";
    std::string r = G->t();
    G->out << "  " << r << " = call ptr @frt_str_to_cstr(ptr " << p << ", i64 "
           << l << ")\n";
    return r;
  }
  // String constructors: out-param runtime calls; value = the temp's
  // address (String is a struct: value = address)
  if (n == "str_cat" || n == "string_new" || n == "string_from" ||
      n == "fmt_f64" || n == "fmt_i64") {
    StructDef* sd = ensurePreludeString(nullptr);
    std::string out = G->t();
    G->out << "  " << out << " = alloca %struct.String, align 8\n";
    if (n == "str_cat") {
      std::string va = genExpr(e->args[0]);
      std::string pa = G->t(), la = G->t();
      G->out << "  " << pa << " = extractvalue { ptr, i64 } " << va << ", 0\n";
      G->out << "  " << la << " = extractvalue { ptr, i64 } " << va << ", 1\n";
      std::string vb = genExpr(e->args[1]);
      std::string pb = G->t(), lb = G->t();
      G->out << "  " << pb << " = extractvalue { ptr, i64 } " << vb << ", 0\n";
      G->out << "  " << lb << " = extractvalue { ptr, i64 } " << vb << ", 1\n";
      G->out << "  call void @frt_str_cat(ptr " << out << ", ptr " << pa
             << ", i64 " << la << ", ptr " << pb << ", i64 " << lb << ")\n";
    } else if (n == "string_new") {
      G->out << "  call void @frt_string_init(ptr " << out << ")\n";
    } else if (n == "string_from") {
      std::string v = genExpr(e->args[0]);
      std::string p = G->t(), l = G->t();
      G->out << "  " << p << " = extractvalue { ptr, i64 } " << v << ", 0\n";
      G->out << "  " << l << " = extractvalue { ptr, i64 } " << v << ", 1\n";
      G->out << "  call void @frt_string_from(ptr " << out << ", ptr " << p
             << ", i64 " << l << ")\n";
    } else if (n == "fmt_f64") {
      std::string v = genExpr(e->args[0]);
      if (e->args[0]->ty && e->args[0]->ty->k == Type::F32)
        v = genCastInline(e->args[0], Type::simple(Type::F64), v);
      std::string dg = genExpr(e->args[1]);
      if (e->args[1]->ty && e->args[1]->ty->bits() < 64)
        dg = genCastInline(e->args[1], Type::simple(Type::I64), dg);
      G->out << "  call void @frt_fmt_f64(ptr " << out << ", double " << v
             << ", i64 " << dg << ")\n";
    } else {   // fmt_i64
      std::string v = genExpr(e->args[0]);
      if (e->args[0]->ty && e->args[0]->ty->bits() < 64)
        v = genCastInline(e->args[0], Type::simple(Type::I64), v);
      G->out << "  call void @frt_fmt_i64(ptr " << out << ", i64 " << v
             << ")\n";
    }
    (void)sd;
    return out;
  }
  if (n == "string_append" || n == "string_free" || n == "str_of") {
    // first arg: *String
    std::string sp = genExpr(e->args[0]);
    if (n == "string_append") {
      std::string v = genExpr(e->args[1]);
      std::string p = G->t(), l = G->t();
      G->out << "  " << p << " = extractvalue { ptr, i64 } " << v << ", 0\n";
      G->out << "  " << l << " = extractvalue { ptr, i64 } " << v << ", 1\n";
      G->out << "  call void @frt_string_append(ptr " << sp << ", ptr " << p
             << ", i64 " << l << ")\n";
      return "";
    }
    if (n == "string_free") {
      G->out << "  call void @frt_string_free(ptr " << sp << ")\n";
      return "";
    }
    // str_of(&s) -> str view over { s.data, s.len }
    std::string dp = G->t();
    G->out << "  " << dp << " = getelementptr inbounds %struct.String, ptr "
           << sp << ", i32 0, i32 0\n";
    std::string data = G->t();
    G->out << "  " << data << " = load ptr, ptr " << dp << "\n";
    std::string lp = G->t();
    G->out << "  " << lp << " = getelementptr inbounds %struct.String, ptr "
           << sp << ", i32 0, i32 1\n";
    std::string len = G->t();
    G->out << "  " << len << " = load i64, ptr " << lp << "\n";
    std::string v1 = G->t();
    G->out << "  " << v1 << " = insertvalue { ptr, i64 } poison, ptr " << data
           << ", 0\n";
    std::string v2 = G->t();
    G->out << "  " << v2 << " = insertvalue { ptr, i64 } " << v1 << ", i64 "
           << len << ", 1\n";
    return v2;
  }

  // ---------------- user functions
  FnDecl* f = e->callee;
  // Phase 3b (report §6.3): indirect call through a fn-pointer LOCAL —
  // the callee is a VALUE loaded from its slot
  if (!f && e->fnPtrSym) {
    Type* fpty = e->fnPtrSym->ty;
    if (fpty && fpty->isFnPtr()) {
      std::string fpAddr = G->varAllocas[e->fnPtrSym->name];
      std::string fp = G->t();
      G->out << "  " << fp << " = load ptr, ptr " << fpAddr << "\n";
      std::vector<std::string> argRegs;
      for (size_t i = 0; i < e->args.size(); ++i) {
        Type* pt = fpty->fnParams[i];
        std::string v;
        if (pt && (pt->k == Type::STRUCT || pt->k == Type::ENUM ||
                   pt->k == Type::ARRAY || pt->k == Type::SOA)) {
          v = genExpr(e->args[i]);   // aggregate value = address
        } else if (pt && pt->isPair()) {
          v = genArg(e->args[i], pt);
        } else {
          v = genArg(e->args[i], pt);
        }
        argRegs.push_back(v);
      }
      std::string r;
      if (fpty->pointee->k != Type::VOID) r = G->t();
      std::string dbgAttr = G->dbg(e->loc.line, e->loc.col);
      G->out << "  ";
      if (!r.empty()) G->out << r << " = ";
      G->out << "call " << G->llvmType(fpty->pointee) << " " << fp << "(";
      for (size_t i = 0; i < argRegs.size(); ++i) {
        if (i) G->out << ", ";
        Type* pt = fpty->fnParams[i];
        if (pt && (pt->k == Type::STRUCT || pt->k == Type::SOA ||
                   pt->k == Type::ENUM || pt->k == Type::ARRAY))
          G->out << "ptr " << argRegs[i];
        else
          G->out << G->llvmType(pt) << " " << argRegs[i];
      }
      G->out << ")" << dbgAttr << "\n";
      if (!r.empty() && fpty->pointee->k == Type::BOOL) {
        std::string n = G->t();
        G->out << "  " << n << " = trunc i8 " << r << " to i1\n";
        r = n;
      }
      if (!r.empty() && (fpty->pointee->k == Type::STRUCT ||
                         fpty->pointee->k == Type::ENUM ||
                         fpty->pointee->k == Type::ARRAY) &&
          !fpty->pointee->isPair()) {
        std::string slot = G->t();
        G->out << "  " << slot << " = alloca " << G->llvmType(fpty->pointee)
               << "\n";
        G->out << "  store " << G->llvmType(fpty->pointee) << " " << r
               << ", ptr " << slot << "\n";
        return slot;
      }
      return r;
    }
  }
  // Phase 3b: indirect call through a fn-pointer GLOBAL
  if (!f && e->globalRef && e->globalRef->ty && e->globalRef->ty->isFnPtr()) {
    Type* fpty = e->globalRef->ty;
    std::string fp = G->t();
    G->out << "  " << fp << " = load ptr, ptr @"
           << irGlobalSymbol(e->globalRef) << "\n";
    std::vector<std::string> argRegs;
    for (size_t i = 0; i < e->args.size(); ++i) {
      Type* pt = fpty->fnParams[i];
      argRegs.push_back(genArg(e->args[i], pt));
    }
    std::string r;
    if (fpty->pointee->k != Type::VOID) r = G->t();
    G->out << "  ";
    if (!r.empty()) G->out << r << " = ";
    G->out << "call " << G->llvmType(fpty->pointee) << " " << fp << "(";
    for (size_t i = 0; i < argRegs.size(); ++i) {
      if (i) G->out << ", ";
      G->out << G->llvmType(fpty->fnParams[i]) << " " << argRegs[i];
    }
    G->out << ")\n";
    if (!r.empty() && fpty->pointee->k == Type::BOOL) {
      std::string n = G->t();
      G->out << "  " << n << " = trunc i8 " << r << " to i1\n";
      r = n;
    }
    return r;
  }
  if (!f) return "";
  std::vector<std::string> argRegs;
  for (size_t i = 0; i < e->args.size(); ++i) {
    Type* pt = f->params[i].ty;
    if (pt && (pt->k == Type::STRUCT || pt->k == Type::ENUM ||
               pt->k == Type::ARRAY)) {
      // by-address aggregate: the callee copies (spec §4)
      std::string v = genExpr(e->args[i]);   // aggregate value = address
      argRegs.push_back(v);
      continue;
    }
    if (pt && pt->k == Type::SOA) {
      std::string v = genExpr(e->args[i]);   // descriptor ptr
      argRegs.push_back(v);
      continue;
    }
    if (pt && pt->isPair()) {
      // Span/str: first-class { ptr, i64 } by value
      std::string v = genArg(e->args[i], pt);
      if (e->args[i]->ty && e->args[i]->ty->isPair() &&
          !typesEquivalent(e->args[i]->ty, pt)) {
        std::string p0 = G->t();
        G->out << "  " << p0 << " = extractvalue { ptr, i64 } " << v << ", 0\n";
        std::string l0 = G->t();
        G->out << "  " << l0 << " = extractvalue { ptr, i64 } " << v << ", 1\n";
        std::string v1 = G->t();
        G->out << "  " << v1 << " = insertvalue { ptr, i64 } poison, ptr "
               << p0 << ", 0\n";
        std::string v2 = G->t();
        G->out << "  " << v2 << " = insertvalue { ptr, i64 } " << v1
               << ", i64 " << l0 << ", 1\n";
        v = v2;
      }
      argRegs.push_back(v);
      continue;
    }
    std::string v = genArg(e->args[i], pt);
    argRegs.push_back(v);
  }
  std::string r;
  bool retPair = f->ret->isPair();
  bool retAddrAgg = f->ret->k == Type::STRUCT || f->ret->k == Type::ENUM ||
                     f->ret->k == Type::ARRAY;
  if (f->ret->k != Type::VOID) r = G->t();
  // audit §2.3 rule 2: the call-site attribute is derived from the same
  // canonical summary (IrGen::fnAttrBody) as the callee's definition,
  // so call site and definition can never disagree.  The historical
  // code stamped #52 (memory(none) willreturn) on every inferred-pure
  // callee — wrong for read-only functions and the root cause of the
  // audit's §2.2 wrong-code reproducer.
  // Imported C prototypes are ABI boundaries. Do not infer purity, memory,
  // or termination attributes when no body is present in this module.
  std::string attrs;
  std::string fname;
  if (f->imported) {
    attrs = "";
    fname = f->name;
  } else {
    attrs = " #" + std::to_string(G->attrGroupFor(f));
    // Phase 3a: collision-aware IR symbol (see emitFunction)
    fname = irFnSymbol(f);
  }
  std::string dbgAttr = G->dbg(e->loc.line, e->loc.col);
  G->out << "  ";
  if (!r.empty()) G->out << r << " = ";
  G->out << "call " << G->llvmType(f->ret) << " @" << fname << "(";
  for (size_t i = 0; i < argRegs.size(); ++i) {
    if (i) G->out << ", ";
    Type* pt = f->params[i].ty;
    if (pt && (pt->k == Type::STRUCT || pt->k == Type::SOA ||
               pt->k == Type::ENUM || pt->k == Type::ARRAY))
      G->out << "ptr " << argRegs[i];   // by-address; callee copies
    else
      G->out << G->llvmType(pt) << " " << argRegs[i];
  }
  G->out << ")" << attrs << dbgAttr << "\n";
  // bool RETURN ABI (mirror of genArg's parameter widening): the callee
  // returns the i8 C ABI; bool VALUES in this function are i1 registers,
  // so narrow the call result before it feeds conditions / bool slots.
  if (!r.empty() && f->ret->k == Type::BOOL) {
    std::string n = G->t();
    G->out << "  " << n << " = trunc i8 " << r << " to i1\n";
    r = n;
  }
  // aggregate-by-address returns: land the value in a temp slot and hand
  // out the slot's address (the STRUCT/ENUM/ARRAY value convention)
  if (!r.empty() && retAddrAgg && !retPair) {
    std::string slot = G->t();
    G->out << "  " << slot << " = alloca " << G->llvmType(f->ret) << "\n";
    G->out << "  store " << G->llvmType(f->ret) << " " << r << ", ptr "
           << slot << "\n";
    return slot;
  }
  return r;
}

} // namespace freshet
