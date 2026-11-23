// Freshet Language Server (Stage E MVP, report §4).
//
// Architecture (report §4.1):
//
//   Editor / IDE
//       │  LSP over stdin/stdout (JSON-RPC 2.0 + Content-Length framing)
//       ▼
//   freshet-language-server
//       ├── LSP transport (this file)
//       ├── Workspace / document manager (this file)
//       │     ├── URI -> open document + version + unsaved text
//       │     └── cancellation / stale-result filtering
//       └── Freshet frontend API (src/session.cpp)
//             ├── lexer + source manager
//             ├── parser with error recovery
//             ├── semantic analysis and symbol index
//             ├── ownership / parfor / layout diagnostics
//             └── type/layout/optimisation metadata
//
// Implemented scope (Stage E, report §4.5):
//   - initialize / shutdown / exit lifecycle
//   - didOpen / didChange / didClose document sync (full text)
//   - textDocument/publishDiagnostics (push diagnostics)
//   - textDocument/hover (position-aware — inferred type / signature)
//   - textDocument/documentSymbol (function list)
//   - textDocument/definition (goto-definition)
//   - textDocument/references (find-references)
//   - textDocument/rename (edits exactly the resolved symbol's occurrences)
//   - textDocument/completion (scope-aware identifier completion: locals
//     and params of the enclosing fn, fns/globals/structs/enums, the
//     keyword and compiler-known builtin surfaces; `module::` paths offer
//     only that module's members)
//   - textDocument/inlayHint (parameter-name hints at resolved call sites,
//     inferred-type hints on un-annotated `let` bindings)
//   - textDocument/formatting + textDocument/rangeFormatting (the same
//     token-preserving formatter that powers `penstock fmt`, honouring
//     the editor's FormattingOptions — "Format Document" and "Format
//     Selection")
//   - textDocument/semanticTokens/full (reference-accurate colouring of
//     functions, function arguments, locals, struct fields, enum
//     variants, module/import names and type names on top of the TextMate
//     grammar)
//
// Everything is serialised through a single analysis mutex because the
// existing frontend still has process-global state.  Concurrent analysis
// across multiple documents will become safe once the CompileSession
// refactor (report §3.1) moves the remaining globals off the process.
//
// CRITICAL: ALL protocol output goes on stdout.  Diagnostics logs go to
// stderr.  A stray printf in the frontend would corrupt the protocol
// stream — the GlobalSwap in analyseDocument() prevents that by
// capturing g_diags locally.
#include "../session.h"
#include "../common.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>
#include <system_error>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

// Tiny JSON-RPC + LSP framing implementation.  We intentionally do NOT
// depend on a third-party JSON library: LSP messages are small and the
// surface is well-defined, so a hand-written writer + a forgiving
// reader is enough for the MVP and keeps the binary dependency-free.
namespace {

using freshet::Diag;
using freshet::DocumentAnalysis;
using freshet::Program;
using freshet::WorkspaceConfig;
using freshet::AnalysisOptions;

// ---------------- JSON value (subset) ------------------------------------
struct Json {
  enum class T { Null, Bool, Int, Str, Arr, Obj };
  T t = T::Null;
  bool b = false;
  long long i = 0;
  std::string s;
  std::vector<Json> arr;
  std::vector<std::pair<std::string, Json>> obj;

  static Json null() { return Json{}; }
  static Json boolean(bool v) { Json j; j.t = T::Bool; j.b = v; return j; }
  static Json num(long long v) { Json j; j.t = T::Int; j.i = v; return j; }
  static Json str(std::string v) { Json j; j.t = T::Str; j.s = std::move(v); return j; }
  static Json array(std::vector<Json> v) { Json j; j.t = T::Arr; j.arr = std::move(v); return j; }
  static Json object(std::vector<std::pair<std::string, Json>> v) {
    Json j; j.t = T::Obj; j.obj = std::move(v); return j;
  }

  const Json* find(const std::string& key) const {
    if (t != T::Obj) return nullptr;
    for (auto& kv : obj) if (kv.first == key) return &kv.second;
    return nullptr;
  }
};

// Minimal JSON serialiser.
std::string escapeStr(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 2);
  out += '"';
  for (char c : s) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if ((unsigned char)c < 0x20) {
          char buf[8]; snprintf(buf, sizeof buf, "\\u%04x", (unsigned)c);
          out += buf;
        } else out += c;
    }
  }
  out += '"';
  return out;
}
std::string serialize(const Json& j) {
  switch (j.t) {
    case Json::T::Null: return "null";
    case Json::T::Bool: return j.b ? "true" : "false";
    case Json::T::Int:  return std::to_string(j.i);
    case Json::T::Str:  return escapeStr(j.s);
    case Json::T::Arr: {
      std::string s = "[";
      for (size_t i = 0; i < j.arr.size(); ++i) {
        if (i) s += ",";
        s += serialize(j.arr[i]);
      }
      s += "]";
      return s;
    }
    case Json::T::Obj: {
      std::string s = "{";
      for (size_t i = 0; i < j.obj.size(); ++i) {
        if (i) s += ",";
        s += escapeStr(j.obj[i].first);
        s += ":";
        s += serialize(j.obj[i].second);
      }
      s += "}";
      return s;
    }
  }
  return "null";
}

// Minimal JSON parser.  Tolerant — the LSP spec allows clients to send
// arbitrary extra keys, and we ignore everything we don't need.
struct JsonParser {
  const std::string& s;
  size_t i = 0;
  JsonParser(const std::string& src) : s(src) {}

  void skipWs() {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
      ++i;
  }
  Json parse() {
    skipWs();
    if (i >= s.size()) return Json::null();
    char c = s[i];
    if (c == '{') return parseObj();
    if (c == '[') return parseArr();
    if (c == '"') { Json j = Json::str(parseStrRaw()); return j; }
    if (c == 't' || c == 'f') return parseBool();
    if (c == 'n') { i += 4; return Json::null(); }
    return parseNum();
  }
  Json parseObj() {
    ++i;  // {
    std::vector<std::pair<std::string, Json>> obj;
    skipWs();
    if (i < s.size() && s[i] == '}') { ++i; return Json::object(obj); }
    while (i < s.size()) {
      skipWs();
      std::string k = parseStrRaw();
      skipWs();
      if (i < s.size() && s[i] == ':') ++i;
      Json v = parse();
      obj.emplace_back(k, std::move(v));
      skipWs();
      if (i < s.size() && s[i] == ',') { ++i; continue; }
      if (i < s.size() && s[i] == '}') { ++i; break; }
      break;
    }
    return Json::object(obj);
  }
  Json parseArr() {
    ++i;  // [
    std::vector<Json> arr;
    skipWs();
    if (i < s.size() && s[i] == ']') { ++i; return Json::array(arr); }
    while (i < s.size()) {
      Json v = parse();
      arr.push_back(std::move(v));
      skipWs();
      if (i < s.size() && s[i] == ',') { ++i; continue; }
      if (i < s.size() && s[i] == ']') { ++i; break; }
      break;
    }
    return Json::array(arr);
  }
  std::string parseStrRaw() {
    std::string out;
    if (i >= s.size() || s[i] != '"') return out;
    ++i;
    while (i < s.size() && s[i] != '"') {
      if (s[i] == '\\' && i + 1 < s.size()) {
        ++i;
        char e = s[i++];
        switch (e) {
          case '"': out += '"'; break;
          case '\\': out += '\\'; break;
          case '/': out += '/'; break;
          case 'n': out += '\n'; break;
          case 't': out += '\t'; break;
          case 'r': out += '\r'; break;
          case 'b': out += '\b'; break;
          case 'f': out += '\f'; break;
          case 'u': {
            if (i + 4 <= s.size()) {
              int cp = std::stoi(s.substr(i, 4), nullptr, 16);
              i += 4;
              if (cp < 0x80) out += (char)cp;
              else if (cp < 0x800) {
                out += (char)(0xC0 | (cp >> 6));
                out += (char)(0x80 | (cp & 0x3F));
              } else {
                out += (char)(0xE0 | (cp >> 12));
                out += (char)(0x80 | ((cp >> 6) & 0x3F));
                out += (char)(0x80 | (cp & 0x3F));
              }
            }
            break;
          }
          default: out += e;
        }
      } else out += s[i++];
    }
    if (i < s.size()) ++i;  // closing quote
    return out;
  }
  Json parseBool() {
    if (i + 4 <= s.size() && s.compare(i, 4, "true") == 0) { i += 4; return Json::boolean(true); }
    if (i + 5 <= s.size() && s.compare(i, 5, "false") == 0) { i += 5; return Json::boolean(false); }
    return Json::null();
  }
  Json parseNum() {
    size_t start = i;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
    while (i < s.size() && (isdigit((unsigned char)s[i]) || s[i] == '.' ||
                            s[i] == 'e' || s[i] == 'E' || s[i] == '+' || s[i] == '-'))
      ++i;
    return Json::num(atoll(s.substr(start, i - start).c_str()));
  }
};

// ---------------- LSP transport (Content-Length framing) ----------------
// Reads one LSP message from stdin.  Returns false on EOF.
bool readMessage(std::string& outBody) {
  // Headers: "Content-Length: N\r\n" optionally followed by other
  // headers, terminated by "\r\n\r\n".
  std::string headers;
  char c;
  long contentLength = -1;
  while (true) {
    int ch = std::cin.get();
    if (ch == EOF) return false;
    c = (char)ch;
    headers += c;
    // detect end-of-headers: \r\n\r\n
    if (headers.size() >= 4 &&
        headers[headers.size()-4] == '\r' && headers[headers.size()-3] == '\n' &&
        headers[headers.size()-2] == '\r' && headers[headers.size()-1] == '\n') {
      break;
    }
    // parse "Content-Length: N" line on the fly
    if (c == '\n') {
      const char* p = strstr(headers.c_str() + (headers.size() > 1024 ? headers.size() - 1024 : 0),
                             "Content-Length:");
      if (p) contentLength = atol(p + strlen("Content-Length:"));
    }
  }
  if (contentLength < 0) {
    // Some clients might omit Content-Length; we cannot recover.
    return false;
  }
  outBody.resize((size_t)contentLength);
  size_t got = 0;
  while (got < (size_t)contentLength) {
    std::cin.read(&outBody[got], (std::streamsize)((size_t)contentLength - got));
    std::streamsize r = std::cin.gcount();
    if (r <= 0) break;
    got += (size_t)r;
  }
  return got == (size_t)contentLength;
}

void writeMessage(const std::string& body) {
  std::cout << "Content-Length: " << body.size() << "\r\n\r\n" << body << std::flush;
}

// ---------------- document manager ---------------------------------------
struct Doc {
  std::string uri;
  int version = 0;
  std::string text;
  // cached analysis (computed on open / change)
  DocumentAnalysis analysis;
  bool dirty = true;  // analysis not yet run for current text
};

struct Workspace {
  std::map<std::string, Doc> docs;  // uri -> doc
  std::mutex mu;                   // serialises analysis (frontend is not yet reentrant)
  WorkspaceConfig config;
  bool shutdownRequested = false;
  std::atomic<bool> exitRequested{false};
};

// uri -> filesystem path (strips the "file://" prefix).
std::string uriToPath(const std::string& uri) {
  if (uri.rfind("file://", 0) == 0) return uri.substr(7);
  return uri;
}

// ---------------- diagnostics -> LSP diagnostic conversion --------------
Json srcLocToRange(const freshet::SrcLoc& loc) {
  // LSP positions are 0-indexed; Freshet's source locations are 1-indexed.
  // Synthetic/prelude declarations can have an empty SrcLoc (line/column 0),
  // so never let an internal/synthetic location create a negative LSP range.
  const int startLine = loc.line > 0 ? loc.line : 1;
  const int startCol  = loc.col > 0 ? loc.col : 1;
  int endLine = loc.endLine > 0 ? loc.endLine : startLine;
  int endCol  = loc.endCol  > 0 ? loc.endCol  : startCol + 1;
  if (endLine < startLine) endLine = startLine;
  if (endLine == startLine && endCol <= startCol) endCol = startCol + 1;
  auto pos = [](int line, int col) -> Json {
    return Json::object({{"line", Json::num(line - 1)},
                         {"character", Json::num(col - 1)}});
  };
  return Json::object({{"start", pos(startLine, startCol)},
                       {"end",   pos(endLine, endCol)}});
}

