// Freshet re-entrant frontend library API (Stage E, report §3.1 + §4.4).
//
// The historical frontend had process-global state: g_diags, g_opts,
// g_backend, the global IrGen* G, g_prog, etc.  That made it unsafe to
// call from a language server (multiple open documents, concurrent
// analysis, cancellation) and hard to unit-test (a failing test polluted
// the next one).
//
// This header introduces:
//   - `CompileSession` — an explicit context bundling diagnostics, options
//     and (eventually) source-manager / AST / symbol-table state.
//   - `DocumentAnalysis` — the structured result of analysing one document
//     (program AST, diagnostics, symbol index, type info).
//   - `analyseDocument()` — the re-entrant entry point that both the
//     `penstock` CLI and the `freshet-language-server` will eventually
//     share.
//
// For now (Stage E, MVP scope), CompileSession is a thin wrapper: it
// swaps g_diags / g_opts / g_backend to a local copy, runs the existing
// pipeline, and restores the originals on exit.  This is enough to make
// the LSP safe for a single open document at a time; full per-document
// isolation requires moving g_prog and the Sema registries off
// process-globals, which is a larger refactor tracked as a separate
// follow-up.
#pragma once
#include "common.h"
#include <memory>
#include <string>
#include <vector>

namespace freshet {

// Workspace + per-document configuration.  Mirrors the relevant subset
// of `Options` (driver flags) without the file-system / binary-emission
// fields that don't apply to in-memory analysis.
struct WorkspaceConfig {
  std::vector<std::string> includeDirs;
  bool soaAuto = false;
  bool noLoopOpts = false;
  int optLevel = 3;
};

struct AnalysisOptions {
  bool runStrictChecks = true;     // F0402, F0403, F0305, F0408, ...
  bool runOwnershipAnalysis = true;
  bool runLoopOptPasses = false;   // -O2+ source-level fusion/unroll/lane
  bool emitLLVMIR = false;         // false = AST+diagnostics only (LSP mode)
};

// Structured result of analysing one document.  Owned by the caller.
// `program` is null when lexing or parsing fails catastrophically; the
// diagnostics vector still contains everything the LSP needs to render.
struct SymbolIndex {
  // (Stub for Stage E MVP) — populated with function names + locations.
  // Will grow to cover globals, structs, fields, locals per-function as
  // the LSP feature surface expands.
  struct Entry {
    std::string name;
    enum Kind { FN, GLOBAL, STRUCT } kind;
    SrcLoc loc;
    std::string detail;   // signature / type string for hover
  };
  std::vector<Entry> entries;
};

struct TypeInfo {
  // (Stub for Stage E MVP) — populated with struct layout info that
  // the LSP inlay-hints layer will surface (size, alignment, padding).
  struct StructInfo {
    std::string name;
    uint64_t size, align, padBytes;
    SrcLoc loc;
  };
  std::vector<StructInfo> structs;
};

struct DocumentAnalysis {
  std::shared_ptr<Program> program;
  std::vector<Diag> diagnostics;
  SymbolIndex symbols;
  TypeInfo types;
};

// Re-entrant entry point.  `source` is the post-preprocessor text (the
// CLI's `preprocess()` step); for the LSP, the caller is responsible for
// resolving #includes against the workspace and producing the same
// shape of preprocessed text the CLI would.
//
// On return, `out->diagnostics` contains every diagnostic emitted
// during lex/parse/sema/checks (no printing).  `out->program` is null
// only if parsing produced no usable AST.
//
// Cancellation: this synchronous version does not honour a cancel
// token; a future async variant will.  For now the LSP should call
// analyseDocument() on a worker thread and discard the result if the
// document version has advanced.
DocumentAnalysis analyseDocument(const WorkspaceConfig& config,
                                 const std::string& filePath,
                                 const std::string& source,
                                 const AnalysisOptions& options);

// Helper: build a SymbolIndex from a parsed Program.  Used by both
// analyseDocument() and (eventually) a workspace-wide indexer.
SymbolIndex buildSymbolIndex(Program* p);

// Helper: build a TypeInfo from a parsed Program (struct layouts).
TypeInfo buildTypeInfo(Program* p);

} // namespace freshet
