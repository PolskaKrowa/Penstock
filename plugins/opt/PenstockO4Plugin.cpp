// Penstock -O4 pass plugin for LLVM's new pass manager.
//
// `penstock-o4` runs LLVM's target-neutral O3 pipeline followed by Penstock's
// bounded finite-domain rewriter and embedded Freshet/Z3/Alive2 superoptimiser.
// The AST-only source transformations in Penstock's compiler
// are not repeated here because arbitrary LLVM IR no longer carries that AST.
#include "common.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalAlias.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Passes/PassBuilder.h"
#if __has_include("llvm/Plugins/PassPlugin.h")
#include "llvm/Plugins/PassPlugin.h" // LLVM 23+
#elif __has_include("llvm/Passes/PassPlugin.h")
#include "llvm/Passes/PassPlugin.h" // LLVM 19-22
#else
#error "Could not find LLVM's PassPlugin.h in the selected LLVM SDK"
#endif
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/ValueMapper.h"

#include <cerrno>
#include <cstdlib>
#include <string>
#include <unistd.h>

using namespace llvm;

#ifndef PENSTOCK_PLUGIN_LLVM_ROOT
#define PENSTOCK_PLUGIN_LLVM_ROOT ""
#endif
#ifndef PENSTOCK_PLUGIN_CLANG
#define PENSTOCK_PLUGIN_CLANG "clang"
#endif

namespace {

static bool envEnabled(const char *name) {
  const char *value = std::getenv(name);
  if (!value || !*value) return false;
  return std::string(value) != "0" && std::string(value) != "false" &&
         std::string(value) != "off";
}

static int solverBudgetSeconds() {
  const char *value = std::getenv("PENSTOCK_O4_BUDGET");
  if (!value || !*value) return 10;
  char *end = nullptr;
  long parsed = std::strtol(value, &end, 10);
  if (end == value || *end != '\0' || parsed < 1) return 10;
  if (parsed > 300) parsed = 300;
  return static_cast<int>(parsed);
}

class TemporaryIR {
public:
  TemporaryIR() {
    const char *tmp = std::getenv("TMPDIR");
    std::string dir = (tmp && *tmp) ? tmp : "/tmp";
    if (!dir.empty() && dir.back() != '/') dir.push_back('/');
    path_ = dir + "penstock-o4-XXXXXX.ll";
    std::vector<char> pattern(path_.begin(), path_.end());
    pattern.push_back('\0');
    fd_ = ::mkstemps(pattern.data(), 3); // keep .ll extension for Clang/opt
    if (fd_ >= 0) path_ = pattern.data();
  }
  ~TemporaryIR() {
    if (fd_ >= 0) ::close(fd_);
    if (!path_.empty()) ::unlink(path_.c_str());
  }
  bool valid() const { return fd_ >= 0 && !path_.empty(); }
  const std::string &path() const { return path_; }

  bool writeModule(const Module &M) {
    if (!valid()) return false;
    ::close(fd_);
    fd_ = -1;
    std::error_code ec;
    raw_fd_ostream out(path_, ec, sys::fs::OF_Text);
    if (ec) {
      errs() << "penstock-o4: cannot write temporary IR: " << ec.message() << '\n';
      return false;
    }
    M.print(out, nullptr);
    out.flush();
    return !out.has_error();
  }

private:
  std::string path_;
  int fd_ = -1;
};

// Copy a candidate module's IR back into the Module owned by opt. The external
// rewriters only change function bodies and may add constant lookup-table
// globals, so retain the existing Module object and clone those parts into it.
static bool importCandidate(Module &destination, Module &candidate) {
  ValueToValueMapTy valueMap;

  // Map all globals before initialisers or function bodies are copied. This
  // ensures that references from a generated lookup table point at the
  // destination module's objects rather than at the temporary parsed module.
  SmallVector<std::pair<const GlobalVariable *, GlobalVariable *>, 8> newGlobals;
  for (GlobalVariable &src : candidate.globals()) {
    GlobalVariable *dst = destination.getGlobalVariable(src.getName(), true);
    if (!dst) {
      dst = new GlobalVariable(destination, src.getValueType(), src.isConstant(),
                               src.getLinkage(), nullptr, src.getName(), nullptr,
                               src.getThreadLocalMode(), src.getAddressSpace(),
                               src.isExternallyInitialized());
      dst->setVisibility(src.getVisibility());
      dst->setDLLStorageClass(src.getDLLStorageClass());
      dst->setDSOLocal(src.isDSOLocal());
      dst->setUnnamedAddr(src.getUnnamedAddr());
      if (src.hasSection()) dst->setSection(src.getSection());
      if (src.getAlign()) dst->setAlignment(*src.getAlign());
      newGlobals.push_back({&src, dst});
    }
    valueMap[&src] = dst;
  }

  for (GlobalAlias &src : candidate.aliases()) {
    if (GlobalAlias *dst = destination.getNamedAlias(src.getName()))
      valueMap[&src] = dst;
  }

  SmallVector<std::pair<const Function *, Function *>, 32> functions;
  for (Function &src : candidate) {
    Function *dst = destination.getFunction(src.getName());
    if (!dst) {
      dst = Function::Create(src.getFunctionType(), src.getLinkage(),
                             src.getAddressSpace(), src.getName(), &destination);
      dst->setVisibility(src.getVisibility());
      dst->setDLLStorageClass(src.getDLLStorageClass());
      dst->setDSOLocal(src.isDSOLocal());
      dst->setUnnamedAddr(src.getUnnamedAddr());
      if (src.hasSection()) dst->setSection(src.getSection());
    }
    valueMap[&src] = dst;
    functions.push_back({&src, dst});
  }

  for (const auto &entry : newGlobals) {
    const GlobalVariable *src = entry.first;
    GlobalVariable *dst = entry.second;
    if (src->hasInitializer()) {
      Value *mapped = MapValue(src->getInitializer(), valueMap, RF_IgnoreMissingLocals);
      auto *initializer = dyn_cast_or_null<Constant>(mapped);
      if (!initializer) {
        errs() << "penstock-o4: could not remap a generated global initializer\n";
        return false;
      }
      dst->setInitializer(initializer);
    }
  }

  // Copy every definition. `CloneFunctionInto` maps all operands, PHIs,
  // blocks, debug metadata and calls using the global map above. The original
  // function objects remain stable for the pass manager and its analyses.
  for (const auto &entry : functions) {
    const Function *src = entry.first;
    Function *dst = entry.second;
    if (src->isDeclaration()) continue;
    if (!dst->isDeclaration()) dst->deleteBody();

    auto srcArg = src->arg_begin();
    auto dstArg = dst->arg_begin();
    for (; srcArg != src->arg_end() && dstArg != dst->arg_end(); ++srcArg, ++dstArg)
      valueMap[&*srcArg] = &*dstArg;

    SmallVector<ReturnInst *, 8> returns;
    CloneFunctionInto(dst, src, valueMap,
                      CloneFunctionChangeType::DifferentModule, returns);
    dst->setCallingConv(src->getCallingConv());
    dst->setAttributes(src->getAttributes());
    dst->setVisibility(src->getVisibility());
    dst->setDLLStorageClass(src->getDLLStorageClass());
    dst->setDSOLocal(src->isDSOLocal());
    if (src->hasSection()) dst->setSection(src->getSection());
  }

  if (verifyModule(destination, &errs())) {
    errs() << "penstock-o4: candidate import failed LLVM module verification\n";
    return false;
  }
  return true;
}

class PenstockVerifiedRewritePass : public PassInfoMixin<PenstockVerifiedRewritePass> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &) {
    TemporaryIR ir;
    if (!ir.valid() || !ir.writeModule(M)) {
      errs() << "penstock-o4: could not create temporary IR; rewrite stages skipped\n";
      return PreservedAnalyses::all();
    }