Json diagToLsp(const Diag& d) {
  const char* sev = nullptr;
  switch (d.sev) {
    case Diag::Error:   sev = "Error";   break;
    case Diag::Warning: sev = "Warning"; break;
    case Diag::Note:    sev = "Information"; break;
  }
  int lspSev = 1;  // Error by default
  if (std::string(sev) == "Warning")      lspSev = 2;
  if (std::string(sev) == "Information")   lspSev = 3;
  freshet::SrcLoc loc;
  loc.file = d.file;
  loc.line = d.line;
  loc.col = d.col;
  loc.endLine = d.endLine;
  loc.endCol = d.endCol;
  Json range = srcLocToRange(loc);
  return Json::object({
    {"range",       range},
    {"severity",    Json::num(lspSev)},
    {"code",        Json::str(d.code)},
    {"source",      Json::str("penstock")},
    {"message",     Json::str(d.msg)},
  });
}

// ---------------- Semantic-token legend ----------------------------------
// Legend order defines the indices in the textDocument/semanticTokens
// `data` array (LSP 3.17).  Only standard token types are used, so every
// theme with semantic-token support (Dark+, Light+ et al) picks them up
// without extension-side colour tables.
static const char* SEMTOK_TYPES[] = {
  "namespace", "type", "struct", "enum", "interface", "function",
  "variable", "parameter", "property", "enumMember",
};
enum {
  ST_NAMESPACE = 0, ST_TYPE = 1, ST_STRUCT = 2, ST_ENUM = 3,
  ST_INTERFACE = 4, ST_FUNCTION = 5, ST_VARIABLE = 6, ST_PARAMETER = 7,
  ST_PROPERTY = 8, ST_ENUMMEMBER = 9,
};
static const char* SEMTOK_MODS[] = {
  "declaration", "readonly", "defaultLibrary", "static",
};
enum {
  SM_DECLARATION = 1 << 0, SM_READONLY = 1 << 1,
  SM_DEFAULTLIB = 1 << 2, SM_STATIC = 1 << 3,
};

static Json semtokStrArr(const char* const* items, size_t n) {
  Json a = Json::array({});
  for (size_t i = 0; i < n; ++i) a.arr.push_back(Json::str(items[i]));
  return a;
}

// ---------------- LSP message handlers -----------------------------------
void sendResponse(int id, const Json& result) {
  Json msg = Json::object({
    {"jsonrpc", Json::str("2.0")},
    {"id",      Json::num(id)},
    {"result",  result},
  });
  writeMessage(serialize(msg));
}
void sendNotification(const std::string& method, const Json& params) {
  Json msg = Json::object({
    {"jsonrpc", Json::str("2.0")},
    {"method",  Json::str(method)},
    {"params",  params},
  });
  writeMessage(serialize(msg));
}

// Diagnostics from imported modules are produced during the same semantic
// analysis as the open document.  They must NOT be published against the
// entry document's URI: LSP ranges are interpreted in the document named by
// the notification, so an imported module's line 20 would otherwise be drawn
// over line 20 of the entry file (often a comment or an unrelated expression).
// Compare canonical paths so relative imports, `..`, and symlinked workspace
// roots do not make diagnostics leak across files.
static bool sameSourceFile(const std::string& a, const std::string& b) {
  if (a.empty() || b.empty()) return false;
  namespace fs = std::filesystem;
  auto normalised = [](const std::string& value) {
    std::error_code ec;
    fs::path p = fs::absolute(fs::path(value), ec);
    if (ec) return fs::path(value).lexically_normal();
    ec.clear();
    fs::path canonical = fs::weakly_canonical(p, ec);
    // If canonicalisation fails (for example, a not-yet-created buffer),
    // compare the absolute lexical paths instead.  Neither path operation
    // throws because both use error_code overloads.
    return ec ? p.lexically_normal() : canonical;
  };
  return normalised(a) == normalised(b);
}

void publishDiagnostics(Workspace& ws, const std::string& uri) {
  std::lock_guard<std::mutex> g(ws.mu);
  auto it = ws.docs.find(uri);
  if (it == ws.docs.end()) return;
  Doc& d = it->second;
  const std::string documentPath = uriToPath(uri);
  if (d.dirty) {
    // Run analysis (synchronously for MVP).
    AnalysisOptions opts;
    opts.runStrictChecks = true;
    opts.runOwnershipAnalysis = true;
    opts.runLoopOptPasses = false;  // LSP shows the user's source, not the rewrite
    opts.emitLLVMIR = false;
    d.analysis = freshet::analyseDocument(ws.config, documentPath, d.text, opts);
    d.dirty = false;
  }
  Json arr = Json::array({});
  for (const auto& dg : d.analysis.diagnostics) {
    // `analyseDocument` intentionally analyses the entire import graph so
    // names and types resolve.  Diagnostics, however, are file-scoped in
    // LSP.  Imported files get their own diagnostics when they are opened
    // as documents; never reinterpret their coordinates in this buffer.
    if (!sameSourceFile(dg.file, documentPath)) continue;
    arr.arr.push_back(diagToLsp(dg));
  }
  sendNotification("textDocument/publishDiagnostics",
                   Json::object({{"uri", Json::str(uri)}, {"diagnostics", arr}}));
}

Json serverCapabilities() {
  // Advertise only features implemented by this server.  LSP 3.17 uses
  // UTF-16 code units by default; state that explicitly because editor
  // clients (including vscode-languageclient) inspect this field.
  return Json::object({
    {"positionEncoding", Json::str("utf-16")},
    {"textDocumentSync", Json::num(1)},  // 1 = Full text sync
    {"hoverProvider",   Json::boolean(true)},
    {"documentSymbolProvider", Json::boolean(true)},
    {"definitionProvider",     Json::boolean(true)},   // Phase 4 (§10.1)
    {"referencesProvider",     Json::boolean(true)},   // Phase 4 (§10.1)
    {"renameProvider",         Json::boolean(true)},   // Phase 4 (§10.1)
    {"completionProvider", Json::object({
      // ":" covers `name:` type positions and `mod::` member paths; "."
      // covers field access.  Word-character typing triggers client-side
      // completion without any trigger character, so these only widen the
      // surface.
      {"triggerCharacters", Json::array({Json::str(":"), Json::str(".")})},
      {"resolveProvider", Json::boolean(false)},
    })},
    {"inlayHintProvider", Json::object({
      {"resolveProvider", Json::boolean(false)},
    })},
    // Document + range formatting ("Format Document" / "Format
    // Selection"): the token-preserving `penstock fmt` engine applied to
    // the open buffer, honouring the client's FormattingOptions.
    {"documentFormattingProvider", Json::boolean(true)},
    {"documentRangeFormattingProvider", Json::boolean(true)},
    // Semantic tokens: reference-accurate colouring of types, functions,
    // function arguments, locals, fields, enum members and module /
    // import names over the TextMate grammar.
    {"semanticTokensProvider", Json::object({
      {"legend", Json::object({
        {"tokenTypes",     semtokStrArr(SEMTOK_TYPES, sizeof SEMTOK_TYPES / sizeof SEMTOK_TYPES[0])},
        {"tokenModifiers", semtokStrArr(SEMTOK_MODS,  sizeof SEMTOK_MODS  / sizeof SEMTOK_MODS[0])},
      })},
      {"full",  Json::boolean(true)},
      {"range", Json::boolean(false)},
    })},
    // Code actions are not implemented yet.  Do not advertise
    // placeholders for them.
  });
}

void handleInitialize(Workspace& ws, int id, const Json&) {
  (void)ws;
  // LSP InitializeResult is an envelope: capabilities must be nested under
  // `capabilities`, while serverInfo is a sibling at the top level.  Returning
  // ServerCapabilities directly makes clients read an undefined
  // result.capabilities and fail during initialisation.
  Json result = Json::object({
    {"capabilities", serverCapabilities()},
    {"serverInfo", Json::object({
      {"name", Json::str("freshet-language-server")},
      {"version", Json::str(FRESHET_VERSION)},
    })},
  });
  sendResponse(id, result);
}
void handleInitialized(Workspace& ws, const Json&) {
  (void)ws;  // nothing to do; documents will arrive via didOpen
}
void handleShutdown(Workspace& ws, int id) {
  ws.shutdownRequested = true;
  sendResponse(id, Json::null());
}
void handleExit(Workspace& ws) {
  ws.exitRequested = true;
}

void handleDidOpen(Workspace& ws, const Json& params) {
  const Json* td = params.find("textDocument");
  if (!td) return;
  const Json* uri = td->find("uri");
  const Json* text = td->find("text");
  const Json* ver = td->find("version");
  if (!uri || !text) return;
  Doc d;
  d.uri = uri->s;
  d.text = text->s;
  d.version = ver ? (int)ver->i : 0;
  d.dirty = true;
  {
    std::lock_guard<std::mutex> g(ws.mu);
    ws.docs[d.uri] = std::move(d);
  }
  publishDiagnostics(ws, uri->s);
}
void handleDidChange(Workspace& ws, const Json& params) {
  const Json* td = params.find("textDocument");
  const Json* changes = params.find("contentChanges");
  if (!td || !changes) return;
  const Json* uri = td->find("uri");
  const Json* ver = td->find("version");
  if (!uri) return;
  std::string uriCopy = uri->s;
  {
    // Mutate the document under the lock, then drop the lock before
    // publishDiagnostics (which re-acquires it to run analysis).
    std::lock_guard<std::mutex> g(ws.mu);
    auto it = ws.docs.find(uriCopy);
    if (it == ws.docs.end()) return;
    // Full sync only (we advertised textDocumentSync=1).
    if (!changes->arr.empty()) {
      const Json* first = &changes->arr[0];
      const Json* t = first->find("text");
      if (t) {
        it->second.text = t->s;
        it->second.version = ver ? (int)ver->i : it->second.version + 1;
        it->second.dirty = true;
      }
    }
  }  // lock released here.
  publishDiagnostics(ws, uriCopy);
}
void handleDidClose(Workspace& ws, const Json& params) {
  const Json* td = params.find("textDocument");
  if (!td) return;
  const Json* uri = td->find("uri");
  if (!uri) return;
  std::lock_guard<std::mutex> g(ws.mu);
  ws.docs.erase(uri->s);
  // Clear diagnostics for the closed document.
  sendNotification("textDocument/publishDiagnostics",
                   Json::object({{"uri", Json::str(uri->s)}, {"diagnostics", Json::array({})}}));
}

// Phase 4 navigation (report §10.1): the resolved symbol under the cursor
struct SymbolHit {
  enum Kind { LOCAL, PARAM, FN, GLOBAL, STRUCT } kind = LOCAL;
  void* decl = nullptr;
  std::string name;
  freshet::SrcLoc declLoc;
  std::string detail;        // hover text
  bool found = false;
};

// Phase 4 navigation helpers (defined below, after the hover handler)
freshet::DocumentAnalysis* ensureAnalysed(Workspace& ws, const std::string& uri);
size_t lspPosToOffset(const std::string& text, int line, int character);
SymbolHit resolveSymbolAt(freshet::Program* prog, size_t offset,
                          std::vector<freshet::SrcLoc>& refsOut);
void handleDefinition(Workspace& ws, int id, const Json& params);
void handleReferences(Workspace& ws, int id, const Json& params);
void handleRename(Workspace& ws, int id, const Json& params);

void handleHover(Workspace& ws, int id, const Json& params) {
  // Phase 4 (report §10.1): position-aware hover — resolves the symbol
  // under the cursor through the sema-annotated AST and shows its
  // inferred type / function signature
  const Json* td = params.find("textDocument");
  const Json* pos = params.find("position");
  if (!td || !pos) { sendResponse(id, Json::null()); return; }
  const Json* uri = td->find("uri");
  if (!uri) { sendResponse(id, Json::null()); return; }
  std::lock_guard<std::mutex> g(ws.mu);
  auto* an = ensureAnalysed(ws, uri->s);
  if (!an || !an->program) { sendResponse(id, Json::null()); return; }
  size_t off = lspPosToOffset(ws.docs[uri->s].text,
                              (int)pos->find("line")->i,
                              (int)pos->find("character")->i);
  std::vector<freshet::SrcLoc> refs;
  SymbolHit hit = resolveSymbolAt(an->program.get(), off, refs);
  if (!hit.found) { sendResponse(id, Json::null()); return; }
  const char* kindName[] = {"let", "param", "fn", "global", "struct"};
  std::string md = "```freshet\n" + hit.detail + "\n```\n**" +
                   kindName[hit.kind] + "** `" + hit.name + "` — " +
                   std::to_string(refs.size()) + " reference(s)";
  Json contents = Json::object({{"kind", Json::str("markdown")},
                                {"value", Json::str(md)}});
  sendResponse(id, Json::object({{"contents", contents}}));
}

