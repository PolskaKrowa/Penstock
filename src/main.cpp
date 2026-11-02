// Penstock — the Freshet compiler driver.
//   penstock [options] file.fsh
// pipeline: preprocess -> lex -> parse -> sema -> strict checks -> ownership
//           -> irgen (.ll) -> [+ runtime.ll] -> clang -O3 -march -> binary
// R2 additions: --profile / --use-profile (PGO), soa support, nested parfor.
#include "irgen.h"
#include "loop_analysis.h"
#include "loop_transforms.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <algorithm>
#include <fstream>

using namespace freshet;

static const char* USAGE =
    "Penstock " FRESHET_VERSION " — Freshet compiler\n"
    "usage: penstock [options] input.fsh [more.fsh ...]\n"
    "       penstock <subcommand> [args...]\n"
    "\n"
    "subcommands (Phase 3/4, report §6/§10):\n"
    "  build [--force] [dir]  compile the project in <dir> (penstock.toml;\n"
    "                    default .).  --force ignores the dependency\n"
    "                    cache and recompiles.\n"
    "  test <file.fsh>    run every @test fn in the file (report §10.3):\n"
    "                    per-test pass/fail lines, non-zero exit on failure\n"
    "  fmt <file.fsh>     format source in place (report §10.2)\n"
    "  fmt --check <f>    exit 1 when the file is not already formatted\n"
    "  lint <file.fsh>    diagnostics-only run (report §10.2)\n"
    "  run <file.fsh>     compile and immediately execute\n"
    "\n"
    "multi-file compilation (report §6.2): every .fsh input is a module\n"
    "of one program; `import geometry;` pulls a module's pub decls from\n"
    "<dir>/geometry.fsh (search: entry dir, -I dirs, builtin modules, cwd).\n"
    "\n"
    "  -o <file>          output file (default a.out; with -S: .ll, with -c: .o)\n"
    "  -S                 emit LLVM IR only (.ll)\n"
    "  -c                 emit object file (.o) only\n"
    "  -O<0..4>           optimization level (default 3; 4 = wider SAT/SMT\n"
    "                    superoptimisation scope)\n"
    "  --superopt-budget=<s> solver budget (default 2s -O3, 10s -O4)\n"
    "  --no-superopt      disable embedded Freshet/Z3/Alive2 verified IR rewriting\n"
    "  --no-precompute    disable finite-domain mapping / algorithm rewriting\n"
    "  --evo-runtime-tune opt into the legacy execute-and-time -O4 tuner\n"
    "  -march=<target>    CPU target (default native)\n"
    "  --keep-ir          keep intermediate .ll next to the output\n"
    "  --ir-out=<file>    write IR to <file> and stop (like -S)\n"
    "  --run              run the produced binary (passes through argv)\n"
    "  --threads=<n>      default parfor thread count (0 = all cores)\n"
    "  --evo-budget=<s>   -O4: evolution time budget in seconds (default 10)\n"
    "  --evo-seed=<n>     -O4: evolution RNG seed (default time-derived)\n"
    "  --report           performance report mode (also runs binary w/ timing)\n"
    "  --profile          PGO: build instrumented binary (writes <exe>.fprof)\n"
    "  --use-profile=<f>  PGO: guide optimization with profile <f>\n"
    "  --soa=<mode>       AoS->SoA: warn (default) | auto (transform hot arrays)\n"
    "                    | aosoa (advisory: frequency-weighted layout report)\n"
    "  --no-loop-opts     disable loop fusion/unrolling/cross-lane passes\n"
    "\n"
    "  Optimisation-audit pass flags (opt-in; --no-* = differential hatch):\n"
    "  --licm             hoist invariant pure lets out of range loops\n"
    "  --partial-unroll   constant-trip unroll by 2/4/8 + exact remainder\n"
    "  --peel             peel first iteration, resolve its guard statically\n"
    "  --interchange      swap perfect rectangular nests (stride-driven)\n"
    "  --tile[=I[,J]]     2-D blocking with imin-clamped boundary tiles\n"
    "  --distribute       fission independent statement groups\n"
    "  --unroll-and-jam   outer x2 + jammed inner bodies\n"
    "  --parfor-advisor   report loops that appear parallel-safe (no rewrite)\n"
    "  --proof-log[=<f>]  per-transform legality-premise log (<out>.prooflog)\n"
    "  --superopt-objective=<o>  latency | size | balanced (default latency)\n"
    "  --target-cache-line= / --target-l1= / --target-l2= /\n"
    "  --target-vector-width=    profitability-model target overrides\n"
    "  --strict-performance  treat performance diagnostics (F0401-F0406: the\n"
    "                      arith-div-*, cache-* and loop-* family) as hard errors\n"
    "                      — the HPC mode\n"
    "                      (default: they are warnings; report §7)\n"
    "  --fast-math         opt the whole build into relaxed FP semantics\n"
    "                      (default: strict IEEE-754-like; report §8 Option A)\n"
    "  --checked           bounds-check Span/array indexing (also on at -O0/-O1;\n"
    "                      --no-bounds-check disables everywhere; report §4.1)\n"
    "\n"
    "  Stage A (optimisation observability):\n"
    "  --inline=<p>       inlining policy: default|conservative|off (Stage A1)\n"
    "                    default lets LLVM decide (NO blanket noinline)\n"
    "                    off reproduces the historical noinline-everything\n"
    "                    baseline for differential benchmarking.\n"
    "                    Per-fn @inline / @noinline attributes always win.\n"
    "  --emit-combined-ir[=<f>]  write the user+runtime IR module Clang sees\n"
    "                    (Stage A2 — fixes the -S gap from the report)\n"
    "  --save-temps       keep every intermediate (pp, .ll, opt-remarks YAML)\n"
    "  --opt-remarks[=c]  pass -Rpass*/-fsave-optimization-record to Clang\n"
    "                    categories: vectorize|inline|all (default all)\n"
    "  --legacy-S         -S writes only user IR (pre-report behaviour)\n"
    "\n"
    "  -I<dir>            add include search dir\n"
    "  --version          print version\n"
    "  -h | --help        print this help\n"
    "  --explain <code>   describe a diagnostic by ID or slug (e.g. F0402)\n"
    "\n"
    "  Audit integration (freshet_penstock_optimization_report.md):\n"
    "  --runtime-opt=<n>   cached runtime IR optimisation level (default 2;\n"
    "                    0 = historical -O0 debug runtime).  §3\n"
    "  --force-vectorize   restore the historical blanket vectorize.enable +\n"
    "                    interleave 4 loop hints (differential baseline).  §5\n"
    "  --veclib=<v>       vector math library: none (default) | libmvec\n"
    "                    (adds -fveclib=libmvec -fno-math-errno; fast-FP\n"
    "                    programs only).  §8\n"
    "  --llvm-pgo-generate LLVM IR PGO training build (-fprofile-generate;\n"
    "                    run with LLVM_PROFILE_FILE=<f>.profraw).  §11.2\n"
    "  --llvm-pgo-use=<f> LLVM IR PGO reuse (.profraw/.profdata/dir/list;\n"
    "                    profraw inputs are merged via llvm-profdata)\n"
    "  --llvm-profdata=<e> override the llvm-profdata executable path\n"
    "  --emit-final-ir[=<f>] write the final OPTIMISED IR (<out>.final.ll)\n"
    "                    the backend produced, for inspection.  §13.1\n"
    "  --target=<triple>  cross-target the whole pipeline (triple + data\n"
    "                    layout are queried from the backend).  §12\n"
    "  --portable          portable baseline build: generic x86-64, no\n"
    "                    -march=native.  §12\n"
    "\n"
    "  Phase 3/4 flags:\n"
    "  -g / --debug      emit DWARF source-level debug info (report §10.3):\n"
    "                    line tables + fn symbols — `gdb <out>` gives source\n"
    "                    locations and backtraces\n"
    "  --project <f>     explicit project manifest (default: penstock.toml in\n"
    "                    the entry directory)\n";

// run a command, return exit code; stderr passes through
static int runCmd(const std::string& cmd) {
  fflush(stdout);
  int rc = system(cmd.c_str());
  return WEXITSTATUS(rc);
}

