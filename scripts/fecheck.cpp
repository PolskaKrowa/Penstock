// fecheck — frontend-only validation driver for Penstock.
//
// Runs the exact pipeline the CLI runs UP TO the point where a real
// LLVM toolchain is needed: preprocess -> parse -> sema -> strict
// checks -> parfor checks -> ownership -> (loop passes + mem & vec
// stage at -O2+) -> IRgen, then writes the user .ll to stdout or a
// file.  Useful for sandbox/no-LLVM environments (scripts/build.sh
// builds it) and for pass-level differential testing of the IR text.
//
// usage: fecheck [flags] input.fsht [-o out.ll]
//   flags: -O0..-O4, and every loop-pass / mem-stage flag penstock
//   understands (--mem-reuse, --mov-minimize, --batch-ops,
//   --vector-plan, --smt-interleave, --no-loop-opts, --licm, ...).
#include "common.h"
#include "loop_analysis.h"
#include "loop_transforms.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace freshet;

// minimal option parsing: reuse g_opts directly
static void parseArgs(int argc, char** argv, std::string* input,
                      std::string* outLl) {
  std::vector<std::string> rest;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a.rfind("-o", 0) == 0 && a.size() > 2) { *outLl = a.substr(2); continue; }
    if (a == "-o" && i + 1 < argc) { *outLl = argv[++i]; continue; }
    if (a.rfind("-O", 0) == 0 && a.size() == 3 && a[2] >= '0' && a[2] <= '4') {
      g_opts.optLevel = a[2] - '0';
      continue;
    }
    // loop-pass flags
    if (a == "--no-loop-opts") { g_opts.noLoopOpts = true; continue; }
    if (a == "--licm") { g_opts.licmOn = true; continue; }
    if (a == "--partial-unroll") { g_opts.partialUnrollOn = true; continue; }
    if (a == "--peel") { g_opts.peelOn = true; continue; }
    if (a == "--interchange") { g_opts.interchangeOn = true; continue; }
    if (a == "--tile") { g_opts.tileOn = true; continue; }
    if (a == "--distribute") { g_opts.distributeOn = true; continue; }
    if (a == "--unroll-and-jam") { g_opts.unrollJamOn = true; continue; }
    if (a == "--parfor-advisor") { g_opts.parforAdvisorOn = true; continue; }
    // mem & vectorisation stage
    if (a == "--mem-stage") {
      g_opts.memReuseOn = g_opts.movMinOn = g_opts.batchOpsOn = true;
      continue;
    }
    if (a == "--mem-reuse") { g_opts.memReuseOn = true; continue; }
    if (a == "--no-mem-reuse") { g_opts.memReuseOn = false; continue; }
    if (a == "--mov-minimize") { g_opts.movMinOn = true; continue; }
    if (a == "--no-mov-minimize") { g_opts.movMinOn = false; continue; }
    if (a == "--batch-ops") { g_opts.batchOpsOn = true; continue; }
    if (a == "--no-batch-ops") { g_opts.batchOpsOn = false; continue; }
    if (a == "--vector-plan") { g_opts.vectorPlanOn = true; continue; }
    if (a == "--no-vector-plan") { g_opts.vectorPlanOn = false; continue; }
    if (a == "--smt-interleave") { g_opts.smtInterleaveOn = true; continue; }
    if (a == "--no-smt-interleave") { g_opts.smtInterleaveOn = false; continue; }
    // goose-ported passes
    if (a == "--bce") { g_opts.bceOn = true; continue; }
    if (a == "--no-bce") { g_opts.bceOn = false; continue; }
    if (a == "--bce-lines") { g_opts.bceLinesOn = true; continue; }
    if (a == "--stacks") { g_opts.dstkReportOn = true; continue; }
    if (a == "--sum-reduction") { g_opts.sumRedOn = true; continue; }
    if (a == "--no-sum-reduction") { g_opts.sumRedOn = false; continue; }
    if (a == "--basecase") { g_opts.basecaseOn = true; continue; }
    if (a == "--no-basecase") { g_opts.basecaseOn = false; continue; }
    if (a == "--checked") { g_opts.checked = true; continue; }
    if (a == "--no-bounds-check") { g_opts.noBoundsCheck = true; continue; }
    if (a.rfind("--target-vector-width=", 0) == 0) {
      g_opts.targetVectorWidth = atoi(a.c_str() + 22);
      continue;
    }
    if (a.rfind("-I", 0) == 0 && a.size() > 2) {
      g_opts.includeDirs.push_back(a.substr(2));
      continue;
    }
    if (a.rfind("--", 0) == 0) { rest.push_back(a); continue; }  // ignore
    *input = a;
  }
  (void)rest;
}

int main(int argc, char** argv) {
  std::string input, outLl;
  parseArgs(argc, argv, &input, &outLl);
  if (input.empty()) {
    fprintf(stderr, "usage: fecheck [flags] input.fsht [-o out.ll]\n");
    return 1;
  }

  // ---- module-aware load (preprocess -> parse -> import resolution)
  Program* prog = nullptr;
  {
    std::vector<std::string> linkLibs;
    prog = loadProgramWithInputs({input}, linkLibs);
  }
  if (!prog || errorCount()) {
    for (auto& d : g_diags)
      fprintf(stderr, "%s:%d:%d: %s [%s]\n", d.file.c_str(), d.line, d.col,
              d.msg.c_str(), d.code.c_str());
    return 1;
  }

  // ---- sema + checks
  if (!semaProgram(prog) || errorCount()) {
    for (auto& d : g_diags)
      fprintf(stderr, "%s:%d:%d: %s [%s]\n", d.file.c_str(), d.line, d.col,
              d.msg.c_str(), d.code.c_str());
    return 1;
  }
  runStrictChecks(prog);
  runParforChecks(prog);
  runOwnershipAnalysis(prog);

  // ---- loop passes + mem & vectorisation stage (same order as main)
  if (g_opts.optLevel >= 2 && !g_opts.noLoopOpts) {
    if (g_opts.basecaseOn) runBasecasePass(prog);
    runTypedConstFoldPass(prog);
    if (g_opts.sumRedOn) runFpReductionPass(prog);
    runFuseUnrollPasses(prog);
    runLaneAnalysisPass(prog);
    runLoopTransformPasses(prog);
    runMemStagePasses(prog);
    runVectorPlanPass(prog, /*applyBatching=*/true);
    runSmtInterleavePass(prog);
    if (g_opts.parforAdvisorOn) runParforAdvisorPass(prog);
  }
  // ---- bounds-check elimination + data-stack analysis (same order/gates
  // as main: BCE last before IRgen, dstk analysis right after it)
  if (g_opts.optLevel >= 1 && g_opts.bceOn) runBcePass(prog);
  runDstkAnalysis(prog);

  // ---- notes / diagnostics
  int errors = 0;
  for (auto& d : g_diags) {
    fprintf(stderr, "%s:%d:%d: %s%s [%s]\n", d.file.c_str(), d.line, d.col,
            d.msg.c_str(), d.extra.empty() ? "" : (" {" + d.extra + "}").c_str(),
            d.code.c_str());
    if (d.sev == Diag::Error) ++errors;
  }
  if (errors) return 1;

  // ---- IRgen
  g_targetTriple = "";   // irgen falls back to its built-in header pair
  g_dataLayout = "";
  if (!irgenProgram(prog, outLl.empty() ? "/dev/stdout" : outLl)) {
    fprintf(stderr, "fecheck: irgen failed\n");
    return 1;
  }
  if (!outLl.empty()) fprintf(stderr, "fecheck: IR written to %s\n", outLl.c_str());
  return 0;
}
