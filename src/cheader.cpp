// fsht tooling surface: automatic C header generation (--emit-c-header).
//
// Emits a C header describing a compilation's C ABI boundary: every
// `export fn` declaration (external linkage keeps the plain symbol name
// — modules.cpp irFnSymbol), the structs/enums those prototypes need,
// and the { ptr, len } pair typedefs Freshet lowers Span<T>/str to.
//
// The emitted declarations mirror EXACTLY what irgen_stmt.cpp lowers:
//   - scalars (u8..u64/usize, i8..i64, f32/f64, bool) — by value
//   - STRUCT/ENUM/ARRAY parameters — by ADDRESS, the callee copies
//     (by-value semantics; irgen "Parameter passing convention")
//   - Span<T>/str — first-class { ptr, i64 } aggregates, by value
//   - fn pointers — plain C function pointers
//   - enums — { i64 tag, [paySize x i8] payload } (irgen %enum. shape)
//   - aggregate returns — by value (LLVM direct aggregate return; the
//     same SysV rule Clang compiles C structs under)
//   - `main` is skipped: C already owns that symbol
//
// Exported functions whose surface is not C-representable (soa(...)
// parameters, generic functions) are listed as skipped comments rather
// than silently dropped.

#include "common.h"
#include <cstdio>
#include <set>
#include <map>
#include <string>
#include <vector>

namespace freshet {

namespace {

std::string cSanitize(const std::string& n) {
  std::string out;
  for (char c : n)
    out += (isalnum((unsigned char)c) || c == '_') ? c : '_';
  return out.empty() ? "anon" : out;
}

struct HeaderGen {
  std::vector<std::string> lines;
  // dependency queues (topological emission below)
  std::vector<const StructDef*> structQueue;
  std::set<const StructDef*> structsQueued;
  std::set<const StructDef*> structsEmitted;
  std::vector<const EnumDef*> enumQueue;
  std::set<const EnumDef*> enumsQueued;
  std::set<const EnumDef*> enumsEmitted;
  // Span<T> element types seen (dedup by sanitized name)
  std::map<std::string, Type*> spanElems;
  bool strUsed = false;
  // fn-pointer signatures seen -> typedef name
  std::map<std::string, std::string> fnptrNames;
  int fnptrCount = 0;
  // exported-but-unrepresentable functions, for the trailer comment
  std::vector<std::pair<std::string, std::string>> skipped;

  void queueStruct(const StructDef* sd) {
    if (!sd || structsQueued.count(sd)) return;
    structsQueued.insert(sd);
    structQueue.push_back(sd);
  }
  void queueEnum(const EnumDef* ed) {
    if (!ed || enumsQueued.count(ed)) return;
    enumsQueued.insert(ed);
    enumQueue.push_back(ed);
  }

  // The C spelling of `t`.  `param` selects the by-address convention for
  // aggregate PARAMETERS; returns "" when t is not C-representable (and
  // the caller skips that function / struct).
  std::string mapType(Type* t, bool param) {
    if (!t) return "";
    switch (t->k) {
      case Type::U8:  return "uint8_t";
      case Type::U16: return "uint16_t";
      case Type::U32: return "uint32_t";
      case Type::U64: case Type::USIZE: return "uint64_t";
      case Type::I8:  return "int8_t";
      case Type::I16: return "int16_t";
      case Type::I32: return "int32_t";
      case Type::I64: return "int64_t";
      case Type::F32: return "float";
      case Type::F64: return "double";
      case Type::BOOL: return "bool";
      case Type::VOID: return "void";
      case Type::PTR: {
        std::string elem = t->pointee ? mapType(t->pointee, false) : "";
        if (elem.empty()) elem = "void";   // void* is a legal fallback
        return elem + "*";
      }
      case Type::STR:
        strUsed = true;
        return "fsh_str";
      case Type::SPAN: {
        std::string elem = t->pointee ? mapType(t->pointee, false) : "";
        if (elem.empty()) return "";
        std::string nm = "fsh_span_" + cSanitize(elem);
        spanElems[nm] = t->pointee;
        return nm;
      }
      case Type::STRUCT:
        if (t->sdef && t->sdef->isGeneric && t->sdef->templateOf == nullptr)
          return "";                       // the bare template itself
        queueStruct(t->sdef);
        return t->sdef ? cSanitize(t->sdef->name) : "";
      case Type::ENUM:
        queueEnum(t->edef);
        return t->edef ? cSanitize(t->edef->name) : "";
      case Type::ARRAY: {
        // parameters pass by address (callee copies — the callee knows N);
        // struct FIELDS keep the in-line array form
        std::string elem = t->pointee ? mapType(t->pointee, false) : "";
        if (elem.empty()) return "";
        if (param) return elem + "*";
        return elem + "[" + std::to_string(t->arrLen) + "]";
      }
      case Type::FNPTR: {
        std::string ret = mapType(t->pointee, false);
        if (ret.empty()) return "";
        std::string sig = ret + "(";
        for (size_t i = 0; i < t->fnParams.size(); ++i) {
          std::string pt = mapType(t->fnParams[i], false);
          if (pt.empty()) return "";
          if (i) sig += ", ";
          sig += pt;
        }
        sig += ")";
        auto it = fnptrNames.find(sig);
        if (it == fnptrNames.end()) {
          std::string nm = "fsh_fn_" + std::to_string(fnptrCount++);
          fnptrNames[sig] = nm;
          return nm;
        }
        return it->second;
      }
      case Type::SOA:
      default:
        return "";                          // not C-representable
    }
  }

