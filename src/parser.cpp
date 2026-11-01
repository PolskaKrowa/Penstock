// Freshet parser — recursive descent over preprocessed tokens.
// Handles: functions, structs (R2), globals, statements, parfor clauses
// (reduction/threads/chunk/affinity/collapse), nested parfor (R2).
#include "common.h"

namespace freshet {

static Program* g_prog = nullptr;

struct Parser {
  std::vector<Token> toks;
  size_t pos = 0;
  std::string file;
  std::set<std::string> knownStructs;   // declared before use (documented rule)

  Token& cur() { return toks[pos]; }
  Token& peek(int n = 1) { return toks[std::min(pos + n, toks.size() - 1)]; }
  Token advance() { return toks[pos < toks.size() - 1 ? pos++ : pos]; }
  bool at(const char* s) {
    return (cur().kind == Token::PUNCT || cur().kind == Token::KW) && cur().text == s;
  }
  bool atIdent(const char* s) { return cur().kind == Token::IDENT && cur().text == s; }
  bool accept(const char* s) { if (at(s)) { advance(); return true; } return false; }
  void expect(const char* s, const char* what) {
    if (!accept(s)) {
      diag(Diag::Error, Code::Parse,
           std::string("expected '") + s + "' (" + what + "), got '" +
           (cur().kind == Token::EOFTOK ? "<eof>" : cur().text) + "'",
           file, cur().loc.line, cur().loc.col);
    }
  }
  std::string expectIdent(const char* what) {
    if (cur().kind != Token::IDENT && cur().kind != Token::KW) {
      diag(Diag::Error, Code::Parse,
           std::string("expected identifier (") + what + ")", file,
           cur().loc.line, cur().loc.col);
      advance();
      return "<error>";
    }
    std::string s = cur().text;
    advance();
    return s;
  }
  // ---------------- types
  // Type syntax (report §4.1/§4.2/§4.5/Phase 2a):
  //   scalar | *T | Struct | soa(S) | str | [T; N] | Span<T>
  //   | Generic<T, U>          (struct/enum instantiation)
  //
  // Named pieces are not resolvable at parse time (sema owns the struct
  // registry), so parseTypeNamed returns a PLACEHOLDER Type plus a
  // recursive textual encoding the sema binder resolves:
  //   ""                      fully resolved (scalar / *scalar / ...)
  //   "Name"                  struct/enum by name
  //   "*:ENC"                 pointer to ENC
  //   "soa:Name"              SoA descriptor
  //   "arr:N:ENC"             [ENC; N] fixed array
  //   "span:ENC"              Span<ENC>
  //   "gen:Name(ENC;ENC)"     generic struct/enum instantiation
  // The encoding is prefix-coded and unambiguous (names never contain
  // ':' ';' '(' ')').
  bool isTypeStart() {
    if (cur().kind == Token::KW) {
      const std::string& t = cur().text;
      if (t=="u8"||t=="u16"||t=="u32"||t=="u64"||t=="usize"||t=="i8"||
          t=="i16"||t=="i32"||t=="i64"||t=="f32"||t=="f64"||t=="bool"||
          t=="void"||t=="str") return true;
      if (t == "soa" && peek().text == "(") return true;
      // Phase 3b: fn(P...) -> R function-pointer type introducer
      if (t == "fn" && peek().text == "(") return true;
      return false;
    }
    if (cur().kind == Token::PUNCT) {
      if (cur().text == "*") return true;
      if (cur().text == "[" ) return true;   // [T; N]
      return false;
    }
    if (cur().kind == Token::IDENT) return true;   // struct/generic name
    return false;
  }

  Type* parseType() { return parseTypeNamed().first; }

  // try to parse `<T1, T2, ...>` generic arguments after a type name.
  // Returns false (position AND diagnostics restored) when the tokens do
  // not form a well-shaped argument list. Backtracking keeps `a < b`
  // comparisons in expression contexts safe even if a caller gets here by
  // accident.
  bool tryParseTypeArgs(std::vector<std::string>* encs,
                        std::vector<Type*>* placeholders) {
    if (!at("<")) return false;
    size_t save = pos;
    size_t diagSave = g_diags.size();
    advance();   // '<'
    std::vector<std::string> es;
    std::vector<Type*> ps;
    while (true) {
      auto [ty, nm] = parseTypeNamed();
      (void)ty;
      es.push_back(nm);
      ps.push_back(ty);
      if (accept(",")) continue;
      if (accept(">")) break;
      // not a type-argument shape: rewind tokens and any diagnostics the
      // failed attempt produced
      pos = save;
      g_diags.resize(diagSave);
      return false;
    }
    if (encs) *encs = std::move(es);
    if (placeholders) *placeholders = std::move(ps);
    return true;
  }

