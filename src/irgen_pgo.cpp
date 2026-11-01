// Freshet PGO — .fprof parsing (text) and profile lookup helpers.
// Format:
//   FRESHET-PROF 1
//   F <fnhash> <count>
//   B <edgehash> <count>      (edgehash = sitehash or sitehash ^ 0x5A5A..)
#include "irgen.h"
#include <algorithm>

namespace freshet {

extern IrGen* G;

struct ProfData {
  std::unordered_map<uint64_t, uint64_t> fnCount;
  std::unordered_map<uint64_t, uint64_t> brEdgeCount;
  bool loaded = false;
  std::string path;
};
static ProfData g_profData;

void loadProfile(const std::string& path) {
  FILE* f = fopen(path.c_str(), "r");
  if (!f) {
    diag(Diag::Error, Code::Pgo, "cannot open profile: " + path, "", 0, 0);
    return;
  }
  char magic[64];
  if (!fgets(magic, sizeof magic, f) ||
      strncmp(magic, "FRESHET-PROF", 12) != 0) {
    diag(Diag::Error, Code::Pgo, "bad profile magic in " + path, "", 0, 0);
    fclose(f);
    return;
  }
  char kind;
  unsigned long long h, c, c2;
  while (fscanf(f, " %c %llx %llx", &kind, &h, &c) == 3) {
    if (kind == 'F') {
      g_profData.fnCount[h] = c;
    } else if (kind == 'B') {
      g_profData.brEdgeCount[h] = c;
    } else if (kind == 'P') {
      // optional parfor region data: P <tag> <trips> <threads>
      if (fscanf(f, " %llx", &c2) != 1) break;
    }
    (void)c2;
  }
  fclose(f);
  g_profData.loaded = true;
  g_profData.path = path;
}

uint64_t profileFnCount(uint64_t hash) {
  auto it = g_profData.fnCount.find(hash);
  return it == g_profData.fnCount.end() ? 0 : it->second;
}
void profileFnLookup(FnDecl* f) {
  if (!g_profData.loaded || f->profIdx < 0) return;
  FnProf& fp = G->fnProf[f->profIdx];
  fp.count = profileFnCount(fp.hash);
  fp.loaded = fp.count > 0;
}

// create a branch site with hash; attach loaded counts
int profSiteFor(uint64_t hash, SrcLoc loc) {
  BrProf bp;
  bp.siteIdx = (int)G->brProf.size();
  bp.hash = hash;
  bp.loc = loc;
  if (g_profData.loaded) {
    uint64_t t = 0, n = 0;
    auto it = g_profData.brEdgeCount.find(hash);
    if (it != g_profData.brEdgeCount.end()) t = it->second;
    auto it2 = g_profData.brEdgeCount.find(hash ^ 0x5A5A5A5A5A5A5A5Aull);
    if (it2 != g_profData.brEdgeCount.end()) n = it2->second;
    bp.taken = t;
    bp.notTaken = n;
    bp.loaded = (t + n) > 0;
  }
  G->brProf.push_back(bp);
  return bp.siteIdx;
}

// PGO report data
std::vector<std::pair<std::string, uint64_t>> profileTopFunctions() {
  std::vector<std::pair<std::string, uint64_t>> out;
  if (!g_profData.loaded || !G) return out;
  for (auto* f : G->prog->fns) {
    uint64_t h = hashName(f->name + ":" + std::to_string(f->loc.line));
    auto it = g_profData.fnCount.find(h);
    if (it != g_profData.fnCount.end() && it->second > 0)
      out.push_back({f->name, it->second});
  }
  std::sort(out.begin(), out.end(),
            [](auto& a, auto& b) { return a.second > b.second; });
  return out;
}

} // namespace freshet