  // struct field name -> C decl fragment; "" skips the struct entirely
  void emitStruct(const StructDef* sd) {
    if (!sd || structsEmitted.count(sd)) return;
    structsEmitted.insert(sd);
    // dependencies first (by-value fields must be complete)
    for (auto& f : sd->fields) {
      if (f.ty && f.ty->k == Type::STRUCT && f.ty->sdef &&
          f.ty->sdef != sd)
        emitStruct(f.ty->sdef);
      if (f.ty && f.ty->k == Type::ENUM && f.ty->edef)
        emitEnum(f.ty->edef);
    }
    if (sd->alignOverride > 0)
      lines.push_back("/* align(" + std::to_string(sd->alignOverride) +
                      ") */");
    lines.push_back("struct " + cSanitize(sd->name) + " {");
    for (auto& f : sd->fields) {
      std::string ct = mapType(f.ty, false);
      if (ct.empty()) {
        // field not representable: whole struct is off the C surface
        lines.push_back("  /* " + f.name + ": not C-representable */");
        continue;
      }
      std::string decl = "  " + ct;
      if (!decl.empty() && decl.back() != '*' && decl.back() != ']')
        decl += " ";
      lines.push_back(decl + f.name + ";");
    }
    lines.push_back("};");
    lines.push_back("typedef struct " + cSanitize(sd->name) + " " +
                    cSanitize(sd->name) + ";");
    lines.push_back("");
  }

