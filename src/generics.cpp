// Freshet generics (Phase 2a, report §4.2) — compile-time monomorphisation.
//
// Design:
//   * `fn clamp<T: Numeric>(x: T, ...) -> T` and `struct Pair<T, U> {...}`
//     are TEMPLATES.  They are never emitted to IR themselves.
//   * Type positions (`Span<f64>`, `Pair<Point, f64>`, `Option<i64>`) are
//     resolved by bindEncType() from the parser's textual encoding; each
//     distinct argument set instantiates a concrete StructDef/EnumDef with
//     the template parameters substituted, interned by a mangled name
//     (`Pair$Point$f64`).
//   * Call sites instantiate generic FUNCTIONS from the argument types and
//     are rewritten to the instance (`clamp$i64`).  The instance body is a
//     deep clone of the template with types substituted, then runs through
//     the ordinary sema — so every instantiation is checked independently
//     and errors point at real code.
//   * No runtime dispatch, no vtables: predictable codegen, compatible with
//     Penstock's performance goals.
//
// Prelude: Option<T> and Result<T, E> (report §4.6) are generic enums the
// compiler provides on demand.
#include "common.h"
#include <sstream>

namespace freshet {

// ---------------------------------------------------------------- mangling
static std::string mangleType(Type* t) {
  if (!t) return "v";
  switch (t->k) {
    case Type::U8: return "u8";   case Type::U16: return "u16";
    case Type::U32: return "u32"; case Type::U64: case Type::USIZE: return "u64";
    case Type::I8: return "i8";   case Type::I16: return "i16";
    case Type::I32: return "i32"; case Type::I64: return "i64";
    case Type::F32: return "f32"; case Type::F64: return "f64";
    case Type::BOOL: return "bool"; case Type::VOID: return "void";
    case Type::STR: return "str";
    case Type::PTR: return "p" + mangleType(t->pointee);
    case Type::SPAN: return "s" + mangleType(t->pointee);
    case Type::ARRAY: return "a" + std::to_string(t->arrLen) + "_" +
                             mangleType(t->pointee);
    case Type::STRUCT: return "S" + t->sdef->name;
    case Type::SOA: return "O" + t->sdef->name;
    case Type::ENUM: return "E" + t->edef->name;
  }
  return "?";
}

// ---------------------------------------------------------------- prelude
static EnumDef* makePreludeEnum(const std::string& name,
                                const std::string& t0,
                                const std::string& okName,
                                const std::string& errName,
                                bool twoParams) {
  EnumDef* ed = new EnumDef();
  ed->name = name;
  ed->templateName = name;
  ed->isGeneric = true;
  ed->tparams.push_back(t0);
  if (twoParams) ed->tparams.push_back("E");
  auto addVariant = [&](const std::string& vn, const std::string& payloadEnc) {
    EnumVariant v;
    v.name = vn;
    if (!payloadEnc.empty()) {
      Param pm;
      pm.name = "__0";
      pm.ty = Type::structOf(nullptr);
      pm.tyName = payloadEnc;
      v.fields.push_back(pm);
    }
    v.idx = (int)ed->variants.size();
    ed->variants.push_back(v);
  };
  addVariant(okName, t0);
  addVariant(errName, twoParams ? "E" : "");
  return ed;
}

// prelude String struct (Phase 2b, report §4.5): the owned growable UTF-8
// buffer.  { data: *u8, len: u64, cap: u64 } — the {data,len} prefix is
// exactly a str view, so views are free.
StructDef* ensurePreludeString(Program* prog) {
  if (StructDef* sd = findStructDef("String")) return sd;
  StructDef* sd = new StructDef();
  sd->name = "String";
  auto addField = [&](const std::string& n, Type* t) {
    Field f;
    f.name = n;
    f.ty = t;
    f.idx = (int)sd->fields.size();
    sd->fields.push_back(f);
  };
  addField("data", Type::ptr(Type::simple(Type::U8)));
  addField("len", Type::simple(Type::U64));
  addField("cap", Type::simple(Type::U64));
  registerStruct(sd);
  computeStructLayout(sd);
  if (prog) {
    sd->id = (int)prog->structs.size();
    prog->structs.push_back(sd);
  }
  return sd;
}

// ensure the Option/Result templates exist in the enum registry
static void ensurePreludeEnum(const std::string& name) {
  if (findEnumDef(name)) return;
  EnumDef* ed = nullptr;
  if (name == "Option") ed = makePreludeEnum("Option", "T", "Some", "None", false);
  else if (name == "Result")
    ed = makePreludeEnum("Result", "T", "Ok", "Err", true);
  if (ed) registerEnum(ed);
}

// fsht-02:01: public wrapper for sema callers (vec_pop/vec_get/vec_remove
// build Option<T> results) — the prelude enums register on first use
EnumDef* ensurePreludeEnumType(const std::string& name) {
  ensurePreludeEnum(name);
  return findEnumDef(name);
}

// fsht-02:01 (registry 02.09.01) — the Vec<T> dynamic-collection template.
// Same trick as user generic structs: a template StructDef whose fields
// carry textual encodings the binder resolves per instantiation, so
//   Vec<T> = { data: *T, len: u64, cap: u64 }
// monomorphises through the ordinary instantiateStruct path (Vec$i64,
// Vec$f64, Vec$struct.Particle ...).  The {data,len} prefix keeps a Vec
// slicing into a Span<T> a two-field view exactly like String -> str.
StructDef* ensurePreludeVec(Program* prog) {
  if (StructDef* sd = findStructDef("Vec")) return sd;
  StructDef* sd = new StructDef();
  sd->name = "Vec";
  sd->isGeneric = true;
  sd->tparams = {"T"};
  auto addField = [&](const std::string& n, Type* t,
                      const std::string& enc) {
    Field f;
    f.name = n;
    f.ty = t;              // nullptr for *T: bound only at instantiation
    f.tyName = enc;        // binder encoding ("*:T" / "u64")
    f.idx = (int)sd->fields.size();
    sd->fields.push_back(f);
  };
  addField("data", nullptr, "*:T");
  addField("len", Type::simple(Type::USIZE), "u64");
  addField("cap", Type::simple(Type::USIZE), "u64");
  registerStruct(sd);
  if (prog) {
    sd->id = (int)prog->structs.size();
    prog->structs.push_back(sd);
  }
  return sd;
}

// instantiate Vec<elem> (fsht-03:02, registry 02.09.02 lifting, ch. 24
// §5): owning element types (Vec<String>, Vec<Vec<T>>) are ACCEPTED —
// drops walk the shape innermost-first, moves transfer whole-shape,
// vec_clone deep-copies element-wise (irgen_stmt.cpp shape machinery).
// The template-vs-concrete and void gates stay.
StructDef* instantiateVecType(Program* prog, Type* elem, SrcLoc loc) {
  if (!elem || elem->k == Type::VOID) {
    diag(Diag::Error, Code::Type,
         "Vec<T> needs a concrete element type", loc);
    return ensurePreludeVec(prog);
  }
  // a generic template is not a usable element (a bare template name
  // slipping through an un-instantiated annotation)
  if (elem->k == Type::STRUCT && elem->sdef && elem->sdef->isGeneric) {
    diag(Diag::Error, Code::Type,
         "Vec<T> element '" + elem->sdef->name +
         "' is a generic template, not a concrete type", loc);
    return ensurePreludeVec(prog);
  }
  ensurePreludeVec(prog);
  return instantiateStruct(prog, findStructDef("Vec"), {elem}, loc);
}

// ---------------- fsht-03:01 (registry 05.04.01/05.05.01) prelude types
// chapter 23 §1: the parse Result's error payload
//     enum ParseError { Invalid, OutOfRange }
// chapter 23 §4: the streaming-I/O error taxonomy and the handle structs
//     enum IoError { NotFound, Permission, Eof, Busy, Other(i64) }
//     Reader { handle: i64 }    Writer { handle: i64 }
// All non-generic prelude decls (module "" — always visible, like the
// String struct and the Option/Result templates).
static void pushEnum(Program* prog, EnumDef* ed) {
  registerEnum(ed);
  computeEnumLayout(ed);
  if (prog) {
    ed->id = (int)prog->enums.size();
    prog->enums.push_back(ed);
  }
}

EnumDef* ensurePreludeParseError(Program* prog) {
  if (EnumDef* ed = findEnumDef("ParseError")) return ed;
  EnumDef* ed = new EnumDef();
  ed->name = "ParseError";
  ed->templateName = "ParseError";
  auto addVar = [&](const std::string& vn) {
    EnumVariant v;
    v.name = vn;
    v.idx = (int)ed->variants.size();
    ed->variants.push_back(v);
  };
  addVar("Invalid");
  addVar("OutOfRange");
  pushEnum(prog, ed);
  return ed;
}

EnumDef* ensurePreludeIoError(Program* prog) {
  if (EnumDef* ed = findEnumDef("IoError")) return ed;
  EnumDef* ed = new EnumDef();
  ed->name = "IoError";
  ed->templateName = "IoError";
  auto addVar = [&](const std::string& vn, Type* payload) {
    EnumVariant v;
    v.name = vn;
    if (payload) {
      Param pm;
      pm.name = "__0";
      pm.ty = payload;
      pm.tyName = "i64";
      v.fields.push_back(pm);
    }
    v.idx = (int)ed->variants.size();
    ed->variants.push_back(v);
  };
  addVar("NotFound", nullptr);
  addVar("Permission", nullptr);
  addVar("Eof", nullptr);
  addVar("Busy", nullptr);
  addVar("Other", Type::simple(Type::I64));
  pushEnum(prog, ed);
  return ed;
}

static StructDef* makeHandleStruct(Program* prog, const std::string& name) {
  StructDef* sd = new StructDef();
  sd->name = name;
  Field f;
  f.name = "handle";
  f.ty = Type::simple(Type::I64);
  f.idx = 0;
  sd->fields.push_back(f);
  registerStruct(sd);
  computeStructLayout(sd);
  if (prog) {
    sd->id = (int)prog->structs.size();
    prog->structs.push_back(sd);
  }
  return sd;
}

StructDef* ensurePreludeReader(Program* prog) {
  if (StructDef* sd = findStructDef("Reader")) return sd;
  return makeHandleStruct(prog, "Reader");
}

StructDef* ensurePreludeWriter(Program* prog) {
  if (StructDef* sd = findStructDef("Writer")) return sd;
  return makeHandleStruct(prog, "Writer");
}

// ---------------------------------------------------------------- binder
// Resolve the parser's textual type encoding (see parser.cpp) to a Type*.
// `tmap` substitutes generic template parameters inside a template body
// ("T" -> concrete). `cenv` supplies const-generic parameter values
// (fsht-02:03) for array-length and const-argument evaluation.  `prog`
// receives instantiated concrete types so later phases (layout, irgen)
// see them.
static Type* bindEncType(Program* prog, const std::string& enc, SrcLoc loc,
                         const std::map<std::string, Type*>* tmap,
                         const std::map<std::string, uint64_t>* cenv = nullptr);

// parse one encoding starting at `pos`; returns the resolved type and
// advances `pos` past what it consumed
static Type* bindEncOne(Program* prog, const std::string& enc, size_t& pos,
                        SrcLoc loc, const std::map<std::string, Type*>* tmap,
                        const std::map<std::string, uint64_t>* cenv = nullptr) {
  auto rest = [&](size_t from) { return enc.substr(from); };
  (void)rest;
  // Phase 3a helper: visibility gate for `mod::Name` qualified access
  auto qualVisible = [&](const std::string& mod, const std::string& item,
                         bool isPub) -> bool {
    if (!moduleLookupIsStrict()) return true;
    if (!moduleVisibleFrom(mod)) {
      diag(Diag::Error, Code::Import,
           "module '" + mod + "' is not imported here — cannot use " +
           mod + "::" + item,
           loc);
      return false;
    }
    if (!isPub && mod != currentLookupModule()) {
      diag(Diag::Error, Code::Import,
           "type '" + item + "' is private to module '" + mod +
           "' — mark it `pub struct'/`pub enum'",
           loc);
      return false;
    }
    return true;
  };
  // Phase 3b: end position of the complete encoding starting at pos —
  // needed to split fn-pointer parameter lists unambiguously (params
  // may themselves contain ':' / ';' inside nested encodings)
  struct EndOf {
    size_t operator()(const std::string& e, size_t p) {
      auto term = [&](char c) { return c == ';' || c == ':'; };
      if (e.compare(p, 4, "soa:") == 0 || e.compare(p, 5, "qual:") == 0) {
        size_t i = p + (e.compare(p, 4, "soa:") == 0 ? 4 : 5);
        while (i < e.size() && !term(e[i])) ++i;
        return i;
      }
      if (e.compare(p, 2, "*:") == 0) return (*this)(e, p + 2);
      if (e.compare(p, 5, "span:") == 0) return (*this)(e, p + 5);
      if (e.compare(p, 4, "arr:") == 0) {
        size_t i = p + 4;
        if (i < e.size() && isdigit((unsigned char)e[i])) {
          while (i < e.size() && isdigit((unsigned char)e[i])) ++i;
        } else {
          // fsht-02:03: const-expression length — scan to the ':' that
          // separates it from the element encoding (const exprs never
          // contain ':'; '(' ')' and ',' group subexpressions)
          size_t depth = 0;
          while (i < e.size() && (depth > 0 || e[i] != ':')) {
            if (e[i] == '(') ++depth;
            else if (e[i] == ')') --depth;
            ++i;
          }
        }
        if (i < e.size() && e[i] == ':') return (*this)(e, i + 1);
        return i;
      }
      if (e.compare(p, 4, "gen:") == 0) {
        size_t open = e.find('(', p + 4);
        if (open == std::string::npos) return e.size();
        int depth = 0;
        for (size_t i = open; i < e.size(); ++i) {
          if (e[i] == '(') ++depth;
          else if (e[i] == ')') { if (--depth == 0) return i + 1; }
        }
        return e.size();
      }
      if (e.compare(p, 4, "fnp:") == 0) {
        size_t i = p + 4;
        while (i < e.size()) {
          size_t end = (*this)(e, i);
          if (end >= e.size()) return e.size();
          if (e[end] == ';') { i = end + 1; continue; }
          if (e[end] == ':') return (*this)(e, end + 1);
          return end;
        }
        return i;
      }
      size_t i = p;
      while (i < e.size() && !term(e[i])) ++i;
      return i;
    }
  };
  static EndOf endOfEnc;
  // Phase 3b (report §6.3): fn(P1; P2; ...) -> R — bind the parameter
  // list + return type through their nested encodings and intern the
  // function-pointer type.
  if (enc.compare(pos, 4, "fnp:") == 0) {
    std::vector<Type*> params;
    size_t i = pos + 4;
    while (true) {
      size_t e = endOfEnc(enc, i);
      if (e < enc.size() && enc[e] == ';') {
        std::string penc = enc.substr(i, e - i);
        size_t sub = 0;
        params.push_back(bindEncOne(prog, penc, sub, loc, tmap, cenv));
        i = e + 1;
        continue;
      }
      if (e < enc.size() && enc[e] == ':') {
        if (e > i) {                      // last parameter before the ret
          std::string penc = enc.substr(i, e - i);
          size_t sub = 0;
          params.push_back(bindEncOne(prog, penc, sub, loc, tmap, cenv));
        }
        std::string renc = enc.substr(e + 1);
        size_t sub = 0;
        Type* rt = renc.empty() ? Type::simple(Type::VOID)
                                : bindEncOne(prog, renc, sub, loc, tmap, cenv);
        pos = enc.size();
        return Type::fnptrOf(params, rt);
      }
      // malformed — no ':' separator for the return type
      pos = enc.size();
      return Type::simple(Type::VOID);
    }
  }
  // Phase 3a: qual:mod::Name — module-qualified plain type
  if (enc.compare(pos, 5, "qual:") == 0) {
    std::string full = enc.substr(pos + 5);
    pos = enc.size();
    size_t dc = full.find("::");
    if (dc == std::string::npos) return Type::simple(Type::VOID);
    std::string mod = full.substr(0, dc);
    std::string item = full.substr(dc + 2);
    if (StructDef* sd = findStructInModule(item, mod)) {
      if (qualVisible(mod, item, sd->isPub)) return Type::structOf(sd);
      return Type::simple(Type::VOID);
    }
    if (EnumDef* ed = findEnumInModule(item, mod)) {
      if (qualVisible(mod, item, ed->isPub)) return Type::enumOf(ed);
      return Type::simple(Type::VOID);
    }
    diag(Diag::Error, Code::Type,
         "module '" + mod + "' has no type named '" + item + "'",
         loc);
    return Type::simple(Type::VOID);
  }
  if (enc.compare(pos, 4, "soa:") == 0) {
    pos = enc.size();
    std::string sn = enc.substr(4);
    StructDef* sd = findStructDef(sn);
    if (!sd) {
      diag(Diag::Error, Code::Type, "unknown struct in soa(): " + sn,
           loc);
      return Type::simple(Type::VOID);
    }
    return Type::soaOf(sd);
  }
  if (enc.compare(pos, 2, "*:") == 0) {
    pos += 2;
    size_t sub = pos;
    Type* inner = bindEncOne(prog, enc, sub, loc, tmap, cenv);
    pos = sub;
    return Type::ptr(inner);
  }
  if (enc.compare(pos, 4, "arr:") == 0) {
    size_t nStart = pos + 4;
    size_t colon = enc.find(':', nStart);
    if (colon == std::string::npos) return Type::simple(Type::VOID);
    uint64_t n = 0;
    std::string lenEnc = enc.substr(nStart, colon - nStart);
    bool lenIsDigits = !lenEnc.empty() &&
      lenEnc.find_first_not_of("0123456789") == std::string::npos;
    if (lenIsDigits) {
      n = strtoull(lenEnc.c_str(), nullptr, 10);
    } else {
      // fsht-02:03: const-expression length — evaluate under the active
      // const-parameter environment (integer literals need none)
      if (!evalConstEnc(lenEnc, cenv, loc, &n))
        return Type::simple(Type::VOID);
    }
    pos = colon + 1;
    size_t sub = pos;
    Type* elem = bindEncOne(prog, enc, sub, loc, tmap, cenv);
    pos = sub;
    return Type::arrayOf(elem, n);
  }
  if (enc.compare(pos, 5, "span:") == 0) {
    pos += 5;
    size_t sub = pos;
    Type* elem = bindEncOne(prog, enc, sub, loc, tmap, cenv);
    pos = sub;
    return Type::spanOf(elem);
  }
  if (enc.compare(pos, 4, "gen:") == 0) {
    // gen:Name(ENC;ENC;...)   — Name may be module-qualified (mod::Item)
    size_t open = enc.find('(', pos + 4);
    if (open == std::string::npos) return Type::simple(Type::VOID);
    std::string name = enc.substr(pos + 4, open - pos - 4);
    // split top-level args on ';'
    std::vector<std::string> argEncs;
    size_t depth = 0, start = open + 1;
    for (size_t i = open + 1; i < enc.size(); ++i) {
      char c = enc[i];
      if (c == '(') ++depth;
      else if (c == ')') {
        if (depth == 0) {
          argEncs.push_back(enc.substr(start, i - start));
          break;
        }
        --depth;
      } else if (c == ';' && depth == 0) {
        argEncs.push_back(enc.substr(start, i - start));
        start = i + 1;
      }
    }
    // '~' marks a fully-resolved argument that carried no name encoding
    // (the parser emits it when an argument's parse produced no name);
    // the placeholder Type was built by the parser, but the encoding
    // alone cannot recover it, so it surfaces as an unresolved argument
    // (nullptr) below.
    //
    // fsht-02:03: const-parameter slots carry const-expression encodings
    // (digits, names, "(A * B)", "fn(args)"); which slots are const is
    // known only from the template (tparamIsConst), so arguments are
    // bound per-slot AFTER the template is identified below.
    pos = enc.size();
    // Phase 3a: module-qualified template name? (gen:mod::Item(...))
    std::string qualMod;
    std::string lookupName = name;
    {
      size_t dc = name.find("::");
      if (dc != std::string::npos) {
        qualMod = name.substr(0, dc);
        lookupName = name.substr(dc + 2);
      }
    }
    // struct template?
    // fsht-02:01: the Vec<T> prelude template registers lazily, exactly
    // like String, so `gen:Vec(i64)` resolves on first use.
    if (qualMod.empty() && name == "Vec") ensurePreludeVec(prog);
    StructDef* tmpl = qualMod.empty() ? findStructDef(name)
                                      : findStructInModule(lookupName, qualMod);
    if (tmpl && !qualMod.empty()) {
      // qualified access: visibility gate
      if (moduleLookupIsStrict()) {
        if (!moduleVisibleFrom(qualMod)) {
          diag(Diag::Error, Code::Import,
               "module '" + qualMod + "' is not imported here — cannot use " +
               name, loc);
          return Type::simple(Type::VOID);
        }
        if (!tmpl->isPub && qualMod != currentLookupModule()) {
          diag(Diag::Error, Code::Import,
               "type '" + lookupName + "' is private to module '" + qualMod +
               "' — mark it `pub struct'", loc);
          return Type::simple(Type::VOID);
        }
      }
    }
    // helper: bind the collected argument encodings for template `t`
    // whose parameter kinds are known.  Type slots bind through the
    // tmap (recursively); const slots evaluate through the cenv.
    auto bindArgs = [&](const std::vector<std::string>& tparams,
                        const std::vector<bool>& isConst,
                        const std::string& kind) ->
        std::pair<std::vector<Type*>, std::vector<uint64_t>> {
      std::vector<Type*> targs;
      std::vector<uint64_t> cargs;
      for (size_t ai = 0; ai < argEncs.size(); ++ai) {
        bool constSlot = ai < isConst.size() ? isConst[ai] : false;
        const std::string& ae = argEncs[ai];
        if (ae == "~" && !constSlot) { targs.push_back(nullptr); continue; }
        // a slot is const only when the template says so; a bare name in
        // a const slot is a const-parameter reference, in a type slot a
        // type name (the parser cannot distinguish — the template can)
        if (constSlot) {
          uint64_t v = 0;
          if (!evalConstEnc(ae, cenv, loc, &v)) {
            diag(Diag::Error, Code::Generic,
                 "argument " + std::to_string(ai + 1) + " of " + name +
                 " is a const parameter slot and must be a const "
                 "expression (integer, const parameter or const fn)", loc);
            return {{}, {}};
          }
          cargs.push_back(v);
          continue;
        }
        // type slot: digits-only encodings are also valid (literals in
        // type position are only meaningful for const slots; reject)
        bool digitsOnly = !ae.empty() &&
          ae.find_first_not_of("0123456789") == std::string::npos;
        if (digitsOnly) {
          diag(Diag::Error, Code::Generic,
               "argument " + std::to_string(ai + 1) + " of " + name +
               " is a literal but parameter " +
               (ai < tparams.size() ? tparams[ai] : "?") +
               " is a type parameter — pass a type", loc);
          targs.push_back(nullptr);
          continue;
        }
        size_t sub = 0;
        targs.push_back(bindEncOne(prog, ae, sub, loc, tmap, cenv));
        if (sub != ae.size()) {
          // trailing garbage after the argument encoding — ignored
          // (bindEncOne reports what it consumed)
        }
      }
      (void)kind;
      return {targs, cargs};
    };
    if (tmpl && tmpl->isGeneric) {
      if (argEncs.size() != tmpl->tparams.size()) {
        diag(Diag::Error, Code::Generic,
             "generic struct " + name + " expects " +
             std::to_string(tmpl->tparams.size()) + " arguments, got " +
             std::to_string(argEncs.size()),
             loc);
        return Type::simple(Type::VOID);
      }
      auto [targs, cargs] = bindArgs(tmpl->tparams, tmpl->tparamIsConst, "struct");
      for (Type* a : targs)
        if (!a) {
          diag(Diag::Error, Code::Generic,
               "unresolved type argument in " + name + "<...>",
               loc);
          return Type::simple(Type::VOID);
        }
      // fsht-02:01: Vec instantiations go through the element gate
      // (owning elements rejected for the first cut).
      if (name == "Vec" && targs.size() == 1)
        return Type::structOf(instantiateVecType(prog, targs[0], loc));
      return Type::structOf(instantiateStruct(prog, tmpl, targs, loc, cargs));
    }
    // enum template? (incl. the Option/Result prelude)
    ensurePreludeEnum(lookupName);
    EnumDef* et = qualMod.empty() ? findEnumDef(lookupName)
                                  : findEnumInModule(lookupName, qualMod);
    if (et && !qualMod.empty() && moduleLookupIsStrict()) {
      if (!moduleVisibleFrom(qualMod)) {
        diag(Diag::Error, Code::Import,
             "module '" + qualMod + "' is not imported here — cannot use " +
             name, loc);
        return Type::simple(Type::VOID);
      }
      if (!et->isPub && qualMod != currentLookupModule()) {
        diag(Diag::Error, Code::Import,
             "type '" + lookupName + "' is private to module '" + qualMod +
             "' — mark it `pub enum'", loc);
        return Type::simple(Type::VOID);
      }
    }
    if (et && et->isGeneric) {
      if (argEncs.size() != et->tparams.size()) {
        diag(Diag::Error, Code::Generic,
             "generic enum " + name + " expects " +
             std::to_string(et->tparams.size()) + " arguments, got " +
             std::to_string(argEncs.size()),
             loc);
        return Type::simple(Type::VOID);
      }
      auto [targs, cargs] = bindArgs(et->tparams, et->tparamIsConst, "enum");
      for (Type* a : targs)
        if (!a) {
          diag(Diag::Error, Code::Generic,
               "unresolved type argument in " + name + "<...>",
               loc);
          return Type::simple(Type::VOID);
        }
      return Type::enumOf(instantiateEnum(prog, et, targs, loc, cargs));
    }
    if (tmpl || et) {
      diag(Diag::Error, Code::Generic,
           "type " + name + " is not generic; drop the <...> arguments",
           loc);
    } else {
      diag(Diag::Error, Code::Type, "unknown type name: " + name,
           loc);
    }
    return Type::simple(Type::VOID);
  }
  // plain name: scalar spelling, template parameter or struct/enum.
  // NOTE: the name is the REMAINDER from pos — recursive calls (after
  // "*:", "arr:N:", "span:") arrive with pos > 0.
  std::string name = enc.substr(pos);
  pos = enc.size();
  if (name == "~") return Type::simple(Type::VOID);  // empty/void marker
  if (name == "u8") return Type::simple(Type::U8);
  if (name == "u16") return Type::simple(Type::U16);
  if (name == "u32") return Type::simple(Type::U32);
  if (name == "u64") return Type::simple(Type::USIZE);
  if (name == "i8") return Type::simple(Type::I8);
  if (name == "i16") return Type::simple(Type::I16);
  if (name == "i32") return Type::simple(Type::I32);
  if (name == "i64") return Type::simple(Type::I64);
  if (name == "f32") return Type::simple(Type::F32);
  if (name == "f64") return Type::simple(Type::F64);
  if (name == "bool") return Type::simple(Type::BOOL);
  if (name == "void") return Type::simple(Type::VOID);
  if (name == "str") return Type::simple(Type::STR);
  // prelude String (owned growable buffer) registers on first use
  if (name == "String") ensurePreludeString(prog);
  // fsht-03:01 (ch. 23 §1/§4): the parse/streaming prelude types
  // register lazily the same way — an annotation like
  // Result<i64, ParseError> may be the FIRST spelling of the name
  if (name == "ParseError") ensurePreludeParseError(prog);
  if (name == "IoError") ensurePreludeIoError(prog);
  if (name == "Reader") ensurePreludeReader(prog);
  if (name == "Writer") ensurePreludeWriter(prog);
  // fsht-02:01: bare `Vec` (no <T>) is the generic template — not a
  // usable value type; point the diagnostic at the missing argument.
  if (name == "Vec") {
    ensurePreludeVec(prog);
    diag(Diag::Error, Code::Type,
         "Vec requires a type argument: Vec<T>", loc);
    return Type::simple(Type::VOID);
  }
  if (tmap) {
    auto it = tmap->find(name);
    if (it != tmap->end()) return it->second;
  }
  if (StructDef* sd = findStructDef(name)) return Type::structOf(sd);
  ensurePreludeEnum(name);
  if (EnumDef* ed = findEnumDef(name)) return Type::enumOf(ed);
  diag(Diag::Error, Code::Type, "unknown type name: " + name,
       loc);
  return Type::simple(Type::VOID);
}

static Type* bindEncType(Program* prog, const std::string& enc, SrcLoc loc,
                         const std::map<std::string, Type*>* tmap,
                         const std::map<std::string, uint64_t>* cenv) {
  if (enc.empty()) return nullptr;   // caller keeps the parsed placeholder
  size_t pos = 0;
  return bindEncOne(prog, enc, pos, loc, tmap, cenv);
}

// ---------------------------------------------------------------- cloning
static Expr* cloneExpr(Expr* e) {
  if (!e) return nullptr;
  Expr* n = new Expr(e->k, e->loc);
  n->ival = e->ival; n->fval = e->fval; n->sval = e->sval; n->bval = e->bval;
  n->name = e->name;
  n->op = e->op;
  n->lhs = cloneExpr(e->lhs);
  n->rhs = cloneExpr(e->rhs);
  n->base = cloneExpr(e->base);
  n->castTy = e->castTy;
  n->castTyName = e->castTyName;
  for (auto* a : e->args) n->args.push_back(cloneExpr(a));
  n->argGive = e->argGive;
  n->slitFields = e->slitFields;
  return n;
}

static Stmt* cloneStmt(Stmt* s) {
  if (!s) return nullptr;
  Stmt* n = new Stmt(s->k, s->loc);
  n->tyName = s->tyName;
  n->op2 = s->op2;
  n->pDescending = s->pDescending;
  n->var = s->var; n->annTy = s->annTy; n->mut = s->mut; n->hasAnn = s->hasAnn;
  n->init = cloneExpr(s->init);
  n->lhs = cloneExpr(s->lhs); n->rhs = cloneExpr(s->rhs);
  n->retVal = cloneExpr(s->retVal);
  n->deferExpr = cloneExpr(s->deferExpr);
  n->cond = cloneExpr(s->cond);
  n->thenB = cloneStmt(s->thenB); n->elseB = cloneStmt(s->elseB);
  n->ivar = s->ivar;
  n->forInit = cloneExpr(s->forInit);
  n->forCond = cloneExpr(s->forCond);
  n->forStep = cloneExpr(s->forStep);
  n->forRange = s->forRange;
  n->rangeLo = cloneExpr(s->rangeLo); n->rangeHi = cloneExpr(s->rangeHi);
  n->pvar = s->pvar;
  n->plo = cloneExpr(s->plo); n->phi = cloneExpr(s->phi);
  n->clauses = s->clauses;
  n->body = cloneStmt(s->body);
  n->depth = s->depth;
  n->collapsed = s->collapsed;
  n->vectorizeHint = s->vectorizeHint;
  n->isUnsafe = s->isUnsafe;
  n->matchOn = cloneExpr(s->matchOn);
  for (auto& arm : s->matchArms) {
    MatchArm a2;
    a2.wildcard = arm.wildcard;
    a2.enumName = arm.enumName;
    a2.variantName = arm.variantName;
    a2.variantIdx = arm.variantIdx;
    a2.bindings = arm.bindings;
    a2.body = cloneStmt(arm.body);
    a2.loc = arm.loc;
    n->matchArms.push_back(std::move(a2));
  }
  for (auto* st : s->stmts) n->stmts.push_back(cloneStmt(st));
  n->expr = cloneExpr(s->expr);
  n->fn = s->fn;
  return n;
}

// ---------------------------------------------------------------- structs
StructDef* instantiateStruct(Program* prog, StructDef* tmpl,
                             const std::vector<Type*>& args, SrcLoc loc,
                             const std::vector<uint64_t>& constArgs) {
  for (Type* a : args)
    if (!a) {
      diag(Diag::Error, Code::Generic,
           "unresolved type argument for struct " + tmpl->name,
           loc);
      return tmpl;
    }
  // fsht-02:03: mangle type AND const arguments into the instance name
  // (Vector<f64, 4> -> Vector$f64$4) so distinct const values are distinct
  // interned types.  TYPE args and CONST args travel in separate vectors
  // (type slots in args, const slots in constArgs) — index them by their
  // own counters over the template's parameter positions.
  size_t typeIdx = 0, constIdx = 0;
  std::string mangled = tmpl->name;
  for (size_t i = 0; i < tmpl->tparams.size(); ++i) {
    bool isConst = i < tmpl->tparamIsConst.size() && tmpl->tparamIsConst[i];
    if (isConst) {
      uint64_t v = constIdx < constArgs.size() ? constArgs[constIdx++] : 0;
      mangled += "$" + std::to_string(v);
    } else {
      mangled += "$" + mangleType(typeIdx < args.size() ? args[typeIdx++]
                                                       : nullptr);
    }
  }
  if (StructDef* existing = findStructDef(mangled)) return existing;
  std::map<std::string, Type*> tmap;
  std::map<std::string, uint64_t> cenv;
  typeIdx = 0;
  constIdx = 0;
  for (size_t i = 0; i < tmpl->tparams.size(); ++i) {
    bool isConst = i < tmpl->tparamIsConst.size() && tmpl->tparamIsConst[i];
    if (isConst) {
      if (constIdx < constArgs.size()) cenv[tmpl->tparams[i]] = constArgs[constIdx++];
    } else {
      if (typeIdx < args.size()) tmap[tmpl->tparams[i]] = args[typeIdx++];
    }
  }
  StructDef* sd = new StructDef();
  sd->name = mangled;
  sd->alignOverride = tmpl->alignOverride;
  sd->loc = tmpl->loc;
  // fsht-02:01: mark the instance's originating template so container
  // predicates can recognise Vec<T> instantiations structurally.
  sd->templateOf = tmpl;
  // Phase 3a: the instance belongs to the TEMPLATE's module (its field
  // encodings resolve there) and inherits pub-ness so imported-module
  // instantiations stay visible to the caller.
  sd->module = tmpl->module;
  sd->isPub = tmpl->isPub;
  sd->targs = args;   // recorded for call-site inference
  sd->constTargs = constArgs;   // fsht-02:03: const values, same order
  // bind fields under the template's module context
  bool ctxPushed = false;
  if (moduleLookupIsStrict() && !tmpl->module.empty()) {
    pushModuleContext(tmpl->module);
    ctxPushed = true;
  }
  for (auto& f : tmpl->fields) {
    Field nf;
    nf.name = f.name;
    nf.loc2 = f.loc2;
    // the TEMPLATE's field indices are never assigned (sema's layout pass
    // skips generic templates), so index positionally here
    nf.idx = (int)sd->fields.size();
    if (!f.tyName.empty()) {
      nf.ty = bindEncType(prog, f.tyName, f.loc2, &tmap, &cenv);
      if (!nf.ty) nf.ty = f.ty;
    } else {
      nf.ty = f.ty;
    }
    sd->fields.push_back(nf);
  }
  if (ctxPushed) popModuleContext();
  registerStruct(sd);
  computeStructLayout(sd);
  if (prog) {
    sd->id = (int)prog->structs.size();
    prog->structs.push_back(sd);
  }
  return sd;
}

// ---------------------------------------------------------------- enums
EnumDef* instantiateEnum(Program* prog, EnumDef* tmpl,
                         const std::vector<Type*>& args, SrcLoc loc,
                         const std::vector<uint64_t>& constArgs) {
  // type/const counters (see instantiateStruct)
  size_t typeIdx = 0, constIdx = 0;
  std::string mangled = tmpl->name;
  for (size_t i = 0; i < tmpl->tparams.size(); ++i) {
    bool isConst = i < tmpl->tparamIsConst.size() && tmpl->tparamIsConst[i];
    if (isConst) {
      uint64_t v = constIdx < constArgs.size() ? constArgs[constIdx++] : 0;
      mangled += "$" + std::to_string(v);
    } else {
      mangled += "$" + mangleType(typeIdx < args.size() ? args[typeIdx++]
                                                       : nullptr);
    }
  }
  if (EnumDef* existing = findEnumDef(mangled)) return existing;
  std::map<std::string, Type*> tmap;
  std::map<std::string, uint64_t> cenv;
  typeIdx = 0;
  constIdx = 0;
  for (size_t i = 0; i < tmpl->tparams.size(); ++i) {
    bool isConst = i < tmpl->tparamIsConst.size() && tmpl->tparamIsConst[i];
    if (isConst) {
      if (constIdx < constArgs.size()) cenv[tmpl->tparams[i]] = constArgs[constIdx++];
    } else {
      if (typeIdx < args.size()) tmap[tmpl->tparams[i]] = args[typeIdx++];
    }
  }
  EnumDef* ed = new EnumDef();
  ed->name = mangled;
  ed->templateName = tmpl->templateName.empty() ? tmpl->name : tmpl->templateName;
  ed->loc = tmpl->loc;
  // Phase 3a: instance module + pub-ness (see instantiateStruct)
  ed->module = tmpl->module;
  ed->isPub = tmpl->isPub;
  for (auto& v : tmpl->variants) {
    EnumVariant nv;
    nv.name = v.name;
    nv.loc = v.loc;
    nv.idx = v.idx;
    for (auto& f : v.fields) {
      Param np;
      np.name = f.name;
      np.loc = f.loc;
      if (!f.tyName.empty()) {
        np.ty = bindEncType(prog, f.tyName, f.loc, &tmap, &cenv);
        if (!np.ty) np.ty = f.ty;
      } else {
        np.ty = f.ty;
      }
      nv.fields.push_back(np);
    }
    ed->variants.push_back(nv);
  }
  registerEnum(ed);
  computeEnumLayout(ed);
  if (prog) {
    ed->id = (int)prog->enums.size();
    prog->enums.push_back(ed);
  }
  return ed;
}

// ---------------------------------------------------------------- functions
// Constraint validation (report §4.2).  fsht-02:02: the constraint may
// be a `+`-separated list mixing builtins and user traits — the shared
// implementation lives in traits.cpp so instantiation and call sites
// cannot drift.
static bool typeSatisfiesConstraint(Type* t, const std::string& cons) {
  return typeSatisfiesConstraints(t, cons);
}

// infer the concrete type of each template parameter from one argument.
// supported parameter shapes: T, *T, Span<T>, generic containers
// (Pair<T, U> — the argument's recorded targs drive the inference) and,
// fsht-02:03, fixed arrays `[T; N]` (the element drives T; the LENGTH
// drives any const parameter, handled by instantiateGenericFn's cenv pass)
static bool inferTParam(Type* arg, const std::string& paramEnc,
                        std::string tparam, Type** out) {
  if (paramEnc == tparam) { *out = arg; return arg != nullptr; }
  if (paramEnc == "*:" + tparam && arg && arg->k == Type::PTR) {
    *out = arg->pointee;
    return *out != nullptr;
  }
  if (paramEnc == "span:" + tparam && arg && arg->k == Type::SPAN) {
    *out = arg->pointee;
    return *out != nullptr;
  }
  // arr:<lenEnc>:<elemEnc> — recurse on the element encoding
  if (paramEnc.rfind("arr:", 0) == 0 && arg && arg->k == Type::ARRAY) {
    // split "<lenEnc>:<elemEnc>" — the length may itself be a const
    // expression containing no ':' (see the binder), so the FIRST ':'
    // terminates it
    size_t colon = paramEnc.find(':', 4);
    if (colon != std::string::npos) {
      std::string elemEnc = paramEnc.substr(colon + 1);
      return inferTParam(arg->pointee, elemEnc, tparam, out);
    }
  }
  // gen:Name(ENC;ENC...) — match the argument's instantiation arguments
  if (paramEnc.rfind("gen:", 0) == 0 && arg && arg->k == Type::STRUCT &&
      arg->sdef && arg->sdef->targs.size() >= 1) {
    size_t open = paramEnc.find('(');
    if (open != std::string::npos) {
      // split the encoded argument list on top-level ';'
      std::vector<std::string> encs;
      size_t depth = 0, start = open + 1;
      for (size_t i = open + 1; i < paramEnc.size(); ++i) {
        char c = paramEnc[i];
        if (c == '(') ++depth;
        else if (c == ')') {
          if (depth == 0) { encs.push_back(paramEnc.substr(start, i - start)); break; }
          --depth;
        } else if (c == ';' && depth == 0) {
          encs.push_back(paramEnc.substr(start, i - start));
          start = i + 1;
        }
      }
      for (size_t i = 0; i < encs.size() && i < arg->sdef->targs.size(); ++i) {
        if (encs[i] == tparam) {
          *out = arg->sdef->targs[i];
          return *out != nullptr;
        }
      }
    }
  }
  return true;   // not this parameter's driver — no information
}

FnDecl* instantiateGenericFn(Program* prog, FnDecl* tmpl,
                             const std::vector<Type*>& argTypes, SrcLoc loc) {
  if (argTypes.size() != tmpl->params.size()) {
    diag(Diag::Error, Code::Args,
         "function " + tmpl->name + " expects " +
         std::to_string(tmpl->params.size()) + " args, got " +
         std::to_string(argTypes.size()),
         loc);
    return nullptr;
  }
  // 1. infer template parameter types from the arguments
  std::map<std::string, Type*> tmap;
  for (size_t i = 0; i < tmpl->params.size(); ++i) {
    // try each tparam spelling against this parameter's encoding
    for (auto& tp : tmpl->tparams) {
      Type* t = nullptr;
      if (inferTParam(argTypes[i], tmpl->params[i].tyName, tp, &t) &&
          t && !tmap.count(tp)) {
        tmap[tp] = t;
      }
    }
  }
  // 1b. fsht-02:03: infer CONST parameters from array-shaped params —
  //     `fn sum<T, const N: usize>(v: [T; N])`: an ARRAY argument's
  //     length feeds the bare const name in the parameter's length slot
  std::map<std::string, uint64_t> cenv;
  for (size_t i = 0; i < tmpl->params.size(); ++i) {
    const std::string& penc = tmpl->params[i].tyName;
    if (penc.compare(0, 4, "arr:") != 0 || !argTypes[i] ||
        argTypes[i]->k != Type::ARRAY)
      continue;
    size_t colon = penc.find(':', 4);
    if (colon == std::string::npos) continue;
    std::string lenEnc = penc.substr(4, colon - 4);
    // bare const-parameter name only (general exprs in param lengths
    // cannot be inferred from the call — first cut)
    for (auto& tp : tmpl->tparams) {
      if (lenEnc == tp && !cenv.count(tp) &&
          !tmap.count(tp))
        cenv[tp] = argTypes[i]->arrLen;
    }
  }
  // 2. validate constraints (types must infer; consts must infer)
  for (size_t i = 0; i < tmpl->tparams.size(); ++i) {
    const std::string& tp = tmpl->tparams[i];
    const std::string& cons = i < tmpl->tconstraints.size()
                              ? tmpl->tconstraints[i] : "";
    bool isConst = i < tmpl->tparamIsConst.size() && tmpl->tparamIsConst[i];
    if (isConst) {
      if (!cenv.count(tp)) {
        diag(Diag::Error, Code::Generic,
             "cannot infer const parameter " + tp + " of " + tmpl->name +
             " from the call arguments — pass an array whose length is "
             "[T; " + tp + "]-shaped, or instantiate through a type that "
             "fixes it",
             loc);
        return nullptr;
      }
      continue;
    }
    if (!tmap.count(tp)) {
      diag(Diag::Error, Code::Generic,
           "cannot infer type parameter " + tp + " of " + tmpl->name +
           " from the call arguments",
           loc);
      return nullptr;
    }
    if (!typeSatisfiesConstraint(tmap[tp], cons)) {
      // fsht-02:02: split the message by part — a missing user-trait impl
      // is F0218, everything else is the generic constraint failure
      std::string missing;
      size_t p = 0;
      while (true) {
        size_t q = cons.find('+', p);
        std::string part = cons.substr(p, q == std::string::npos
                                        ? std::string::npos : q - p);
        if (findTraitDef(part) && !findImplFor(part, tmap[tp])) {
          // a user trait by this name exists (even if it shares a builtin
          // constraint's name) and the type has no impl — the precise
          // diagnostic is F0218
          missing = part;
          break;
        }
        if (q == std::string::npos) break;
        p = q + 1;
      }
      if (!missing.empty())
        diag(Diag::Error, Code::NoTraitImpl,
             "type argument " + typeToString(tmap[tp]) + " for " + tp +
             " does not implement the trait " + missing +
             " (fn " + tmpl->name + ") — add an `impl " + missing + " for " +
             typeToString(tmap[tp]) + "` block",
             loc);
      else
        diag(Diag::Error, Code::Generic,
             "type argument " + typeToString(tmap[tp]) + " for " + tp +
             " does not satisfy constraint " + cons + " (fn " + tmpl->name + ")",
             loc);
      return nullptr;
    }
  }
  // 3. mangled instance name; intern (const values join the mangling:
  // sum$f64$4)
  std::string mangled = tmpl->name;
  for (auto& tp : tmpl->tparams) {
    if (tmap.count(tp)) mangled += "$" + mangleType(tmap[tp]);
    else if (cenv.count(tp)) mangled += "$" + std::to_string(cenv[tp]);
  }
  if (FnDecl* existing = lookupFn(mangled)) return existing;
  // 4. clone the declaration with substituted types (Phase 3a: bound
  // under the TEMPLATE's module context)
  if (moduleLookupIsStrict() && !tmpl->module.empty())
    pushModuleContext(tmpl->module);
  FnDecl* inst = new FnDecl();
  inst->name = mangled;
  inst->loc = tmpl->loc;
  // Phase 3a: instance module + pub-ness (the mangled instance name is
  // looked up cross-module after the first instantiation)
  inst->module = tmpl->module;
  inst->isPub = tmpl->isPub;
  inst->isTest = false;
  inst->strictfp = tmpl->strictfp;
  inst->fastmath = tmpl->fastmath;
  inst->userPure = tmpl->userPure;
  inst->exported = tmpl->exported;
  inst->imported = tmpl->imported;
  inst->hpcStrict = tmpl->hpcStrict;
  inst->unsafeFn = tmpl->unsafeFn;
  inst->userInline = tmpl->userInline;
  inst->userNoinline = tmpl->userNoinline;
  inst->aliasPairs = tmpl->aliasPairs;
  inst->isConst = tmpl->isConst;   // fsht-02:03: instances stay const fn
  for (auto& pm : tmpl->params) {
    Param np;
    np.name = pm.name;
    np.loc = pm.loc;
    np.owned = pm.owned;
    np.borrowed = pm.borrowed;
    if (!pm.tyName.empty()) {
      np.ty = bindEncType(prog, pm.tyName, pm.loc, &tmap, &cenv);
      if (!np.ty) np.ty = pm.ty;
    } else {
      np.ty = pm.ty;
    }
    inst->params.push_back(np);
  }
  if (!tmpl->retTyName.empty()) {
    inst->ret = bindEncType(prog, tmpl->retTyName, tmpl->loc, &tmap, &cenv);
    if (!inst->ret) inst->ret = tmpl->ret;
  }
  inst->body = cloneStmt(tmpl->body);
  inst->instTMap = tmap;
  inst->instCMap = cenv;   // fsht-02:03: const values for the body binder
  // fsht-02:02: rewrite Trait::method / T::method calls the tmap already
  // determines into direct mangled impl calls; the rest is picked up by
  // the instance body sema (typed arguments) through the same resolver
  rewriteTraitCallsInBody(inst->body, tmpl, tmap, prog);
  inst->id = (int)prog->fns.size();
  prog->fns.push_back(inst);
  // intern NOW: concurrent call sites resolve to this one instance
  registerGenericInstance(inst);
  if (moduleLookupIsStrict() && !tmpl->module.empty())
    popModuleContext();
  return inst;
}

// exposed for sema: resolve the parser's encoding (with optional tmap;
// fsht-02:03 adds the const-parameter environment for array lengths and
// const arguments)
Type* bindTypeEncoding(Program* prog, const std::string& enc, SrcLoc loc,
                       const std::map<std::string, Type*>* tmap,
                       const std::map<std::string, uint64_t>* cenv) {
  return bindEncType(prog, enc, loc, tmap, cenv);
}

} // namespace freshet