  std::pair<Type*, std::string> parseTypeNamed() {
    SrcLoc l = cur().loc;
    const std::string& t = cur().text;
    auto simple = [&](Type::K k, const char* enc) {
      advance();
      return std::make_pair(Type::simple(k), std::string(enc));
    };
    // scalars carry their spelling so generic arguments (Pair<f64, i64>)
    // survive the trip through the textual encoding
    if (t == "u8") return simple(Type::U8, "u8");
    if (t == "u16") return simple(Type::U16, "u16");
    if (t == "u32") return simple(Type::U32, "u32");
    if (t == "u64" || t == "usize") return simple(Type::USIZE, "u64");
    if (t == "i8") return simple(Type::I8, "i8");
    if (t == "i16") return simple(Type::I16, "i16");
    if (t == "i32") return simple(Type::I32, "i32");
    if (t == "i64") return simple(Type::I64, "i64");
    if (t == "f32") return simple(Type::F32, "f32");
    if (t == "f64") return simple(Type::F64, "f64");
    if (t == "bool") return simple(Type::BOOL, "bool");
    if (t == "void") return simple(Type::VOID, "void");
    if (t == "str") return simple(Type::STR, "str");
    // Phase 3b (report §6.3): fn(P1, P2, ...) -> R function-pointer type.
    // The parse-time placeholder keeps parameter/return placeholders; sema
    // rebinds through the "fnp:" encoding like every other named type.
    if (t == "fn" && peek().text == "(") {
      advance();                        // 'fn'
      expect("(", "fn pointer params");
      std::vector<Type*> ptypes;
      std::vector<std::string> pencs;
      if (!at(")")) {
        while (true) {
          auto [ty, nm] = parseTypeNamed();
          ptypes.push_back(ty);
          pencs.push_back(nm.empty() ? std::string("~") : nm);
          if (!accept(",")) break;
        }
      }
      expect(")", "fn pointer params");
      Type* retTy = Type::simple(Type::VOID);
      std::string retEnc = "~";
      if (accept("->")) {
        auto [ty, nm] = parseTypeNamed();
        retTy = ty;
        retEnc = nm.empty() ? std::string("~") : nm;
      }
      std::string enc = "fnp:";
      for (size_t i = 0; i < pencs.size(); ++i) {
        if (i) enc += ";";
        enc += pencs[i];
      }
      enc += ":" + retEnc;
      return {Type::fnptrOf(ptypes, retTy), enc};
    }
    if (t == "*") {
      advance();
      auto [ty, nm] = parseTypeNamed();
      if (nm.empty()) return {Type::ptr(ty), ""};
      return {Type::ptr(ty), "*:" + nm};
    }
    if (t == "soa") {
      advance();
      expect("(", "soa");
      std::string sn = expectIdent("struct name");
      expect(")", "soa");
      (void)l;
      return {Type::soaOf(nullptr), "soa:" + sn};
    }
    // [T; N] — fixed-size array
    if (cur().kind == Token::PUNCT && t == "[") {
      advance();
      auto [ty, nm] = parseTypeNamed();
      expect(";", "array length");
      if (cur().kind != Token::INT) {
        diag(Diag::Error, Code::Parse,
             "array length must be an integer literal, got '" + cur().text + "'",
             file, cur().loc.line, cur().loc.col);
        advance();
        return {Type::arrayOf(ty, 0), "arr:0:" + nm};
      }
      uint64_t n = cur().ival;
      advance();
      expect("]", "array type");
      if (n == 0) {
        diag(Diag::Error, Code::Parse, "array length must be at least 1",
             file, l.line, l.col);
      }
      return {Type::arrayOf(ty, n), nm.empty() ? "" : "arr:" + std::to_string(n) + ":" + nm};
    }
    if (cur().kind == Token::IDENT || cur().kind == Token::KW) {
      std::string name = cur().text;
      bool isSpan = (name == "Span");
      advance();
      // Phase 3a: mod::Type — a module-qualified type name.  The
      // encoding keeps the qualifier so the sema binder can resolve the
      // name inside that module only (pub visibility still applies).
      if (at("::") && cur().kind == Token::PUNCT) {
        advance();   // '::'
        std::string item = expectIdent("type name after ::");
        name = name + "::" + item;
        // qualified generic instantiation: gen:mod::Item(ARGS)
        if (at("<") && cur().kind == Token::PUNCT) {
          std::vector<std::string> encs;
          if (tryParseTypeArgs(&encs, nullptr)) {
            std::string argEnc;
            for (size_t i = 0; i < encs.size(); ++i) {
              if (i) argEnc += ";";
              argEnc += encs[i].empty() ? std::string("~") : encs[i];
            }
            return {Type::structOf(nullptr), "gen:" + name + "(" + argEnc + ")"};
          }
        }
        if (accept("*"))
          return {Type::ptr(Type::structOf(nullptr)), "*:qual:" + name};
        return {Type::structOf(nullptr), "qual:" + name};
      }
      // Span<T> — builtin generic view type
      if (isSpan && at("<")) {
        advance();
        auto [ty, nm] = parseTypeNamed();
        expect(">", "Span type argument");
        return {Type::spanOf(ty), nm.empty() ? "" : "span:" + nm};
      }
      // Generic<T, U> — instantiation of a generic struct/enum template.
      // The placeholder is an unresolved struct ref; sema resolves the
      // name, instantiates the template, and interns the result.
      if (at("<") && cur().kind == Token::PUNCT) {
        std::vector<std::string> encs;
        if (tryParseTypeArgs(&encs, nullptr)) {
          std::string argEnc;
          for (size_t i = 0; i < encs.size(); ++i) {
            if (i) argEnc += ";";
            argEnc += encs[i].empty() ? std::string("~") : encs[i];
          }
          return {Type::structOf(nullptr), "gen:" + name + "(" + argEnc + ")"};
        }
      }
      // Struct* — pointer to struct (historical postfix form)
      if (accept("*")) {
        return {Type::ptr(Type::structOf(nullptr)), "*:" + name};
      }
      return {Type::structOf(nullptr), name};
    }
    diag(Diag::Error, Code::Parse, "expected type, got '" + cur().text + "'",
         file, l.line, l.col);
    advance();
    return {Type::simple(Type::VOID), ""};
  }

