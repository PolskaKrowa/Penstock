// Freshet module system — Phase 3a/3b integration (report §6.1/§6.2/§6.3).
//
// What lives here:
//
//   * Module identity + import resolution (§6.1):
//       `module geometry;` declares the file's module (default: the
//       sanitised file stem).  `import geometry;` pulls in the module's
//       PUBLIC declarations — importing is NOT textual inclusion: private
//       decls stay invisible, macro/#define leakage is impossible, and
//       same-named decls in different modules coexist.
//
//   * Multi-file compilation (§6.2):
//       `penstock main.fsh util.fsh` — every input is a module of one
//       program.  `import` is resolved transitively with cycle
//       detection; search order: directory of the entry file, `-I` dirs,
//       the builtin modules directory (where the shipped `fsh` module
//       lives), then the current directory.
//
//   * Visibility during sema:
//       setModuleLookupContext() pins (current module, import set)
//       while a declaration's types/body are bound.  lookups then see:
//         - decls of the current module (pub or private),
//         - pub decls of directly imported modules,
//         - prelude decls (module "") — always visible.
//       Post-sema passes run with the strict filter OFF: every
//       cross-reference is already resolved on the AST by then.
//
//   * IR symbol naming:
//       internal fns keep the historical `u_<name>` while the name is
//       unique in the program; on a cross-module name collision they
//       disambiguate to `u_<module>$<name>`.  `main`, `export fn` and
//       `extern fn` always keep their exact C-ABI names.
//
//   * Project manifest (§6.2): a deliberately small `penstock.toml`
//       (project name, target type, entry modules, link libs, module
//       search dirs, opt level).
//
//   * Test-runner synthesis (§10.3): `penstock test` replaces main with
//       a generated harness that runs every `@test` fn through the
//       frt_test_* runtime (SIGABRT-captured assertions, per-test
//       pass/fail lines, non-zero exit on failure).
#include "common.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <dirent.h>
#include <algorithm>
#include <fstream>
#include <sstream>