void handleDocumentSymbol(Workspace& ws, int id, const Json& params) {
  const Json* td = params.find("textDocument");
  if (!td) { sendResponse(id, Json::array({})); return; }
  const Json* uri = td->find("uri");
  if (!uri) { sendResponse(id, Json::array({})); return; }
  std::lock_guard<std::mutex> g(ws.mu);
  auto it = ws.docs.find(uri->s);
  if (it == ws.docs.end()) { sendResponse(id, Json::array({})); return; }
  // (MVP) a stale analysis is returned as-is here; the next didChange
  // refreshes it.
  Json arr = Json::array({});
  if (it->second.analysis.program) {
    // `analyseDocument()` merges imported modules into one Program, and
    // semantic analysis may append prelude declarations (for example the
    // builtin String struct). Those are not symbols declared in this editor
    // document. In particular, prelude declarations can have SrcLoc{};
    // converting line 0 to an LSP position used to emit line -1 and crash
    // vscode-languageclient while it converted DocumentSymbols.
    const std::string documentPath = uriToPath(uri->s);
    for (auto& e : it->second.analysis.symbols.entries) {
      if (e.loc.file != documentPath || e.loc.line < 1 || e.loc.col < 1)
        continue;
      Json range = srcLocToRange(e.loc);
      arr.arr.push_back(Json::object({
        {"name",          Json::str(e.name)},
        {"kind",          Json::num(e.kind == freshet::SymbolIndex::Entry::FN ? 12 :
                                       e.kind == freshet::SymbolIndex::Entry::STRUCT ? 23 : 13)},
        {"range",         range},
        {"selectionRange", range},
        {"detail",        Json::str(e.detail)},
      }));
    }
  }
  sendResponse(id, arr);
}


// ---------------- Phase 4 navigation (report §10.1) -----------------------
// Position-aware hover, goto-definition, find-references and rename over
// the analysed AST.  Symbol resolution reuses the SEMA results attached
// to every expression (sym / callee / globalRef / fnValue), so the
// navigation is type-accurate, not textual.

// LSP (line, UTF-16 character) -> byte offset in the document text
size_t lspPosToOffset(const std::string& text, int line, int character) {
  size_t off = 0;
  int l = 0;
  while (off < text.size() && l < line) {
    if (text[off] == '\n') ++l;
    ++off;
  }
  // advance `character` UTF-16 code units within the line
  int units = 0;
  while (off < text.size() && text[off] != '\n' && units < character) {
    unsigned char c = (unsigned char)text[off];
    // UTF-16 units per code point: 1 for <0x10000 (1-3 byte UTF-8),
    // 2 for 4-byte UTF-8 (surrogate pair)
    units += (c & 0xF0) == 0xF0 ? 2 : 1;
    if ((c & 0x80) == 0) ++off;                       // ASCII
    else if ((c & 0xE0) == 0xC0) off += 2;
    else if ((c & 0xF0) == 0xE0) off += 3;
    else off += 4;
  }
  return off;
}


struct NavWalker {
  // mode A: find the symbol under `offset` (narrowest containing span)
  size_t offset = 0;
  SymbolHit hit;
  size_t bestLen = SIZE_MAX;
  // mode B: collect every identifier occurrence of (kind, decl)
  SymbolHit::Kind wantKind = SymbolHit::LOCAL;
  void* wantDecl = nullptr;
  std::vector<freshet::SrcLoc> refs;

  // The semantic AST is normally a tree, but transformations and shared
  // subexpressions can make it a graph.  These sets both avoid visiting a
  // shared node more than once and prevent a malformed cycle from taking
  // down the language server.  Iterative worklists also avoid consuming the
  // C++ call stack for very deeply nested expressions / blocks.
  std::unordered_set<const freshet::Expr*> seenExprs;
  std::unordered_set<const freshet::Stmt*> seenStmts;

  bool spanContains(freshet::SrcLoc loc) const {
    // single-point spans (endOffset unset) match a cursor at the start
    uint64_t end = loc.endOffset > loc.startOffset ? loc.endOffset
                                                   : loc.startOffset + 1;
    return offset >= loc.startOffset && offset < end;
  }

  void consider(freshet::SrcLoc loc, SymbolHit::Kind k, void* decl,
                const std::string& name, const std::string& detail,
                freshet::SrcLoc declLoc) {
    if (wantDecl) {
      if (k == wantKind && decl == wantDecl) refs.push_back(loc);
      return;
    }
    if (!spanContains(loc)) return;
    uint64_t end = loc.endOffset > loc.startOffset ? loc.endOffset
                                                   : loc.startOffset + 1;
    size_t len = (size_t)(end - loc.startOffset);
    if (len < bestLen) {
      bestLen = len;
      hit.kind = k;
      hit.decl = decl;
      hit.name = name;
      hit.detail = detail;
      hit.declLoc = declLoc;
      hit.found = true;
    }
  }

  void expr(freshet::Expr* root, freshet::FnSema* /*fs*/) {
    if (!root) return;
    std::vector<freshet::Expr*> pending;
    pending.push_back(root);
    while (!pending.empty()) {
      freshet::Expr* e = pending.back();
      pending.pop_back();
      if (!e || !seenExprs.insert(e).second) continue;

      switch (e->k) {
        case freshet::Expr::IDENT:
          if (e->sym)
            consider(e->loc, e->sym->k == freshet::Sym::PARAM ? SymbolHit::PARAM
                                                              : SymbolHit::LOCAL,
                     e->sym, e->name,
                     typeToString(e->sym->ty) +
                         (e->sym->mut ? " (mut)" : ""),
                     e->sym->declLoc);
          if (e->globalRef)
            consider(e->loc, SymbolHit::GLOBAL, e->globalRef, e->name,
                     (e->globalRef->mut ? "mut global: " : "global: ") +
                         typeToString(e->globalRef->ty),
                     e->globalRef->loc);
          if (e->fnValue)
            consider(e->loc, SymbolHit::FN, e->fnValue, e->name,
                     fnSignature(e->fnValue), e->fnValue->loc);
          break;
        case freshet::Expr::CALL:
          if (e->callee)
            consider(e->loc, SymbolHit::FN, e->callee, e->name,
                     fnSignature(e->callee), e->callee->loc);
          if (e->fnPtrSym)
            consider(e->loc,
                     e->fnPtrSym->k == freshet::Sym::PARAM ? SymbolHit::PARAM
                                                            : SymbolHit::LOCAL,
                     e->fnPtrSym, e->name,
                     typeToString(e->fnPtrSym->ty), e->fnPtrSym->declLoc);
          if (e->globalRef)
            consider(e->loc, SymbolHit::GLOBAL, e->globalRef, e->name,
                     "global: " + typeToString(e->globalRef->ty),
                     e->globalRef->loc);
          break;
        case freshet::Expr::SLIT:
          // In incomplete/error-recovery ASTs a struct placeholder may
          // have a STRUCT kind but no bound StructDef.  Do not dereference
          // it while answering an editor navigation request.
          if (e->ty && e->ty->k == freshet::Type::STRUCT && e->ty->sdef) {
            freshet::StructDef* sd = e->ty->sdef;
            consider(e->loc, SymbolHit::STRUCT, sd, e->name,
                     "struct " + e->name + " { " +
                         std::to_string(sd->fields.size()) + " fields }",
                     sd->loc);
          }
          break;
        default:
          break;
      }

      // Push children in reverse so popping yields the original depth-first
      // visitation order: args, lhs, rhs, then base.
      if (e->base) pending.push_back(e->base);
      if (e->rhs) pending.push_back(e->rhs);
      if (e->lhs) pending.push_back(e->lhs);
      for (auto it = e->args.rbegin(); it != e->args.rend(); ++it)
        if (*it) pending.push_back(*it);
    }
  }

  void stmt(freshet::Stmt* root, freshet::FnSema* fs) {
    if (!root) return;
    std::vector<freshet::Stmt*> pending;
    pending.push_back(root);
    while (!pending.empty()) {
      freshet::Stmt* s = pending.back();
      pending.pop_back();
      if (!s || !seenStmts.insert(s).second) continue;

      switch (s->k) {
        case freshet::Stmt::LET:
          // The declaration site itself is a reference for rename.
          if (wantDecl && s->init && fs) {
            for (auto& sym : fs->allSyms) {
              if (sym.declLoc.startOffset == s->loc.startOffset &&
                  (void*)&sym == wantDecl) {
                refs.push_back(s->loc);
                break;
              }
            }
          }
          expr(s->init, fs);
          break;
        case freshet::Stmt::ASSIGN:
          expr(s->lhs, fs); expr(s->rhs, fs); break;
        case freshet::Stmt::RETURN: expr(s->retVal, fs); break;
        case freshet::Stmt::DEFER: expr(s->deferExpr, fs); break;
        case freshet::Stmt::EXPR: expr(s->expr, fs); break;
        case freshet::Stmt::IF:
          expr(s->cond, fs);
          if (s->elseB) pending.push_back(s->elseB);
          if (s->thenB) pending.push_back(s->thenB);
          break;
        case freshet::Stmt::WHILE:
          expr(s->cond, fs);
          if (s->body) pending.push_back(s->body);
          break;
        case freshet::Stmt::FOR:
          if (s->forRange) {
            expr(s->rangeLo, fs); expr(s->rangeHi, fs);
          } else {
            expr(s->forInit, fs); expr(s->forCond, fs); expr(s->forStep, fs);
          }
          if (s->body) pending.push_back(s->body);
          break;
        case freshet::Stmt::PARFOR:
          expr(s->plo, fs); expr(s->phi, fs);
          for (auto& c : s->clauses) expr(c.e, fs);
          if (s->body) pending.push_back(s->body);
          break;
        case freshet::Stmt::MATCH:
          expr(s->matchOn, fs);
          for (auto it = s->matchArms.rbegin(); it != s->matchArms.rend(); ++it)
            if (it->body) pending.push_back(it->body);
          break;
        case freshet::Stmt::BLOCK:
          for (auto it = s->stmts.rbegin(); it != s->stmts.rend(); ++it)
            if (*it) pending.push_back(*it);
          break;
        default:
          break;
      }
    }
  }

  static std::string fnSignature(freshet::FnDecl* f) {
    if (!f) return "fn <invalid>";
    std::string sig = "fn " + f->name + "(";
    for (size_t i = 0; i < f->params.size(); ++i) {
      if (i) sig += ", ";
      sig += f->params[i].name + ": " + typeToString(f->params[i].ty);
    }
    sig += ") -> " + typeToString(f->ret);
    return sig;
  }
};
// resolve the symbol at a byte offset; also collects ALL references to it
SymbolHit resolveSymbolAt(Program* prog, size_t offset,
                          std::vector<freshet::SrcLoc>& refsOut) {
  if (!prog) return {};
  NavWalker finder;
  finder.offset = offset;
  for (auto* f : prog->fns) {
    if (!f || f->imported || f->isGeneric || !f->body) continue;
    freshet::FnSema* fs = freshet::semaForFn(f);
    finder.stmt(f->body, fs);
  }
  if (!finder.hit.found) return finder.hit;
  // collect every identifier occurrence of the resolved decl
  NavWalker collector;
  collector.wantKind = finder.hit.kind;
  collector.wantDecl = finder.hit.decl;
  for (auto* f : prog->fns) {
    if (!f || f->imported || f->isGeneric || !f->body) continue;
    freshet::FnSema* fs = freshet::semaForFn(f);
    collector.stmt(f->body, fs);
  }
  // declaration site (rename must rewrite it too)
  if (finder.hit.declLoc.startOffset || finder.hit.declLoc.line)
    collector.refs.push_back(finder.hit.declLoc);
  refsOut = collector.refs;
  return finder.hit;
}

// force a fresh analysis when dirty, then hand the program to the caller
freshet::DocumentAnalysis* ensureAnalysed(Workspace& ws, const std::string& uri) {
  auto it = ws.docs.find(uri);
  if (it == ws.docs.end()) return nullptr;
  Doc& d = it->second;
  if (d.dirty) {
    freshet::AnalysisOptions opts;
    d.analysis = freshet::analyseDocument(ws.config, uriToPath(uri), d.text, opts);
    d.dirty = false;
  }
  return &d.analysis;
}

void handleDefinition(Workspace& ws, int id, const Json& params) {
  const Json* td = params.find("textDocument");
  const Json* pos = params.find("position");
  if (!td || !pos) { sendResponse(id, Json::null()); return; }
  const Json* uri = td->find("uri");
  if (!uri) { sendResponse(id, Json::null()); return; }
  std::lock_guard<std::mutex> g(ws.mu);
  auto* an = ensureAnalysed(ws, uri->s);
  if (!an || !an->program) { sendResponse(id, Json::null()); return; }
  size_t off = lspPosToOffset(ws.docs[uri->s].text,
                              (int)pos->find("line")->i,
                              (int)pos->find("character")->i);
  std::vector<freshet::SrcLoc> refs;
  SymbolHit hit = resolveSymbolAt(an->program.get(), off, refs);
  if (!hit.found || (!hit.declLoc.line && !hit.declLoc.startOffset)) {
    sendResponse(id, Json::null());
    return;
  }
  sendResponse(id, Json::object({
    {"uri",   Json::str(uri->s)},
    {"range", srcLocToRange(hit.declLoc)},
  }));
}