  // ---------------- expressions (precedence climbing)
  Expr* parseExpr(int minPrec = 0) {
    Expr* lhs = parseUnary();
    // binary operators, precedence
    static const std::map<std::string, int> prec = {
      {"||", 1}, {"&&", 2}, {"==", 3}, {"!=", 3}, {"<", 4}, {"<=", 4}, {">", 4}, {">=", 4},
      {"|", 5}, {"^", 6}, {"&", 7}, {"<<", 8}, {">>", 8},
      {"+", 9}, {"-", 9}, {"*", 10}, {"/", 10}, {"%", 10},
    };
    while (true) {
      if (cur().kind != Token::PUNCT) break;
      auto it = prec.find(cur().text);
      if (it == prec.end() || it->second < minPrec) break;
      std::string op = cur().text;
      SrcLoc l = cur().loc;
      advance();
      Expr* rhs = parseExpr(it->second + 1);
      Expr* b = new Expr(Expr::BIN, l);
      b->op = op; b->lhs = lhs; b->rhs = rhs;
      lhs = b;
    }
    return lhs;
  }

  Expr* parseUnary() {
    SrcLoc l = cur().loc;
    if (at("&")) {
      // address-of: &lvalue
      advance();
      Expr* u = new Expr(Expr::UN, l);
      u->op = "&";
      u->rhs = parseUnary();
      return u;
    }
    if (at("-") || at("!") || at("~") || at("+")) {
      std::string op = cur().text;
      advance();
      Expr* u = new Expr(Expr::UN, l);
      u->op = op;
      u->rhs = parseUnary();
      return u;
    }
    return parsePostfix();
  }

  Expr* parsePostfix() {
    Expr* e = parsePrimary();
    while (true) {
      if (at("[")) {
        SrcLoc l = cur().loc;
        advance();
        Expr* idx = parseExpr();
        // Phase 1c (report §4.1): base[lo..hi] — a borrowed view
        // (Span for arrays/spans, str for strings), no element copy
        if (at("..")) {
          advance();
          Expr* hi = parseExpr();
          expect("]", "slice");
          Expr* sl = new Expr(Expr::SLICE, l);
          sl->base = e; sl->lhs = idx; sl->rhs = hi;
          e = sl;
          continue;
        }
        expect("]", "index");
        Expr* ix = new Expr(Expr::INDEX, l);
        ix->base = e; ix->rhs = idx;
        e = ix;
      } else if (at(".")) {
        SrcLoc l = cur().loc;
        advance();
        std::string f = expectIdent("field");
        // dotted call: freshet.time_ns(...) etc.
        if (at("(") && e->k == Expr::IDENT) {
          advance();
          Expr* c = new Expr(Expr::CALL, l);
          c->name = e->name + "." + f;
          if (!at(")")) {
            while (true) {
              Expr* a = parseExpr();
              c->args.push_back(a);
              c->argGive.push_back(false);
              if (!accept(",")) break;
            }
          }
          expect(")", "call args");
          e = c;
          continue;
        }
        Expr* fld = new Expr(Expr::FIELD, l);
        fld->base = e; fld->name = f;
        e = fld;
      } else if (at("as")) {
        // cast postfix: expr as T (works after any expression)
        SrcLoc l = cur().loc;
        advance();
        auto [ty, nm] = parseTypeNamed();
        Expr* c = new Expr(Expr::CAST, l);
        c->base = e; c->castTy = ty; c->castTyName = nm;
        e = c;
      } else if (at("?")) {
        // Phase 2c (report §4.6): expr? — Option/Result error
        // propagation (early-return on None/Err)
        SrcLoc l = cur().loc;
        advance();
        Expr* q = new Expr(Expr::TRYQ, l);
        q->base = e;
        e = q;
      } else break;
    }
    return e;
  }

