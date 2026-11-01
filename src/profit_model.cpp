// Freshet — shared profitability model (implementation). See profit_model.h.
#include "profit_model.h"

namespace freshet {
namespace profit {

TargetProfile& target() {
  static TargetProfile tp;
  return tp;
}

BodyCost estimateBody(const optutil::BodyInfo& info) {
  BodyCost c;
  c.nodes = info.nodes;
  c.branches = info.branches;
  c.laneWork = info.laneWork;
  c.ops = info.laneWork;                  // arithmetic + memory ops
  for (auto& kv : info.reads) c.memOps += (int)kv.second.size();
  for (auto& kv : info.writes) c.memOps += (int)kv.second.size();
  for (auto& kv : info.reads) c.bytesPerIter += 8 * (int64_t)kv.second.size();
  for (auto& kv : info.writes) c.bytesPerIter += 8 * (int64_t)kv.second.size();
  // calls: every root access summary that came from a call argument is
  // approximated by counting escaped names — cheap and monotone
  c.calls = (int)info.escapedNames.size();
  return c;
}

int chooseUnrollFactor(const BodyCost& c, int64_t trip) {
  if (trip <= 16) return 0;               // full unroller owns this range
  for (int f : {8, 4, 2}) {
    if ((int64_t)c.nodes * f > 200) continue;
    if (c.ops * f > 96) continue;         // register-pressure guard
    if (trip < (int64_t)f * 2) continue;  // too few iterations to amortise
    return f;
  }
  return 0;
}

void chooseTileEdges(int64_t distinctStreams, int64_t elemBytes,
                     int* ti, int* tj) {
  TargetProfile& tp = target();
  if (elemBytes < 1) elemBytes = 8;
  if (distinctStreams < 1) distinctStreams = 2;
  // default: vector-width rows, one cache line of columns
  int a = tp.vectorWidth;
  int b = (int)(tp.cacheLine / elemBytes);
  if (b < 1) b = 1;
  // grow the row edge while half of L1 still holds the tile working set
  int64_t l1Half = tp.l1Bytes / 2;
  while ((int64_t)(a * 2) * b * distinctStreams * elemBytes <= l1Half / 4)
    a *= 2;
  while ((int64_t)a * (b * 2) * distinctStreams * elemBytes <= l1Half / 2)
    b *= 2;
  if (a > 256) a = 256;
  if (b > 512) b = 512;
  *ti = a;
  *tj = b;
}

std::string describe(const BodyCost& c) {
  return "ops=" + std::to_string(c.ops) +
         ",mem=" + std::to_string(c.memOps) +
         ",bytes/iter=" + std::to_string(c.bytesPerIter) +
         ",nodes=" + std::to_string(c.nodes);
}

} // namespace profit
} // namespace freshet