  void emitEnum(const EnumDef* ed) {
    if (!ed || enumsEmitted.count(ed)) return;
    enumsEmitted.insert(ed);
    lines.push_back("/* enum " + ed->name + ": tagged union { i64 tag; " +
                    "[payload x i8] } — variant tags follow */");
    lines.push_back("struct " + cSanitize(ed->name) + " {");
    lines.push_back("  int64_t tag;");
    if (ed->paySize > 0)
      lines.push_back("  unsigned char payload[" + std::to_string(ed->paySize) +
                      "];");
    lines.push_back("};");
    lines.push_back("typedef struct " + cSanitize(ed->name) + " " +
                    cSanitize(ed->name) + ";");
    for (auto& v : ed->variants)
      lines.push_back("/* " + cSanitize(ed->name) + "_" + v.name + " = " +
                      std::to_string(v.idx) + " */");
    lines.push_back("");
  }
};

} // namespace

bool emitCHeader(Program* p, const std::string& path) {
  if (!p) return false;
  HeaderGen g;

  // ---- pick the exported surface --------------------------------------
  std::vector<FnDecl*> exported;
  for (auto* f : p->fns) {
    if (!f || !f->exported || f->imported) continue;
    if (f->name == "main") continue;      // C owns main's symbol
    if (f->isGeneric) continue;           // no monomorphised instance is
                                          // exported under this name
    exported.push_back(f);
  }

  // ---- render prototypes (collecting type dependencies) ----------------
  struct Proto {
    std::vector<std::string> comments;
    std::string sig;
  };
  std::vector<Proto> protos;
  for (auto* f : exported) {
    std::string ret = g.mapType(f->ret, false);
    if (ret.empty()) {
      g.skipped.push_back({f->name, "return type is not C-representable"});
      continue;
    }
    std::string sig = ret + " " + cSanitize(f->name) + "(";
    bool ok = true;
    Proto pr;
    for (size_t i = 0; i < f->params.size(); ++i) {
      Param& pm = f->params[i];
      Type* pt = pm.ty;
      bool byAddress = pt && (pt->k == Type::STRUCT || pt->k == Type::ENUM ||
                              pt->k == Type::ARRAY);
      std::string ct = g.mapType(pt, byAddress);
      if (ct.empty()) {
        g.skipped.push_back({f->name, "parameter '" + pm.name +
                                          "' is not C-representable"});
        ok = false;
        break;
      }
      if (byAddress) {
        // shared borrows are const; owned / borrowed mut / plain by-value
        // params hand the callee writable memory
        bool readOnly = pm.borrowed && !pm.borrowedMut;
        ct = readOnly ? "const " + ct + "*" : ct + "*";
        std::string note = pm.owned ? "owned - callee frees"
                          : pm.borrowedMut ? "borrowed mut - callee may write"
                          : readOnly ? "borrowed - read-only view"
                          : "by value - callee copies";
        pr.comments.push_back(pm.name + ": " + note);
      }
      if (pt && pt->k == Type::ARRAY)
        pr.comments.push_back(pm.name + ": array length " +
                              std::to_string(pt->arrLen) +
                              " (known to the callee)");
      if (i) sig += ", ";
      sig += ct + (pm.name.empty() ? "" : " " + pm.name);
    }
    if (!ok) continue;
    if (f->params.empty()) sig += "void";
    sig += ")";
    pr.sig = sig + ";";
    protos.push_back(std::move(pr));
  }

  // ---- assemble the header ----------------------------------------------
  // Close the type-dependency queue first: field types queue more types,
  // and the forward-declaration pass below must see every one of them.
  // (Index-based loops: queueStruct may append while we walk.)
  for (size_t i = 0; i < g.structQueue.size(); ++i)
    for (auto& f : g.structQueue[i]->fields)
      g.mapType(f.ty, false);
  for (size_t i = 0; i < g.enumQueue.size(); ++i)
    for (auto& v : g.enumQueue[i]->variants)
      for (auto& pf : v.fields)
        g.mapType(pf.ty, false);

  std::vector<std::string> out;
  out.push_back("/* C header for this Freshet compilation.");
  out.push_back(" * Generated by penstock --emit-c-header (fsht tooling,");
  out.push_back(" * registry 07.04.01 surface).  Prototypes mirror the");
  out.push_back(" * emitted LLVM ABI exactly: aggregate parameters pass by");
  out.push_back(" * address (the callee copies), Span<T>/str are { ptr, len }");
  out.push_back(" * pairs, enums are { tag, payload } unions.");
  out.push_back(" */");
  out.push_back("#ifndef FRESHET_" + cSanitize(path) + "_H");
  out.push_back("#define FRESHET_" + cSanitize(path) + "_H");
  out.push_back("");
  out.push_back("#include <stdint.h>");
  out.push_back("#include <stdbool.h>");
  out.push_back("#include <stddef.h>");
  out.push_back("");
  out.push_back("#ifdef __cplusplus");
  out.push_back("extern \"C\" {");
  out.push_back("#endif");
  out.push_back("");

  // forward declarations first: pointer fields may reference any struct
  // before its body is emitted
  for (auto* sd : g.structQueue)
    out.push_back("typedef struct " + cSanitize(sd->name) + " " +
                  cSanitize(sd->name) + ";");
  for (auto* ed : g.enumQueue)
    out.push_back("typedef struct " + cSanitize(ed->name) + " " +
                  cSanitize(ed->name) + ";");
  if (!g.structQueue.empty() || !g.enumQueue.empty()) out.push_back("");

  // bodies in dependency order (emitStruct recurses into dependencies;
  // index loops again — late-queued types are already in the queue)
  for (size_t i = 0; i < g.structQueue.size(); ++i)
    g.emitStruct(g.structQueue[i]);
  for (size_t i = 0; i < g.enumQueue.size(); ++i)
    g.emitEnum(g.enumQueue[i]);
  out.insert(out.end(), g.lines.begin(), g.lines.end());

  // pair typedefs
  if (g.strUsed) {
    out.push_back("typedef struct { const char* ptr; int64_t len; } fsh_str;");
    out.push_back("");
  }
  for (auto& kv : g.spanElems) {
    out.push_back("typedef struct { const " + g.mapType(kv.second, false) +
                  "* ptr; int64_t len; } " + kv.first + ";");
  }
  if (!g.spanElems.empty()) out.push_back("");

  // fn-pointer typedefs
  for (auto& kv : g.fnptrNames) {
    size_t paren = kv.first.find('(');
    out.push_back("typedef " + kv.first.substr(0, paren) + " (*" +
                  kv.second + ")" + kv.first.substr(paren) + ";");
  }
  if (!g.fnptrNames.empty()) out.push_back("");

  // prototypes
  out.push_back("/* ---- exported functions ---- */");
  for (auto& pr : protos) {
    for (auto& c : pr.comments) out.push_back("/* " + c + " */");
    out.push_back(pr.sig);
  }
  out.push_back("");

  if (!g.skipped.empty()) {
    out.push_back("/* ---- exported but not C-representable ---- */");
    for (auto& s : g.skipped)
      out.push_back("/* " + s.first + ": " + s.second + " */");
    out.push_back("");
  }

  out.push_back("#ifdef __cplusplus");
  out.push_back("}");
  out.push_back("#endif");
  out.push_back("#endif /* FRESHET_" + cSanitize(path) + "_H */");

  FILE* fp = fopen(path.c_str(), "w");
  if (!fp) return false;
  for (auto& l : out) fprintf(fp, "%s\n", l.c_str());
  fclose(fp);
  return true;
}

} // namespace freshet