  Expr* parsePrimary() {
    SrcLoc l = cur().loc;
    switch (cur().kind) {
      case Token::INT: {
        Expr* e = new Expr(Expr::INT, l);
        e->ival = cur().ival;
        e->constant = true;
        advance();
        return e;
      }
      case Token::FLT: {
        Expr* e = new Expr(Expr::FLT, l);
        e->fval = cur().fval;
        e->constant = true;
        advance();
        return e;
      }
      case Token::STR: {
        Expr* e = new Expr(Expr::STR, l);
        e->sval = cur().sval;
        advance();
        return e;
      }
      case Token::KW: {
        if (at("true") || at("false")) {
          Expr* e = new Expr(Expr::BOOL, l);
          e->bval = (cur().text == "true");
          e->constant = true;
          advance();
          return e;
        }
        if (at("sizeof") || at("alignof")) {
          std::string kw = cur().text;
          advance();
          expect("(", "sizeof");
          auto [ty, nm] = parseTypeNamed();
          expect(")", "sizeof");
          Expr* e = new Expr(Expr::CALL, l);
          e->name = kw;
          // encode type via castTy on a synthetic arg; NAMED types keep
          // their binder encoding so sema can resolve struct/soa/array
          // types (sizeof(Body), sizeof(soa(Body)), sizeof([f64; 8]))
          Expr* a = new Expr(Expr::IDENT, l);
          a->name = nm.empty() ? "__tyarg__" : "__tyarg__:" + nm;
          a->castTy = ty;
          e->args.push_back(a);
          e->constant = true;
          return e;
        }
        if (at("nil")) {
          Expr* e = new Expr(Expr::NILQ, l);
          e->constant = true;
          advance();
          return e;
        }
        break;
      }
      case Token::IDENT: {
        // Phase 3a: mod::name qualified path — consumed before the plain
        // identifier logic so CALL/IDENT both carry the module qualifier.
        if (peek().text == "::" && cur().kind == Token::IDENT) {
          std::string mod = cur().text;
          advance();              // ident
          advance();              // '::'
          SrcLoc ql = cur().loc;
          std::string item = expectIdent("name after '::'");
          std::string full = mod + "::" + item;
          if (at("(")) {
            // qualified CALL — cur() is already the '(' (expectIdent
            // consumed the item); parse the argument list directly
            Expr* e = new Expr(Expr::CALL, l);
            e->name = full;
            e->qualModule = mod;
            advance();              // consume '('
            if (!at(")")) {
              while (true) {
                bool give = false;
                if (at("give")) { give = true; advance(); }
                Expr* a = parseExpr();
                e->args.push_back(a);
                e->argGive.push_back(give);
                if (!accept(",")) break;
              }
            }
            expect(")", "call args");
            return e;
          }
          Expr* e = new Expr(Expr::IDENT, ql);
          e->name = full;
          e->qualModule = mod;
          return e;
        }
        // struct literal: Name { field: expr, ... }
        // Phase 3a: resolved by SEMA, not by the parser-local
        // knownStructs set — a type imported from another module
        // (`import geometry;` … `Point { x: 1.0 }`) is not in this
        // file's set, and `IDENT {` has no other valid parse in
        // expression position anyway.
        if (peek().text == "{") {
          Expr* e = new Expr(Expr::SLIT, l);
          e->name = cur().text;
          advance();
          expect("{", "struct literal");
          while (!at("}") && cur().kind != Token::EOFTOK) {
            std::string fn = expectIdent("field name");
            expect(":", "field");
            Expr* fv = parseExpr();
            e->slitFields.push_back(fn);
            e->args.push_back(fv);
            e->argGive.push_back(false);
            if (!accept(",")) break;
          }
          expect("}", "struct literal");
          return e;
        }
        // function call?
        if (peek().text == "(") {
          Expr* e = new Expr(Expr::CALL, l);
          e->name = cur().text;
          advance();
          expect("(", "call");
          if (!at(")")) {
            while (true) {
              bool give = false;
              // `give` is a KEYWORD (spec §6: give(a) transfers ownership
              // into a callee parameter) — at() matches keywords, the
              // historical atIdent() check could never fire, so the
              // documented transfer syntax did not parse at all
              if (at("give")) { give = true; advance(); }
              Expr* a = parseExpr();
              e->args.push_back(a);
              e->argGive.push_back(give);
              if (!accept(",")) break;
            }
          }
          expect(")", "call args");
          return e;
        }
        // cast as type
        Expr* e = new Expr(Expr::IDENT, l);
        e->name = cur().text;
        advance();
        // `expr as T`
        if (at("as")) {
          advance();
          auto [ty, nm] = parseTypeNamed();
          Expr* c = new Expr(Expr::CAST, l);
          c->base = e; c->castTy = ty; c->castTyName = nm;
          return c;
        }
        return e;
      }
      case Token::PUNCT: {
        if (at("(")) {
          advance();
          Expr* e = parseExpr();
          expect(")", "paren");
          return e;
        }
        // Phase 1b (report §4.1): array literal [a, b, c]
        if (at("[")) {
          advance();
          Expr* e = new Expr(Expr::ARRLIT, l);
          if (!at("]")) {
            while (true) {
              Expr* el = parseExpr();
              e->args.push_back(el);
              e->argGive.push_back(false);
              if (!accept(",")) break;
              if (at("]")) break;   // trailing comma allowed
            }
          }
          expect("]", "array literal");
          return e;
        }
        break;
      }
      default: break;
    }
    diag(Diag::Error, Code::Parse,
         "expected expression, got '" + cur().text + "'", file, l.line, l.col);
    advance();
    return new Expr(Expr::INT, l);
  }

  // ---------------- statements
  Stmt* parseBlock(FnDecl* fn, Stmt* parent) {
    SrcLoc l = cur().loc;
    Stmt* b = new Stmt(Stmt::BLOCK, l);
    b->fn = fn;
    b->parent = parent;
    expect("{", "block");
    while (!at("}") && cur().kind != Token::EOFTOK) {
      Stmt* s = parseStmt(fn, b);
      if (s) b->stmts.push_back(s);
    }
    expect("}", "block");
    return b;
  }

