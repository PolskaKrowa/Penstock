// Freshet preprocessor — #include / #define (object + function-like macros) /
// #undef / #ifdef / #ifndef / #else / #endif / #link / #pragma once.
// Line-directive aware so diagnostics point at real files/lines.
#include "common.h"
#include <fstream>
#include <sstream>

namespace freshet {

struct Macro {
  std::string name;
  std::vector<std::string> params;   // empty = object-like
  std::string body;
  SrcLoc loc;
};

struct PpState {
  std::unordered_map<std::string, Macro> macros;
  std::set<std::string> includedOnce;   // #pragma once paths
  std::set<std::string> activeFile;     // include cycle guard
  std::vector<std::string>* linkLibs;
  int totalLines = 0;
  bool failed = false;
};

static std::string readFile(const std::string& path, bool& ok) {
  std::ifstream f(path, std::ios::binary);
  if (!f) { ok = false; return ""; }
  std::stringstream ss;
  ss << f.rdbuf();
  ok = true;
  return ss.str();
}

static std::string dirName(const std::string& p) {
  size_t s = p.find_last_of('/');
  return (s == std::string::npos) ? "." : p.substr(0, s);
}

static std::string resolveInclude(PpState& st, const std::string& spec,
                                  const std::string& curFile) {
  std::vector<std::string> cands;
  cands.push_back(dirName(curFile) + "/" + spec);
  for (auto& d : g_opts.includeDirs) cands.push_back(d + "/" + spec);
  for (auto& c : cands) {
    bool ok;
    readFile(c, ok);
    if (ok) return c;
  }
  return "";
}

// Split macro body into tokens for parameter substitution (simple, no
// stringify/paste — out of scope, documented in language spec).
static std::vector<std::string> tokenizeSimple(const std::string& s) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < s.size()) {
    if (isspace((unsigned char)s[i])) { ++i; continue; }
    size_t j = i;
    if (s[i] == '"') {
      // FLUME PATCH (upstream bug): string literals must survive macro
      // re-tokenization as ONE token — the historical tokenizer split them
      // into quote/word/word/quote tokens, so re-joining with single
      // spaces corrupted interiors (multi-space collapsing) and any
      // macro-using line that also contained `::` was re-emitted as
      // `: :`, which the parser rejects.
      j = i + 1;
      while (j < s.size() && s[j] != '"') {
        if (s[j] == '\\' && j + 1 < s.size()) ++j;
        ++j;
      }
      if (j < s.size()) ++j;               // include the closing quote
      out.push_back(s.substr(i, j - i));
      i = j;
      continue;
    }
    if (isalpha((unsigned char)s[i]) || s[i] == '_') {
      while (j < s.size() && (isalnum((unsigned char)s[j]) || s[j] == '_')) ++j;
    } else if (isdigit((unsigned char)s[i])) {
      while (j < s.size() && (isalnum((unsigned char)s[j]) || s[j] == '.' ||
             s[j] == '_')) ++j;
    } else {
      ++j;
      // multi-char puncts (FLUME PATCH: :: => &= |= ^= were missing and
      // got split into single chars, breaking qualified names on
      // macro-expanded lines)
      static const char* two[] = {"..","::","=>","==","!=","<=",">=","&&","||",
                                  "+=","-=","*=","/=","%=","->","<<",">>",
                                  "&=","|=","^="};
      if (i + 1 < s.size()) {
        std::string two2 = s.substr(i, 2);
        for (auto t : two) if (two2 == t) { j = i + 2; break; }
      }
    }
    out.push_back(s.substr(i, j - i));
    i = j;
  }
  return out;
}

static std::string applyMacros(PpState& st, const std::string& text,
                               const SrcLoc& loc, int depth = 0);

