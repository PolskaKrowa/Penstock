// Low-level native services for the Freshet-hosted superoptimiser.
//
// Freshet owns the search/control loop (superopt.fsh). This translation unit
// deliberately contains only the parts that need native library APIs:
// LLVM IR parsing/materialisation, Z3 bit-vector proof obligations, and
// Alive2 translation validation. No external Souper/opt/alive-tv/z3 process
// is launched.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <z3++.h>

#include "llvm/ADT/APInt.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include "llvm_util/compare.h"
#include "llvm_util/llvm2alive.h"
#include "smt/smt.h"

namespace {

struct FshStrView {
  const char *p;
  int64_t len;
};

static std::string asString(FshStrView s) {
  if (!s.p || s.len <= 0) return {};
  return std::string(s.p, static_cast<size_t>(s.len));
}

static std::string readFile(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

static bool writeFileAtomically(const std::string &path,
                                const std::string &contents) {
  const std::string temporary = path + ".penstock-superopt.tmp";
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!out.good()) {
      out.close();
      std::remove(temporary.c_str());
      return false;
    }
  }
  if (std::rename(temporary.c_str(), path.c_str()) != 0) {
    std::remove(temporary.c_str());
    return false;
  }
  return true;
}

static bool validModule(llvm::Module &module, std::string *why = nullptr) {
  std::string message;
  llvm::raw_string_ostream out(message);
  const bool invalid = llvm::verifyModule(module, &out);
  out.flush();
  if (invalid && why) *why = message;
  return !invalid;
}

static llvm::Function *getFunction(llvm::Module &module,
                                   const std::string &name) {
  return module.getFunction(name);
}

// Alive2 keeps process-global LLVM and SMT state. In particular,
// smt_initializer::~smt_initializer() calls Z3_finalize_memory(). Creating
// and destroying those initialisers for every candidate/function repeatedly
// finalises and reinitialises Z3 inside one Penstock invocation. Alive2's own
// driver instead initialises once for a module, reuses the session for every
// check, and finalises once at process shutdown. Mirror that lifecycle here.
// Serialise access because Alive2's SMT context and output/configuration are
// process-global and are not safe for concurrent Verifier instances.
struct Alive2Session {
  std::mutex mutex;
  std::ostringstream output;
  // Declare the SMT initializer before the LLVM initializer so destruction
  // follows Alive2's own order: LLVM utility state first, then Z3/SMT state.
  std::unique_ptr<smt::smt_initializer> smtInit;
  std::unique_ptr<llvm::DataLayout> dataLayoutObject;
  std::unique_ptr<llvm_util::initializer> llvmInit;
  std::string dataLayout;
};

static Alive2Session &alive2Session() {
  static Alive2Session session;
  return session;
}

// In-process Alive2 translation validation. compareFunctions()'s boolean is
// not sufficient by itself: Alive2 can return true after a failed query, so
// all four verifier counters are checked as well.
static bool alive2Compare(llvm::Module &before, llvm::Module &after,
                          const std::string &functionName,
                          int64_t timeoutMs, std::string *diagnostic = nullptr) {
  if (before.getTargetTriple() != after.getTargetTriple() ||
      before.getDataLayoutStr() != after.getDataLayoutStr()) {
    if (diagnostic) *diagnostic = "target triple or data layout changed";
    return false;
  }
  llvm::Function *src = getFunction(before, functionName);
  llvm::Function *dst = getFunction(after, functionName);
  if (!src || !dst || src->isDeclaration() || dst->isDeclaration()) {
    if (diagnostic) *diagnostic = "function missing from one IR module";
    return false;
  }

  Alive2Session &session = alive2Session();
  std::lock_guard<std::mutex> lock(session.mutex);
  const std::string layout = before.getDataLayoutStr();
  const bool newSession = !session.llvmInit || !session.smtInit;
  if (newSession) {
    auto dataLayoutObject = std::make_unique<llvm::DataLayout>(
        before.getDataLayout());
    auto llvmInit = std::make_unique<llvm_util::initializer>(
        session.output, *dataLayoutObject);
    auto smtInit = std::make_unique<smt::smt_initializer>();
    session.dataLayout = layout;
    session.dataLayoutObject = std::move(dataLayoutObject);
    session.llvmInit = std::move(llvmInit);
    session.smtInit = std::move(smtInit);
  } else if (session.dataLayout != layout) {
    if (diagnostic)
      *diagnostic = "Alive2 session already initialised for a different data layout";
    return false;
  }

  // Keep diagnostics local to this comparison while retaining the stream
  // object for the lifetime of Alive2's initializer.
  session.output.str(std::string());
  session.output.clear();
  const llvm::Triple triple(before.getTargetTriple());
  llvm::TargetLibraryInfoWrapperPass tli(triple);
  // Alive2's command-line driver resets the SMT context between separate
  // transformations. This releases per-query solver state without invoking
  // Z3_finalize_memory() (which is reserved for the single session teardown).
  if (!newSession) session.smtInit->reset();
  if (timeoutMs > 0) smt::set_query_timeout(std::to_string(timeoutMs));

  llvm_util::Verifier verifier(tli, *session.smtInit, session.output);
  verifier.always_verify = true;
  const bool apiResult = verifier.compareFunctions(*src, *dst);
  const bool proven = apiResult && verifier.num_correct == 1 &&
                      verifier.num_unsound == 0 && verifier.num_failed == 0 &&
                      verifier.num_errors == 0;
  if (!proven && diagnostic) {
    *diagnostic = session.output.str();
    if (diagnostic->empty()) *diagnostic = "Alive2 did not report a clean proof";
  }
  return proven;
}

