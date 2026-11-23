// Freshet source formatter — Phase 4 (report §10.2), clang-format-inspired
// layout engine.
//
// `penstock fmt <file.fsht>` / `penstock fmt --check <file.fsht>` and the
// language server's textDocument/formatting + textDocument/rangeFormatting
// ("Format Document" / "Format Selection") share this engine.
//
// Design (compiler-aware, token-level):
//   * A dedicated tokeniser re-reads the source keeping COMMENTS as trivia
//     tokens, so nothing the user wrote is ever dropped.  Number literals
//     are lexed exactly like the compiler lexer — decimal with optional
//     fraction and SIGNED EXPONENT (`1.0e-4`, `2.5e+3`, `1.5E-3`), `0x`
//     hex, u/i/f suffixes (`5u64`) — so a scientific-notation literal is
//     ONE token and the spacing pass can never split it into invalid code
//     (`1.0e - 4`, which the old alnum+dot scan produced).
//   * Spacing is decided per adjacent token PAIR (clang-format's
//     TokenAnnotator role), not by blind rules on single characters:
//       - generic angles hug the name and their contents — `Span<f64>`,
//         `Result<T, E>`, `fn name<T: Numeric>(...)` — while comparison
//         `<`/`>` keep operator spacing (`a < b`, `c > d`);
//       - call/index brackets hug: `foo(a, b)`, `ptr[2 * element]`,
//         `geometry::scale(p, 2.0)`, `x.len`;
//       - unary operators hug their operand (`!finite(x)`, `-x`, `: *u8`);
//         binary operators and `=` take single spaces (`a + b * c`);
//       - postfix `?` hugs (`norm3(...)?;`); `:`/`,`/`;` never take a
//         leading space and `:` takes exactly one trailing space;
//       - parenthesised expressions keep the operator's space
//         (`= (a + b) * c`, `&& (x < y)`).
//   * Line layout:
//       - one statement per line (several `;`-statements on one source
//         line are split); short blocks collapse to `{ ...; ...; }` when
//         they fit the 80-column limit and contain no nested block or
//         comment (`if (c) { return x; }`);
//       - function signatures bin-pack like clang-format: a signature that
//         fits stays on one line, a long one breaks after `(` and packs
//         parameters on continuation lines (one extra indent), closing
//         with `) -> Ret {` — the same for struct/enum/struct-literal
//         bodies and their fields;
//       - long statements wrap like clang-format too (the engine used to
//         emit any statement, however long, on ONE line): a call/index
//         argument list that no longer fits breaks after `(` and
//         bin-packs its arguments on continuation lines (two extra
//         indents), the closing bracket hugging the last argument
//         (`... span(lengths, element_count));`); a binary operator whose
//         following operand run would not fit breaks AFTER the operator
//         (BreakBeforeBinaryOperators = false) with one extra indent —
//         the 80-column limit is a limit again;
//       - blank lines: toplevel declarations are separated by one blank
//         line, but runs of one-line `module`/`import`/`extern` (and other
//         single-line `;`-declarations) stay tightly packed; inside
//         bodies one blank line separates an EXPANDED control-flow block
//         from neighbouring statements, and an author's blank line is kept
//         (at most one — clang-format's MaxEmptyLinesToKeep);
//       - comments stay attached where they were written: a comment ahead
//         of a declaration stays ahead of it (never relocated into the
//         declaration's body).
//   * SAFETY: the formatter refuses to produce a program whose token
//     stream (comments/whitespace stripped) differs from the input's —
//     the file is only rewritten when the formatted text re-lexes to the
//     exact same tokens.  On any doubt the original file is kept.  The
//     gate's lexer now mirrors the compiler's literal syntax, so a
//     mangled number (the old `1.0e - 4` bug) can never pass the gate.
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
  size_t off = 0;        // byte offset of the token start in the source
                         // (range formatting slices declarations by it)
  bool hugAngle = false; // `<` / `>` / `>>` opening or closing a generic
                         // argument list (`Span<f64>`); comparison `<`/`>`
                         // are NOT marked and keep operator spacing
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
  const char* base = src.c_str();
  const char* p = base;
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
      t.isLineComment = true; t.line = line; t.off = (size_t)(s - base);
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
      t.isBlockComment = true; t.line = line; t.off = (size_t)(s - base);
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
      t.off = (size_t)(s - base);
      out.push_back(t);
      continue;
    }
    // numbers, lexed EXACTLY like the compiler lexer (lexer.cpp): decimal
    // with optional fraction and SIGNED EXPONENT (`1.0e-4`, `2.5e+3`,
    // `1e9`, `1.5E-3`), `0x` hex, and the u/i/f suffixes (`5u64`).  The
    // old scan (alnum + `.` + `_`) split `1.0e-4` into `1.0e` / `-` / `4`
    // and the binary-operator spacing then emitted the invalid
    // `1.0e - 4` — and the safety gate's own lexer split it the same way
    // on BOTH sides, so the damage slipped through.  Mirroring the real
    // literal syntax makes scientific notation ONE token (and `3.` /
    // `0..n` token boundaries match the compiler too).
    if (isdigit((unsigned char)c) ||
        (c == '.' && p + 1 < end && isdigit((unsigned char)p[1]))) {
      const char* s = p;
      if (c == '0' && p + 1 < end && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        while (p < end && isxdigit((unsigned char)*p)) ++p;
      } else {
        while (p < end && isdigit((unsigned char)*p)) ++p;
        if (p < end && *p == '.' && p + 1 < end &&
            isdigit((unsigned char)p[1])) {
          ++p;
          while (p < end && isdigit((unsigned char)*p)) ++p;
        }
        if (p < end && (*p == 'e' || *p == 'E') && p + 1 < end &&
            (isdigit((unsigned char)p[1]) ||
             ((p[1] == '+' || p[1] == '-') && p + 2 < end &&
              isdigit((unsigned char)p[2])))) {
          ++p;                          // the e / E
          if (p < end && (*p == '+' || *p == '-')) ++p;
          while (p < end && isdigit((unsigned char)*p)) ++p;
        }
      }
      // suffixes u8/u16/u32/u64/usize/i8/i16/i32/i64/f32/f64, matched in
      // the compiler's order with a word-boundary guard
      static const char* sfx[] = { "u8",  "u16", "u32", "u64", "usize",
                                   "i8",  "i16", "i32", "i64", "f32",
                                   "f64" };
      for (auto* sf : sfx) {
        size_t n = strlen(sf);
        if ((size_t)(end - p) >= n && strncmp(p, sf, n) == 0 &&
            (p + n == end || !fIsIdentChar(p[n]))) {
          p += n;
          break;
        }
      }
      FTok t; t.text = std::string(s, p); t.kind = Token::INT; t.line = line;
      t.off = (size_t)(s - base);
      out.push_back(t);
      continue;
    }
    if (fIsIdentStart(c)) {
      const char* s = p;
      while (p < end && fIsIdentChar(*p)) ++p;
      FTok t; t.text = std::string(s, p); t.kind = Token::IDENT; t.line = line;
      t.off = (size_t)(s - base);
      out.push_back(t);
      continue;
    }
    bool matched = false;
    for (auto* q : puncts) {
      size_t n = strlen(q);
      if ((size_t)(end - p) >= n && strncmp(p, q, n) == 0) {
        FTok t; t.text = q; t.kind = Token::PUNCT; t.line = line;
        t.off = (size_t)(p - base);
        out.push_back(t);
        p += n;
        matched = true;
        break;
      }
    }
    if (!matched) {
      // unknown character: keep verbatim so the safety check trips
      FTok t; t.text = std::string(1, c); t.kind = Token::PUNCT;
      t.line = line; t.off = (size_t)(p - base);
      out.push_back(t);
      ++p;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Generic-angle detection: `Name<args>` vs comparison operators.
//
// A `<` that follows an identifier opens a generic argument list when the
// tokens up to its closing bracket are all type-shaped (identifiers, `,`,
// `:`, `::`, `.`, nested angles, `*`/`&`) and the token AFTER the closing
// bracket is one that can follow a type.  Matched brackets are flagged
// `hugAngle` and print without inner spaces; every unmarked `<`/`>` is a
// comparison and keeps clang-format's binary-operator spacing.
//
// Consequence for ambiguous pairs (`a < b, c > (d)`): the token stream is
// unchanged either way, so the safety gate is never violated; only spacing
// may differ.
void markAngleHugs(std::vector<FTok>& toks) {
  for (size_t i = 0; i < toks.size(); ++i) {
    if (toks[i].text != "<") continue;
    // the opening `<` must follow a name
    size_t p = i;
    while (p > 0 && (toks[p - 1].isLineComment || toks[p - 1].isBlockComment))
      --p;
    if (p == 0) continue;
    if (toks[p - 1].kind != Token::IDENT) continue;
    // ...and the first argument must be a name too (rejects `x < 2`)
    size_t first = i + 1;
    while (first < toks.size() &&
           (toks[first].isLineComment || toks[first].isBlockComment))
      ++first;
    if (first >= toks.size() || toks[first].kind != Token::IDENT) continue;

    int depth = 1;
    size_t closer = SIZE_MAX;
    for (size_t j = i + 1; j < toks.size(); ++j) {
      if (toks[j].isLineComment || toks[j].isBlockComment) continue;
      const std::string& t = toks[j].text;
      if (toks[j].kind == Token::IDENT || t == "," || t == ":" ||
          t == "::" || t == "." || t == "*" || t == "&" || t == "+")
        continue;                      // `+`: compound trait bounds
      if (t == "<") { ++depth; continue; }
      if (t == ">") { if (--depth == 0) closer = j; break; }
      if (t == ">>") {                    // shared closer of nested generics
        depth -= 2;
        if (depth <= 0) closer = j;        // `Vec<Vec<u64>>`
        break;
      }
      // operators, literals, brackets, `;` ...: this `<` is a comparison
      break;
    }
    if (closer == SIZE_MAX) continue;
    // the token after the closing bracket must be able to follow a type
    size_t f = closer + 1;
    while (f < toks.size() &&
           (toks[f].isLineComment || toks[f].isBlockComment))
      ++f;
    if (f < toks.size()) {
      static const std::set<std::string> ok = {
        ",", ")", "]", ";", ">", ">>", "(", "{", "}", "=", "->", "=>",
      };
      if (!ok.count(toks[f].text)) continue;
    }
    toks[i].hugAngle = true;
    toks[closer].hugAngle = true;
  }
}

// control-flow keywords: `(` after these keeps its space (`if (`, `match (`)
bool ctrlKw(const std::string& s) {
  static const std::set<std::string> ctrl = {
    "if", "while", "for", "parfor", "match", "return", "align", "soa",
    "defer", "in",
  };
  return ctrl.count(s) > 0;
}

struct Fmt {
  std::vector<FTok> toks;
  const std::string* srcp = nullptr;   // raw source (blank-line detection)
  size_t pos = 0;
  std::string out;
  int depth = 0;
  int col = 0;                 // current output column
  bool freshLine = true;       // line still empty beyond its indent
  std::vector<FTok> pendingComments;
  // One indentation level.  The house style is four spaces; the language
  // server passes the editor's FormattingOptions through here instead
  // (tabSize 4 / insertSpaces -> "    ", tabSize 2 -> "  ", tabs -> "\t").
  std::string indentUnit = "    ";
  int limit = 80;              // clang-format ColumnLimit analogue
  size_t lastTokEndOff = 0;    // byte end of the last consumed token
  bool suppressAutoBlank = false;   // inside a match body: arms stay tight

  FTok* cur() { return pos < toks.size() ? &toks[pos] : nullptr; }
  bool at(const char* s) {
    FTok* c = cur();
    return c && !c->isLineComment && !c->isBlockComment && c->text == s;
  }
  void advance() {
    if (pos < toks.size()) {
      lastTokEndOff = toks[pos].off + toks[pos].text.size();
      if (toks[pos].isLineComment || toks[pos].isBlockComment)
        pendingComments.push_back(toks[pos]);
      ++pos;
    }
  }

  void nl() {
    while (!out.empty() && out.back() == ' ') out.pop_back();
    out += '\n';
    col = 0;
    freshLine = true;
  }
  void indent(int d) {
    for (int i = 0; i < d; ++i) {
      out += indentUnit;
      col += (int)indentUnit.size();
    }
  }
  void sp() {
    if (col > 0 && !out.empty() && out.back() != ' ' && out.back() != '\n') {
      out += ' ';
      ++col;
      freshLine = false;
    }
  }
  void write(const std::string& s) {
    out += s;
    col += (int)s.size();
    freshLine = false;
  }

  // flush pending comments at the current indent (own lines, never
  // relocated across a declaration boundary)
  void flushComments() {
    for (auto& c : pendingComments) {
      if (!out.empty() && out.back() != '\n') nl();
      indent(depth);
      write(c.text);
      nl();
    }
    pendingComments.clear();
  }

  // ---------------------------------------------------------------- queries

  // did the author leave a BLANK line between byte offsets `a` and `b`
  // (two newlines with nothing but spaces between them)?  Comment-only
  // lines do NOT count as blank — measuring lines would misread
  // `stmt;\n// note\nstmt2` as a blank.
  bool blankBetween(size_t a, size_t b) const {
    if (!srcp) return false;
    const std::string& s = *srcp;
    if (a > s.size()) a = s.size();
    if (b > s.size()) b = s.size();
    bool sawNl = false;
    for (size_t i = a; i < b; ++i) {
      char c = s[i];
      if (c == '\n') {
        if (sawNl) return true;
        sawNl = true;
      } else if (c == ' ' || c == '\t' || c == '\r') {
        // whitespace alone keeps the line empty
      } else {
        sawNl = false;                // comment or code content
      }
    }
    return false;
  }

  // previous non-comment token index before `i` (SIZE_MAX at stream start)
  size_t prevTokIdx(size_t i) {
    while (i > 0) {
      --i;
      if (!toks[i].isLineComment && !toks[i].isBlockComment) return i;
    }
    return SIZE_MAX;
  }

  // operands: tokens a binary operator may follow — identifiers (except
  // control keywords), numbers, strings, closing brackets, postfix `?`,
  // and closed generic angles (`Vec<u64>`)
  bool operandTok(const FTok& t) const {
    if (t.text == ")" || t.text == "]" || t.text == "?") return true;
    if (t.hugAngle) return true;
    if (t.kind == Token::INT || t.kind == Token::STR) return true;
    if (t.kind == Token::IDENT && !ctrlKw(t.text)) return true;
    return false;
  }

  bool isUnaryAt(size_t i) {
    const std::string& t = toks[i].text;
    if (t == "!" || t == "~") return true;
    if (t != "-" && t != "+" && t != "*" && t != "&") return false;
    size_t p = prevTokIdx(i);
    if (p == SIZE_MAX) return true;
    return !operandTok(toks[p]);
  }

  // clang-format-style spacing between the tokens adjacent at `curIdx`
  // (the previous non-comment token and toks[curIdx])
  bool spaceBetween(size_t curIdx) {
    if (curIdx == 0 || curIdx >= toks.size()) return false;
    size_t pi = prevTokIdx(curIdx);
    if (pi == SIZE_MAX) return false;
    const FTok& pv = toks[pi];
    const FTok& c = toks[curIdx];
    const std::string& P = pv.text;
    const std::string& C = c.text;
    // never a space BEFORE these
    if (C == "," || C == ";" || C == ":" || C == ")" || C == "]" ||
        C == "}" || C == "::" || C == "." || C == "?" || C == "..")
      return false;
    // never a space AFTER these
    if (P == "(" || P == "[" || P == "." || P == "@" || P == "::" ||
        P == "..")
      return false;
    if (P == "!" || P == "~") return false;        // prefix operators
    if (isUnaryAt(pi)) return false;                // unary - + * &
    if (pv.hugAngle && P == "<") return false;      // `Span<`f64>
    // call/index brackets hug what they are attached to
    if (C == "(") {
      if (P == ")" || P == "]") return false;       // `foo(x)(y)`
      if (pv.hugAngle) return false;                // `name<T>(`
      if (pv.kind == Token::INT || pv.kind == Token::STR) return false;
      if (pv.kind == Token::IDENT) return ctrlKw(P);// `foo(` vs `if (`
      return true;                                   // `= (`, `&& (`, `, (`
    }
    if (C == "[") {
      if (P == ")" || P == "]") return false;       // `arr[0][1]`
      if (pv.hugAngle) return false;                // `Vec<T>[`?
      if (pv.kind == Token::INT || pv.kind == Token::STR) return false;
      if (pv.kind == Token::IDENT) return ctrlKw(P);// `arr[` vs `in [`
      return true;                                   // `= [`, `, [`
    }
    if (C == "{") return true;                      // `) {`, `Name {`
    if (c.hugAngle) return false;                   // `<f64>`, `u64>>`
    // everything else — binary operators, `=`, `->`, `=>`, identifiers and
    // literals — takes exactly one space
    return true;
  }

  // emit toks[pos] with pair spacing.  When `endIdx` bounds the emission
  // (parameter lists, packed chunks, brace bodies), comment-skipping must
  // stop at the boundary — otherwise a comment sitting right before it
  // would push emission past the chunk end and duplicate the closing token.
  void emitTok(size_t endIdx = SIZE_MAX) {
    while (pos < endIdx && cur() &&
           (cur()->isLineComment || cur()->isBlockComment))
      advance();
    if (pos >= endIdx) return;
    FTok* c = cur();
    if (!c) return;
    if (spaceBetween(pos)) sp();
    write(c->text);
    advance();
  }

  // ------------------------------------------------------------- measurements

  size_t matchParen(size_t open) {
    int d = 0;
    for (size_t j = open; j < toks.size(); ++j) {
      if (toks[j].isLineComment || toks[j].isBlockComment) continue;
      if (toks[j].text == "(") { ++d; continue; }
      if (toks[j].text == ")") { if (--d == 0) return j; }
    }
    return SIZE_MAX;
  }
  size_t matchBrace(size_t open) {
    int d = 0;
    for (size_t j = open; j < toks.size(); ++j) {
      if (toks[j].isLineComment || toks[j].isBlockComment) continue;
      if (toks[j].text == "{") { ++d; continue; }
      if (toks[j].text == "}") { if (--d == 0) return j; }
    }
    return SIZE_MAX;
  }
  size_t matchBracket(size_t open) {
    int d = 0;
    for (size_t j = open; j < toks.size(); ++j) {
      if (toks[j].isLineComment || toks[j].isBlockComment) continue;
      if (toks[j].text == "[") { ++d; continue; }
      if (toks[j].text == "]") { if (--d == 0) return j; }
    }
    return SIZE_MAX;
  }

  // width in columns of toks[from, to) emitted linearly (comments skipped;
  // the pair spacing matches what emission would produce)
  int spanWidth(size_t from, size_t to) {
    int w = 0;
    for (size_t j = from; j < to && j < toks.size(); ++j) {
      if (toks[j].isLineComment || toks[j].isBlockComment) continue;
      w += (int)toks[j].text.size();
      if (spaceBetween(j)) ++w;
    }
    return w;
  }

  // index of the next `,` separating top-level list items, or `to`
  size_t nextTopLevelComma(size_t from, size_t to) {
    int paren = 0, brk = 0, brace = 0, angle = 0;
    for (size_t j = from; j < to; ++j) {
      if (toks[j].isLineComment || toks[j].isBlockComment) continue;
      const std::string& t = toks[j].text;
      if (t == "(") { ++paren; continue; }
      if (t == ")") { --paren; continue; }
      if (t == "[") { ++brk; continue; }
      if (t == "]") { --brk; continue; }
      if (t == "{") { ++brace; continue; }
      if (t == "}") { --brace; continue; }
      if (toks[j].hugAngle) {                    // generic angle brackets
        if (t == "<") { ++angle; continue; }
        if (t == ">") { --angle; continue; }
        if (t == ">>") { angle -= 2; continue; }
      }
      if (t == "," && paren == 0 && brk == 0 && brace == 0 && angle == 0)
        return j;
    }
    return to;
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
      if (t.text == ">" || t.text == ">>") { angle += (t.text == ">>") ? 2 : 1; continue; }
      if (t.text == ")" || t.text == "->" || t.text == "=>" ||
          t.text == "else" || t.text == "unsafe" || t.text == "fn" ||
          t.text == "in" || t.text == "as" || t.text == "impl" ||
          t.text == "trait" || t.text == "match")
        return false;                       // block contexts
      if (t.text == "struct" || t.text == "enum")
        return true;                        // declaration header
      if (t.text == "=" || t.text == "," || t.text == "(" ||
          t.text == "[" || t.text == ":" ||
          t.text == "return" || t.text == "break" || t.text == "continue")
        return true;     // expression position (literal); a `;` before `{`
                        // is a bare block STATEMENT, not a literal
      if (t.text == "{" || t.text == "}")
        return true;                        // statement boundary
      // IDENT / scalar keyword: keep walking (type name or literal name)
    }
    return false;
  }

  // ---------------------------------------------------------------- program

  // does the declaration starting at `start` own a `{` before its `;`?
  // (fn/struct/enum/trait/impl/globals with literals get blank-line
  // separation; one-line module/import/extern declarations group tightly)
  bool declStartsBrace(size_t start) {
    int paren = 0, brk = 0;
    for (size_t j = start; j < toks.size(); ++j) {
      if (toks[j].isLineComment || toks[j].isBlockComment) continue;
      const std::string& t = toks[j].text;
      if (t == "(") { ++paren; continue; }
      if (t == ")") { --paren; continue; }
      if (t == "[") { ++brk; continue; }
      if (t == "]") { --brk; continue; }
      if (t == "{") return true;
      if (t == "}") return false;
      if (t == ";" && paren == 0 && brk == 0) return false;
    }
    return false;
  }

  void emitProgram() {
    bool firstDecl = true;
    bool lastMulti = false;
    while (cur()) {
      // gather the comments leading this declaration (they stay put,
      // attached ahead of the declaration)
      size_t prevEndOff = lastTokEndOff;  // end of the previous declaration
      size_t groupOff = cur()->off;      // start of the comment group
      bool had = false;
      while (cur() && (cur()->isLineComment || cur()->isBlockComment)) {
        advance();
        had = true;
      }
      if (!cur()) {
        if (had) flushComments();          // trailing file comments
        break;
      }
      if (!firstDecl) {
        // one newline ends the previous declaration; a second (blank)
        // separates brace-owning or multi-line declarations, or wherever
        // the author left one (comment-only lines do not count)
        bool blank = lastMulti || declStartsBrace(pos) ||
                     blankBetween(prevEndOff, groupOff);
        nl();
        if (blank) nl();
      }
      flushComments();
      size_t outBefore = out.size();
      size_t posBefore = pos;
      emitToplevel();
      if (pos == posBefore) advance();      // always make progress
      bool multi = false;
      for (size_t k = outBefore; k < out.size(); ++k)
        if (out[k] == '\n') { multi = true; break; }
      lastMulti = multi;
      firstDecl = false;
    }
  }

  // one toplevel declaration: tokens up to `;` (extern/import/module/
  // globals) or through a balanced `{ ... }` (fn/struct/enum/trait/impl)
  void emitToplevel() {
    if (atFnDecl()) { emitFnDecl(); return; }
    while (cur()) {
      FTok* c = cur();
      if (c->isLineComment || c->isBlockComment) {
        advance();
        flushComments();      // comment stays ahead of the declaration
        continue;
      }
      if (c->text == "{") {
        bool header = headerBraceAt(pos);
        size_t braceIdx = pos;
        emitTok();            // `{`
        if (header) {
          emitInlineBraceBody(braceIdx);
        } else {
          ++depth;
          nl();
          emitBlockBody(depth - 1);
          --depth;
        }
        // A toplevel `}` can still be followed by its declaration's `;`
        // (a global initialised with a struct/enum literal:
        // `let p: Point = Point { x: 1, y: 2 };`).  That `;` belongs to
        // THIS declaration — consuming it here stops the formatter from
        // emitting it as its own blank-line-separated pseudo-declaration.
        if (cur() && at(";")) { write(";"); advance(); }
        return;               // declaration complete
      }
      if (c->text == ";") { write(";"); advance(); return; }
      // long globals wrap like statements: operators break after
      // themselves, call argument lists bin-pack
      if (tryBreakAfterOperator()) continue;
      if (!tryBreakCallGroup(SIZE_MAX)) emitTok();
    }
  }

  // [`@attr`] [`extern`|`pub`] `fn` name[<...>](...)
  bool atFnDecl() {
    size_t i = pos;
    for (int guard = 0; guard < 64 && i < toks.size(); ++guard) {
      const FTok& t = toks[i];
      if (t.isLineComment || t.isBlockComment) { ++i; continue; }
      if (t.text == "@") {                 // attribute: @ name [ (args) ]
        ++i;
        while (i < toks.size() &&
               (toks[i].isLineComment || toks[i].isBlockComment))
          ++i;
        if (i < toks.size() && toks[i].kind == Token::IDENT) ++i;
        if (i < toks.size() && toks[i].text == "(") {
          int d = 0;
          for (; i < toks.size(); ++i) {
            if (toks[i].isLineComment || toks[i].isBlockComment) continue;
            if (toks[i].text == "(") { ++d; continue; }
            if (toks[i].text == ")") { if (--d == 0) { ++i; break; } }
          }
        }
        continue;
      }
      if (t.text == "extern" || t.text == "pub") { ++i; continue; }
      if (t.text == "fn") {
        size_t j = i + 1;
        while (j < toks.size() &&
               (toks[j].isLineComment || toks[j].isBlockComment))
          ++j;
        return j < toks.size() && toks[j].kind == Token::IDENT;
      }
      return false;
    }
    return false;
  }

  // fn signature, clang-format style: one line when it fits the column
  // limit, else break after `(` and bin-pack the parameters on continuation
  // lines (one extra indent), closing with `) -> Ret {` / `);`
  void emitFnDecl() {
    while (cur() && !at("(")) emitTok();   // [@attr] extern pub fn name<...>
    if (!cur()) return;
    size_t openIdx = pos;
    size_t closeIdx = matchParen(openIdx);
    if (closeIdx == SIZE_MAX) {             // unbalanced: verbatim dump
      while (cur()) emitTok();
      return;
    }
    // terminator: `{` (definition) or `;` (extern declaration)
    size_t termIdx = SIZE_MAX;
    bool isDef = false;
    for (size_t j = closeIdx + 1; j < toks.size(); ++j) {
      if (toks[j].isLineComment || toks[j].isBlockComment) continue;
      if (toks[j].text == "{") { termIdx = j; isDef = true; break; }
      if (toks[j].text == ";") { termIdx = j; break; }
      if (toks[j].text == "}") break;       // malformed
    }
    if (termIdx == SIZE_MAX) { while (cur()) emitTok(); return; }

    bool commentInSig = false;
    for (size_t j = openIdx; j <= termIdx && j < toks.size(); ++j)
      if (toks[j].isLineComment || toks[j].isBlockComment) {
        commentInSig = true;
        break;
      }
    int sigW = spanWidth(openIdx, termIdx + 1);   // `(` .. `{` / `;`
    bool oneLine = !commentInSig && col + sigW <= limit;

    if (oneLine) {
      while (pos <= termIdx) emitTok(termIdx + 1);   // `(` params `)` [`-> Ret] `{`
    } else {
      emitTok();                            // `(`
      if (pos < closeIdx) {                 // non-empty parameter list
        ++depth;
        nl();
        indent(depth);
        emitPackedList(closeIdx);
        --depth;
        nl();
        indent(depth);
      }
      emitTok();                            // `)`
      while (pos < termIdx) emitTok(termIdx);  // `-> Ret` (generics hug)
      emitTok();                            // `{` or `;`
    }
    if (isDef) {
      ++depth;
      nl();
      emitBlockBody(depth - 1);
      --depth;
    }
  }

  // emit toks[pos, endIdx) as comma-separated chunks, greedily bin-packed
  // onto lines at the current indent (clang-format's BinPackParameters /
  // PackConstructor concept).  Caller positions the first line; `pos`
  // ends at endIdx.  Chunks are split at commas that are top-level in the
  // list (nested `()`/`[]`/`{}`/`<...>` commas belong to inner lists).
  void emitPackedList(size_t endIdx) {
    while (pos < endIdx) {
      size_t chunkEnd = nextTopLevelComma(pos, endIdx);
      int cw = spanWidth(pos, chunkEnd);   // includes the leading space
      // after the previous `,` — so the remaining trailer is the comma
      // itself: +1, and the line never exceeds the column limit
      if (!freshLine && col + cw + 1 > limit) {     // does not fit: wrap
        nl();
        indent(depth);
      }
      // still too long for a line of its own (measured at the chunk's
      // home indent, whether or not it just wrapped): the chunk breaks
      // INSIDE — its argument list bin-packs (recursive
      // BinPackArguments).  A chunk that fits is emitted linearly, its
      // nested groups hugging inline; the outer comma is always the
      // preferred break point.
      bool breakInside = (int)(indentUnit.size() * depth) + cw + 2 > limit;
      while (pos < chunkEnd) {
        if (breakInside && !tryBreakCallGroup(chunkEnd)) emitTok(chunkEnd);
        else emitTok(chunkEnd);
      }
      if (chunkEnd < endIdx) emitTok();    // the separating comma
    }
  }

  // clang-format's BinPackArguments: a call/index ARGUMENT LIST that no
  // longer fits on the current line breaks after its opener and
  // bin-packs its arguments on continuation lines — two extra indent
  // levels, the house argument-list continuation indent — with the
  // closing bracket hugging the last argument
  // (`... span(lengths, element_count));`).  Groups WITHOUT a top-level
  // comma stay inline: parenthesised expressions and single
  // struct-literal arguments keep their `{` on the statement flow, where
  // the brace paths lay them out.  A comment inside the group vetoes
  // breaking (the group is emitted verbatim instead).
  bool tryBreakCallGroup(size_t endIdx) {
    FTok* c = cur();
    if (!c || (c->text != "(" && c->text != "[")) return false;
    size_t pi = prevTokIdx(pos);
    if (pi == SIZE_MAX) return false;
    const FTok& pv = toks[pi];
    bool callish = pv.kind == Token::IDENT && !ctrlKw(pv.text);
    if (pv.text == ")" || pv.text == "]") callish = true;   // f(x)(y), a[i](
    if (pv.hugAngle) callish = true;                       // name<T>(
    if (c->text == "[" && (pv.text == "=" || pv.text == "(" ||
                           pv.text == "," || pv.text == "return"))
      callish = true;                                      // array literal
    if (!callish) return false;
    size_t openIdx = pos;
    size_t closeIdx = c->text == "(" ? matchParen(openIdx)
                                     : matchBracket(openIdx);
    if (closeIdx == SIZE_MAX || closeIdx >= endIdx) return false;
    if (closeIdx == openIdx + 1) return false;             // `()`: hug
    // must be an argument list: at least one top-level comma inside
    if (nextTopLevelComma(openIdx + 1, closeIdx) == closeIdx) return false;
    for (size_t j = openIdx + 1; j < closeIdx; ++j)
      if (toks[j].isLineComment || toks[j].isBlockComment) return false;
    int w = spanWidth(openIdx, closeIdx + 1);              // `(args)`
    if (col + w + 2 <= limit) return false;                // fits: keep
    emitTok();                     // `(` / `[` hugs the callee
    depth += 2;
    nl();
    indent(depth);
    emitPackedList(closeIdx);
    depth -= 2;
    emitTok();                     // `)` / `]` hugs the last argument
    return true;
  }

  // clang-format breaks AFTER a binary operator (LLVM style:
  // BreakBeforeBinaryOperators = false) when the operand run that
  // follows — measured up to the next break opportunity at the same
  // bracket depth (another binary operator, a call/index opener, a
  // closing bracket, or the statement terminator) — would not fit on
  // the current line; the continuation gets ONE extra indent level
  // (ContinuationIndentWidth 4).
  bool tryBreakAfterOperator() {
    static const std::set<std::string> ops = {
      "+", "-", "*", "/", "%", "=", "==", "!=", "<=", ">=", "&&", "||",
      "<<", ">>", "->", "=>", "+=", "-=", "*=", "/=", "%=", "&=", "|=",
      "^=", "<<=", ">>=",
    };
    FTok* c = cur();
    if (!c || !ops.count(c->text)) return false;
    if (isUnaryAt(pos)) return false;          // prefix -/+/&/* hug
    size_t next = pos + 1;
    while (next < toks.size() &&
           (toks[next].isLineComment || toks[next].isBlockComment))
      ++next;
    if (next >= toks.size()) return false;    // nothing follows: keep
    int w = 0;
    int paren = 0, brk = 0, brace = 0;
    for (size_t j = next; j < toks.size(); ++j) {
      const FTok& t = toks[j];
      if (t.isLineComment || t.isBlockComment) continue;
      const std::string& tx = t.text;
      if (paren == 0 && brk == 0 && brace == 0) {
        if ((ops.count(tx) && !isUnaryAt(j)) || tx == "{" || tx == "}" ||
            tx == ")" || tx == "]")
          break;                        // next break opportunity
        if (tx == "(" || tx == "[") {
          // an ARGUMENT LIST breaks after its opener (hook A below), so
          // it ends this run at its callee — `= callee(` stays on the
          // operator's line and the list breaks after `(`.  Other
          // bracket groups (casts, parenthesised expressions) emit
          // linearly and their width counts toward the run.
          size_t close = tx == "(" ? matchParen(j) : matchBracket(j);
          if (close == SIZE_MAX) break;
          if (nextTopLevelComma(j + 1, close) != close) break;
          w += spanWidth(j, close + 1);
          j = close;                    // skip past the balanced group
          continue;
        }
      }
      if (tx == "(") ++paren;
      else if (tx == ")") { if (paren == 0) break; --paren; }
      else if (tx == "[") ++brk;
      else if (tx == "]") { if (brk == 0) break; --brk; }
      else if (tx == "{") ++brace;
      else if (tx == "}") { if (brace == 0) break; --brace; }
      w += (int)tx.size();
      if (j > next && spaceBetween(j)) ++w;
      if (tx == ";") break;            // statement terminator (width kept)
    }
    // `col` already holds everything before the operator; +2 is the space
    // on each side of it, +w the operand run that would follow
    if (col + (int)c->text.size() + 2 + w <= limit) return false;
    emitTok();                          // operator, at the line's end
    nl();
    indent(depth + 1);                 // continuation indent
    return true;
  }

  // inside a header brace (struct/enum body or struct literal): one line
  // when short, else fields bin-packed one per line group.  `braceIdx`
  // is the index of the (already consumed) `{`.
  void emitInlineBraceBody(size_t braceIdx) {
    size_t closeIdx = matchBrace(braceIdx);
    if (closeIdx == SIZE_MAX) {             // unbalanced: verbatim dump
      while (cur()) emitTok();
      return;
    }
    bool hasComment = false;
    for (size_t j = pos; j < closeIdx; ++j)
      if (toks[j].isLineComment || toks[j].isBlockComment) {
        hasComment = true;
        break;
      }
    int innerW = spanWidth(pos, closeIdx);
    if (!hasComment && col + innerW + 2 <= limit) {
      // ` x: 1, y: 2 }`
      while (pos < closeIdx) emitTok(closeIdx);
      write(" }");
      advance();                            // consume `}`
      return;
    }
    ++depth;
    nl();
    indent(depth);
    emitPackedList(closeIdx);
    --depth;
    nl();
    indent(depth);
    advance();                             // consume `}`
    write("}");
  }

  // statements inside a block until the matching `}` (consumed); the
  // closing brace is written at `closeIndent` levels of 4-space indent.
  // One blank line separates an expanded control-flow block from its
  // neighbours (clang-format keeps blocks compact, but the house style
  // asks for breathing room between logical blocks); an author's blank
  // line is always kept (at most one).
  struct StmtMeta {
    int firstLine = 0, lastLine = 0;
    bool isBlockStmt = false;
    bool fitsInline = true;
  };

  // measure the statement starting at `start` (its first token — no
  // comments): source lines, whether it owns a `{`, whether a short block
  // would stay inline
  StmtMeta peekStatement(size_t start) {
    StmtMeta m;
    int paren = 0, brk = 0, brace = 0;
    int w = (int)indentUnit.size() * depth;
    bool saw = false;
    size_t j = start;
    while (j < toks.size()) {
      const FTok& t = toks[j];
      if (t.isLineComment || t.isBlockComment) {
        m.fitsInline = false;              // comment forces expansion
        ++j;
        continue;
      }
      if (!saw) { m.firstLine = t.line; saw = true; }
      m.lastLine = t.line;
      const std::string& tx = t.text;
      bool endsStmt = false;
      if (tx == "{") {
        if (brace == 0 && paren == 0 && brk == 0) m.isBlockStmt = true;
        if (brace >= 1) m.fitsInline = false;   // nested block
        ++brace;
      } else if (tx == "}") {
        --brace;
        if (brace <= 0) {
          brace = 0;
          // end of the statement's own block — unless `else` continues it
          size_t k = j + 1;
          while (k < toks.size() &&
                 (toks[k].isLineComment || toks[k].isBlockComment))
            ++k;
          if (!(k < toks.size() && toks[k].text == "else")) endsStmt = true;
        }
      } else if (tx == "(") ++paren;
      else if (tx == ")") --paren;
      else if (tx == "[") ++brk;
      else if (tx == "]") --brk;
      else if (tx == ";" && paren == 0 && brk == 0 && brace == 0)
        endsStmt = true;
      w += (int)tx.size();
      if (j > start && spaceBetween(j)) ++w;
      if (w > limit) m.fitsInline = false;
      ++j;
      if (endsStmt) break;
    }
    return m;
  }

  void emitBlockBody(int closeIndent) {
    bool first = true;
    size_t prevEndOff = 0;        // byte end of the previous statement
    bool prevExpands = false;
    while (cur()) {
      // comments ahead of this statement are emitted HERE — never
      // deferred past the statement they annotate
      size_t groupOff = cur()->off;   // start of comment group / statement
      while (cur() && (cur()->isLineComment || cur()->isBlockComment)) {
        advance();                       // stash
      }
      FTok* c = cur();
      if (!c) break;
      if (c->text == "}") {
        flushComments();                 // trailing comments of the block
        advance();
        break;
      }
      StmtMeta m = peekStatement(pos);
      if (!first) {
        bool gapBlank = blankBetween(prevEndOff, groupOff);
        bool curExpands = m.isBlockStmt && !m.fitsInline;
        bool blank = gapBlank ||
                     (!suppressAutoBlank && (prevExpands || curExpands));
        if (blank) nl();    // statements already ended with a newline
      }
      flushComments();     // the statement's leading comment lines
      emitStatement();
      prevEndOff = lastTokEndOff;
      prevExpands = m.isBlockStmt && !m.fitsInline;
      first = false;
    }
    indent(closeIndent);
    write("}");
  }

  // one statement: tokens up to `;`, or through a balanced `{...}` block
  void emitStatement() {
    if (col == 0) indent(depth);
    bool isMatchStmt = cur() && toks[pos].text == "match";
    while (cur()) {
      FTok* c = cur();
      if (c->isLineComment || c->isBlockComment) { advance(); continue; }
      if (c->text == "}") return;         // block end (caller closes)
      if (c->text == "{") {
        if (headerBraceAt(pos)) {
          // struct literal in expression position
          size_t braceIdx = pos;
          emitTok();
          emitInlineBraceBody(braceIdx);
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
          return;           // block-terminated statement complete
        } else {
          emitTok();        // `{`
          if (isMatchStmt) suppressAutoBlank = true;   // arms stay tight
          ++depth;
          nl();
          emitBlockBody(depth - 1);
          --depth;
          suppressAutoBlank = false;
          // `else` / `else if` continues the same statement line
          if (cur() && at("else")) {
            sp();
            write("else");
            advance();
            if (at("if")) { sp(); continue; }
            continue;
          }
          nl();
          return;           // block-terminated statement complete
        }
        continue;
      }
      if (c->text == ";") {
        int semiLine = c->line;
        write(";");
        advance();
        // a comment that shared the `;`'s source line stays trailing on
        // this output line instead of moving below the statement
        if (cur() && (cur()->isLineComment || cur()->isBlockComment) &&
            cur()->line == semiLine) {
          size_t cOff = cur()->off;
          sp();
          write(cur()->text);
          advance();                  // advance() stashed it — undo that
          if (!pendingComments.empty() &&
              pendingComments.back().off == cOff)
            pendingComments.pop_back();
        }
        nl();
        return;
      }
      // clang-format-style wrapping for long statements (the engine used
      // to emit any statement, however long, on ONE line): a binary
      // operator whose following operand run would not fit breaks AFTER
      // the operator (one extra indent); a call/index argument list that
      // no longer fits breaks after its opener and bin-packs its
      // arguments (two extra indents).
      if (tryBreakAfterOperator()) continue;
      if (!tryBreakCallGroup(SIZE_MAX)) emitTok();
    }
  }

  // heuristic: block containing no nested `{`, no comment, fitting the
  // column limit — collapse to `{ ...; ...; }`
  bool looksShortBlock() {
    size_t closeIdx = matchBrace(pos);
    if (closeIdx == SIZE_MAX) return false;
    for (size_t j = pos + 1; j < closeIdx; ++j) {
      if (toks[j].isLineComment || toks[j].isBlockComment) return false;
      if (toks[j].text == "{") return false;      // nested block
    }
    int innerW = spanWidth(pos + 1, closeIdx);
    return col + innerW + 4 <= limit;             // ` { ...; }`
  }

  void emitShortBlockInline() {
    advance();                 // consume `{` — we write our own spacing
    write("{");
    sp();
    while (cur()) {
      FTok* c = cur();
      if (c->isLineComment || c->isBlockComment) { advance(); continue; }
      if (c->text == "}") { advance(); break; }
      emitTok();
    }
    sp();
    write("}");
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

// ---------------------------------------------------------------------------
// In-memory formatter surface for the language server (LSP textDocument/
// formatting + textDocument/rangeFormatting).  The CLI keeps its file-based
// entry point below; both share the same token-level engine and the same
// token-preserving safety gate.

// FormattingOptions (LSP) -> one indentation level of source text.
static std::string indentUnitFor(int tabSize, bool insertSpaces) {
  if (!insertSpaces) return "\t";
  if (tabSize < 1) tabSize = 4;
  if (tabSize > 16) tabSize = 16;   // degenerate client options
  return std::string((size_t)tabSize, ' ');
}

// run the token-level formatter over a complete "program" (token stream)
static std::string formatTokenStream(const std::string& src,
                                     const std::string& indentUnit) {
  Fmt fmt;
  fmt.indentUnit = indentUnit;
  fmt.srcp = &src;
  lexForFmt(src, fmt.toks);
  markAngleHugs(fmt.toks);
  fmt.emitProgram();
  return fmt.out;
}

// document-level post-processing: exactly one trailing newline and runs
// of 3+ newlines collapsed to a single blank line (the CLI's rules)
static std::string normalizeDocument(const std::string& raw) {
  std::string t = raw;
  while (!t.empty() && t.back() == '\n') t.pop_back();
  t += "\n";
  std::string collapsed;
  int nl = 0;
  for (char c : t) {
    if (c == '\n') { if (++nl > 2) continue; }
    else nl = 0;
    collapsed += c;
  }
  return collapsed;
}

// Byte spans of the toplevel declarations in `toks`.  A declaration runs
// from the first token after the previous declaration's terminator
// (leading comments attach FORWARD, matching the formatter's own
// flushComments placement) through either a `;` at brace depth 0
// (module/import/extern/global) or the `}` that closes a toplevel
// fn/struct/enum body — plus a `;` that immediately follows such a `}`
// (a global initialised with a struct literal, matching the `;`-consume
// in emitToplevel).  Whitespace between declarations belongs to no span,
// so range formatting never rewrites bytes outside a declaration.
static std::vector<std::pair<size_t, size_t>>
toplevelDeclSpans(const std::vector<FTok>& toks) {
  std::vector<std::pair<size_t, size_t>> spans;
  size_t startIdx = 0;
  int depth = 0;
  bool open = false;
  for (size_t i = 0; i < toks.size(); ++i) {
    const FTok& t = toks[i];
    if (t.isLineComment || t.isBlockComment) continue;
    open = true;
    if (t.text == "{") { ++depth; continue; }
    if (t.text == "}") {
      if (depth > 0) --depth;
      if (depth == 0) {
        size_t endOff = t.off + t.text.size();
        size_t nextIdx = i + 1;
        // an immediately following `;` ends the SAME declaration
        size_t j = i + 1;
        while (j < toks.size() &&
               (toks[j].isLineComment || toks[j].isBlockComment)) ++j;
        if (j < toks.size() && toks[j].text == ";") {
          endOff = toks[j].off + toks[j].text.size();
          nextIdx = j + 1;
        }
        spans.push_back({toks[startIdx].off, endOff});
        open = false;
        startIdx = nextIdx;
        i = nextIdx - 1;
      }
      continue;
    }
    if (t.text == ";" && depth == 0) {
      spans.push_back({toks[startIdx].off, t.off + t.text.size()});
      open = false;
      startIdx = i + 1;
    }
  }
  if (open && startIdx < toks.size()) {
    const FTok& last = toks.back();
    spans.push_back({toks[startIdx].off, last.off + last.text.size()});
  }
  return spans;
}

} // namespace

std::string formatSourceText(const std::string& src, int tabSize,
                             bool insertSpaces, bool* okOut) {
  std::string unit = indentUnitFor(tabSize, insertSpaces);
  std::string formatted = normalizeDocument(formatTokenStream(src, unit));
  // SAFETY: identical token streams or refuse (same gate as the CLI)
  bool ok = formatted == src || tokenStreamsEqual(src, formatted);
  if (okOut) *okOut = ok;
  return formatted;
}

RangeFormatResult formatSourceRange(const std::string& src, size_t startOff,
                                     size_t endOff, int tabSize,
                                     bool insertSpaces) {
  RangeFormatResult r;
  if (startOff > src.size()) startOff = src.size();
  if (endOff > src.size()) endOff = src.size();
  if (startOff > endOff) { size_t tmp = startOff; startOff = endOff; endOff = tmp; }
  std::string unit = indentUnitFor(tabSize, insertSpaces);
  std::vector<FTok> toks;
  lexForFmt(src, toks);
  auto spans = toplevelDeclSpans(toks);
  // declarations intersecting the selection — snap outward to whole
  // declarations so the formatter always sees self-contained programs
  size_t first = SIZE_MAX, last = SIZE_MAX;
  for (size_t i = 0; i < spans.size(); ++i) {
    if (spans[i].first < endOff && spans[i].second > startOff) {
      if (first == SIZE_MAX) first = i;
      last = i;
    }
  }
  if (first == SIZE_MAX) return r;          // nothing selected to format
  r.start = spans[first].first;
  r.end = spans[last].second;
  std::string frag = src.substr(r.start, r.end - r.start);
  std::string formatted = formatTokenStream(frag, unit);
  // fragment post-processing: collapse 3+ newline runs only — the bytes
  // AFTER the span stay exactly as the user wrote them, so no trailing-
  // newline rule applies here
  {
    std::string collapsed;
    int nl = 0;
    for (char c : formatted) {
      if (c == '\n') { if (++nl > 2) continue; }
      else nl = 0;
      collapsed += c;
    }
    formatted = collapsed;
  }
  if (!tokenStreamsEqual(frag, formatted)) return r;   // gate refusal
  r.text = formatted;
  r.ok = true;
  return r;
}

bool formatSourceFile(const std::string& path, bool checkOnly) {
  std::ifstream f(path, std::ios::binary);
  if (!f.is_open()) {
    diag(Diag::Error, Code::Io, "fmt: cannot open " + path, path, 0, 0);
    return false;
  }
  std::stringstream ss;
  ss << f.rdbuf();
  std::string src = ss.str();

  bool ok = false;
  std::string formatted = formatSourceText(src, 4, true, &ok);

  if (formatted == src) {
    if (!checkOnly) printf("penstock fmt: %s already formatted\n", path.c_str());
    return true;
  }
  // SAFETY: identical token streams or refuse
  if (!ok) {
    fprintf(stderr,
            "penstock fmt: refusing to rewrite %s — the formatted output "
            "would change the token stream\n", path.c_str());
    g_diags.push_back({Diag::Error, info(Code::Fmt).id,
                        "fmt: token stream would change", path, 0, 0, 0, 0, ""});
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
