// Freshet source formatter — Phase 4 (report §10.2).
//
// `penstock fmt <file.fsh>` / `penstock fmt --check <file.fsh>`
//
// Design (compiler-aware, token-level):
//   * A dedicated tokenizer re-reads the source keeping COMMENTS as
//     trivia tokens, so nothing the user wrote is ever dropped.
//   * Layout follows the project's house style:
//       - 4-space indentation
//       - `fn name(a: T) -> R {` … `}` blocks, one statement per line
//       - SHORT blocks/struct literals collapsed to one line when they
//         fit in 78 columns and contain no nested block or comment
//         (`if (c) { return x; }`, `struct Pt { x: f64, y: f64 }`)
//       - space after commas, none before `;` `,` `:` `)`; spaces around
//         binary/assignment operators; none around `.` `::` `[` `]`
//       - toplevel declarations separated by one blank line
//   * SAFETY: the formatter refuses to produce a program whose token
//     stream (comments/whitespace stripped) differs from the input's —
//     the file is only rewritten when the formatted text re-lexes to the
//     exact same tokens.  On any doubt the original file is kept.
#include "common.h"
#include <fstream>
#include <sstream>
#include <cctype>

namespace freshet {

namespace {

struct FTok {
  std::string text;      // exact source spelling
  int kind;              // Token::K
  bool isLineComment = false;
  bool isBlockComment = false;
  int line = 0;
};

bool fIsIdentStart(char c) { return isalpha((unsigned char)c) || c == '_'; }
bool fIsIdentChar(char c)  { return isalnum((unsigned char)c) || c == '_'; }

// tokenise the RAW source, comments included
bool lexForFmt(const std::string& src, std::vector<FTok>& out) {
  static const char* puncts[] = {
    "..", "->", "=>", "==", "!=", "<=", ">=", "&&", "||", "+=", "-=", "*=",
    "/=", "%=", "<<=", ">>=", "<<", ">>", "&=", "|=", "^=", "::",
    "(", ")", "{", "}", "[", "]", ",", ";", ":", ".", "+", "-", "*", "/",
    "%", "=", "<", ">", "!", "&", "|", "^", "~", "@", "?", "#",
  };
  const char* p = src.c_str();
  const char* end = p + src.size();
  int line = 1;
  while (p < end) {
    while (p < end && isspace((unsigned char)*p)) {
      if (*p == '\n') ++line;
      ++p;
    }
    if (p >= end) break;
    char c = *p;
    if (c == '/' && p + 1 < end && p[1] == '/') {
      const char* s = p;
      while (p < end && *p != '\n') ++p;
      FTok t; t.text = std::string(s, p); t.kind = Token::PUNCT;
      t.isLineComment = true; t.line = line;
      out.push_back(t);
      continue;
    }
    if (c == '/' && p + 1 < end && p[1] == '*') {
      const char* s = p;
      p += 2;
      while (p < end && !(p[0] == '*' && p + 1 < end && p[1] == '/')) {
        if (*p == '\n') ++line;
        ++p;
      }
      if (p < end) p += 2;
      FTok t; t.text = std::string(s, p); t.kind = Token::PUNCT;
      t.isBlockComment = true; t.line = line;
      out.push_back(t);
      continue;
    }
    if (c == '"') {
      const char* s = p;
      ++p;
      while (p < end && *p != '"') {
        if (*p == '\\' && p + 1 < end) ++p;
        ++p;
      }
      if (p < end) ++p;
      FTok t; t.text = std::string(s, p); t.kind = Token::STR; t.line = line;
      out.push_back(t);
      continue;
    }
    if (isdigit((unsigned char)c) ||
        (c == '.' && p + 1 < end && isdigit((unsigned char)p[1]))) {
      const char* s = p;
      while (p < end && (isalnum((unsigned char)*p) || *p == '.' || *p == '_'))
        ++p;
      FTok t; t.text = std::string(s, p); t.kind = Token::INT; t.line = line;
      out.push_back(t);
      continue;
    }
    if (fIsIdentStart(c)) {
      const char* s = p;
      while (p < end && fIsIdentChar(*p)) ++p;
      FTok t; t.text = std::string(s, p); t.kind = Token::IDENT; t.line = line;
      out.push_back(t);
      continue;
    }
    bool matched = false;
    for (auto* q : puncts) {
      size_t n = strlen(q);
      if ((size_t)(end - p) >= n && strncmp(p, q, n) == 0) {
        FTok t; t.text = q; t.kind = Token::PUNCT; t.line = line;
        out.push_back(t);
        p += n;
        matched = true;
        break;
      }
    }
    if (!matched) {
      // unknown character: keep verbatim so the safety check trips
      FTok t; t.text = std::string(1, c); t.kind = Token::PUNCT;
      t.line = line;
      out.push_back(t);
      ++p;
    }
  }
  return true;
}

bool isKeywordText(const std::string& s) {
  static const std::set<std::string> kw = {
    "fn","let","mut","return","defer","if","else","while","for","parfor","in",
    "struct","align","true","false","as","export","extern","give","by","step",
    "reduction","threads","chunk","affinity","collapse","close","spread",
    "soa","sizeof","alignof","nil","break","continue","enum","match","unsafe",
    "str","module","import","pub","u8","u16","u32","u64","usize","i8","i16",
    "i32","i64","f32","f64","bool","void",
  };
  return kw.count(s) > 0;
}

// no space BEFORE these when they follow an identifier-ish token
bool noSpaceBefore(const std::string& t) {
  return t == "," || t == ";" || t == ":" || t == ")" || t == "]" ||
         t == "." || t == "::";
}
bool noSpaceAfter(const std::string& t) {
  return t == "(" || t == "[" || t == "." || t == "::" || t == "@" ||
         t == "!";
}
// binary / assignment operators that want spaces on both sides
bool spacedOp(const std::string& t) {
  static const std::set<std::string> ops = {
    "+","-","*","/","%","=","==","!=","<",">","<=",">=","&&","||","&","|","^",
    "<<",">>","+=","-=","*=","/=","%=","&=","|=","^=","<<=",">>=","->","=>",
    "..",
  };
  return ops.count(t) > 0;
}

struct Fmt {
  std::vector<FTok> toks;
  size_t pos = 0;
  std::string out;
  int depth = 0;
  int col = 0;                 // current output column
  std::vector<FTok> pendingComments;