  Stmt* parseStmt(FnDecl* fn, Stmt* parent) {
    SrcLoc l = cur().loc;
    // labels for loop names? (skip)
    if (accept(";")) return new Stmt(Stmt::EMPTY, l);
    if (at("{")) return parseBlock(fn, parent);
    // §5 loop-hint policy: `@vectorize for ...` / `@vectorize parfor ...`
    // — an explicit per-loop request for backend vectorisation.  The
    // hint lowers to llvm.loop.vectorize.enable WITHOUT an interleave
    // count (the cost model still picks width/unroll).  Placing it on
    // any other statement is a warning, not an error (matches the
    // function-attribute F0105 precedent).
    //
    // Phase 5 (report §12): `@reproducible parfor ... [reduction(...)]`
    // — deterministic parallel reduction: fixed block partition,
    // strict-FP combines, identical bitwise results for any thread
    // count.  Only meaningful on parfor with reductions.
    bool vectorizeHint = false;
    bool reproducibleHint = false;
    while (at("@")) {
      advance();
      std::string attr = expectIdent("statement attribute");
      if (attr == "vectorize") {
        vectorizeHint = true;
      } else if (attr == "reproducible") {
        reproducibleHint = true;
      } else {
        diag(Diag::Warning, Code::Attr, "unknown statement attribute @" + attr,
             file, l.line, l.col);
      }
    }
    Stmt* s = parseStmtInner(fn, parent);
    if (s) {
      s->vectorizeHint = vectorizeHint;
      if (reproducibleHint) {
        if (s->k == Stmt::PARFOR) s->reproducible = true;
        else
          diag(Diag::Warning, Code::Attr,
               "@reproducible only applies to parfor with reduction clauses",
               file, l.line, l.col);
      }
    }
    return s;
  }