void handleReferences(Workspace& ws, int id, const Json& params) {
  const Json* td = params.find("textDocument");
  const Json* pos = params.find("position");
  const Json* inc  = params.find("includeDeclaration");
  if (!td || !pos) { sendResponse(id, Json::array({})); return; }
  const Json* uri = td->find("uri");
  if (!uri) { sendResponse(id, Json::array({})); return; }
  std::lock_guard<std::mutex> g(ws.mu);
  auto* an = ensureAnalysed(ws, uri->s);
  if (!an || !an->program) { sendResponse(id, Json::array({})); return; }
  size_t off = lspPosToOffset(ws.docs[uri->s].text,
                              (int)pos->find("line")->i,
                              (int)pos->find("character")->i);
  std::vector<freshet::SrcLoc> refs;
  SymbolHit hit = resolveSymbolAt(an->program.get(), off, refs);
  Json arr = Json::array({});
  bool includeDecl = inc && inc->b;
  for (auto& r : refs) {
    if (!includeDecl && r.startOffset == hit.declLoc.startOffset &&
        r.line == hit.declLoc.line)
      continue;
    arr.arr.push_back(Json::object({
      {"uri",   Json::str(uri->s)},
      {"range", srcLocToRange(r)},
    }));
  }
  sendResponse(id, arr);
}

void handleRename(Workspace& ws, int id, const Json& params) {
  const Json* td = params.find("textDocument");
  const Json* pos = params.find("position");
  const Json* newName = params.find("newName");
  if (!td || !pos || !newName) {
    sendResponse(id, Json::object({{"error", Json::str("missing params")}}));
    return;
  }
  const Json* uri = td->find("uri");
  if (!uri) { sendResponse(id, Json::null()); return; }
  std::lock_guard<std::mutex> g(ws.mu);
  auto* an = ensureAnalysed(ws, uri->s);
  if (!an || !an->program) { sendResponse(id, Json::null()); return; }
  size_t off = lspPosToOffset(ws.docs[uri->s].text,
                              (int)pos->find("line")->i,
                              (int)pos->find("character")->i);
  std::vector<freshet::SrcLoc> refs;
  SymbolHit hit = resolveSymbolAt(an->program.get(), off, refs);
  if (!hit.found) { sendResponse(id, Json::null()); return; }
  Json edits = Json::array({});
  for (auto& r : refs) {
    edits.arr.push_back(Json::object({
      {"range", srcLocToRange(r)},
      {"newText", Json::str(newName->s)},
    }));
  }
  Json changes = Json::object({});
  changes.obj.push_back({uri->s, edits});
  sendResponse(id, Json::object({{"changes", changes}}));
}

// ---------------- Completion + inlay hints -------------------------------
// textDocument/completion and textDocument/inlayHint close the tooling
// checklist surface (registry 10.04.01): scope-aware identifier completion
// and the two rust-analyzer-style inlay hint families.

// byte offset -> LSP (line, UTF-16 character) position; the inverse of
// lspPosToOffset above.  Textbook editors only ever position hints inside
// single-Byte ASCII identifiers here, but the conversion is general.
static Json offsetToLspPos(const std::string& text, size_t off) {
  off = off > text.size() ? text.size() : off;
  int line = 0, units = 0;
  size_t i = 0;
  while (i < off) {
    if (text[i] == '\n') { ++line; units = 0; ++i; continue; }
    unsigned char c = (unsigned char)text[i];
    units += (c & 0xF0) == 0xF0 ? 2 : 1;   // surrogate pair for 4-Byte UTF-8
    if ((c & 0x80) == 0) i += 1;
    else if ((c & 0xE0) == 0xC0) i += 2;
    else if ((c & 0xF0) == 0xE0) i += 3;
    else i += 4;
  }
  return Json::object({{"line", Json::num(line)},
                       {"character", Json::num(units)}});
}

static bool hintSpanContains(freshet::SrcLoc loc, size_t off) {
  uint64_t end = loc.endOffset > loc.startOffset ? loc.endOffset
                                                 : loc.startOffset + 1;
  return off >= loc.startOffset && off < end;
}

// the identifier being completed: chars immediately left of the cursor.
// Returns the prefix and where the word starts.
static void wordPrefixAt(const std::string& text, size_t off,
                         std::string& prefix, size_t& wordStart) {
  size_t i = off;
  while (i > 0) {
    char c = text[i - 1];
    if (isalnum((unsigned char)c) || c == '_') --i; else break;
  }
  wordStart = i;
  prefix = text.substr(i, off - i);
}

// `module::|` — completing a qualified path.  Returns the module name and
// true when the text just left of the word is the `::` separator.
static bool qualifiedBefore(const std::string& text, size_t wordStart,
                            std::string& module) {
  if (wordStart < 2) return false;
  if (text.compare(wordStart - 2, 2, "::") != 0) return false;
  size_t i = wordStart - 2;
  while (i > 0) {
    char c = text[i - 1];
    if (isalnum((unsigned char)c) || c == '_') --i; else break;
  }
  module = text.substr(i, wordStart - 2 - i);
  return !module.empty();
}

// push the direct child statements of `s` (BLOCK / IF / WHILE / FOR /
// PARFOR / MATCH arms).  Mirrors the traversal shape of NavWalker::stmt.
static void pushStmtChildren(freshet::Stmt* s,
                             std::vector<freshet::Stmt*>& out) {
  switch (s->k) {
    case freshet::Stmt::IF:
      if (s->thenB) out.push_back(s->thenB);
      if (s->elseB) out.push_back(s->elseB);
      break;
    case freshet::Stmt::WHILE:
    case freshet::Stmt::FOR:
    case freshet::Stmt::PARFOR:
      if (s->body) out.push_back(s->body);
      break;
    case freshet::Stmt::MATCH:
      for (auto& a : s->matchArms)
        if (a.body) out.push_back(a.body);
      break;
    case freshet::Stmt::BLOCK:
      for (auto* t : s->stmts)
        if (t) out.push_back(t);
      break;
    default:
      break;
  }
}

// the fn whose (non-generic, non-imported) body encloses the cursor —
// the scope whose locals and params completion offers.
static freshet::FnDecl* fnEnclosing(Program* prog, size_t off) {
  if (!prog) return nullptr;
  for (auto* f : prog->fns) {
    if (!f || f->imported || f->isGeneric || !f->body) continue;
    std::vector<freshet::Stmt*> pending{f->body};
    std::unordered_set<const freshet::Stmt*> seen;
    while (!pending.empty()) {
      freshet::Stmt* s = pending.back();
      pending.pop_back();
      if (!s || !seen.insert(s).second) continue;
      if (hintSpanContains(s->loc, off)) return f;
      std::vector<freshet::Stmt*> kids;
      pushStmtChildren(s, kids);
      for (auto it = kids.rbegin(); it != kids.rend(); ++it)
        pending.push_back(*it);
    }
  }
  return nullptr;
}

// Keyword list mirrors src/lexer.cpp KEYWORDS — advisory editor data;
// a stale entry only costs a bogus suggestion, so duplication is safe.
static const char* COMPLETION_KEYWORDS[] = {
  "fn","let","mut","return","defer","if","else","while","for","parfor","in",
  "struct","align","true","false","as","export","extern","give","by","step",
  "reduction","threads","chunk","affinity","collapse","close","spread",
  "soa","sizeof","alignof","nil","break","continue","enum","match","unsafe",
  "str","module","import","pub","trait","impl","Self","const",
};

// Compiler-known builtins (sema_expr.cpp typeBuiltin): the completion
// surface most useful while editing.  Advisory only — signatures shown as
// documentation anchors, not checked here.
static const struct { const char* name; const char* sig; } COMPLETION_BUILTINS[] = {
  {"malloc",       "malloc(T, n) -> *T"},
  {"stack_alloc",  "stack_alloc(T, n) -> *T"},
  {"free",         "free(p)"},
  {"give",         "give(p)"},
  {"memcpy",       "memcpy(dst, src, n)"},
  {"memset",       "memset(dst, v, n)"},
  {"print",        "print(s: str)"},
  {"println",      "println()"},
  {"print_i64",    "print_i64(x)"},
  {"print_u64",    "print_u64(x)"},
  {"print_f64",    "print_f64(x, digits)"},
  {"soa_new",      "soa_new(S, n) -> soa(S)"},
  {"soa_free",     "soa_free(p)"},
  {"soa_len",      "soa_len(p) -> u64"},
  {"assert",       "assert(cond)"},
  {"const_assert", "const_assert(cond)"},
  {"span",         "span(arr) -> Span<T>"},
  {"len",          "len(view) -> i64"},
  {"str_eq",       "str_eq(a: str, b: str) -> bool"},
  {"str_cat",      "str_cat(a: str, b: str) -> String"},
  {"str_of",       "str_of(&s: String) -> str"},
  {"str_to_cstr",  "str_to_cstr(s: str) -> *u8"},
  {"string_new",   "string_new() -> String"},
  {"string_from",  "string_from(s: str) -> String"},
  {"string_append","string_append(&mut s: String, a: str)"},
  {"string_clone", "string_clone(&s: String) -> String"},
  {"string_clear", "string_clear(&mut s: String)"},
  {"fmt_i64",      "fmt_i64(x) -> String"},
  {"fmt_f64",      "fmt_f64(x, digits) -> String"},
  {"fmt",          "fmt(parts: str...) -> String"},
  {"parse_i64",    "parse_i64(s: str) -> Result<i64, ParseError>"},
  {"parse_u64",    "parse_u64(s: str) -> Result<u64, ParseError>"},
  {"parse_f64",    "parse_f64(s: str) -> Result<f64, ParseError>"},
  {"vec_new",      "vec_new(T, cap) -> Vec<T>"},
  {"vec_from_span","vec_from_span(v: Span<T>) -> Vec<T>"},
  {"vec_from_array","vec_from_array(a: [T; N]) -> Vec<T>"},
  {"vec_clone",    "vec_clone(&v: Vec<T>) -> Vec<T>"},
  {"vec_slice",    "vec_slice(&v: Vec<T>, lo, hi) -> Span<T>"},
  {"vec_push",     "vec_push(&mut v: Vec<T>, x: T)"},
  {"vec_pop",      "vec_pop(&mut v: Vec<T>) -> Option<T>"},
  {"vec_get",      "vec_get(&v: Vec<T>, i) -> Option<T>"},
  {"vec_remove",   "vec_remove(&mut v: Vec<T>, i)"},
  {"vec_insert",   "vec_insert(&mut v: Vec<T>, i, x: T)"},
  {"vec_reserve",  "vec_reserve(&mut v: Vec<T>, cap)"},
  {"vec_resize",   "vec_resize(&mut v: Vec<T>, n)"},
  {"vec_truncate", "vec_truncate(&mut v: Vec<T>, n)"},
  {"vec_clear",    "vec_clear(&mut v: Vec<T>)"},
  {"vec_free",     "vec_free(&mut v: Vec<T>)"},
  {"vec_len",      "vec_len(&v: Vec<T>) -> u64"},
  {"vec_capacity", "vec_capacity(&v: Vec<T>) -> u64"},
  {"vec_is_empty", "vec_is_empty(&v: Vec<T>) -> bool"},
  {"checked_add",  "checked_add(a, b) -> Option<T>"},
  {"checked_sub",  "checked_sub(a, b) -> Option<T>"},
  {"checked_mul",  "checked_mul(a, b) -> Option<T>"},
  {"checked_div",  "checked_div(a, b) -> Option<T>"},
  {"checked_rem",  "checked_rem(a, b) -> Option<T>"},
  {"saturating_add","saturating_add(a, b)"},
  {"saturating_sub","saturating_sub(a, b)"},
  {"saturating_mul","saturating_mul(a, b)"},
  {"wrapping_add", "wrapping_add(a, b)"},
  {"wrapping_sub", "wrapping_sub(a, b)"},
  {"wrapping_mul", "wrapping_mul(a, b)"},
  {"checked_cast", "checked_cast(x) -> Option<U>"},
  {"saturating_cast","saturating_cast(x) -> U"},
  {"int_min",      "int_min(x) -> T"},
  {"int_max",      "int_max(x) -> T"},
  {"imin",         "imin(a, b)"},
  {"imax",         "imax(a, b)"},
  {"abs",          "abs(x)"},
  {"gcd",          "gcd(a, b)"},
  {"clamp",        "clamp(x, lo, hi)"},
  {"sqrt",         "sqrt(x: f64) -> f64"},
  {"sin",          "sin(x: f64) -> f64"},
  {"cos",          "cos(x: f64) -> f64"},
  {"tan",          "tan(x: f64) -> f64"},
  {"exp",          "exp(x: f64) -> f64"},
  {"log",          "log(x: f64) -> f64"},
  {"log2",         "log2(x: f64) -> f64"},
  {"log10",        "log10(x: f64) -> f64"},
  {"fabs",         "fabs(x: f64) -> f64"},
  {"floor",        "floor(x: f64) -> f64"},
  {"ceil",         "ceil(x: f64) -> f64"},
  {"round",        "round(x: f64) -> f64"},
  {"pow",          "pow(x, y) -> f64"},
  {"fmin",         "fmin(x, y) -> f64"},
  {"fmax",         "fmax(x, y) -> f64"},
  {"bswap16",      "bswap16(x: u16) -> u16"},
  {"bswap32",      "bswap32(x: u32) -> u32"},
  {"bswap64",      "bswap64(x: u64) -> u64"},
  {"popcount",     "popcount(x: u64) -> u64"},
  {"now_ns",       "now_ns() -> i64"},
  {"now_ms",       "now_ms() -> i64"},
  {"page_size",    "page_size() -> u64"},
  {"sm64_next",    "sm64_next(&mut st: u64) -> u64"},
};

