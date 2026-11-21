// fsht-02:02 (registry 06.02.01) — user-defined traits with static dispatch.
//
// Architecture (see common.h's TraitDef/ImplDef comments):
//   * a `trait` block is a signature set; `Self` is the implementing-type
//     placeholder inside its encodings.
//   * an `impl Trait for Type` block's member functions are ordinary
//     FnDecls.  Sema resolves the target type, checks every member
//     against the declared signature, and renames it to the mangled
//     `Trait$<type>$method` — from that point the impl function is an
//     ordinary function for EVERY downstream pass.  Static dispatch is
//     therefore a direct call by construction: no vtables, no dyn Trait,
//     monomorphisation-friendly.
//   * calls spell `Trait::method(args)` / `T::method(args)` (inside
//     generic fns) / `Self::method(args)` (inside impls; the parser
//     already re-targets Self:: to the trait path).  Resolution:
//       - inside a generic instance: rewriteTraitCallsInBody() uses the
//         instantiation's type map before the instance body is sema'd;
//         anything it cannot prove is left for the body sema, which sees
//         typed arguments and retries.
//       - in concrete code (incl. instance bodies): resolveTraitMethodCall()
//         picks the impl from the first argument whose type matches an
//         impl target, from an enclosing impl's target, or — for a
//         zero-argument method with exactly one impl — the single impl.
#include "common.h"
#include <algorithm>

