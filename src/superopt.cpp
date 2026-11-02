// C++/Freshet ABI boundary for Penstock's embedded superoptimiser.
// The search loop itself is compiled from src/superopt.fsh by the bootstrap
// compiler and linked into the final Penstock executable.
#include "common.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace freshet {

#if !defined(PENSTOCK_SUPEROPT_BOOTSTRAP) && defined(PENSTOCK_HAS_FSH_SUPEROPT)
struct FshStrView {
  const char *p;
  int64_t len;
};
extern "C" int64_t penstock_freshet_superopt(FshStrView path, int64_t level,
                                               int64_t budgetMs,
                                               int64_t objective);
#endif

bool runVerifiedSuperoptimizer(const std::string& /*llvmRoot*/,
                               const std::string& finalLl, int optLevel,
                               int budgetSec) {
#if !defined(PENSTOCK_SUPEROPT_BOOTSTRAP) && defined(PENSTOCK_HAS_FSH_SUPEROPT)
  if (budgetSec <= 0) budgetSec = (optLevel >= 4) ? 10 : 2;
  // Bound the C++/solver timeout conversion and pass milliseconds to the
  // in-process engines.  No child process, PATH lookup, or Souper plugin is
  // involved.
  const int64_t budgetMs = static_cast<int64_t>(budgetSec) * 1000;
  int64_t objective = 0; // latency
  if (g_opts.superoptObjective == "size") objective = 1;
  else if (g_opts.superoptObjective == "balanced") objective = 2;
  const FshStrView path{finalLl.c_str(), static_cast<int64_t>(finalLl.size())};
  const bool changed = penstock_freshet_superopt(
      path, static_cast<int64_t>(optLevel), budgetMs, objective) != 0;
  return changed;
#else
  (void)finalLl;
  (void)optLevel;
  (void)budgetSec;
  return false;
#endif
}

} // namespace freshet