static bool alive2CompareModules(llvm::Module &before, llvm::Module &after,
                                 int64_t timeoutMs, std::string *diagnostic) {
  std::vector<std::string> names;
  for (llvm::Function &fn : before) {
    if (!fn.isDeclaration()) names.push_back(fn.getName().str());
  }
  if (names.empty()) {
    if (diagnostic) *diagnostic = "module contains no defined functions";
    return false;
  }
  const auto deadline = std::chrono::steady_clock::now() +
      std::chrono::milliseconds(std::max<int64_t>(1, timeoutMs));
  for (const std::string &name : names) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      if (diagnostic) *diagnostic = "Alive2 whole-module proof budget exhausted";
      return false;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - now).count();
    std::string why;
    if (!alive2Compare(before, after, name, std::max<int64_t>(1, remaining), &why)) {
      if (diagnostic) *diagnostic = name + ": " + why;
      return false;
    }
  }
  return true;
}

enum class RewriteKind {
  MulZero,
  Identity,
  MulShift,
  UDivShift,
  URemMask
};

struct RewriteDesc {
  RewriteKind kind;
  unsigned width = 0;
  uint64_t constant = 0;
  unsigned shift = 0;
  unsigned opcode = 0;
  bool constantOnLeft = false;
};

static bool describeRewrite(llvm::BinaryOperator &op, RewriteDesc *desc) {
  if (!op.getType()->isIntegerTy()) return false;
  const unsigned width = op.getType()->getIntegerBitWidth();
  if (width == 0 || width > 64) return false;
  // LLVM exposes these flags through opcode-specific operator casts. Calling
  // them on an unrelated BinaryOperator is invalid (for example, isExact()
  // is only meaningful for udiv/sdiv/shift-right instructions).
  if (op.getOpcode() == llvm::Instruction::Mul &&
      (op.hasNoUnsignedWrap() || op.hasNoSignedWrap()))
    return false;
  if (op.getOpcode() == llvm::Instruction::UDiv && op.isExact())
    return false;

  auto *left = llvm::dyn_cast<llvm::ConstantInt>(op.getOperand(0));
  auto *right = llvm::dyn_cast<llvm::ConstantInt>(op.getOperand(1));
  llvm::ConstantInt *constant = right;
  bool constantOnLeft = false;
  if (!constant && op.getOpcode() == llvm::Instruction::Mul && left) {
    constant = left;
    constantOnLeft = true;
  }
  if (!constant) return false;
  const llvm::APInt value = constant->getValue();
  if (value.getActiveBits() > 64) return false;
  const uint64_t k = value.getZExtValue();
  desc->width = width;
  desc->constant = k;
  desc->opcode = op.getOpcode();
  desc->constantOnLeft = constantOnLeft;

  switch (op.getOpcode()) {
    case llvm::Instruction::Mul:
      if (k == 0) desc->kind = RewriteKind::MulZero;
      else if (k == 1) desc->kind = RewriteKind::Identity;
      else if ((k & (k - 1)) == 0) {
        desc->kind = RewriteKind::MulShift;
        desc->shift = static_cast<unsigned>(__builtin_ctzll(k));
      } else return false;
      return true;
    case llvm::Instruction::UDiv:
      if (k == 1) desc->kind = RewriteKind::Identity;
      else if (k != 0 && (k & (k - 1)) == 0) {
        desc->kind = RewriteKind::UDivShift;
        desc->shift = static_cast<unsigned>(__builtin_ctzll(k));
      } else return false;
      return true;
    case llvm::Instruction::URem:
      if (k == 0) return false; // division by zero is poison; never touch it
      desc->kind = RewriteKind::URemMask;
      desc->shift = (k == 1) ? 0u : static_cast<unsigned>(__builtin_ctzll(k));
      // Only powers of two have the low-bit remainder identity.
      return (k & (k - 1)) == 0;
    default:
      return false;
  }
}

