// Freshet performance report — merges four layers:
//   1. frontend diagnostics statistics (E-*/W-* tallies, struct layout table)
//   2. IR metrics (parse .ll text: loops with vectorise annotations, reductions, branch
//      weights, parfor chunk fns)
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
  int nChunkFns = 0;        // __pf_body_*
  int nVectorLoops = 0;     // llvm.loop vectorize.enable
  int nInterleave4 = 0;     // interleave.count 4
  int nBranchWeights = 0;   // !prof branch_weights (PGO)
  int nFastMath = 0;        // fadd/fsub/fmul/fast
  int nFpReductions = 0;    // fadd fast inside chunk fns (approx)
  int nNoAlias = 0;         // noalias params
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
    }
    if (t.find("call") != std::string::npos && t.find("@frt_soa_") != std::string::npos)
      ++m.nSoaCalls;
  }
  return m;
}

// ---- runtime timing file ------------------------------------------------
// FRESHET-TIMING 1
// PF <tag> <cycles> <count> <nchunks> <pinfails>
// ST <serial> <dispatched>     (§10.1 scheduler stats; absent on old runs)
// SI <paired> <memLane> <compLane>   (mem & vectorisation stage: SMT
//     interleave stats; absent when no paired dispatch ever ran)
struct TimingEnt {
  uint64_t tag, cycles;
  int64_t count, nchunks;
  int pinfails;
  // (registry 10.06.01): v2 fields — measured wall time,
  // measured iterations, per-thread hardware-counter aggregates (0 =
  // counters off/unavailable).  v1 files leave these zero.
  uint64_t ns = 0;
  int64_t iters = 0;
  uint64_t instr = 0, llc = 0;
};

struct TimingStats {
  int64_t serial = 0, dispatched = 0;
  bool present = false;
  // mem & vectorisation stage: SMT interleave (optional)
  int64_t smtPaired = 0, smtMemLane = 0, smtCompLane = 0;
  bool smtPresent = false;
  // NUMA topology + the memory policy actually applied
  int numaNodes = 0, numaPolicy = 0;
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
  bool v2 = magic.find("2") != std::string::npos;   // header
  std::string kind;
  while (f >> kind) {
    if (kind == "ST") {
      if (stats && (f >> stats->serial >> stats->dispatched)) stats->present = true;
      continue;
    }
    if (kind == "SI") {
      if (stats && (f >> stats->smtPaired >> stats->smtMemLane >>
                    stats->smtCompLane))
        stats->smtPresent = true;
      continue;
    }
    if (kind == "NU") {   // nodes + applied policy
      int nodes = 0, policy = 0;
      if (stats && (f >> nodes >> policy)) stats->numaNodes = nodes,
                                           stats->numaPolicy = policy;
      continue;
    }
    if (kind != "PF") { continue; }
    TimingEnt t;
    if (!(f >> t.tag >> t.cycles >> t.count >> t.nchunks >> t.pinfails)) break;
    if (v2 && !(f >> t.ns >> t.iters >> t.instr >> t.llc)) break;
    out.push_back(t);
  }
  return out;
}

// ---- machine profile (the bundled microbenchmark's output) ------
// MACHINE-PROFILE 1 written by tools/machine_profile.c next to the
// profiled binary; every field is a MEASUREMENT, never an estimate.
struct MachineProfile {
  bool present = false;
  double bwTriad = 0;    // bytes/s (2 reads + 1 write per element)
  double fmaPeak = 0;    // flop/s (16 independent fma chains)
  int64_t l1d = 0, l2 = 0, l3 = 0, page = 0;
  int64_t memTotal = 0, memAvail = 0;
  int cores = 0, threads = 0, smt = 0, numa = 0;
};