// LSP CompletionItemKind numbers used below.
enum {
  K_FUNCTION = 3, K_VARIABLE = 6, K_KEYWORD = 14, K_ENUM = 13,
  K_STRUCT = 22, K_MODULE = 9,
};

// Well-known prelude types.  The prelude materialises lazily in the AST
// (ensurePreludeString etc. on first use), so the dynamic struct/enum
// tables only carry them AFTER the document references them.  Completion
// offers the language-level surface unconditionally; the labels are
// always valid to write.
static const struct { const char* name; const char* detail; int kind; }
PRELUDE_TYPES[] = {
  {"String",     "prelude owned growable UTF-8 buffer",      22},
  {"Vec",        "prelude dynamic container <T>",            22},
  {"Span",       "prelude borrowed contiguous view <T>",      22},
  {"Reader",     "prelude streaming input handle",           22},
  {"Writer",     "prelude streaming output handle",          22},
  {"Option",     "prelude enum <T> (Some/None)",             13},
  {"Result",     "prelude enum <T, E> (Ok/Err)",             13},
  {"ParseError", "prelude enum (Invalid/OutOfRange)",        13},
  {"IoError",    "prelude enum (streaming I/O errors)",       13},
};

void handleCompletion(Workspace& ws, int id, const Json& params) {
  const Json* td = params.find("textDocument");
  const Json* pos = params.find("position");
  if (!td || !pos) {
    sendResponse(id, Json::object({{"isIncomplete", Json::boolean(false)},
                                   {"items", Json::array({})}}));
    return;
  }
  const Json* uri = td->find("uri");
  if (!uri) {
    sendResponse(id, Json::object({{"isIncomplete", Json::boolean(false)},
                                   {"items", Json::array({})}}));
    return;
  }
  const Json* line = pos->find("line");
  const Json* ch = pos->find("character");
  if (!line || !ch) {
    sendResponse(id, Json::object({{"isIncomplete", Json::boolean(false)},
                                   {"items", Json::array({})}}));
    return;
  }
  std::lock_guard<std::mutex> g(ws.mu);
  auto* an = ensureAnalysed(ws, uri->s);
  const std::string& text = ws.docs[uri->s].text;
  size_t off = lspPosToOffset(text, (int)line->i, (int)ch->i);
  std::string prefix;
  size_t wordStart = 0;
  wordPrefixAt(text, off, prefix, wordStart);
  std::string moduleQualifier;
  bool qualified = qualifiedBefore(text, wordStart, moduleQualifier);

  Json items = Json::array({});
  auto addItem = [&](const std::string& label, int kind,
                     const std::string& detail, char sortClass) {
    if (label.empty()) return;
    if (!prefix.empty() && label.compare(0, prefix.size(), prefix) != 0)
      return;
    items.arr.push_back(Json::object({
      {"label",    Json::str(label)},
      {"kind",     Json::num(kind)},
      {"detail",   Json::str(detail)},
      {"sortText", Json::str(std::string(1, sortClass) + label)},
    }));
  };

  if (an && an->program) {
    Program* prog = an->program.get();

    // scope-visible locals + params of the fn the cursor sits in.  The
    // same name can be re-bound in inner scopes; keep the binding closest
    // BEFORE the cursor (a binding after the cursor is not yet in scope).
    if (!qualified) {
      freshet::FnDecl* enc = fnEnclosing(prog, off);
      if (enc) {
        freshet::FnSema* fs = freshet::semaForFn(enc);
        if (fs) {
          std::map<std::string, const freshet::Sym*> latest;
          for (auto& sym : fs->allSyms) {
            if (!sym.declLoc.startOffset && !sym.declLoc.line) continue;
            if (sym.declLoc.startOffset > off) continue;
            latest[sym.name] = &sym;
          }
          for (auto& kv : latest)
            addItem(kv.first, K_VARIABLE,
                    freshet::typeToString(kv.second->ty) +
                        (kv.second->mut ? " (mut)" : ""),
                    '0');
        }
      }
    }

    // functions (qualified mode: only that module's).  Trait impl methods
    // are mangled and not nameable bare — leave them out.
    for (auto* f : prog->fns) {
      if (!f || f->isTraitImpl) continue;
      if (qualified && f->module != moduleQualifier) continue;
      std::string sig = NavWalker::fnSignature(f);
      if (f->isGeneric) {
        sig += "  <";
        for (size_t i = 0; i < f->tparams.size(); ++i) {
          if (i) sig += ", ";
          sig += f->tparams[i];
        }
        sig += ">";
      }
      std::string detail = sig;
      if (!qualified && !f->module.empty() && !f->imported)
        detail += "  [module " + f->module + "]";
      addItem(f->name, f->imported ? K_FUNCTION : K_FUNCTION, detail, '1');
    }
    // globals (qualified mode: only that module's)
    for (auto* gv : prog->globals) {
      if (!gv) continue;
      if (qualified && gv->module != moduleQualifier) continue;
      addItem(gv->name, K_VARIABLE,
              (gv->mut ? "mut global: " : "global: ") +
                  freshet::typeToString(gv->ty),
              '1');
    }
  }

  if (!qualified) {
    // declared type constructors (templates included: `Vec` is usable
    // through instantiation even when the template itself is not)
    if (an && an->program) {
      for (auto* sd : an->program->structs) {
        if (!sd) continue;
        std::string detail = sd->isGeneric ? "generic struct" : "struct";
        if (sd->isGeneric && !sd->tparams.empty()) {
          detail += " <";
          for (size_t i = 0; i < sd->tparams.size(); ++i) {
            if (i) detail += ", ";
            detail += sd->tparams[i];
          }
          detail += ">";
        }
        addItem(sd->name, K_STRUCT, detail, '2');
      }
      for (auto* ed : an->program->enums) {
        if (!ed) continue;
        addItem(ed->name, K_ENUM, "enum", '2');
      }
    }
    // the language surface: prelude types, keywords, compiler-known builtins
    for (auto& pt : PRELUDE_TYPES)
      addItem(pt.name, pt.kind, pt.detail, '2');
    for (auto* kw : COMPLETION_KEYWORDS)
      addItem(kw, K_KEYWORD, "keyword", '3');
    for (auto& b : COMPLETION_BUILTINS)
      addItem(b.name, K_FUNCTION, std::string("builtin: ") + b.sig, '1');
  }

  sendResponse(id, Json::object({{"isIncomplete", Json::boolean(false)},
                                 {"items", items}}));
}

// Collect the inlay-hint sites of one fn body: every un-annotated `let`
// (type hint) and every resolved user call (parameter hints).
struct HintWalker {
  std::vector<freshet::Stmt*> lets;
  std::vector<freshet::Expr*> calls;
  std::unordered_set<const freshet::Expr*> seenExprs;
  std::unordered_set<const freshet::Stmt*> seenStmts;

  void expr(freshet::Expr* root) {
    if (!root) return;
    std::vector<freshet::Expr*> pending;
    pending.push_back(root);
    while (!pending.empty()) {
      freshet::Expr* e = pending.back();
      pending.pop_back();
      if (!e || !seenExprs.insert(e).second) continue;
      if (e->k == freshet::Expr::CALL && e->callee) calls.push_back(e);
      if (e->base) pending.push_back(e->base);
      if (e->rhs) pending.push_back(e->rhs);
      if (e->lhs) pending.push_back(e->lhs);
      // SLICE reuses lhs/rhs; CAST carries castTy only
      for (auto it = e->args.rbegin(); it != e->args.rend(); ++it)
        if (*it) pending.push_back(*it);
    }
  }

  void stmt(freshet::Stmt* root) {
    if (!root) return;
    std::vector<freshet::Stmt*> pending;
    pending.push_back(root);
    while (!pending.empty()) {
      freshet::Stmt* s = pending.back();
      pending.pop_back();
      if (!s || !seenStmts.insert(s).second) continue;
      if (s->k == freshet::Stmt::LET && !s->hasAnn) lets.push_back(s);
      expr(s->init);
      expr(s->lhs);      expr(s->rhs);
      expr(s->retVal);   expr(s->deferExpr);
      expr(s->cond);     expr(s->expr);
      expr(s->matchOn);
      expr(s->rangeLo);  expr(s->rangeHi);
      expr(s->forInit);  expr(s->forCond); expr(s->forStep);
      expr(s->plo);      expr(s->phi);
      for (auto& c : s->clauses) expr(c.e);
      std::vector<freshet::Stmt*> kids;
      pushStmtChildren(s, kids);
      for (auto it = kids.rbegin(); it != kids.rend(); ++it)
        pending.push_back(*it);
    }
  }
};

// end offset of the binding identifier of `let [mut] name ...` — the type
// hint is rendered right after the name.  Falls back to the statement
// start when the scan does not line up (error-recovery ASTs).
static size_t letNameEnd(const std::string& text, freshet::Stmt* s) {
  size_t i = s->loc.startOffset;
  if (i >= text.size()) return s->loc.startOffset;
  auto skipWs = [&]() {
    while (i < text.size() && isspace((unsigned char)text[i])) ++i;
  };
  auto skipIdent = [&]() {
    while (i < text.size() &&
           (isalnum((unsigned char)text[i]) || text[i] == '_')) ++i;
  };
  skipWs();
  if (text.compare(i, 3, "let") != 0) return s->loc.startOffset;
  i += 3;
  skipWs();
  // optional `mut` keyword (followed by more word chars => it was the
  // binding's own name spelled like the keyword — impossible, keywords
  // cannot bind; take the safe branch anyway)
  if (text.compare(i, 3, "mut") == 0 &&
      (i + 3 >= text.size() ||
       (!isalnum((unsigned char)text[i + 3]) && text[i + 3] != '_'))) {
    i += 3;
    skipWs();
  }
  skipIdent();
  return i;
}

void handleInlayHint(Workspace& ws, int id, const Json& params) {
  const Json* td = params.find("textDocument");
  const Json* range = params.find("range");
  if (!td || !range) { sendResponse(id, Json::null()); return; }
  const Json* uri = td->find("uri");
  if (!uri) { sendResponse(id, Json::null()); return; }
  const Json* rs = range->find("start");
  const Json* re = range->find("end");
  if (!rs || !re) { sendResponse(id, Json::null()); return; }

  std::lock_guard<std::mutex> g(ws.mu);
  auto* an = ensureAnalysed(ws, uri->s);
  if (!an || !an->program) { sendResponse(id, Json::array({})); return; }
  const std::string& text = ws.docs[uri->s].text;
  size_t startOff = lspPosToOffset(text, (int)rs->find("line")->i,
                                   (int)rs->find("character")->i);
  size_t endOff = lspPosToOffset(text, (int)re->find("line")->i,
                                 (int)re->find("character")->i);

  Json hints = Json::array({});
  auto addHint = [&](size_t pos, const std::string& label, int kind,
                    bool padLeft, bool padRight) {
    if (pos < startOff || pos > endOff) return;
    Json h = Json::object({
      {"position", offsetToLspPos(text, pos)},
      {"label",    Json::str(label)},
      {"kind",     Json::num(kind)},
    });
    if (padLeft)   h.obj.push_back({"paddingLeft",  Json::boolean(true)});
    if (padRight)  h.obj.push_back({"paddingRight", Json::boolean(true)});
    hints.arr.push_back(h);
  };

  for (auto* f : an->program->fns) {
    if (!f || f->imported || f->isGeneric || !f->body) continue;
    freshet::FnSema* fs = freshet::semaForFn(f);
    HintWalker hw;
    hw.stmt(f->body);

    // ---- inferred-type hints: `let x = ...;` without an annotation ----
    for (auto* s : hw.lets) {
      if (!s || !s->init) continue;
      freshet::Type* ty = nullptr;
      if (fs) {
        for (auto& sym : fs->allSyms) {
          if (sym.declLoc.startOffset == s->loc.startOffset &&
              (sym.declLoc.line == s->loc.line)) {
            ty = sym.ty;
            break;
          }
        }
      }
      if (!ty) ty = s->init->ty;      // fall back to the init's own type
      if (!ty || ty->k == freshet::Type::VOID) continue;
      size_t pos = letNameEnd(text, s);
      addHint(pos, ": " + freshet::typeToString(ty),
              /*Type=*/1, /*padLeft=*/true, /*padRight=*/false);
    }

    // ---- parameter-name hints at resolved call sites ----
    for (auto* e : hw.calls) {
      freshet::FnDecl* callee = e->callee;
      if (!callee) continue;
      for (size_t i = 0; i < e->args.size() && i < callee->params.size();
           ++i) {
        freshet::Expr* arg = e->args[i];
        if (!arg) continue;
        // an argument already spelled as the parameter's own name carries
        // no information — editors suppress those, so do not send them
        if (arg->k == freshet::Expr::IDENT && arg->name == callee->params[i].name)
          continue;
        addHint(arg->loc.startOffset, callee->params[i].name + ":",
                /*Parameter=*/2, false, /*padRight=*/true);
      }
    }
  }

  sendResponse(id, hints);
}