static bool z3ProvesRewrite(const RewriteDesc &desc, int64_t timeoutMs) {
  try {
    z3::context context;
    z3::expr x = context.bv_const("x", desc.width);
    z3::expr k = context.bv_val(desc.constant, desc.width);
    z3::expr original = x;
    z3::expr replacement = x;
    const z3::expr shift = context.bv_val(desc.shift, desc.width);
    switch (desc.kind) {
      case RewriteKind::MulZero:
        original = x * k;
        replacement = context.bv_val(0, desc.width);
        break;
      case RewriteKind::Identity:
        if (desc.opcode == llvm::Instruction::Mul) original = x * k;
        else if (desc.opcode == llvm::Instruction::UDiv) original = z3::udiv(x, k);
        else return false;
        replacement = x;
        break;
      case RewriteKind::MulShift:
        original = x * k;
        replacement = z3::shl(x, shift);
        break;
      case RewriteKind::UDivShift:
        original = z3::udiv(x, k);
        replacement = z3::lshr(x, shift);
        break;
      case RewriteKind::URemMask: {
        original = z3::urem(x, k);
        const uint64_t mask = desc.constant - 1;
        replacement = x & context.bv_val(mask, desc.width);
        break;
      }
    }
    z3::solver solver(context);
    z3::params parameters(context);
    if (timeoutMs > 0)
      parameters.set("timeout", static_cast<unsigned>(std::min<int64_t>(timeoutMs, 0xffffffffLL)));
    solver.set(parameters);
    solver.add(original != replacement);
    return solver.check() == z3::unsat;
  } catch (const z3::exception &e) {
    std::fprintf(stderr, "superopt: Z3 proof error: %s\n", e.msg());
    return false;
  }
}

static llvm::Value *materialiseRewrite(llvm::BinaryOperator &op,
                                       const RewriteDesc &desc) {
  llvm::IRBuilder<> builder(&op);
  builder.SetCurrentDebugLocation(op.getDebugLoc());
  llvm::Value *x = op.getOperand(desc.constantOnLeft ? 1 : 0);
  llvm::Type *type = op.getType();
  switch (desc.kind) {
    case RewriteKind::MulZero:
      return llvm::ConstantInt::get(type, 0);
    case RewriteKind::Identity:
      return x;
    case RewriteKind::MulShift:
    case RewriteKind::UDivShift:
      return desc.kind == RewriteKind::MulShift
          ? builder.CreateShl(x, llvm::ConstantInt::get(type, desc.shift),
                              op.getName() + ".so")
          : builder.CreateLShr(x, llvm::ConstantInt::get(type, desc.shift),
                               op.getName() + ".so");
    case RewriteKind::URemMask: {
      const uint64_t mask = desc.constant - 1;
      return builder.CreateAnd(x, llvm::ConstantInt::get(type, mask),
                               op.getName() + ".so");
    }
  }
  return nullptr;
}

struct CostVector {
  uint64_t instructions = 0;
  uint64_t latency = 0;
};

