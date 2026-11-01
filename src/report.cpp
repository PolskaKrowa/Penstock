// Freshet performance report — merges four layers:
//   1. frontend diagnostics statistics (E-*/W-* tallies, struct layout table)
//   2. IR metrics (parse .ll text: vectorizable loops, reductions, branch
//      weights, hot fns, parfor chunk fns)
//   3. runtime timing (FRT_TIMING=1 makes the binary write <exe>.frtiming;
//      tags correlate to fn:line via IrGen::parforTags)
//   4. PGO layer (top functions by profile count, coverage of branch sites)
#include "irgen.h"
#include <algorithm>
#include <fstream>
#include <sstream>

namespace freshet {

extern IrGen* G;

static std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

struct IrMetrics {
  int nFnDefs = 0;          // user fns (u_*/main/exported)
  int nChunkFns = 0;        // __pf_body_*
  int nVectorLoops = 0;     // llvm.loop vectorize.enable
  int nInterleave4 = 0;     // interleave.count 4
  int nBranchWeights = 0;   // !prof branch_weights (PGO)
  int nFastMath = 0;        // fadd/fsub/fmul/fast
  int nFpReductions = 0;    // fadd fast inside chunk fns (approx)
  int nNoAlias = 0;         // noalias params
  int nHotFns = 0;          // hot attribute
  int nLoadStore = 0;       // memory ops
  int nGep = 0;             // address computations
  int nSoaCalls = 0;        // frt_soa_*
  int nAtomicRmw = 0;       // atomicrmw (thread-safe PGO counters)
  bool inChunkFn = false;
};

static IrMetrics scanIr(const std::string& irPath) {
  IrMetrics m;
  std::ifstream f(irPath);
  if (!f.is_open()) return m;
  std::string line;
  while (std::getline(f, line)) {
    std::string t = trim(line);
    if (t.rfind("define ", 0) == 0) {
      m.inChunkFn = t.find("@__pf_body_") != std::string::npos;
      if (m.inChunkFn) {
        ++m.nFnDefs;
        ++m.nChunkFns;
      }
      continue;
    }
    if (t.rfind("declare ", 0) == 0) continue;
    if (t.find("llvm.loop.vectorize.enable") != std::string::npos)
      ++m.nVectorLoops;
    if (t.find("llvm.loop.interleave.count\", i32 4") != std::string::npos)
      ++m.nInterleave4;
    if (t.find("branch_weights") != std::string::npos) ++m.nBranchWeights;
    if (t.find("noalias") != std::string::npos) ++m.nNoAlias;
    if (t.find(" hot ") == 0 || t.find(" hot#") != std::string::npos) {
      // function def line already counted; skip
    }
    if (t.find("\"function_entry_count\"") != std::string::npos) {
      // prof metadata on define line
    }
    if (t.rfind("attributes", 0) == 0) continue;
    if (t.find("= atomicrmw ") != std::string::npos) ++m.nAtomicRmw;
    // instruction lines start with %
    if (!t.empty() && t[0] == '%') {
      if (t.find(" = fadd ") != std::string::npos ||
          t.find(" = fsub ") != std::string::npos ||
          t.find(" = fmul ") != std::string::npos) {
        if (t.find(" fast") != std::string::npos) {
          ++m.nFastMath;
          if (m.inChunkFn) ++m.nFpReductions;
        }
      }
      if (t.find(" = load ") != std::string::npos) ++m.nLoadStore;
      if (t.find(" = getelementptr ") != std::string::npos) ++m.nGep;
    }
    if (t.find(" = load ") == 0 || t.find(" = store ") == 0) ++m.nLoadStore;
    if (t.find("call") != std::string::npos && t.find("@frt_soa_") != std::string::npos)
      ++m.nSoaCalls;
  }
  return m;
}

// ---- runtime timing file ------------------------------------------------
// FRESHET-TIMING 1
// PF <tag> <cycles> <count> <nchunks> <pinfails>
// ST <serial> <dispatched>     (§10.1 scheduler stats; absent on old runs)
struct TimingEnt {
  uint64_t tag, cycles;
  int64_t count, nchunks;
  int pinfails;
};

struct TimingStats {
  int64_t serial = 0, dispatched = 0;
  bool present = false;
};

static std::vector<TimingEnt> readTiming(const std::string& binPath,
                                         TimingStats* stats) {
  std::vector<TimingEnt> out;
  std::string path = binPath + ".frtiming";
  std::ifstream f(path);
  if (!f.is_open()) return out;
  std::string magic;
  std::getline(f, magic);
  if (magic.find("FRESHET-TIMING") == std::string::npos) return out;
  std::string kind;
  while (f >> kind) {
    if (kind == "ST") {
      if (stats && (f >> stats->serial >> stats->dispatched)) stats->present = true;
      continue;
    }
    if (kind != "PF") { continue; }
    TimingEnt t;
    if (!(f >> t.tag >> t.cycles >> t.count >> t.nchunks >> t.pinfails)) break;
    out.push_back(t);
  }
  return out;
}

// ---- backend optimisation-record YAML (§13.1) -----------------------------
// Counts ACTUAL backend transformations from the -fsave-optimization-record
// YAML written next to the binary when --opt-remarks was given.  This is
// the truth source the audit asks for: "loop has vectorise metadata" is a
// static estimate; "loop vectorised with VF=8" is a backend result.
struct RemarkMetrics {
  int nVectorized = 0;       // loop-vectorize / slp-vectorizer successes
  int nMissed = 0;           // vectorisation misses
  int nInlined = 0;          // inliner successes
  int nInlineMissed = 0;
  std::vector<std::string> missedReasons;
  std::vector<std::string> vectorizedDetail;  // "fn:line VFxIF"
};

static std::string trim2(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r");
  return s.substr(a, b - a + 1);
}
static std::string unquote(const std::string& s) {
  if (s.size() >= 2 && s.front() == '\'' && s.back() == '\'')
    return s.substr(1, s.size() - 2);
  return s;
}

static RemarkMetrics readOptRecords(const std::string& binPath) {
  RemarkMetrics m;
  std::ifstream f(binPath + ".opt-records.yaml");
  if (!f.is_open()) return m;
  // Clang's opt-record YAML is a stream of documents:
  //   --- !Passed / !Missed / !Analysis
  //   Pass: 'loop-vectorize'
  //   Name: 'VectorizedLoop'
  //   DebugLoc: { File: ..., Line: N, ... }
  //   Function: name
  //   Args: [...]
  // We parse line-wise and tolerantly (formats vary across versions).
  std::string line;
  std::string tag, pass, name, fn, argStr;
  int lineNo = 0;
  auto flush = [&]() {
    if (tag.empty() || pass.empty()) { tag = pass = name = fn = argStr = ""; lineNo = 0; return; }
    bool isVec = pass.find("vectorize") != std::string::npos;
    bool isInl = pass.find("inline") != std::string::npos;
    if (isVec) {
      if (tag == "!Passed") {
        ++m.nVectorized;
        std::string d = fn;
        if (lineNo > 0) d += ":" + std::to_string(lineNo);
        // capture VF/interleave from args when present
        std::string vf, ic;
        size_t p = argStr.find("'vector width'");
        if (p == std::string::npos) p = argStr.find("vector-width");
        if (p != std::string::npos) {
          size_t q = argStr.find("'", argStr.find(':', p) + 1);
          size_t r = argStr.find(',', argStr.find(':', p));
          vf = trim2(argStr.substr(argStr.find(':', p) + 1,
                                   std::min(q == std::string::npos ? r : q, r) -
                                       argStr.find(':', p) - 1));
        }
        if (!vf.empty()) d += " VF=" + vf;
        if (m.vectorizedDetail.size() < 12) m.vectorizedDetail.push_back(d);
      } else if (tag == "!Missed") {
        ++m.nMissed;
        if (m.missedReasons.size() < 8)
          m.missedReasons.push_back(fn + (lineNo ? ":" + std::to_string(lineNo)
                                                : "") +
                                    " " + name +
                                    (argStr.empty() ? "" : " " + argStr));
      }
    } else if (isInl) {
      if (tag == "!Passed") ++m.nInlined;
      else if (tag == "!Missed") ++m.nInlineMissed;
    }
    tag = pass = name = fn = argStr = "";
    lineNo = 0;
  };
  while (std::getline(f, line)) {
    std::string t = trim2(line);
    if (t.rfind("--- !", 0) == 0) {
      flush();
      tag = t.substr(4);
      continue;
    }
    if (t.rfind("Pass:", 0) == 0)
      pass = unquote(trim2(t.substr(5)));
    else if (t.rfind("Name:", 0) == 0)
      name = unquote(trim2(t.substr(5)));
    else if (t.rfind("Function:", 0) == 0)
      fn = unquote(trim2(t.substr(9)));
    else if (t.rfind("Line:", 0) == 0)
      lineNo = atoi(t.c_str() + 5);
    else if (t.rfind("Args:", 0) == 0)
      argStr = t.substr(5);
  }
  flush();
  return m;
}

// ---- report -------------------------------------------------------------
void mergeAndPrintReport(Program* p, const std::string& irPath,
                         const std::string& binPath) {
  std::ostringstream R;
  R << "\n==================== Freshet Performance Report ====================\n";
  R << "input    : " << p->file << "\n";
  R << "backend  : " << g_backend << "\n";
  R << "mode     : -O" << g_opts.optLevel << " -march=" << g_opts.march;
  if (!g_targetTriple.empty()) R << " -target " << g_targetTriple;
  R << "  (runtime IR -O" << g_opts.runtimeOpt << ")";
  if (g_opts.forceVectorize) R << "  [forced vectorize hints]";
  if (g_opts.veclib != "none") R << "  [veclib=" << g_opts.veclib << "]";
  if (g_opts.profile) R << "  [PGO-instrument]";
  if (!g_opts.useProfile.empty()) R << "  [PGO-guided: " << g_opts.useProfile << "]";
  if (g_opts.llvmPgoGenerate) R << "  [LLVM-PGO training]";
  if (!g_opts.llvmPgoUse.empty()) R << "  [LLVM-PGO use]";
  R << "\n";
  // §13.1: metrics are labelled [static] (front-end estimate), [backend]
  // (transformation LLVM actually performed) or [runtime] (measured).

  // ---------------- layer 1: frontend
  R << "\n---- [1] frontend diagnostics -------------------------------------\n";
  std::map<std::string, int> byCode;
  int nerr = 0, nwarn = 0;
  for (auto& d : g_diags) {
    byCode[d.code]++;
    if (d.sev == Diag::Error) nerr++;
    else if (d.sev == Diag::Warning) nwarn++;
  }
  R << "errors: " << nerr << "   warnings: " << nwarn << "\n";
  for (auto& [code, n] : byCode)
    R << "  " << code << " (" << findCode(code)->slug << ") x" << n << "\n";

  // struct layout table
  if (!p->structs.empty()) {
    std::ostringstream S;
    for (auto* sd : p->structs) {
      char buf[64];
      snprintf(buf, sizeof buf, "  %-12s %5llu %5llu %5llu   ",
               sd->name.c_str(), (unsigned long long)sd->size,
               (unsigned long long)sd->align, (unsigned long long)sd->padBytes);
      S << buf;
      for (size_t i = 0; i < sd->fields.size(); ++i) {
        if (i) S << ", ";
        S << sd->fields[i].name << ":" << sd->offsets[i]
          << "@" << sd->sizes[i];
      }
      S << "\n";
    }
    R << "\nstruct layouts (cache line = 64 B):\n";
    R << "  name         size  align   pad   fields (name:offset@size)\n";
    R << S.str();
  }

  // AoS/SoA advice summary
  int nsoa = 0;
  for (auto& d : g_diags) if (d.code == info(Code::Soa).id) nsoa++;
  if (nsoa) {
    R << "\nAoS->SoA: " << nsoa << " site(s) flagged. Transform with "
         "`let a: soa(T) = soa_new<T>(n, ...)`; see docs/language-spec.md.\n";
  }

  // ---------------- layer 2: IR
  R << "\n---- [2] IR layer (" << irPath << ") ------------------------------\n";
  IrMetrics m = scanIr(irPath);
  R << "user functions   : " << p->fns.size() << "\n";
  R << "parfor chunk fns : " << m.nChunkFns << "\n";
  // §13.1 rename: counting annotated loops was reported as "vectorizable
  // loops" — an annotation count, not proof of backend vectorisation.
  R << "loops w/ vectorise annotation [static]: " << m.nVectorLoops
    << "  (interleave 4: " << m.nInterleave4 << ")\n";
  R << "fast-math fp ops : " << m.nFastMath
    << "  (in parfor bodies: " << m.nFpReductions << ")\n";
  R << "noalias params   : " << m.nNoAlias << "\n";
  R << "soa plane ops    : " << m.nSoaCalls << "\n";
  if (m.nAtomicRmw)
    R << "atomic counters  : " << m.nAtomicRmw
      << "  (thread-safe PGO bumps, §11.1)\n";
  if (G && !G->parforTags.empty()) {
    R << "parfor regions   : " << G->parforTags.size() << "\n";
  }
  if (!g_opts.useProfile.empty()) {
    R << "branch weights   : " << m.nBranchWeights
      << "  (profile-guided)\n";
  }

  // ---------------- layer 2b: backend transformation results (§13.1)
  {
    RemarkMetrics rm = readOptRecords(binPath);
    if (rm.nVectorized + rm.nMissed + rm.nInlined + rm.nInlineMissed > 0) {
      R << "\n---- [2b] backend transformation results ----------------------------\n";
      R << "loops vectorised [backend]       : " << rm.nVectorized << "\n";
      R << "vectorisation missed [backend] : " << rm.nMissed << "\n";
      R << "fns inlined [backend]           : " << rm.nInlined
        << "  (missed: " << rm.nInlineMissed << ")\n";
      for (auto& d : rm.vectorizedDetail)
        R << "  vectorised: " << d << "\n";
      for (auto& r : rm.missedReasons)
        R << "  missed    : " << r << "\n";
    } else {
      R << "\n(no backend remark data — rebuild with --opt-remarks to see "
         "actual vectorisation/inlining results)\n";
    }
  }

  // ---------------- layer 3: runtime timing
  R << "\n---- [3] runtime timing --------------------------------------------\n";
  TimingStats tstats;
  auto timing = readTiming(binPath, &tstats);
  if (timing.empty()) {
    R << "no timing data. re-run the binary with FRT_TIMING=1 to collect\n";
  } else {
    // correlate tags
    std::map<uint64_t, std::pair<std::string, int>> tagMap;
    if (G)
      for (auto& [tag, fn, line] : G->parforTags) tagMap[tag] = {fn, line};
    R << "  fn:line                 calls     cycles       chunks  pinfails\n";
    for (auto& t : timing) {
      std::string where = "unknown";
      if (tagMap.count(t.tag)) {
        where = tagMap[t.tag].first + ":" + std::to_string(tagMap[t.tag].second);
      }
      char buf[96];
      snprintf(buf, sizeof buf, "  %-24s %6lld %12llu %10lld %8d\n",
               where.c_str(), (long long)t.count,
               (unsigned long long)t.cycles, (long long)t.nchunks,
               t.pinfails);
      R << buf;
    }
    if (tstats.present) {
      // §10.1 scheduler stats: inline (under the serial cutoff) vs
      // dispatched regions — the crossover threshold is tunable via
      // FRT_SERIAL_CUTOFF / the -O4 genome.
      R << "  parfor regions [runtime]: " << tstats.serial
        << " inline (serial cutoff), " << tstats.dispatched
        << " dispatched to pool\n";
    }
  }

  // ---------------- layer 4: PGO
  if (!g_opts.useProfile.empty() || g_opts.profile) {
    R << "\n---- [4] profile-guided optimization -------------------------------\n";
    if (g_opts.profile) {
      R << "instrumented build: run the binary, then recompile with\n";
      R << "  penstock --use-profile=<binary>.fprof " << p->file << "\n";
      R << "(counters: " << (G ? G->fnProf.size() : 0) << " fns, "
        << (G ? G->brProf.size() : 0) << " branch sites)\n";
    } else {
      auto top = profileTopFunctions();
      R << "profile: " << g_opts.useProfile << "\n";
      int covered = 0;
      if (G) {
        for (auto& fp : G->fnProf) if (fp.loaded) covered++;
      }
      R << "hot functions (" << covered << "/" << p->fns.size() << " covered):\n";
      int i = 0;
      for (auto& [name, cnt] : top) {
        if (i++ >= 10) { R << "  ...\n"; break; }
        char buf[64];
        snprintf(buf, sizeof buf, "  %-20s %12llu calls\n", name.c_str(),
                 (unsigned long long)cnt);
        R << buf;
      }
    }
  }

  R << "====================================================================\n";
  fputs(R.str().c_str(), stderr);
}

} // namespace freshet