// Stage A2: build the Clang `-Rpass*` / `-fsave-optimization-record` argument
// string corresponding to `g_opts.optRemarks`.  Returns "" when --opt-remarks
// was not requested.  Clang's optimisation-remark flags are:
//   -Rpass=<regex>          remarks for transformations that succeed
//   -Rpass-missed=<regex>   remarks for transformations that failed
//   -Rpass-analysis=<regex>  analysis remarks (alias info, IV, etc.)
//   -fsave-optimization-record=<format>  machine-readable record file
// We scope by category so a vectorisation-focused investigation doesn't have
// to wade through every inline remark in the module.  The user picks:
//   --opt-remarks               -> all categories
//   --opt-remarks=vectorize     -> only loop/SLP vectorisation
//   --opt-remarks=inline        -> only inlining decisions
//   --opt-remarks=vectorize,inline  -> both
// The YAML record file is always named <out>.opt-records.yaml when enabled.
static std::string optRemarksFlags(const std::string& outPath) {
  if (!g_opts.optRemarksEnabled) return "";
  std::string cats = g_opts.optRemarks.empty() ? "all" : g_opts.optRemarks;
  std::string flags;
  auto has = [&](const char* k) {
    return cats == "all" || cats.find(k) != std::string::npos;
  };
  // The Clang regex argument is matched against the pass NAME; for our
  // purposes the user-visible strings are "loop-vectorize", "slp-vectorizer",
  // "inline".  An empty regex ("") means "all passes".
  auto addRpass = [&](const char* kind, const char* regex) {
    flags += " -Rpass";
    if (kind && *kind) { flags += "-"; flags += kind; }
    flags += "='";
    flags += regex;
    flags += "'";
  };
  if (has("vectorize")) {
    addRpass("missed",   "loop-vectorize|slp-vectorizer");
    addRpass("",         "loop-vectorize|slp-vectorizer");
    addRpass("analysis", "loop-vectorize|slp-vectorizer");
  }
  if (has("inline")) {
    addRpass("missed",   "inline|cgscc-inline|always-inline");
    addRpass("",         "inline|cgscc-inline|always-inline");
    addRpass("analysis", "inline");
  }
  if (cats == "all") {
    // broad: every pass, no regex
    flags = " -Rpass-missed -Rpass -Rpass-analysis";
  }
  // Always drop a YAML record file alongside the binary so CI / IDE tools
  // can post-process without scraping stderr.
  flags += " -fsave-optimization-record -foptimization-record-file=" + outPath + ".opt-records.yaml";
  return flags;
}

static std::string readFile(const std::string& p) {
  FILE* f = fopen(p.c_str(), "rb");
  if (!f) return "";
  std::string s;
  char buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
  fclose(f);
  return s;
}

static bool writeFile(const std::string& p, const std::string& s) {
  FILE* f = fopen(p.c_str(), "wb");
  if (!f) return false;
  fwrite(s.data(), 1, s.size(), f);
  fclose(f);
  return true;
}

static std::string exeDir() {
  static std::string d;
  if (d.empty()) {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return ".";
    buf[n] = 0;
    std::string s(buf);
    size_t slash = s.rfind('/');
    d = slash == std::string::npos ? "." : s.substr(0, slash);
  }
  return d;
}

// ---------------- runtime .ll production (cached per LLVM major) ------------
// The cache key includes the LLVM major version: a runtime .ll compiled by
// one LLVM must not be reused after switching toolchains (auto-upgraded IR
// carries version-specific intrinsic/attribute syntax).
static std::string runtimeLl(const std::string& clangBin,
                             const std::string& cacheDir,
                             const std::string& rtSrc,
                             const std::string& llvmMajor) {
  struct stat st;
  time_t srcMtime = stat(rtSrc.c_str(), &st) == 0 ? st.st_mtime : 0;
  // §3: the cache key now encodes the LLVM major AND the runtime
  // optimisation level, so an -O2 runtime can never be reused when
  // --runtime-opt=0 (debug) is requested or vice versa.  The historical
  // key was LLVM-major only, which baked -O0's optnone/noinline into
  // every cached runtime and blocked the final -O3 pipeline from
  // optimising or inlining the runtime's hot paths.
  std::string cache = cacheDir + "/frt_rt" +
      (llvmMajor.empty() ? "" : "_" + llvmMajor) +
      "_o" + std::to_string(g_opts.runtimeOpt) + ".ll";
  struct stat cst;
  if (stat(cache.c_str(), &cst) == 0 && cst.st_mtime >= srcMtime)
    return readFile(cache);
  std::string cmd = clangBin + " -S -emit-llvm -O" +
                    std::to_string(g_opts.runtimeOpt) + " -g0 -x c " + rtSrc +
                    " -o " + cache + " 2>/dev/null";
  if (runCmd(cmd) != 0 || readFile(cache).empty()) {
    fprintf(stderr, "penstock: cannot compile runtime library\n");
    return "";
  }
  return readFile(cache);
}

// strip module-level header lines; drop `declare @llvm.*` lines already
// declared by the user module (user decls win; runtime bodies resolve to
// them since intrinsic decls are semantically identical)
//
// §3 fix: ALSO drop the runtime's own named module metadata —
// `!llvm.module.flags` (which on modern clang carries !"Dwarf Version",
// !"uwtable", ... and COLLIDES with the user module's module flags:
// LLVM requires module-flag identifiers to be unique, so concatenating
// both broke the backend with "module flag identifiers must be unique")
// and `!llvm.ident`.  The user module is the owner of module-level flags;
// the runtime IR inherits them.  Numbered metadata nodes are kept because
// runtime instructions reference them (!llvm.loop, !prof).
static std::string stripRuntimeHeader(const std::string& ll,
                                      const std::set<std::string>& userIntrinsics) {
  std::istringstream in(ll);
  std::ostringstream out;
  std::string line;
  while (std::getline(in, line)) {
    std::string t = line;
    size_t a = t.find_first_not_of(" \t");
    if (a != std::string::npos) t = t.substr(a);
    if (t.rfind("; ModuleID", 0) == 0) continue;
    if (t.rfind("source_filename", 0) == 0) continue;
    if (t.rfind("target datalayout", 0) == 0) continue;
    if (t.rfind("target triple", 0) == 0) continue;
    if (t.rfind("!llvm.module.flags", 0) == 0) continue;
    if (t.rfind("!llvm.ident", 0) == 0) continue;
    if (t.rfind("declare ", 0) == 0) {
      size_t at = t.find("@llvm.");
      if (at != std::string::npos) {
        size_t e = t.find('(', at);
        if (e != std::string::npos) {
          std::string name = t.substr(at + 1, e - at - 1);
          if (userIntrinsics.count(name)) continue;   // user already declares it
        }
      }
    }
    out << line << "\n";
  }
  return out.str();
}

// collect @llvm.* names declared by the user module
static std::set<std::string> userIntrinsicSet(const std::string& ll) {
  std::set<std::string> s;
  std::istringstream in(ll);
  std::string line;
  while (std::getline(in, line)) {
    size_t a = line.find_first_not_of(" \t");
    if (a == std::string::npos || line.compare(a, 8, "declare ") != 0) continue;
    size_t at = line.find("@llvm.", a);
    if (at == std::string::npos) continue;
    size_t e = line.find('(', at);
    if (e == std::string::npos) continue;
    s.insert(line.substr(at + 1, e - at - 1));
  }
  return s;
}

// strip declares that the runtime defines (frt_*), and the external
// __frt_env_threads placeholder — after concat the runtime part defines them.
static std::string stripUserRtDecls(const std::string& ll) {
  std::istringstream in(ll);
  std::ostringstream out;
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("declare ", 0) == 0 && line.find("@frt_") != std::string::npos)
      continue;
    if (line.rfind("@__frt_env_threads = external global i32", 0) == 0)
      continue;
    out << line << "\n";
  }
  return out.str();
}

// ---------------- diagnostics printing (rustc-style) -----------------------
//   error[F0214]: division by zero
//    --> prog.fsh:5:20
//     |
//   5 |     let r: i64 = x / 0;
//     |                    ^
//     |
//     = help: <text after " — " in the message>
// Colour: only when stderr is a tty (or CLICOLOR_FORCE); NO_COLOR disables.
static std::string paint(const char* sgr, const std::string& s) {
  static const bool on = getenv("CLICOLOR_FORCE") || (!getenv("NO_COLOR") && isatty(2));
  return on ? std::string("\x1b[") + sgr + "m" + s + "\x1b[0m" : s;
}
static const std::vector<std::string>& srcLines(const std::string& f) {
  static std::map<std::string, std::vector<std::string>> cache;
  auto& v = cache[f];
  if (v.empty()) { std::ifstream in(f); for (std::string l; std::getline(in, l);) { if (!l.empty() && l.back() == '\r') l.pop_back(); v.push_back(l); } }
  return v;
}
static std::string detab(const std::string& s) { std::string o; for (char c : s) o += c == '\t' ? "    " : std::string(1, c); return o; }
static size_t cols(const std::string& s) { size_t n = 0; for (unsigned char c : s) n += (c & 0xC0) != 0x80; return n; }  // display columns (UTF-8 aware)

// `penstock --explain <ID|slug>`: registry entry plus docs/errors/<ID>.md, found in the
// source tree (<root>/docs/errors) or an install (<prefix>/share/penstock/errors).
static int explainCode(const std::string& key) {
  const CodeInfo* c = findCode(key);
  if (!c) { fprintf(stderr, "penstock: unknown diagnostic '%s' (IDs look like F0402; slugs like arith-div-runtime)\n", key.c_str()); return 1; }
  for (const std::string& dir : {exeDir() + "/../docs/errors", exeDir() + "/../share/penstock/errors"})
    if (std::ifstream in{dir + "/" + c->id + ".md"}) { std::ostringstream o; o << in.rdbuf(); fputs(o.str().c_str(), stdout); return 0; }
  printf("%s (%s): %s\n\nNo extended explanation has been written for this diagnostic yet.\n", c->id, c->slug, c->title);
  return 0;
}

