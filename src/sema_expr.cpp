// Freshet expression typing — builtins, intrinsics, struct literals, soa
// indexing, casts. Literal type adoption rules.
#include "common.h"

namespace freshet {

static std::string numericKindDescription(Type* ty) {
  if (ty->isFloat()) return "floating-point";
  return ty->isSigned() ? "signed integer" : "unsigned integer";
}

static std::string integerValueRange(Type* ty) {
  const int bits = ty->bits();
  if (ty->isSigned()) {
    if (bits >= 64) return "-9223372036854775808..9223372036854775807";
    const long long limit = 1LL << (bits - 1);
    return "-" + std::to_string(limit) + ".." + std::to_string(limit - 1);
  }
  if (bits >= 64) return "0..18446744073709551615";
  const uint64_t max = (uint64_t(1) << bits) - 1;
  return "0.." + std::to_string(max);
}

static std::string narrowingCastMessage(Type* from, Type* to) {
  const std::string fromName = typeToString(from);
  const std::string toName = typeToString(to);
  const std::string fromDesc = fromName + " (" + std::to_string(from->bits()) +
      "-bit " + numericKindDescription(from) + ")";
  const std::string toDesc = toName + " (" + std::to_string(to->bits()) +
      "-bit " + numericKindDescription(to) + ")";

  if (from->isInt() && to->isInt()) {
    const std::string range = integerValueRange(to);
    std::string behaviour;
    if (to->isSigned()) {
      behaviour = "Values outside that range are truncated to the low " +
          std::to_string(to->bits()) + " bits and may change sign.";
    } else {
      behaviour = "Values outside that range are truncated to the low " +
          std::to_string(to->bits()) + " bits (equivalent to wrapping modulo " +
          (to->bits() >= 64 ? std::string("2^64") :
             std::to_string(uint64_t(1) << to->bits())) + ").";
    }
    if (from->isSigned() && !to->isSigned())
      behaviour += " Negative source values also become large unsigned values.";
    return "Narrowing cast from " + fromDesc + " to " + toDesc + ". " +
        toName + " can represent " + range + ". " + behaviour +
        " Check the value against this range before casting, or retain the " +
        "wider type if truncation is not intentional.";
  }

  if (from->isFloat() && to->isInt()) {
    return "Narrowing cast from " + fromDesc + " to " + toDesc + ". " +
        toName + " can represent " + integerValueRange(to) +
        "; the fractional part is discarded (truncated towards zero). " +
        "A value outside the destination range has no valid integer result. " +
        "Check the range before casting, and round explicitly if that is " +
        "what you intend.";
  }

  if (from->isInt() && to->isFloat()) {
    const std::string exactLimit = to->k == Type::F32
        ? "16777216 (2^24)" : "9007199254740992 (2^53)";
    return "Narrowing cast from " + fromDesc + " to " + toDesc +
        " may round integer values because the destination cannot represent "
        "every source integer exactly. " + toName + " represents every integer "
        "exactly only up to " + exactLimit + "; larger values can lose low "
        "bits. Keep the integer type for exact arithmetic, or use a wider "
        "floating-point type if approximation is acceptable.";
  }

  if (from->isFloat() && to->isFloat()) {
    return "Narrowing cast from " + fromDesc + " to " + toDesc +
        " reduces precision from binary64 to binary32 (about 7 significant "
        "decimal digits) and reduces the finite range. Values may round or "
        "overflow; keep f64 when that precision/range matters.";
  }

  return "Narrowing cast from " + fromDesc + " to " + toDesc +
      " may lose value information. Check that the value is representable "
      "in the destination type before casting.";
}

static bool isBuiltinCall(const std::string& n) {
  static const std::set<std::string> b = {
    "malloc", "free", "give", "stack_alloc",
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
    // fsht-02:01 (registry 02.09.01): dynamic collections.  Vec<T> is a
    // prelude generic struct; the API is free functions in the String
    // style — &v for mutation, plain v for reads, Option<T> for
    // fallible access.
    "vec_new", "vec_from_span", "vec_from_array", "vec_clone",
    "vec_push", "vec_pop", "vec_get", "vec_insert", "vec_remove",
    "vec_reserve", "vec_truncate", "vec_resize", "vec_clear",
    "vec_free", "vec_len", "vec_capacity", "vec_is_empty",
    "vec_slice",
    // fsht-02:03 (registry 06.03.01): compile-time assertion — the
    // argument must const-evaluate (const fn calls included) to true,
    // else the compile fails (F0219/F0220)
    "const_assert",
    // fsht-03:01 (registry 08.01.04, ch. 23 §2–§3): checked/saturating/
    // wrapping operation variants — overflow policy written, not
    // assumed.  Identical-operand-type rule enforced in typeBuiltin.
    "checked_add", "checked_sub", "checked_mul", "checked_div",
    "checked_rem",
    "saturating_add", "saturating_sub", "saturating_mul",
    "wrapping_add", "wrapping_sub", "wrapping_mul",
    "int_min", "int_max",
    "checked_cast", "saturating_cast",
    // fsht-03:01 (registry 05.04.01, ch. 23 §1): numeric text parsing —
    // the fmt_* counterpart, Result<T, ParseError> out
    "parse_i64", "parse_u64", "parse_f64",
    // fsht-03:01 (ch. 23 §5): numeric module minimum content
    "abs", "gcd", "clamp",
    // fsht-03:01 (ch. 23 §5): bits module — byte swap + population count
    "bswap16", "bswap32", "bswap64", "popcount",
    // fsht-03:01 (ch. 23 §5): time module — monotonic, unspecified epoch
    "now_ns", "now_ms",
    // fsht-03:01 (ch. 23 §5): random module — xorshift64* (shifts
    // 12/25/27, multiplier 2685821657736338717), the same generator the
    // conformance fuzz/benchmark harnesses use
    "sm64_next",
    // fsht-03:01 (ch. 23 §5): alloc module
    "page_size",
    // fsht-03:02 (registry 02.06.01 update, ch. 24 §4): the String
    // migration's new builtins — deep copy and clear
    "string_clone", "string_clear",
  };
  return b.count(n) > 0;
}

// fsht-03:01 (registry 08.01.04, ch. 23 §3): set a
// checked_cast/saturating_cast call's destination type U from a context
// type — an Option<U> context unwraps its Some payload, a plain numeric
// context IS U.  Called from the binding/return/argument/assignment
// sites BEFORE the init expression is typed, so typeBuiltin sees the
// resolved U.  Unresolved U at typing time is F0201.
void trySetCastTarget(FnSema* fs, Expr* e, Type* expected) {
  (void)fs;
  if (!e || !expected) return;
  bool isCast = e->k == Expr::CALL &&
                (e->name == "checked_cast" || e->name == "saturating_cast");
  if (!isCast || e->castTargetTy) return;
  Type* u = expected;
  if (expected->k == Type::ENUM) {
    const std::string& en = expected->edef->templateName.empty()
                            ? expected->edef->name
                            : expected->edef->templateName;
    if (en == "Option") {
      for (auto& v : expected->edef->variants)
        if (v.name == "Some" && !v.fields.empty()) u = v.fields[0].ty;
    } else {
      return;   // other enums are not a U context
    }
  }
  if (!u || !u->isNum()) return;
  e->castTargetTy = u;
}

static Type* typeFromIdent(const std::string& n) {
  // generic instantiation scope: `malloc(T, n)` inside fn f<T>(...)
  if (Type* tp = tparamTypeInScope(n)) return tp;
  // fsht-03:02 (ch. 24 §5): a TYPE NAME in expression position carries
  // its generic instantiation ENCODING (Vec<Vec<i64>> element types) —
  // resolve through the sema binder
  if (n.rfind("gen:", 0) == 0 || n.find(':') != std::string::npos)
    return bindTypeEncoding(currentProgram(), n, SrcLoc(), nullptr);
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
           e->loc);
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
           e->loc);
      return Type::ptr(Type::simple(Type::U8));
    }
    if (elem->k == Type::VOID || elem->k == Type::SOA) {
      diag(Diag::Error, Code::Args, "cannot malloc " + typeToString(elem),
           e->loc);
    }
    if (!argT(1) || !argT(1)->isInt()) {
      diag(Diag::Error, Code::Args, "malloc count must be integer",
           e->loc);
    }
    e->mallocElem = elem;
    return Type::ptr(elem);
  }
  if (n == "stack_alloc") {
    // data stacks (goose port): stack_alloc(T, n) — like malloc's first
    // arg, a type name; the allocation dies with the binding's block
    // scope (watermark restore), never free()d
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
      diag(Diag::Error, Code::Args,
           "stack_alloc's first argument must be a type", e->loc);
      return Type::ptr(Type::simple(Type::U8));
    }
    if (elem->k == Type::VOID || elem->k == Type::SOA) {
      diag(Diag::Error, Code::Args,
           "cannot stack_alloc " + typeToString(elem), e->loc);
    }
    if (!argT(1) || !argT(1)->isInt()) {
      diag(Diag::Error, Code::Args,
           "stack_alloc count must be integer", e->loc);
    }
    e->mallocElem = elem;
    return Type::ptr(elem);
  }
  if (n == "free") {
    if (!need(1)) return Type::simple(Type::VOID);
    if (argT(0) && !argT(0)->isPtr()) {
      diag(Diag::Error, Code::Args, "free expects a pointer",
           e->loc);
    }
    return Type::simple(Type::VOID);
  }
  if (n == "give") {
    if (!need(1)) return Type::simple(Type::VOID);
    return argT(0);   // give(p) is an ownership marker; meaningful in call arguments
  }
  if (n == "print") {
    if (!need(1)) return Type::simple(Type::VOID);
    // Phase 2b (report §4.5): print accepts any str value — literals AND
    // views/slices (the historical checker required a literal, which is
    // what made strings a print-only facility)
    if (e->args[0]->k != Expr::STR &&
        !(e->args[0]->ty && e->args[0]->ty->k == Type::STR)) {
      diag(Diag::Error, Code::Args, "print expects a str value",
           e->loc);
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
           e->loc);
      return Type::simple(Type::VOID);
    }
    if (!argT(1) || !argT(1)->isInt()) {
      diag(Diag::Error, Code::Args, "soa_new count must be integer",
           e->loc);
    }
    e->mallocElem = Type::structOf(sd);
    return Type::soaOf(sd);
  }
  if (n == "soa_free") {
    if (!need(1)) return Type::simple(Type::VOID);
    if (argT(0) && argT(0)->k != Type::SOA) {
      diag(Diag::Error, Code::Args, "soa_free expects a soa() value",
           e->loc);
    }
    return Type::simple(Type::VOID);
  }
  if (n == "soa_len") {
    if (!need(1)) return Type::simple(Type::USIZE);
    if (argT(0) && argT(0)->k != Type::SOA) {
      diag(Diag::Error, Code::Args, "soa_len expects a soa() value",
           e->loc);
    }
    return Type::simple(Type::USIZE);
  }
  if (n == "soa_field_ptr") {
    // soa_field_ptr(s, "x") -> *f64  (for raw interop)
    need(2);
    if (argT(0)) {
      if (argT(0)->k != Type::SOA) {
        diag(Diag::Error, Code::Args, "soa_field_ptr expects soa() first arg",
             e->loc);
        return Type::ptr(Type::simple(Type::U8));
      }
      StructDef* sd = argT(0)->sdef;
      if (e->args[1]->k == Expr::STR) {
        for (auto& f : sd->fields)
          if (f.name == e->args[1]->sval) return Type::ptr(f.ty);
        diag(Diag::Error, Code::Args, "no field " + e->args[1]->sval +
             " in struct " + sd->name, e->loc);
      }
      return Type::ptr(Type::simple(Type::U8));
    }
    return Type::ptr(Type::simple(Type::U8));
  }
  if (n == "assert") {
    need(1);
    return Type::simple(Type::VOID);
  }
  // fsht-02:03 (registry 06.03.01): compile-time assertion.  The
  // argument is interpreted by the const evaluator (const fn calls,
  // literals, comparisons and arithmetic over both); a failure to
  // evaluate is F0219, a false result is F0220 — the compile stops
  // either way, so no runtime code is generated for the call.
  if (n == "const_assert") {
    if (!need(1)) return Type::simple(Type::VOID);
    uint64_t v = 0;
    if (!evalConstExpr(e->args[0], {}, &v, e->loc)) {
      diag(Diag::Error, Code::ConstEval,
           "const_assert argument is not const-evaluable — build it from "
           "integer literals, arithmetic, comparisons and const fn calls",
           e->loc);
    } else if (v == 0) {
      diag(Diag::Error, Code::ConstAssert,
           "const_assert failed: the expression evaluated to false at "
           "compile time", e->loc);
    }
    return Type::simple(Type::VOID);
  }
  // ---------------- Phase 1c (report §4.1): span construction
  if (n == "span") {
    // span(p: *T, n) -> Span<T>   |   span(a: [T; N]) -> Span<T>
    if (e->args.empty()) {
      diag(Diag::Error, Code::Args, "span expects a pointer + length or an array",
           e->loc);
      return Type::spanOf(Type::simple(Type::U8));
    }
    Type* a0 = argT(0);
    if (e->args.size() == 1) {
      if (a0 && a0->k == Type::ARRAY)
        return Type::spanOf(a0->pointee);
      diag(Diag::Error, Code::Args,
           "span(a) expects a fixed array [T; N]; for pointers use span(p, len)",
           e->loc);
      return Type::spanOf(Type::simple(Type::U8));
    }
    if (e->args.size() != 2) {
      diag(Diag::Error, Code::Args, "span expects (pointer, length)",
           e->loc);
      return Type::spanOf(Type::simple(Type::U8));
    }
    if (!a0 || !a0->isPtr()) {
      diag(Diag::Error, Code::Args, "span's first argument must be a pointer",
           e->loc);
      return Type::spanOf(Type::simple(Type::U8));
    }
    if (a0->pointee && (a0->pointee->k == Type::VOID)) {
      diag(Diag::Error, Code::Args,
           "cannot build a Span<void>; cast the pointer to *T first",
           e->loc);
    }
    if (argT(1) && !argT(1)->isInt()) {
      diag(Diag::Error, Code::Args, "span length must be an integer",
           e->loc);
    }
    return Type::spanOf(a0->pointee);
  }
  // ---------------- Phase 2b (report §4.5): strings
  if (n == "str_eq") {
    need(2);
    for (int i = 0; i < 2; ++i)
      if (argT(i) && argT(i)->k != Type::STR)
        diag(Diag::Error, Code::Args, "str_eq expects two str values",
             e->loc);
    return Type::simple(Type::BOOL);
  }
  if (n == "str_cat" || n == "string_from") {
    if (n == "str_cat") {
      if ((int)e->args.size() != 2) {
        diag(Diag::Error, Code::Args, "str_cat expects two str values",
             e->loc);
      }
      for (int i = 0; i < (int)e->args.size(); ++i)
        if (argT(i) && argT(i)->k != Type::STR)
          diag(Diag::Error, Code::Args, "str_cat expects str arguments",
               e->loc);
    } else {
      need(1);
      if (argT(0) && argT(0)->k != Type::STR)
        diag(Diag::Error, Code::Args, "string_from expects a str value",
             e->loc);
    }
    return Type::structOf(ensurePreludeString(currentProgram()));
  }
  if (n == "str_to_cstr") {
    need(1);
    if (argT(0) && argT(0)->k != Type::STR)
      diag(Diag::Error, Code::Args, "str_to_cstr expects a str value",
           e->loc);
    return Type::ptr(Type::simple(Type::U8));
  }
  if (n == "string_new" || n == "fmt_f64" || n == "fmt_i64") {
    if (n == "string_new") need(0);
    else if (n == "fmt_f64") {
      need(2);
      if (argT(0) && !argT(0)->isFloat())
        diag(Diag::Error, Code::Args, "fmt_f64 expects (f64, digits)",
             e->loc);
      if (argT(1) && !argT(1)->isInt())
        diag(Diag::Error, Code::Args, "fmt_f64 digit count must be an integer",
             e->loc);
    } else {
      need(1);
      if (argT(0) && !argT(0)->isInt())
        diag(Diag::Error, Code::Args, "fmt_i64 expects an integer",
             e->loc);
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
           e->loc);
    if (argT(1) && argT(1)->k != Type::STR)
      diag(Diag::Error, Code::Args, "string_append's second argument must be str",
           e->loc);
    return Type::simple(Type::VOID);
  }
  if (n == "string_free") {
    need(1);
    if (argT(0) && !(argT(0)->k == Type::PTR &&
                     argT(0)->pointee && argT(0)->pointee->k == Type::STRUCT &&
                     argT(0)->pointee->sdef == ensurePreludeString(currentProgram())))
      diag(Diag::Error, Code::Args, "string_free expects a *String",
           e->loc);
    return Type::simple(Type::VOID);
  }
  if (n == "str_of") {
    need(1);
    if (argT(0) && !(argT(0)->k == Type::PTR &&
                     argT(0)->pointee && argT(0)->pointee->k == Type::STRUCT &&
                     argT(0)->pointee->sdef == ensurePreludeString(currentProgram())))
      diag(Diag::Error, Code::Args, "str_of expects a *String",
           e->loc);
    return Type::simple(Type::STR);
  }

  // ---------------- fsht-02:01 (registry 02.09.01): dynamic collections
  //
  // Vec<T> is a prelude generic struct; the builtin surface mirrors the
  // String API style: &v for mutation, plain v for reads, Option<T> for
  // fallible access, Span<T> for borrowed views (slicing copies nothing).
  // All of these BORROW the container — ownership transfer happens only
  // through by-value parameter passing, return, and give().
  auto asVecArg = [&](int i) -> Type* {
    Type* t = argT(i);
    if (t && t->k == Type::STRUCT && containerElemType(t)) return t;
    return nullptr;
  };
  auto vecRefArg = [&](const char* what, int i) -> Type* {
    // (&v) form: *Vec<T>
    Type* t = argT(i);
    if (t && t->k == Type::PTR && t->pointee && t->pointee->k == Type::STRUCT &&
        containerElemType(t->pointee))
      return t->pointee;
    diag(Diag::Error, Code::Args,
         std::string(what) + " expects (&mut Vec<T>, ...) — take the address with &v",
         e->loc);
    return nullptr;
  };
  auto needMutBinding = [&](int i) -> bool {
    Expr* a = i < (int)e->args.size() ? e->args[i] : nullptr;
    if (a && a->k == Expr::UN && a->op == "&" && a->rhs &&
        a->rhs->k == Expr::IDENT && a->rhs->sym && !a->rhs->sym->mut) {
      diag(Diag::Error, Code::Args,
           "'" + a->rhs->name + "' must be a `let mut` binding to mutate it",
           e->loc);
      return false;
    }
    return true;
  };
  auto optOf = [&](Type* elem) -> Type* {
    if (!elem) return Type::simple(Type::VOID);
    ensurePreludeEnumType("Option");
    return Type::enumOf(instantiateEnum(currentProgram(),
                                        findEnumDef("Option"), {elem}, e->loc));
  };
  // constant literal adoption (the user-call rule): vec_push(&v, 5) with a
  // Vec<i64> adopts the element type for the literal instead of erroring
  auto adoptConst = [&](int i, Type* elem) {
    Expr* a = i < (int)e->args.size() ? e->args[i] : nullptr;
    if (a && elem && a->ty && a->constant && a->ty->isNum() && elem->isNum() &&
        a->ty->k != elem->k)
      a->ty = elem;
  };
  if (n == "vec_new") {
    // vec_new(T) or vec_new(T, cap) — element type first (malloc
    // precedent); capacity 0 means "first push allocates"
    if (e->args.size() != 1 && e->args.size() != 2) {
      diag(Diag::Error, Code::Args,
           "vec_new expects (T) or (T, cap), got " +
           std::to_string(e->args.size()) + " args",
           e->loc);
      return Type::simple(Type::VOID);
    }
    Type* elem = nullptr;
    Expr* tArg = e->args[0];
    if (tArg->k == Expr::IDENT) elem = typeFromIdent(tArg->name);
    // the String prelude registers lazily; resolve it so the owning-element
    // gate below reports the real reason instead of "not a type"
    if (!elem && tArg->k == Expr::IDENT && tArg->name == "String") {
      ensurePreludeString(currentProgram());
      if (StructDef* sd = findStructDef("String")) elem = Type::structOf(sd);
    }
    if (!elem) {
      diag(Diag::Error, Code::Args,
           "vec_new's first argument must be a type", e->loc);
      return Type::simple(Type::VOID);
    }
    if (e->args.size() == 2 && (!argT(1) || !argT(1)->isInt())) {
      diag(Diag::Error, Code::Args,
           "vec_new capacity must be an integer", e->loc);
    }
    return Type::structOf(instantiateVecType(currentProgram(), elem, e->loc));
  }
  if (n == "vec_from_span" || n == "vec_from_array" || n == "vec_clone") {
    Type* elem = nullptr;
    if (n == "vec_from_span") {
      need(1);
      if (argT(0) && argT(0)->k == Type::SPAN) elem = argT(0)->pointee;
      else diag(Diag::Error, Code::Args,
               "vec_from_span expects a Span<T>", e->loc);
    } else if (n == "vec_from_array") {
      need(1);
      if (argT(0) && argT(0)->k == Type::ARRAY) elem = argT(0)->pointee;
      else diag(Diag::Error, Code::Args,
               "vec_from_array expects a [T; N]", e->loc);
    } else {
      need(1);
      Type* v = asVecArg(0);
      if (v) elem = containerElemType(v);
      else diag(Diag::Error, Code::Args,
               "vec_clone expects a Vec<T>", e->loc);
    }
    if (!elem) return Type::simple(Type::VOID);
    return Type::structOf(instantiateVecType(currentProgram(), elem, e->loc));
  }
  if (n == "vec_push") {
    need(2);
    Type* v = vecRefArg("vec_push", 0);
    needMutBinding(0);
    Type* elem = v ? containerElemType(v) : nullptr;
    adoptConst(1, elem);
    if (elem && argT(1) && !typesEquivalent(argT(1), elem)) {
      diag(Diag::Error, Code::Args,
           "vec_push element must be " + typeToString(elem) + ", got " +
           typeToString(argT(1)), e->loc);
    }
    return Type::simple(Type::VOID);
  }
  if (n == "vec_pop" || n == "vec_remove") {
    // mutating fallible access: Option<T> out through &mut v
    need(n == "vec_pop" ? 1 : 2);
    Type* v = vecRefArg(n.c_str(), 0);
    needMutBinding(0);
    Type* elem = v ? containerElemType(v) : nullptr;
    if (n == "vec_remove" && argT(1) && !argT(1)->isInt())
      diag(Diag::Error, Code::Args,
           "vec_remove index must be an integer", e->loc);
    return optOf(elem);
  }
  if (n == "vec_get") {
    // pure read: the container argument is borrowed by value
    need(2);
    Type* v = asVecArg(0);
    Type* elem = v ? containerElemType(v) : nullptr;
    if (!v)
      diag(Diag::Error, Code::Args,
           "vec_get expects a Vec<T> by value", e->loc);
    if (argT(1) && !argT(1)->isInt())
      diag(Diag::Error, Code::Args,
           "vec_get index must be an integer", e->loc);
    return optOf(elem);
  }
  if (n == "vec_insert") {
    need(3);
    Type* v = vecRefArg("vec_insert", 0);
    needMutBinding(0);
    Type* elem = v ? containerElemType(v) : nullptr;
    adoptConst(2, elem);
    if (argT(1) && !argT(1)->isInt())
      diag(Diag::Error, Code::Args,
           "vec_insert index must be an integer", e->loc);
    if (elem && argT(2) && !typesEquivalent(argT(2), elem)) {
      diag(Diag::Error, Code::Args,
           "vec_insert element must be " + typeToString(elem) + ", got " +
           typeToString(argT(2)), e->loc);
    }
    return Type::simple(Type::VOID);
  }
  if (n == "vec_reserve" || n == "vec_truncate") {
    need(2);
    vecRefArg(n.c_str(), 0);
    needMutBinding(0);
    if (argT(1) && !argT(1)->isInt())
      diag(Diag::Error, Code::Args,
           std::string(n) + " count must be an integer", e->loc);
    return Type::simple(Type::VOID);
  }
  if (n == "vec_resize") {
    need(3);
    Type* v = vecRefArg("vec_resize", 0);
    needMutBinding(0);
    Type* elem = v ? containerElemType(v) : nullptr;
    adoptConst(2, elem);
    if (argT(1) && !argT(1)->isInt())
      diag(Diag::Error, Code::Args,
           "vec_resize count must be an integer", e->loc);
    if (elem && argT(2) && !typesEquivalent(argT(2), elem)) {
      diag(Diag::Error, Code::Args,
           "vec_resize fill must be " + typeToString(elem) + ", got " +
           typeToString(argT(2)), e->loc);
    }
    return Type::simple(Type::VOID);
  }
  if (n == "vec_clear" || n == "vec_free") {
    need(1);
    vecRefArg(n.c_str(), 0);
    needMutBinding(0);
    return Type::simple(Type::VOID);
  }
  if (n == "vec_len" || n == "vec_capacity" || n == "vec_is_empty") {
    need(1);
    if (!asVecArg(0))
      diag(Diag::Error, Code::Args,
           std::string(n) + " expects a Vec<T> by value", e->loc);
    return n == "vec_is_empty" ? Type::simple(Type::BOOL)
                               : Type::simple(Type::USIZE);
  }
  if (n == "vec_slice") {
    need(3);
    Type* v = asVecArg(0);
    Type* elem = v ? containerElemType(v) : nullptr;
    if (!v)
      diag(Diag::Error, Code::Args,
           "vec_slice expects (Vec<T>, lo, hi)", e->loc);
    if (argT(1) && !argT(1)->isInt())
      diag(Diag::Error, Code::Args,
           "vec_slice start must be an integer", e->loc);
    if (argT(2) && !argT(2)->isInt())
      diag(Diag::Error, Code::Args,
           "vec_slice end must be an integer", e->loc);
    if (!elem) return Type::spanOf(Type::simple(Type::U8));
    return Type::spanOf(elem);
  }
  // -------------- fsht-03:01 (registry 08.01.04, ch. 23 §2–§3) ----------
  // The checked/saturating/wrapping variant family: identical integer
  // operand types (precision-explicit, no mixed-width promotion, no
  // float operands — F0201), a bare INT literal adopts the other
  // operand's type (chapter 14's literal rule).  checked_div/rem
  // return None exactly where the plain operators trap.
  auto intVariantType = [&](const char* family) -> Type* {
    Type* a = argT(0);
    Type* b = argT(1);
    if (a && b && a->isInt() && b->isInt() && a->k != b->k) {
      // literal adoption (only constant INT literals retype)
      auto adopt = [&](int i, Type* other) {
        e->args[i]->ty = other;
        if (i == 0) a = other; else b = other;
      };
      if (e->args[0]->constant && e->args[0]->k == Expr::INT) adopt(0, b);
      else if (e->args[1]->constant && e->args[1]->k == Expr::INT) adopt(1, a);
    }
    if (!a || !b || !a->isInt() || !b->isInt()) {
      diag(Diag::Error, Code::Type,
           std::string(family) + " takes two INTEGER operands (floating-point "
           "overflow is defined IEEE behaviour and needs no variants)",
           e->loc);
      return Type::simple(Type::I64);
    }
    if (a->k != b->k) {
      diag(Diag::Error, Code::Type,
           std::string(family) + " requires the IDENTICAL integer type on "
           "both operands — the variant family is precision-explicit and "
           "performs no mixed-width promotion (" + typeToString(a) + " vs " +
           typeToString(b) + "); cast explicitly or widen both",
           e->loc);
      return Type::simple(a->bits() >= b->bits() ? a->k : b->k);
    }
    return a;
  };
  if (n == "checked_add" || n == "checked_sub" || n == "checked_mul" ||
      n == "checked_div" || n == "checked_rem") {
    if (!need(2)) return Type::simple(Type::I64);
    Type* t = intVariantType(n.c_str());
    return optOf(t);
  }
  if (n == "saturating_add" || n == "saturating_sub" || n == "saturating_mul" ||
      n == "wrapping_add" || n == "wrapping_sub" || n == "wrapping_mul") {
    if (!need(2)) return Type::simple(Type::I64);
    return intVariantType(n.c_str());
  }
  if (n == "int_min" || n == "int_max") {
    // the argument only fixes the instantiation T — its value is unused
    if (!need(1)) return Type::simple(Type::I64);
    Type* t = argT(0);
    if (!t || !t->isInt()) {
      diag(Diag::Error, Code::Type,
           n + " takes one INTEGER value (the seed fixes T; the value is "
           "not used)", e->loc);
      return Type::simple(Type::I64);
    }
    int bits = t->bits();
    uint64_t v;
    if (n == "int_min") {
      // signed min: 1 << (bits-1) in two's complement; unsigned: 0
      v = t->isSigned() ? ((bits == 64) ? 0x8000000000000000ULL
                                        : (1ULL << (bits - 1)))
                        : 0ULL;
    } else {
      v = t->isSigned() ? ((bits == 64) ? 0x7FFFFFFFFFFFFFFFULL
                                        : ((1ULL << (bits - 1)) - 1ULL))
                        : ((bits == 64) ? 0xFFFFFFFFFFFFFFFFULL
                                       : ((1ULL << bits) - 1ULL));
    }
    // fold: the call node becomes a typed constant literal
    e->k = Expr::INT;
    e->ival = v;
    e->constant = true;
    e->args.clear();
    e->argGive.clear();
    return t;
  }
  if (n == "checked_cast" || n == "saturating_cast") {
    if (!need(1)) return Type::simple(Type::I64);
    Type* at = argT(0);
    Type* u = e->castTargetTy;
    if (!u) {
      diag(Diag::Error, Code::Type,
           std::string(n) + " cannot infer the destination type U — it is "
           "inferred from the binding's type, an Option<U>/U-typed "
           "context, or a match arm (e.g. `let x: Option<i8> = " + n +
           "(v);`)", e->loc);
      return Type::simple(Type::I64);
    }
    if (!at || !at->isNum() || !u->isNum()) {
      diag(Diag::Error, Code::Type,
           std::string(n) + " converts numeric values (got " +
           typeToString(at) + " to " + typeToString(u) + ")", e->loc);
      return u;
    }
    // in-scope conversions: int->int and float->int.  int->float and
    // float->float are exactness-invisible and use `as` (ch. 23 §3)
    if ((at->isFloat() && u->isFloat()) || (at->isInt() && u->isFloat())) {
      diag(Diag::Error, Code::Type,
           std::string(n) + " covers the lossy int->int and float->int "
           "conversions only — " + typeToString(at) + " to " +
           typeToString(u) + " uses the plain `as` cast (its documented "
           "rounding)", e->loc);
      return u;
    }
    return n == "checked_cast" ? optOf(u) : u;
  }
  // -------------- fsht-03:01 (registry 05.04.01, ch. 23 §1) -------------
  // parse_i64/u64/f64: str in, Result<T, ParseError> out
  if (n == "parse_i64" || n == "parse_u64" || n == "parse_f64") {
    if (!need(1)) return Type::simple(Type::I64);
    if (argT(0) && argT(0)->k != Type::STR) {
      diag(Diag::Error, Code::Args, n + " expects a str value", e->loc);
    }
    Type* t = n == "parse_i64" ? Type::simple(Type::I64)
            : n == "parse_u64" ? Type::simple(Type::USIZE)
                               : Type::simple(Type::F64);
    Program* prog = currentProgram();
    EnumDef* pe = ensurePreludeParseError(prog);
    ensurePreludeEnumType("Result");
    EnumDef* res = findEnumDef("Result");
    return Type::enumOf(instantiateEnum(prog, res, {t, Type::enumOf(pe)},
                                         e->loc));
  }
  // -------------- fsht-03:01 (ch. 23 §5): numeric / bits / time / random /
  // alloc module minimum content, usable without import like §1–§3
  if (n == "abs") {
    if (!need(1)) return Type::simple(Type::I64);
    Type* t = argT(0);
    if (!t || !t->isNum())
      diag(Diag::Error, Code::Args, "abs expects a numeric argument", e->loc);
    return t ? t : Type::simple(Type::I64);
  }
  if (n == "gcd") {
    if (!need(2)) return Type::simple(Type::I64);
    Type* t = intVariantType("gcd");
    return t;
  }
  if (n == "clamp") {
    if (!need(3)) return Type::simple(Type::I64);
    Type* t = argT(0);
    for (int i = 0; i < 3; ++i) {
      Type* ai = argT(i);
      if (!ai || !ai->isNum()) {
        diag(Diag::Error, Code::Args, "clamp takes three numeric values",
             e->loc);
        return Type::simple(Type::I64);
      }
    }
    return t;
  }
  if (n == "bswap16" || n == "bswap32" || n == "bswap64") {
    if (!need(1)) return Type::simple(Type::I64);
    int want = n == "bswap16" ? 16 : (n == "bswap32" ? 32 : 64);
    Type* t = argT(0);
    if (!t || !t->isInt() || t->bits() != want) {
      diag(Diag::Error, Code::Args,
           n + " expects a " + std::to_string(want) + "-bit integer (got " +
           typeToString(t) + ")", e->loc);
      return Type::simple(Type::I64);
    }
    return t;
  }
  if (n == "popcount") {
    if (!need(1)) return Type::simple(Type::I64);
    Type* t = argT(0);
    if (!t || !t->isInt())
      diag(Diag::Error, Code::Args, "popcount expects an integer", e->loc);
    return t ? t : Type::simple(Type::I64);
  }
  if (n == "now_ns" || n == "now_ms" || n == "page_size") {
    need(0);
    return Type::simple(Type::U64);
  }
  if (n == "sm64_next") {
    if (!need(1)) return Type::simple(Type::U64);
    Type* t = argT(0);
    if (!t || t->k != Type::PTR || !t->pointee ||
        t->pointee->k != Type::USIZE) {
      diag(Diag::Error, Code::Args,
           "sm64_next expects (&mut state: u64) — take the address with "
           "&mut", e->loc);
    }
    needMutBinding(0);
    return Type::simple(Type::U64);
  }
  // -------------- fsht-03:02 (registry 02.06.01, ch. 24 §4) -------------
  // the String migration's new builtins
  if (n == "string_clone") {
    if (!need(1)) return Type::structOf(ensurePreludeString(currentProgram()));
    if (argT(0) && !(argT(0)->k == Type::PTR &&
                     argT(0)->pointee &&
                     argT(0)->pointee->k == Type::STRUCT &&
                     argT(0)->pointee->sdef == ensurePreludeString(currentProgram())))
      diag(Diag::Error, Code::Args,
           "string_clone expects a *String — take the address with &mut s",
           e->loc);
    return Type::structOf(ensurePreludeString(currentProgram()));
  }
  if (n == "string_clear") {
    need(1);
    if (argT(0) && !(argT(0)->k == Type::PTR &&
                     argT(0)->pointee &&
                     argT(0)->pointee->k == Type::STRUCT &&
                     argT(0)->pointee->sdef == ensurePreludeString(currentProgram())))
      diag(Diag::Error, Code::Args,
           "string_clear expects a *String — take the address with &mut s",
           e->loc);
    return Type::simple(Type::VOID);
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
           e->loc);
    }
    return (a && a->k == Type::F32) ? Type::simple(Type::F32)
                                    : Type::simple(Type::F64);
  }
  if (n == "pow" || n == "fmin" || n == "fmax") {
    if (!need(2)) return Type::simple(Type::F64);
    Type* a = argT(0);
    if (a && !a->isFloat()) {
      diag(Diag::Error, Code::Args, n + " expects float arguments",
           e->loc);
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
           e->loc);
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
           e->loc);
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
           e->loc);
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
           e->loc);
    } else if (args->size() != v->fields.size()) {
      diag(Diag::Error, Code::Args,
           "variant " + vname + " expects " +
           std::to_string(v->fields.size()) + " payload value(s), got " +
           std::to_string(args->size()),
           e->loc);
    }
    for (size_t i = 0; i < args->size() && i < v->fields.size(); ++i) {
      Type* at = typeExpr(fs, (*args)[i]);
      Type* pt = v->fields[i].ty;
      if (at && pt && !typesCompatible(pt, at) &&
          !(at->isNum() && pt->isNum())) {
        diag(Diag::Error, Code::Type,
             "payload " + std::to_string(i + 1) + " of " + vname + ": expected " +
             typeToString(pt) + ", got " + typeToString(at),
             e->loc);
      }
    }
  } else if (!v->fields.empty()) {
    // allow zero-arg spellings only for payload-less variants
    diag(Diag::Error, Code::Args,
         "variant " + vname + " carries a payload — construct it as " +
         vname + "(...)",
         e->loc);
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
             e->loc);
        return true;
      }
  }
  (void)hasArgs;
  return resolveVariantExprAgainst(fs, e, found, Type::enumOf(found));
}

