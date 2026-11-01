// Freshet lexer
#include "common.h"
#include <cctype>

namespace freshet {

static const char* KEYWORDS[] = {
  "fn","let","mut","return","defer","if","else","while","for","parfor","in",
  "struct","align","true","false","as","export","extern","give","by","step",
  "reduction","threads","chunk","affinity","collapse","close","spread",
  "soa","sizeof","alignof","nil",
  // Phase 1a (report §4.4): break/continue loop exits
  "break","continue",
  // Phase 1d (report §4.3): enums + match
  "enum","match",
  // Phase MEM §5.1: unsafe scopes
  "unsafe",
  // Phase 2b (report §4.5): the UTF-8 string view type
  "str",
  // Phase 3a (report §6.1): module system — file-based module identity,
  // import declarations, pub visibility.  `fn` doubles as the function-
  // pointer type introducer in type position (Phase 3b, report §6.3).
  "module","import","pub",
};

static bool isIdentStart(char c) { return isalpha((unsigned char)c) || c == '_'; }
static bool isIdentChar(char c)  { return isalnum((unsigned char)c) || c == '_'; }

static bool isKeyword(const std::string& s) {
  for (auto k : KEYWORDS) if (s == k) return true;
  return false;
}

int lexFile(const std::string& path, const std::string& src,
            std::vector<Token>& toks) {
  const char* base = src.c_str();
  const char* p = base;
  const char* end = p + src.size();
  int line = 1, col = 1;
  std::string curFile = path;
  auto adv = [&](int n = 1) { for (int i = 0; i < n && p < end; ++i) { if (*p == '\n') { ++line; col = 1; } else ++col; ++p; } };
  // Stage E (report §3.2): mk now records byte offsets and end position.
  // `startP` is the byte position the token began at; we fill `endOffset`/
  // `endLine`/`endCol` after the consume loop finishes.  Callers that
  // want a single-point span (e.g. for synthetic tokens) get the
  // historical behaviour via mkPoint().
  auto mk = [&](Token::K k) {
    Token t;
    t.kind = k;
    t.loc.file = curFile;
    t.loc.line = line;
    t.loc.col = col;
    t.loc.startOffset = (uint64_t)(p - base);
    return t;
  };
  // finalise a token's end position after the consume loop is done.
  auto finish = [&](Token& t, const char* startP) {
    t.loc.endOffset = (uint64_t)(p - base);
    if (t.loc.endOffset == t.loc.startOffset) {
      // zero-length (synthetic): end == start
      t.loc.endLine = t.loc.line;
      t.loc.endCol  = t.loc.col;
    } else {
      // end position is one past the last consumed character — back up
      // one to point at the last byte of the token, then translate to
      // (line, col) by walking forward from startP.  This is O(tokenLen)
      // which is fine for tokens (always short).
      const char* last = p - 1;
      int l = t.loc.line, c = t.loc.col;
      const char* q = startP;
      while (q < last) {
        if (*q == '\n') { ++l; c = 1; } else ++c;
        ++q;
      }
      t.loc.endLine = l;
      t.loc.endCol  = c + 1;   // one past the last char
    }
  };

  while (p < end) {
    // whitespace
    while (p < end && isspace((unsigned char)*p)) adv();
    if (p >= end) break;
    char c = *p;

    // comments: // and /* */
    if (c == '/' && p + 1 < end && p[1] == '/') {
      while (p < end && *p != '\n') adv();
      continue;
    }

    // #line N "file" — preprocessor line markers
    if (c == '#') {
      const char* q = p + 1;   // skip '#'
      while (q < end && (*q == ' ' || *q == '\t')) ++q;
      if (q + 5 <= end && strncmp(q, "line", 4) == 0) {
        // consume to end of line, parse N and optional "file"
        q += 4;
        while (q < end && (*q == ' ' || *q == '\t')) ++q;
        long n = 0;
        while (q < end && isdigit((unsigned char)*q)) { n = n * 10 + (*q - '0'); ++q; }
        std::string f;
        while (q < end && (*q == ' ' || *q == '\t')) ++q;
        if (q < end && *q == '"') {
          ++q;
          while (q < end && *q != '"' && *q != '\n') { f += *q; ++q; }
          if (q < end && *q == '"') ++q;
        }
        // skip rest of line including newline
        while (q < end && *q != '\n') ++q;
        if (q < end) ++q;
        // fast-forward p to q, updating counters
        while (p < q) adv();
        line = (int)n;
        col = 1;
        if (!f.empty()) curFile = f;
        continue;
      }
    }
    if (c == '/' && p + 1 < end && p[1] == '*') {
      adv(2);
      while (p < end && !(p[0] == '*' && p + 1 < end && p[1] == '/')) adv();
      if (p < end) adv(2);
      continue;
    }

    // numbers: 123, 1.5, 1e9, 0x1F, 1.5e-3, 3. (reject bare trailing dot)
    if (isdigit((unsigned char)c) || (c == '.' && p + 1 < end && isdigit((unsigned char)p[1]))) {
      Token t = mk(Token::INT);
      const char* start = p;
      std::string lex;
      bool flt = false;
      if (c == '0' && p + 1 < end && (p[1] == 'x' || p[1] == 'X')) {
        adv(2); lex = "0x";
        while (p < end && isxdigit((unsigned char)*p)) { lex += *p; adv(); }
        t.ival = strtoull(lex.c_str(), nullptr, 16);
      } else {
        while (p < end && isdigit((unsigned char)*p)) { lex += *p; adv(); }
        if (p < end && *p == '.' && p + 1 < end && isdigit((unsigned char)p[1])) {
          flt = true;
          lex += *p; adv();
          while (p < end && isdigit((unsigned char)*p)) { lex += *p; adv(); }
        }
        if (p < end && (*p == 'e' || *p == 'E') &&
            (p + 1 < end && (isdigit((unsigned char)p[1]) ||
             ((p[1] == '+' || p[1] == '-') && p + 2 < end && isdigit((unsigned char)p[2]))))) {
          flt = true;
          lex += *p; adv();
          if (*p == '+' || *p == '-') { lex += *p; adv(); }
          while (p < end && isdigit((unsigned char)*p)) { lex += *p; adv(); }
        }
        if (flt) t.fval = strtod(lex.c_str(), nullptr);
        else t.ival = strtoull(lex.c_str(), nullptr, 10);
      }
      // suffixes: u8/u16/u32/u64/usize/i*/f32/f64
      static const char* sfx[] = {"u8","u16","u32","u64","usize","i8","i16","i32","i64","f32","f64"};
      for (auto s : sfx) {
        size_t n = strlen(s);
        if (p + n <= end && strncmp(p, s, n) == 0 &&
            (p + n == end || !isIdentChar(p[n]))) {
          if (std::string(s).find('f') != std::string::npos) {
            if (!flt) { t.fval = (double)t.ival; flt = true; }
          }
          adv(n); break;
        }
      }
      t.isflt = flt;
      t.kind = flt ? Token::FLT : Token::INT;
      t.text = std::string(start, p);
      finish(t, start);
      toks.push_back(t);
      continue;
    }

    // identifiers / keywords
    if (isIdentStart(c)) {
      Token t = mk(Token::IDENT);
      const char* start = p;
      while (p < end && isIdentChar(*p)) adv();
      t.text = std::string(start, p);
      if (isKeyword(t.text)) t.kind = Token::KW;
      finish(t, start);
      toks.push_back(t);
      continue;
    }

    // strings (only for print-style intrinsics)
    if (c == '"') {
      Token t = mk(Token::STR);
      const char* start = p;
      adv();
      std::string s;
      while (p < end && *p != '"') {
        if (*p == '\\' && p + 1 < end) {
          adv();
          char e = *p;
          switch (e) { case 'n': s += '\n'; break; case 't': s += '\t'; break;
            case '\\': s += '\\'; break; case '"': s += '"'; break;
            case '0': s += '\0'; break; default: s += e; }
          adv();
        } else { s += *p; adv(); }
      }
      if (p < end) adv(); // closing quote
      t.sval = s; t.text = s;
      finish(t, start);
      toks.push_back(t);
      continue;
    }

    // punctuation — longest match first
    static const char* puncts[] = {
      "..", "->", "=>", "==", "!=", "<=", ">=", "&&", "||", "+=", "-=", "*=", "/=",
      "%=", "<<=", ">>=", "<<", ">>", "&=", "|=", "^=", "::",
      "(", ")", "{", "}", "[", "]", ",", ";", ":", ".", "+", "-", "*", "/",
      "%", "=", "<", ">", "!", "&", "|", "^", "~", "@", "?",
    };
    bool matched = false;
    for (auto q : puncts) {
      size_t n = strlen(q);
      if (p + n <= end && strncmp(p, q, n) == 0) {
        Token t = mk(Token::PUNCT);
        const char* start = p;
        t.text = q;
        adv(n);
        finish(t, start);
        toks.push_back(t);
        matched = true;
        break;
      }
    }
    if (!matched) {
      diag(Diag::Error, Code::Lex, std::string("unexpected character '") + c + "'",
           path, line, col);
      adv();
    }
  }
  Token t = mk(Token::EOFTOK);
  t.loc.endOffset = t.loc.startOffset;
  t.loc.endLine = t.loc.line;
  t.loc.endCol  = t.loc.col;
  t.text = "<eof>";
  toks.push_back(t);
  return 0;
}

} // namespace freshet
