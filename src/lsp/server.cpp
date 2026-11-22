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
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
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
    // Code actions and semantic tokens are not implemented yet.  Do not
    // advertise placeholders for them.
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