namespace freshet {

// ---------------------------------------------------------------- registry
static std::vector<TraitDef*>& traitTable() {
  static std::vector<TraitDef*> tab;
  return tab;
}
static std::vector<ImplDef*>& implTable() {
  static std::vector<ImplDef*> tab;
  return tab;
}

TraitDef* findTraitDef(const std::string& name) {
  for (auto* t : traitTable()) if (t->name == name) return t;
  return nullptr;
}
void registerTrait(TraitDef* td) {
  if (findTraitDef(td->name)) {
    diag(Diag::Error, Code::Dup, "duplicate trait: " + td->name, td->loc);
    return;
  }
  traitTable().push_back(td);
}
std::vector<TraitDef*> allTraitDefs() { return traitTable(); }
void registerImpl(ImplDef* im) { implTable().push_back(im); }
std::vector<ImplDef*>& allImpls() { return implTable(); }

TraitDef::Method* traitMethod(TraitDef* td, const std::string& m) {
  if (!td) return nullptr;
  for (auto& mm : td->methods) if (mm.name == m) return &mm;
  return nullptr;
}
// exported for sema's impl signature check
TraitDef::Method* findTraitMethodIn(const std::string& trait,
                                   const std::string& method) {
  return traitMethod(findTraitDef(trait), method);
}

// ---------------------------------------------------------------- Self subst
// Replace every `Self` appearing as a COMPLETE encoding segment with the
// target encoding.  Segments are delimited by string start/end, ':', ';',
// '(' and ')' — the binder grammar is prefix-coded over those separators
// (names cannot contain them), so "Self" can only ever appear as a whole
// segment:  "Self" -> "i64", "*:Self" -> "*:i64",
// "gen:Pair(Self;f64)" -> "gen:Pair(i64;f64)".
std::string substSelfInEnc(const std::string& enc, const std::string& selfEnc) {
  if (enc.empty() || selfEnc.empty()) return enc;
  if (enc == "Self") return selfEnc;
  std::string out;
  out.reserve(enc.size() + 8);
  size_t i = 0;
  while (i < enc.size()) {
    // is a "Self" run here delimited on both sides?
    if (enc.compare(i, 4, "Self") == 0) {
      bool leftOk = i == 0 || enc[i-1] == ':' || enc[i-1] == ';' ||
                    enc[i-1] == '(' || enc[i-1] == ')';
      size_t j = i + 4;
      bool rightOk = j >= enc.size() || enc[j] == ':' || enc[j] == ';' ||
                     enc[j] == '(' || enc[j] == ')';
      if (leftOk && rightOk) {
        out += selfEnc;
        i = j;
        continue;
      }
    }
    out += enc[i++];
  }
  return out;
}

// ---------------------------------------------------------------- mangling
// The impl fn's final name: `Trait$<mangleType>$method`.  mangleType is
// generics.cpp's private mangler; a small local twin avoids widening the
// internal surface (it must stay byte-identical — both sides derive from
// the same Type*).
static std::string mangleTraitTy(Type* t) {
  if (!t) return "v";
  switch (t->k) {
    case Type::U8: return "u8";   case Type::U16: return "u16";
    case Type::U32: return "u32"; case Type::U64: case Type::USIZE: return "u64";
    case Type::I8: return "i8";   case Type::I16: return "i16";
    case Type::I32: return "i32"; case Type::I64: return "i64";
    case Type::F32: return "f32"; case Type::F64: return "f64";
    case Type::BOOL: return "bool"; case Type::VOID: return "void";
    case Type::STR: return "str";
    case Type::PTR: return "p" + mangleTraitTy(t->pointee);
    case Type::SPAN: return "s" + mangleTraitTy(t->pointee);
    case Type::ARRAY: return "a" + std::to_string(t->arrLen) + "_" +
                             mangleTraitTy(t->pointee);
    case Type::STRUCT: return "S" + t->sdef->name;
    case Type::SOA: return "O" + t->sdef->name;
    case Type::ENUM: return "E" + t->edef->name;
    default: return "?";
  }
}

std::string traitMethodMangle(const std::string& trait, Type* selfTy,
                              const std::string& method) {
  return trait + "$" + mangleTraitTy(selfTy) + "$" + method;
}

// ---------------------------------------------------------------- impl lookup
ImplDef* findImplFor(const std::string& trait, Type* selfTy) {
  if (!selfTy) return nullptr;
  for (auto* im : implTable())
    if (im->traitName == trait && im->selfTy == selfTy) return im;
  return nullptr;
}

// the impl fn for `trait`/`method` on `selfTy`, if (transitively) present
static FnDecl* implFnFor(const std::string& trait, const std::string& method,
                         Type* selfTy) {
  ImplDef* im = findImplFor(trait, selfTy);
  if (!im) return nullptr;
  for (auto* f : im->fns) {
    if (!f->isTraitImpl) continue;
    // the fn's name IS the mangled form after sema's rename; match on the
    // method tail to stay robust before/after renaming
    if (f->traitName == trait) {
      std::string tail = "$" + method;
      if (f->name == method ||  // pre-rename (diagnostics only)
          f->name.size() > tail.size() &&
            f->name.compare(f->name.size() - tail.size(), tail.size(), tail) == 0)
        return f;
    }
  }
  return nullptr;
}

// ---------------------------------------------------------------- resolution
FnDecl* resolveTraitMethodCall(const std::string& prefix,
                               const std::string& method,
                               const std::vector<Type*>& argTys,
                               Type* preferSelf, SrcLoc loc) {
  // 1. prefix is a registered trait
  if (TraitDef* td = findTraitDef(prefix)) {
    if (!traitMethod(td, method)) {
      diag(Diag::Error, Code::TraitImplMismatch,
           "trait " + prefix + " has no method '" + method + "' — its "
           "methods are: " + [&]{
             std::string s;
             for (auto& m : td->methods) s += (s.empty()?"":" ") + m.name;
             return s;
           }(),
           loc);
      return nullptr;
    }
    // (a) caller-known implementing type (impl bodies, unique bound param)
    if (preferSelf && implFnFor(prefix, method, preferSelf)) {
      return implFnFor(prefix, method, preferSelf);
    }
    // (b) first argument whose type is an impl target of this trait
    for (Type* at : argTys) {
      if (!at) continue;
      if (FnDecl* f = implFnFor(prefix, method, at)) return f;
    }
    // (c) zero-argument method with a single impl of this trait
    if (argTys.empty()) {
      ImplDef* only = nullptr;
      int n = 0;
      for (auto* im : implTable())
        if (im->traitName == prefix) { only = im; ++n; }
      if (n == 1) return implFnFor(prefix, method, only->selfTy);
      if (n == 0) {
        diag(Diag::Error, Code::NoTraitImpl,
             "no impl of " + prefix + " exists — cannot call " + prefix +
             "::" + method, loc);
        return nullptr;
      }
    }
    diag(Diag::Error, Code::TraitCallAmbig,
         "call " + prefix + "::" + method +
         " does not determine its implementing type — pass a `" +
         prefix + "`-typed argument (first argument types: " + [&]{
           std::string s;
           for (auto* t : argTys) s += (s.empty()?"":" ") + typeToString(t);
           return s.empty() ? "none" : s;
         }() + "), or call it inside a generic fn bound by " + prefix,
         loc);
    return nullptr;
  }
  // 2. prefix is a concrete TYPE name: any trait impl on that type
  //    providing the method (`Rect::area()`)
  Type* pty = nullptr;
  if (StructDef* sd = findStructDef(prefix)) pty = Type::structOf(sd);
  if (!pty) if (EnumDef* ed = findEnumDef(prefix)) pty = Type::enumOf(ed);
  if (!pty) return nullptr;    // not a trait, not a type: caller's problem
  FnDecl* found = nullptr;
  ImplDef* foundIm = nullptr;
  for (auto* im : implTable()) {
    if (im->selfTy != pty) continue;
    if (!traitMethod(findTraitDef(im->traitName), method)) continue;
    FnDecl* f = implFnFor(im->traitName, method, pty);
    if (f) {
      if (found && foundIm != im) {
        diag(Diag::Error, Code::TraitCallAmbig,
             "method '" + method + "' of " + prefix +
             " is provided by multiple traits (" + foundIm->traitName +
             ", " + im->traitName + ") — qualify the call as " +
             foundIm->traitName + "::" + method, loc);
        return nullptr;
      }
      found = f;
      foundIm = im;
    }
  }
  if (!found) {
    diag(Diag::Error, Code::NoTraitImpl,
         "type " + prefix + " has no trait method '" + method + "'", loc);
    return nullptr;
  }
  return found;
}

// ---------------------------------------------------------------- rewriting
// Instance-body pass: rewrite `Trait::method` / `T::method` calls whose
// implementing type the tmap already determines, so the instance body
// sema (and irgen) sees plain direct calls.  Unprovable calls are left
// untouched for the body sema's typed-argument retry.

// does this (untyped) argument expression mention the template parameter
// `tp`?  A conservative IDENT walk: `x`, `x.field`, `a[i]`, `f(x)` — any
// path through a binding named like the parameter, plus cast targets.
static bool exprMentionsParam(Expr* e, const std::string& tp) {
  if (!e) return false;
  if (e->k == Expr::IDENT && e->name == tp) return true;
  if (exprMentionsParam(e->lhs, tp) || exprMentionsParam(e->rhs, tp) ||
      exprMentionsParam(e->base, tp))
    return true;
  for (auto* a : e->args)
    if (exprMentionsParam(a, tp)) return true;
  return false;
}

// does the arg list mention exactly ONE distinct type tparam (return it)?
static const std::string* soleMentionedTParam(
    const std::vector<Expr*>& args, const std::vector<std::string>& tparams) {
  const std::string* found = nullptr;
  for (auto* a : args)
    for (auto& tp : tparams)
      if (exprMentionsParam(a, tp)) {
        if (found && *found != tp) return nullptr;   // two different params
        found = &tp;
      }
  return found;
}

static void rewriteCalls(Expr* e, FnDecl* tmpl,
                         const std::map<std::string, Type*>& tmap,
                         Program* prog) {
  if (!e) return;
  for (auto* a : e->args) rewriteCalls(a, tmpl, tmap, prog);
  rewriteCalls(e->lhs, tmpl, tmap, prog);
  rewriteCalls(e->rhs, tmpl, tmap, prog);
  rewriteCalls(e->base, tmpl, tmap, prog);
  if (e->k != Expr::CALL) return;
  size_t dc = e->name.find("::");
  if (dc == std::string::npos) return;
  std::string prefix = e->name.substr(0, dc);
  std::string method = e->name.substr(dc + 2);
  Type* selfTy = nullptr;
  // (a) `T::method` where T is a type tparam of THIS template
  auto it = tmap.find(prefix);
  if (it != tmap.end()) selfTy = it->second;
  // (b) `Trait::method` — the implementing type comes from the argument
  //     expressions mentioning exactly one tparam bound by that trait
  if (!selfTy && findTraitDef(prefix)) {
    const std::string* bound = nullptr;
    for (size_t i = 0; i < tmpl->tparams.size(); ++i) {
      const std::string& tp = tmpl->tparams[i];
      const std::string& cons = i < tmpl->tconstraints.size()
                                ? tmpl->tconstraints[i] : "";
      // constraint contains the trait as a `+`-separated part?
      bool mentions = cons == prefix;
      size_t p = 0;
      while (!mentions && p < cons.size()) {
        size_t q = cons.find('+', p);
        std::string part = cons.substr(p, q == std::string::npos
                                        ? std::string::npos : q - p);
        if (part == prefix) mentions = true;
        if (q == std::string::npos) break;
        p = q + 1;
      }
      if (mentions) {
        if (bound && *bound != tp) { bound = nullptr; break; }
        bound = &tp;
      }
    }
    if (bound) {
      auto bit = tmap.find(*bound);
      if (bit != tmap.end()) selfTy = bit->second;
    } else {
      // fall back: sole tparam mentioned by the call's arguments
      const std::string* sole = soleMentionedTParam(e->args, tmpl->tparams);
      if (sole) {
        auto sit = tmap.find(*sole);
        if (sit != tmap.end() && implFnFor(prefix, method, sit->second))
          selfTy = sit->second;
      }
    }
  }
  if (!selfTy) return;    // left for the body sema's typed-argument retry
  FnDecl* target = implFnFor(prefix, method, selfTy);
  if (!target) {
    // constraint says the type implements the trait — it does not
    diag(Diag::Error, Code::NoTraitImpl,
         "no impl of " + prefix + " for " + typeToString(selfTy) +
         " (required by the " + tmpl->name + " instantiation)",
         e->loc);
    return;
  }
  e->name = target->name;   // the mangled direct-call name
  e->qualModule.clear();
}

static void rewriteStmts(Stmt* s, FnDecl* tmpl,
                         const std::map<std::string, Type*>& tmap,
                         Program* prog) {
  if (!s) return;
  rewriteCalls(s->init, tmpl, tmap, prog);
  rewriteCalls(s->lhs, tmpl, tmap, prog);
  rewriteCalls(s->rhs, tmpl, tmap, prog);
  rewriteCalls(s->retVal, tmpl, tmap, prog);
  rewriteCalls(s->deferExpr, tmpl, tmap, prog);
  rewriteCalls(s->cond, tmpl, tmap, prog);
  rewriteCalls(s->expr, tmpl, tmap, prog);
  rewriteCalls(s->matchOn, tmpl, tmap, prog);
  rewriteCalls(s->forInit, tmpl, tmap, prog);
  rewriteCalls(s->forCond, tmpl, tmap, prog);
  rewriteCalls(s->forStep, tmpl, tmap, prog);
  rewriteCalls(s->rangeLo, tmpl, tmap, prog);
  rewriteCalls(s->rangeHi, tmpl, tmap, prog);
  rewriteCalls(s->plo, tmpl, tmap, prog);
  rewriteCalls(s->phi, tmpl, tmap, prog);
  rewriteCalls(s->lhs, tmpl, tmap, prog);
  rewriteStmts(s->thenB, tmpl, tmap, prog);
  rewriteStmts(s->elseB, tmpl, tmap, prog);
  rewriteStmts(s->body, tmpl, tmap, prog);
  for (auto& arm : s->matchArms) rewriteStmts(arm.body, tmpl, tmap, prog);
  for (auto* st : s->stmts) rewriteStmts(st, tmpl, tmap, prog);
}

void rewriteTraitCallsInBody(Stmt* body, FnDecl* tmpl,
                             const std::map<std::string, Type*>& tmap,
                             Program* prog) {
  if (!body || !tmpl) return;
  for (auto* st : body->stmts) rewriteStmts(st, tmpl, tmap, prog);
}

// ---------------------------------------------------------------- checking
// does a type satisfy a constraint part?  (generics.cpp's private twin is
// extended here because trait bounds must be honoured everywhere a
// constraint is checked — one shared implementation, no drift)
bool typeSatisfiesConstraintPart(Type* t, const std::string& part) {
  if (part.empty()) return true;
  if (part == "Numeric") return t && t->isNum();
  if (part == "Int") return t && t->isInt();
  if (part == "Float") return t && t->isFloat();
  if (part == "Signed") return t && t->isSigned();
  // user trait: an impl for exactly this type
  if (findTraitDef(part)) return t && findImplFor(part, t) != nullptr;
  return false;   // unknown constraint (validated at declaration)
}

bool typeSatisfiesConstraints(Type* t, const std::string& cons) {
  if (cons.empty()) return true;
  size_t p = 0;
  while (true) {
    size_t q = cons.find('+', p);
    std::string part = cons.substr(p, q == std::string::npos
                                    ? std::string::npos : q - p);
    if (!typeSatisfiesConstraintPart(t, part)) return false;
    if (q == std::string::npos) break;
    p = q + 1;
  }
  return true;
}

// is a constraint list well-formed (builtins or declared traits)?
std::string firstUnknownConstraintPart(const std::string& cons) {
  if (cons.empty()) return "";
  size_t p = 0;
  while (true) {
    size_t q = cons.find('+', p);
    std::string part = cons.substr(p, q == std::string::npos
                                    ? std::string::npos : q - p);
    if (part != "Numeric" && part != "Int" && part != "Float" &&
        part != "Signed" && !findTraitDef(part))
      return part;
    if (q == std::string::npos) return "";
    p = q + 1;
  }
}

} // namespace freshet