static MachineProfile readMachineProfile(const std::string& binPath) {
  MachineProfile mp;
  std::ifstream f(binPath + ".machprof");
  if (!f.is_open()) return mp;
  std::string magic;
  std::getline(f, magic);
  if (magic.find("MACHINE-PROFILE") == std::string::npos) return mp;
  mp.present = true;
  std::string key;
  double d;
  long long v;
  while (f >> key) {
    if (key == "BW_TRIAD_BYTES_PER_S" && (f >> d)) mp.bwTriad = d;
    else if (key == "FMA_PEAK_FLOP_S" && (f >> d)) mp.fmaPeak = d;
    else if (key == "CORES_PHYSICAL" && (f >> v)) mp.cores = (int)v;
    else if (key == "THREADS_LOGICAL" && (f >> v)) mp.threads = (int)v;
    else if (key == "SMT" && (f >> v)) mp.smt = (int)v;
    else if (key == "NUMA_NODES" && (f >> v)) mp.numa = (int)v;
    else if (key == "L1D_BYTES" && (f >> v)) mp.l1d = v;
    else if (key == "L2_BYTES" && (f >> v)) mp.l2 = v;
    else if (key == "L3_BYTES" && (f >> v)) mp.l3 = v;
    else if (key == "PAGE_BYTES" && (f >> v)) mp.page = v;
    else if (key == "MEM_TOTAL_BYTES" && (f >> v)) mp.memTotal = v;
    else if (key == "MEM_AVAILABLE_BYTES" && (f >> v)) mp.memAvail = v;
    else { f.clear(); std::string skip; std::getline(f, skip); }
  }
  return mp;
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
  // --march-levels — the ISA rungs the hot kernels were
  // multi-versioned over (ifunc dispatch, CPUID-resolved at load)
  if (!g_opts.marchLevels.empty()) {
    R << " (levels:";
    for (size_t i = 0; i < g_opts.marchLevels.size(); ++i)
      R << (i ? "," : "") << " " << g_opts.marchLevels[i];
    R << ")";
  }
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

  // struct layout table.  Generic STRUCT TEMPLATES (e.g. `struct P<T>`)
  // carry field names but never get a layout — offsets/sizes are computed
  // at instantiation only (generics.cpp) — so index them defensively:
  // a template row shows its fields without offset@size instead of
  // reading past the end of empty vectors (null-base SEGV).
  if (!p->structs.empty()) {
    std::ostringstream S;
    for (auto* sd : p->structs) {
      const bool laidOut =
          sd->offsets.size() == sd->fields.size() &&
          sd->sizes.size() == sd->fields.size();
      char buf[64];
      snprintf(buf, sizeof buf, "  %-12s %5llu %5llu %5llu   ",
               sd->name.c_str(),
               (unsigned long long)(laidOut ? sd->size : 0),
               (unsigned long long)(laidOut ? sd->align : 0),
               (unsigned long long)(laidOut ? sd->padBytes : 0));
      S << buf;
      for (size_t i = 0; i < sd->fields.size(); ++i) {
        if (i) S << ", ";
        S << sd->fields[i].name;
        if (laidOut)
          S << ":" << sd->offsets[i] << "@" << sd->sizes[i];
        else
          S << ":(template)";
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

      // a STABLE
      // per-kernel verdict — the remark stream is correlated back to
      // the parfor region registry (chunk fn __pf_body_N <-> region N,
      // .Lk-suffixed FMV versions map to the same region) and each
      // region gets one line: VECTORIZED / NOT VECTORIZED + the backend's
      // own missed-remark reason + the source fn:line.  Under
      // --strict-performance (the HPC mode; @hpc_strict per-fn is the
      // source-level spelling) a NOT-VECTORIZED verdict is a hard
      // F0930 error instead of a table row.
      if (G && !G->parforTags.empty()) {
        // collect per chunk-fn verdicts from the remark stream
        std::map<std::string, std::string> fnVerdict, fnReason;
        std::ifstream rf(binPath + ".opt-records.yaml");
        (void)rf;   // already parsed by readOptRecords; reuse its walks:
        // (the two-pass shape keeps readOptRecords untouched)
        struct V {
          std::map<std::string, std::pair<int, std::string>> tab;
        } v;
        {
          std::ifstream f(binPath + ".opt-records.yaml");
          if (f.is_open()) {
            std::string line, tag, pass, name, fn;
            auto flush = [&]() {
              if (tag.empty() || fn.empty() || pass.empty()) return;
              if (pass.find("vectorize") == std::string::npos) return;
              bool passed = tag == "!Passed";
              auto& slot = v.tab[fn];
              if (passed) slot.first = 1;
              else if (slot.first != 1) slot.first = -1, slot.second = name;
              tag = pass = name = fn = "";
            };
            while (std::getline(f, line)) {
              std::string t = trim2(line);
              if (t.rfind("--- !", 0) == 0) { flush(); tag = t.substr(4); continue; }
              if (t.rfind("Pass:", 0) == 0) pass = unquote(trim2(t.substr(5)));
              else if (t.rfind("Name:", 0) == 0) name = unquote(trim2(t.substr(5)));
              else if (t.rfind("Function:", 0) == 0)
                fn = unquote(trim2(t.substr(9)));
            }
            flush();
          }
        }
        if (!v.tab.empty()) {
          R << "\n  ---- [2b+] parfor vectorisation verdicts ----\n";
          R << "  region (fn:line)               verdict    reason\n";
          int nNot = 0;
          for (size_t N = 0; N < G->parforTags.size(); ++N) {
            auto& pt = G->parforTags[N];
            // strip a .Lk FMV suffix from the chunk fn name
            std::string base = "__pf_body_" + std::to_string(N);
            std::string verdict = "no remark";
            std::string reason;
            int any = 0;
            for (auto& [fn, vr] : v.tab) {
              if (fn.rfind(base, 0) != 0) continue;
              if (fn.size() > base.size() &&
                  fn[base.size()] != '.')
                continue;   // __pf_body_12 not of __pf_body_1
              any = vr.first;
              if (vr.first < 0) reason = vr.second;
              if (vr.first > 0) { any = 1; break; }
            }
            if (any > 0) verdict = "VECTORIZED";
            else if (any < 0) { verdict = "NOT VECTORIZED"; ++nNot; }
            R << "  " << pt.fn << ":" << pt.line
              << std::string(30 - std::min<size_t>(29, pt.fn.size() + 6), ' ')
              << verdict << (reason.empty() ? "" : "   " + reason) << "\n";
          }
          if (nNot && g_opts.strictPerformance) {
            // the HPC mode: a not-vectorised hot kernel is a hard error
            diag(Diag::Error, Code::VecVerdict,
                 std::to_string(nNot) + " parfor kernel(s) were not "
                 "vectorised (--strict-performance: F0930); the verdict "
                 "table above names the reason and source line",
                 "", 0, 0);
          }
        }
      }
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
    std::map<uint64_t, std::pair<int64_t, int64_t>> tagCensus;
    if (G)
      for (auto& pt : G->parforTags) {
        tagMap[pt.tag] = {pt.fn, pt.line};
        tagCensus[pt.tag] = {pt.flopsPerIter, pt.bytesPerIter};
      }
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
    if (tstats.smtPresent) {
      // mem & vectorisation stage: SMT interleave stats — paired
      // dispatches and the per-lane chunk counts (memory lane vs
      // compute lane on sibling hardware threads)
      R << "  smt interleave [runtime]: " << tstats.smtPaired
        << " paired dispatch(es), " << tstats.smtMemLane
        << " memory-lane chunk(s), " << tstats.smtCompLane
        << " compute-lane chunk(s)\n";
    }
    if (tstats.numaNodes > 0) {
      R << "  numa [runtime]: " << tstats.numaNodes << " node(s), policy ";
      const char* pol = tstats.numaPolicy == 0 ? "off"
                        : tstats.numaPolicy == 1 ? "local"
                        : tstats.numaPolicy == 2 ? "interleave"
                        : "refused";
      R << pol << "\n";
    }

    // ---------------- measured per-region throughput --------------
    // registry fsht-03:04-10.06.01 (numerical profiling): measured
    // wall time x the compiler's static per-iteration census ->
    // per-region FLOP/s and bytes/s.  Every number is labelled with
    // where it came from: [runtime] measured time/iterations, [static]
    // the front-end flops/bytes-per-iteration model, [hw] the hardware
    // counters when the PMU was available.  The roofline compares the
    // achieved rate against the machine profile's measured peaks.
    bool anyRate = false;
    for (auto& t : timing)
      if (t.ns > 0 && t.iters > 0) { anyRate = true; break; }
    if (anyRate) {
      MachineProfile mp = readMachineProfile(binPath);
      R << "\n  ---- [3b] measured per-region throughput (registry 10.06.01)\n";
      R << "  fn:line                 iters      wall-ns    GFLOP/s    GB/s"
           "      hw-instr/s  hw-LLC-B/s\n";
      double bestG = 0, bestB = 0;
      for (auto& t : timing) {
        if (t.ns == 0 || t.iters == 0) continue;
        std::string where = "unknown";
        std::pair<int64_t, int64_t> cen{0, 0};
        if (tagMap.count(t.tag)) {
          where = tagMap[t.tag].first + ":" +
                  std::to_string(tagMap[t.tag].second);
          cen = tagCensus.count(t.tag) ? tagCensus[t.tag] : cen;
        }
        double gflops = (double)cen.first * (double)t.iters /
                        (double)t.ns;                 // flops/ns = GFLOP/s
        double gbs = (double)cen.second * (double)t.iters /
                     (double)t.ns;                    // bytes/ns = GB/s
        double ips = t.instr ? (double)t.instr / (double)t.ns : 0;
        double llcB = t.llc ? (double)t.llc * 64.0 / (double)t.ns : 0;
        if (gflops > bestG) bestG = gflops;
        if (gbs > bestB) bestB = gbs;
        char buf[128];
        snprintf(buf, sizeof buf,
                 "  %-24s %9lld %12llu %10.3f %10.3f %10.3e %10.3e\n",
                 where.c_str(), (long long)t.iters,
                 (unsigned long long)t.ns, gflops, gbs, ips, llcB);
        R << buf;
      }
      R << "  (rate = [static] flops/bytes per iteration x [runtime] "
           "measured iterations / [runtime] measured wall-ns;\n"
           "   hw columns are [hw] per-thread counter aggregates — 0 "
           "means the PMU was off/unavailable, never an estimate)\n";
      // roofline-style summary against the machine profile's peaks
      if (mp.present && (mp.bwTriad > 0 || mp.fmaPeak > 0)) {
        R << "\n  roofline (machine profile " << binPath << ".machprof)\n";
        if (mp.bwTriad > 0)
          R << "    bandwidth peak  : " << mp.bwTriad / 1e9 << " GB/s"
               " (triad, best of 5)\n";
        if (mp.fmaPeak > 0)
          R << "    fma peak         : " << mp.fmaPeak / 1e9
               << " GFLOP/s (16 independent chains)\n";
        if (bestB > 0 && mp.bwTriad > 0)
          R << "    best region BW   : " << bestB << " GB/s = "
               << 100.0 * bestB / (mp.bwTriad / 1e9) << "% of peak\n";
        if (bestG > 0 && mp.fmaPeak > 0)
          R << "    best region FLOP : " << bestG << " GFLOP/s = "
               << 100.0 * bestG / (mp.fmaPeak / 1e9) << "% of peak\n";
        R << "    (a region's ceiling is min(fma peak, AI x bandwidth "
               "peak); re-run tools/machine_profile.c after hardware "
               "changes)\n";
      } else {
        R << "\n  (no machine profile at " << binPath << ".machprof — "
             "run `cc -O2 tools/machine_profile.c runtime/frt_rt.c -lm "
             "-lpthread && ./a.out <exe>.machprof` for the roofline "
             "comparison)\n";
      }
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
