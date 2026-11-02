// Freshet -O4 — stochastic evolution engine (R3).
//
// -O4 = the deterministic -O3 pipeline + a brief genetic search over the
// tuning knobs that LLVM cannot derive from the source, evaluated by
// actually running the program:
//
//   codegen genes : clang -O2 vs -O3, -march native/x86-64-v3/x86-64
//   runtime genes : worker pool size (FRT_THREADS), core pinning
//                   (FRT_AFFINITY), default parfor chunk (FRT_CHUNK)
//
// The engine compiles candidate binaries lazily (memoized per
// opt-level x march), evaluates genomes by fork/exec with wall-clock
// timing (adaptive best-of-k against noise), and evolves with
// tournament selection + uniform crossover + mutation + elitism under
// a wall-clock budget (--evo-budget, default 10s).
//
// It never pessimizes: the champion must beat the incumbent
// configuration on alternating re-validated median runs by > 3%.
// The winner is baked into the final binary by the driver (relink with
// -DFRT_TUNED_* runtime defaults).
#include "common.h"
#include <random>
#include <chrono>
#include <algorithm>
#include <map>
#include <string>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>

namespace freshet {

namespace evo {

static constexpr double INF = 1e18;
static constexpr int POPL = 8;

struct Genome {
  int optIdx = 0;    // into optLevels
  int marchIdx = 0;  // into marches
  int threads = 0;   // actual worker count (eval sets env; 0 never happens)
  int pin = 0;
  int chunkIdx = 0;  // into chunks
  // §10.1/§11.3 dimensions added after the correctness work:
  //   cutoffIdx — FRT_SERIAL_CUTOFF (0 = default heuristic, >0 = inline
  //               parfor regions up to that many iterations)
  //   veclib    — compile with -fveclib=libmvec -fno-math-errno (§8)
  int cutoffIdx = 0;
  bool veclib = false;
  bool operator<(const Genome& o) const {
    return std::tie(optIdx, marchIdx, threads, pin, chunkIdx, cutoffIdx, veclib) <
           std::tie(o.optIdx, o.marchIdx, o.threads, o.pin, o.chunkIdx,
                    o.cutoffIdx, o.veclib);
  }
};

// one timed run with CAPTURED stdout — the checksum gate (§11.3)
// rejects candidates whose output differs from the reference run
struct RunResult {
  double ms = 0;
  std::string out;
  bool ok = false;
};

static double median(std::vector<double>& v) {
  std::sort(v.begin(), v.end());
  if (v.empty()) return INF;
  if (v.size() % 2) return v[v.size() / 2];
  return (v[v.size() / 2 - 1] + v[v.size() / 2]) / 2.0;
}

struct Ind {
  Genome g;
  double fitness = -1;   // ms; -1 unevaluated
};

// Run compiler tools with an argv vector, never through a shell. Paths and
// target options therefore remain data even when they contain shell syntax.
static int runCompiler(const std::vector<std::string>& command) {
  if (command.empty() || command.front().empty()) return 127;
  std::vector<char*> argv;
  argv.reserve(command.size() + 1);
  for (const auto& arg : command)
    argv.push_back(const_cast<char*>(arg.c_str()));
  argv.push_back(nullptr);

  const pid_t pid = fork();
  if (pid < 0) return 127;
  if (pid == 0) {
    const int nullFd = open("/dev/null", O_WRONLY);
    if (nullFd < 0 || dup2(nullFd, STDERR_FILENO) < 0) _exit(126);
    if (nullFd != STDERR_FILENO) close(nullFd);
    execvp(command.front().c_str(), argv.data());
    _exit(errno == ENOENT ? 127 : 126);
  }

  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno == EINTR) continue;
    return 127;
  }
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
  return 127;
}

struct Engine {
  std::string clang, finalLl, outBin, tmpDir;
  std::vector<int> optLevels{3, 2};
  std::vector<std::string> marches{"native"};
  std::vector<long> chunks{0, 64, 256, 1024, 4096, 16384};
  // §10.1: serial cutoff candidates (FRT_SERIAL_CUTOFF); 0 = default
  // heuristic, larger values inline more tiny regions
  std::vector<long> cutoffs{0, 128, 1024, 8192, 65536};
  std::string reference;          // reference stdout (checksum gate)
  int nproc = 1;
  double deadlineMs = 0;         // steady_clock ms since start()
  std::mt19937 rng;
  // caches
  std::map<std::tuple<int,int,bool>, std::string> binCache; // (opt,march,veclib)
  std::map<Genome, double> fitCache;
  int evals = 0, compiles = 0, generations = 0;

