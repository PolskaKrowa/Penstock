// Freshet expression typing — builtins, intrinsics, struct literals, soa
// indexing, casts. Literal type adoption rules.
#include "common.h"

namespace freshet {

static bool isBuiltinCall(const std::string& n) {
  static const std::set<std::string> b = {
    "malloc", "free", "give",
    "print", "print_f64", "print_i64", "print_u64", "println",
    "memcpy", "memset",
    "soa_new", "soa_free", "soa_len", "soa_field_ptr",
    "assert",
    "sqrt", "sin", "cos", "tan", "exp", "log", "log2", "log10", "pow",
    "fabs", "fmin", "fmax", "floor", "ceil", "round",
    // optimisation-audit §6.7: exact integer min/max — used by the loop
    // tiler for boundary-tile bounds, and generally useful for clamped
    // index arithmetic
    "imin", "imax",
    // Phase 1c (report §4.1): span construction
    "span",
    // Phase 2b (report §4.5): string views + owned strings
    "str_eq", "str_cat", "str_to_cstr",
    "string_new", "string_from", "string_append", "string_free",
    "str_of", "fmt_f64", "fmt_i64",
  };
  return b.count(n) > 0;
}

static Type* typeFromIdent(const std::string& n) {
  // generic instantiation scope: `malloc(T, n)` inside fn f<T>(...)
  if (Type* tp = tparamTypeInScope(n)) return tp;
  if (n == "u8") return Type::simple(Type::U8);
  if (n == "u16") return Type::simple(Type::U16);
  if (n == "u32") return Type::simple(Type::U32);
  if (n == "u64" || n == "usize") return Type::simple(Type::USIZE);
  if (n == "i8") return Type::simple(Type::I8);
  if (n == "i16") return Type::simple(Type::I16);
  if (n == "i32") return Type::simple(Type::I32);
  if (n == "i64") return Type::simple(Type::I64);
  if (n == "f32") return Type::simple(Type::F32);
  if (n == "f64") return Type::simple(Type::F64);
  if (n == "bool") return Type::simple(Type::BOOL);
  StructDef* sd = findStructDef(n);
  if (sd) return Type::structOf(sd);
  return nullptr;
}

// ---------------------------------------------------------------- operator matrix
// Phase 0c (report §3.3): operator families are checked explicitly, in
// sema, BEFORE code generation. The historical checker accepted any
// numeric operand for any arithmetic/bitwise operator, so invalid
// combinations (float << 1, 1.5 % 2) only exploded later in IRgen.
static bool isArithOp(const std::string& op) {
  return op == "+" || op == "-" || op == "*" || op == "/";
}
static bool isIntOnlyOp(const std::string& op) {
  return op == "%" || op == "&" || op == "|" || op == "^" ||
         op == "<<" || op == ">>";
}
static bool isCmpOp(const std::string& op) {
  return op == "==" || op == "!=" || op == "<" || op == "<=" ||
         op == ">" || op == ">=";
}

// builtin/intrinsic arg checking; returns return type or nullptr if unknown
static Type* typeBuiltin(FnSema* fs, Expr* e) {
  const std::string& n = e->name;
  auto argT = [&](int i) { return i < (int)e->args.size() ? e->args[i]->ty : nullptr; };
  auto need = [&](int cnt) {
    if ((int)e->args.size() != cnt) {
      diag(Diag::Error, Code::Args, n + " expects " + std::to_string(cnt) +
           " args, got " + std::to_string(e->args.size()),
           e->loc.file, e->loc.line, e->loc.col);
      return false;
    }
    return true;
  };

  if (n == "malloc") {
    // malloc(T, n) — first arg is a type name
    if (!need(2)) return Type::ptr(Type::simple(Type::U8));
    Expr* tArg = e->args[0];
    Type* elem = nullptr;
    if (tArg->k == Expr::IDENT) {
      elem = typeFromIdent(tArg->name);
      if (!elem) {
        StructDef* sd = findStructDef(tArg->name);
        if (sd) elem = Type::structOf(sd);
      }
    }
    if (!elem) {
      diag(Diag::Error, Code::Args, "malloc's first argument must be a type",
           e->loc.file, e->loc.line, e->loc.col);
      return Type::ptr(Type::simple(Type::U8));
    }
    if (elem->k == Type::VOID || elem->k == Type::SOA) {
      diag(Diag::Error, Code::Args, "cannot malloc " + typeToString(elem),
           e->loc.file, e->loc.line, e->loc.col);
    }
    if (!argT(1) || !argT(1)->isInt()) {
      diag(Diag::Error, Code::Args, "malloc count must be integer",
           e->loc.file, e->loc.line, e->loc.col);
    }
    e->mallocElem = elem;
    return Type::ptr(elem);
  }
  if (n == "free") {
    if (!need(1)) return Type::simple(Type::VOID);
    if (argT(0) && !argT(0)->isPtr()) {
      diag(Diag::Error, Code::Args, "free expects a pointer",
           e->loc.file, e->loc.line, e->loc.col);
    }
    return Type::simple(Type::VOID);
  }
  if (n == "give") {
    if (!need(1)) return Type::simple(Type::VOID);
    return argT(0);   // give(p) is a marker used in calls; also standalone?
  }
  if (n == "print") {
    if (!need(1)) return Type::simple(Type::VOID);
    // Phase 2b (report §4.5): print accepts any str value — literals AND
    // views/slices (the historical checker required a literal, which is
    // what made strings a print-only facility)
    if (e->args[0]->k != Expr::STR &&
        !(e->args[0]->ty && e->args[0]->ty->k == Type::STR)) {
      diag(Diag::Error, Code::Args, "print expects a str value",
           e->loc.file, e->loc.line, e->loc.col);
    }
    return Type::simple(Type::VOID);
  }
  if (n == "print_f64") { need(2); return Type::simple(Type::VOID); }
  if (n == "print_i64" || n == "print_u64") { need(1); return Type::simple(Type::VOID); }
  if (n == "println") { need(0); return Type::simple(Type::VOID); }
  if (n == "memcpy" || n == "memset") {
    if (n == "memcpy") need(3); else need(3);
    return Type::simple(Type::VOID);
  }
  if (n == "soa_new") {
    // soa_new(StructName, n)
    if (!need(2)) return Type::simple(Type::VOID);
    Expr* tArg = e->args[0];
    StructDef* sd = nullptr;
    if (tArg->k == Expr::IDENT) sd = findStructDef(tArg->name);
    if (!sd) {
      diag(Diag::Error, Code::Args,
           "soa_new's first argument must be a struct name",
           e->loc.file, e->loc.line, e->loc.col);
      return Type::simple(Type::VOID);
    }
    if (!argT(1) || !argT(1)->isInt()) {
      diag(Diag::Error, Code::Args, "soa_new count must be integer",
           e->loc.file, e->loc.line, e->loc.col);
    }
    e->mallocElem = Type::structOf(sd);
    return Type::soaOf(sd);
  }
  if (n == "soa_free") {
    if (!need(1)) return Type::simple(Type::VOID);
    if (argT(0) && argT(0)->k != Type::SOA) {
      diag(Diag::Error, Code::Args, "soa_free expects a soa() value",
           e->loc.file, e->loc.line, e->loc.col);
    }
    return Type::simple(Type::VOID);
  }
  if (n == "soa_len") {
    if (!need(1)) return Type::simple(Type::USIZE);
    if (argT(0) && argT(0)->k != Type::SOA) {
      diag(Diag::Error, Code::Args, "soa_len expects a soa() value",
           e->loc.file, e->loc.line, e->loc.col);
    }
    return Type::simple(Type::USIZE);
  }
  if (n == "soa_field_ptr") {
    // soa_field_ptr(s, "x") -> *f64  (for raw interop)
    need(2);
    if (argT(0)) {
      if (argT(0)->k != Type::SOA) {
        diag(Diag::Error, Code::Args, "soa_field_ptr expects soa() first arg",
             e->loc.file, e->loc.line, e->loc.col);
        return Type::ptr(Type::simple(Type::U8));
      }
      StructDef* sd = argT(0)->sdef;
      if (e->args[1]->k == Expr::STR) {
        for (auto& f : sd->fields)
          if (f.name == e->args[1]->sval) return Type::ptr(f.ty);
        diag(Diag::Error, Code::Args, "no field " + e->args[1]->sval +
             " in struct " + sd->name, e->loc.file, e->loc.line, e->loc.col);
      }
      return Type::ptr(Type::simple(Type::U8));
    }
    return Type::ptr(Type::simple(Type::U8));
  }
  if (n == "assert") {
    need(1);
    return Type::simple(Type::VOID);
  }
  // ---------------- Phase 1c (report §4.1): span construction
  if (n == "span") {
    // span(p: *T, n) -> Span<T>   |   span(a: [T; N]) -> Span<T>
    if (e->args.empty()) {
      diag(Diag::Error, Code::Args, "span expects a pointer + length or an array",
           e->loc.file, e->loc.line, e->loc.col);
      return Type::spanOf(Type::simple(Type::U8));
    }
    Type* a0 = argT(0);
    if (e->args.size() == 1) {
      if (a0 && a0->k == Type::ARRAY)
        return Type::spanOf(a0->pointee);
      diag(Diag::Error, Code::Args,
           "span(a) expects a fixed array [T; N]; for pointers use span(p, len)",
           e->loc.file, e->loc.line, e->loc.col);
      return Type::spanOf(Type::simple(Type::U8));
    }
    if (e->args.size() != 2) {
      diag(Diag::Error, Code::Args, "span expects (pointer, length)",
           e->loc.file, e->loc.line, e->loc.col);
      return Type::spanOf(Type::simple(Type::U8));
    }
    if (!a0 || !a0->isPtr()) {
      diag(Diag::Error, Code::Args, "span's first argument must be a pointer",
           e->loc.file, e->loc.line, e->loc.col);
      return Type::spanOf(Type::simple(Type::U8));
    }
    if (a0->pointee && (a0->pointee->k == Type::VOID)) {
      diag(Diag::Error, Code::Args,
           "cannot build a Span<void>; cast the pointer to *T first",
           e->loc.file, e->loc.line, e->loc.col);
    }
    if (argT(1) && !argT(1)->isInt()) {
      diag(Diag::Error, Code::Args, "span length must be an integer",
           e->loc.file, e->loc.line, e->loc.col);
    }
    return Type::spanOf(a0->pointee);
  }
  // ---------------- Phase 2b (report §4.5): strings
  if (n == "str_eq") {
    need(2);
    for (int i = 0; i < 2; ++i)
      if (argT(i) && argT(i)->k != Type::STR)
        diag(Diag::Error, Code::Args, "str_eq expects two str values",
             e->loc.file, e->loc.line, e->loc.col);
    return Type::simple(Type::BOOL);
  }
  if (n == "str_cat" || n == "string_from") {
    if (n == "str_cat") {
      if ((int)e->args.size() != 2) {
        diag(Diag::Error, Code::Args, "str_cat expects two str values",
             e->loc.file, e->loc.line, e->loc.col);
      }
      for (int i = 0; i < (int)e->args.size(); ++i)
        if (argT(i) && argT(i)->k != Type::STR)
          diag(Diag::Error, Code::Args, "str_cat expects str arguments",
               e->loc.file, e->loc.line, e->loc.col);
    } else {
      need(1);
      if (argT(0) && argT(0)->k != Type::STR)
        diag(Diag::Error, Code::Args, "string_from expects a str value",
             e->loc.file, e->loc.line, e->loc.col);
    }
    return Type::structOf(ensurePreludeString(currentProgram()));
  }
  if (n == "str_to_cstr") {
    need(1);
    if (argT(0) && argT(0)->k != Type::STR)
      diag(Diag::Error, Code::Args, "str_to_cstr expects a str value",
           e->loc.file, e->loc.line, e->loc.col);
    return Type::ptr(Type::simple(Type::U8));
  }
  if (n == "string_new" || n == "fmt_f64" || n == "fmt_i64") {
    if (n == "string_new") need(0);
    else if (n == "fmt_f64") {
      need(2);
      if (argT(0) && !argT(0)->isFloat())
        diag(Diag::Error, Code::Args, "fmt_f64 expects (f64, digits)",
             e->loc.file, e->loc.line, e->loc.col);
      if (argT(1) && !argT(1)->isInt())
        diag(Diag::Error, Code::Args, "fmt_f64 digit count must be an integer",
             e->loc.file, e->loc.line, e->loc.col);
    } else {
      need(1);
      if (argT(0) && !argT(0)->isInt())
        diag(Diag::Error, Code::Args, "fmt_i64 expects an integer",
             e->loc.file, e->loc.line, e->loc.col);
    }
    return Type::structOf(ensurePreludeString(currentProgram()));
  }
  if (n == "string_append") {
    need(2);
    if (argT(0) && !(argT(0)->k == Type::PTR &&
                     argT(0)->pointee && argT(0)->pointee->k == Type::STRUCT &&
                     argT(0)->pointee->sdef == ensurePreludeString(currentProgram())))
      diag(Diag::Error, Code::Args,
           "string_append expects (&String, str) — take the address with &s",
           e->loc.file, e->loc.line, e->loc.col);
    if (argT(1) && argT(1)->k != Type::STR)
      diag(Diag::Error, Code::Args, "string_append's second argument must be str",
           e->loc.file, e->loc.line, e->loc.col);
    return Type::simple(Type::VOID);
  }
  if (n == "string_free") {
    need(1);
    if (argT(0) && !(argT(0)->k == Type::PTR &&
                     argT(0)->pointee && argT(0)->pointee->k == Type::STRUCT &&
                     argT(0)->pointee->sdef == ensurePreludeString(currentProgram())))
      diag(Diag::Error, Code::Args, "string_free expects a *String",
           e->loc.file, e->loc.line, e->loc.col);
    return Type::simple(Type::VOID);
  }
  if (n == "str_of") {
    need(1);
    if (argT(0) && !(argT(0)->k == Type::PTR &&
                     argT(0)->pointee && argT(0)->pointee->k == Type::STRUCT &&
                     argT(0)->pointee->sdef == ensurePreludeString(currentProgram())))
      diag(Diag::Error, Code::Args, "str_of expects a *String",
           e->loc.file, e->loc.line, e->loc.col);
    return Type::simple(Type::STR);
  }
  // math
  static const std::set<std::string> math1 = {
    "sqrt", "sin", "cos", "tan", "exp", "log", "log2", "log10",
    "fabs", "floor", "ceil", "round"
  };
  if (math1.count(n)) {
    if (!need(1)) return Type::simple(Type::F64);
    Type* a = argT(0);
    if (a && !a->isFloat()) {
      diag(Diag::Error, Code::Args, n + " expects a float argument",
           e->loc.file, e->loc.line, e->loc.col);
    }
    return (a && a->k == Type::F32) ? Type::simple(Type::F32)
                                    : Type::simple(Type::F64);
  }
  if (n == "pow" || n == "fmin" || n == "fmax") {
    if (!need(2)) return Type::simple(Type::F64);
    Type* a = argT(0);
    if (a && !a->isFloat()) {
      diag(Diag::Error, Code::Args, n + " expects float arguments",
           e->loc.file, e->loc.line, e->loc.col);
    }
    return (a && a->k == Type::F32) ? Type::simple(Type::F32)
                                    : Type::simple(Type::F64);
  }
  // integer min/max: two integer arguments of compatible width; the
  // result is their common type (INT literals adopt the wider type, the
  // same rule mixed-width arithmetic uses).  min/max are SIGN-SENSITIVE:
  // a syntactically negative operand (unary minus) forces the SIGNED
  // interpretation of the common width, so `-5` can never be silently
  // reinterpreted as a huge unsigned value by the promotion rule.
  if (n == "imin" || n == "imax") {
    if (!need(2)) return Type::simple(Type::I64);
    Type* a = argT(0);
    Type* b = argT(1);
    if ((!a || !a->isInt()) || (!b || !b->isInt())) {
      diag(Diag::Error, Code::Args, n + " expects two integer arguments",
           e->loc.file, e->loc.line, e->loc.col);
      return Type::simple(Type::I64);
    }
    Type* c = commonType(a, b);
    if (!c || !c->isInt()) c = Type::simple(Type::I64);
    auto isNegShape = [](Expr* x) {
      return x && x->k == Expr::UN && x->op == "-";
    };
    if (!c->isSigned() &&
        (isNegShape(e->args[0]) || isNegShape(e->args[1]))) {
      switch (c->bits()) {
        case 8:  c = Type::simple(Type::I8);  break;
        case 16: c = Type::simple(Type::I16); break;
        case 32: c = Type::simple(Type::I32); break;
        default: c = Type::simple(Type::I64); break;
      }
    }
    for (int i = 0; i < 2; ++i) {
      Expr* arg = e->args[i];
      if (arg->k == Expr::INT && arg->ty && arg->ty->isInt() &&
          arg->ty->bits() < c->bits()) {
        arg->ty = c;   // literal adoption
        arg->constant = true;
      }
    }
    return c;
  }
  return nullptr;
}

static Type* typeIntrinsic(FnSema* fs, Expr* e) {
  // freshet.xxx
  const std::string& n = e->name;
  auto need = [&](int cnt) {
    if ((int)e->args.size() != cnt) {
      diag(Diag::Error, Code::Args, n + " expects " + std::to_string(cnt) + " args",
           e->loc.file, e->loc.line, e->loc.col);
      return false;
    }
    return true;
  };
  if (n == "freshet.prefetch_r" || n == "freshet.prefetch_w" ||
      n == "freshet.clflush") {
    need(1);
    Type* a = e->args.empty() ? nullptr : e->args[0]->ty;
    if (a && !a->isPtr()) {
      diag(Diag::Error, Code::Args, n + " expects a pointer",
           e->loc.file, e->loc.line, e->loc.col);
    }
    return Type::simple(Type::VOID);
  }
  if (n == "freshet.cycles" || n == "freshet.time_ns" || n == "freshet.time_ms") {
    need(0);
    return Type::simple(Type::U64);
  }
  if (n == "freshet.pause") { need(0); return Type::simple(Type::VOID); }
  if (n == "freshet.threads") { need(0); return Type::simple(Type::USIZE); }
  return nullptr;
}

// ---------------------------------------------------------------- variants
// Phase 1d (report §4.3): resolve a variant construction against an
// expected enum type. Handles:
//   bare          Red          (IDENT)
//   payload       Red(5)       (CALL)
//   qualified     Color.Red    (FIELD or CALL "Color.Red")
// Returns true when the expression was resolved as a variant.
static bool resolveVariantExprAgainst(FnSema* fs, Expr* e, EnumDef* ed,
                                       Type* enumTy) {
  if (!e || !ed) return false;
  std::string vname;
  bool hasArgs = false;
  std::vector<Expr*>* args = nullptr;
  if (e->k == Expr::IDENT) {
    // locals shadow variant names
    if (fs && fs->lookup(e->name)) return false;
    vname = e->name;
  } else if (e->k == Expr::CALL) {
    if (lookupFn(e->name)) return false;   // real functions win
    if (e->name.find('.') != std::string::npos) {
      size_t dot = e->name.find('.');
      if (e->name.substr(0, dot) !=
          (ed->templateName.empty() ? ed->name : ed->templateName))
        return false;
      vname = e->name.substr(dot + 1);
    } else {
      vname = e->name;
    }
    hasArgs = true;
    args = &e->args;
  } else if (e->k == Expr::FIELD) {
    if (!e->base || e->base->k != Expr::IDENT) return false;
    if (e->base->name != (ed->templateName.empty() ? ed->name : ed->templateName))
      return false;
    vname = e->name;
  } else {
    return false;
  }
  EnumVariant* v = nullptr;
  for (auto& vv : ed->variants)
    if (vv.name == vname) { v = &vv; break; }
  if (!v) return false;
  e->enumRef = ed;
  e->variantIdx = v->idx;
  e->ty = enumTy;
  if (hasArgs) {
    if (v->fields.empty()) {
      diag(Diag::Error, Code::Args,
           "variant " + vname + " carries no payload",
           e->loc.file, e->loc.line, e->loc.col);
    } else if (args->size() != v->fields.size()) {
      diag(Diag::Error, Code::Args,
           "variant " + vname + " expects " +
           std::to_string(v->fields.size()) + " payload value(s), got " +
           std::to_string(args->size()),
           e->loc.file, e->loc.line, e->loc.col);
    }
    for (size_t i = 0; i < args->size() && i < v->fields.size(); ++i) {
      Type* at = typeExpr(fs, (*args)[i]);
      Type* pt = v->fields[i].ty;
      if (at && pt && !typesCompatible(pt, at) &&
          !(at->isNum() && pt->isNum())) {
        diag(Diag::Error, Code::Type,
             "payload " + std::to_string(i + 1) + " of " + vname + ": expected " +
             typeToString(pt) + ", got " + typeToString(at),
             e->loc.file, e->loc.line, e->loc.col);
      }
    }
  } else if (!v->fields.empty()) {
    // allow zero-arg spellings only for payload-less variants
    diag(Diag::Error, Code::Args,
         "variant " + vname + " carries a payload — construct it as " +
         vname + "(...)",
         e->loc.file, e->loc.line, e->loc.col);
  }
  return true;
}

// Phase 2a: `Pair { a: 1.5, b: 7 }` against an instantiated annotation
// `Pair<f64, i64>` — the literal names the TEMPLATE; adopt the instance's
// mangled name so typing, layout and IRgen all see concrete field types.
void tryAdoptStructLiteral(Expr* e, Type* expected) {
  if (!e || !expected || e->k != Expr::SLIT) return;
  if (expected->k != Type::STRUCT || !expected->sdef) return;
  const std::string& want = expected->sdef->name;
  if (want == e->name) return;   // non-generic: already exact
  if (want.rfind(e->name + "$", 0) == 0) e->name = want;
}

void tryResolveVariantExpr(FnSema* fs, Expr* e, Type* expected) {
  if (!e || !expected || expected->k != Type::ENUM) return;
  resolveVariantExprAgainst(fs, e, expected->edef, expected);
}

// global fallback: a bare name that is a variant of exactly one NON-generic
// enum in the program resolves without context
static bool resolveVariantGlobally(FnSema* fs, Expr* e) {
  std::string vname;
  bool hasArgs = false;
  if (e->k == Expr::IDENT) {
    if (fs && fs->lookup(e->name)) return false;
    vname = e->name;
  } else if (e->k == Expr::CALL) {
    if (lookupFn(e->name) || isBuiltinCall(e->name)) return false;
    if (e->name.find('.') != std::string::npos) {
      size_t dot = e->name.find('.');
      std::string en = e->name.substr(0, dot);
      vname = e->name.substr(dot + 1);
      if (EnumDef* ed = findEnumDef(en))
        if (!ed->isGeneric)
          return resolveVariantExprAgainst(fs, e, ed, Type::enumOf(ed));
      return false;
    }
    vname = e->name;
    hasArgs = true;
  } else {
    return false;
  }
  EnumDef* found = nullptr;
  for (EnumDef* ed : allEnumDefs()) {
    if (ed->isGeneric) continue;   // Option<T> etc. need context
    for (auto& v : ed->variants)
      if (v.name == vname) { found = ed; break; }
    if (found) break;
  }
  if (!found) return false;
  // ambiguity check: same variant name in another enum?
  for (EnumDef* ed : allEnumDefs()) {
    if (ed == found || ed->isGeneric) continue;
    for (auto& v : ed->variants)
      if (v.name == vname) {
        diag(Diag::Error, Code::Match,
             "ambiguous variant name " + vname + " (in " + found->name +
             " and " + ed->name + ") — qualify it as " + ed->name + "." + vname,
             e->loc.file, e->loc.line, e->loc.col);
        return true;
      }
  }
  (void)hasArgs;
  return resolveVariantExprAgainst(fs, e, found, Type::enumOf(found));
}

Type* typeExpr(FnSema* fs, Expr* e) {
  if (e->ty) return e->ty;   // memoized
  auto bad = [&](const std::string& m) {
    diag(Diag::Error, Code::Type, m, e->loc.file, e->loc.line, e->loc.col);
    e->ty = Type::simple(Type::U64);
    return e->ty;
  };

  switch (e->k) {
    case Expr::INT:
      e->ty = Type::simple(Type::U64);
      e->constant = true;
      return e->ty;
    case Expr::FLT:
      e->ty = Type::simple(Type::F64);
      e->constant = true;
      return e->ty;
    case Expr::STR:
      e->ty = Type::simple(Type::STR);
      return e->ty;
    case Expr::BOOL:
      e->ty = Type::simple(Type::BOOL);
      e->constant = true;
      return e->ty;
    case Expr::NILQ:
      e->ty = Type::ptr(Type::simple(Type::VOID));
      e->constant = true;
      return e->ty;
    case Expr::IDENT: {
      if (fs) {
        if (Sym* s = fs->lookup(e->name)) {
          e->ty = s->ty;
          e->isLvalue = s->k != Sym::FN;
          e->sym = s;
          return e->ty;
        }
      }
      if (GlobalVar* g = lookupGlobal(e->name)) {
        e->ty = g->ty;
        e->isLvalue = g->mut;
        e->globalRef = g;
        return e->ty;
      }
      if (FnDecl* f = lookupFn(e->name)) {
        // Phase 3b (report §6.3): referencing a function by name (not a
        // call) yields its fn-pointer value — `let f: fn(i32, i32) -> i32
        // = add;` / passing `add` as a C callback argument
        e->ty = f->ty;
        e->fnValue = f;
        return e->ty;
      }
      // Phase 1d: bare enum variant (Red) with no local shadow
      if (resolveVariantGlobally(fs, e)) return e->ty;
      // might be a type name in expr position (malloc arg handled elsewhere)
      return bad("unknown identifier: " + e->name);
    }
    case Expr::SLIT: {
      StructDef* sd = findStructDef(e->name);
      if (!sd) return bad("unknown struct: " + e->name);
      if (sd->isGeneric)
        return bad("struct " + sd->name + " is generic — annotate the "
                   "binding with concrete type arguments (e.g. " +
                   sd->name + "<i64, f64>)");
      if (e->slitFields.size() != sd->fields.size()) {
        return bad("struct literal for " + sd->name + " must list all " +
                   std::to_string(sd->fields.size()) + " fields (strict: no partial init)");
      }
      for (size_t i = 0; i < e->slitFields.size(); ++i) {
        Field* f = nullptr;
        for (auto& fd : sd->fields) if (fd.name == e->slitFields[i]) f = &fd;
        if (!f) return bad("struct " + sd->name + " has no field " + e->slitFields[i]);
        // nested literals adopt the field's instantiated type
        // (Pair { a: Pair { ... }, ... })
        tryAdoptStructLiteral(e->args[i], f->ty);
        Type* vt = typeExpr(fs, e->args[i]);
        if (vt && !typesCompatible(f->ty, vt)) {
          if (!(e->args[i]->constant && f->ty->isNum() && vt->isNum()))
            diag(Diag::Error, Code::Type, "field " + f->name + ": " +
                 typeToString(f->ty) + " vs " + typeToString(vt),
                 e->loc.file, e->loc.line, e->loc.col);
        }
      }
      e->ty = Type::structOf(sd);
      return e->ty;
    }
    case Expr::CALL: {
      // Phase 0d follow-on (report §3.4): sizeof/alignof are documented
      // as compile-time constants (spec §4) but were never implemented —
      // the call fell through to "unknown function". Resolve them here.
      if (e->name == "sizeof" || e->name == "alignof") {
        Expr* ta = e->args.empty() ? nullptr : e->args[0];
        Type* t = ta ? ta->castTy : nullptr;
        if (ta && ta->name.rfind("__tyarg__:", 0) == 0) {
          std::string enc = ta->name.substr(10);
          Type* resolved = bindTypeEncoding(currentProgram(), enc, e->loc,
                                            nullptr);
          if (resolved) { t = resolved; ta->castTy = resolved; }
        }
        if (!t)
          return bad(e->name + " needs a type argument, e.g. " +
                     e->name + "(f64)");
        e->ival = (e->name == "sizeof") ? typeSizeOf(t) : typeAlignOf(t);
        e->ty = Type::simple(Type::U64);
        e->constant = true;
        return e->ty;
      }
      // intrinsics
      if (e->name.rfind("freshet.", 0) == 0) {
        e->ty = typeIntrinsic(fs, e);
        if (e->ty) return e->ty;
        return bad("unknown intrinsic: " + e->name);
      }
      if (isBuiltinCall(e->name)) {
        // malloc(T, n) / soa_new(Struct, n): first arg is a type name, not
        // an expression — skip expression typing for it
        size_t skipTypeArg = (e->name == "malloc" || e->name == "soa_new") ? 1 : 0;
        for (size_t ai = 0; ai < e->args.size(); ++ai) {
          if (ai < skipTypeArg) {
            e->args[ai]->ty = Type::simple(Type::U64);
            continue;
          }
          typeExpr(fs, e->args[ai]);
        }
        e->ty = typeBuiltin(fs, e);
        if (e->ty) return e->ty;
        return bad("bad builtin call: " + e->name);
      }
      // user function
      FnDecl* f = lookupFn(e->name);
      // Phase 1d: variant constructor with payload (`Ok(v)`, `Red(3)`)
      if (!f && resolveVariantGlobally(fs, e)) return e->ty;
      if (!f) {
        // Phase 3b (report §6.3): indirect call through a function
        // pointer — `f(a, b)` where `f` is a local/global of type
        // fn(A, B) -> R.  The callee is a VALUE, not a name.
        Type* fpty = nullptr;
        Sym* fps = nullptr;
        GlobalVar* fpg = nullptr;
        if (fs) {
          if (Sym* s = fs->lookup(e->name)) {
            if (s->ty && s->ty->isFnPtr()) { fpty = s->ty; fps = s; }
          }
        }
        if (!fpty) {
          if (GlobalVar* g = lookupGlobal(e->name)) {
            if (g->ty && g->ty->isFnPtr()) { fpty = g->ty; fpg = g; }
          }
        }
        if (fpty) {
          if (e->args.size() != fpty->fnParams.size()) {
            for (auto* a : e->args) typeExpr(fs, a);
            return bad("function pointer " + e->name + " expects " +
                       std::to_string(fpty->fnParams.size()) + " args, got " +
                       std::to_string(e->args.size()));
          }
          for (size_t i = 0; i < e->args.size(); ++i) {
            Type* pt = fpty->fnParams[i];
            Type* at = typeExpr(fs, e->args[i]);
            if (at && pt && !typesCompatible(pt, at)) {
              if (!(e->args[i]->constant && pt->isNum() && at->isNum()))
                diag(Diag::Error, Code::Type,
                     "arg " + std::to_string(i + 1) + " of " + e->name +
                     ": expected " + typeToString(pt) + ", got " +
                     typeToString(at),
                     e->loc.file, e->loc.line, e->loc.col);
            }
          }
          e->fnPtrSym = fps;
          e->globalRef = fpg;
          e->ty = fpty->pointee;
          return e->ty;
        }
        for (auto* a : e->args) typeExpr(fs, a);
        return bad("unknown function: " + e->name);
      }
      // Phase 2a: generic instantiation from the argument types
      if (f->isGeneric) {
        for (auto* a : e->args) typeExpr(fs, a);
        std::vector<Type*> argTys;
        for (auto* a : e->args) argTys.push_back(a->ty);
        FnDecl* inst = instantiateGenericFn(currentProgram(), f, argTys,
                                             e->loc);
        if (!inst) return bad("cannot instantiate " + e->name);
        queueGenericInstance(inst);
        f = inst;
      }
      if ((int)e->args.size() != (int)f->params.size()) {
        for (auto* a : e->args) typeExpr(fs, a);
        return bad("function " + e->name + " expects " +
                   std::to_string(f->params.size()) + " args, got " +
                   std::to_string(e->args.size()));
      }
      for (size_t i = 0; i < e->args.size(); ++i) {
        // Phase 1d: enum-typed parameters resolve bare variants
        Type* pt = f->params[i].ty;
        if (pt && pt->k == Type::ENUM)
          tryResolveVariantExpr(fs, e->args[i], pt);
        // Phase 2a: instantiated struct params adopt template-named literals
        if (pt && pt->k == Type::STRUCT)
          tryAdoptStructLiteral(e->args[i], pt);
        Type* at = typeExpr(fs, e->args[i]);
        if (at && pt && !typesCompatible(pt, at)) {
          if (!(e->args[i]->constant && pt->isNum() && at->isNum()))
            diag(Diag::Error, Code::Type, "arg " + std::to_string(i + 1) + " of " +
                 e->name + ": expected " + typeToString(pt) + ", got " +
                 typeToString(at), e->loc.file, e->loc.line, e->loc.col);
        }
        if (e->argGive[i] && !(at && at->isPtr())) {
          diag(Diag::Error, Code::Own, "give() requires a pointer",
               e->loc.file, e->loc.line, e->loc.col);
        }
        // Phase MEM (§5.3): parameter mode enforcement at the call site
        if (f->params[i].owned && !e->argGive[i]) {
          diag(Diag::Error, Code::OwnCall,
               "argument " + std::to_string(i + 1) + " of " + e->name +
               " (" + f->params[i].name + ") is declared `owned` — the caller "
               "must transfer it with give(...)",
               e->loc.file, e->loc.line, e->loc.col);
        }
        if (f->params[i].borrowed && e->argGive[i]) {
          diag(Diag::Error, Code::OwnCall,
               "argument " + std::to_string(i + 1) + " of " + e->name +
               " (" + f->params[i].name + ") is declared `borrowed` — it cannot "
               "be given away",
               e->loc.file, e->loc.line, e->loc.col);
        }
      }
      e->callee = f;
      e->ty = f->ret;
      return e->ty;
    }
    case Expr::BIN: {
      Type* lt = typeExpr(fs, e->lhs);
      Type* rt = typeExpr(fs, e->rhs);
      const std::string& op = e->op;
      if (op == "&&" || op == "||") {
        if ((lt && lt->k != Type::BOOL) || (rt && rt->k != Type::BOOL))
          return bad("logical operators need bool operands");
        e->ty = Type::simple(Type::BOOL);
        return e->ty;
      }
      if (isCmpOp(op)) {
        // aggregate comparison is rejected (str: use str_eq)
        if ((lt && (lt->isAggregate() && lt->k != Type::SOA &&
                    lt->k != Type::PTR)) ||
            (rt && (rt->isAggregate() && rt->k != Type::SOA &&
                    rt->k != Type::PTR))) {
          if (lt && lt->k == Type::STR)
            return bad("str values are compared with str_eq(a, b)");
          if (rt && rt->k == Type::STR)
            return bad("str values are compared with str_eq(a, b)");
          return bad("aggregate values cannot be compared directly");
        }
        // pointer comparisons: == and != are defined (incl. nil); ordering
        // on pointers is not (report §3.5)
        if ((lt && lt->isPtr()) || (rt && rt->isPtr())) {
          bool eqOnly = (op == "==" || op == "!=");
          bool bothPtr = lt && rt && lt->isPtr() && rt->isPtr();
          bool ptrVsNil = (lt && lt->isPtr() && e->rhs->k == Expr::NILQ) ||
                          (rt && rt->isPtr() && e->lhs->k == Expr::NILQ);
          if (!eqOnly)
            return bad("pointer ordering comparisons are not defined — "
                       "only == and != are available for pointers");
          if (!bothPtr && !ptrVsNil)
            return bad("cannot compare pointer with non-pointer");
          e->ty = Type::simple(Type::BOOL);
          return e->ty;
        }
        if (lt && rt && !typesEquivalent(lt, rt) && !typesCompatible(lt, rt) &&
            !typesCompatible(rt, lt)) {
          if (!(e->lhs->constant || e->rhs->constant))
            return bad("comparison of " + typeToString(lt) + " and " +
                       typeToString(rt));
        }
        e->ty = Type::simple(Type::BOOL);
        e->constant = e->lhs->constant && e->rhs->constant;
        return e->ty;
      }
      // ---------------- pointer arithmetic (report §3.3 matrix)
      if ((lt && lt->isPtr()) || (rt && rt->isPtr())) {
        if (op == "+" && lt && rt && lt->isPtr() && rt->isInt()) {
          e->ty = lt;   // ptr + int -> ptr
          return e->ty;
        }
        if (op == "+" && lt && rt && rt->isPtr() && lt->isInt()) {
          e->ty = rt;   // int + ptr -> ptr
          return e->ty;
        }
        if (op == "-" && lt && rt && lt->isPtr() && rt->isInt()) {
          e->ty = lt;   // ptr - int -> ptr
          return e->ty;
        }
        if (op == "-" && lt && rt && lt->isPtr() && rt->isPtr()) {
          if (!typesEquivalent(lt, rt))
            return bad("pointer difference requires the same pointee type: " +
                       typeToString(lt) + " - " + typeToString(rt));
          e->ty = Type::simple(Type::I64);   // element distance
          return e->ty;
        }
        return bad("unsupported pointer operation '" + op + "' on " +
                   typeToString(lt) + " and " + typeToString(rt));
      }
      if (!lt || !rt) { e->ty = Type::simple(Type::U64); return e->ty; }
      // ---------------- integer-only family (report §3.3)
      if (isIntOnlyOp(op)) {
        if (!lt->isInt() || !rt->isInt())
          return bad("operator '" + op + "' needs integer operands: " +
                     typeToString(lt) + " " + op + " " + typeToString(rt) +
                     " (floats are not allowed here)");
      } else if (isArithOp(op)) {
        if (!lt->isNum() || !rt->isNum())
          return bad("operator '" + op + "' on non-numeric: " +
                     typeToString(lt) + " " + op + " " + typeToString(rt));
        if ((lt->isFloat() ^ rt->isFloat())) {
          // int/float mix: constant int adopts; runtime int is an error (strict)
          if (e->lhs->constant && e->lhs->k == Expr::INT) { /* adopt */ }
          else if (e->rhs->constant && e->rhs->k == Expr::INT) { /* adopt */ }
          else
            return bad("mixed int/float arithmetic requires explicit cast: " +
                       typeToString(lt) + " " + op + " " + typeToString(rt));
        }
      }
      Type* ct = commonType(lt, rt);
      // literal adoption for the RESULT type (documented §2 rule): a bare
      // INT literal adopts the other operand's integer type — signedness
      // AND width — so `i32var / 4` is 32-bit signed arithmetic, not 64-bit
      // unsigned. Without this, commonType(i32, u64) picked u64 by width and
      // every narrow signed division went unsigned.
      if (ct && ct->isInt() && lt->isInt() && rt->isInt()) {
        if (e->rhs->constant && e->rhs->k == Expr::INT) ct = lt;
        else if (e->lhs->constant && e->lhs->k == Expr::INT) ct = rt;
      }
      if (!ct) ct = lt;
      e->ty = ct;
      // constant folding
      if (e->lhs->constant && e->rhs->constant) e->constant = true;
      return e->ty;
    }
    case Expr::UN: {
      Type* rt = typeExpr(fs, e->rhs);
      if (e->op == "&") {
        // Phase 3b: &fn_name is the fn pointer itself (functions are not
        // lvalues, but taking their address is the classic C idiom for
        // callbacks — accepted alongside the bare-name form)
        if (e->rhs->k == Expr::IDENT && e->rhs->fnValue) {
          e->ty = e->rhs->ty;
          e->fnValue = e->rhs->fnValue;
          return e->ty;
        }
        // address-of lvalue -> *T
        if (!e->rhs->isLvalue)
          return bad("address-of needs an lvalue (variable, field, index)");
        if (!rt) return bad("address-of unknown type");
        e->ty = Type::ptr(rt);
        return e->ty;
      }
      if (e->op == "!") {
        if (rt && rt->k != Type::BOOL) return bad("! needs bool");
        e->ty = Type::simple(Type::BOOL);
        e->constant = e->rhs->constant;
        return e->ty;
      }
      if (e->op == "~") {
        if (rt && !rt->isInt()) return bad("~ needs int");
        e->ty = rt ? rt : Type::simple(Type::U64);
        return e->ty;
      }
      // unary -
      if (rt && !rt->isNum()) return bad("unary minus needs number");
      e->ty = rt ? rt : Type::simple(Type::U64);
      if (rt && rt->isInt() && !rt->isSigned() && !e->rhs->constant) {
        // unsigned negation is a logic error in strict mode
        diag(Diag::Error, Code::ArithNegUnsigned,
             "negation of unsigned value — declare as i64 or use 0 - x",
             e->loc.file, e->loc.line, e->loc.col);
      }
      e->constant = e->rhs->constant;
      return e->ty;
    }
    case Expr::INDEX: {
      Type* bt = typeExpr(fs, e->base);
      Type* it = typeExpr(fs, e->rhs);
      if (it && !it->isInt()) return bad("index must be integer");
      if (!bt) { e->ty = Type::simple(Type::U64); return e->ty; }
      if (bt->k == Type::SOA) {
        // soa(T)[i] — virtual struct ref; only .field use is meaningful
        e->soaIndex = true;
        e->ty = Type::structOf(bt->sdef);
        e->isLvalue = e->base->isLvalue;
        return e->ty;
      }
      // Phase 1b (report §4.1): fixed arrays index with a compile-time
      // known bound; the length is part of the type
      if (bt->k == Type::ARRAY) {
        // compile-time bounds check when the index is a constant
        if (e->rhs->constant && e->rhs->k == Expr::INT &&
            e->rhs->ival >= bt->arrLen) {
          diag(Diag::Error, Code::Bound,
               "constant index " + std::to_string(e->rhs->ival) +
               " is out of bounds for " + typeToString(bt) +
               " (valid: 0.." + std::to_string(bt->arrLen - 1) + ")",
               e->loc.file, e->loc.line, e->loc.col);
        }
        e->ty = bt->pointee;
        e->isLvalue = true;
        return e->ty;
      }
      // Phase 1c (report §4.1): Span<T> indexing is bounds-checked in
      // checked builds
      if (bt->k == Type::SPAN) {
        if (e->rhs->constant && e->rhs->k == Expr::INT &&
            e->rhs->ival >= (uint64_t)-1) { /* runtime len: no static bound */ }
        e->ty = bt->pointee;
        e->isLvalue = true;
        return e->ty;
      }
      // Phase 2b (report §4.5): str indexing yields the i-th BYTE (u8);
      // the view itself is immutable
      if (bt->k == Type::STR) {
        e->ty = Type::simple(Type::U8);
        e->isLvalue = false;
        return e->ty;
      }
      if (bt->k != Type::PTR) return bad("indexing non-pointer " + typeToString(bt));
      e->ty = bt->pointee;
      e->isLvalue = true;
      return e->ty;
    }
    case Expr::FIELD: {
      Type* bt = typeExpr(fs, e->base);
      if (!bt) { e->ty = Type::simple(Type::U64); return e->ty; }
      // .len / .ptr on the pair aggregates (report §4.1/§4.5)
      if (e->name == "len") {
        if (bt->k == Type::ARRAY) {
          e->ty = Type::simple(Type::U64);
          e->constant = true;
          return e->ty;   // the length is part of the type
        }
        if (bt->k == Type::SPAN || bt->k == Type::STR) {
          e->ty = Type::simple(Type::U64);
          return e->ty;   // runtime field read
        }
      }
      if (e->name == "ptr" && bt->k == Type::SPAN) {
        e->ty = Type::ptr(bt->pointee);
        return e->ty;   // raw pointer out of a view (FFI interop)
      }
      // auto-deref: p.f on *Struct means (*p).f
      if (bt->k == Type::PTR && bt->pointee && bt->pointee->k == Type::STRUCT) {
        e->ptrDeref = true;
        bt = bt->pointee;
      }
      if (bt->k != Type::STRUCT)
        return bad("field access on non-struct " + typeToString(bt));
      StructDef* sd = bt->sdef;
      Field* found = nullptr;
      for (auto& f : sd->fields) if (f.name == e->name) { found = &f; break; }
      if (!found) return bad("struct " + sd->name + " has no field " + e->name);
      e->fieldIdx = found->idx;
      e->ty = found->ty;
      e->isLvalue = true;   // through a pointer or an lvalue base
      return e->ty;
    }
    case Expr::CAST: {
      Type* bt = typeExpr(fs, e->base);
      Type* to = e->castTy;
      // inside a generic instantiation, `0 as T` carries an unbound
      // placeholder — rebind through the instance's substitution map
      if (to && to->k == Type::STRUCT && to->sdef == nullptr &&
          !e->castTyName.empty()) {
        if (Type* tp = tparamTypeInScope(e->castTyName)) {
          to = tp;
          e->castTy = tp;
        }
      }
      if (!to) { e->ty = bt; return e->ty; }
      if (to->k == Type::VOID)
        return bad("cannot cast to void");
      if (bt && bt->k == Type::BOOL)
        return bad("cast from bool — use 'x != 0' or 'x ? 1.0 : 0.0'");
      if (to->k == Type::BOOL)
        return bad("cast to bool — use 'x != 0'");
      if (to->k == Type::SOA || (bt && bt->k == Type::SOA))
        return bad("soa values cannot be cast");
      if (to->k == Type::PTR || (bt && bt->k == Type::PTR)) {
        // ptr <-> ptr allowed; ptr <-> num forbidden
        if (to->k != Type::PTR || (bt && bt->k != Type::PTR))
          return bad("pointer/number casts are forbidden — cast through usize with memcpy (strict memory model)");
      }
      if (bt && bt->k == Type::STRUCT)
        return bad("cannot cast struct values");
      // numeric cast: warn on narrowing
      if (bt && to && bt->isNum() && to->isNum() && to->bits() < bt->bits()) {
        diag(Diag::Warning, Code::CastNarrowing,
             "narrowing cast " + typeToString(bt) + " -> " + typeToString(to),
             e->loc.file, e->loc.line, e->loc.col);
      }
      e->ty = to;
      e->constant = e->base->constant;
      return e->ty;
    }
    // Phase 1b (report §4.1): fixed-array literal
    case Expr::ARRLIT: {
      if (e->args.empty()) {
        return bad("empty array literal needs a type annotation: "
                   "let a: [T; 0] is not a valid type — use a Span instead");
      }
      Type* et = typeExpr(fs, e->args[0]);
      for (size_t i = 1; i < e->args.size(); ++i) {
        Type* at = typeExpr(fs, e->args[i]);
        if (et && at && !typesCompatible(et, at) &&
            !(e->args[i]->constant && et->isNum() && at->isNum())) {
          diag(Diag::Error, Code::Type,
               "array literal element " + std::to_string(i + 1) + ": " +
               typeToString(at) + " does not match element type " +
               typeToString(et),
               e->loc.file, e->loc.line, e->loc.col);
        }
      }
      e->ty = Type::arrayOf(et ? et : Type::simple(Type::U64), e->args.size());
      return e->ty;
    }
    // Phase 1c/2b (report §4.1/§4.5): slicing base[lo..hi]
    case Expr::SLICE: {
      Type* bt = typeExpr(fs, e->base);
      if (e->lhs) typeExpr(fs, e->lhs);
      if (e->rhs) typeExpr(fs, e->rhs);
      if (e->lhs && !e->lhs->ty->isInt())
        return bad("slice start must be an integer");
      if (e->rhs && !e->rhs->ty->isInt())
        return bad("slice end must be an integer");
      if (!bt) { e->ty = Type::simple(Type::STR); return e->ty; }
      if (bt->k == Type::STR) {
        e->ty = Type::simple(Type::STR);
        return e->ty;   // str[a..b] -> str view
      }
      if (bt->k == Type::ARRAY || bt->k == Type::SPAN) {
        e->ty = Type::spanOf(bt->pointee);
        return e->ty;   // a[i..j] -> Span<T> view, no copy
      }
      if (bt->k == Type::PTR)
        return bad("cannot slice a raw pointer — build a Span first: "
                   "span(p, len)[i..j]");
      return bad("cannot slice " + typeToString(bt));
    }
    // Phase 2c (report §4.6): `expr?` — Option/Result propagation
    case Expr::TRYQ: {
      Type* bt = typeExpr(fs, e->base);
      if (!bt || bt->k != Type::ENUM) {
        return bad("`?` applies to Option<T> / Result<T, E> values, got " +
                   typeToString(bt));
      }
      EnumDef* ed = bt->edef;
      const std::string& en = ed->templateName.empty() ? ed->name
                                                       : ed->templateName;
      bool isOpt = (en == "Option");
      bool isRes = (en == "Result");
      if (!isOpt && !isRes) {
        return bad("`?` applies to Option<T> / Result<T, E>, not enum " + en);
      }
      // error variant: the payload-less (Option) / second (Result) variant
      int errIdx = -1;
      if (isOpt) {
        for (auto& v : ed->variants)
          if (v.name == "None") errIdx = v.idx;
      } else {
        for (auto& v : ed->variants)
          if (v.name == "Err") errIdx = v.idx;
      }
      if (errIdx < 0) return bad("corrupted prelude enum " + en);
      e->variantIdx = errIdx;
      e->enumRef = ed;
      // ok variant carries the payload T
      int okIdx = -1;
      for (auto& v : ed->variants)
        if (isOpt ? v.name == "Some" : v.name == "Ok") okIdx = v.idx;
      if (okIdx < 0 || ed->variants[okIdx].fields.empty())
        return bad("corrupted prelude enum " + en);
      Type* payload = ed->variants[okIdx].fields[0].ty;
      // the enclosing function must return a compatible Option/Result
      Type* rt = fs && fs->fn ? fs->fn->ret : nullptr;
      if (!rt || rt->k != Type::ENUM) {
        return bad("`?` can only be used in a function returning Option or "
                   "Result" +
                   (fs && fs->fn ? " (fn " + fs->fn->name + " returns " +
                    typeToString(rt) + ")" : ""));
      }
      const std::string& ren = rt->edef->templateName.empty()
                               ? rt->edef->name : rt->edef->templateName;
      if (isOpt && ren != "Option") {
        return bad("`?` on Option<T> requires the fn to return Option<U>, "
                   "but it returns " + typeToString(rt));
      }
      if (isRes && ren != "Result") {
        return bad("`?` on Result<T, E> requires the fn to return "
                   "Result<U, F>, but it returns " + typeToString(rt));
      }
      if (isRes) {
        // error payloads must match
        Type* fnErr = nullptr;
        for (auto& v : rt->edef->variants)
          if (v.name == "Err" && !v.fields.empty()) fnErr = v.fields[0].ty;
        Type* valErr = ed->variants[errIdx].fields.empty()
                       ? nullptr : ed->variants[errIdx].fields[0].ty;
        if (fnErr && valErr && !typesEquivalent(fnErr, valErr) &&
            !typesCompatible(fnErr, valErr)) {
          diag(Diag::Error, Code::Type,
               "`?` propagation error type mismatch: value carries " +
               typeToString(valErr) + ", fn " + fs->fn->name +
               " returns " + typeToString(rt),
               e->loc.file, e->loc.line, e->loc.col);
        }
      }
      e->ty = payload;
      return e->ty;
    }
  }
  e->ty = Type::simple(Type::U64);
  return e->ty;
}

} // namespace freshet