Type* typeExpr(FnSema* fs, Expr* e) {
  if (e->ty) return e->ty;   // memoized
  auto bad = [&](const std::string& m) {
    diag(Diag::Error, Code::Type, m, e->loc);
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
      // fsht-02:03: a const generic parameter in the current
      // instantiation is a compile-time VALUE — fold the identifier into
      // an integer literal so IRgen (and every later pass) sees a
      // constant, never a variable slot
      if (uint64_t* cv = constParamValueInScope(e->name)) {
        e->k = Expr::INT;
        e->ival = *cv;
        e->constant = true;
        e->ty = Type::simple(Type::USIZE);
        return e->ty;
      }
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
                 e->loc);
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
      // fsht-03:01 (ch. 23 §5): a module-qualified spelling of a
      // compiler-known builtin (`string::parse_i64`, `numeric::checked_add`)
      // resolves through the builtin surface once the qualifier is
      // stripped — the §1–§3 builtins are usable without any import and
      // are re-exported by the string/numeric modules.
      if (!e->qualModule.empty()) {
        std::string stripped = e->name.substr(e->qualModule.size() + 2);
        if (isBuiltinCall(stripped)) {
          e->name = stripped;
          e->qualModule.clear();
        }
      }
      if (isBuiltinCall(e->name)) {
        // malloc(T, n) / soa_new(Struct, n) / stack_alloc(T, n) / vec_new(T, cap):
        // first arg is a type name, not an expression — skip expression typing for it
        size_t skipTypeArg = (e->name == "malloc" || e->name == "soa_new" ||
                              e->name == "stack_alloc" || e->name == "vec_new") ? 1 : 0;
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
                     e->loc);
            }
          }
          e->fnPtrSym = fps;
          e->globalRef = fpg;
          e->ty = fpty->pointee;
          return e->ty;
        }
        // ---- fsht-02:02 (registry 06.02.01): trait-method calls --------
        // `Numeric::add(a, b)` / `Rect::area()` / `T::zero()` in concrete
        // (or instantiated-generic) code.  A `Self::m` call inside an impl
        // was already re-targeted to `Trait::m` by the parser, so the
        // enclosing impl fn's target (implSelfTy) makes zero-argument
        // self-calls unambiguous.
        if (e->name.find("::") != std::string::npos) {
          size_t dc = e->name.find("::");
          std::string prefix = e->name.substr(0, dc);
          std::string method = e->name.substr(dc + 2);
          bool maybeTrait = findTraitDef(prefix) != nullptr ||
                            findStructDef(prefix) != nullptr ||
                            findEnumDef(prefix) != nullptr;
          // a TPARAM prefix (T::zero() inside a generic instance body) also
          // names a concrete type by now — resolve through the same path
          // using the typed arguments
          if (maybeTrait || tparamTypeInScope(prefix) ||
              constParamValueInScope(prefix)) {
            for (auto* a : e->args) typeExpr(fs, a);
            std::vector<Type*> argTys;
            for (auto* a : e->args) argTys.push_back(a->ty);
            Type* preferSelf = nullptr;
            if (fs && fs->fn && fs->fn->isTraitImpl &&
                fs->fn->traitName == prefix)
              preferSelf = fs->fn->implSelfTy;
            if (!preferSelf && tparamTypeInScope(prefix))
              preferSelf = tparamTypeInScope(prefix);
            FnDecl* impl = resolveTraitMethodCall(prefix, method, argTys,
                                                   preferSelf, e->loc);
            if (impl) {
              // rewrite to the direct mangled call — static dispatch by
              // construction (irgen sees an ordinary user call)
              e->name = impl->name;
              e->qualModule.clear();
              f = impl;
            }
          }
        }
        if (!f) {
          for (auto* a : e->args) typeExpr(fs, a);
          return bad("unknown function: " + e->name);
        }
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
        // fsht-03:01: checked_cast/saturating_cast infer U from the
        // parameter's type (Option<U>/U context)
        trySetCastTarget(fs, e->args[i], pt);
        Type* at = typeExpr(fs, e->args[i]);
        // fsht-03:02 (ch. 24 §1): `borrowed mut` parameters — the call
        // site passes a mutable borrow (&mut place / &place legacy) or,
        // for POINTER parameters, a pointer binding that HOLDS a borrow
        // (the chapter's own fill(r, 4) example).  An aggregate passed
        // by value is the passing-mode mismatch F0303 (it would move).
        if (f->params[i].borrowedMut) {
          Expr* a = e->args[i];
          bool isBorrowForm = a && a->k == Expr::UN && a->op == "&";
          bool pointerValue = at && at->k == Type::PTR;
          bool aggregateForm = isBorrowForm && at && at->k == Type::PTR &&
                               at->pointee && pt && pt->k == Type::STRUCT &&
                               at->pointee->k == Type::STRUCT &&
                               at->pointee->sdef == pt->sdef;
          if (!isBorrowForm && !pointerValue) {
            diag(Diag::Error, Code::OwnCall,
                 "argument " + std::to_string(i + 1) + " of " + e->name +
                 " (" + f->params[i].name + ") is declared `borrowed mut` — "
                 "pass a mutable borrow (&mut place) or a borrow binding, "
                 "not a by-value aggregate",
                 e->loc);
          } else if (isBorrowForm && a->rhs && a->rhs->k == Expr::IDENT &&
                     a->rhs->sym && !a->rhs->sym->mut) {
            diag(Diag::Error, Code::Args,
                 "'" + a->rhs->name + "' must be a `let mut` binding to "
                 "borrow it mutably", e->loc);
          }
          (void)aggregateForm;
        }
        // fsht-03:02: &mut place of an aggregate borrows it — same ABI
        // as the by-address aggregate parameter (ch. 24 §1)
        bool borrowAggregateOk = f->params[i].borrowedMut &&
                                 at && at->k == Type::PTR && pt &&
                                 pt->k == Type::STRUCT &&
                                 at->pointee && at->pointee->k == Type::STRUCT &&
                                 at->pointee->sdef == pt->sdef;
        if (at && pt && !typesCompatible(pt, at) && !borrowAggregateOk) {
          if (!(e->args[i]->constant && pt->isNum() && at->isNum()))
            diag(Diag::Error, Code::Type, "arg " + std::to_string(i + 1) + " of " +
                 e->name + ": expected " + typeToString(pt) + ", got " +
                 typeToString(at), e->loc);
        }
        if (e->argGive[i] && !(at && at->isPtr()) &&
            // fsht-02:01: give() of a container is a move (registry
            // 02.09.02) — struct containers are as transferable as
            // owned pointers
            !(at && at->k == Type::STRUCT && containerElemType(at))) {
          diag(Diag::Error, Code::Own, "give() requires a pointer",
               e->loc);
        }
        // Phase MEM (§5.3): parameter mode enforcement at the call site
        if (f->params[i].owned && !e->argGive[i]) {
          diag(Diag::Error, Code::OwnCall,
               "argument " + std::to_string(i + 1) + " of " + e->name +
               " (" + f->params[i].name + ") is declared `owned` — the caller "
               "must transfer it with give(...)",
               e->loc);
        }
        if (f->params[i].borrowed && e->argGive[i]) {
          diag(Diag::Error, Code::OwnCall,
               "argument " + std::to_string(i + 1) + " of " + e->name +
               " (" + f->params[i].name + ") is declared `borrowed` — it cannot "
               "be given away",
               e->loc);
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
        // unsigned negation is always a hard error (F0215); only
        // constant operands are exempt (they fold in sema)
        diag(Diag::Error, Code::ArithNegUnsigned,
             "negation of unsigned value — declare as i64 or use 0 - x",
             e->loc);
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
               e->loc);
        }
        e->ty = bt->pointee;
        e->isLvalue = true;
        return e->ty;
      }
      // Phase 1c (report §4.1): Span<T> indexing is bounds-checked in
      // checked builds
      if (bt->k == Type::SPAN) {
        e->ty = bt->pointee;
        e->isLvalue = true;
        return e->ty;
      }
      // fsht-02:01 (registry 02.09.01): Vec<T> indexing — the element is
      // an lvalue; bounds-checked against the runtime len like a Span
      if (bt->k == Type::STRUCT && isContainerType(bt) &&
          containerElemType(bt)) {
        e->ty = containerElemType(bt);
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
             narrowingCastMessage(bt, to), e->loc);
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
               e->loc);
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
      // fsht-02:01 (registry 02.09.01): slicing a Vec<T> yields a
      // borrowed Span<T> view (no copy); slicing an owned String yields
      // a str view — the {data,len} prefix makes both free
      if (bt->k == Type::STRUCT && isContainerType(bt)) {
        Type* elem = containerElemType(bt);
        e->ty = elem ? Type::spanOf(elem) : Type::simple(Type::STR);
        return e->ty;
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
               e->loc);
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
