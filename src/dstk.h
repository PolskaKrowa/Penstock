// Freshet — data-stack memory model (goose port): shared analysis results.
// Computed by src/dstk.cpp BEFORE IRgen (runDstkAnalysis) and consumed by
// the IRgen emitters (irgen.cpp / irgen_call.cpp / irgen_stmt.cpp /
// irgen_parfor.cpp).
#pragma once
#include "common.h"
#include <map>

namespace freshet {
namespace dstk {

// per-function layout facts (goose codegen_stacks.h's stackown/stackneed)
struct Info {
  bool uses = false;     // body contains a stack_alloc site
  bool needsSp = false;  // uses, or (transitively) calls a fn that uses,
                         // or dispatches a parfor region that uses
  int own = 0;           // stacks this fn's own body holds at once
  int need = 0;          // own + everything callees/regions add above it
};

// per-REGION layout facts (a dispatched parfor body = its own chunk
// function, addressed at %sp = the dispatching context's first-free index;
// goose's workers are separate programs — here the index threads through
// frt_parfor's range struct and the chunk fn's hidden %sp parameter)
struct RegionInfo {
  bool uses = false;     // the region body contains a stack_alloc site
  int own = 0;           // stacks the chunk fn's own body holds at once
  int need = 0;          // own + everything the body's callees add above it
};

extern std::map<FnDecl*, Info> g_info;
extern std::map<const Stmt*, RegionInfo> g_regions;
// the stack index assigned to a stack_alloc CALL site (scope-assigned,
// mirroring IRgen's BLOCK scoping exactly — the same walk, the same tree;
// a site inside a dispatched parfor body gets a REGION-LOCAL index,
// addressed at the chunk fn's %sp base)
extern std::map<const Expr*, int> g_site;
extern int g_total;  // the program's stack count N (0 = no stacks)

inline const Info* infoOf(FnDecl* f) {
  auto it = g_info.find(f);
  return it == g_info.end() ? nullptr : &it->second;
}
inline const RegionInfo* regionInfoOf(const Stmt* pf) {
  auto it = g_regions.find(pf);
  return it == g_regions.end() ? nullptr : &it->second;
}
inline int siteIndex(const Expr* e) {
  auto it = g_site.find(e);
  return it == g_site.end() ? -1 : it->second;
}

}  // namespace dstk
}  // namespace freshet
