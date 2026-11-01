// Penstock solver-backed LLVM IR superoptimisation.
//
// Souper synthesises cheaper, equivalent instruction expressions with an SMT
// solver.  We deliberately treat its output as an UNTRUSTED candidate: it is
// only accepted when Alive2 proves refinement AND the candidate wins on
// the target-aware cost vector (optimisation-audit §7.1) — a shorter
// sequence with a long-latency divide or an extra load no longer wins on
// length alone.  All tools are optional because Souper's pass plugin must be
// built against the exact LLVM version in use.
#include "common.h"
#include "opt_util.h"   // §7.2: proofLogLine for the extraction certificate
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace freshet {
namespace {

static std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return {};
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

static std::string quote(const std::string& s) {
  std::string q = "'";
  for (char c : s) {
    if (c == '\'') q += "'\\''";
    else q += c;
  }
  q += "'";
  return q;
}

static std::string readText(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

static bool writeText(const std::string& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out << text;
  return out.good();
}

static std::string findCommand(const char* envName, const std::string& fallback,
                               const std::string& llvmRoot = {}) {
  const char* e = std::getenv(envName);
  if (e && *e) {
    std::string p(e);
    if (access(p.c_str(), X_OK) == 0) return p;
    if (p.find('/') == std::string::npos) {
      FILE* fp = popen(("command -v " + quote(p) + " 2>/dev/null").c_str(), "r");
      if (fp) {
        char buf[2048];
        std::string resolved;
        if (fgets(buf, sizeof buf, fp)) resolved = trim(buf);
        pclose(fp);
        if (!resolved.empty() && access(resolved.c_str(), X_OK) == 0) return resolved;
      }
    }
    return {};
  }
  if (!fallback.empty() && access(fallback.c_str(), X_OK) == 0) return fallback;
  if (!llvmRoot.empty()) {
    std::string p = llvmRoot + "/bin/" + fallback;
    if (access(p.c_str(), X_OK) == 0) return p;
  }
  // The fallback is an executable name, not user input.  Ask the shell's
  // PATH resolver and then retain the absolute result for quoting.
  std::string cmd = "command -v " + quote(fallback) + " 2>/dev/null";
  FILE* fp = popen(cmd.c_str(), "r");
  if (!fp) return {};
  char buf[2048];
  std::string got;
  if (fgets(buf, sizeof buf, fp)) got = trim(buf);
  pclose(fp);
  if (!got.empty() && access(got.c_str(), X_OK) == 0) return got;
  return {};
}

static std::string findSouperPass(const std::string& llvmRoot) {
  const char* env = std::getenv("PENSTOCK_SOUPER_PASS");
  if (!env || !*env) env = std::getenv("SOUPER_PASS");
  if (env && *env) return access(env, R_OK) == 0 ? std::string(env) : std::string();
  std::vector<std::string> candidates = {
    llvmRoot + "/lib/libsouperPass.so",
    llvmRoot + "/lib/Souper/libsouperPass.so",
    llvmRoot + "/lib/libsouper.so",
    "/usr/local/lib/libsouperPass.so",
    "/usr/lib/libsouperPass.so"
  };
  for (const auto& p : candidates)
    if (!p.empty() && access(p.c_str(), R_OK) == 0) return p;
  return {};
}

static size_t countInstructions(const std::string& ir) {
  std::istringstream in(ir);
  std::string line;
  bool inFn = false;
  size_t count = 0;
  while (std::getline(in, line)) {
    std::string t = trim(line);
    if (!inFn) {
      if (t.rfind("define ", 0) == 0) inFn = true;
      continue;
    }
    if (t == "}") { inFn = false; continue; }
    if (t.empty() || t[0] == ';' || t.back() == ':') continue;
    // LLVM instructions are the only non-label, non-comment lines inside a
    // function body; metadata is normally outside the closing brace.
    ++count;
  }
  return count;
}

// ------------------------------------------------------------- cost model
// optimisation-audit §7.1: instruction COUNT is not a performance
// objective.  A rewrite that is provably equivalent (Alive2) can still be
// SLOWER — a shorter sequence that introduces a long-latency divide, a
// serial dependency, or an extra load must not win on length alone.
// The acceptance metric is now a target-dependent COST VECTOR:
//   (instructions, latencyScore, memOps, branchOps)
// Penstock drives an external Clang/LLVM (no linkable
// TargetTransformInfo), so the per-opcode latency weights below are a
// DOCUMENTED static approximation of a typical out-of-order x86-64 core;
// --superopt-objective selects which component dominates the decision:
//   latency  — weighted latency must strictly improve; code growth is
//              capped at +25% (register pressure / i-cache guard)
//   size     — instruction count must strictly improve and latency must
//              not regress (no divide creep for shorter code)
//   balanced — either of the two above conditions
struct IrCost {
  size_t instructions = 0;
  int64_t latency = 0;
  int64_t memOps = 0;
  int64_t branchOps = 0;
};

static void costInstruction(const std::string& t, IrCost* c) {
  ++c->instructions;
  // the opcode follows the result-register assignment "  %x = op ..."
  size_t opAt = t.find("= ");
  std::string op = opAt == std::string::npos ? t : t.substr(opAt + 2);
  auto has = [&](const char* k) { return op.rfind(k, 0) == 0; };
  if (has("sdiv") || has("udiv") || has("fdiv")) c->latency += 20;
  else if (has("srem") || has("urem")) c->latency += 26;
  else if (has("mul") || has("fmul")) c->latency += 5;
  else if (has("call")) c->latency += 12;
  else if (has("load") || has("store")) { c->latency += 4; ++c->memOps; }
  else if (has("getelementptr")) c->latency += 2;
  else if (has("icmp") || has("fcmp") || has("select")) c->latency += 2;
  else if (has("br") || has("switch") || has("invoke")) {
    c->latency += 2;
    ++c->branchOps;
  } else c->latency += 1;
}

static IrCost costOf(const std::string& ir) {
  IrCost c;
  std::istringstream in(ir);
  std::string line;
  bool inFn = false;
  while (std::getline(in, line)) {
    std::string t = trim(line);
    if (!inFn) {
      if (t.rfind("define ", 0) == 0) inFn = true;
      continue;
    }
    if (t == "}") { inFn = false; continue; }
    if (t.empty() || t[0] == ';' || t.back() == ':') continue;
    costInstruction(t, &c);
  }
  return c;
}

static std::string costString(const IrCost& c) {
  return "{" + std::to_string(c.instructions) + " inst, latency " +
         std::to_string(c.latency) + ", mem " + std::to_string(c.memOps) +
         ", br " + std::to_string(c.branchOps) + "}";
}

// §7.1/§7.2 objective comparison: does the candidate beat the incumbent
// under the selected objective?  This is the C++ mirror of the Lean
// kernel's cost-based selector (lean/PenstockProofs.lean,
// `selectMinCost`): the accepted candidate is the minimum-cost member of
// the verified candidate list {incumbent, Souper candidate}.
static bool candidateWins(const IrCost& before, const IrCost& after,
                          const std::string& objective) {
  if (objective == "size")
    return after.instructions < before.instructions &&
           after.latency <= before.latency;
  if (objective == "balanced")
    return (after.instructions < before.instructions &&
            after.latency <= before.latency) ||
           (after.latency < before.latency &&
            after.instructions <= before.instructions);
  // default: latency
  return after.latency < before.latency &&
         after.instructions <= before.instructions +
                                   before.instructions / 4;   // +25% cap
}

static bool runOK(const std::string& command) {
  fflush(nullptr);
  int rc = std::system(command.c_str());
  return rc != -1 && WIFEXITED(rc) && WEXITSTATUS(rc) == 0;
}

static bool aliveProves(const std::string& alive, const std::string& timeout,
                        const std::string& before, const std::string& after,
                        const std::string& log, int budget) {
  std::string cmd = quote(timeout) + " " + std::to_string(budget) + "s " +
      quote(alive) + " " + quote(before) + " " + quote(after) + " > " +
      quote(log) + " 2>&1";
  bool rc = runOK(cmd);
  const std::string text = readText(log);
  // Don't rely on exit status alone: Alive2 versions have differed in their
  // diagnostics/exit conventions.  Only accept its explicit positive result.
  return rc && text.find("Transformation seems to be correct!") != std::string::npos &&
         text.find("Transformation doesn't verify!") == std::string::npos &&
         text.find("ERROR:") == std::string::npos;
}

} // namespace

bool runVerifiedSuperoptimizer(const std::string& llvmRoot,
                               const std::string& finalLl, int optLevel,
                               int budgetSec) {
  const std::string beforeText = readText(finalLl);
  if (beforeText.empty()) {
    fprintf(stderr, "superopt: skipped — combined IR is unreadable\n");
    return false;
  }

  const bool wide = optLevel >= 4;
  const size_t byteLimit = wide ? 4u * 1024u * 1024u : 256u * 1024u;
  const size_t instructionLimit = wide ? 24000u : 2400u;
  const size_t ninst = countInstructions(beforeText);
  if (beforeText.size() > byteLimit || ninst > instructionLimit) {
    fprintf(stderr, "superopt: skipped — module exceeds the %s scope budget "
                    "(%zu bytes, %zu instructions; limits %zu / %zu)\n",
            wide ? "-O4" : "-O3", beforeText.size(), ninst,
            byteLimit, instructionLimit);
    return false;
  }

  std::string opt = findCommand("PENSTOCK_OPT", "opt", llvmRoot);
  std::string alive = findCommand("PENSTOCK_ALIVE_TV", "alive-tv");
  std::string timeout = findCommand("PENSTOCK_TIMEOUT", "timeout");
  std::string z3 = findCommand("PENSTOCK_Z3", "z3");
  std::string plugin = findSouperPass(llvmRoot);
  if (opt.empty() || alive.empty() || timeout.empty() || z3.empty() || plugin.empty()) {
    fprintf(stderr,
      "superopt: solver pass unavailable — requires opt, z3, Alive2's alive-tv, "
      "and a Souper pass plugin built for this LLVM (set PENSTOCK_OPT, "
      "PENSTOCK_Z3, PENSTOCK_ALIVE_TV, PENSTOCK_TIMEOUT, "
      "PENSTOCK_SOUPER_PASS); keeping LLVM input unchanged\n");
    return false;
  }

  if (budgetSec <= 0) budgetSec = wide ? 10 : 2;
  const std::string base = finalLl + ".superopt." + std::to_string(getpid());
  const std::string before = base + ".before.ll";
  const std::string candidate = base + ".candidate.ll";
  const std::string optLog = base + ".opt.log";
  const std::string verifyLog = base + ".alive.log";
  auto cleanup = [&]() {
    unlink(before.c_str()); unlink(candidate.c_str());
    unlink(optLog.c_str()); unlink(verifyLog.c_str());
  };
  if (!writeText(before, beforeText)) {
    fprintf(stderr, "superopt: cannot create scratch module; skipping\n");
    cleanup();
    return false;
  }

  // Souper's public pass uses the legacy opt pass manager in current upstream
  // builds.  Try the explicit legacy-manager form first, then the compatible
  // spelling used by older opt binaries.  A failed invocation never changes
  // the incumbent IR.
  const std::string prefix = quote(timeout) + " " + std::to_string(budgetSec) + "s " +
      quote(opt) + " -load=" + quote(plugin) + " ";
  std::string cmd = prefix + "-enable-new-pm=0 -souper -z3-path=" + quote(z3) +
      " -S " + quote(before) + " -o " + quote(candidate) + " > " +
      quote(optLog) + " 2>&1";
  bool optimized = runOK(cmd);
  if (!optimized) {
    unlink(candidate.c_str());
    cmd = prefix + "-souper -z3-path=" + quote(z3) + " -S " + quote(before) +
        " -o " + quote(candidate) + " > " + quote(optLog) + " 2>&1";
    optimized = runOK(cmd);
  }
  const std::string candidateText = readText(candidate);
  if (!optimized || candidateText.empty()) {
    fprintf(stderr, "superopt: Souper did not produce a candidate; keeping "
                    "the existing IR (tool log: %s)\n", optLog.c_str());
    if (!g_opts.keepIr) cleanup();
    else fprintf(stderr, "superopt: diagnostic log kept at %s\n", optLog.c_str());
    return false;
  }

  const IrCost beforeCost = costOf(beforeText);
  const IrCost afterCost = costOf(candidateText);
  // §7.1: the objective-aware cost vector decides; a mere instruction-count
  // decrease no longer accepts a latency/branch/memory regression
  if (!candidateWins(beforeCost, afterCost, g_opts.superoptObjective)) {
    fprintf(stderr, "superopt: rejected candidate on the cost vector — "
                    "incumbent %s vs candidate %s under objective '%s'\n",
            costString(beforeCost).c_str(), costString(afterCost).c_str(),
            g_opts.superoptObjective.c_str());
    if (!g_opts.keepIr) cleanup();
    return false;
  }
  if (!aliveProves(alive, timeout, before, candidate, verifyLog, budgetSec)) {
    fprintf(stderr, "superopt: rejected candidate — Alive2 did not prove "
                    "equivalence/refinement; original IR retained\n");
    if (g_opts.keepIr)
      fprintf(stderr, "superopt: verification log kept at %s\n", verifyLog.c_str());
    else cleanup();
    return false;
  }

  // Atomic rename in the same directory preserves the incumbent if any
  // previous step failed.  The verified candidate is the only path that can
  // replace the compiler input.
  const std::string accepted = base + ".accepted.ll";
  if (!writeText(accepted, candidateText) || rename(accepted.c_str(), finalLl.c_str()) != 0) {
    unlink(accepted.c_str());
    fprintf(stderr, "superopt: cannot install verified IR; original retained\n");
    cleanup();
    return false;
  }
  fprintf(stderr, "superopt: Alive2-verified SMT rewrite accepted under "
                  "objective '%s' (%s -> %s; scope=%s) — minimum-cost "
                  "extraction over the verified candidate list "
                  "{incumbent, souper}\n",
          g_opts.superoptObjective.c_str(),
          costString(beforeCost).c_str(), costString(afterCost).c_str(),
          wide ? "whole module" : "bounded small module");
  {
    // §7.2 bounded-extraction certificate line: same shape the Lean
    // CertifiedCandidate/selectMinCost theorem reasons about
    FnDecl* noFn = nullptr;
    SrcLoc noLoc{};
    optutil::proofLogLine("superopt",
                 "alive2:Transformation-seems-to-be-correct;"
                 "objective:" + g_opts.superoptObjective +
                 ";incumbent-cost:" + costString(beforeCost) +
                 ";candidate-cost:" + costString(afterCost) +
                 ";extraction:min-over-verified-candidate-list",
                 noFn, noLoc);
  }
  cleanup();
  return true;
}

} // namespace freshet