  FTok* cur() { return pos < toks.size() ? &toks[pos] : nullptr; }
  bool at(const char* s) {
    FTok* c = cur();
    return c && !c->isLineComment && !c->isBlockComment && c->text == s;
  }
  void advance() {
    if (pos < toks.size()) {
      if (toks[pos].isLineComment || toks[pos].isBlockComment)
        pendingComments.push_back(toks[pos]);
      ++pos;
    }
  }

  void nl() {
    while (!out.empty() && out.back() == ' ') out.pop_back();
    out += '\n';
    col = 0;
  }
  void indent(int d) {
    for (int i = 0; i < d; ++i) { out += "    "; col += 4; }
  }
  void sp() {
    if (col > 0 && !out.empty() && out.back() != ' ' && out.back() != '\n') {
      out += ' ';
      ++col;
    }
  }
  void write(const std::string& s) {
    out += s;
    col += (int)s.size();
  }

  // flush pending comments at the current indent (own lines)
  void flushComments() {
    for (auto& c : pendingComments) {
      indent(depth);
      write(c.text);
      nl();
    }
    pendingComments.clear();
  }

  // token immediately before `pos` (ignoring comments)
  FTok* prevTok(size_t p) {
    while (p > 0) {
      --p;
      if (!toks[p].isLineComment && !toks[p].isBlockComment) return &toks[p];
    }
    return nullptr;
  }

