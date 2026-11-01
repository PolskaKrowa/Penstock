// Freshet compiler — diagnostic registry: the single source of truth for what
// a diagnostic IS, independent of how severe it is on a given run.
//
//   X(Name, "Fnnnn", "slug", "one-line title", isRemark)
//
// Rules (see docs/errors/):
//   * The ID is the stable public identity: it is printed in the terminal,
//     carried in LSP/report output, and is the file name of its documentation
//     page (docs/errors/<ID>.md).  `penstock --explain <ID|slug>` shows it.
//   * IDs are explicit and NEVER reused or renumbered.  A retired diagnostic
//     keeps its row (and page); a diagnostic that is split gets NEW ids.
//   * Severity is deliberately NOT stored here.  It is decided where the
//     diagnostic is raised (e.g. --strict-performance, or a stride threshold),
//     so the same ID can be a warning on one run and an error on another.
//   * The hundreds digit groups IDs by area for humans only; never infer
//     meaning from it.  F00xx driver/project, F01xx syntax, F02xx semantics,
//     F03xx safety (ownership/aliasing/purity/parfor), F04xx performance,
//     F09xx optimisation remarks (isRemark = true; compact in the terminal).
#pragma once
#include <cstddef>
#include <string>

namespace freshet {

#define FRESHET_DIAGNOSTICS(X) \
  /* ---- F00xx driver / project --------------------------------------- */ \
  X(Io,               "F0001", "io",                  "file could not be read or written", false) \
  X(Proj,             "F0002", "proj",                "project manifest error", false) \
  X(Mod,              "F0003", "mod",                 "module declaration error", false) \
  X(Import,           "F0004", "import",              "import could not be resolved or is not visible", false) \
  X(Test,             "F0005", "test",                "invalid @test function", false) \
  X(Fmt,              "F0006", "fmt",                 "formatter refused to rewrite the file", false) \
  X(Pgo,              "F0007", "pgo",                 "profile data could not be used", false) \
  X(Ir,               "F0008", "ir",                  "internal error while generating IR", false) \
  /* ---- F01xx syntax -------------------------------------------------- */ \
  X(Lex,              "F0101", "lex",                 "unexpected character", false) \
  X(Parse,            "F0102", "parse",               "syntax error", false) \
  X(Preproc,          "F0103", "preproc",             "preprocessor error", false) \
  X(PreprocDirective, "F0104", "preproc-directive",   "unknown preprocessor directive", false) \
  X(Attr,             "F0105", "attr",                "unknown or conflicting attribute", false) \
  X(Parfor,           "F0106", "parfor",              "invalid parfor syntax or clause", false) \
  /* ---- F02xx semantics ----------------------------------------------- */ \
  X(Type,             "F0201", "type",                "type error", false) \
  X(Args,             "F0202", "args",                "wrong arguments to a call or builtin", false) \
  X(Generic,          "F0203", "generic",             "generic instantiation error", false) \
  X(Match,            "F0204", "match",               "invalid match expression", false) \
  X(Dup,              "F0205", "dup",                 "duplicate definition", false) \
  X(Main,             "F0206", "main",                "no entry point", false) \
  X(Global,           "F0207", "global",              "global initializer must be constant", false) \
  X(Mut,              "F0208", "mut",                 "assignment through an immutable binding", false) \
  X(Ret,              "F0209", "ret",                 "missing or invalid return", false) \
  X(RetMainImplicit,  "F0210", "ret-main-implicit",   "main returns implicitly", false) \
  X(CastNarrowing,    "F0211", "cast-narrowing",      "narrowing numeric cast", false) \
  X(Bound,            "F0212", "bound",               "constant index out of bounds", false) \
  X(LoopBreakOutside, "F0213", "loop-break-outside",  "break or continue outside a loop", false) \
  X(ArithDivZero,     "F0214", "arith-div-zero",      "division by zero", false) \
  X(ArithNegUnsigned, "F0215", "arith-neg-unsigned",  "negation of an unsigned value", false) \
  /* ---- F03xx safety -------------------------------------------------- */ \
  X(Own,              "F0301", "own",                 "ownership violation", false) \
  X(OwnSuspicious,    "F0302", "own-suspicious",      "suspicious ownership operation", false) \
  X(OwnCall,          "F0303", "own-call",            "argument passing mode mismatch", false) \
  X(Pure,             "F0304", "pure",                "@pure function has side effects", false) \
  X(AliasViolation,   "F0305", "alias-violation",     "aliased arguments passed to noalias parameters", false) \
  X(AliasLie,         "F0306", "alias-lie",           "call breaks a function's @alias no-overlap promise", false) \
  X(Thread,           "F0307", "thread",              "unsafe construct inside a parfor body", false) \
  X(RedAddInit,       "F0308", "red-add-init",        "reduction(+) not initialized to 0", false) \
  X(RedMulInit,       "F0309", "red-mul-init",        "reduction(*) not initialized to 1", false) \
  /* ---- F04xx performance --------------------------------------------- */ \
  X(ArithDivConst,    "F0401", "arith-div-const",     "division by a non-power-of-2 constant", false) \
  X(ArithDivRuntime,  "F0402", "arith-div-runtime",   "division by a runtime value", false) \
  X(CacheStride,      "F0403", "cache-stride",        "strided access wastes cache lines", false) \
  X(CacheLineStraddle,"F0404", "cache-line-straddle", "hot field straddles a cache line", false) \
  X(LoopStride,       "F0405", "loop-stride",         "for-loop stride is not constant 1", false) \
  X(LoopIvWritten,    "F0406", "loop-iv-written",     "loop induction variable modified in the body", false) \
  X(LayoutPad,        "F0407", "layout-pad",          "struct layout wastes bytes on padding", false) \
  X(Soa,              "F0408", "soa",                 "array-of-structs access would benefit from SoA layout", false) \
  /* ---- F09xx optimisation remarks ------------------------------------ */ \
  X(Lane,             "F0901", "lane",                "cross-lane optimisation applied", true) \
  X(LaneSkip,         "F0902", "lane-skip",           "cross-lane optimisation skipped", true) \
  X(SoaAuto,          "F0903", "soa-auto",            "automatic AoS-to-SoA conversion", true) \
  X(SoaAosoa,         "F0904", "soa-aosoa",           "AoSoA layout cost report", true) \
  X(ParAdvise,        "F0905", "par-advise",          "loop appears safe to parallelise", true) \
  X(NestedWs,         "F0906", "nested-ws",           "nested parfor inner chunks become stealable tasks", true) \
  X(Collapse,         "F0907", "collapse",            "rectangular nested parfor collapsed", true) \
  X(Fuse,             "F0908", "fuse",                "adjacent loops fused", true) \
  X(Unroll,           "F0909", "unroll",              "constant-trip loop fully unrolled", true) \
  X(PartialUnroll,    "F0910", "partial-unroll",      "loop partially unrolled", true) \
  X(Peel,             "F0911", "peel",                "first loop iteration peeled", true) \
  X(Licm,             "F0912", "licm",                "loop-invariant binding hoisted", true) \
  X(Interchange,      "F0913", "interchange",         "loop nest interchanged", true) \
  X(Tile,             "F0914", "tile",                "loop nest tiled", true) \
  X(Distribute,       "F0915", "distribute",          "loop distributed", true) \
  X(UnrollJam,        "F0916", "unroll-jam",          "loop nest unrolled and jammed", true) \
  X(ConstFold,        "F0917", "const-fold",          "constants folded at exact source width", true)

enum class Code {
#define X(name, id, slug, title, remark) name,
  FRESHET_DIAGNOSTICS(X)
#undef X
};

struct CodeInfo { const char* id; const char* slug; const char* title; bool remark; };

inline constexpr CodeInfo kCodes[] = {
#define X(name, id, slug, title, remark) {id, slug, title, remark},
  FRESHET_DIAGNOSTICS(X)
#undef X
};
inline constexpr size_t kNumCodes = sizeof(kCodes) / sizeof(kCodes[0]);

constexpr bool sameStr(const char* a, const char* b) {
  while (*a && *a == *b) { ++a; ++b; }
  return *a == *b;
}
constexpr bool registryIsUnique() {
  for (size_t i = 0; i < kNumCodes; ++i)
    for (size_t j = 0; j < i; ++j)
      if (sameStr(kCodes[i].id, kCodes[j].id) || sameStr(kCodes[i].slug, kCodes[j].slug)) return false;
  return true;
}
static_assert(registryIsUnique(), "duplicate diagnostic id or slug in FRESHET_DIAGNOSTICS");

inline const CodeInfo& info(Code c) { return kCodes[(size_t)c]; }

// Look up by printed ID ("F0402", any case) or slug ("arith-div-runtime").
inline const CodeInfo* findCode(std::string key) {
  if (!key.empty() && key[0] == 'f') key[0] = 'F';
  for (auto& c : kCodes) if (key == c.id || key == c.slug) return &c;
  return nullptr;
}

}  // namespace freshet