  Stmt* parseStmtInner(FnDecl* fn, Stmt* parent) {
    SrcLoc l = cur().loc;
    if (cur().kind == Token::KW) {
      const std::string& kw = cur().text;
      // Phase 1a (report §4.4): break/continue
      if (kw == "break") {
        advance();
        Stmt* s = new Stmt(Stmt::BREAK, l);
        s->fn = fn; s->parent = parent;
        expect(";", "break");
        return s;
      }
      if (kw == "continue") {
        advance();
        Stmt* s = new Stmt(Stmt::CONTINUE, l);
        s->fn = fn; s->parent = parent;
        expect(";", "continue");
        return s;
      }
      // Phase 1d (report §4.3/§4.4): match with exhaustive arms
      if (kw == "match") {
        advance();
        expect("(", "match");
        Stmt* s = new Stmt(Stmt::MATCH, l);
        s->fn = fn; s->parent = parent;
        s->matchOn = parseExpr();
        expect(")", "match");
        expect("{", "match arms");
        while (!at("}") && cur().kind != Token::EOFTOK) {
          MatchArm arm;
          arm.loc = cur().loc;
          // `_` lexes as an IDENT (isIdentStart('_')), so the token-kind
          // based at() cannot see it — check by text
          if (cur().kind == Token::IDENT && cur().text == "_") {
            advance();
            arm.wildcard = true;
          } else {
            arm.variantName = expectIdent("variant name");
          }
          if (at("(")) {
            advance();
            while (!at(")") && cur().kind != Token::EOFTOK) {
              arm.bindings.push_back(expectIdent("payload binding"));
              if (!accept(",")) break;
            }
            expect(")", "payload bindings");
          }
          expect("=>", "match arm");
          arm.body = parseBlock(fn, s);
          s->matchArms.push_back(std::move(arm));
        }
        expect("}", "match arms");
        return s;
      }
      // Phase MEM (report §5.1): unsafe scope — represented as a flagged
      // BLOCK so every existing AST walker keeps visiting it
      if (kw == "unsafe") {
        advance();
        Stmt* s = parseBlock(fn, parent);
        s->isUnsafe = true;
        s->loc = l;
        return s;
      }
      if (kw == "let") {
        advance();
        Stmt* s = new Stmt(Stmt::LET, l);
        s->fn = fn; s->parent = parent;
        s->mut = accept("mut");
        s->var = expectIdent("let name");
        if (accept(":")) {
          auto [ty, nm] = parseTypeNamed();
          s->annTy = ty;
          s->tyName = nm; // parser-level: store in extra field
          s->hasAnn = true;
        }
        expect("=", "let initializer");
        s->init = parseExpr();
        expect(";", "let");
        return s;
      }
      if (kw == "return") {
        advance();
        Stmt* s = new Stmt(Stmt::RETURN, l);
        s->fn = fn; s->parent = parent;
        if (!at(";")) s->retVal = parseExpr();
        expect(";", "return");
        return s;
      }
      if (kw == "defer") {
        advance();
        Stmt* s = new Stmt(Stmt::DEFER, l);
        s->fn = fn; s->parent = parent;
        s->deferExpr = parseExpr();
        expect(";", "defer");
        return s;
      }
      if (kw == "if") {
        advance();
        Stmt* s = new Stmt(Stmt::IF, l);
        s->fn = fn; s->parent = parent;
        s->cond = parseExpr();
        s->thenB = parseBlock(fn, s);
        if (accept("else")) {
          if (at("if")) s->elseB = parseStmt(fn, s);
          else s->elseB = parseBlock(fn, s);
        }
        return s;
      }
      if (kw == "while") {
        advance();
        Stmt* s = new Stmt(Stmt::WHILE, l);
        s->fn = fn; s->parent = parent;
        s->cond = parseExpr();
        s->body = parseBlock(fn, s);
        return s;
      }
      if (kw == "for") {
        advance();
        expect("(", "for");
        Stmt* s = new Stmt(Stmt::FOR, l);
        s->fn = fn; s->parent = parent;
        s->ivar = expectIdent("loop var");
        if (at("in")) {
          advance();
          s->forRange = true;
          s->rangeLo = parseExpr();
          if (accept("..")) {
            s->rangeHi = parseExpr();
          } else if (accept("by")) {
            // for (i in a..b by s) — step after range
            s->rangeHi = parseExpr();
            if (accept("by")) s->forStep = parseExpr();
          } else {
            expect("..", "range");
          }
          if (accept("by")) s->forStep = parseExpr();
        } else {
          // for (i = init; cond; step)
          expect("=", "for init");
          s->forInit = parseExpr();
          expect(";", "for");
          s->forCond = parseExpr();
          expect(";", "for");
          // Phase 0d follow-on: the step is an ASSIGNMENT-shaped form —
          // `i += 2`, `i -= 1`, `i = i + 1` — none of which are binary
          // operators, so the historical parseExpr() could never accept
          // them and the C-style loop simply did not parse (the F0405
          // stride analysis was dead code). Parse the assignment head
          // explicitly. (There is no `++` token; report §3.4.)
          Expr* step = parseExpr();
          if (step && step->k == Expr::IDENT &&
              (at("+=") || at("-=") || at("="))) {
            std::string op = cur().text;
            SrcLoc sl = cur().loc;
            advance();
            Expr* rhs = parseExpr();
            Expr* b = new Expr(Expr::BIN, sl);
            b->op = op;
            b->lhs = step;
            b->rhs = rhs;
            step = b;
          }
          s->forStep = step;
        }
        expect(")", "for");
        s->body = parseBlock(fn, s);
        return s;
      }
      if (kw == "parfor") {
        advance();
        expect("(", "parfor");
        Stmt* s = new Stmt(Stmt::PARFOR, l);
        s->fn = fn; s->parent = parent;
        s->pvar = expectIdent("parfor var");
        if (!accept("in")) {
          diag(Diag::Error, Code::Parse, "parfor requires 'in'", file,
               cur().loc.line, cur().loc.col);
        }
        s->plo = parseExpr();
        if (!accept("..")) expect("..", "parfor range");
        s->phi = parseExpr();
        if (accept("by")) {
          Expr* step = parseExpr();
          // descending: by -1 → note in sema
          if (!(step->k == Expr::UN && step->op == "-" &&
                step->rhs->k == Expr::INT && step->rhs->ival == 1)) {
            diag(Diag::Error, Code::Parfor,
                 "parfor supports only unit step (by -1 for descending)",
                 file, l.line, l.col);
          } else {
            s->pDescending = true;
          }
        }
        expect(")", "parfor");
        // clauses: [reduction(+:acc), threads:8, chunk:64, affinity:close,
        //           collapse:2]
        if (at("[")) {
          advance();
          while (!at("]") && cur().kind != Token::EOFTOK) {
            ParForClause c;
            c.loc = cur().loc;
            std::string cname = expectIdent("clause");
            if (cname == "reduction") {
              c.k = ParForClause::REDUCTION;
              expect("(", "reduction");
              // op: + - * (punct) or min max (ident)
              if (at("+") || at("-") || at("*")) {
                c.redOp = cur().text;
                advance();
              } else {
                c.redOp = expectIdent("reduction op");
              }
              expect(":", "reduction");
              c.var = expectIdent("accumulator");
              expect(")", "reduction");
            } else if (cname == "threads") {
              c.k = ParForClause::THREADS;
              expect(":", "threads");
              c.e = parseExpr();
            } else if (cname == "chunk") {
              c.k = ParForClause::CHUNK;
              expect(":", "chunk");
              c.e = parseExpr();
            } else if (cname == "affinity") {
              c.k = ParForClause::AFFINITY;
              expect(":", "affinity");
              c.sval = expectIdent("affinity mode");
            } else if (cname == "collapse") {
              c.k = ParForClause::COLLAPSE;
              expect(":", "collapse");
              if (cur().kind == Token::INT) { c.ival = (int)cur().ival; advance(); }
            } else {
              diag(Diag::Error, Code::Parfor, "unknown parfor clause: " + cname,
                   file, c.loc.line, c.loc.col);
            }
            s->clauses.push_back(c);
            if (!accept(",")) break;
          }
          expect("]", "parfor clauses");
        }
        s->body = parseBlock(fn, s);
        return s;
      }
    }
    // expression / assignment statement
    Expr* e = parseExpr();
    // Phase 0d (report §3.4): <<= and >>= are documented in spec §5 —
    // the lexer now produces single tokens for them, and this list
    // accepts every documented compound assignment operator.
    static const std::string assignOps[] = {"=", "+=", "-=", "*=", "/=", "%=",
                                            "&=", "|=", "^=", "<<=", ">>="};
    for (auto& op : assignOps) {
      if (at(op.c_str())) {
        advance();
        Stmt* s = new Stmt(Stmt::ASSIGN, l);
        s->fn = fn; s->parent = parent;
        s->lhs = e; s->op2 = op;
        s->rhs = parseExpr();
        expect(";", "assignment");
        return s;
      }
    }
    expect(";", "expression statement");
    Stmt* s = new Stmt(Stmt::EXPR, l);
    s->fn = fn; s->parent = parent;
    s->expr = e;
    return s;
  }