static CostVector moduleCost(const llvm::Module &module) {
  CostVector cost;
  for (const llvm::Function &fn : module) {
    if (fn.isDeclaration()) continue;
    for (const llvm::BasicBlock &bb : fn) {
      for (const llvm::Instruction &inst : bb) {
        ++cost.instructions;
        // Treat control-flow terminators with successors as branches for the
        // cost model. This deliberately avoids naming concrete branch classes:
        // newer LLVM revisions split BranchInst into CondBrInst and
        // UncondBrInst, while older revisions expose BranchInst. The generic
        // Instruction API works across both representations (and also covers
        // switch/indirect control-flow terminators).
        if (inst.isTerminator() && inst.getNumSuccessors() != 0) {
          cost.latency += 2;
          continue;
        }
        switch (inst.getOpcode()) {
          case llvm::Instruction::Mul: cost.latency += 5; break;
          case llvm::Instruction::UDiv: cost.latency += 20; break;
          case llvm::Instruction::URem: cost.latency += 26; break;
          case llvm::Instruction::SDiv: cost.latency += 20; break;
          case llvm::Instruction::SRem: cost.latency += 26; break;
          case llvm::Instruction::Load:
          case llvm::Instruction::Store: cost.latency += 4; break;
          case llvm::Instruction::Shl:
          case llvm::Instruction::LShr:
          case llvm::Instruction::And: cost.latency += 1; break;
          case llvm::Instruction::ICmp:
          case llvm::Instruction::Select: cost.latency += 2; break;
          default: cost.latency += 1; break;
        }
      }
    }
  }
  return cost;
}

static bool costWins(const CostVector &before, const CostVector &after,
                     int64_t objective) {
  const bool smaller = after.instructions < before.instructions &&
                       after.latency <= before.latency;
  const bool faster = after.latency < before.latency &&
                      after.instructions <= before.instructions +
                          before.instructions / 4;
  if (objective == 1) return smaller;
  if (objective == 2) return smaller || faster;
  return faster;
}

static const char *const kCandidateMetadata = "penstock.superopt.candidate";

static void tagCandidates(llvm::Module &module,
                          std::vector<std::pair<uint64_t, std::string>> *out) {
  uint64_t id = 1;
  llvm::LLVMContext &context = module.getContext();
  for (llvm::Function &fn : module) {
    if (fn.isDeclaration()) continue;
    for (llvm::Instruction &inst : llvm::instructions(fn)) {
      auto *binary = llvm::dyn_cast<llvm::BinaryOperator>(&inst);
      if (!binary) continue;
      RewriteDesc desc{};
      if (!describeRewrite(*binary, &desc)) continue;
      llvm::Metadata *value = llvm::MDString::get(context, std::to_string(id));
      inst.setMetadata(kCandidateMetadata, llvm::MDNode::get(context, {value}));
      out->push_back({id, fn.getName().str()});
      ++id;
    }
  }
}

static llvm::Instruction *findCandidate(llvm::Module &module, uint64_t id) {
  for (llvm::Function &fn : module) {
    if (fn.isDeclaration()) continue;
    for (llvm::Instruction &inst : llvm::instructions(fn)) {
      llvm::MDNode *node = inst.getMetadata(kCandidateMetadata);
      if (!node || node->getNumOperands() != 1) continue;
      auto *text = llvm::dyn_cast<llvm::MDString>(node->getOperand(0));
      if (!text) continue;
      if (text->getString().str() == std::to_string(id)) return &inst;
    }
  }
  return nullptr;
}

static void clearCandidateMetadata(llvm::Module &module) {
  for (llvm::Function &fn : module) {
    if (fn.isDeclaration()) continue;
    for (llvm::Instruction &inst : llvm::instructions(fn))
      inst.setMetadata(kCandidateMetadata, nullptr);
  }
}

struct OptimizerState {
  std::unique_ptr<llvm::LLVMContext> context;
  std::unique_ptr<llvm::Module> best;
  std::string path;
  std::vector<std::pair<uint64_t, std::string>> candidates;
  size_t cursor = 0;
  int64_t budgetMs = 2000;
  int64_t objective = 0;
  std::chrono::steady_clock::time_point start;
  bool changed = false;
  bool finished = false;
};

static bool timedOut(const OptimizerState &state) {
  return state.budgetMs > 0 &&
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - state.start).count() >= state.budgetMs;
}

