// Freshet re-entrant frontend library — implementation (Stage E, §4.4).
//
// This file is intentionally conservative: it does NOT move every
// process-global off the historical globals in one shot (that would be
// a multi-thousand-line change touching every src/*.cpp).  Instead it
// implements CompileSession as a swap-and-restore guard around the
// existing pipeline, which is enough to:
//
//   - make analyseDocument() safe to call from a language server one
//     document at a time,
//   - make unit tests fully isolated (no diagnostic pollution between
//     cases), and
//   - serve as the seam future refactors will plug into.
//
// The swap-and-restore pattern works because every diagnostic flows
// through `diag()`, which writes to `g_diags`.  By snapshotting g_diags
// on entry, clearing it, running the pipeline, then collecting the
// freshly-emitted diags into a local vector, we get a per-call
// diagnostic stream without touching every diag() call site.
#include "session.h"
#include "irgen.h"
#include <filesystem>
#include <cctype>

namespace freshet {

SymbolIndex buildSymbolIndex(Program* p) {
  SymbolIndex idx;
  if (!p) return idx;
  for (auto* f : p->fns) {
    SymbolIndex::Entry e;
    e.name = f->name;
    e.kind = SymbolIndex::Entry::FN;
    e.loc = f->loc;
    // signature: (a: T, b: T) -> R
    std::string sig = "(";
    for (size_t i = 0; i < f->params.size(); ++i) {
      if (i) sig += ", ";
      sig += f->params[i].name + ": " + typeToString(f->params[i].ty);
    }
    sig += ") -> " + typeToString(f->ret);
    if (f->strictfp)  sig += " @strictfp";
    if (f->userPure)  sig += " @pure";
    if (f->exported)  sig += " export";
    if (f->userInline)    sig += " @inline";
    if (f->userNoinline)  sig += " @noinline";
    e.detail = sig;
    idx.entries.push_back(e);
  }
  for (auto* g : p->globals) {
    SymbolIndex::Entry e;
    e.name = g->name;
    e.kind = SymbolIndex::Entry::GLOBAL;
    e.loc = g->loc;
    e.detail = typeToString(g->ty) + (g->mut ? " mut" : "");
    if (g->exported) e.detail += " export";
    idx.entries.push_back(e);
  }
  for (auto* sd : p->structs) {
    SymbolIndex::Entry e;
    e.name = sd->name;
    e.kind = SymbolIndex::Entry::STRUCT;
    e.loc = sd->loc;
    e.detail = "struct " + sd->name + " { " +
               std::to_string(sd->fields.size()) + " fields, " +
               std::to_string(sd->size) + "B }";
    idx.entries.push_back(e);
  }
  return idx;
}

TypeInfo buildTypeInfo(Program* p) {
  TypeInfo ti;
  if (!p) return ti;
  for (auto* sd : p->structs) {
    ti.structs.push_back({sd->name, sd->size, sd->align, sd->padBytes, sd->loc});
  }
  return ti;
}

// RAII swap-and-restore for the process-global state.  Only fields that
// affect lex/parse/sema/checks are saved — emit-IR / binary paths are
// not used by the LSP MVP.
namespace {
// The language server analyses individual editor buffers rather than a
// whole executable target.  Only direct children of the project's `src/`
// directory whose basename contains `main` as a standalone filename token
// are treated as entry-point files.  In particular, src/pkg/foo_main.fsh
// is a reusable child module and must not require fn main().
bool lspFileRequiresMain(const std::string& filePath) {
  namespace fs = std::filesystem;
  const fs::path path = fs::path(filePath).lexically_normal();
  if (path.extension() != ".fsh" || path.parent_path().filename() != "src")
    return false;

  std::string stem = path.stem().string();
  for (char& ch : stem) ch = (char)std::tolower((unsigned char)ch);
  size_t at = stem.find("main");
  while (at != std::string::npos) {
    const bool leftBoundary = at == 0 ||
        !std::isalnum((unsigned char)stem[at - 1]);
    const size_t after = at + 4;
    const bool rightBoundary = after == stem.size() ||
        !std::isalnum((unsigned char)stem[after]);
    if (leftBoundary && rightBoundary) return true;
    at = stem.find("main", at + 1);
  }
  return false;
}

struct GlobalSwap {
  std::vector<Diag> savedDiags;
  Options savedOpts;
  std::string savedBackend;
  GlobalSwap() : savedOpts(g_opts), savedBackend(g_backend) {
    savedDiags.swap(g_diags);
    g_diags.clear();
  }
  ~GlobalSwap() {
    g_diags.swap(savedDiags);
    g_opts = savedOpts;
    g_backend = savedBackend;
  }
};
} // namespace

DocumentAnalysis analyseDocument(const WorkspaceConfig& config,
                                 const std::string& filePath,
                                 const std::string& source,
                                 const AnalysisOptions& options) {
  DocumentAnalysis result;
  GlobalSwap swap;  // isolate g_diags / g_opts for this call

  // Map WorkspaceConfig onto the global Options struct that the
  // existing pipeline reads.  After the larger refactor, WorkspaceConfig
  // will be passed through every function instead.
  g_opts.input = filePath;
  g_opts.includeDirs = config.includeDirs;
  g_opts.soaAuto = config.soaAuto;
  g_opts.noLoopOpts = config.noLoopOpts;
  g_opts.optLevel = config.optLevel;
  // LSP analysis should never invoke the backend or write files:
  g_opts.irOut.clear();
  g_opts.emitObj = false;
  g_opts.run = false;
  g_opts.report = false;
  g_opts.keepIr = false;

  // ----- 1-2. modules: parse the RAW entry text (positions stay valid),
  // resolve its `import`s from disk and merge (Phase 3a, report §6.1).
  // The loader preprocesses only the IMPORTED files — the entry document
  // is lexed as-is so every diagnostic/hover position maps to the buffer
  // the editor holds.
  Program* prog = nullptr;
  {
    std::vector<std::string> linkLibsUnused;
    prog = loadProgramWithImports(filePath, source, linkLibsUnused);
  }
  // Even on parse error, parseTokens returns a (partial) Program so the
  // LSP can still navigate symbols / offer completions for the parts
  // that did parse.  Diagnostics capture what failed.
  if (!prog) {
    result.diagnostics = std::move(g_diags);
    return result;
  }
  // Wrap the raw Program* in a shared_ptr with no-op deleter — the
  // parser allocates from raw `new` and the existing CLI does not free
  // it (process exits after one compilation).  For the LSP we will need
  // a proper arena allocator; for the MVP, leaking the AST across
  // document versions is acceptable and matches the CLI's lifetime.
  result.program = std::shared_ptr<Program>(prog, [](Program*) {});

  // ----- 3. sema
  if (errorCount() == 0) {
    // Normal compiler invocations retain the default required-main policy;
    // the LSP only expects an entry point in src/<name containing `main`>.
    semaProgram(prog, lspFileRequiresMain(filePath));
  }

  // ----- 4. strict checks (always run in LSP mode — they're how the
  // editor surfaces F0402 / F0305 / F0408 / F0407 / etc.)
  if (options.runStrictChecks && errorCount() == 0) {
    runStrictChecks(prog);
    runParforChecks(prog);
    if (options.runOwnershipAnalysis) {
      runOwnershipAnalysis(prog);
    }
  }

  // ----- 5. optional source-level loop passes (off by default in LSP
  // mode — the LSP shows the *user's* code, not the compiler's rewrite)
  if (options.runLoopOptPasses && g_opts.optLevel >= 2 &&
      !g_opts.noLoopOpts && errorCount() == 0) {
    runFuseUnrollPasses(prog);
    runLaneAnalysisPass(prog);
  }

  // ----- 6. optional IR emission (off by default; useful for an LSP
  // feature that wants to surface the LLVM IR of the function under the
  // cursor).  Not in the MVP scope, but the seam is here.
  (void)options.emitLLVMIR;

  result.diagnostics = std::move(g_diags);
  result.symbols = buildSymbolIndex(prog);
  result.types = buildTypeInfo(prog);
  return result;
}

} // namespace freshet