  // `{` at toks[p] opens a HEADER brace (struct/enum declaration or a
  // struct literal in expression position) rather than a statement block.
  // Walk BACK from the brace, skipping generic arguments, to find the
  // anchoring token:
  //   `->`  or `)`      -> fn signature / control flow  -> BLOCK
  //   `struct`/`enum`   -> declaration                  -> HEADER
  //   `=`/`,`/`(`/`[`/`:`/`return` -> expression        -> HEADER (literal)
  // A bare IDENT (type or literal name) keeps the walk going — that is
  // what distinguishes `-> T {` (fn body) from `= Point {` (literal).
  bool headerBraceAt(size_t p) {
    int angle = 0;
    for (size_t q = p; q > 0; --q) {
      const FTok& t = toks[q - 1];
      if (t.isLineComment || t.isBlockComment) continue;
      if (angle > 0) {                       // inside <...> generic args
        if (t.text == "<") --angle;
        continue;
      }
      if (t.text == ">") { ++angle; continue; }
      if (t.text == ")" || t.text == "->" || t.text == "=>" ||
          t.text == "else" || t.text == "unsafe" || t.text == "fn" ||
          t.text == "in" || t.text == "as")
        return false;                       // block contexts
      if (t.text == "struct" || t.text == "enum")
        return true;                        // declaration header
      if (t.text == "=" || t.text == "," || t.text == "(" ||
          t.text == "[" || t.text == ":" || t.text == ";" ||
          t.text == "return" || t.text == "break" || t.text == "continue")
        return true;                        // expression position (literal)
      if (t.text == "{" || t.text == "}")
        return true;                        // statement boundary
      // IDENT / scalar keyword: keep walking (type name or literal name)
    }
    return false;
  }

  // ---------------------------------------------------------------- program
  void emitProgram() {
    while (cur()) {
      flushComments();
      if (!cur()) break;
      emitToplevel();
      if (cur()) { nl(); nl(); }
    }
  }

  // one toplevel declaration: tokens up to `;` (extern/import/module/
  // globals) or through a balanced `{ ... }` (fn/struct/enum)
  void emitToplevel() {
    while (cur()) {
      FTok* c = cur();
      if (c->isLineComment || c->isBlockComment) { advance(); continue; }
      if (c->text == "{") {
        bool header = headerBraceAt(pos);
        emitToken(c->text, true);
        advance();                      // consume '{'
        if (header) {
          emitInlineBraceBody();
        } else {
          ++depth;
          nl();
          emitBlockBody(depth - 1);
          --depth;
        }
        return;                         // declaration complete
      }
      if (c->text == ";") { write(";"); advance(); return; }
      emitToken(c->text, wantSpaceBefore(c->text, nullptr));
      advance();
    }
  }

  // inside a header brace (struct/enum/struct literal): inline when it
  // fits and has no nested block, else one field per line
  void emitInlineBraceBody() {
    std::vector<FTok> body;
    int d = 1;
    size_t i = pos;
    while (i < toks.size()) {
      FTok& t = toks[i];
      if (t.isLineComment || t.isBlockComment) { body.push_back(t); ++i; continue; }
      if (t.text == "{") ++d;
      else if (t.text == "}") { --d; if (d == 0) break; }
      body.push_back(t);
      ++i;
    }
    int w = col;
    bool hasComment = false;
    for (auto& t : body) {
      if (t.isLineComment || t.isBlockComment) { hasComment = true; break; }
      w += (int)t.text.size() + 1;
    }
    bool inlineable = !hasComment && w <= 78;
    if (inlineable) {
      bool first = true;
      while (cur()) {
        FTok* c = cur();
        if (c->isLineComment || c->isBlockComment) { advance(); continue; }
        if (c->text == "}") { advance(); break; }
        if (c->text == ",") { write(","); advance(); sp(); first = true; continue; }
        emitToken(c->text, wantSpaceBefore(c->text, nullptr));
        first = false;
        advance();
      }
      write(" }");
    } else {
      ++depth;
      nl();
      bool first = true;
      while (cur()) {
        flushComments();
        FTok* c = cur();
        if (!c) break;
        if (c->text == "}") { advance(); break; }
        if (c->text == ",") { write(","); advance(); nl(); first = true; continue; }
        indent(depth);
        emitToken(c->text, wantSpaceBefore(c->text, nullptr));
        first = false;
        advance();
      }
      --depth;
      nl();
      indent(depth);
      write("}");
    }
  }