// ---------------- Document formatting (report §10.2 + LSP) ----------------
// textDocument/formatting + textDocument/rangeFormatting back "Format
// Document" / "Format Selection" with the same token-preserving formatter
// that `penstock fmt` uses, applied to the open buffer and honouring the
// client's FormattingOptions (tabSize / insertSpaces).  Formatting never
// needs the analysed AST — it is token-level — so it works even while the
// document has parse errors.

static void sendNoEdits(int id) { sendResponse(id, Json::array({})); }

static void readFormatOptions(const Json& params, int& tabSize,
                               bool& insertSpaces) {
  const Json* opts = params.find("options");
  if (!opts) return;
  const Json* ts = opts->find("tabSize");
  const Json* ins = opts->find("insertSpaces");
  if (ts && ts->i > 0) tabSize = (int)ts->i;
  if (ins) insertSpaces = ins->b;
}

void handleFormatting(Workspace& ws, int id, const Json& params) {
  const Json* td = params.find("textDocument");
  const Json* uri = td ? td->find("uri") : nullptr;
  if (!uri) { sendNoEdits(id); return; }
  int tabSize = 4;
  bool insertSpaces = true;
  readFormatOptions(params, tabSize, insertSpaces);
  std::lock_guard<std::mutex> g(ws.mu);
  auto it = ws.docs.find(uri->s);
  if (it == ws.docs.end()) { sendNoEdits(id); return; }
  const std::string& text = it->second.text;
  bool ok = false;
  std::string formatted = freshet::formatSourceText(text, tabSize,
                                                    insertSpaces, &ok);
  // gate refusal or already-formatted: no edits, editor shows "no changes"
  if (!ok || formatted == text) { sendNoEdits(id); return; }
  Json edit = Json::object({
    {"range", Json::object({
      {"start", Json::object({{"line", Json::num(0)},
                              {"character", Json::num(0)}})},
      {"end",   offsetToLspPos(text, text.size())},
    })},
    {"newText", Json::str(formatted)},
  });
  sendResponse(id, Json::array({edit}));
}

void handleRangeFormatting(Workspace& ws, int id, const Json& params) {
  const Json* td = params.find("textDocument");
  const Json* range = params.find("range");
  const Json* uri = td ? td->find("uri") : nullptr;
  if (!uri || !range) { sendNoEdits(id); return; }
  const Json* rs = range->find("start");
  const Json* re = range->find("end");
  if (!rs || !re) { sendNoEdits(id); return; }
  int tabSize = 4;
  bool insertSpaces = true;
  readFormatOptions(params, tabSize, insertSpaces);
  std::lock_guard<std::mutex> g(ws.mu);
  auto it = ws.docs.find(uri->s);
  if (it == ws.docs.end()) { sendNoEdits(id); return; }
  const std::string& text = it->second.text;
  size_t sOff = lspPosToOffset(text, (int)rs->find("line")->i,
                               (int)rs->find("character")->i);
  size_t eOff = lspPosToOffset(text, (int)re->find("line")->i,
                               (int)re->find("character")->i);
  freshet::RangeFormatResult r =
      freshet::formatSourceRange(text, sOff, eOff, tabSize, insertSpaces);
  // selection covering no declaration, gate refusal, or nothing to do
  if (!r.ok || r.text == text.substr(r.start, r.end - r.start)) {
    sendNoEdits(id);
    return;
  }
  Json edit = Json::object({
    {"range", Json::object({
      {"start", offsetToLspPos(text, r.start)},
      {"end",   offsetToLspPos(text, r.end)},
    })},
    {"newText", Json::str(r.text)},
  });
  sendResponse(id, Json::array({edit}));
}

// ---------------- Semantic tokens (LSP) -----------------------------------
// textDocument/semanticTokens/full colours every name the sema-annotated
// AST can resolve (locals, params, calls, fields, enum variants, module
// paths, declarations) plus the names expressions cannot reach: type
// references in annotation positions (`p: Point`, `-> String`,
// `Span<f64>`) and module/import identifiers.
//
// Precision policy (three layers):
//   1. AST pass — claims exact identifier spans from sema resolutions
//      (expr->sym / callee / globalRef / fnValue / enumRef / fieldIdx);
//   2. declaration pass — fn/struct/enum/trait/impl/import declaration
//      sites from their SrcLocs;
//   3. textual pass — every UNCLAIMED identifier classified by name
//      tables built from the analysed program (types, fns, modules).
//
// Tokens overlay the TextMate grammar (semantic wins), so themes gain
// distinct colours for types, function arguments, module/import names,
// fields and enum members while the grammar keeps providing keyword /
// string / comment colouring.

static bool semIsIdentChar(char c) {
  return isalnum((unsigned char)c) || c == '_';
}

// the source-spelled part of a sema-mangled name ("Vec$i64", the
// trait-impl "Trait$Type$method" forms) — `$` never appears in source
static std::string semSourceName(const std::string& n) {
  size_t d = n.find('$');
  return d == std::string::npos ? n : n.substr(0, d);
}

// whole-word search of `name` inside text[lo, hi); SIZE_MAX when absent
static size_t semFindWord(const std::string& text, const std::string& name,
                          size_t lo, size_t hi) {
  if (name.empty() || hi > text.size() || lo >= hi) return SIZE_MAX;
  size_t pos = lo;
  while (pos + name.size() <= hi) {
    size_t f = text.find(name, pos);
    if (f == std::string::npos || f + name.size() > hi) return SIZE_MAX;
    bool left = f == 0 || !semIsIdentChar(text[f - 1]);
    bool right = f + name.size() >= text.size() ||
                 !semIsIdentChar(text[f + name.size()]);
    if (left && right) return f;
    pos = f + 1;
  }
  return SIZE_MAX;
}

// the exclusive end of a node's span; single-point spans (unset end)
// widen to the end of the line so identifier searches can proceed
static size_t semSpanEnd(const std::string& text,
                         const freshet::SrcLoc& loc) {
  size_t lo = (size_t)loc.startOffset;
  size_t hi = (size_t)loc.endOffset;
  if (hi <= lo || hi > text.size()) {
    size_t nl = text.find('\n', lo);
    hi = (nl == std::string::npos) ? text.size() : nl;
  }
  return hi > text.size() ? text.size() : hi;
}

// identifier span of `name` within a node's span; falls back to the whole
// line because some nodes store only their FIRST token (a param whose
// stored token is the `owned`/`borrowed` mode word; an import whose stored
// token is the `import` keyword itself)
static size_t semFindNameNear(const std::string& text,
                              const std::string& name,
                              const freshet::SrcLoc& loc) {
  size_t lo = (size_t)loc.startOffset;
  if (lo >= text.size()) return SIZE_MAX;
  size_t at = semFindWord(text, name, lo, semSpanEnd(text, loc));
  if (at != SIZE_MAX) return at;
  size_t nl = text.find('\n', lo);
  size_t le = (nl == std::string::npos) ? text.size() : nl;
  return semFindWord(text, name, lo, le);
}

// `mod::name` / `Enum.Variant` — the qualifier and the final tail
static void semSplitQualified(const std::string& name,
                              std::string& qualifier, std::string& tail) {
  size_t p = name.rfind("::");
  size_t d = name.rfind('.');
  size_t cut = std::string::npos;
  bool sep2 = false;
  if (p != std::string::npos && (d == std::string::npos || p > d)) {
    cut = p; sep2 = true;
  } else if (d != std::string::npos) {
    cut = d;
  }
  if (cut == std::string::npos) { qualifier.clear(); tail = name; return; }
  qualifier = name.substr(0, cut);
  tail = name.substr(cut + (sep2 ? 2 : 1));
}

// the fn declaration name inside its (possibly attribute-prefixed) span:
// the identifier following the FIRST `fn` keyword.  Needed because
// trait-impl methods get their names mangled in sema, so searching
// f->name can fail; the decl's own `fn` keyword always precedes the name.
static size_t semFindFnDeclName(const std::string& text, size_t lo,
                                size_t hi, size_t& lenOut) {
  size_t i = lo;
  while (i < hi) {
    if (semIsIdentChar(text[i]) &&
        (i == 0 || !semIsIdentChar(text[i - 1]))) {
      size_t j = i;
      while (j < hi && semIsIdentChar(text[j])) ++j;
      if (j - i == 2 && text[i] == 'f' && text[i + 1] == 'n') {
        size_t k = j;
        while (k < hi && isspace((unsigned char)text[k])) ++k;
        if (k < hi && semIsIdentChar(text[k])) {
          size_t e = k;
          while (e < hi && semIsIdentChar(text[e])) ++e;
          lenOut = e - k;
          return k;
        }
      }
      i = j;
    } else {
      ++i;
    }
  }
  return SIZE_MAX;
}

struct SemTokOut { size_t s, len; int type, mods; };

struct SemTokCollector {
  const std::string& text;
  std::vector<SemTokOut> raw;
  std::map<size_t, size_t> claims;   // accepted token starts -> ends

  explicit SemTokCollector(const std::string& t) : text(t) {}

  // accept the identifier span [s, s+len) unless it is malformed,
  // not identifier-bounded, or already claimed
  void add(size_t s, size_t len, int type, int mods) {
    if (len == 0 || s >= text.size() || s + len > text.size()) return;
    for (size_t i = s; i < s + len; ++i)
      if (!semIsIdentChar(text[i])) return;
    if (s > 0 && semIsIdentChar(text[s - 1])) return;
    if (s + len < text.size() && semIsIdentChar(text[s + len])) return;
    // identifiers are disjoint by construction; the backward window only
    // has to catch claims starting shortly before `s`
    for (auto it = claims.lower_bound(s > 72 ? s - 72 : 0);
         it != claims.end() && it->first < s + len; ++it)
      if (it->second > s) return;
    claims[s] = s + len;
    raw.push_back({s, len, type, mods});
  }

  bool claimed(size_t s, size_t len) const {
    for (auto it = claims.lower_bound(s > 72 ? s - 72 : 0);
         it != claims.end() && it->first < s + len; ++it)
      if (it->second > s) return true;
    return false;
  }
};

struct SemWalker {
  SemTokCollector& C;
  std::string docPath;
  std::unordered_set<const freshet::Expr*> seenExprs;
  std::unordered_set<const freshet::Stmt*> seenStmts;

  bool inDoc(const freshet::SrcLoc& loc) const {
    return !loc.file.empty() && sameSourceFile(loc.file, docPath);
  }

  void nameTok(const freshet::SrcLoc& loc, const std::string& name,
               int type, int mods) {
    if (name.empty() || !inDoc(loc)) return;
    size_t at = semFindNameNear(C.text, name, loc);
    if (at != SIZE_MAX) C.add(at, name.size(), type, mods);
  }

  // qualified paths: colour the qualifier (`geometry` of
  // `geometry::distance`, `Mode` of `Mode.On`) + resolve the tail
  void identTok(const freshet::Expr* e, int type, int mods,
                int prefixType = ST_NAMESPACE) {
    std::string qualifier, tail;
    semSplitQualified(e->name, qualifier, tail);
    if (!qualifier.empty()) {
      size_t lo = (size_t)e->loc.startOffset;
      if (inDoc(e->loc) && lo < C.text.size() &&
          C.text.compare(lo, qualifier.size(), qualifier) == 0)
        C.add(lo, qualifier.size(), prefixType, 0);
    }
    nameTok(e->loc, tail, type, mods);
  }