static std::unique_ptr<llvm::Module> parseModule(
    const std::string &path, llvm::LLVMContext &context, std::string *why) {
  llvm::SMDiagnostic diagnostic;
  std::unique_ptr<llvm::Module> module = llvm::parseIRFile(path, diagnostic, context);
  if (!module && why) {
    std::string message;
    llvm::raw_string_ostream out(message);
    diagnostic.print("penstock", out);
    out.flush();
    *why = message;
  }
  return module;
}

} // namespace

extern "C" int64_t penstock_so_begin(FshStrView pathView, int64_t level,
                                      int64_t budgetMs, int64_t objective) {
  try {
    auto state = std::make_unique<OptimizerState>();
    state->path = asString(pathView);
    state->budgetMs = std::max<int64_t>(1, budgetMs);
    state->objective = objective;
    state->start = std::chrono::steady_clock::now();
    const std::string text = readFile(state->path);
    if (text.empty()) {
      std::fprintf(stderr, "superopt: skipped — input IR is unreadable\n");
      return 0;
    }
    const bool wide = level >= 4;
    const size_t byteLimit = wide ? 4u * 1024u * 1024u : 256u * 1024u;
    if (text.size() > byteLimit) {
      std::fprintf(stderr, "superopt: skipped — module exceeds %s byte budget\n",
                   wide ? "-O4" : "-O3");
      return 0;
    }
    state->context = std::make_unique<llvm::LLVMContext>();
    std::string why;
    state->best = parseModule(state->path, *state->context, &why);
    if (!state->best || !validModule(*state->best, &why)) {
      std::fprintf(stderr, "superopt: cannot parse/verify input IR: %s\n", why.c_str());
      return 0;
    }
    tagCandidates(*state->best, &state->candidates);
    if (state->candidates.empty()) {
      std::fprintf(stderr, "superopt: no supported integer strength-reduction candidates\n");
      return 0;
    }
    if (state->candidates.size() > (wide ? 4000u : 500u))
      state->candidates.resize(wide ? 4000u : 500u);
    std::fprintf(stderr, "superopt: Freshet engine found %zu bounded candidates\n",
                 state->candidates.size());
    return static_cast<int64_t>(reinterpret_cast<uintptr_t>(state.release()));
  } catch (const std::exception &e) {
    std::fprintf(stderr, "superopt: initialisation failed: %s\n", e.what());
    return 0;
  } catch (...) {
    std::fprintf(stderr, "superopt: initialisation failed\n");
    return 0;
  }
}