  // ---------------- toplevel
  void parseToplevel(Program* p) {
    while (cur().kind != Token::EOFTOK) {
      if (accept(";")) continue;
      SrcLoc l = cur().loc;
      // ---- Phase 3a (report §6.1): module identity + imports ----------
      // `module geometry;` — file-based module identity; must be the
      // FIRST declaration of the file (a later one cannot retroactively
      // tag already-parsed decls).
      if (at("module")) {
        advance();
        std::string name = expectIdent("module name");
        expect(";", "module declaration");
        if (!p->moduleName.empty()) {
          diag(Diag::Error, Code::Mod,
               "duplicate module declaration (already '" + p->moduleName +
               "')",
               file, l.line, l.col);
        } else if (!p->fns.empty() || !p->structs.empty() ||
                   !p->enums.empty() || !p->globals.empty()) {
          diag(Diag::Error, Code::Mod,
               "module declaration must come before every other declaration",
               file, l.line, l.col);
        } else {
          p->moduleName = name;
        }
        continue;
      }
      if (at("import")) {
        advance();
        ImportDecl im;
        im.loc = l;
        im.name = expectIdent("module name");
        expect(";", "import declaration");
        p->imports.push_back(im);
        continue;
      }
      // `pub` visibility prefix (Phase 3a): applies to the following
      // fn / struct / enum / global declaration.
      bool pub = false;
      if (at("pub") && (peek().kind == Token::KW || peek().kind == Token::IDENT)) {
        advance();
        pub = true;
      }
      // Phase 1d (report §4.3): enum declarations
      //   enum Name { A, B(i32, f64) }
      //   enum Name<T> { Some(T), None }
      if (at("enum")) {
        advance();
        EnumDef* ed = new EnumDef();
        ed->loc = l;
        ed->isPub = pub;
        ed->name = expectIdent("enum name");
        // generic template parameters
        if (at("<")) {
          advance();
          while (true) {
            ed->tparams.push_back(expectIdent("type parameter"));
            if (!accept(",")) break;
          }
          expect(">", "enum type parameters");
          ed->isGeneric = true;
        }
        expect("{", "enum body");
        while (!at("}") && cur().kind != Token::EOFTOK) {
          EnumVariant v;
          v.loc = cur().loc;
          v.name = expectIdent("variant name");
          if (at("(")) {
            advance();
            int fi = 0;
            while (!at(")") && cur().kind != Token::EOFTOK) {
              Param pm;
              pm.loc = cur().loc;
              pm.name = "__" + std::to_string(fi++);   // positional
              auto [ty, nm] = parseTypeNamed();
              pm.ty = ty; pm.tyName = nm;
              v.fields.push_back(pm);
              if (!accept(",")) break;
            }
            expect(")", "variant payload");
          }
          v.idx = (int)ed->variants.size();
          ed->variants.push_back(v);
          if (!accept(",")) break;
          if (at("}")) break;   // trailing comma
        }
        expect("}", "enum body");
        if (ed->variants.empty()) {
          diag(Diag::Error, Code::Parse, "enum " + ed->name +
               " must declare at least one variant",
               file, l.line, l.col);
        }
        ed->id = (int)p->enums.size();
        p->enums.push_back(ed);
        continue;
      }
      if (at("struct")) {
        advance();
        StructDef* sd = new StructDef();
        sd->loc = l;
        sd->isPub = pub;
        sd->name = expectIdent("struct name");
        knownStructs.insert(sd->name);
        // Phase 2a: generic struct template parameters
        if (at("<")) {
          advance();
          while (true) {
            sd->tparams.push_back(expectIdent("type parameter"));
            if (!accept(",")) break;
          }
          expect(">", "struct type parameters");
          sd->isGeneric = true;
        }
        if (at("align")) {
          advance();
          expect("(", "align");
          if (cur().kind == Token::INT) { sd->alignOverride = cur().ival; advance(); }
          expect(")", "align");
        }
        expect("{", "struct body");
        while (!at("}") && cur().kind != Token::EOFTOK) {
          Field f;
          f.loc2 = cur().loc;
          f.name = expectIdent("field name");
          expect(":", "field type");
          auto [ty, nm] = parseTypeNamed();
          f.ty = ty; f.tyName = nm;
          sd->fields.push_back(f);
          if (!accept(",")) break;
          if (at("}")) break;   // trailing comma
        }
        expect("}", "struct body");
        sd->id = (int)p->structs.size();
        p->structs.push_back(sd);
        continue;
      }
      bool exported = false;
      bool imported = false;
      while (at("export") || at("extern")) {
        if (at("export")) exported = true;
        if (at("extern")) imported = true;
        advance();
      }
      // Phase 4 (report §10.3): @test marks a zero-argument function as
      // a test case for `penstock test`.
      bool preTest = false;
      // Stage B4 fix: accept function attributes BEFORE `fn` (matching the
      // language spec example: `@alias(a, b) fn f(a: *f64, b: *f64)`).
      // The historical parser only accepted attributes AFTER the parameter
      // list, which contradicted the spec — meaning any code matching the
      // spec example failed to parse.  We now accept both orderings.
      //
      // Use a "pending attributes" buffer: attributes seen before `fn`
      // are stashed here and merged into the FnDecl once it exists.
      // After the signature we still loop to accept attributes there too
      // (back-compat with any code using the historical form).
      bool preInline = false, preNoinline = false, preStrictfp = false;
      bool prePure = false, preFastmath = false, preHpcStrict = false;
      bool preUnsafe = false;
      std::vector<std::pair<std::string,std::string>> preAliasPairs;
      while (at("@")) {
        advance();
        std::string attr = expectIdent("attribute");
        if (attr == "strictfp") preStrictfp = true;
        else if (attr == "pure") prePure = true;
        else if (attr == "inline") preInline = true;
        else if (attr == "noinline") preNoinline = true;
        else if (attr == "fastmath") preFastmath = true;
        else if (attr == "hpc_strict") preHpcStrict = true;
        else if (attr == "unsafe") preUnsafe = true;
        else if (attr == "test") preTest = true;
        else if (attr == "alias") {
          expect("(", "alias");
          std::string a = expectIdent("param");
          expect(",", "alias");
          std::string b = expectIdent("param");
          expect(")", "alias");
          preAliasPairs.push_back({a, b});
        } else {
          diag(Diag::Warning, Code::Attr, "unknown attribute @" + attr,
               file, l.line, l.col);
        }
      }
      if (at("fn")) {
        advance();
        FnDecl* f = new FnDecl();
        f->loc = l;
        f->exported = exported;
        f->imported = imported;
        f->isPub = pub;
        f->isTest = preTest;
        // merge pending attributes (pre-fn form)
        f->strictfp = preStrictfp;
        f->userPure = prePure;
        f->userInline = preInline;
        f->userNoinline = preNoinline;
        f->fastmath = preFastmath;
        f->hpcStrict = preHpcStrict;
        f->unsafeFn = preUnsafe;
        for (auto& pr : preAliasPairs) f->aliasPairs.push_back(pr);
        f->name = expectIdent("fn name");
        // Phase 2a (report §4.2): generic function template
        //   fn clamp<T: Numeric>(x: T, low: T, high: T) -> T
        if (at("<")) {
          advance();
          while (true) {
            std::string tp = expectIdent("type parameter");
            std::string cons;
            if (accept(":")) cons = expectIdent("type constraint");
            f->tparams.push_back(tp);
            f->tconstraints.push_back(cons);
            if (!accept(",")) break;
          }
          expect(">", "fn type parameters");
          f->isGeneric = true;
        }
        expect("(", "params");
        if (!at(")")) {
          while (true) {
            Param pm;
            pm.loc = cur().loc;
            // Phase MEM (report §5.3): parameter ownership modes —
            // `owned data: *u8` / `borrowed data: Span<f64>`. Contextual
            // keywords: only modes when an identifier follows.
            if (atIdent("owned") && peek().kind == Token::IDENT) {
              advance();
              pm.owned = true;
            } else if (atIdent("borrowed") && peek().kind == Token::IDENT) {
              advance();
              pm.borrowed = true;
            }
            pm.name = expectIdent("param name");
            expect(":", "param type");
            auto [ty, nm] = parseTypeNamed();
            pm.ty = ty; pm.tyName = nm;
            f->params.push_back(pm);
            if (!accept(",")) break;
          }
        }
        expect(")", "params");
        if (accept("->")) {
          auto [ty, nm] = parseTypeNamed();
          f->ret = ty; f->retTyName = nm;
        }
        // fn attributes after signature (back-compat form): @strictfp
        // @pure @alias(a,b) @inline @noinline @fastmath @hpc_strict
        // @unsafe @test.  These MERGE with any attributes already set from the
        // pre-fn form — both are honoured, with `true` winning if either
        // spelling appears.
        while (at("@")) {
          advance();
          std::string attr = expectIdent("attribute");
          if (attr == "strictfp") f->strictfp = true;
          else if (attr == "pure") f->userPure = true;
          else if (attr == "inline") f->userInline = true;
          else if (attr == "noinline") f->userNoinline = true;
          else if (attr == "fastmath") f->fastmath = true;
          else if (attr == "hpc_strict") f->hpcStrict = true;
          else if (attr == "unsafe") f->unsafeFn = true;
          else if (attr == "test") f->isTest = true;
          else if (attr == "alias") {
            expect("(", "alias");
            std::string a = expectIdent("param");
            expect(",", "alias");
            std::string b = expectIdent("param");
            expect(")", "alias");
            f->aliasPairs.push_back({a, b});
          } else {
            diag(Diag::Warning, Code::Attr, "unknown attribute @" + attr,
                 file, l.line, l.col);
          }
        }
        // @inline + @noinline is contradictory (LLVM resolves it as
        // noinline wins).  Warn so the user notices.
        if (f->userInline && f->userNoinline) {
          diag(Diag::Warning, Code::Attr,
               "function " + f->name + " has both @inline and @noinline — "
               "@noinline takes precedence",
               file, l.line, l.col);
        }
        if (imported) {
          expect(";", "extern function declaration");
          f->body = nullptr;
        } else {
          f->body = parseBlock(f, nullptr);
        }
        f->id = (int)p->fns.size();
        p->fns.push_back(f);
        continue;
      }
      if (at("let")) {
        // global
        advance();
        GlobalVar* g = new GlobalVar();
        g->loc = l;
        g->exported = exported;
        g->isPub = pub;
        g->mut = accept("mut");
        g->name = expectIdent("global name");
        if (accept(":")) {
          auto [ty, nm] = parseTypeNamed();
          g->ty = ty; g->tyName = nm;
        }
        expect("=", "global init");
        g->init = parseExpr();
        expect(";", "global");
        p->globals.push_back(g);
        continue;
      }
      diag(Diag::Error, Code::Parse,
           "expected fn/struct/let at toplevel, got '" + cur().text + "'",
           file, cur().loc.line, cur().loc.col);
      advance();
    }
  }
};

Program* parseTokens(std::vector<Token>& toks, const std::string& file) {
  Program* p = new Program();
  p->file = file;
  g_prog = p;
  Parser ps;
  ps.toks = std::move(toks);
  ps.file = file;
  ps.parseToplevel(p);
  return p;
}

} // namespace freshet