namespace freshet {

// ---------------------------------------------------------------- visibility
static std::string g_curMod;
static std::set<std::string> g_curImports;
static bool g_strictVisibility = false;   // true only during semaProgram

static Program* g_merged = nullptr;
static std::map<std::string, int> g_fnNameCount;     // fn name -> # decls
static std::map<std::string, int> g_globNameCount;   // global name -> # decls

void setModuleLookupContext(const std::string& currentModule,
                             const std::set<std::string>& imports) {
  g_curMod = currentModule;
  g_curImports = imports;
  g_strictVisibility = true;
}

// context stack for generic-template instantiation: the template's body
// binds under the TEMPLATE's module, not the caller's
static std::vector<std::pair<std::string, std::set<std::string>>> g_ctxStack;

void pushModuleContext(const std::string& mod) {
  g_ctxStack.push_back({g_curMod, g_curImports});
  std::set<std::string> imps;
  if (g_merged) {
    auto it = g_merged->moduleImports.find(mod);
    if (it != g_merged->moduleImports.end()) imps = it->second;
  }
  g_curMod = mod;
  g_curImports = imps;
}

void popModuleContext() {
  if (g_ctxStack.empty()) return;
  g_curMod = g_ctxStack.back().first;
  g_curImports = g_ctxStack.back().second;
  g_ctxStack.pop_back();
}

static void relaxModuleLookup() {
  g_strictVisibility = false;
}

const std::string& currentLookupModule() { return g_curMod; }

bool moduleLookupIsStrict() { return g_strictVisibility; }

bool moduleVisibleFrom(const std::string& m) {
  if (!g_strictVisibility) return true;              // post-sema passes
  if (m.empty()) return true;                        // prelude
  if (m == g_curMod) return true;                    // own module
  return g_curImports.count(m) > 0;                  // imported module
}

// ---------------------------------------------------------------- file utils
static bool fileReadable(const std::string& p) {
  struct stat st;
  return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

static std::string dirNameOf(const std::string& p) {
  size_t s = p.find_last_of('/');
  return (s == std::string::npos) ? "." : p.substr(0, s);
}

// "src/geometry-utils.fsh" -> "geometry_utils" (a valid module ident)
static std::string fileStemModule(const std::string& p) {
  std::string base = p;
  size_t s = base.find_last_of('/');
  if (s != std::string::npos) base = base.substr(s + 1);
  size_t dot = base.rfind('.');
  if (dot != std::string::npos && dot > 0) base = base.substr(0, dot);
  std::string out;
  for (char c : base)
    out.push_back((isalnum((unsigned char)c) || c == '_') ? c : '_');
  return out.empty() ? "main" : out;
}

static std::string exeDirectory() {
  char buf[4096];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
  if (n <= 0) return ".";
  buf[n] = 0;
  std::string p = buf;
  size_t s = p.find_last_of('/');
  return (s == std::string::npos) ? "." : p.substr(0, s);
}

std::string builtinModulesDir() {
  // source-tree layout: <root>/bin/penstock + <root>/modules/
  // installed layout:  <prefix>/bin/penstock + <prefix>/share/penstock/modules
  static std::string cached;
  if (!cached.empty()) return cached;
  std::string exe = exeDirectory();
  const std::string cands[] = {
    exe + "/../modules",
    exe + "/../share/penstock/modules",
  };
  for (auto& c : cands) {
    if (fileReadable(c + "/fsh.fsh")) { cached = c; return cached; }
  }
  cached = cands[0];
  return cached;
}

// ---------------------------------------------------------------- parsing
Program* parseModuleFile(const std::string& path,
                          std::vector<std::string>& linkLibs) {
  std::string pp;
  std::vector<std::string> ll;
  if (!preprocess(path, pp, ll)) return nullptr;
  for (auto& x : ll)
    if (std::find(linkLibs.begin(), linkLibs.end(), x) == linkLibs.end())
      linkLibs.push_back(x);
  std::vector<Token> toks;
  lexFile(path, pp, toks);
  Program* p = parseTokens(toks, path);
  if (!p) return nullptr;
  if (p->moduleName.empty()) p->moduleName = fileStemModule(path);
  return p;
}

// ---------------------------------------------------------------- loader
namespace {

struct ModuleLoader {
  std::vector<std::string> searchDirs;
  std::map<std::string, Program*> mods;   // module name -> parsed program
  std::vector<Program*> order;            // load order (entry first)
  std::vector<std::string> stack;         // resolution stack (cycle detect)
  std::set<std::string> onStack;

  Program* loadFile(const std::string& path,
                    std::vector<std::string>& linkLibs) {
    Program* p = parseModuleFile(path, linkLibs);
    if (!p || errorCount()) return nullptr;
    auto it = mods.find(p->moduleName);
    if (it != mods.end()) {
      // same module name from two different files
      diag(Diag::Error, Code::Mod,
           "two files declare the same module '" + p->moduleName + "' (" +
           it->second->file + " and " + path + ")",
           path, 0, 0);
      return nullptr;
    }
    mods[p->moduleName] = p;
    order.push_back(p);
    onStack.insert(p->moduleName);
    stack.push_back(p->moduleName);
    for (auto& im : p->imports)
      if (!resolveImport(im.name, im.loc, linkLibs)) return nullptr;
    stack.pop_back();
    onStack.erase(p->moduleName);
    return p;
  }

  Program* resolveImport(const std::string& name, SrcLoc loc,
                          std::vector<std::string>& linkLibs) {
    auto known = mods.find(name);
    if (known != mods.end()) {
      if (onStack.count(name)) {
        std::string chain;
        for (auto& m : stack) chain += m + " -> ";
        diag(Diag::Error, Code::Import,
             "circular import: " + chain + name + " — cycles are not " +
             "allowed (restructure the shared part into its own module)",
             loc.file, loc.line, loc.col);
        return nullptr;
      }
      return known->second;   // already loaded
    }
    for (auto& d : searchDirs) {
      const std::string cands[] = { d + "/" + name + ".fsh",
                                    d + "/" + name + "/mod.fsh" };
      for (auto& c : cands) {
        if (!fileReadable(c)) continue;
        return loadFile(c, linkLibs);
      }
    }
    std::string searched;
    for (auto& d : searchDirs) searched += "\n  " + d + "/" + name + ".fsh";
    diag(Diag::Error, Code::Import,
         "module '" + name + "' not found — searched:" + searched +
         "\n  (add the directory with -I<dir>, or create the module file)",
         loc.file, loc.line, loc.col);
    return nullptr;
  }
};

} // namespace

// shared merge step: tags every decl with its originating module, builds
// the per-module import map, and precomputes IR-name collision counts.
static Program* mergeLoaded(ModuleLoader& L, Program* entry) {
  Program* m = new Program();
  m->file = entry->file;
  m->moduleName = entry->moduleName;
  m->imports = entry->imports;
  for (auto* p : L.order) {
    std::set<std::string> imp;
    for (auto& im : p->imports) imp.insert(im.name);
    m->moduleImports[p->moduleName] = imp;
    for (auto* f : p->fns) { f->module = p->moduleName; m->fns.push_back(f); }
    for (auto* s : p->structs) { s->module = p->moduleName; m->structs.push_back(s); }
    for (auto* e : p->enums) { e->module = p->moduleName; m->enums.push_back(e); }
    for (auto* g : p->globals) { g->module = p->moduleName; m->globals.push_back(g); }
    for (auto& l : p->linkLibs)
      if (std::find(m->linkLibs.begin(), m->linkLibs.end(), l) == m->linkLibs.end())
        m->linkLibs.push_back(l);
  }
  g_fnNameCount.clear();
  for (auto* f : m->fns) g_fnNameCount[f->name]++;
  g_globNameCount.clear();
  for (auto* g : m->globals) g_globNameCount[g->name]++;
  g_merged = m;
  return m;
}

Program* loadProgramWithInputs(const std::vector<std::string>& inputs,
                               std::vector<std::string>& linkLibs) {
  if (inputs.empty()) return nullptr;
  ModuleLoader L;
  // search order (§6.2): entry file dir, -I dirs, builtin modules, cwd
  L.searchDirs.push_back(dirNameOf(inputs[0]));
  for (auto& d : g_opts.includeDirs) L.searchDirs.push_back(d);
  L.searchDirs.push_back(builtinModulesDir());
  L.searchDirs.push_back(".");

  Program* entry = nullptr;
  for (size_t i = 0; i < inputs.size(); ++i) {
    Program* p = L.loadFile(inputs[i], linkLibs);
    if (!p) return nullptr;
    if (i == 0) entry = p;
  }
  if (!entry) return nullptr;
  return mergeLoaded(L, entry);
}

// LSP variant: the entry document's RAW text is analysed (positions stay
// valid — no preprocessing of the entry buffer); imported modules come
// from disk and ARE preprocessed normally.
Program* loadProgramWithImports(const std::string& entryPath,
                                const std::string& entryText,
                                std::vector<std::string>& linkLibs) {
  ModuleLoader L;
  L.searchDirs.push_back(dirNameOf(entryPath));
  for (auto& d : g_opts.includeDirs) L.searchDirs.push_back(d);
  L.searchDirs.push_back(builtinModulesDir());
  L.searchDirs.push_back(".");

  std::vector<Token> toks;
  lexFile(entryPath, entryText, toks);
  Program* entry = parseTokens(toks, entryPath);
  if (!entry) return nullptr;
  if (entry->moduleName.empty()) entry->moduleName = fileStemModule(entryPath);
  L.mods[entry->moduleName] = entry;
  L.order.push_back(entry);
  L.onStack.insert(entry->moduleName);
  L.stack.push_back(entry->moduleName);
  for (auto& im : entry->imports)
    if (!L.resolveImport(im.name, im.loc, linkLibs)) return entry;  // partial
  L.stack.pop_back();
  L.onStack.erase(entry->moduleName);
  return mergeLoaded(L, entry);
}

// sema calls this after semaProgram: post-sema passes get plain lookup
void moduleLookupRelaxHook() { relaxModuleLookup(); }

// ---------------------------------------------------------------- IR symbols
std::string irFnSymbol(FnDecl* f) {
  if (f->name == "main") return "main";
  if (f->exported || f->imported) return f->name;
  auto it = g_fnNameCount.find(f->name);
  if (it != g_fnNameCount.end() && it->second > 1)
    return "u_" + f->module + "$" + f->name;
  return "u_" + f->name;
}

std::string irGlobalSymbol(GlobalVar* g) {
  if (g->exported) return g->name;
  auto it = g_globNameCount.find(g->name);
  if (it != g_globNameCount.end() && it->second > 1)
    return g->module + "$" + g->name;
  return g->name;
}

// ------------------------------------------------------------- manifest (§6.2)
// penstock.toml — deliberately tiny:
//   [project]
//   name = "hello"
//   [target]
//   type = "bin"              # bin | static-lib | shared-lib
//   src  = ["main.fsh"]       # entry modules (imports pull the rest)
//   [build]
//   opt = 3                   # 0..4
//   threads = 0
//   [link]
//   libs = ["m", "curl"]
//   [modules]
//   path = ["lib"]            # extra import search dirs (manifest-relative)
bool loadProjectManifest(const std::string& path) {
  std::ifstream f(path);
  if (!f.is_open()) {
    diag(Diag::Error, Code::Proj, "cannot open project manifest: " + path,
         path, 0, 0);
    return false;
  }
  std::string section;
  std::string lineNo;
  int lineno = 0;
  std::string dir = dirNameOf(path) == "." ? "" : dirNameOf(path) + "/";
  g_opts.projectDir = dir.empty() ? "." : dir.substr(0, dir.size() - 1);
  std::vector<std::string> srcs;
  auto fail = [&](const std::string& msg) {
    diag(Diag::Error, Code::Proj, msg + " (" + path + ":" +
         std::to_string(lineno) + ")", path, lineno, 0);
  };
  std::string line;
  while (std::getline(f, line)) {
    ++lineno;
    // strip comments / whitespace
    size_t h = line.find('#');
    if (h != std::string::npos) line = line.substr(0, h);
    std::string t;
    for (char c : line) if (!isspace((unsigned char)c)) t += c;
    if (t.empty()) continue;
    if (t.front() == '[' && t.back() == ']') {
      section = t.substr(1, t.size() - 2);
      if (section != "project" && section != "target" && section != "build" &&
          section != "link" && section != "modules")
        fail("unknown manifest section [" + section + "]");
      continue;
    }
    size_t eq = t.find('=');
    if (eq == std::string::npos) { fail("expected key = value"); continue; }
    std::string key = t.substr(0, eq);
    std::string val = t.substr(eq + 1);
    auto unquote = [&](std::string& v) {
      if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
        v = v.substr(1, v.size() - 2);
    };
    if (key == "name") {
      unquote(val);
      g_opts.projectName = val;
      continue;
    }
    if (section == "target" && key == "type") {
      unquote(val);
      if (val != "bin" && val != "static-lib" && val != "shared-lib") {
        fail("unknown target type '" + val + "' (bin|static-lib|shared-lib)");
        return false;
      }
      g_opts.projectTarget = val;
      continue;
    }
    if (section == "target" && key == "src") {
      // ["main.fsh", "other.fsh"]
      if (val.front() != '[' || val.back() != ']') {
        fail("src must be a list: src = [\"main.fsh\"]");
        return false;
      }
      std::string item;
      std::stringstream ss(val.substr(1, val.size() - 2));
      while (std::getline(ss, item, ',')) {
        // trim + unquote
        size_t a = item.find_first_not_of(" \t\"");
        size_t b = item.find_last_not_of(" \t\"");
        if (a == std::string::npos) continue;
        srcs.push_back(dir + item.substr(a, b - a + 1));
      }
      continue;
    }
    if (section == "build" && key == "opt") {
      g_opts.optLevel = atoi(val.c_str());
      if (g_opts.optLevel < 0 || g_opts.optLevel > 4)
        fail("opt must be 0..4 — clamped");
      g_opts.optLevel = std::max(0, std::min(4, g_opts.optLevel));
      continue;
    }
    if (section == "build" && key == "threads") {
      g_opts.threads = atoi(val.c_str());
      continue;
    }
    if (section == "link" && key == "libs") {
      if (val.front() != '[') { fail("libs must be a list"); continue; }
      std::string item;
      std::stringstream ss(val.substr(1, val.size() - 2));
      while (std::getline(ss, item, ',')) {
        size_t a = item.find_first_not_of(" \t\"");
        size_t b = item.find_last_not_of(" \t\"");
        if (a == std::string::npos) continue;
        g_opts.projectLinkLibs.push_back(item.substr(a, b - a + 1));
      }
      continue;
    }
    if (section == "modules" && key == "path") {
      if (val.front() != '[') { fail("path must be a list"); continue; }
      std::string item;
      std::stringstream ss(val.substr(1, val.size() - 2));
      while (std::getline(ss, item, ',')) {
        size_t a = item.find_first_not_of(" \t\"");
        size_t b = item.find_last_not_of(" \t\"");
        if (a == std::string::npos) continue;
        g_opts.projectSrcDirs.push_back(dir + item.substr(a, b - a + 1));
      }
      continue;
    }
    fail("unknown manifest key '" + key + "' in [" + section + "]");
  }
  if (srcs.empty()) {
    // default entry: main.fsh beside the manifest
    srcs.push_back(dir + "main.fsh");
  }
  for (auto& s : srcs) {
    if (!fileReadable(s)) {
      diag(Diag::Error, Code::Proj,
           "manifest src file not found: " + s, path, 0, 0);
      return false;
    }
    g_opts.inputs.push_back(s);
  }
  for (auto& d : g_opts.projectSrcDirs)
    g_opts.includeDirs.push_back(d);
  return true;
}

// ------------------------------------------------------- test main (§10.3)
// `penstock test`: replace main with a harness that runs every @test fn:
//   fn main() -> i32 {
//     frt_test_setup();
//     frt_test_run("name", name);   // fn-pointer: the @test fn by name
//     ...
//     return frt_test_summary();
//   }
// frt_test_run sigsetjmp's in ITS OWN frame and calls the test body from
// there — the frame stays live, so an assert() abort inside the test
// long-jumps back well-definedly and is reported as a failure.
static FnDecl* makeExternFn(const std::string& name, Type* ret,
                            std::vector<Param> params, int id) {
  FnDecl* f = new FnDecl();
  f->name = name;
  f->imported = true;
  f->isPub = true;
  f->module = "";    // runtime/prelude module: always visible
  f->ret = ret;
  f->retTyName = "";
  f->params = std::move(params);
  f->id = id;
  return f;
}

static Param strParam(const std::string& n) {
  Param p;
  p.name = n;
  p.ty = Type::simple(Type::STR);
  return p;
}
static Param intParam(const std::string& n, Type* t) {
  Param p;
  p.name = n;
  p.ty = t;
  return p;
}

bool synthesizeTestMain(Program* p) {
  std::vector<FnDecl*> tests;
  for (auto* f : p->fns) {
    if (!f->isTest) continue;
    if (f->isGeneric) {
      diag(Diag::Error, Code::Test,
           "@test functions cannot be generic: " + f->name,
           f->loc.file, f->loc.line, f->loc.col);
      return false;
    }
    if (f->imported) {
      diag(Diag::Error, Code::Test,
           "@test functions must have a body: " + f->name,
           f->loc.file, f->loc.line, f->loc.col);
      return false;
    }
    if (!f->params.empty()) {
      diag(Diag::Error, Code::Test,
           "@test functions take no parameters: " + f->name,
           f->loc.file, f->loc.line, f->loc.col);
      return false;
    }
    if (f->retTyName != "" && f->retTyName != "void") {
      diag(Diag::Error, Code::Test,
           "@test functions must return void (assertions signal failure): " +
           f->name,
           f->loc.file, f->loc.line, f->loc.col);
      return false;
    }
    tests.push_back(f);
  }
  if (tests.empty()) {
    diag(Diag::Error, Code::Test,
         "no @test functions found — mark zero-arg void fns with @test",
         p->file, 0, 0);
    return false;
  }

  // extern runtime decls for the harness.  frt_test_run's second
  // parameter is the fn-pointer type fn() -> void — the Freshet @test fn
  // is passed by name and coerces to it (Phase 3b feature).
  int nextId = (int)p->fns.size();
  auto externs = std::vector<FnDecl*>{
    makeExternFn("frt_test_setup", Type::simple(Type::VOID), {}, nextId++),
    makeExternFn("frt_test_run", Type::simple(Type::I32),
                 {strParam("name"), intParam("f", Type::fnptrOf({}, Type::simple(Type::VOID)))},
                 nextId++),
    makeExternFn("frt_test_summary", Type::simple(Type::I32), {}, nextId++),
  };

  // build the harness body
  SrcLoc here{p->file, 1, 1, 0, 0, 1, 1};
  FnDecl* main = new FnDecl();
  main->name = "main";
  main->module = p->moduleName;
  main->isPub = true;
  main->ret = Type::simple(Type::I32);
  main->retTyName = "i32";
  main->loc = here;
  main->id = nextId++;

  Stmt* body = new Stmt(Stmt::BLOCK, here);
  body->fn = main;
  main->body = body;

  auto mkExprStmt = [&](Expr* e) {
    Stmt* s = new Stmt(Stmt::EXPR, here);
    s->fn = main;
    s->expr = e;
    return s;
  };
  auto mkCall = [&](const std::string& n, std::vector<Expr*> args) {
    Expr* c = new Expr(Expr::CALL, here);
    c->name = n;
    c->argGive.assign(args.size(), false);   // keep vectors aligned
    c->args = std::move(args);
    return c;
  };
  auto mkStr = [&](const std::string& s) {
    Expr* e = new Expr(Expr::STR, here);
    e->sval = s;
    e->constant = true;
    return e;
  };
  auto mkFnRef = [&](FnDecl* f) {
    Expr* e = new Expr(Expr::IDENT, here);
    e->name = f->name;
    e->constant = true;
    return e;
  };

  body->stmts.push_back(mkExprStmt(mkCall("frt_test_setup", {})));
  for (auto* t : tests)
    body->stmts.push_back(
        mkExprStmt(mkCall("frt_test_run", {mkStr(t->name), mkFnRef(t)})));
  Stmt* ret = new Stmt(Stmt::RETURN, here);
  ret->fn = main;
  ret->retVal = mkCall("frt_test_summary", {});
  body->stmts.push_back(ret);

  // swap out every user main; the harness is the new entry point
  std::vector<FnDecl*> kept;
  for (auto* f : p->fns)
    if (f->name != "main" || f->isTest) kept.push_back(f);
  kept.push_back(main);
  for (auto* e : externs) kept.push_back(e);
  p->fns = std::move(kept);
  return true;
}

} // namespace freshet