  // statements inside a block until the matching `}` (consumed); the
  // closing brace is written at `closeIndent` levels of 4-space indent
  void emitBlockBody(int closeIndent) {
    while (cur()) {
      flushComments();
      FTok* c = cur();
      if (!c) break;
      if (c->text == "}") { advance(); break; }
      emitStatement();
    }
    indent(closeIndent);
    write("}");
  }

  // one statement: tokens up to `;`, or through a balanced `{...}` block
  void emitStatement() {
    if (col == 0) indent(depth);
    while (cur()) {
      FTok* c = cur();
      if (c->isLineComment || c->isBlockComment) { advance(); continue; }
      if (c->text == "}") return;         // block end (caller closes)
      if (c->text == "{") {
        if (headerBraceAt(pos)) {
          // struct literal in expression position
          emitToken("{", true);
          advance();
          emitInlineBraceBody();
        } else if (looksShortBlock()) {
          sp();
          emitShortBlockInline();
          if (cur() && at("else")) {
            sp();
            write("else");
            advance();
            if (at("if")) { sp(); continue; }
            continue;
          }
          nl();
          return;               // block-terminated statement complete
        } else {
          emitToken("{", true);
          advance();
          ++depth;
          nl();
          emitBlockBody(depth - 1);
          --depth;
          // `else` / `else if` continues the same statement line
          if (cur() && at("else")) {
            sp();
            write("else");
            advance();
            if (at("if")) { sp(); continue; }
            continue;
          }
          nl();
          return;               // block-terminated statement complete
        }
        continue;
      }
      if (c->text == ";") {
        write(";");
        advance();
        nl();
        return;
      }
      emitToken(c->text, wantSpaceBefore(c->text, nullptr));
      advance();
    }
  }

  // heuristic: block containing no nested `{`, no comment, fitting in 78
  // columns — collapse to `{ ...; ...; }`
  bool looksShortBlock() {
    int d = 1;
    size_t i = pos + 1;
    int w = col + 2;
    while (i < toks.size()) {
      FTok& t = toks[i];
      if (t.isLineComment || t.isBlockComment) return false;
      if (t.text == "{") return false;            // nested block
      if (t.text == "}") {
        if (--d == 0) return w <= 78;
      } else {
        if (t.text == ";") w += 2;
        w += (int)t.text.size() + (spacedOp(t.text) ? 2 : 0);
      }
      ++i;
      if (w > 78) return false;
    }
    return false;
  }

  void emitShortBlockInline() {
    advance();                 // consume '{' — we write our own spacing
    write("{");
    col += 1;
    sp();
    bool first = true;
    while (cur()) {
      FTok* c = cur();
      if (c->isLineComment || c->isBlockComment) { advance(); continue; }
      if (c->text == "}") { advance(); break; }
      if (c->text == ";") { write(";"); advance(); first = true; continue; }
      emitToken(c->text, wantSpaceBefore(c->text, nullptr));
      first = false;
      advance();
    }
    sp();
    write("}");
    col += 1;
  }

  // spacing decision for token `t`
  bool wantSpaceBefore(const std::string& t, void*) {
    if (col == 0) return false;
    if (out.empty() || out.back() == '\n') return false;
    // spacing AFTER the previously emitted token
    {
      char b = out.back();
      if (b == '{') return true;                      // ` { x`
      if (b == '(' || b == '[' || b == '.' || b == '@') return false;
    }
    if (noSpaceBefore(t)) return false;
    if (t == "(") {
      // call/index: attached to the name; control keywords keep a space
      FTok* pv = prevTok(pos);
      if (!pv) return false;
      if (pv->kind == Token::IDENT) {
        static const std::set<std::string> ctrl = {
          "if","while","for","parfor","match","return","align","soa",
        };
        return ctrl.count(pv->text) > 0;
      }
      return false;
    }
    if (t == "{") return true;
    // generic-parameter brackets hug the name / parameter list:
    // `fn name<T: Numeric>(...)` — `<` after the fn NAME, `>` before `(`
    if (t == "<") {
      FTok* pv = prevTok(pos);
      if (pv && pv->kind == Token::IDENT && !isKeywordText(pv->text)) {
        // is the token before the name `fn`? (walk two back)
        size_t q = pos;
        int back = 0;
        while (q > 0 && back < 2) {
          --q;
          if (toks[q].isLineComment || toks[q].isBlockComment) continue;
          ++back;
          if (back == 2 && toks[q].text == "fn") return false;
        }
      }
    }
    if (t == ">") {
      size_t q = pos + 1;
      while (q < toks.size()) {
        if (toks[q].isLineComment || toks[q].isBlockComment) { ++q; continue; }
        if (toks[q].text == "(") return false;
        break;
      }
    }
    if (spacedOp(t)) {
      if (t == "-" || t == "!" || t == "&" || t == "~" || t == "*") {
        char b = out.back();
        if (b == '(' || b == ',' || b == '[' || b == '=' || b == ' ' ||
            b == '\n' || b == '{' || b == ':' || b == '>')
          return false;
        if (strchr("+-*/%&|^<>!=~", b)) return false;
      }
      return true;
    }
    return true;
  }