// Expand one macro invocation starting at token index ti of toks.
static std::string expandMacro(PpState& st, const Macro& m,
                               const std::vector<std::string>& toks,
                               size_t& ti, const SrcLoc& loc, int depth) {
  if (depth > 16) {
    diag(Diag::Error, Code::Preproc, "macro recursion too deep: " + m.name,
         loc);
    st.failed = true;
    return "";
  }
  std::map<std::string, std::string> args;
  if (!m.params.empty()) {
    // function-like: expect '(' ... ')'
    size_t j = ti + 1;
    if (j >= toks.size() || toks[j] != "(") return toks[ti]; // not an invocation
    std::vector<std::vector<std::string>> actuals;
    std::vector<std::string> cur;
    int depthP = 0;
    ++j;
    while (j < toks.size()) {
      const std::string& t = toks[j];
      if (t == "(") ++depthP;
      if (t == ")") {
        if (depthP == 0) { actuals.push_back(cur); ++j; break; }
        --depthP;
      }
      if (t == "," && depthP == 0) { actuals.push_back(cur); cur.clear(); ++j; continue; }
      cur.push_back(t);
      ++j;
    }
    if (actuals.size() == 1 && actuals[0].empty()) actuals.clear();
    if (actuals.size() != m.params.size()) {
      diag(Diag::Error, Code::Preproc, "macro " + m.name + " expects " +
           std::to_string(m.params.size()) + " args, got " +
           std::to_string(actuals.size()), loc);
      st.failed = true;
    }
    for (size_t i = 0; i < m.params.size() && i < actuals.size(); ++i) {
      std::string joined;
      for (auto& t : actuals[i]) { if (!joined.empty()) joined += " "; joined += t; }
      args[m.params[i]] = joined;
    }
    ti = j;  // consumed the invocation
  } else {
    ti = ti + 1;  // consumed the name
  }
  std::string out;
  auto bodyToks = tokenizeSimple(m.body);
  for (auto& t : bodyToks) {
    if (args.count(t)) { out += (out.empty() ? "" : " ") + args[t]; }
    else if (st.macros.count(t) && t != m.name) {
      // nested expansion of other macros
      out += (out.empty() ? "" : " ");
      std::vector<std::string> sub = tokenizeSimple(applyMacros(st, t, loc, depth + 1));
      for (auto& s2 : sub) { out += (out == "" ? "" : " ") ; }
      // simple: expand recursively as text
      out += applyMacros(st, t, loc, depth + 1);
    } else {
      out += (out.empty() ? "" : " ") + t;
    }
  }
  return out;
}

static std::string applyMacros(PpState& st, const std::string& text,
                               const SrcLoc& loc, int depth) {
  // fast path: no macro name anywhere in the line — keep text verbatim
  // (protects comments, strings, and formatting from re-tokenization)
  if (st.macros.empty()) return text;
  bool any = false;
  for (auto& kv : st.macros)
    if (text.find(kv.first) != std::string::npos) { any = true; break; }
  if (!any) return text;

  // split off a trailing // comment so it survives expansion
  std::string code = text, comment;
  if (text.find('"') == std::string::npos) {
    size_t cm = text.find("//");
    if (cm != std::string::npos) {
      comment = text.substr(cm);
      code = text.substr(0, cm);
    }
  }

  auto toks = tokenizeSimple(code);
  std::string out;
  size_t i = 0;
  while (i < toks.size()) {
    const std::string& t = toks[i];
    auto it = st.macros.find(t);
    if (it != st.macros.end()) {
      std::string e = expandMacro(st, it->second, toks, i, loc, depth);
      out += (out.empty() ? "" : " ") + e;
    } else {
      out += (out.empty() ? "" : " ") + t;
      ++i;
    }
  }
  return out + (out.empty() && !comment.empty() ? "" : "") + comment;
}