extern "C" int64_t penstock_so_step(int64_t handle) {
  if (!handle) return 0;
  auto *state = reinterpret_cast<OptimizerState *>(static_cast<uintptr_t>(handle));
  if (!state || state->finished || timedOut(*state)) return 0;
  try {
    if (state->cursor >= state->candidates.size()) return 0;
    const auto candidate = state->candidates[state->cursor++];
    const int64_t elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - state->start).count();
    const int64_t remainingMs = std::max<int64_t>(1, state->budgetMs - elapsedMs);
    auto *currentInstruction = findCandidate(*state->best, candidate.first);
    auto *currentBinary = llvm::dyn_cast_or_null<llvm::BinaryOperator>(currentInstruction);
    if (!currentBinary) return state->cursor < state->candidates.size() ? 1 : 0;

    RewriteDesc desc{};
    if (!describeRewrite(*currentBinary, &desc) || !z3ProvesRewrite(desc, remainingMs))
      return state->cursor < state->candidates.size() ? 1 : 0;

    std::unique_ptr<llvm::Module> trial = llvm::CloneModule(*state->best);
    llvm::Instruction *trialInstruction = findCandidate(*trial, candidate.first);
    auto *trialBinary = llvm::dyn_cast_or_null<llvm::BinaryOperator>(trialInstruction);
    if (!trialBinary) return state->cursor < state->candidates.size() ? 1 : 0;
    llvm::Value *replacement = materialiseRewrite(*trialBinary, desc);
    if (!replacement) return state->cursor < state->candidates.size() ? 1 : 0;
    trialBinary->replaceAllUsesWith(replacement);
    trialBinary->eraseFromParent();

    std::string why;
    if (!validModule(*trial, &why)) {
      std::fprintf(stderr, "superopt: LLVM rejected a generated candidate: %s\n", why.c_str());
      return state->cursor < state->candidates.size() ? 1 : 0;
    }
    const CostVector beforeCost = moduleCost(*state->best);
    const CostVector afterCost = moduleCost(*trial);
    if (!costWins(beforeCost, afterCost, state->objective))
      return state->cursor < state->candidates.size() ? 1 : 0;
    if (timedOut(*state)) return 0;

    const int64_t verifyElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - state->start).count();
    const int64_t verifyRemainingMs = std::max<int64_t>(1, state->budgetMs - verifyElapsedMs);
    if (timedOut(*state)) return 0;
    if (!alive2Compare(*state->best, *trial, candidate.second,
                       verifyRemainingMs, &why)) {
      std::fprintf(stderr, "superopt: Alive2 rejected candidate in '%s': %s\n",
                   candidate.second.c_str(), why.c_str());
      return state->cursor < state->candidates.size() ? 1 : 0;
    }
    state->best = std::move(trial);
    state->changed = true;
    std::fprintf(stderr,
        "superopt: Z3-equivalent, Alive2-verified Freshet rewrite in '%s'\n",
        candidate.second.c_str());
    return state->cursor < state->candidates.size() ? 1 : 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "superopt: candidate failed safely: %s\n", e.what());
    return state->cursor < state->candidates.size() ? 1 : 0;
  } catch (...) {
    std::fprintf(stderr, "superopt: candidate failed safely\n");
    return state->cursor < state->candidates.size() ? 1 : 0;
  }
}

extern "C" int64_t penstock_so_finish(int64_t handle) {
  if (!handle) return 0;
  std::unique_ptr<OptimizerState> state(
      reinterpret_cast<OptimizerState *>(static_cast<uintptr_t>(handle)));
  if (!state || state->finished) return 0;
  state->finished = true;
  if (!state->changed) return 0;
  try {
    clearCandidateMetadata(*state->best);
    std::string why;
    if (!validModule(*state->best, &why)) {
      std::fprintf(stderr, "superopt: final LLVM verification failed: %s\n", why.c_str());
      return 0;
    }
    std::string output;
    llvm::raw_string_ostream out(output);
    state->best->print(out, nullptr);
    out.flush();
    if (!writeFileAtomically(state->path, output)) {
      std::fprintf(stderr, "superopt: failed to atomically write verified IR; original retained\n");
      return 0;
    }
    std::fprintf(stderr, "superopt: installed verified in-process IR rewrites\n");
    return 1;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "superopt: finalisation failed: %s\n", e.what());
    return 0;
  }
}

extern "C" int64_t penstock_so_abort(int64_t handle) {
  if (!handle) return 0;
  delete reinterpret_cast<OptimizerState *>(static_cast<uintptr_t>(handle));
  return 1;
}

extern "C" int64_t penstock_alive2_verify_files(FshStrView beforeView,
                                                 FshStrView afterView,
                                                 int64_t timeoutMs) {
  try {
    const std::string beforePath = asString(beforeView);
    const std::string afterPath = asString(afterView);
    if (beforePath.empty() || afterPath.empty()) return 0;
    llvm::LLVMContext context;
    std::string why;
    auto before = parseModule(beforePath, context, &why);
    if (!before || !validModule(*before, &why)) {
      std::fprintf(stderr, "precompute: Alive2 input IR invalid: %s\n", why.c_str());
      return 0;
    }
    auto after = parseModule(afterPath, context, &why);
    if (!after || !validModule(*after, &why)) {
      std::fprintf(stderr, "precompute: Alive2 candidate IR invalid: %s\n", why.c_str());
      return 0;
    }
    if (!alive2CompareModules(*before, *after, timeoutMs, &why)) {
      std::fprintf(stderr, "precompute: embedded Alive2 rejected candidate: %s\n", why.c_str());
      return 0;
    }
    return 1;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "precompute: embedded Alive2 error: %s\n", e.what());
    return 0;
  } catch (...) {
    std::fprintf(stderr, "precompute: embedded Alive2 error\n");
    return 0;
  }
}