  void fieldTok(const freshet::Expr* e) {
    if (e->name.empty() || !inDoc(e->loc)) return;
    // qualified enum access `Color.Red` resolves onto the FIELD node
    // itself (resolveVariantExprAgainst) — colour the trailing name as an
    // enum member, the enum name prefix as a type
    int type = (e->enumRef && e->variantIdx >= 0) ? ST_ENUMMEMBER : ST_PROPERTY;
    size_t hi = semSpanEnd(C.text, e->loc);
    // the FIELD name is the trailing identifier of `base.name`
    size_t s = hi >= e->name.size() ? hi - e->name.size() : SIZE_MAX;
    if (s != SIZE_MAX && s >= e->loc.startOffset &&
        C.text.compare(s, e->name.size(), e->name) == 0)
      C.add(s, e->name.size(), type, 0);
    else
      nameTok(e->loc, e->name, type, 0);
  }

  void slitTok(const freshet::Expr* e) {
    std::string name = semSourceName(e->name);
    if (name.empty() || !inDoc(e->loc)) return;
    int mods = 0;
    for (auto& pt : PRELUDE_TYPES)
      if (name == pt.name) { mods |= SM_DEFAULTLIB; break; }
    // the literal's name is the leading identifier of its span
    size_t lo = (size_t)e->loc.startOffset;
    if (lo + name.size() <= C.text.size() &&
        C.text.compare(lo, name.size(), name) == 0)
      C.add(lo, name.size(), ST_STRUCT, mods);
    else
      nameTok(e->loc, name, ST_STRUCT, mods);
  }

  // the bound identifier of `let [mut] name ...`
  void letTok(const freshet::Stmt* s) {
    if (!inDoc(s->loc)) return;
    size_t i = s->loc.startOffset;
    if (i >= C.text.size()) return;
    auto skipWs = [&]() {
      while (i < C.text.size() && isspace((unsigned char)C.text[i])) ++i;
    };
    skipWs();
    if (i + 3 > C.text.size() || C.text.compare(i, 3, "let") != 0) return;
    i += 3;
    skipWs();
    if (i + 3 <= C.text.size() && C.text.compare(i, 3, "mut") == 0 &&
        (i + 3 >= C.text.size() || !semIsIdentChar(C.text[i + 3]))) {
      i += 3;
      skipWs();
    }
    size_t start = i;
    while (i < C.text.size() && semIsIdentChar(C.text[i])) ++i;
    if (i > start && C.text.compare(start, i - start, s->var) == 0)
      C.add(start, i - start, ST_VARIABLE,
            SM_DECLARATION | (s->mut ? 0 : SM_READONLY));
  }

  // `for (i in a..b)` / `for (i = 0; ...)` / `parfor (i in ...)`: the
  // control variable right after the `(`
  void controlVarTok(const freshet::Stmt* s, const std::string& var) {
    if (var.empty() || !inDoc(s->loc)) return;
    size_t i = s->loc.startOffset;
    if (i >= C.text.size()) return;
    while (i < C.text.size() && C.text[i] != '(' && C.text[i] != ';') ++i;
    if (i >= C.text.size() || C.text[i] != '(') return;
    ++i;
    while (i < C.text.size() && isspace((unsigned char)C.text[i])) ++i;
    size_t start = i;
    while (i < C.text.size() && semIsIdentChar(C.text[i])) ++i;
    if (i > start && C.text.compare(start, i - start, var) == 0)
      C.add(start, i - start, ST_VARIABLE, SM_READONLY);
  }

  void armTok(const freshet::MatchArm& arm) {
    if (!arm.wildcard && !arm.variantName.empty() && inDoc(arm.loc)) {
      std::string qualifier, tail;
      semSplitQualified(arm.variantName, qualifier, tail);
      size_t at = semFindNameNear(C.text, tail, arm.loc);
      if (at != SIZE_MAX) C.add(at, tail.size(), ST_ENUMMEMBER, 0);
      if (!qualifier.empty()) {
        size_t lo = (size_t)arm.loc.startOffset;
        if (lo < C.text.size() &&
            C.text.compare(lo, qualifier.size(), qualifier) == 0)
          C.add(lo, qualifier.size(), ST_ENUM, 0);
      }
    }
    // payload bindings are fresh immutable locals
    for (size_t i = 0; i < arm.bindings.size(); ++i) {
      if (arm.bindings[i].empty()) continue;
      freshet::SrcLoc loc =
          i < arm.bindingLocs.size() ? arm.bindingLocs[i] : arm.loc;
      size_t at = semFindNameNear(C.text, arm.bindings[i], loc);
      if (at != SIZE_MAX)
        C.add(at, arm.bindings[i].size(), ST_VARIABLE, SM_READONLY);
    }
  }

  void expr(freshet::Expr* root) {
    if (!root) return;
    std::vector<freshet::Expr*> pending{root};
    while (!pending.empty()) {
      freshet::Expr* e = pending.back();
      pending.pop_back();
      if (!e || !seenExprs.insert(e).second) continue;
      switch (e->k) {
        case freshet::Expr::IDENT:
          if (e->sym)
            identTok(e, e->sym->k == freshet::Sym::PARAM ? ST_PARAMETER
                                                          : ST_VARIABLE,
                     e->sym->mut ? 0 : SM_READONLY);
          else if (e->globalRef)
            identTok(e, ST_VARIABLE,
                     SM_STATIC | (e->globalRef->mut ? 0 : SM_READONLY));
          else if (e->fnValue)
            identTok(e, ST_FUNCTION, 0);
          else if (e->enumRef && e->variantIdx >= 0)
            identTok(e, ST_ENUMMEMBER, 0, ST_ENUM);
          break;
        case freshet::Expr::CALL:
          if (e->enumRef && e->variantIdx >= 0)
            identTok(e, ST_ENUMMEMBER, 0, ST_ENUM);   // variant constructor
          else if (e->callee)
            identTok(e, ST_FUNCTION, 0);
          else
            // no resolution: the compiler-known builtins (print, malloc,
            // sqrt, ...) — unresolved error calls land here too, which
            // only costs colour, never correctness
            identTok(e, ST_FUNCTION, SM_DEFAULTLIB);
          if (e->fnPtrSym)
            identTok(e, e->fnPtrSym->k == freshet::Sym::PARAM ? ST_PARAMETER
                                                              : ST_VARIABLE,
                     e->fnPtrSym->mut ? 0 : SM_READONLY);
          if (e->globalRef)
            identTok(e, ST_VARIABLE,
                     SM_STATIC | (e->globalRef->mut ? 0 : SM_READONLY));
          break;
        case freshet::Expr::FIELD:
          fieldTok(e);
          break;
        case freshet::Expr::SLIT:
          slitTok(e);
          break;
        default:
          break;
      }
      if (e->base) pending.push_back(e->base);
      if (e->rhs) pending.push_back(e->rhs);
      if (e->lhs) pending.push_back(e->lhs);
      for (auto it = e->args.rbegin(); it != e->args.rend(); ++it)
        if (*it) pending.push_back(*it);
    }
  }

  void stmt(freshet::Stmt* root) {
    if (!root) return;
    std::vector<freshet::Stmt*> pending{root};
    while (!pending.empty()) {
      freshet::Stmt* s = pending.back();
      pending.pop_back();
      if (!s || !seenStmts.insert(s).second) continue;
      switch (s->k) {
        case freshet::Stmt::LET:
          letTok(s);
          expr(s->init);
          break;
        case freshet::Stmt::ASSIGN:
          expr(s->lhs); expr(s->rhs); break;
        case freshet::Stmt::RETURN: expr(s->retVal); break;
        case freshet::Stmt::DEFER: expr(s->deferExpr); break;
        case freshet::Stmt::EXPR: expr(s->expr); break;
        case freshet::Stmt::IF:
          expr(s->cond);
          if (s->elseB) pending.push_back(s->elseB);
          if (s->thenB) pending.push_back(s->thenB);
          break;
        case freshet::Stmt::WHILE:
          expr(s->cond);
          if (s->body) pending.push_back(s->body);
          break;
        case freshet::Stmt::FOR:
          controlVarTok(s, s->ivar);
          if (s->forRange) {
            expr(s->rangeLo); expr(s->rangeHi);
          } else {
            expr(s->forInit); expr(s->forCond); expr(s->forStep);
          }
          if (s->body) pending.push_back(s->body);
          break;
        case freshet::Stmt::PARFOR:
          controlVarTok(s, s->pvar);
          expr(s->plo); expr(s->phi);
          for (auto& c : s->clauses) expr(c.e);
          if (s->body) pending.push_back(s->body);
          break;
        case freshet::Stmt::MATCH:
          expr(s->matchOn);
          for (auto& arm : s->matchArms) {
            armTok(arm);
            if (arm.body) pending.push_back(arm.body);
          }
          break;
        case freshet::Stmt::BLOCK:
          for (auto it = s->stmts.rbegin(); it != s->stmts.rend(); ++it)
            if (*it) pending.push_back(*it);
          break;
        default:
          break;
      }
    }
  }
};

// ---- declaration sites: fns (incl. extern/generic/trait-impl members),
// globals, structs + fields, enums + variants, traits + methods, impl
// blocks, imports.  The `module name;` declaration itself has no stored
// SrcLoc on Program — the textual pass below colours it.
static void semAddDecls(SemTokCollector& C, freshet::Program* prog,
                        const std::string& docPath) {
  for (auto* f : prog->fns) {
    if (!f || !sameSourceFile(f->loc.file, docPath)) continue;
    size_t lo = (size_t)f->loc.startOffset;
    if (lo >= C.text.size()) continue;
    size_t len = 0;
    size_t at = semFindFnDeclName(C.text, lo, semSpanEnd(C.text, f->loc), len);
    if (at != SIZE_MAX) C.add(at, len, ST_FUNCTION, SM_DECLARATION);
    for (auto& p : f->params) {
      if (!sameSourceFile(p.loc.file, docPath)) continue;
      size_t pa = semFindNameNear(C.text, p.name, p.loc);
      if (pa != SIZE_MAX)
        C.add(pa, p.name.size(), ST_PARAMETER, SM_DECLARATION | SM_READONLY);
    }
  }
  for (auto* gv : prog->globals) {
    if (!gv || !sameSourceFile(gv->loc.file, docPath)) continue;
    size_t at = semFindNameNear(C.text, gv->name, gv->loc);
    if (at != SIZE_MAX)
      C.add(at, gv->name.size(), ST_VARIABLE,
            SM_DECLARATION | SM_STATIC | (gv->mut ? 0 : SM_READONLY));
  }
  for (auto* sd : prog->structs) {
    if (!sd || sd->templateOf || !sameSourceFile(sd->loc.file, docPath))
      continue;
    std::string nm = semSourceName(sd->name);
    size_t at = semFindNameNear(C.text, nm, sd->loc);
    if (at != SIZE_MAX) C.add(at, nm.size(), ST_STRUCT, SM_DECLARATION);
    for (auto& fl : sd->fields) {
      if (!sameSourceFile(fl.loc2.file, docPath)) continue;
      size_t fa = semFindNameNear(C.text, fl.name, fl.loc2);
      if (fa != SIZE_MAX)
        C.add(fa, fl.name.size(), ST_PROPERTY, SM_DECLARATION);
    }
  }
  for (auto* ed : prog->enums) {
    if (!ed || !sameSourceFile(ed->loc.file, docPath)) continue;
    std::string nm = ed->templateName.empty() ? semSourceName(ed->name)
                                              : ed->templateName;
    size_t at = semFindNameNear(C.text, nm, ed->loc);
    if (at != SIZE_MAX) C.add(at, nm.size(), ST_ENUM, SM_DECLARATION);
    for (auto& v : ed->variants) {
      if (!sameSourceFile(v.loc.file, docPath)) continue;
      size_t va = semFindNameNear(C.text, v.name, v.loc);
      if (va != SIZE_MAX)
        C.add(va, v.name.size(), ST_ENUMMEMBER, SM_DECLARATION);
    }
  }
  for (auto* td : prog->traits) {
    if (!td || !sameSourceFile(td->loc.file, docPath)) continue;
    size_t at = semFindNameNear(C.text, td->name, td->loc);
    if (at != SIZE_MAX) C.add(at, td->name.size(), ST_INTERFACE, SM_DECLARATION);
    for (auto& m : td->methods) {
      if (!sameSourceFile(m.loc.file, docPath)) continue;
      size_t ma = semFindNameNear(C.text, m.name, m.loc);
      if (ma != SIZE_MAX) C.add(ma, m.name.size(), ST_FUNCTION, SM_DECLARATION);
      for (auto& p : m.params) {
        size_t pa = semFindNameNear(C.text, p.name, p.loc);
        if (pa != SIZE_MAX)
          C.add(pa, p.name.size(), ST_PARAMETER, SM_DECLARATION | SM_READONLY);
      }
    }
  }
  for (auto* im : prog->impls) {
    if (!im || !sameSourceFile(im->loc.file, docPath)) continue;
    size_t at = semFindNameNear(C.text, im->traitName, im->loc);
    if (at != SIZE_MAX) C.add(at, im->traitName.size(), ST_INTERFACE, 0);
    // target type when it is a plain identifier (`impl Numeric for i64`)
    std::string self = semSourceName(im->selfEnc);
    if (!self.empty() &&
        self.find_first_of(":;()[]<>*") == std::string::npos) {
      size_t ta = semFindNameNear(C.text, self, im->loc);
      if (ta != SIZE_MAX) C.add(ta, self.size(), ST_TYPE, 0);
    }
  }
  for (auto& im : prog->imports) {
    if (!sameSourceFile(im.loc.file, docPath)) continue;
    size_t at = semFindNameNear(C.text, im.name, im.loc);
    if (at != SIZE_MAX) C.add(at, im.name.size(), ST_NAMESPACE, SM_DECLARATION);
  }
}

