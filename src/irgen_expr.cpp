// Freshet IRgen — expressions.
#include "irgen.h"
#include <algorithm>

namespace freshet {

extern IrGen* G;

static std::string zeroOf(Type* t) {
  switch (t->k) {
    case Type::F32: case Type::F64: return "0.0";
    case Type::BOOL: return "false";
    default: return "0";
  }
}

static int intCastOp(Type* from, Type* to) {
  // returns: 0 = trunc, 1 = zext, 2 = sext
  if (to->bits() < from->bits()) return 0;
  if (to->bits() == from->bits()) return from->isSigned() ? 2 : 1;
  return from->isSigned() ? 2 : 1;
}

// ------------------------------------------------- shared arithmetic emitter
// Used by both genExpr(BIN) and the compound-assignment lowering so the
// two paths can never disagree about signedness, strength reduction or
// shift semantics (report §3.1/§3.4 follow-on: the historical compound
// path emitted `div`/`rem` — not even valid LLVM — and truncated `<<=`
// to a bitwise op).
//
// IMPORTANT: the result temp is allocated FIRST and, for multi-instruction
// sequences (signed div/rem strength reduction), consumed as the FIRST
// intermediate so penstock's explicit %N register numbering stays strictly
// increasing in the instruction stream.
std::string emitArithOp(const std::string& op, Type* dt, const std::string& a,
                        const std::string& b, Expr* rhsExpr, SrcLoc loc) {
  std::string r = G->t();
  std::string ty = G->llvmType(dt);
  if (dt->isFloat()) {
    const std::string flag = G->fflag();
    if (op == "+") G->out << "  " << r << " = fadd " << flag << " " << ty << " " << a << ", " << b << "\n";
    else if (op == "-") G->out << "  " << r << " = fsub " << flag << " " << ty << " " << a << ", " << b << "\n";
    else if (op == "*") G->out << "  " << r << " = fmul " << flag << " " << ty << " " << a << ", " << b << "\n";
    else if (op == "/") G->out << "  " << r << " = fdiv " << flag << " " << ty << " " << a << ", " << b << "\n";
    else diag(Diag::Error, Code::Ir, "bad float op " + op,
              loc.file, loc.line, loc.col);
    return r;
  }
  if (op == "+") G->out << "  " << r << " = add " << ty << " " << a << ", " << b << "\n";
  else if (op == "-") G->out << "  " << r << " = sub " << ty << " " << a << ", " << b << "\n";
  else if (op == "*") G->out << "  " << r << " = mul " << ty << " " << a << ", " << b << "\n";
  else if (op == "/") {
    if (rhsExpr && rhsExpr->constant && rhsExpr->k == Expr::INT) {
      uint64_t d = rhsExpr->ival;
      if ((d & (d - 1)) == 0 && d > 0) {
        int sh = 0;
        while ((1ull << sh) < d) ++sh;
        if (dt->isSigned()) {
          int W = dt->bits();
          std::string sign = r;
          G->out << "  " << sign << " = ashr " << ty << " " << a
                 << ", " << (W - 1) << "\n";
          std::string bias = G->t();
          G->out << "  " << bias << " = and " << ty << " " << sign
                 << ", " << (d - 1) << "\n";
          std::string biased = G->t();
          G->out << "  " << biased << " = add " << ty << " " << a
                 << ", " << bias << "\n";
          std::string res = G->t();
          G->out << "  " << res << " = ashr " << ty << " " << biased
                 << ", " << sh << "  ; strength-reduced x/" << d
                 << " (signed, truncating)\n";
          return res;
        }
        G->out << "  " << r << " = lshr " << ty << " " << a << ", " << sh
               << "  ; strength-reduced x/" << d << "\n";
        return r;
      }
    }
    G->out << "  " << r << " = " << (dt->isSigned() ? "sdiv " : "udiv ")
           << ty << " " << a << ", " << b << "\n";
  }
  else if (op == "%") {
    if (rhsExpr && rhsExpr->constant && rhsExpr->k == Expr::INT) {
      uint64_t d = rhsExpr->ival;
      if (d == 1) {
        G->out << "  " << r << " = sub " << ty << " " << a << ", " << a
               << "  ; x % 1\n";
        return r;
      }
      if ((d & (d - 1)) == 0 && d > 0) {
        if (dt->isSigned()) {
          int W = dt->bits();
          int sh = 0;
          while ((1ull << sh) < d) ++sh;
          std::string sign = r;
          G->out << "  " << sign << " = ashr " << ty << " " << a
                 << ", " << (W - 1) << "\n";
          std::string bias = G->t();
          G->out << "  " << bias << " = and " << ty << " " << sign
                 << ", " << (d - 1) << "\n";
          std::string biased = G->t();
          G->out << "  " << biased << " = add " << ty << " " << a
                 << ", " << bias << "\n";
          std::string q = G->t();
          G->out << "  " << q << " = ashr " << ty << " " << biased
                 << ", " << sh << "\n";
          std::string prod = G->t();
          G->out << "  " << prod << " = shl " << ty << " " << q << ", "
                 << sh << "\n";
          std::string res = G->t();
          G->out << "  " << res << " = sub " << ty << " " << a << ", "
                 << prod << "  ; strength-reduced x%" << d << " (signed)\n";
          return res;
        }
        G->out << "  " << r << " = and " << ty << " " << a << ", " << (d - 1)
               << "  ; strength-reduced x%" << d << "\n";
        return r;
      }
    }
    G->out << "  " << r << " = " << (dt->isSigned() ? "srem " : "urem ")
           << ty << " " << a << ", " << b << "\n";
  }
  else if (op == "&") G->out << "  " << r << " = and " << ty << " " << a << ", " << b << "\n";
  else if (op == "|") G->out << "  " << r << " = or " << ty << " " << a << ", " << b << "\n";
  else if (op == "^") G->out << "  " << r << " = xor " << ty << " " << a << ", " << b << "\n";
  else if (op == "<<") G->out << "  " << r << " = shl " << ty << " " << a << ", " << b << "\n";
  else if (op == ">>")
    G->out << "  " << r << " = " << (dt->isSigned() ? "ashr " : "lshr ")
           << ty << " " << a << ", " << b << "\n";
  else diag(Diag::Error, Code::Ir, "bad int op " + op,
            loc.file, loc.line, loc.col);
  return r;
}

// map a compound-assignment operator to its binary operator
std::string compoundBinaryOp(const std::string& op2) {
  if (op2 == "+=") return "+";
  if (op2 == "-=") return "-";
  if (op2 == "*=") return "*";
  if (op2 == "/=") return "/";
  if (op2 == "%=") return "%";
  if (op2 == "&=") return "&";
  if (op2 == "|=") return "|";
  if (op2 == "^=") return "^";
  if (op2 == "<<=") return "<<";   // Phase 0d (report §3.4)
  if (op2 == ">>=") return ">>";
  return op2;
}

// ------------------------------------------------- lvalue addresses
static void emitBoundsCheck(const std::string& idxV, const std::string& lenV,
                            SrcLoc loc);
std::string genVariantConstruct(Expr* e, std::vector<Expr*>* args);
std::string genAddr(Expr* e) {
  switch (e->k) {
    case Expr::IDENT: {
      if (e->sym) {
        return G->varAllocas[e->name];
      }
      if (e->globalRef) {
        return "@" + irGlobalSymbol(e->globalRef);
      }
      return "";
    }
    case Expr::INDEX: {
      Type* bt = e->base->ty;
      // ---- fixed array [T; N]: base storage is addressable, GEP inbounds
      if (bt && bt->k == Type::ARRAY) {
        std::string base = genAddr(e->base);
        if (base.empty()) base = genExpr(e->base);
        std::string idx = genExpr(e->rhs);
        if (e->rhs->ty && e->rhs->ty->bits() < 64) {
          std::string op = e->rhs->ty->isSigned() ? "sext" : "zext";
          std::string w = G->t();
          G->out << "  " << w << " = " << op << " i" << e->rhs->ty->bits()
                 << " " << idx << " to i64\n";
          idx = w;
        }
        // bounds check (checked builds only; constants are proven safe in
        // sema)
        if (!(e->rhs->constant && e->rhs->k == Expr::INT &&
              e->rhs->ival < bt->arrLen)) {
          std::string lenV = G->t();
          G->out << "  " << lenV << " = add i64 0, " << bt->arrLen << "\n";
          emitBoundsCheck(idx, lenV, e->loc);
        }
        std::string a = G->t();
        G->out << "  " << a << " = getelementptr inbounds "
               << G->llvmType(bt) << ", ptr " << base << ", i64 0, i64 " << idx
               << "\n";
        return a;
      }
      // ---- Span<T>: value is a { ptr, i64 } register; index the data
      // pointer, optionally bounds-checked against the carried length
      if (bt && bt->k == Type::SPAN) {
        std::string val = genExpr(e->base);   // { ptr, i64 }
        std::string data = G->t();
        G->out << "  " << data << " = extractvalue { ptr, i64 } " << val
               << ", 0\n";
        std::string len = G->t();
        G->out << "  " << len << " = extractvalue { ptr, i64 } " << val
               << ", 1\n";
        std::string idx = genExpr(e->rhs);
        if (e->rhs->ty && e->rhs->ty->bits() < 64) {
          std::string op = e->rhs->ty->isSigned() ? "sext" : "zext";
          std::string w = G->t();
          G->out << "  " << w << " = " << op << " i" << e->rhs->ty->bits()
                 << " " << idx << " to i64\n";
          idx = w;
        }
        emitBoundsCheck(idx, len, e->loc);
        std::string a = G->t();
        G->out << "  " << a << " = getelementptr inbounds "
               << G->llvmType(bt->pointee) << ", ptr " << data << ", i64 "
               << idx << "\n";
        return a;
      }
      // ---- str: same as Span but the element is i8 and immutable
      if (bt && bt->k == Type::STR) {
        std::string val = genExpr(e->base);
        std::string data = G->t();
        G->out << "  " << data << " = extractvalue { ptr, i64 } " << val
               << ", 0\n";
        std::string len = G->t();
        G->out << "  " << len << " = extractvalue { ptr, i64 } " << val
               << ", 1\n";
        std::string idx = genExpr(e->rhs);
        if (e->rhs->ty && e->rhs->ty->bits() < 64) {
          std::string op = e->rhs->ty->isSigned() ? "sext" : "zext";
          std::string w = G->t();
          G->out << "  " << w << " = " << op << " i" << e->rhs->ty->bits()
                 << " " << idx << " to i64\n";
          idx = w;
        }
        emitBoundsCheck(idx, len, e->loc);
        std::string a = G->t();
        G->out << "  " << a << " = getelementptr inbounds i8, ptr " << data
               << ", i64 " << idx << "\n";
        return a;
      }
      // ---- raw pointer (historical path)
      std::string base;
      if (e->base->ty && e->base->ty->isPtr())
        base = genExpr(e->base);          // loads the pointer
      else {
        base = genAddr(e->base);
        if (base.empty()) base = genExpr(e->base);
      }
      std::string idx = genExpr(e->rhs);
      // index may be narrower than i64
      if (e->rhs->ty && e->rhs->ty->bits() < 64) {
        std::string op = e->rhs->ty->isSigned() ? "sext" : "zext";
        std::string w = G->t();
        G->out << "  " << w << " = " << op << " i" << e->rhs->ty->bits()
               << " " << idx << " to i64\n";
        idx = w;
      }
      Type* elem = e->base->ty ? e->base->ty->pointee : nullptr;
      if (e->soaIndex) return genSoaFieldAddr(e);  // not used: soa handled at FIELD
      std::string a = G->t();
      G->out << "  " << a << " = getelementptr inbounds "
             << G->llvmType(elem) << ", ptr " << base << ", i64 " << idx << "\n";
      return a;
    }
    case Expr::FIELD: {
      // struct field address (AoS or plain struct value)
      if (e->base->ty && e->base->k == Expr::INDEX && e->base->soaIndex) {
        return genSoaFieldAddr(e);
      }
      std::string base;
      if (e->ptrDeref) {
        // p.f — load pointer, then GEP into the struct
        base = genExpr(e->base);
      } else if (e->base->k == Expr::IDENT || e->base->k == Expr::INDEX ||
                 e->base->k == Expr::FIELD) {
        base = genAddr(e->base);
        if (base.empty()) base = genExpr(e->base);
      } else {
        base = genExpr(e->base);
      }
      std::string a = G->t();
      // struct type for the GEP: deref'd pointee or the base's struct
      StructDef* sd = nullptr;
      if (e->ptrDeref && e->base->ty && e->base->ty->pointee)
        sd = e->base->ty->pointee->sdef;
      else if (e->base->ty) {
        Type* bt = e->base->ty;
        if (bt->k == Type::PTR && bt->pointee) bt = bt->pointee;
        if (bt->k == Type::STRUCT) sd = bt->sdef;
      }
      G->out << "  " << a << " = getelementptr inbounds "
             << G->structTypes[sd ? sd->name : std::string("?")]
             << ", ptr " << base
             << ", i32 0, i32 " << e->fieldIdx << "\n";
      return a;
    }
    default:
      return "";
  }
}

// s[i].f — SoA plane address
std::string genSoaFieldAddr(Expr* fld) {
  // fld = (INDEX (IDENT s)) . f — or deeper: s[i].f.g -> plane of g's parent
  Expr* node = fld;
  std::vector<std::string> path;   // field path below the soa elem
  while (node->k == Expr::FIELD) {
    path.push_back(node->name);
    node = node->base;
  }
  // node is INDEX with soaIndex
  Expr* idxExpr = node;
  Expr* soaVar = node->base;
  std::string descPtr;
  if (soaVar->k == Expr::IDENT) {
    descPtr = G->varAllocas[soaVar->name];   // alloca holding descriptor ptr
    if (descPtr.empty()) descPtr = genExpr(soaVar);
  } else {
    descPtr = genExpr(soaVar);
  }
  // resolve top field index
  std::reverse(path.begin(), path.end());
  StructDef* sd = soaVar->ty->sdef;
  StructDef* owner = sd;
  int fieldIdx = -1;
  Type* fieldTy = nullptr;
  for (size_t i = 0; i < path.size(); ++i) {
    Field* f = nullptr;
    for (auto& fd : owner->fields) if (fd.name == path[i]) f = &fd;
    if (!f) break;
    if (i == 0) { fieldIdx = f->idx; fieldTy = f->ty; }
    else owner = fieldTy->sdef;
  }
  if (fieldIdx < 0) return "";

  // load descriptor
  std::string d = G->t();
  G->out << "  " << d << " = load ptr, ptr " << descPtr << "\n";
  // plane pointer: block layout {i32 nf @0, i64 n @8, ptr planes[] @16}
  std::string pslot = G->t();
  G->out << "  " << pslot << " = getelementptr i8, ptr " << d << ", i64 "
         << (16 + 8 * fieldIdx) << "\n";
  std::string plane = G->t();
  G->out << "  " << plane << " = load ptr, ptr " << pslot << "\n";
  // index
  std::string idx = genExpr(idxExpr->rhs);
  if (idxExpr->rhs->ty && idxExpr->rhs->ty->bits() < 64) {
    std::string op = idxExpr->rhs->ty->isSigned() ? "sext" : "zext";
    std::string w = G->t();
    G->out << "  " << w << " = " << op << " i" << idxExpr->rhs->ty->bits()
           << " " << idx << " to i64\n";
    idx = w;
  }
  uint64_t fsz = typeSizeOf(fieldTy);
  std::string a = G->t();
  if (fsz == 1) {
    G->out << "  " << a << " = getelementptr i8, ptr " << plane << ", i64 "
           << idx << "\n";
  } else {
    G->out << "  " << a << " = getelementptr " << G->llvmType(fieldTy)
           << ", ptr " << plane << ", i64 " << idx << "\n";
  }
  // sub-field path (nested struct inside soa element)
  for (size_t i = 1; i < path.size(); ++i) {
    StructDef* sub = fieldTy->sdef;
    Field* f = nullptr;
    for (auto& fd : sub->fields) if (fd.name == path[i]) f = &fd;
    if (!f) break;
    std::string a2 = G->t();
    G->out << "  " << a2 << " = getelementptr inbounds "
           << G->structTypes[sub->name] << ", ptr " << a << ", i32 0, i32 "
           << f->idx << "\n";
    a = a2;
    fieldTy = f->ty;
  }
  return a;
}

// ------------------------------------------------- enum variant construction
// Phase 1d (report §4.3): build { i64 tag, payload } in a fresh alloca.
// args may be null (payload-less variant). Value = alloca address.
std::string genVariantConstruct(Expr* e, std::vector<Expr*>* args) {
  EnumDef* ed = e->enumRef;
  EnumVariant& v = ed->variants[e->variantIdx];
  std::string a = G->t();
  G->out << "  " << a << " = alloca %enum." << ed->name << ", align "
         << ed->align << "\n";
  G->out << "  store i64 " << v.idx << ", ptr " << a << "\n";
  for (size_t i = 0; i < v.fields.size(); ++i) {
    if (!args || i >= args->size()) break;
    Expr* arg = (*args)[i];
    std::string fp = G->t();
    G->out << "  " << fp << " = getelementptr i8, ptr " << a << ", i64 "
           << (8 + v.offsets[i]) << "\n";
    Type* ft = v.fields[i].ty;
    if (ft->k == Type::STRUCT || ft->k == Type::ENUM || ft->k == Type::ARRAY) {
      std::string src = genExpr(arg);   // address
      G->out << "  call void @llvm.memcpy.p0.p0.i64(ptr " << fp << ", ptr "
             << src << ", i64 " << typeSizeOf(ft) << ", i1 false)\n";
    } else if (ft->isPair()) {
      std::string val = genExpr(arg);   // { ptr, i64 } register
      G->out << "  store { ptr, i64 } " << val << ", ptr " << fp << "\n";
    } else {
      std::string val = genExpr(arg);
      if (arg->ty && !typesEquivalent(arg->ty, ft) && arg->ty->isNum() &&
          ft->isNum())
        val = genCastInline(arg, ft, val);
      G->out << "  store " << G->llvmType(ft) << " " << val << ", ptr " << fp
             << "\n";
    }
  }
  return a;
}

// ------------------------------------------------- bounds check helper
// Phase 1c (report §4.1): `--checked` (or -O0/-O1) bounds-checks Span and
// fixed-array indexing. Provable constants are never checked.
bool boundsChecksEnabled() {
  if (g_opts.noBoundsCheck) return false;
  if (g_opts.checked) return true;
  return g_opts.optLevel <= 1;
}

// emit an optional bounds check for index `idxV` (i64) against `lenV`
// (i64); traps (abort) when violated. Layout:
//   br i1 ok, label %cont, label %trap
// trap:  call @frt_bound_trap; unreachable
// cont:  ...
static void emitBoundsCheck(const std::string& idxV, const std::string& lenV,
                            SrcLoc loc) {
  if (!boundsChecksEnabled()) return;
  (void)loc;
  std::string ok = G->t();
  int cont = G->bb++;
  int trap = G->bb++;
  G->out << "  " << ok << " = icmp ult i64 " << idxV << ", " << lenV << "\n";
  G->out << "  br i1 " << ok << ", label %" << G->l(cont)
         << ", label %" << G->l(trap) << "\n";
  G->terminate();
  G->out << G->l(trap) << ":\n";
  G->curLabelTerminated = false;
  G->out << "  call void @frt_bound_trap(i64 " << idxV << ", i64 " << lenV
         << ")\n";
  G->out << "  unreachable\n";
  G->terminate();
  G->out << G->l(cont) << ":\n";
  G->curLabelTerminated = false;
}

// ------------------------------------------------- rvalues
std::string genExpr(Expr* e) {
  switch (e->k) {
    case Expr::INT: {
      std::string r = G->t();
      Type* t = e->ty;
      if (t->k == Type::F64) {
        G->out << "  " << r << " = fadd " << G->fflag() << " double 0.0, "
               << G->doubleToHex((double)e->ival) << "\n";
      } else if (t->k == Type::F32) {
        G->out << "  " << r << " = fadd " << G->fflag() << " float 0.0, "
               << G->floatToHex((float)e->ival) << "\n";
      } else {
        G->out << "  " << r << " = add i64 0, " << (int64_t)e->ival << "\n";
      }
      return r;
    }
    case Expr::FLT: {
      std::string r = G->t();
      if (e->ty->k == Type::F32) {
        G->out << "  " << r << " = fadd " << G->fflag() << " float 0.0, "
               << G->floatToHex((float)e->fval) << "\n";
      } else {
        G->out << "  " << r << " = fadd " << G->fflag() << " double 0.0, "
               << G->doubleToHex(e->fval) << "\n";
      }
      return r;
    }
    case Expr::BOOL: {
      std::string r = G->t();
      G->out << "  " << r << " = add i1 0, " << (e->bval ? 1 : 0) << "\n";
      return r;
    }
    case Expr::STR: {
      // Phase 2b (report §4.5): a string literal is a full str VALUE —
      // a { ptr, i64 } pair pointing into the constant pool (NUL-
      // terminated for C interop), not a bare pointer
      std::string gname = G->internStr(e->sval);
      std::string p = G->t();
      G->out << "  " << p << " = getelementptr inbounds ["
             << (e->sval.size() + 1) << " x i8], ptr " << gname
             << ", i64 0, i64 0\n";
      std::string v1 = G->t();
      G->out << "  " << v1 << " = insertvalue { ptr, i64 } poison, ptr "
             << p << ", 0\n";
      std::string v2 = G->t();
      G->out << "  " << v2 << " = insertvalue { ptr, i64 } " << v1
             << ", i64 " << e->sval.size() << ", 1\n";
      return v2;
    }
    case Expr::NILQ: {
      std::string r = G->t();
      G->out << "  " << r << " = alloca i8\n";  // nil = unique empty ptr
      return r;
    }
    case Expr::IDENT: {
      // Phase 1d: enum variant construction without payload (Red, None)
      if (e->enumRef && e->variantIdx >= 0)
        return genVariantConstruct(e, nullptr);
      // Phase 3b (report §6.3): a function referenced as a VALUE — the
      // symbol itself is the pointer (opaque-ptr LLVM: @fn is a ptr)
      if (e->fnValue) return "@" + irFnSymbol(e->fnValue);
      if (e->sym || e->globalRef) {
        // struct/enum/array-typed var: value is its address
        if (e->ty && (e->ty->k == Type::STRUCT || e->ty->k == Type::ENUM ||
                      e->ty->k == Type::ARRAY)) {
          return e->sym ? G->varAllocas[e->name]
                        : ("@" + irGlobalSymbol(e->globalRef));
        }
        if (e->ty && e->ty->k == Type::SOA) {
          // soa value = descriptor ptr (load from var slot)
          std::string addr = e->sym ? G->varAllocas[e->name]
                                    : ("@" + irGlobalSymbol(e->globalRef));
          std::string r = G->t();
          G->out << "  " << r << " = load ptr, ptr " << addr << "\n";
          return r;
        }
        // pair aggregates (Span/str): value is the loaded { ptr, i64 }
        if (e->ty && e->ty->isPair()) {
          std::string addr = e->sym ? G->varAllocas[e->name]
                                    : ("@" + irGlobalSymbol(e->globalRef));
          std::string r = G->t();
          G->out << "  " << r << " = load { ptr, i64 }, ptr " << addr << "\n";
          return r;
        }
        std::string addr = e->sym ? G->varAllocas[e->name]
                                  : ("@" + irGlobalSymbol(e->globalRef));
        std::string r = G->t();
        G->out << "  " << r << " = load " << G->scalarCodegenTy(e->ty)
               << ", ptr " << addr << "\n";
        return r;
      }
      return "";
    }
    case Expr::SLIT: {
      // struct literal -> alloca + field stores; value = address
      StructDef* sd = e->ty->sdef;
      std::string a = G->t();
      G->out << "  " << a << " = alloca " << G->structTypes[sd->name] << "\n";
      for (size_t i = 0; i < e->args.size(); ++i) {
        // find field idx
        Field* f = nullptr;
        for (auto& fd : sd->fields) if (fd.name == e->slitFields[i]) f = &fd;
        if (!f) continue;
        if (f->ty->k == Type::STRUCT) {
          // copy from value
          std::string v = genExpr(e->args[i]);
          std::string fa = G->t();
          G->out << "  " << fa << " = getelementptr inbounds "
                 << G->structTypes[sd->name] << ", ptr " << a << ", i32 0, i32 "
                 << f->idx << "\n";
          G->out << "  call void @llvm.memcpy.p0.p0.i64(ptr " << fa << ", ptr "
                 << v << ", i64 " << typeSizeOf(f->ty) << ", i1 false)\n";
        } else {
          std::string v = genExpr(e->args[i]);
          if (e->args[i]->ty && f->ty && !typesEquivalent(e->args[i]->ty, f->ty)) {
            v = genCastInline(e->args[i], f->ty, v);
          }
          std::string fa = G->t();
          G->out << "  " << fa << " = getelementptr inbounds "
                 << G->structTypes[sd->name] << ", ptr " << a << ", i32 0, i32 "
                 << f->idx << "\n";
          G->out << "  store " << G->llvmType(f->ty) << " " << v << ", ptr "
                 << fa << "\n";
        }
      }
      return a;
    }
    case Expr::INDEX: {
      // aggregate elements (struct/enum/array): the value IS the element's
      // address — callers copy when they need a value
      if (e->ty && (e->ty->k == Type::STRUCT || e->ty->k == Type::ENUM ||
                    e->ty->k == Type::ARRAY)) {
        return genAddr(e);
      }
      if (e->soaIndex) {
        // s[i] as rvalue struct — build temp copy? Only .field chains valid.
        // synthesize: materialize via alloca copy of planes
        return genAddr(e);
      }
      std::string a = genAddr(e);
      std::string r = G->t();
      G->out << "  " << r << " = load " << G->llvmType(e->ty) << ", ptr " << a
             << "\n";
      return r;
    }
    case Expr::FIELD: {
      // Phase 1d: qualified variant construction (Color.Green)
      if (e->enumRef && e->variantIdx >= 0)
        return genVariantConstruct(e, nullptr);
      // Phase 1b/1c/2b: .len / .ptr on arrays, spans and str views
      Type* btF = e->base->ty;
      if (btF && btF->k == Type::ARRAY && e->name == "len") {
        std::string r = G->t();
        G->out << "  " << r << " = add i64 0, " << btF->arrLen
               << "  ; array length is part of the type\n";
        return r;
      }
      if (btF && btF->isPair()) {
        if (e->name == "len") {
          std::string val = genExpr(e->base);
          std::string r = G->t();
          G->out << "  " << r << " = extractvalue { ptr, i64 } " << val
                 << ", 1\n";
          return r;
        }
        if (e->name == "ptr" && btF->k == Type::SPAN) {
          std::string val = genExpr(e->base);
          std::string r = G->t();
          G->out << "  " << r << " = extractvalue { ptr, i64 } " << val
                 << ", 0\n";
          return r;
        }
      }
      // struct field rvalue
      if (e->base->ty && e->base->k == Expr::INDEX && e->base->soaIndex) {
        std::string a = genSoaFieldAddr(e);
        if (e->ty->k == Type::STRUCT) return a;
        std::string r = G->t();
        G->out << "  " << r << " = load " << G->llvmType(e->ty) << ", ptr " << a
               << "\n";
        return r;
      }
      if (e->ty->k == Type::STRUCT || e->ty->k == Type::ENUM ||
          e->ty->k == Type::ARRAY)
        return genAddr(e);
      std::string a = genAddr(e);
      std::string r = G->t();
      G->out << "  " << r << " = load " << G->llvmType(e->ty) << ", ptr " << a
             << "\n";
      return r;
    }
    case Expr::UN: {
      if (e->op == "&") return genAddr(e->rhs);   // address-of lvalue
      std::string v = genExpr(e->rhs);
      if (e->op == "!") {
        std::string r = G->t();
        G->out << "  " << r << " = xor i1 " << v << ", true\n";
        return r;
      }
      if (e->op == "~") {
        std::string r = G->t();
        G->out << "  " << r << " = xor " << G->llvmType(e->ty) << " " << v
               << ", -1\n";
        return r;
      }
      if (e->op == "+") return v;
      // unary -
      std::string r = G->t();
      if (e->ty->isFloat())
        G->out << "  " << r << " = fneg " << G->fflag() << " "
               << G->llvmType(e->ty) << " " << v << "\n";
      else
        G->out << "  " << r << " = sub " << G->llvmType(e->ty) << " 0, " << v
               << "\n";
      return r;
    }
    case Expr::BIN: {
      const std::string& op = e->op;
      if (op == "&&" || op == "||") {
        // short-circuit via alloca bool
        std::string res = G->t();
        G->out << "  " << res << " = alloca i1\n";
        std::string a = genExpr(e->lhs);
        G->out << "  store i1 " << a << ", ptr " << res << "\n";
        int cont = G->bb++;
        int done = G->bb++;
        if (op == "&&") {
          G->out << "  br i1 " << a << ", label %" << G->l(cont)
                 << ", label %" << G->l(done) << "\n";
        } else {
          G->out << "  br i1 " << a << ", label %" << G->l(done)
                 << ", label %" << G->l(cont) << "\n";
        }
        G->out << G->l(cont) << ":\n";
        G->curLabelTerminated = false;
        std::string b = genExpr(e->rhs);
        G->out << "  store i1 " << b << ", ptr " << res << "\n";
        G->out << "  br label %" << G->l(done) << "\n";
        G->out << G->l(done) << ":\n";
        G->curLabelTerminated = false;
        std::string r = G->t();
        G->out << "  " << r << " = load i1, ptr " << res << "\n";
        return r;
      }
      std::string a = genExpr(e->lhs);
      std::string b = genExpr(e->rhs);
      // numeric promotion between lhs/rhs types
      Type* lt = e->lhs->ty, *rt = e->rhs->ty;
      Type* dt = e->ty;   // result type for arithmetic
      if (e->ty->k == Type::BOOL) {
        // comparison
        std::string cmp;
        if (lt->isFloat()) {
          if (op == "==") cmp = "oeq";
          else if (op == "!=") cmp = "une";
          else if (op == "<") cmp = "olt";
          else if (op == "<=") cmp = "ole";
          else if (op == ">") cmp = "ogt";
          else cmp = "oge";
        } else {
          if (op == "==") cmp = "eq";
          else if (op == "!=") cmp = "ne";
          else if (op == "<") cmp = lt->isSigned() ? "slt" : "ult";
          else if (op == "<=") cmp = lt->isSigned() ? "sle" : "ule";
          else if (op == ">") cmp = lt->isSigned() ? "sgt" : "ugt";
          else cmp = lt->isSigned() ? "sge" : "uge";
        }
        // mixed-width comparison (i32 vs i64 etc.): promote BOTH operands
        // to the common type so the icmp/fcmp sees matching widths.
        // INT literals adopt the other operand's integer type first
        // (sema rule): `i32var < 4` compares in i32 signed, not u64.
        Type* cmpTy = lt;
        if (lt->isInt() && rt->isInt()) {
          if (e->rhs->constant && e->rhs->k == Expr::INT) cmpTy = lt;
          else if (e->lhs->constant && e->lhs->k == Expr::INT) cmpTy = rt;
        }
        if (!cmpTy->isPtr() && !rt->isPtr() && cmpTy->isNum() && rt->isNum() &&
            !typesEquivalent(cmpTy, rt)) {
          if (!typesEquivalent(lt, cmpTy)) a = genCastInline(e->lhs, cmpTy, a);
          if (!typesEquivalent(rt, cmpTy)) b = genCastInline(e->rhs, cmpTy, b);
        }
        // predicate is chosen from the FINAL comparison type: a signedness
        // change through promotion (i32 x u64 -> u64) must select ult/slt
        // accordingly
        if (cmpTy->isFloat()) {
          if (op == "==") cmp = "oeq";
          else if (op == "!=") cmp = "une";
          else if (op == "<") cmp = "olt";
          else if (op == "<=") cmp = "ole";
          else if (op == ">") cmp = "ogt";
          else cmp = "oge";
        } else {
          if (op == "==") cmp = "eq";
          else if (op == "!=") cmp = "ne";
          else if (op == "<") cmp = cmpTy->isSigned() ? "slt" : "ult";
          else if (op == "<=") cmp = cmpTy->isSigned() ? "sle" : "ule";
          else if (op == ">") cmp = cmpTy->isSigned() ? "sgt" : "ugt";
          else cmp = cmpTy->isSigned() ? "sge" : "uge";
        }
        std::string ty = (cmpTy->isPtr()) ? std::string("ptr") : G->llvmType(cmpTy);
        bool isCmpFloat = cmpTy->isFloat() && !cmpTy->isPtr();
        std::string r = G->t();
        if (isCmpFloat)
          G->out << "  " << r << " = fcmp " << G->fflag() << " " << cmp
                 << " " << ty << " " << a << ", " << b << "\n";
        else
          G->out << "  " << r << " = icmp " << cmp << " " << ty << " " << a
                 << ", " << b << "\n";
        return r;
      }
      if (lt->isPtr() || rt->isPtr()) {
        // ptr + int / ptr - int (pointer arithmetic)
        if (op == "+" || op == "-") {
          std::string elem = lt->isPtr() ? G->llvmType(lt->pointee)
                                         : G->llvmType(rt->pointee);
          std::string idx = lt->isPtr() ? b : a;
          std::string base = lt->isPtr() ? a : b;
          if (op == "-") {
            std::string neg = G->t();
            G->out << "  " << neg << " = sub i64 0, " << idx << "\n";
            idx = neg;
          }
          std::string r = G->t();
          G->out << "  " << r << " = getelementptr inbounds " << elem
                 << ", ptr " << base << ", i64 " << idx << "\n";
          return r;
        }
        if (op == "-") {  // ptr - ptr -> element distance
          std::string ai = G->t(), bi = G->t(), d = G->t();
          std::string r = G->t();
          G->out << "  " << ai << " = ptrtoint ptr " << a << " to i64\n";
          G->out << "  " << bi << " = ptrtoint ptr " << b << " to i64\n";
          G->out << "  " << d << " = sub i64 " << ai << ", " << bi << "\n";
          G->out << "  " << r << " = sdiv exact i64 " << d << ", "
                 << typeSizeOf(lt->pointee) << "\n";
          return r;
        }
      }
      // cast BOTH operands to the common type if needed (report §3.1
      // follow-on: mixed-width arithmetic like i32 + i64 used to leave the
      // lhs register at its narrow width while the rhs was widened,
      // emitting `add i64 %i32val, %i64val` — malformed IR)
      if (rt && lt && !typesEquivalent(lt, rt)) {
        if (dt->isFloat() && rt->isFloat()) b = genCastInline(e->rhs, dt, b);
        else if (dt->isFloat() && rt->isInt()) b = genCastInline(e->rhs, dt, b);
        else if (dt->isInt() && rt->isInt()) b = genCastInline(e->rhs, dt, b);
      }
      if (lt && dt && !typesEquivalent(lt, dt) && lt->isNum() && dt->isNum())
        a = genCastInline(e->lhs, dt, a);
      // delegate to the shared emitter (also used by compound assignment)
      return emitArithOp(op, dt, a, b, e->rhs, e->loc);
    }
    case Expr::CAST: {
      std::string v = genExpr(e->base);
      return genCastInline(e->base, e->castTy, v);
    }
    case Expr::CALL: {
      // Phase 1d: variant constructor with payload (Ok(v), Red(3))
      if (e->enumRef && e->variantIdx >= 0)
        return genVariantConstruct(e, &e->args);
      return genCall(e);
    }
    // Phase 1b (report §4.1): array literal — alloca + element stores;
    // value = address
    case Expr::ARRLIT: {
      Type* at = e->ty;   // ARRAY<elem, n>
      std::string a = G->t();
      G->out << "  " << a << " = alloca " << G->llvmType(at) << ", align "
             << typeAlignOf(at) << "\n";
      Type* et = at->pointee;
      for (size_t i = 0; i < e->args.size(); ++i) {
        Expr* el = e->args[i];
        std::string fp = G->t();
        G->out << "  " << fp << " = getelementptr inbounds "
               << G->llvmType(at) << ", ptr " << a << ", i64 0, i64 " << i
               << "\n";
        if (et->k == Type::STRUCT || et->k == Type::ENUM || et->k == Type::ARRAY) {
          std::string src = genExpr(el);
          G->out << "  call void @llvm.memcpy.p0.p0.i64(ptr " << fp << ", ptr "
                 << src << ", i64 " << typeSizeOf(et) << ", i1 false)\n";
        } else if (et->isPair()) {
          std::string v = genExpr(el);
          G->out << "  store { ptr, i64 } " << v << ", ptr " << fp << "\n";
        } else {
          std::string v = genExpr(el);
          if (el->ty && !typesEquivalent(el->ty, et) && el->ty->isNum() &&
              et->isNum())
            v = genCastInline(el, et, v);
          G->out << "  store " << G->scalarCodegenTy(et) << " " << v
                 << ", ptr " << fp << "\n";
        }
      }
      return a;
    }
    // Phase 1c/2b (report §4.1/§4.5): base[lo..hi] — build a view pair
    case Expr::SLICE: {
      Type* bt = e->base->ty;
      std::string lo;
      if (e->lhs) {
        lo = genExpr(e->lhs);
        if (e->lhs->ty && e->lhs->ty->bits() < 64)
          lo = genCastInline(e->lhs, Type::simple(Type::I64), lo);
      } else {
        lo = G->t();
        G->out << "  " << lo << " = add i64 0, 0\n";
      }
      std::string basePtr;
      std::string baseLen;
      if (bt->k == Type::STR || bt->k == Type::SPAN) {
        std::string val = genExpr(e->base);
        basePtr = G->t();
        G->out << "  " << basePtr << " = extractvalue { ptr, i64 } " << val
               << ", 0\n";
        baseLen = G->t();
        G->out << "  " << baseLen << " = extractvalue { ptr, i64 } " << val
               << ", 1\n";
      } else if (bt->k == Type::ARRAY) {
        basePtr = genAddr(e->base);
        if (basePtr.empty()) basePtr = genExpr(e->base);
        baseLen = G->t();
        G->out << "  " << baseLen << " = add i64 0, " << bt->arrLen << "\n";
      }
      // hi defaults to the full length
      std::string hi = baseLen;
      if (e->rhs) {
        std::string hv = genExpr(e->rhs);
        if (e->rhs->ty && e->rhs->ty->bits() < 64)
          hv = genCastInline(e->rhs, Type::simple(Type::I64), hv);
        hi = hv;
      }
      // checked builds validate 0 <= lo <= hi <= len
      if (boundsChecksEnabled()) {
        std::string c1 = G->t();
        G->out << "  " << c1 << " = icmp ule i64 " << lo << ", " << hi << "\n";
        std::string c2 = G->t();
        G->out << "  " << c2 << " = icmp ule i64 " << hi << ", " << baseLen
               << "\n";
        std::string both = G->t();
        G->out << "  " << both << " = and i1 " << c1 << ", " << c2 << "\n";
        int cont = G->bb++;
        int trap = G->bb++;
        G->out << "  br i1 " << both << ", label %" << G->l(cont)
               << ", label %" << G->l(trap) << "\n";
        G->terminate();
        G->out << G->l(trap) << ":\n";
        G->curLabelTerminated = false;
        G->out << "  call void @frt_bound_trap(i64 " << lo << ", i64 "
               << baseLen << ")\n";
        G->out << "  unreachable\n";
        G->terminate();
        G->out << G->l(cont) << ":\n";
        G->curLabelTerminated = false;
      }
      // data pointer advanced by lo elements
      std::string elemTy = (bt->k == Type::STR) ? std::string("i8")
                                                : G->llvmType(bt->pointee);
      std::string ptr = G->t();
      G->out << "  " << ptr << " = getelementptr inbounds " << elemTy
             << ", ptr " << basePtr << ", i64 " << lo << "\n";
      std::string nlen = G->t();
      G->out << "  " << nlen << " = sub i64 " << hi << ", " << lo << "\n";
      std::string v1 = G->t();
      G->out << "  " << v1 << " = insertvalue { ptr, i64 } poison, ptr " << ptr
             << ", 0\n";
      std::string v2 = G->t();
      G->out << "  " << v2 << " = insertvalue { ptr, i64 } " << v1 << ", i64 "
             << nlen << ", 1\n";
      return v2;
    }
    // Phase 2c (report §4.6): `expr?` — propagate None/Err early
    case Expr::TRYQ: {
      // enum value = address of { i64 tag, payload }
      std::string agg = genExpr(e->base);
      EnumDef* ed = e->enumRef;
      EnumVariant& errV = ed->variants[e->variantIdx];
      std::string tagp = G->t();
      G->out << "  " << tagp << " = getelementptr %enum." << ed->name
             << ", ptr " << agg << ", i32 0, i32 0\n";
      std::string tag = G->t();
      G->out << "  " << tag << " = load i64, ptr " << tagp << "\n";
      std::string iserr = G->t();
      G->out << "  " << iserr << " = icmp eq i64 " << tag << ", " << errV.idx
             << "\n";
      int okBB = G->bb++;
      int errBB = G->bb++;
      G->out << "  br i1 " << iserr << ", label %" << G->l(errBB)
             << ", label %" << G->l(okBB) << "\n";
      G->terminate();
      // ---- error path: build the fn's error value and return
      G->out << G->l(errBB) << ":\n";
      G->curLabelTerminated = false;
      {
        Type* rt = G->curFn->ret;
        EnumDef* red = rt->edef;
        std::string slot = G->t();
        G->out << "  " << slot << " = alloca %enum." << red->name << ", align "
               << red->align << "\n";
        // find the matching error variant in the fn's return enum
        int rErrIdx = -1;
        for (auto& v : red->variants)
          if (v.name == errV.name) rErrIdx = v.idx;
        G->out << "  store i64 " << rErrIdx << ", ptr " << slot << "\n";
        // copy the error payload across when both carry one
        if (!errV.fields.empty()) {
          for (auto& v : red->variants) {
            if (v.name != errV.name || v.fields.empty()) continue;
            std::string sp = G->t();
            G->out << "  " << sp << " = getelementptr i8, ptr " << agg
                   << ", i64 " << (8 + errV.offsets[0]) << "\n";
            std::string dp = G->t();
            G->out << "  " << dp << " = getelementptr i8, ptr " << slot
                   << ", i64 " << (8 + v.offsets[0]) << "\n";
            Type* et = errV.fields[0].ty;
            if (et->k == Type::STRUCT || et->k == Type::ENUM ||
                et->k == Type::ARRAY) {
              G->out << "  call void @llvm.memcpy.p0.p0.i64(ptr " << dp
                     << ", ptr " << sp << ", i64 " << typeSizeOf(et)
                     << ", i1 false)\n";
            } else if (et->isPair()) {
              std::string ev = G->t();
              G->out << "  " << ev << " = load { ptr, i64 }, ptr " << sp
                     << "\n";
              G->out << "  store { ptr, i64 } " << ev << ", ptr " << dp
                     << "\n";
            } else {
              std::string ev = G->t();
              G->out << "  " << ev << " = load " << G->llvmType(et) << ", ptr "
                     << sp << "\n";
              G->out << "  store " << G->llvmType(v.fields[0].ty) << " " << ev
                     << ", ptr " << dp << "\n";
            }
            break;
          }
        }
        // defers must run on this early exit (same as return)
        emitDeferredAll();
        std::string ev = G->t();
        G->out << "  " << ev << " = load %enum." << red->name << ", ptr "
               << slot << "\n";
        if (G->curFn->name == "main" && G->curFn->ret->k == Type::VOID) {
          G->out << "  ret i32 0\n";
        } else {
          G->out << "  ret %enum." << red->name << " " << ev << "\n";
        }
        G->terminate();
      }
      // ---- ok path: extract payload field 0
      G->out << G->l(okBB) << ":\n";
      G->curLabelTerminated = false;
      {
        // find the ok variant (Some/Ok) of the value's enum
        const std::string& en = ed->templateName.empty() ? ed->name
                                                         : ed->templateName;
        const char* okName = (en == "Option") ? "Some" : "Ok";
        int okIdx = -1;
        for (auto& v : ed->variants)
          if (v.name == okName) okIdx = v.idx;
        EnumVariant& okV = ed->variants[okIdx];
        Type* pt = okV.fields[0].ty;
        std::string fp = G->t();
        G->out << "  " << fp << " = getelementptr i8, ptr " << agg << ", i64 "
               << (8 + okV.offsets[0]) << "\n";
        if (pt->k == Type::STRUCT || pt->k == Type::ENUM || pt->k == Type::ARRAY) {
          // aggregate payload: copy into a slot; value = slot address
          std::string slot = G->t();
          G->out << "  " << slot << " = alloca " << G->llvmType(pt) << "\n";
          G->out << "  call void @llvm.memcpy.p0.p0.i64(ptr " << slot
                 << ", ptr " << fp << ", i64 " << typeSizeOf(pt)
                 << ", i1 false)\n";
          return slot;
        }
        std::string r = G->t();
        G->out << "  " << r << " = load " << G->llvmType(pt) << ", ptr " << fp
               << "\n";
        return r;
      }
    }
  }
  return "";
}

// inline numeric cast: value v of type from -> to
std::string genCastInline(Expr* fromExpr, Type* to, std::string v) {
  Type* from = fromExpr->ty;
  if (!from || !to || typesEquivalent(from, to)) return v;
  // identity-by-representation checks FIRST: penstock names registers
  // %1..%N explicitly, so a temp allocated here and then abandoned would
  // leave a gap in the numbering and LLVM rejects the whole function.
  // (same-width ints share representation; opaque pointers are all `ptr`)
  if (from->isInt() && to->isInt() && from->bits() == to->bits()) return v;
  if (from->isPtr() && to->isPtr()) return v;
  std::string r = G->t();
  if (from->isFloat() && to->isFloat()) {
    if (to->k == Type::F64)
      G->out << "  " << r << " = fpext float " << v << " to double\n";
    else
      G->out << "  " << r << " = fptrunc double " << v << " to float\n";
    return r;
  }
  if (from->isFloat() && to->isInt()) {
    std::string op = to->isSigned() ? "fptosi" : "fptoui";
    G->out << "  " << r << " = " << op << " " << G->llvmType(from) << " " << v
           << " to " << G->llvmType(to) << "\n";
    return r;
  }
  if (from->isInt() && to->isFloat()) {
    std::string op = from->isSigned() ? "sitofp" : "uitofp";
    G->out << "  " << r << " = " << op << " " << G->llvmType(from) << " " << v
           << " to " << G->llvmType(to) << "\n";
    return r;
  }
  if (from->isInt() && to->isInt()) {
    int cop = intCastOp(from, to);
    const char* name = cop == 0 ? "trunc" : (cop == 1 ? "zext" : "sext");
    G->out << "  " << r << " = " << name << " i" << from->bits() << " " << v
           << " to i" << to->bits() << "\n";
    return r;
  }
  return v;
}

} // namespace freshet