static void printDiags() {
  int nerr = 0, nwarn = 0;
  for (auto& d : g_diags) {
    const bool err = d.sev == Diag::Error, warn = d.sev == Diag::Warning;
    nerr += err; nwarn += warn;
    const char* sgr = err ? "1;31" : warn ? "1;33" : "1;32";
    // headline = text up to the first " — " or newline; the rest is the help/note body
    static const std::string kDash = " \xe2\x80\x94 ";
    size_t cut = std::min(d.msg.find(kDash), d.msg.find('\n'));
    std::string head = d.msg.substr(0, cut), body = cut == std::string::npos ? "" : d.msg.substr(cut + (d.msg[cut] == '\n' ? 1 : kDash.size()));
    fprintf(stderr, "%s%s\n", paint(sgr, std::string(err ? "error" : warn ? "warning" : "note") + "[" + d.code + "]").c_str(), paint("1", ": " + head).c_str());

    const size_t w = std::to_string(std::max(d.line, 1)).size();
    const std::string pad(w + 1, ' '), bar = paint("1;34", "|");
    bool snip = false;
    if (!d.file.empty()) {
      fprintf(stderr, "%s%s %s%s%s\n", std::string(w, ' ').c_str(), paint("1;34", "-->").c_str(), d.file.c_str(), d.line > 0 ? (":" + std::to_string(d.line)).c_str() : "", d.col > 0 ? (":" + std::to_string(d.col)).c_str() : "");
      // snippet + underline; optimisation remarks (N-*) stay compact so they don't flood the output
      auto& src = srcLines(d.file);
      const CodeInfo* ci = findCode(d.code);
      if (!(ci && ci->remark) && d.line > 0 && (size_t)d.line <= src.size()) {
        snip = true;
        const std::string& ln = src[d.line - 1];
        fprintf(stderr, "%s%s\n%s %s %s\n", pad.c_str(), bar.c_str(), paint("1;34", std::to_string(d.line)).c_str(), bar.c_str(), detab(ln).c_str());
        if (d.col > 0) {
          size_t i = std::min((size_t)d.col - 1, ln.size()), j = i;
          auto id = [](unsigned char c) { return isalnum(c) || c == '_' || c == '@' || c >= 0x80; };
          if (j < ln.size() && id(ln[j])) while (j < ln.size() && id(ln[j])) ++j; else j = std::min(i + 1, ln.size());
          fprintf(stderr, "%s%s %s%s\n", pad.c_str(), bar.c_str(), std::string(cols(detab(ln.substr(0, i))), ' ').c_str(), paint(sgr, std::string(std::max<size_t>(cols(detab(ln.substr(i, j - i))), 1), '^')).c_str());
        }
      }
    }
    if (!body.empty()) {   // first body line after the label, the rest aligned under it
      if (snip) fprintf(stderr, "%s%s\n", pad.c_str(), bar.c_str());
      std::istringstream in(body); std::string l;
      for (bool first = true; std::getline(in, l); first = false)
        fprintf(stderr, "%s%s %s\n", pad.c_str(), first ? paint("1;34", "=").c_str() : " ", (first ? paint("1", d.sev == Diag::Note ? "note:" : "help:") + " " + l : "      " + l).c_str());
    }
    fputc('\n', stderr);
  }
  auto n = [](int k, const char* w) { return std::to_string(k) + " " + w + (k == 1 ? "" : "s"); };
  const std::string warns = nwarn ? n(nwarn, "warning") + " emitted" : "";
  if (nerr) fprintf(stderr, "%s: aborting due to %s%s\n", paint("1;31", "error").c_str(), n(nerr, "previous error").c_str(), nwarn ? ("; " + warns).c_str() : "");
  else if (nwarn) fprintf(stderr, "%s: %s\n", paint("1;33", "warning").c_str(), warns.c_str());
}

// Create a directory and any missing parents. This is used only for the
// compiler's private scratch/cache directory; files are not shared between
// users and the cache is not part of the installed read-only payload.
static void makeDirs(const std::string& path) {
  if (path.empty()) return;
  size_t begin = path[0] == '/' ? 1 : 0;
  for (size_t i = begin; i <= path.size(); ++i) {
    if (i != path.size() && path[i] != '/') continue;
    if (i == 0) continue;
    std::string part = path.substr(0, i);
    if (part.empty()) continue;
    if (mkdir(part.c_str(), 0700) != 0 && errno != EEXIST) {
      // A later write will produce the concrete diagnostic if this prevents
      // creation; don't fail merely because an intermediate already exists.
    }
  }
}

static bool readableFile(const std::string& path) {
  return access(path.c_str(), R_OK) == 0;
}

// Development builds keep their cache in <checkout>/build, as before.
// Installed builds instead use a user-writable per-user cache and resolve
// the immutable runtime source from <prefix>/share/penstock/runtime.
static std::string cacheDirectory() {
  static std::string d;
  if (!d.empty()) return d;

  const char* overrideDir = getenv("PENSTOCK_CACHE_DIR");
  if (overrideDir && *overrideDir) {
    d = overrideDir;
  } else if (readableFile(exeDir() + "/../runtime/frt_rt.c")) {
    d = exeDir() + "/../build";
  } else {
    const char* xdg = getenv("XDG_CACHE_HOME");
    const char* home = getenv("HOME");
    const char* tmp = getenv("TMPDIR");
    if (xdg && *xdg) d = std::string(xdg) + "/penstock";
    else if (home && *home) d = std::string(home) + "/.cache/penstock";
    else d = std::string((tmp && *tmp) ? tmp : "/tmp") +
             "/penstock-" + std::to_string((unsigned long)getuid());
  }
  makeDirs(d);
  return d;
}

static std::string runtimeSourcePath() {
  // Source checkout / build-tree layout.
  std::string dev = exeDir() + "/../runtime/frt_rt.c";
  if (readableFile(dev)) return dev;

  // Installed layout. CMake embeds a relative GNUInstallDirs location by
  // default, so a normal prefix relocation remains supported.
#ifdef PENSTOCK_INSTALL_DATADIR
  std::string dataDir = PENSTOCK_INSTALL_DATADIR;
  std::string base = (!dataDir.empty() && dataDir[0] == '/')
      ? dataDir : exeDir() + "/../" + dataDir;
  std::string installed = base + "/runtime/frt_rt.c";
  if (readableFile(installed)) return installed;
#endif
  // Keep the standard layout as a useful fallback for non-CMake/manual builds.
  std::string fallback = exeDir() + "/../share/penstock/runtime/frt_rt.c";
  if (readableFile(fallback)) return fallback;
  return dev;
}

// scratch dir for driver-owned temp files (target probes, merged profdata)
static std::string scratchDir() {
  return cacheDirectory();
}

// §8: backend flags selecting the vector math library.  libmvec (glibc's
// vector math) lets the loop vectoriser replace llvm.sin/.cos/.log2/.log10
// calls with vector routines; -fno-math-errno is required for that and
// matches the language's default fast-FP semantics (errno is not part of
// Freshet's observable model).  "none" keeps the portable lowering.
static std::string veclibFlags() {
  if (g_opts.veclib == "libmvec") return " -fveclib=libmvec -fno-math-errno";
  return "";
}

// §11.2: backend flags for the LLVM IR-based PGO workflow.
static std::string llvmPgoFlags() {
  std::string f;
  if (g_opts.llvmPgoGenerate) f += " -fprofile-generate";
  if (!g_opts.llvmPgoUse.empty()) f += " -fprofile-use=" + g_opts.llvmPgoUse;
  return f;
}

// §12: backend -target flag when a non-default target is configured.
static std::string targetFlags() {
  if (!g_targetTriple.empty()) return " -target " + g_targetTriple;
  return "";
}