  void emitToken(const std::string& text, bool spaceBefore) {
    if (spaceBefore) sp();
    write(text);
  }
};

// strip comments+whitespace and compare token streams (the safety gate)
bool tokenStreamsEqual(const std::string& a, const std::string& b) {
  std::vector<FTok> ta, tb;
  lexForFmt(a, ta);
  lexForFmt(b, tb);
  size_t i = 0, j = 0;
  while (true) {
    while (i < ta.size() && (ta[i].isLineComment || ta[i].isBlockComment)) ++i;
    while (j < tb.size() && (tb[j].isLineComment || tb[j].isBlockComment)) ++j;
    if (i >= ta.size() || j >= tb.size()) break;
    if (ta[i].text != tb[j].text) return false;
    ++i; ++j;
  }
  while (i < ta.size() && (ta[i].isLineComment || ta[i].isBlockComment)) ++i;
  while (j < tb.size() && (tb[j].isLineComment || tb[j].isBlockComment)) ++j;
  return i == ta.size() && j == tb.size();
}

} // namespace

bool formatSourceFile(const std::string& path, bool checkOnly) {
  std::ifstream f(path, std::ios::binary);
  if (!f.is_open()) {
    diag(Diag::Error, Code::Io, "fmt: cannot open " + path, path, 0, 0);
    return false;
  }
  std::stringstream ss;
  ss << f.rdbuf();
  std::string src = ss.str();

  Fmt fmt;
  lexForFmt(src, fmt.toks);
  fmt.emitProgram();
  std::string formatted = fmt.out;
  // trailing newline
  while (!formatted.empty() && formatted.back() == '\n') formatted.pop_back();
  formatted += "\n";
  // collapse >2 consecutive newlines to 1 blank line
  {
    std::string collapsed;
    int nl = 0;
    for (char c : formatted) {
      if (c == '\n') {
        if (++nl > 2) continue;
      } else nl = 0;
      collapsed += c;
    }
    formatted = collapsed;
  }

  if (formatted == src) {
    if (!checkOnly) printf("penstock fmt: %s already formatted\n", path.c_str());
    return true;
  }
  // SAFETY: identical token streams or refuse
  if (!tokenStreamsEqual(src, formatted)) {
    fprintf(stderr,
            "penstock fmt: refusing to rewrite %s — the formatted output "
            "would change the token stream\n", path.c_str());
    g_diags.push_back({Diag::Error, info(Code::Fmt).id,
                        "fmt: token stream would change", path, 0, 0, ""});
    return false;
  }
  if (checkOnly) {
    printf("%s: not formatted (penstock fmt --check)\n", path.c_str());
    return false;
  }
  std::ofstream o(path, std::ios::binary | std::ios::trunc);
  if (!o.is_open()) {
    diag(Diag::Error, Code::Io, "fmt: cannot write " + path, path, 0, 0);
    return false;
  }
  o << formatted;
  o.close();
  printf("penstock fmt: formatted %s\n", path.c_str());
  return true;
}

} // namespace freshet