  ~Engine() {
    for (auto& [k, p] : binCache) if (!p.empty()) unlink(p.c_str());
  }

  std::chrono::steady_clock::time_point t0;
  double elapsedMs() const {
    return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - t0).count();
  }
  double remainingMs() const { return deadlineMs - elapsedMs(); }

  // ---------------- candidate binaries (opt x march x veclib), lazy
  std::string candidateBin(const Genome& g) {
    auto key = std::make_tuple(g.optIdx, g.marchIdx, g.veclib);
    auto it = binCache.find(key);
    if (it != binCache.end()) return it->second;   // may be "" = dead
    std::string path = tmpDir + "/evo_cand_" + std::to_string(getpid()) + "_" +
                       std::to_string(optLevels[g.optIdx]) + "_" + marches[g.marchIdx] +
                       (g.veclib ? "_vl" : "");
    std::vector<std::string> command{
        clang, "-O" + std::to_string(optLevels[g.optIdx]),
        "-march=" + marches[g.marchIdx]};
    if (g.veclib) {
      command.push_back("-fveclib=libmvec");
      command.push_back("-fno-math-errno");
    }
    command.insert(command.end(), {finalLl, "-o", path, "-lm", "-lpthread"});
    ++compiles;
    fflush(nullptr);
    const int rc = runCompiler(command);
    std::string res = (rc == 0) ? path : std::string();
    if (!res.empty() && access(res.c_str(), X_OK) != 0) res.clear();
    binCache[key] = res;
    return res;
  }

  // ---------------- timed run of one binary under a runtime config
  // §11.3: stdout is captured through a pipe (not discarded) so fitness
  // can reject wrong-output candidates; stderr goes to /dev/null.
  RunResult runTimed(const std::string& bin, const Genome& g, int timeoutSec) {
    RunResult R;
    int fds[2];
    if (pipe(fds) != 0) return R;
    auto r0 = std::chrono::steady_clock::now();
    pid_t pid = fork();
    if (pid == 0) {
      close(fds[0]);
      dup2(fds[1], 1);
      close(fds[1]);
      int dn = open("/dev/null", O_WRONLY);
      if (dn >= 0) { dup2(dn, 2); close(dn); }
      char b[32];
      snprintf(b, sizeof b, "%d", g.threads > 0 ? g.threads : nproc);
      setenv("FRT_THREADS", b, 1);
      setenv("FRT_AFFINITY", g.pin ? "1" : "0", 1);
      snprintf(b, sizeof b, "%ld", chunks[g.chunkIdx]);
      if (chunks[g.chunkIdx] > 0) setenv("FRT_CHUNK", b, 1);
      else unsetenv("FRT_CHUNK");
      snprintf(b, sizeof b, "%ld", cutoffs[g.cutoffIdx]);
      if (cutoffs[g.cutoffIdx] > 0) setenv("FRT_SERIAL_CUTOFF", b, 1);
      else unsetenv("FRT_SERIAL_CUTOFF");
      unsetenv("FRT_TIMING");
      unsetenv("FRT_PROF_OUT");
      alarm(timeoutSec > 0 ? timeoutSec : 30);
      execl(bin.c_str(), bin.c_str(), (char*)nullptr);
      _exit(127);
    }
    close(fds[1]);
    char buf[4096];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof buf)) > 0) R.out.append(buf, n);
    close(fds[0]);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    auto r1 = std::chrono::steady_clock::now();
    R.ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    if (R.ok) R.ms = std::chrono::duration<double, std::milli>(r1 - r0).count();
    return R;
  }

  // ---------------- evaluate one genome (§11.3 measurement protocol)
  // warm-up + repeated timed samples, MEDIAN (not the single fastest
  // run — that optimises measurement noise, not the program); any
  // candidate whose stdout differs from the reference is rejected.
  double evaluate(const Genome& g) {
    auto key = g;
    auto it = fitCache.find(key);
    if (it != fitCache.end()) return it->second;
    std::string bin = candidateBin(g);
    double f = INF;
    if (!bin.empty()) {
      int guard = std::max(2, (int)(remainingMs() / 1000.0 + 1.0));
      RunResult warm = runTimed(bin, g, guard);
      ++evals;
      if (warm.ok) {
        if (reference.empty()) reference = warm.out;   // first clean run = reference
        if (warm.out != reference) {
          fprintf(stderr, "evo: candidate rejected — output differs from "
                          "reference (checksum gate)\n");
          fitCache[key] = INF;
          return INF;
        }
        // samples scale inversely with run cost; stay inside the budget
        int samples = 3;
        while (samples > 1 && warm.ms * (samples + 1) > remainingMs() * 0.8)
          --samples;
        std::vector<double> ts;
        ts.push_back(warm.ms);
        for (int i = 1; i < samples; ++i) {
          RunResult t = runTimed(bin, g, guard);
          ++evals;
          if (!t.ok || t.out != reference) {
            fitCache[key] = INF;
            return INF;
          }
          ts.push_back(t.ms);
        }
        f = median(ts);
      }
    }
    fitCache[key] = f;
    return f;
  }

  // ---------------- GA operators
  int randInt(int lo, int hi) {  // inclusive
    return std::uniform_int_distribution<int>(lo, hi)(rng);
  }
  Genome tournament(const std::vector<Ind>& pop) {
    Ind best = pop[randInt(0, (int)pop.size() - 1)];
    for (int i = 0; i < 2; ++i) {
      Ind c = pop[randInt(0, (int)pop.size() - 1)];
      if (c.fitness >= 0 && (best.fitness < 0 || c.fitness < best.fitness)) best = c;
    }
    return best.g;
  }
  Genome crossover(const Genome& a, const Genome& b) {
    Genome c;
    c.optIdx = randInt(0, 1) ? a.optIdx : b.optIdx;
    c.marchIdx = randInt(0, 1) ? a.marchIdx : b.marchIdx;
    c.threads = randInt(0, 1) ? a.threads : b.threads;
    c.pin = randInt(0, 1) ? a.pin : b.pin;
    c.chunkIdx = randInt(0, 1) ? a.chunkIdx : b.chunkIdx;
    c.cutoffIdx = randInt(0, 1) ? a.cutoffIdx : b.cutoffIdx;
    c.veclib = randInt(0, 1) ? a.veclib : b.veclib;
    return c;
  }
  Genome mutate(const Genome& g) {
    Genome m = g;
    if (randInt(1, 100) <= 35) m.optIdx = randInt(0, (int)optLevels.size() - 1);
    if (randInt(1, 100) <= 35) m.marchIdx = randInt(0, (int)marches.size() - 1);
    if (randInt(1, 100) <= 35) m.threads = randInt(1, std::max(1, 2 * nproc));
    if (randInt(1, 100) <= 20) m.pin = 1 - m.pin;
    if (randInt(1, 100) <= 35) m.chunkIdx = randInt(0, (int)chunks.size() - 1);
    if (randInt(1, 100) <= 30) m.cutoffIdx = randInt(0, (int)cutoffs.size() - 1);
    if (randInt(1, 100) <= 15) m.veclib = !m.veclib;
    return m;
  }
};

} // namespace evo