// Process one file, appending to `out` with #line markers.
static void processFile(PpState& st, const std::string& path,
                        std::string& out, int sysDepth = 0) {
  if (st.activeFile.count(path)) {
    diag(Diag::Error, Code::Preproc, "circular #include of " + path, path, 0, 0);
    st.failed = true;
    return;
  }
  if (sysDepth > 64) { st.failed = true; return; }
  bool ok = false;
  std::string src = readFile(path, ok);
  if (!ok) {
    diag(Diag::Error, Code::Preproc, "cannot open file: " + path, path, 0, 0);
    st.failed = true;
    return;
  }
  st.activeFile.insert(path);

  char buf[64];
  snprintf(buf, sizeof buf, "#line 1 \"%s\"\n", path.c_str());
  out += buf;

  std::istringstream ss(src);
  std::string line;
  int lineNo = 0;
  // conditional stack: true = currently emitting
  std::vector<bool> condStack;
  auto emitting = [&]() {
    for (bool b : condStack) if (!b) return false;
    return true;
  };

  while (std::getline(ss, line)) {
    ++lineNo;
    // strip \r
    if (!line.empty() && line.back() == '\r') line.pop_back();
    // detect directive
    size_t ns = line.find_first_not_of(" \t");
    bool isDir = ns != std::string::npos && line[ns] == '#';
    std::string dir = isDir ? line.substr(ns) : "";

    auto emitLine = [&](const std::string& l) {
      out += l;
      out += '\n';
    };

    if (isDir) {
      // parse directive word
      std::string d;
      size_t i = 1;
      while (i < dir.size() && isspace((unsigned char)dir[i])) ++i;
      size_t s2 = i;
      while (i < dir.size() && isalpha((unsigned char)dir[i])) ++i;
      d = dir.substr(s2, i - s2);
      std::string rest;
      size_t r2 = i;
      while (r2 < dir.size() && isspace((unsigned char)dir[r2])) ++r2;
      rest = dir.substr(r2);

      if (d == "endif") { if (!condStack.empty()) condStack.pop_back(); continue; }
      if (d == "else") {
        if (!condStack.empty()) condStack.back() = !condStack.back();
        continue;
      }
      if (d == "ifdef" || d == "ifndef") {
        auto toks = tokenizeSimple(rest);
        bool has = !toks.empty() && st.macros.count(toks[0]);
        condStack.push_back(d == "ifdef" ? has : !has);
        continue;
      }
      if (!emitting()) continue;
      if (d == "define") {
        // name(params) body  or  name body
        auto toks = tokenizeSimple(rest);
        if (toks.empty()) {
          diag(Diag::Error, Code::Preproc, "#define requires a name", path, lineNo, 0);
          st.failed = true;
          continue;
        }
        // handle "(params)" attached to name — tokenizeSimple splits them apart
        Macro m;
        m.loc = {path, lineNo, 0};
        m.name = toks[0];
        size_t ti = 1;
        if (ti < toks.size() && toks[ti] == "(" &&
            rest.find(m.name + "(") != std::string::npos) {
          // function-like
          ++ti;
          while (ti < toks.size() && toks[ti] != ")") {
            if (toks[ti] != ",") m.params.push_back(toks[ti]);
            ++ti;
          }
          if (ti < toks.size()) ++ti;
        }
        std::string body;
        for (; ti < toks.size(); ++ti) {
          if (!body.empty()) body += " ";
          body += toks[ti];
        }
        m.body = body;
        st.macros[m.name] = m;
        continue;
      }
      if (d == "undef") {
        auto toks = tokenizeSimple(rest);
        if (!toks.empty()) st.macros.erase(toks[0]);
        continue;
      }
      if (d == "include") {
        std::string spec;
        if (!rest.empty() && (rest[0] == '"' || rest[0] == '<')) {
          char close = (rest[0] == '"') ? '"' : '>';
          size_t e = rest.find(close, 1);
          if (e != std::string::npos) spec = rest.substr(1, e - 1);
        }
        if (spec.empty()) {
          diag(Diag::Error, Code::Preproc, "malformed #include", path, lineNo, 0);
          st.failed = true;
          continue;
        }
        std::string resolved = resolveInclude(st, spec, path);
        if (resolved.empty()) {
          diag(Diag::Error, Code::Preproc, "include not found: " + spec, path, lineNo, 0);
          st.failed = true;
          continue;
        }
        if (st.includedOnce.count(resolved)) {
          emitLine("");  // keep line numbering stable
          snprintf(buf, sizeof buf, "#line %d \"%s\"\n", lineNo + 1, path.c_str());
          out += buf;
          continue;
        }
        processFile(st, resolved, out, sysDepth + 1);
        snprintf(buf, sizeof buf, "#line %d \"%s\"\n", lineNo + 1, path.c_str());
        out += buf;
        continue;
      }
      if (d == "link") {
        auto toks = tokenizeSimple(rest);
        for (auto& t : toks) if (t != ",") st.linkLibs->push_back(t);
        continue;
      }
      if (d == "pragma") {
        if (rest.rfind("once", 0) == 0) {
          st.includedOnce.insert(path);
          continue;
        }
        continue;  // unknown pragmas ignored
      }
      if (d == "line" || d.empty()) {
        emitLine(line);  // pass through #line directives
        continue;
      }
      diag(Diag::Warning, Code::PreprocDirective, "unknown directive #" + d, path, lineNo, 0);
      continue;
    }

    if (!emitting()) continue;

    // macro-expand non-directive lines
    SrcLoc loc{path, lineNo, 0};
    std::string expanded = applyMacros(st, line, loc, 0);
    emitLine(expanded);
  }

  if (!condStack.empty()) {
    diag(Diag::Error, Code::Preproc, "unterminated #ifdef", path, 0, 0);
    st.failed = true;
  }
  st.activeFile.erase(path);
}

bool preprocess(const std::string& path, std::string& out,
                std::vector<std::string>& linkLibs) {
  PpState st;
  st.linkLibs = &linkLibs;
  processFile(st, path, out);
  return !st.failed;
}

// overload declared in common.h with Program* — adapt
bool preprocessAdapt(const std::string& path, std::string& out,
                     std::vector<std::string>& linkLibs) {
  return preprocess(path, out, linkLibs);
}

uint64_t hashName(const std::string& s) {
  uint64_t h = 1469598103934665603ull;
  for (char c : s) { h ^= (uint8_t)c; h *= 1099511628211ull; }
  return h;
}

} // namespace freshet