// ---- textual pass: classify UNCLAIMED identifiers by program name tables
struct SemMiniTok { size_t off, len; std::string text; };

static void semLexMini(const std::string& src, std::vector<SemMiniTok>& out) {
  size_t i = 0;
  while (i < src.size()) {
    unsigned char c = (unsigned char)src[i];
    if (isspace(c)) { ++i; continue; }
    if (c == '/' && i + 1 < src.size() && src[i + 1] == '/') {
      while (i < src.size() && src[i] != '\n') ++i;
      continue;
    }
    if (c == '/' && i + 1 < src.size() && src[i + 1] == '*') {
      i += 2;
      while (i + 1 < src.size() && !(src[i] == '*' && src[i + 1] == '/')) ++i;
      i = (i + 1 < src.size()) ? i + 2 : src.size();
      continue;
    }
    if (c == '"') {
      ++i;
      while (i < src.size() && src[i] != '"') {
        if (src[i] == '\\' && i + 1 < src.size()) ++i;
        ++i;
      }
      if (i < src.size()) ++i;
      continue;               // strings claim nothing
    }
    if (isalpha(c) || c == '_') {
      size_t s = i;
      while (i < src.size() && semIsIdentChar(src[i])) ++i;
      out.push_back(SemMiniTok{s, i - s, src.substr(s, i - s)});
      continue;
    }
    if (isdigit(c)) {
      while (i < src.size() && (semIsIdentChar(src[i]) || src[i] == '.')) ++i;
      continue;
    }
    // punct — the two-char separators matter for `::` lookahead
    static const char* pairs[] = {"::", "->", "=>", "==", "!=", "<=", ">=",
                                  "<<", ">>", "&&", "||", ".."};
    size_t n = 1;
    for (auto* p2 : pairs)
      if (src.compare(i, 2, p2) == 0) { n = 2; break; }
    out.push_back(SemMiniTok{i, n, src.substr(i, n)});
    i += n;
  }
}

static void semAddTextual(SemTokCollector& C, freshet::Program* prog,
                          const std::string& text) {
  std::map<std::string, int> typeNames;
  std::set<std::string> preludeNames;
  for (auto* sd : prog->structs) {
    if (!sd) continue;
    std::string nm = semSourceName(sd->name);
    if (!nm.empty()) typeNames[nm] = ST_STRUCT;
  }
  for (auto* ed : prog->enums) {
    if (!ed) continue;
    std::string nm = ed->templateName.empty() ? semSourceName(ed->name)
                                              : ed->templateName;
    if (!nm.empty()) typeNames[nm] = ST_ENUM;
  }
  for (auto* td : prog->traits) {
    if (td && !td->name.empty()) typeNames[td->name] = ST_INTERFACE;
  }
  // prelude names enter last so the compiler-known surface wins, and
  // they carry the defaultLibrary modifier
  for (auto& pt : PRELUDE_TYPES) {
    typeNames[pt.name] = pt.kind == 13 ? ST_ENUM : ST_STRUCT;
    preludeNames.insert(pt.name);
  }
  std::set<std::string> fnNames;
  for (auto* f : prog->fns) {
    if (!f) continue;
    std::string nm = semSourceName(f->name);
    if (!nm.empty()) fnNames.insert(nm);
  }
  std::set<std::string> builtinNames;
  for (auto& b : COMPLETION_BUILTINS) builtinNames.insert(b.name);

  std::vector<SemMiniTok> toks;
  semLexMini(text, toks);
  for (size_t i = 0; i < toks.size(); ++i) {
    if (toks[i].text.empty()) continue;
    bool isIdent = semIsIdentChar(toks[i].text[0]) && !isdigit((unsigned char)toks[i].text[0]);
    if (!isIdent) continue;
    const std::string& w = toks[i].text;
    const std::string* next = nullptr;
    if (i + 1 < toks.size()) next = &toks[i + 1].text;
    const std::string* prev = nullptr;
    if (i > 0) prev = &toks[i - 1].text;
    // `module name;` / `import name;`
    if (prev && (*prev == "module" || *prev == "import")) {
      C.add(toks[i].off, toks[i].len, ST_NAMESPACE, SM_DECLARATION);
      continue;
    }
    // `name::member` — the module qualifier
    if (next && *next == "::") {
      C.add(toks[i].off, toks[i].len, ST_NAMESPACE, 0);
      continue;
    }
    if (C.claimed(toks[i].off, toks[i].len)) continue;
    // call positions: `name(`
    if (next && *next == "(") {
      if (fnNames.count(w))
        C.add(toks[i].off, toks[i].len, ST_FUNCTION, 0);
      else if (builtinNames.count(w))
        C.add(toks[i].off, toks[i].len, ST_FUNCTION, SM_DEFAULTLIB);
      continue;
    }
    // type references (annotations, generic arguments, casts)
    auto ty = typeNames.find(w);
    if (ty != typeNames.end()) {
      C.add(toks[i].off, toks[i].len, ty->second,
           preludeNames.count(w) ? SM_DEFAULTLIB : 0);
      continue;
    }
    // bare fn-value references
    if (fnNames.count(w))
      C.add(toks[i].off, toks[i].len, ST_FUNCTION, 0);
  }
}

// byte offset -> (line, UTF-16 character); the same conversion
// offsetToLspPos performs, but indexed for many repeated queries
struct SemLineIndex {
  const std::string& text;
  std::vector<size_t> lineStarts{0};
  explicit SemLineIndex(const std::string& t) : text(t) {
    for (size_t i = 0; i < text.size(); ++i)
      if (text[i] == '\n') lineStarts.push_back(i + 1);
  }
  std::pair<int, int> at(size_t off) const {
    if (off > text.size()) off = text.size();
    size_t lo = 0, hi = lineStarts.size() - 1;
    while (lo < hi) {              // last lineStart <= off
      size_t mid = (lo + hi + 1) / 2;
      if (lineStarts[mid] <= off) lo = mid; else hi = mid - 1;
    }
    size_t ls = lineStarts[lo];
    int units = 0;
    size_t i = ls;
    while (i < off) {
      unsigned char c = (unsigned char)text[i];
      units += (c & 0xF0) == 0xF0 ? 2 : 1;
      if ((c & 0x80) == 0) i += 1;
      else if ((c & 0xE0) == 0xC0) i += 2;
      else if ((c & 0xF0) == 0xE0) i += 3;
      else i += 4;
    }
    return {(int)lo, units};
  }
};

void handleSemanticTokensFull(Workspace& ws, int id, const Json& params) {
  const Json* td = params.find("textDocument");
  const Json* uri = td ? td->find("uri") : nullptr;
  if (!uri) { sendResponse(id, Json::object({{"data", Json::array({})}})); return; }
  std::lock_guard<std::mutex> g(ws.mu);
  auto* an = ensureAnalysed(ws, uri->s);
  auto it = ws.docs.find(uri->s);
  if (it == ws.docs.end()) {
    sendResponse(id, Json::object({{"data", Json::array({})}}));
    return;
  }
  const std::string& text = it->second.text;
  SemTokCollector C(text);
  if (an && an->program) {
    freshet::Program* prog = an->program.get();
    const std::string docPath = uriToPath(uri->s);
    SemWalker w{C, docPath};
    for (auto* f : prog->fns) {
      // bodies of fns the sema annotated (mirrors resolveSymbolAt's
      // filter); decl sites — including externs and generics — come from
      // the declaration pass below
      if (!f || f->imported || f->isGeneric || !f->body) continue;
      w.stmt(f->body);
    }
    semAddDecls(C, prog, docPath);
    semAddTextual(C, prog, text);
  }
  // sort by byte offset, convert to UTF-16 positions, delta-encode
  std::sort(C.raw.begin(), C.raw.end(),
            [](const SemTokOut& a, const SemTokOut& b) { return a.s < b.s; });
  SemLineIndex idx(text);
  Json data = Json::array({});
  int prevLine = 0, prevChar = 0;
  size_t prevEnd = 0;
  for (auto& t : C.raw) {
    if (t.s < prevEnd) continue;          // defensive: overlapping spans
    std::pair<int, int> pos = idx.at(t.s);
    int dl = pos.first - prevLine;
    int dc = (dl == 0) ? pos.second - prevChar : pos.second;
    data.arr.push_back(Json::num(dl));
    data.arr.push_back(Json::num(dc));
    // identifiers are ASCII ([A-Za-z0-9_]) — bytes == UTF-16 units
    data.arr.push_back(Json::num((long long)t.len));
    data.arr.push_back(Json::num(t.type));
    data.arr.push_back(Json::num(t.mods));
    prevLine = pos.first;
    prevChar = pos.second;
    prevEnd = t.s + t.len;
  }
  sendResponse(id, Json::object({{"data", data}}));
}

void handleUnknownMethod(int id, const std::string& method) {
  // Method not found error (LSP spec §6.2).
  Json err = Json::object({
    {"code",     Json::num(-32601)},
    {"message",  Json::str("method not found: " + method)},
  });
  Json msg = Json::object({
    {"jsonrpc", Json::str("2.0")},
    {"id",      Json::num(id)},
    {"error",   err},
  });
  writeMessage(serialize(msg));
}

void handleMessage(Workspace& ws, const Json& msg) {
  const Json* method = msg.find("method");
  const Json* id     = msg.find("id");
  const Json* params = msg.find("params");
  if (!method) return;
  Json emptyParams = Json::object({});
  const Json& p = params ? *params : emptyParams;
  const std::string& m = method->s;
  bool isRequest = (id != nullptr);

  if (m == "initialize") {
    if (isRequest) handleInitialize(ws, (int)id->i, p);
  } else if (m == "initialized") {
    handleInitialized(ws, p);
  } else if (m == "shutdown") {
    if (isRequest) handleShutdown(ws, (int)id->i);
  } else if (m == "exit") {
    handleExit(ws);
  } else if (m == "textDocument/didOpen") {
    handleDidOpen(ws, p);
  } else if (m == "textDocument/didChange") {
    handleDidChange(ws, p);
  } else if (m == "textDocument/didClose") {
    handleDidClose(ws, p);
  } else if (m == "textDocument/hover") {
    if (isRequest) handleHover(ws, (int)id->i, p);
  } else if (m == "textDocument/definition") {
    if (isRequest) handleDefinition(ws, (int)id->i, p);
  } else if (m == "textDocument/references") {
    if (isRequest) handleReferences(ws, (int)id->i, p);
  } else if (m == "textDocument/rename") {
    if (isRequest) handleRename(ws, (int)id->i, p);
  } else if (m == "textDocument/documentSymbol") {
    if (isRequest) handleDocumentSymbol(ws, (int)id->i, p);
  } else if (m == "textDocument/completion") {
    if (isRequest) handleCompletion(ws, (int)id->i, p);
  } else if (m == "textDocument/inlayHint") {
    if (isRequest) handleInlayHint(ws, (int)id->i, p);
  } else if (m == "textDocument/formatting") {
    if (isRequest) handleFormatting(ws, (int)id->i, p);
  } else if (m == "textDocument/rangeFormatting") {
    if (isRequest) handleRangeFormatting(ws, (int)id->i, p);
  } else if (m == "textDocument/semanticTokens/full") {
    if (isRequest) handleSemanticTokensFull(ws, (int)id->i, p);
  } else if (m == "$/cancelRequest") {
    // MVP ignores cancellation; analysis is fast enough for typical
    // files.  A future async variant will honour cancellation tokens.
  } else {
    if (isRequest) handleUnknownMethod((int)id->i, m);
    // Unknown notifications are silently dropped per LSP spec.
  }
}

} // namespace

int main() {
  using namespace freshet;
  Workspace ws;
  std::string body;
  while (readMessage(body)) {
    JsonParser jp(body);
    Json msg = jp.parse();
    handleMessage(ws, msg);
    if (ws.exitRequested.load()) break;
  }
  return ws.shutdownRequested ? 0 : 1;
}