    // The existing file-based engines are reused here rather than forked into
    // separate plugin implementations. The verifier and instruction optimiser
    // are linked in-process; evaluator failures/timeouts never install a
    // candidate. These settings are isolated to this plugin's DSO.
    freshet::g_opts.optLevel = 4;
    freshet::g_opts.superoptBudgetSec = solverBudgetSeconds();
    freshet::g_opts.keepIr = envEnabled("PENSTOCK_O4_KEEP_IR");
    freshet::g_opts.noPrecompute = envEnabled("PENSTOCK_O4_DISABLE_PRECOMPUTE");
    freshet::g_opts.noSuperopt = envEnabled("PENSTOCK_O4_DISABLE_SUPEROPT");

    bool changed = false;
    const std::string llvmRoot = PENSTOCK_PLUGIN_LLVM_ROOT;
    const std::string clang = PENSTOCK_PLUGIN_CLANG;
    if (!freshet::g_opts.noPrecompute) {
      changed = freshet::runPrecomputeRewriter(
          clang, llvmRoot, ir.path(), 4, freshet::g_opts.superoptBudgetSec);
    }
    if (!freshet::g_opts.noSuperopt) {
      const bool superoptChanged = freshet::runVerifiedSuperoptimizer(
          llvmRoot, ir.path(), 4, freshet::g_opts.superoptBudgetSec);
      changed = changed || superoptChanged;
    }

    if (!changed) return PreservedAnalyses::all();

    SMDiagnostic diagnostic;
    std::unique_ptr<Module> candidate = parseIRFile(ir.path(), diagnostic, M.getContext());
    if (!candidate) {
      diagnostic.print("penstock-o4", errs());
      errs() << "penstock-o4: failed to parse verified candidate; original IR retained\n";
      return PreservedAnalyses::all();
    }
    if (verifyModule(*candidate, &errs())) {
      errs() << "penstock-o4: verified candidate failed LLVM verification; original IR retained\n";
      return PreservedAnalyses::all();
    }
    if (!importCandidate(M, *candidate)) {
      errs() << "penstock-o4: could not import candidate; output may require inspection\n";
    }
    return PreservedAnalyses::none();
  }
};

} // namespace

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "PenstockO4Plugin", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [&PB](StringRef name, ModulePassManager &MPM,
                      ArrayRef<PassBuilder::PipelineElement>) {
                  if (name == "penstock-o4") {
                    // LLVM's normal target-neutral O3 pass pipeline supplies
                    // inlining, loop transforms, SLP/vectorisation and cleanup.
                    // Penstock's custom verified stages run afterwards.
                    MPM.addPass(PB.buildPerModuleDefaultPipeline(OptimizationLevel::O3));
                    MPM.addPass(PenstockVerifiedRewritePass());
                    return true;
                  }
                  if (name == "penstock-o4-rewrites") {
                    // For users who already ran their own LLVM pipeline.
                    MPM.addPass(PenstockVerifiedRewritePass());
                    return true;
                  }
                  return false;
                });
          }};
}