// §11.2: turn --llvm-pgo-use=<profraw|dir|comma-list|profdata> into a
// .profdata path, merging raw profiles with llvm-profdata on the way —
// the documented Clang workflow.  Returns "" (with a diagnostic) when
// the tool is unavailable; passing an already-merged .profdata never
// needs the tool.
static std::string resolveLlvmPgoUse(const std::string& llvmRoot,
                                    const std::string& scratch) {
  std::string in = g_opts.llvmPgoUse;
  struct stat st;
  if (in.size() >= 8 && in.substr(in.size() - 8) == ".profdata" &&
      stat(in.c_str(), &st) == 0)
    return in;   // already merged

  // gather .profraw inputs: single file, comma list, or directory
  std::vector<std::string> raws;
  auto addEntry = [&](const std::string& e) {
    if (e.empty()) return;
    if (stat(e.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
      DIR* d = opendir(e.c_str());
      if (!d) return;
      while (dirent* de = readdir(d)) {
        std::string n = de->d_name;
        if (n.size() > 7 && n.substr(n.size() - 7) == ".profraw")
          raws.push_back(e + "/" + n);
      }
      closedir(d);
    } else {
      raws.push_back(e);
    }
  };
  size_t pos = 0;
  while (pos != std::string::npos) {
    size_t comma = in.find(',', pos);
    addEntry(in.substr(pos, comma == std::string::npos ? comma : comma - pos));
    pos = (comma == std::string::npos) ? std::string::npos : comma + 1;
  }
  raws.erase(std::remove_if(raws.begin(), raws.end(),
                            [](const std::string& r) {
                              return r.size() < 8 ||
                                     r.substr(r.size() - 7) != ".profraw";
                            }),
             raws.end());
  if (raws.empty()) {
    fprintf(stderr, "penstock: --llvm-pgo-use found no .profraw inputs in "
                    "'%s' (and it is not a .profdata file)\n", in.c_str());
    return "";
  }

  // locate llvm-profdata: --llvm-profdata > $LLVM_ROOT/bin > $PATH
  std::string tool = g_opts.llvmProfdata;
  if (tool.empty() &&
      access((llvmRoot + "/bin/llvm-profdata").c_str(), X_OK) == 0)
    tool = llvmRoot + "/bin/llvm-profdata";
  if (tool.empty()) {
    FILE* p = popen("command -v llvm-profdata 2>/dev/null", "r");
    char buf[512];
    if (p && fgets(buf, sizeof buf, p)) {
      std::string s(buf);
      while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
      if (!s.empty()) tool = s;
    }
    if (p) pclose(p);
  }
  if (tool.empty()) {
    fprintf(stderr, "penstock: llvm-profdata not found — merge the .profraw "
                    "files yourself (llvm-profdata merge -output=x.profdata "
                    "...) and pass the .profdata, or point --llvm-profdata "
                    "at the tool\n");
    return "";
  }
  std::string out = scratch + "/pgo_" + std::to_string(getpid()) + ".profdata";
  std::string cmd = tool + " merge -output=" + out;
  for (auto& r : raws) cmd += " " + r;
  cmd += " 2>&1";
  if (runCmd(cmd) != 0 || stat(out.c_str(), &st) != 0) {
    fprintf(stderr, "penstock: llvm-profdata merge failed\n");
    return "";
  }
  fprintf(stderr, "penstock: merged %zu profile(s) into %s\n",
          raws.size(), out.c_str());
  return out;
}

int main(int argc, char** argv) {
  if (argc < 2) { fputs(USAGE, stderr); return 1; }

  // ------------------------------------------- Phase 3/4 subcommands
  // `penstock build|test|fmt|lint|run ...` — a file literally named like
  // a subcommand still compiles fine (`./build` or any path), because the
  // dispatch below only fires for a bare word that is NOT an existing file.
  std::vector<std::string> argList(argv, argv + argc);
  // Phase 3 (§6.2): set when a project manifest was loaded (build
  // subcommand or auto-detected penstock.toml) — drives the output name
  // derivation, library targets and the whole-program build cache
  bool manifestLoaded = false;
  {
    std::string a1 = argList[1];
    struct stat st;
    // only a REGULAR file named like a subcommand suppresses dispatch
    // (the source tree's build/ directory must not shadow `penstock build`)
    bool isFile = stat(a1.c_str(), &st) == 0 && S_ISREG(st.st_mode);
    if (!isFile && (a1 == "build" || a1 == "test" || a1 == "fmt" ||
                    a1 == "lint" || a1 == "run")) {
      if (a1 == "fmt") {
        // ---- fmt: source formatter (report §10.2)
        bool check = false;
        std::string file;
        for (size_t i = 2; i < argList.size(); ++i) {
          const std::string& a = argList[i];
          if (a == "--check") { check = true; continue; }
          if (!a.empty() && a[0] == '-') {
            fprintf(stderr, "penstock fmt: unknown flag %s\n", a.c_str());
            return 1;
          }
          file = a;
        }
        if (file.empty()) {
          fprintf(stderr, "usage: penstock fmt [--check] <file.fsh>\n");
          return 1;
        }
        return formatSourceFile(file, check) ? 0 : 1;
      }
      if (a1 == "test") {
        // ---- test: run @test fns through the generated harness (§10.3).
        // Remaining flags + files are handled by the normal loop below.
        g_opts.testMode = true;
        g_opts.output = "penstock_test";
        argList.erase(argList.begin() + 1);
      } else if (a1 == "lint") {
        // ---- lint: diagnostics-only run (§10.2)
        g_opts.lintMode = true;
        argList.erase(argList.begin() + 1);
      } else if (a1 == "run") {
        // ---- run: compile then execute
        g_opts.run = true;
        argList.erase(argList.begin() + 1);
      } else if (a1 == "build") {
        // ---- build: project manifest (§6.2).  The manifest owns the
        // input list; any remaining arguments must be flags.
        std::string dir = ".";
        for (size_t i = 2; i < argList.size(); ++i) {
          const std::string& a = argList[i];
          if (a == "--force") { g_opts.forceRebuild = true; continue; }
          if (!a.empty() && a[0] == '-') continue;   // flags parsed below
          if (dir == ".") { dir = a; argList.erase(argList.begin() + i); --i; }
          else {
            fprintf(stderr, "penstock build: unexpected argument '%s' "
                            "(only one project directory)\n", a.c_str());
            return 1;
          }
        }
        std::string manifest = dir == "." ? "penstock.toml"
                                          : dir + "/penstock.toml";
        if (!loadProjectManifest(manifest)) {
          printDiags();
          return 1;
        }
        manifestLoaded = true;
        argList.erase(argList.begin() + 1);   // drop the subcommand
      }
    }
  }

  for (size_t i = 1; i < argList.size(); ++i) {
    const std::string& a = argList[i];
    if (a == "--version") {
      printf("Penstock %s\n", FRESHET_VERSION);
      return 0;
    }
    if (a == "-h" || a == "--help") { fputs(USAGE, stderr); return 0; }
    if (a == "--explain") {
      if (i + 1 >= argList.size()) { fprintf(stderr, "usage: penstock --explain <ID|slug>   (e.g. F0402)\n"); return 1; }
      return explainCode(argList[i + 1]);
    }
    if (a.rfind("-o", 0) == 0 && a.size() > 2 && a[2] != '=') {
      g_opts.output = a.substr(2);
      continue;
    }
    if (a == "-o" && i + 1 < argList.size()) { g_opts.output = argList[++i]; continue; }
    if (a == "-S") { g_opts.irOut = "a.ll"; g_opts.emitObj = false; continue; }
    if (a == "-c") { g_opts.emitObj = true; continue; }
    if (a.rfind("-O", 0) == 0 && a.size() == 3 && isdigit(a[2]) && a[2] <= '4') {
      g_opts.optLevel = a[2] - '0';
      continue;
    }
    if (a.rfind("-march=", 0) == 0) { g_opts.march = a.substr(7); continue; }
    if (a == "--keep-ir") { g_opts.keepIr = true; continue; }
    if (a.rfind("--ir-out=", 0) == 0) { g_opts.irOut = a.substr(9); continue; }
    if (a == "--run") { g_opts.run = true; continue; }
    if (a.rfind("--threads=", 0) == 0) {
      g_opts.threads = atoi(a.c_str() + 10);
      continue;
    }
    if (a.rfind("--evo-budget=", 0) == 0) {
      g_opts.evoBudgetSec = atoi(a.c_str() + 13);
      if (g_opts.evoBudgetSec < 1) g_opts.evoBudgetSec = 1;
      continue;
    }
    if (a.rfind("--evo-seed=", 0) == 0) {
      g_opts.evoSeed = (unsigned)strtoul(a.c_str() + 11, nullptr, 10);
      continue;
    }
    if (a.rfind("--superopt-budget=", 0) == 0) {
      g_opts.superoptBudgetSec = atoi(a.c_str() + 18);
      if (g_opts.superoptBudgetSec < 1) g_opts.superoptBudgetSec = 1;
      continue;
    }
    if (a == "--no-superopt") { g_opts.noSuperopt = true; continue; }
    if (a == "--no-precompute") { g_opts.noPrecompute = true; continue; }
    if (a == "--evo-runtime-tune") { g_opts.evoRuntimeTune = true; continue; }
    if (a == "--report") { g_opts.report = true; continue; }
    if (a == "--profile") { g_opts.profile = true; continue; }
    if (a.rfind("--use-profile=", 0) == 0) {
      g_opts.useProfile = a.substr(14);
      continue;
    }
    if (a.rfind("--soa=", 0) == 0) {
      std::string mode = a.substr(6);
      g_opts.soaAuto = (mode == "auto");
      // optimisation-audit §6.11: AoSoA is advisory — the frequency-
      // weighted layout comparison is reported, the code is not
      // transformed (a full AoSoA lowering needs new runtime types)
      g_opts.soaAosoa = (mode == "aosoa");
      continue;
    }
    if (a == "--no-loop-opts") { g_opts.noLoopOpts = true; continue; }
    // ---- optimisation-audit integration: Phase C/D pass flags (opt-in,
    // each with a --no-* escape hatch for differential testing §10.2)
    if (a == "--licm")               { g_opts.licmOn = true; continue; }
    if (a == "--no-licm")            { g_opts.licmOn = false; continue; }
    if (a == "--partial-unroll")     { g_opts.partialUnrollOn = true; continue; }
    if (a == "--no-partial-unroll")  { g_opts.partialUnrollOn = false; continue; }
    if (a == "--peel")               { g_opts.peelOn = true; continue; }
    if (a == "--no-peel")            { g_opts.peelOn = false; continue; }
    if (a == "--interchange")        { g_opts.interchangeOn = true; continue; }
    if (a == "--no-interchange")     { g_opts.interchangeOn = false; continue; }
    if (a.rfind("--tile=", 0) == 0) {
      // --tile=I[,J]  (either may be 0 = choose from the target profile)
      std::string v = a.substr(7);
      g_opts.tileOn = true;
      size_t c = v.find(',');
      g_opts.tileI = atoi(v.substr(0, c == std::string::npos ? v.size() : c).c_str());
      if (c != std::string::npos) g_opts.tileJ = atoi(v.substr(c + 1).c_str());
      continue;
    }
    if (a == "--tile")               { g_opts.tileOn = true; continue; }
    if (a == "--no-tile")            { g_opts.tileOn = false; continue; }
    if (a == "--distribute")         { g_opts.distributeOn = true; continue; }
    if (a == "--no-distribute")      { g_opts.distributeOn = false; continue; }
    if (a == "--unroll-and-jam")     { g_opts.unrollJamOn = true; continue; }
    if (a == "--no-unroll-and-jam")  { g_opts.unrollJamOn = false; continue; }
    if (a == "--parfor-advisor")     { g_opts.parforAdvisorOn = true; continue; }
    if (a.rfind("--proof-log=", 0) == 0) {
      g_opts.proofLogOn = true;
      g_opts.proofLogPath = a.substr(12);
      continue;
    }
    if (a == "--proof-log") { g_opts.proofLogOn = true; continue; }
    // §7.1: superoptimiser objective
    if (a.rfind("--superopt-objective=", 0) == 0) {
      std::string v = a.substr(21);
      if (v != "latency" && v != "size" && v != "balanced") {
        fprintf(stderr, "penstock: unknown --superopt-objective '%s' "
                        "(latency|size|balanced)\n", v.c_str());
        return 1;
      }
      g_opts.superoptObjective = v;
      continue;
    }
    // §8: target profile overrides for the shared profitability model
    if (a.rfind("--target-cache-line=", 0) == 0) {
      g_opts.targetCacheLine = atoi(a.c_str() + 20);
      continue;
    }
    if (a.rfind("--target-l1=", 0) == 0) {
      g_opts.targetL1 = atoi(a.c_str() + 12);
      continue;
    }
    if (a.rfind("--target-l2=", 0) == 0) {
      g_opts.targetL2 = atoi(a.c_str() + 12);
      continue;
    }
    if (a.rfind("--target-vector-width=", 0) == 0) {
      g_opts.targetVectorWidth = atoi(a.c_str() + 22);
      continue;
    }
    // ---- report §7: performance-diagnostics policy (warn-first default)
    if (a == "--strict-performance") { g_opts.strictPerformance = true; continue; }
    // ---- report §8 Option A: strict FP is the default; --fast-math opts in
    if (a == "--fast-math") { g_opts.fastMathGlobal = true; continue; }
    // ---- report §4.1/§4.5: bounds-check policy for Span/array indexing
    if (a == "--checked") { g_opts.checked = true; continue; }
    if (a == "--no-bounds-check") { g_opts.noBoundsCheck = true; continue; }
    // ---- Stage A1: inlining policy
    if (a.rfind("--inline=", 0) == 0) {
      std::string v = a.substr(9);
      if (v == "default")      g_opts.inlinePolicy = InlinePolicy::Default;
      else if (v == "conservative")
        g_opts.inlinePolicy = InlinePolicy::Conservative;
      else if (v == "off")    g_opts.inlinePolicy = InlinePolicy::Off;
      else {
        fprintf(stderr, "penstock: unknown --inline= value '%s' "
                        "(default|conservative|off)\n", v.c_str());
        return 1;
      }
      continue;
    }
    // ---- Stage A2: optimisation observability loop
    if (a.rfind("--emit-combined-ir=", 0) == 0) {
      g_opts.emitCombinedIr = a.substr(19);
      continue;
    }
    if (a == "--emit-combined-ir") {
      g_opts.emitCombinedIr = "<auto>";  // resolved to <out>.combined.ll
      continue;
    }
    if (a == "--save-temps") { g_opts.saveTemps = true; g_opts.keepIr = true; continue; }
    if (a.rfind("--opt-remarks", 0) == 0) {
      g_opts.optRemarksEnabled = true;
      if (a.size() > 13 && a[13] == '=') g_opts.optRemarks = a.substr(14);
      else                              g_opts.optRemarks = "all";
      continue;
    }
    // ---- Stage A3: opt out of emitting combined IR on -S
    if (a == "--legacy-S") { g_opts.emitCombinedOnS = false; continue; }
    // ---- audit integration flags
    if (a.rfind("--runtime-opt=", 0) == 0) {
      int v = atoi(a.c_str() + 14);
      if (v < 0 || v > 3) {
        fprintf(stderr, "penstock: --runtime-opt must be 0..3\n");
        return 1;
      }
      g_opts.runtimeOpt = v;
      continue;
    }
    if (a == "--force-vectorize") { g_opts.forceVectorize = true; continue; }
    if (a.rfind("--veclib=", 0) == 0) {
      g_opts.veclib = a.substr(9);
      if (g_opts.veclib != "none" && g_opts.veclib != "libmvec") {
        fprintf(stderr, "penstock: unknown --veclib value '%s' (none|libmvec)\n",
                g_opts.veclib.c_str());
        return 1;
      }
      continue;
    }
    if (a == "--llvm-pgo-generate") { g_opts.llvmPgoGenerate = true; continue; }
    if (a.rfind("--llvm-pgo-use=", 0) == 0) {
      g_opts.llvmPgoUse = a.substr(15);
      continue;
    }
    if (a.rfind("--llvm-profdata=", 0) == 0) {
      g_opts.llvmProfdata = a.substr(16);
      continue;
    }
    if (a == "--emit-final-ir") { g_opts.emitFinalIr = "<auto>"; continue; }
    if (a.rfind("--emit-final-ir=", 0) == 0) {
      g_opts.emitFinalIr = a.substr(16);
      continue;
    }
    if (a.rfind("--target=", 0) == 0) { g_opts.targetTriple = a.substr(9); continue; }
    if (a == "--portable") { g_opts.portable = true; continue; }
    if (a.rfind("-I", 0) == 0 && a.size() > 2) {
      g_opts.includeDirs.push_back(a.substr(2));
      continue;
    }
    // Phase 3c/4 new flags ---------------------------------------------
    if (a == "-g" || a == "--debug") { g_opts.debugInfo = true; continue; }
    if (a.rfind("--project=", 0) == 0) { g_opts.projectFile = a.substr(10); continue; }
    if (a.rfind("-", 0) == 0) {
      fprintf(stderr, "penstock: unknown option %s\n", a.c_str());
      return 1;
    }
    // Phase 3a (report §6.2): every .fsh argument is a module of this
    // compilation — the FIRST one is the entry module
    if (g_opts.input.empty()) { g_opts.input = a; continue; }
    g_opts.inputs.push_back(a);
  }
  // project manifest (explicit --project, or auto-detected penstock.toml
  // beside the entry file)
  {
    std::string manifest = g_opts.projectFile;
    if (manifest.empty() && !g_opts.input.empty()) {
      std::string d = g_opts.input;
      size_t s = d.find_last_of('/');
      d = (s == std::string::npos) ? "." : d.substr(0, s);
      std::string cand = d == "." ? "penstock.toml" : d + "/penstock.toml";
      struct stat st;
      if (stat(cand.c_str(), &st) == 0) manifest = cand;
    }
    if (!manifest.empty()) {
      // an explicit --project replaces any CLI input list
      if (!g_opts.projectFile.empty()) { g_opts.inputs.clear(); g_opts.input.clear(); }
      if (loadProjectManifest(manifest)) manifestLoaded = true;
      else { printDiags(); return 1; }
    }
  }
  if (g_opts.inputs.empty() && g_opts.input.empty()) {
    fputs(USAGE, stderr); return 1;
  }
  // entry module first, then any extra CLI modules, then manifest modules
  if (!g_opts.input.empty()) g_opts.inputs.insert(g_opts.inputs.begin(), g_opts.input);
  // `build` without -o: output = <manifest dir>/<project name> (or the
  // entry-module stem when the manifest has no [project] name)
  if (manifestLoaded && g_opts.output == "a.out") {
    std::string stem = g_opts.projectName.empty() ? g_opts.inputs[0]
                                                  : g_opts.projectName;
    size_t s = stem.find_last_of('/');
    if (s != std::string::npos) stem = stem.substr(s + 1);
    if (g_opts.projectTarget == "static-lib") stem = "lib" + stem + ".a";
    else if (g_opts.projectTarget == "shared-lib") stem = stem + ".so";
    g_opts.output = stem;   // relative — resolved below against projectDir
    if (!g_opts.projectDir.empty() && g_opts.projectDir != ".")
      g_opts.output = g_opts.projectDir + "/" + stem;
  }
  g_opts.input = g_opts.inputs[0];
  // project library targets: build as an object first, then archive /
  // shared-link (the pipeline below sees emitObj semantics)
  bool libTarget = manifestLoaded && (g_opts.projectTarget == "static-lib" ||
                                      g_opts.projectTarget == "shared-lib");
  std::string libOut = g_opts.output;
  if (libTarget) {
    g_opts.emitObj = true;
    g_opts.output = g_opts.output + ".o";
  }
  if (g_opts.irOut == "a.ll" && g_opts.output != "a.out")
    g_opts.irOut = g_opts.output;  // -S -o f.ll

  // Locate the backend toolchain.  Precedence:
  //   1. $LLVM_ROOT — explicit override, always wins
  //   2. PENSTOCK_DEFAULT_LLVM_ROOT — the backend CMake resolved (and
  //      smoke-tested) when building this binary; also the toolchain the
  //      prewarmed build/frt_rt_<N>.ll cache belongs to
  //   3. autodetect — tools/llvmNN beside the compiler or a system
  //      prefix /usr/lib/llvm-NN, newest supported first (23 .. 19)
  std::string llvmRoot = getenv("LLVM_ROOT") ? getenv("LLVM_ROOT") : "";
  struct stat tst;
  bool found = false;
  if (!llvmRoot.empty() && stat((llvmRoot + "/bin/clang").c_str(), &tst) == 0)
    found = true;
#ifdef PENSTOCK_DEFAULT_LLVM_ROOT
  if (!found) {
    llvmRoot = PENSTOCK_DEFAULT_LLVM_ROOT;
    if (stat((llvmRoot + "/bin/clang").c_str(), &tst) == 0)
      found = true;
  }
#endif
  if (!found) {
    static const int kMajors[] = {23, 22, 21, 20, 19};
    for (int maj : kMajors) {
      std::string m = std::to_string(maj);
      const std::string cands[] = {
        exeDir() + "/../../tools/llvm" + m + "/usr/lib/llvm-" + m,
        "/usr/lib/llvm-" + m,
      };
      for (auto& fb : cands)
        if (stat((fb + "/bin/clang").c_str(), &tst) == 0) {
          llvmRoot = fb; found = true; break;
        }
      if (found) break;
    }
  }
  if (!found) {
#ifdef PENSTOCK_DEFAULT_LLVM_ROOT
    fprintf(stderr, "penstock: the backend this penstock was built against\n"
                    "  " PENSTOCK_DEFAULT_LLVM_ROOT "\n"
                    "is missing bin/clang, and no supported LLVM (19-23) was "
                    "found either;\nset LLVM_ROOT to an install prefix with "
                    "bin/clang\n");
#else
    fprintf(stderr, "penstock: no supported LLVM toolchain found (19-23); "
                    "set LLVM_ROOT to an install prefix with bin/clang\n");
#endif
    return 1;
  }
  std::string clang = llvmRoot + "/bin/clang";

  // resolve backend version once: cache key + report description
  std::string llvmMajor, llvmVer;
  {
    FILE* vp = popen((clang + " --version 2>/dev/null").c_str(), "r");
    if (vp) {
      char buf[512];
      while (fgets(buf, sizeof buf, vp)) {
        std::string line(buf);
        size_t p = line.find("clang version ");
        if (p == std::string::npos) continue;
        std::string rest = line.substr(p + 14);
        size_t e = rest.find_first_not_of("0123456789.");
        llvmVer = rest.substr(0, e == std::string::npos ? rest.size() : e);
        size_t dot = llvmVer.find('.');
        llvmMajor = llvmVer.substr(0, dot == std::string::npos ? llvmVer.size() : dot);
        break;
      }
      pclose(vp);
    }
  }
  g_backend = "LLVM " + (llvmVer.empty() ? "<unknown>" : llvmVer) + " (" + llvmRoot + ")";

  // --------------------------------------------- 0.5 §12 target config
  // Resolve the canonical triple + data layout by compiling an empty C
  // file with the backend clang and scraping the module header — the
  // strings stay exact for whatever LLVM build is in use instead of
  // being hard-coded guesses.  This runs for EVERY build:
  //   * --target=<triple> / --portable probe WITH `-target <want>` so
  //     the requested triple is normalised through the backend itself;
  //   * a native build probes WITHOUT -target and scrapes the backend's
  //     OWN default triple.  The historical hard-coded module header
  //     (x86_64-pc-linux-gnu, irgen.cpp) disagreed with toolchains that
  //     default to x86_64-unknown-linux-gnu (upstream LLVM builds) and
  //     made clang warn on every .ll ingest:
  //       warning: overriding the module target triple with
  //       x86_64-unknown-linux-gnu [-Woverride-module]
  // The IR module (irgen.cpp) and the backend invocation (targetFlags())
  // then agree on ONE target by construction; -march alone could not
  // change front-end assumptions already baked into the module.
  {
    bool cross = !g_opts.targetTriple.empty() || g_opts.portable;
    std::string want = g_opts.portable ? "x86_64-pc-linux-gnu"
                                       : g_opts.targetTriple;
    std::string probe = scratchDir() + "/tgt_probe_" +
                        std::to_string(getpid()) + ".ll";
    std::string cmd = clang + " -S -emit-llvm -O0 -g0 -x c /dev/null" +
                      (cross ? " -target " + want : "") +
                      " -o " + probe + " 2>/dev/null";
    std::string tr, dl;
    if (runCmd(cmd) == 0) {
      std::string ll = readFile(probe);
      size_t t = ll.find("target triple = \"");
      if (t != std::string::npos) {
        size_t e = ll.find('"', t + 17);   // past the opening quote
        tr = ll.substr(t + 17, e - t - 17);
      }
      size_t d = ll.find("target datalayout = \"");
      if (d != std::string::npos) {
        size_t e = ll.find('"', d + 21);   // past the opening quote
        dl = ll.substr(d + 21, e - d - 21);
      }
      unlink(probe.c_str());
    } else {
      unlink(probe.c_str());
    }
    if (tr.empty() || dl.empty()) {
      // Emergency fallback only: the built-in native-host header pair
      // (irgen.cpp).  A broken backend (e.g. one built with an empty
      // baked-in default triple) also lands here — with a clear warning
      // instead of an obscure later codegen failure.
      fprintf(stderr, "penstock: warning: backend target probe failed; "
                      "keeping the built-in module header "
                      "(x86_64-pc-linux-gnu)\n");
    } else {
      g_targetTriple = tr;
      g_dataLayout = dl;
      // native builds stay silent: the scraped triple IS the backend
      // default, so there is nothing the user needs to confirm
      if (cross)
        fprintf(stderr, "penstock: target %s (data layout from backend)\n",
                tr.c_str());
    }
    if (g_opts.portable && g_opts.march == "native") g_opts.march = "x86-64";
  }

  // --------------------------------------------- 0.6 §11.2 LLVM IR PGO
  // Resolve --llvm-pgo-use into a .profdata path: raw .profraw inputs (or
  // a directory / comma-separated list) are merged with llvm-profdata
  // first, exactly the workflow the Clang user manual documents
  // (-fprofile-generate -> run -> llvm-profdata merge -> -fprofile-use).
  if (!g_opts.llvmPgoUse.empty()) {
    g_opts.llvmPgoUse = resolveLlvmPgoUse(llvmRoot, scratchDir());
    if (g_opts.llvmPgoUse.empty()) return 1;   // diagnostic already printed
  }
  if (g_opts.llvmPgoGenerate) {
    fprintf(stderr, "penstock: LLVM IR PGO training build — run the binary "
                    "with LLVM_PROFILE_FILE=<name>.profraw, then rebuild "
                    "with --llvm-pgo-use=<name>.profraw\n");
  }

  // ---------------------------------------------------------- 1. modules
  // Phase 3a (report §6.1/§6.2): preprocess + parse every input file,
  // resolve `import`s transitively (cycle-checked) and merge everything
  // into ONE program whose decls carry their originating module tags.
  std::vector<std::string> linkLibs;
  for (auto& l : g_opts.projectLinkLibs)
    if (std::find(linkLibs.begin(), linkLibs.end(), l) == linkLibs.end())
      linkLibs.push_back(l);
  Program* prog = loadProgramWithInputs(g_opts.inputs, linkLibs);
  if (!prog || errorCount()) { printDiags(); return 1; }

  // Phase 3 (§6.2) dependency tracking: project builds are cached at the
  // whole-program level — the key covers the compiler version, the build
  // flags and the CONTENT of every module in the import closure, so any
  // change anywhere rebuilds (and only then).  `--force` overrides.
  uint64_t buildKey = 0;
  std::string cachePath;
  if (manifestLoaded && !g_opts.forceRebuild && !g_opts.testMode) {
    std::set<std::string> srcFiles;
    for (auto* f : prog->fns) if (!f->loc.file.empty()) srcFiles.insert(f->loc.file);
    for (auto* s : prog->structs) if (!s->loc.file.empty()) srcFiles.insert(s->loc.file);
    for (auto* e : prog->enums) if (!e->loc.file.empty()) srcFiles.insert(e->loc.file);
    for (auto* g : prog->globals) if (!g->loc.file.empty()) srcFiles.insert(g->loc.file);
    std::string key = std::string("penstock|") + FRESHET_VERSION +
                      "|O" + std::to_string(g_opts.optLevel) +
                      "|m=" + g_opts.march +
                      "|t=" + g_targetTriple +   // §12: --target switch = rebuild
                      "|out=" + g_opts.output +
                      (g_opts.debugInfo ? "|g" : "") +
                      (g_opts.fastMathGlobal ? "|fm" : "") +
                      (g_opts.strictPerformance ? "|sp" : "");
    for (auto& f : srcFiles)
      key += "|" + f + ":" + std::to_string(hashName(readFile(f)));
    buildKey = hashName(key);
    cachePath = g_opts.output + ".pbcache";
    struct stat cst;
    if (stat(g_opts.output.c_str(), &cst) == 0 &&
        stat(cachePath.c_str(), &cst) == 0) {
      std::string cached = readFile(cachePath);
      if (!cached.empty() && cached == std::to_string(buildKey)) {
        fprintf(stderr, "penstock: %s is up to date (%zu module(s), --force "
                        "to rebuild)\n", g_opts.output.c_str(), srcFiles.size());
        return 0;
      }
    }
  }

  // Phase 4 (report §10.3): `penstock test` — replace main with the
  // generated @test harness BEFORE sema so it type-checks like user code
  if (g_opts.testMode) {
    if (!synthesizeTestMain(prog)) { printDiags(); return 1; }
  }

  // ---------------------------------------------------------- 2.5 soa=auto
  // Pre-sema AST transform: rewrites *S arrays that F0408 would flag
  // into soa(S) planes. Must run before sema binds struct names.
  // --soa=aosoa runs the same analysis but stays ADVISORY (§6.11).
  if (g_opts.soaAuto || g_opts.soaAosoa) applySoaAutoTransform(prog);

  // ---------------------------------------------------------- 3. sema
  if (!semaProgram(prog) || errorCount()) { printDiags(); return 1; }

  // ---------------------------------------------------------- 4. checks
  runStrictChecks(prog);
  runParforChecks(prog);
  runOwnershipAnalysis(prog);

  // ------------------------------------ 4.5 R4 loop optimisation passes
  // Post-sema, post-checks, pre-irgen AST transforms.  Enabled at -O2+;
  // skipped on --profile builds so instrumented profiles are measured
  // on the same structure that gets optimised in the --use-profile build.
  //
  // Pipeline order follows the optimisation-audit §9 table:
  //   (1-2) typed AST normalisation + typed constant folding — exact
  //         bit-vector literals feed every later analysis;
  //   (3-4) effect summaries + alias/footprints — computed by the passes
  //         themselves (computeEffects/computeFootprints);
  //   (5-6) range/induction + dependence info — embedded in the
  //         transform legality checks (loop_analysis.cpp);
  //   (7-8) fusion / full unrolling / cross-lane, then the Phase D
  //         opt-in transforms (LICM, partial unroll, peel, interchange,
  //         tiling, distribution, unroll-and-jam) and the parfor advisor.
  // The AoS->SoA selection runs pre-sema (step 2.5) by necessity: it
  // rewrites struct declarations before names are bound.
  int nConstFold = 0, nFuseUnroll = 0, nLane = 0, nAdvise = 0;
  LoopXformCounts nXform;
  if (g_opts.optLevel >= 2 && !g_opts.noLoopOpts && !g_opts.profile) {
    nConstFold = runTypedConstFoldPass(prog);
    nFuseUnroll = runFuseUnrollPasses(prog);
    nLane = runLaneAnalysisPass(prog);
    nXform = runLoopTransformPasses(prog);
    if (g_opts.parforAdvisorOn) nAdvise = runParforAdvisorPass(prog);
    if (nConstFold + nFuseUnroll + nLane + nXform.total() + nAdvise > 0) {
      fprintf(stderr, "penstock: loop-opts: %d const-fold, %d fusion/unroll, "
                      "%d cross-lane, %d phase-D transform(s)"
                      " (%d licm, %d punroll, %d peel, %d interchange, "
                      "%d tile, %d distribute, %d unroll-jam)"
                      "%s applied\n",
              nConstFold, nFuseUnroll, nLane, nXform.total(),
              nXform.licm, nXform.partialUnroll, nXform.peel,
              nXform.interchange, nXform.tile, nXform.distribute,
              nXform.unrollJam,
              nAdvise > 0 ? ", +parfor advice" : "");
    }
  }

  printDiags();
  if (errorCount()) return 1;

  // Phase 4 (report §10.2): lint mode stops here — the diagnostics above
  // (E-* correctness + W-* performance/style) are the deliverable
  if (g_opts.lintMode) {
    int warns = 0;
    for (auto& d : g_diags) if (d.sev != Diag::Error) ++warns;
    fprintf(stderr, "penstock lint: %d error(s), %d warning(s)\n",
            errorCount(), warns);
    return errorCount() ? 1 : 0;
  }
  std::string base = g_opts.output;
  size_t dot = base.rfind('.');
  if (dot != std::string::npos && dot > base.rfind('/'))
    base = base.substr(0, dot);
  std::string buildDir = cacheDirectory();
  makeDirs(buildDir);

  // -S / --ir-out: emit self-contained IR (runtime decls kept) and stop.
  //
  // Stage A3 (report §1.3): historically `-S` stopped after writing user IR,
  // so optimisation analysis saw a DIFFERENT module from the one Clang
  // actually optimised (user IR lacks the runtime bodies, has different
  // attribute groups, etc.).  With `emitCombinedOnS` (default true) the
  // -S path now produces the combined module the way the full pipeline
  // does.  `--legacy-S` restores the old behaviour for differential tests.
  bool irOnly = !g_opts.irOut.empty();
  std::string userLl = irOnly ? g_opts.irOut
      : buildDir + "/user_" + std::to_string(getpid()) + ".ll";

  if (!irgenProgram(prog, userLl) || errorCount()) {
    printDiags();
    return 1;
  }

  // Resolve --emit-combined-ir target.  "<auto>" => <out>.combined.ll
  std::string combinedOutPath;
  if (!g_opts.emitCombinedIr.empty()) {
    combinedOutPath = g_opts.emitCombinedIr;
    if (combinedOutPath == "<auto>") {
      combinedOutPath = base + ".combined.ll";
    }
  }

  // Stage A3: when -S is given and the user did not opt out, treat the
  // requested .ll as the COMBINED module (write runtime IR into it).  This
  // is the right default for analysis — but we still write the user-only
  // IR to <out>.user.ll if --save-temps was requested, so diffing is easy.
  bool emitCombinedForS = irOnly && g_opts.emitCombinedOnS;

  if (irOnly && !emitCombinedForS) {
    printf("penstock: IR written to %s\n", userLl.c_str());
    return 0;
  }

  // ---------------------------------------------------------- 6. combine
  std::string rtSrc = runtimeSourcePath();
  std::string rtLl = runtimeLl(clang, buildDir, rtSrc, llvmMajor);
  if (rtLl.empty()) {
    // -S with no available clang to compile the runtime: fall back to the
    // user-only IR rather than fail hard, but warn loudly.
    if (irOnly) {
      fprintf(stderr, "penstock: warning: --emit-combined-ir requested but "
                      "runtime .ll could not be produced (no working clang); "
                      "writing user IR only to %s\n", userLl.c_str());
      printf("penstock: IR written to %s\n", userLl.c_str());
      return 0;
    }
    return 1;
  }

  std::string out = g_opts.output;
  std::string finalLl;
  if (emitCombinedForS) {
    // -S mode: write combined IR to the user-supplied path (userLl).
    finalLl = userLl;
  } else {
    finalLl = buildDir + "/combined_" + std::to_string(getpid()) + ".ll";
  }
  // If --save-temps, keep the user-only IR alongside.
  if (g_opts.saveTemps && !irOnly) {
    writeFile(base + ".user.ll", readFile(userLl));
  }
  std::string userIr = stripUserRtDecls(readFile(userLl));
  std::string combined = userIr +
                         "\n; ---- freshet runtime (libfreshet) ----\n" +
                         stripRuntimeHeader(rtLl, userIntrinsicSet(userIr));
  if (!writeFile(finalLl, combined)) {
    fprintf(stderr, "penstock: cannot write %s\n", finalLl.c_str());
    return 1;
  }

  // Stage A2: --emit-combined-ir = always also drop a copy at the
  // requested path (separate from the build/ scratch file).
  if (!combinedOutPath.empty() && combinedOutPath != finalLl) {
    if (!writeFile(combinedOutPath, combined)) {
      fprintf(stderr, "penstock: warning: cannot write --emit-combined-ir "
                      "target %s\n", combinedOutPath.c_str());
    } else {
      fprintf(stderr, "penstock: combined IR written to %s\n",
              combinedOutPath.c_str());
    }
  }

  // Stage A3: -S now returns here, having written the COMBINED IR to userLl.
  if (irOnly) {
    printf("penstock: combined IR written to %s\n", userLl.c_str());
    if (g_opts.saveTemps) {
      std::string userOnly = base + ".user.ll";
      writeFile(userOnly, userIr);
      printf("penstock: user-only IR kept at %s\n", userOnly.c_str());
    }
    return 0;
  }

  if (g_opts.profile) {
    fprintf(stderr, "penstock: PGO instrumented build — after running, "
                    "profile lands in <exe>.fprof (or $FRT_PROF_OUT)\n");
  }

  // Stage 6.4: exhaustive finite-domain algorithm rewriting. For small pure
  // integer functions, build a complete input/output map (including inverse
  // search routines whose input is a desired output), propose a constant-table
  // replacement, and install it only after Alive2 proves the whole-module
  // rewrite. -O3 is deliberately tiny; -O4 permits a larger finite domain.
  // Solver/tool failures and evaluation timeouts always leave the IR unchanged.
  if (g_opts.optLevel >= 3 && !g_opts.profile && !g_opts.noPrecompute) {
    bool changed = runPrecomputeRewriter(
        clang, llvmRoot, finalLl, g_opts.optLevel, g_opts.superoptBudgetSec);
    if (changed && !combinedOutPath.empty())
      writeFile(combinedOutPath, readFile(finalLl));
  }

  // Stage 6.5: Freshet-hosted, in-process instruction superoptimisation.
  // The Freshet control loop is compiled during the CMake bootstrap; its
  // native bridge uses embedded Z3 for bit-vector obligations and Alive2 for
  // LLVM refinement validation. No Souper pass or alive-tv executable is
  // required at runtime.
  if (g_opts.optLevel >= 3 && !g_opts.profile && !g_opts.noSuperopt) {
    bool changed = runVerifiedSuperoptimizer(
        llvmRoot, finalLl, g_opts.optLevel, g_opts.superoptBudgetSec);
    if (changed && !combinedOutPath.empty())
      writeFile(combinedOutPath, readFile(finalLl));
  }

  // ---------------------------------------------------------- 7. backend
  // Phase 3c (report §10.3): -g / --debug adds DWARF emission — the IR
  // already carries !dbg line locations + DISubprograms, so the backend
  // only needs the flag to materialise them
  std::string dbgFlag = g_opts.debugInfo ? " -g" : "";
  // project library targets compile to ONE object, then archive (static)
  // or link shared — handled after the emitObj branch
  if (g_opts.emitObj) {
    int clangO = g_opts.optLevel > 3 ? 3 : g_opts.optLevel;
    std::string cmd = clang + " -O" + std::to_string(clangO) +
                      " -march=" + g_opts.march + targetFlags() + veclibFlags() +
                      llvmPgoFlags() + dbgFlag + " -c " + finalLl +
                      " -o " + out + optRemarksFlags(out) + " 2>&1";
    int rc = runCmd(cmd);
    if (!g_opts.keepIr) { unlink(finalLl.c_str()); unlink(userLl.c_str()); }
    if (rc) { fprintf(stderr, "penstock: clang failed on object emit\n"); return 1; }
    // Phase 3 (§6.2): project library targets — object -> archive / .so
    if (libTarget) {
      std::string lcmd;
      if (g_opts.projectTarget == "static-lib")
        lcmd = "rm -f " + libOut + " && ar rcs " + libOut + " " + out;
      else
        lcmd = clang + " -shared -o " + libOut + " " + out + " -lm -lpthread 2>&1";
      if (runCmd(lcmd) == 0) {
        fprintf(stderr, "penstock: wrote %s\n", libOut.c_str());
        return 0;
      }
      fprintf(stderr, "penstock: library link failed (%s)\n",
              g_opts.projectTarget.c_str());
      return 1;
    }
    return 0;
  }

  {
    // -O4 = -O3 codegen + evolution pass (run after link below); clang
    // itself caps at -O3
    int clangO = g_opts.optLevel > 3 ? 3 : g_opts.optLevel;
    std::string llibs = " -lm -lpthread";
    // #link declarations are collected by the preprocessor.  Resolve them at
    // the final native link: simple identifiers become -lNAME, with the
    // current directory searched first so local C shims can be packaged as
    // libNAME.a.  Compiler command paths use the existing shell-command model.
    if (!linkLibs.empty()) llibs += " -L.";
    for (const auto& lib : linkLibs) {
      if (lib.empty()) continue;
      if (lib[0] == '-') llibs += " " + lib;
      else llibs += " -l" + lib;
    }
    std::string cmd = clang + " -O" + std::to_string(clangO) +
                      " -march=" + g_opts.march + targetFlags() + veclibFlags() +
                      llvmPgoFlags() + dbgFlag + " " + finalLl + " -o " + out +
                      llibs + optRemarksFlags(out) + " 2>&1";
    int rc = runCmd(cmd);
    if (rc) {
      fprintf(stderr, "penstock: backend failed; IR kept at %s\n",
              finalLl.c_str());
      return 1;
    }
  }

  // §13.1: --emit-final-ir — drop the final OPTIMISED IR next to the
  // binary.  This is the artefact the backend actually produced (unlike
  // --emit-combined-ir, which is the INPUT to the backend), so vectorised
  // loops, inlined bodies and lowered intrinsics are visible for
  // inspection / -Rpass cross-checks.
  if (!g_opts.emitFinalIr.empty()) {
    std::string finalIrPath = g_opts.emitFinalIr;
    if (finalIrPath == "<auto>") finalIrPath = base + ".final.ll";
    int clangO = g_opts.optLevel > 3 ? 3 : g_opts.optLevel;
    std::string cmd = clang + " -S -emit-llvm -O" + std::to_string(clangO) +
                      " -march=" + g_opts.march + targetFlags() + veclibFlags() +
                      " " + finalLl + " -o " + finalIrPath + " 2>&1";
    if (runCmd(cmd) == 0)
      fprintf(stderr, "penstock: final optimised IR written to %s\n",
              finalIrPath.c_str());
    else
      fprintf(stderr, "penstock: warning: --emit-final-ir backend pass "
                      "failed\n");
  }

  // -------------------------------------------- 7.5 -O4 evolution engine
  if (g_opts.optLevel == 4 && !g_opts.profile && g_opts.evoRuntimeTune) {
    EvoWinner w;
    if (evoOptimize(clang, finalLl, out, g_opts.evoBudgetSec, g_opts.evoSeed, &w) &&
        w.improved) {
      // bake: relink the final binary with the winning clang flags and a
      // runtime compiled with -DFRT_TUNED_* defaults so the tuning holds
      // without env vars
      std::string tunedRt = buildDir + "/frt_rt_tuned_" +
                            std::to_string(getpid()) + ".ll";
      // §3: the tuned runtime uses the SAME optimisation level as the
      // cached runtime (default -O2), not the historical -O0
      std::string tunedDefs =
          "-DFRT_TUNED_THREADS=" + std::to_string(w.threads) +
          " -DFRT_TUNED_AFFINITY=" + std::to_string(w.pin) +
          " -DFRT_TUNED_CHUNK=" + std::to_string(w.chunk);
      if (w.serialCutoff >= 0)   // -1 = leave the default heuristic
        tunedDefs += " -DFRT_TUNED_SERIAL_CUTOFF=" +
                     std::to_string(w.serialCutoff);
      std::string dcmd = clang + " -S -emit-llvm -O" +
          std::to_string(g_opts.runtimeOpt) + " -g0 -x c " + tunedDefs +
          " " + rtSrc + " -o " + tunedRt + " 2>/dev/null";
      if (runCmd(dcmd) != 0 || readFile(tunedRt).empty()) {
        fprintf(stderr, "evo: tuned runtime compile failed — keeping "
                        "default binary (set env vars instead: "
                        "FRT_THREADS=%d FRT_AFFINITY=%d FRT_CHUNK=%ld)\n",
                w.threads, w.pin, w.chunk);
      } else {
        std::string userIr2 = stripUserRtDecls(readFile(userLl));
        std::string combined2 = userIr2 +
            "\n; ---- freshet runtime (libfreshet, -O4 tuned) ----\n" +
            stripRuntimeHeader(readFile(tunedRt), userIntrinsicSet(userIr2));
        std::string final2 = buildDir + "/combined_tuned_" +
                             std::to_string(getpid()) + ".ll";
        if (!writeFile(final2, combined2)) {
          fprintf(stderr, "penstock: cannot write %s\n", final2.c_str());
        } else {
          std::string cmd2 = clang + " -O" + std::to_string(w.clangOpt) +
              " -march=" + w.march + targetFlags() +
              (w.veclib ? " -fveclib=libmvec -fno-math-errno" : "") +
              " " + final2 + " -o " + out + " -lm -lpthread 2>&1";
          if (runCmd(cmd2) == 0) {
            fprintf(stderr, "evo: tuning baked into %s (reproduce with: "
                            "FRT_THREADS=%d FRT_AFFINITY=%d FRT_CHUNK=%ld "
                            "FRT_SERIAL_CUTOFF=%ld%s)\n",
                    out.c_str(), w.threads, w.pin, w.chunk, w.serialCutoff,
                    w.veclib ? " + --veclib=libmvec" : "");
            if (g_opts.keepIr) {
              std::string keep = out + ".evo.ll";
              writeFile(keep, combined2);
              fprintf(stderr, "evo: tuned IR kept at %s\n", keep.c_str());
            }
          } else {
            fprintf(stderr, "evo: tuned relink failed — keeping default "
                            "binary; reproduce with: FRT_THREADS=%d "
                            "FRT_AFFINITY=%d FRT_CHUNK=%ld\n",
                    w.threads, w.pin, w.chunk);
          }
          unlink(final2.c_str());
        }
      }
      unlink(tunedRt.c_str());
    }
  } else if (g_opts.optLevel == 4 && g_opts.profile) {
    fprintf(stderr, "penstock: -O4 runtime timing search skipped on --profile "
                    "builds (instrumented timing is meaningless)\n");
  } else if (g_opts.optLevel == 4) {
    fprintf(stderr, "evo: legacy execute-and-time runtime tuning is disabled "
                    "by default; use --evo-runtime-tune to opt in. The -O4 "
                    "superoptimisation stage uses solver-synthesised IR "
                    "rewrites with Alive2 validation instead.\n");
  }

  // ---------------------------------------------------------- 8. run/report
  // record the successful build in the project cache (§6.2)
  if (!cachePath.empty())
    writeFile(cachePath, std::to_string(buildKey));
  // sh needs an explicit path for outputs without a directory component
  auto runPath = [](const std::string& o) {
    return o.find('/') == std::string::npos ? "./" + o : o;
  };
  if (g_opts.report) {
    std::string cmd = "FRT_TIMING=1 " + runPath(out);
    runCmd(cmd);
    mergeAndPrintReport(prog, finalLl, out);
  }
  if (g_opts.run) runCmd(runPath(out));
  // Phase 4 (§10.3): `penstock test` executes the harness and mirrors its
  // exit code (0 = all tests passed, 1 = at least one failure)
  if (g_opts.testMode) {
    fflush(stdout);
    return runCmd(runPath(out));
  }

  if (!g_opts.keepIr) { unlink(finalLl.c_str()); unlink(userLl.c_str()); }
  return 0;
}