using namespace evo;

// ---------------------------------------------------------------- entry
bool evoOptimize(const std::string& clangBin, const std::string& finalLl,
                 const std::string& outBinary, int budgetSec, unsigned seed,
                 EvoWinner* result) {
  Engine E;
  E.clang = clangBin;
  E.finalLl = finalLl;
  E.outBin = outBinary;
  E.nproc = sysconf(_SC_NPROCESSORS_ONLN) > 0 ? (int)sysconf(_SC_NPROCESSORS_ONLN) : 1;
  if (E.nproc > 32) E.nproc = 32;
  E.tmpDir = "/tmp";
  E.t0 = std::chrono::steady_clock::now();
  E.deadlineMs = budgetSec * 1000.0;

  if (seed == 0) {
    seed = (unsigned)std::chrono::steady_clock::now().time_since_epoch().count() ^
           (unsigned)getpid();
  }
  E.rng.seed(seed);

  // march dimension: only explore if the user left -march at native and
  // we're on x86-64 (the host the binary will run on)
  if (g_opts.march == "native") {
#if defined(__x86_64__)
    E.marches = {"native", "x86-64-v3", "x86-64"};
#endif
  } else {
    E.marches = {g_opts.march};   // pinned by user: respect it
  }

  Genome incumbent;
  incumbent.optIdx = 0;                       // -O3
  incumbent.marchIdx = 0;                     // native (or user march)
  incumbent.threads = E.nproc;                // == default behavior
  incumbent.pin = 0;
  incumbent.chunkIdx = 0;
  incumbent.cutoffIdx = 0;                    // default serial cutoff
  incumbent.veclib = false;

  // search-space size for the header (threads 1..2n x chunks x pin x opts
  // x marches x cutoffs x veclib) — §11.3: widened ONLY after the
  // correctness (checksum gate) and measurement (median) work landed
  long space = (long)std::max(1, 2 * E.nproc) * (long)E.chunks.size() * 2 *
               (long)E.optLevels.size() * (long)E.marches.size() *
               (long)E.cutoffs.size() * 2;
  fprintf(stderr, "evo: -O4 stochastic search (budget %ds, seed %u, %d cores, "
                  "%ld configurations in scope; median-of-3 fitness + "
                  "output checksum gate)\n",
          budgetSec, seed, E.nproc, space);

  // ---- incumbent fitness (the already-linked default binary)
  double baseT = E.evaluate(incumbent);
  if (baseT >= INF) {
    fprintf(stderr, "evo: program failed to run cleanly under timing "
                    "(nonzero exit or timeout) — search skipped\n");
    return false;
  }
  if (baseT > E.deadlineMs * 0.6) {
    fprintf(stderr, "evo: single run takes %.0f ms — too long for a %ds "
                    "budget; increase --evo-budget or skip -O4\n",
            baseT, budgetSec);
    return false;
  }
  fprintf(stderr, "evo: incumbent (-O3 defaults) %.1f ms\n", baseT);

  // ---- seed population
  std::vector<Ind> pop;
  auto addInd = [&](Genome g) {
    Ind i; i.g = g; pop.push_back(i);
  };
  addInd(incumbent);
  Genome g1 = incumbent; g1.threads = 1; addInd(g1);              // serial
  Genome g2 = incumbent; g2.optIdx = 1; addInd(g2);               // -O2
  Genome g3 = incumbent; g3.pin = 1; addInd(g3);                  // pinned
  Genome g4 = incumbent; g4.chunkIdx = 2; addInd(g4);             // chunk 256
  while ((int)pop.size() < POPL) {
    Genome g = incumbent;
    g.optIdx = E.randInt(0, (int)E.optLevels.size() - 1);
    g.marchIdx = E.randInt(0, (int)E.marches.size() - 1);
    g.threads = E.randInt(1, std::max(1, 2 * E.nproc));
    g.pin = E.randInt(0, 1);
    g.chunkIdx = E.randInt(0, (int)E.chunks.size() - 1);
    g.cutoffIdx = E.randInt(0, (int)E.cutoffs.size() - 1);
    g.veclib = E.randInt(0, 1) != 0;
    addInd(g);
  }

  // ---- generations
  Genome bestG = incumbent;
  double bestF = baseT;
  int gensWithoutImprove = 0;
  int gen = 0;
  for (; gen < 200; ++gen) {
    bool outOfTime = false;
    for (auto& ind : pop) {
      if (ind.fitness < 0) {
        if (E.remainingMs() < 600) { outOfTime = true; break; }
        ind.fitness = E.evaluate(ind.g);
        if (ind.fitness < bestF) {
          bestF = ind.fitness;
          bestG = ind.g;
          gensWithoutImprove = 0;
          fprintf(stderr, "evo: gen %d: new best %.1f ms\n", gen, bestF);
        }
      }
    }
    ++E.generations;
    if (outOfTime) break;
    ++gensWithoutImprove;
    if (gensWithoutImprove >= 3 && E.elapsedMs() > E.deadlineMs * 0.5) break;
    if (E.remainingMs() < 800) break;

    // evolve: sort, keep 2 elites, breed the rest
    std::vector<Ind> sorted = pop;
    std::sort(sorted.begin(), sorted.end(),
              [](const Ind& a, const Ind& b) {
                if (a.fitness < 0) return false;
                if (b.fitness < 0) return true;
                return a.fitness < b.fitness;
              });
    std::vector<Ind> next;
    for (int i = 0; i < 2 && i < (int)sorted.size(); ++i) next.push_back(sorted[i]);
    while ((int)next.size() < POPL) {
      Genome p1 = E.tournament(sorted);
      Genome p2 = E.tournament(sorted);
      Genome child = E.mutate(E.crossover(p1, p2));
      Ind ci; ci.g = child; next.push_back(ci);
    }
    pop = next;
  }

  // ---- re-validate champion vs incumbent (alternating, medians)
  bool sameAsIncumbent = !(bestG < incumbent) && !(incumbent < bestG);
  if (sameAsIncumbent) {
    fprintf(stderr, "evo: %d evals / %d compiles / %d gens — no configuration "
                    "beat the -O3 defaults; keeping them\n",
            E.evals, E.compiles, E.generations);
    if (result) { result->improved = false; result->evals = E.evals; }
    return false;
  }

  std::vector<double> champT, incT;
  std::string champBin = E.candidateBin(bestG);
  if (champBin.empty()) {
    fprintf(stderr, "evo: champion binary failed to link — keeping defaults\n");
    return false;
  }
  // warm both binaries (page cache, CPU freq) so the alternating runs
  // measure steady-state, not cold-start
  E.runTimed(champBin, bestG, 30);
  E.runTimed(outBinary, incumbent, 30);
  int revalRuns = (baseT + bestF < 150) ? 3 : 2;
  bool outputOk = true;
  for (int i = 0; i < revalRuns; ++i) {
    RunResult c = E.runTimed(champBin, bestG, 30);
    RunResult b = E.runTimed(outBinary, incumbent, 30);
    if (c.ok && !E.reference.empty() && c.out != E.reference) outputOk = false;
    if (b.ok && !E.reference.empty() && b.out != E.reference) outputOk = false;
    if (c.ok) champT.push_back(c.ms);
    if (b.ok) incT.push_back(b.ms);
  }
  E.evals += 2 * revalRuns;
  double champMed = median(champT), incMed = median(incT);
  if (!outputOk) {
    fprintf(stderr, "evo: champion output diverged during re-validation — "
                    "keeping -O3 defaults (correctness gate)\n");
    if (result) { result->improved = false; result->evals = E.evals; }
    return false;
  }
  if (champMed >= incMed * 0.97) {
    fprintf(stderr, "evo: %d evals / %d compiles / %d gens — best candidate "
            "%.1f ms did not beat incumbent %.1f ms by >3%% (noise); "
            "keeping -O3 defaults\n",
            E.evals, E.compiles, E.generations, champMed, incMed);
    if (result) { result->improved = false; result->evals = E.evals; }
    return false;
  }

  // ---- report winner
  double speedup = incMed / champMed;
  fprintf(stderr, "evo: %d evals / %d compiles / %d gens\n",
          E.evals, E.compiles, E.generations);
  fprintf(stderr, "evo: winner %.1f ms vs %.1f ms (%.2fx)  config: -O%d "
          "march=%s threads=%d pin=%d chunk=%ld serial-cutoff=%ld veclib=%s\n",
          champMed, incMed, speedup, E.optLevels[bestG.optIdx],
          E.marches[bestG.marchIdx].c_str(), bestG.threads, bestG.pin,
          E.chunks[bestG.chunkIdx],
          E.cutoffs[bestG.cutoffIdx] > 0 ? E.cutoffs[bestG.cutoffIdx] : -1,
          bestG.veclib ? "libmvec" : "none");

  if (result) {
    result->improved = true;
    result->clangOpt = E.optLevels[bestG.optIdx];
    result->march = E.marches[bestG.marchIdx];
    result->threads = bestG.threads;
    result->pin = bestG.pin;
    result->chunk = E.chunks[bestG.chunkIdx];
    result->serialCutoff = E.cutoffs[bestG.cutoffIdx] > 0
                             ? E.cutoffs[bestG.cutoffIdx] : -1;
    result->veclib = bestG.veclib;
    result->baseMs = incMed;
    result->bestMs = champMed;
    result->evals = E.evals;
    result->compiles = E.compiles;
    result->generations = E.generations;
  }
  return true;
}

} // namespace freshet
