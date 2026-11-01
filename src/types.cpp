// Freshet type system — builtin types, struct registry, layout computation
// (R2: cache-line aware struct layouts with padding analysis), equivalence.
#include "common.h"

namespace freshet {

static std::map<std::string, std::vector<StructDef*>>& registry() {
  static std::map<std::string, std::vector<StructDef*>> r;
  return r;
}

static uint64_t typeSize(Type* t);
static uint64_t typeAlign(Type* t);

// Phase 3a: module-aware resolution — same-named structs in different
// modules coexist; the visible one (own module / pub + imported / prelude)
// is picked, with F0004 diagnostics for private/ambiguous cases.
static StructDef* findStructVisible(const std::string& name, SrcLoc loc) {
  auto& r = registry();
  auto it = r.find(name);
  if (it == r.end()) return nullptr;
  return pickVisible(it->second, name, "struct", loc);
}

Type* resolveTypeName(const std::string& name) {
  StructDef* sd = findStructVisible(name, SrcLoc{});
  return sd ? Type::structOf(sd) : nullptr;
}

// ---------------------------------------------------------------- enums
static std::map<std::string, std::vector<EnumDef*>>& enumRegistry() {
  static std::map<std::string, std::vector<EnumDef*>> r;
  return r;
}

EnumDef* findEnumDef(const std::string& name) {
  auto& r = enumRegistry();
  auto it = r.find(name);
  if (it == r.end()) return nullptr;
  return pickVisible(it->second, name, "enum", SrcLoc{});
}

// Phase 3a: module-qualified enum lookup (see findStructInModule)
EnumDef* findEnumInModule(const std::string& name, const std::string& mod) {
  auto& r = enumRegistry();
  auto it = r.find(name);
  if (it == r.end()) return nullptr;
  for (auto* ed : it->second)
    if (ed->module == mod) return ed;
  return nullptr;
}

std::vector<EnumDef*> allEnumDefs() {
  auto& r = enumRegistry();
  std::vector<EnumDef*> out;
  for (auto& [n, v] : r) out.push_back(v.back());
  return out;
}

void registerEnum(EnumDef* ed) {
  auto& r = enumRegistry();
  // Phase 3a: same name in DIFFERENT modules is legal (private decls);
  // within one module it stays a hard error
  for (auto* o : r[ed->name])
    if (o->module == ed->module) {
      diag(Diag::Error, Code::Type, "duplicate enum definition: " + ed->name,
           ed->loc.file, ed->loc.line, ed->loc.col);
      return;
    }
  r[ed->name].push_back(ed);
}

// enum layout: { i64 tag, [paySize x i8] payload } aligned to payAlign.
// Each variant's payload is laid out like a mini-struct.
void computeEnumLayout(EnumDef* e) {
  if (e->size != 0) return;   // computed once
  e->paySize = 0;
  e->payAlign = 1;
  for (auto& v : e->variants) {
    v.offsets.assign(v.fields.size(), 0);
    v.sizes.assign(v.fields.size(), 0);
    uint64_t off = 0, maxa = 1;
    for (size_t i = 0; i < v.fields.size(); ++i) {
      Param& f = v.fields[i];
      uint64_t fa = typeAlign(f.ty);
      uint64_t fs = typeSize(f.ty);
      off = alignUp(off, fa);
      v.offsets[i] = off;
      v.sizes[i] = fs;
      off += fs;
      maxa = std::max(maxa, fa);
    }
    v.paySize = alignUp(off, maxa);
    v.payAlign = maxa;
    if (v.paySize > e->paySize) e->paySize = v.paySize;
    if (v.payAlign > e->payAlign) e->payAlign = v.payAlign;
  }
  e->align = std::max<uint64_t>(8, e->payAlign);
  // tag word first, then payload at its natural alignment
  uint64_t off = alignUp(8, e->payAlign);
  e->size = alignUp(off + e->paySize, e->align);
}

StructDef* findStructDef(const std::string& name) {
  return findStructVisible(name, SrcLoc{});
}

// Phase 3a: module-qualified lookup (mod::Name) — used by bindEncType's
// "qual:" / "gen:mod::Item" encodings.  Returns the decl registered in
// THAT module regardless of the current lookup context (visibility is
// checked by the caller).
StructDef* findStructInModule(const std::string& name, const std::string& mod) {
  auto& r = registry();
  auto it = r.find(name);
  if (it == r.end()) return nullptr;
  for (auto* sd : it->second)
    if (sd->module == mod) return sd;
  return nullptr;
}

void registerStruct(StructDef* sd) {
  auto& r = registry();
  // Phase 3a: same name in DIFFERENT modules is legal; within one
  // module it stays a hard error
  for (auto* o : r[sd->name])
    if (o->module == sd->module) {
      diag(Diag::Error, Code::Type, "duplicate struct definition: " + sd->name,
           sd->loc.file, sd->loc.line, sd->loc.col);
      return;
    }
  r[sd->name].push_back(sd);
}

static uint64_t typeSize(Type* t);
static uint64_t typeAlign(Type* t);

// LLVM-ish layout: struct align = max field align, size = aligned sum.
// Cached in StructDef (offsets/sizes/size/align/padBytes).
void computeStructLayout(StructDef* s) {
  if (s->size != 0 && !s->fields.empty() && s->offsets.size() == s->fields.size())
    return; // already computed
  s->align = s->alignOverride ? s->alignOverride : 1;
  s->offsets.assign(s->fields.size(), 0);
  s->sizes.assign(s->fields.size(), 0);
  uint64_t off = 0;
  uint64_t sumFields = 0;
  for (size_t i = 0; i < s->fields.size(); ++i) {
    Field& f = s->fields[i];
    uint64_t fa = typeAlign(f.ty);
    uint64_t fs = typeSize(f.ty);
    if (s->alignOverride) fa = std::min<uint64_t>(fa, s->alignOverride);
    s->align = std::max(s->align, fa);
    off = alignUp(off, fa);
    s->offsets[i] = off;
    s->sizes[i] = fs;
    off += fs;
    sumFields += fs;
  }
  s->size = alignUp(off, s->align);
  s->padBytes = s->size - sumFields;
}

static uint64_t typeSize(Type* t) {
  if (!t) return 0;
  switch (t->k) {
    case Type::U8: case Type::I8: case Type::BOOL: return 1;
    case Type::U16: case Type::I16: return 2;
    case Type::U32: case Type::I32: case Type::F32: return 4;
    case Type::U64: case Type::USIZE: case Type::I64: case Type::F64: return 8;
    case Type::PTR: return 8;
    case Type::FNPTR: return 8;                  // pointer-sized value
    case Type::STR: case Type::SPAN: return 16;   // { ptr, i64 } aggregate
    case Type::ARRAY: {
      // arrays wrap: size = len * elem (like C arrays)
      return t->arrLen * typeSize(t->pointee);
    }
    case Type::ENUM: {
      computeEnumLayout(t->edef);
      return t->edef->size;
    }
    case Type::STRUCT: case Type::SOA: {
      if (!t->sdef) return 0;   // unbound template placeholder
      computeStructLayout(t->sdef);
      return t->sdef->size;
    }
    default: return 0;
  }
}

static uint64_t typeAlign(Type* t) {
  if (!t) return 1;
  switch (t->k) {
    case Type::U8: case Type::I8: case Type::BOOL: return 1;
    case Type::U16: case Type::I16: return 2;
    case Type::U32: case Type::I32: case Type::F32: return 4;
    case Type::U64: case Type::USIZE: case Type::I64: case Type::F64:
    case Type::PTR: case Type::FNPTR: return 8;
    case Type::STR: case Type::SPAN: return 8;    // { ptr, i64 } aggregate
    case Type::ARRAY: return typeAlign(t->pointee);
    case Type::ENUM: {
      computeEnumLayout(t->edef);
      return t->edef->align;
    }
    case Type::STRUCT: case Type::SOA: {
      if (!t->sdef) return 1;   // unbound template placeholder
      computeStructLayout(t->sdef);
      return t->sdef->align;
    }
    default: return 1;
  }
}

uint64_t typeSizeOf(Type* t) { return typeSize(t); }
uint64_t typeAlignOf(Type* t) { return typeAlign(t); }

// Phase 3b (report §6.3): fn(P...) -> R — interned by structural identity.
Type* Type::fnptrOf(const std::vector<Type*>& params, Type* ret) {
  static std::vector<std::unique_ptr<Type>> pool;
  for (auto& t : pool) {
    if (t->k != FNPTR || t->pointee != ret || t->fnParams.size() != params.size())
      continue;
    bool same = true;
    for (size_t i = 0; i < params.size(); ++i)
      if (t->fnParams[i] != params[i]) { same = false; break; }
    if (same) return t.get();
  }
  pool.push_back(std::make_unique<Type>());
  pool.back()->k = FNPTR;
  pool.back()->pointee = ret;
  pool.back()->fnParams = params;
  return pool.back().get();
}

// u64 <-> usize are equivalent (documented language rule)
bool typesEquivalent(Type* a, Type* b) {
  if (!a || !b) return a == b;
  auto norm = [](Type* t) {
    if (t->k == Type::U64) return Type::USIZE;
    return t->k;
  };
  if (norm(a) != norm(b)) return false;
  if (a->k == Type::PTR || a->k == Type::SPAN)
    return typesEquivalent(a->pointee, b->pointee);
  if (a->k == Type::FNPTR) {
    if (!typesEquivalent(a->pointee, b->pointee)) return false;
    if (a->fnParams.size() != b->fnParams.size()) return false;
    for (size_t i = 0; i < a->fnParams.size(); ++i)
      if (!typesEquivalent(a->fnParams[i], b->fnParams[i])) return false;
    return true;
  }
  if (a->k == Type::ARRAY)
    return a->arrLen == b->arrLen && typesEquivalent(a->pointee, b->pointee);
  if (a->k == Type::STRUCT || a->k == Type::SOA)
    return a->sdef == b->sdef;
  if (a->k == Type::ENUM)
    return a->edef == b->edef;
  return true;   // scalars, str
}

// Implicit conversions allowed: exact equivalence, or int literal adoption
// (constant int expression coerces to any int type of sufficient range),
// f32<-f64 literal, usize<->u64, and widening int of same signedness.
bool typesCompatible(Type* to, Type* from) {
  if (typesEquivalent(to, from)) return true;
  if (!to || !from) return false;
  if (to->k == Type::USIZE && from->k == Type::U64) return true;
  if (to->k == Type::U64 && from->k == Type::USIZE) return true;
  // literal adoption handled in sema_expr via 'constant'; here: widening
  if (to->isInt() && from->isInt() && to->bits() > from->bits() &&
      to->isSigned() == from->isSigned())
    return true;
  if (to->k == Type::F64 && from->k == Type::F32) return true;
  return false;
}

Type* commonType(Type* a, Type* b) {
  if (typesEquivalent(a, b)) return a;
  if (!a || !b) return a ? a : b;
  if (a->isFloat() && b->isInt()) return a;
  if (b->isFloat() && a->isInt()) return b;
  if (a->isFloat() && b->isFloat()) return a->bits() >= b->bits() ? a : b;
  if (a->isInt() && b->isInt()) {
    if (a->bits() != b->bits()) return a->bits() >= b->bits() ? a : b;
    return a;  // mixed signedness at same width: signed (sema warns)
  }
  return nullptr;
}

std::string typeToString(Type* t) {
  if (!t) return "<null>";
  switch (t->k) {
    case Type::U8: return "u8"; case Type::U16: return "u16";
    case Type::U32: return "u32"; case Type::U64: return "u64";
    case Type::USIZE: return "usize"; case Type::I8: return "i8";
    case Type::I16: return "i16"; case Type::I32: return "i32";
    case Type::I64: return "i64"; case Type::F32: return "f32";
    case Type::F64: return "f64"; case Type::BOOL: return "bool";
    case Type::VOID: return "void";
    case Type::PTR: return "*" + typeToString(t->pointee);
    case Type::FNPTR: {
      std::string s = "fn(";
      for (size_t i = 0; i < t->fnParams.size(); ++i) {
        if (i) s += ", ";
        s += typeToString(t->fnParams[i]);
      }
      s += ") -> " + typeToString(t->pointee);
      return s;
    }
    case Type::STRUCT: return "struct " + t->sdef->name;
    case Type::SOA: return "soa(" + t->sdef->name + ")";
    case Type::STR: return "str";
    case Type::SPAN: return "Span<" + typeToString(t->pointee) + ">";
    case Type::ARRAY: return "[" + typeToString(t->pointee) + "; " +
                         std::to_string(t->arrLen) + "]";
    case Type::ENUM: return "enum " + t->edef->name;
  }
  return "?";
}

} // namespace freshet
