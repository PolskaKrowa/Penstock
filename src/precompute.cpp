// Exhaustive finite-domain algorithm precomputation for Penstock.
//
// For small, deterministic integer functions, enumerate every possible input
// tuple once, build the exact input->output map, replace the function body by a
// lookup in an immutable constant table, then require Alive2 to prove the
// replacement refines the original module.  This also handles inverse-search
// functions: a function taking a desired output and searching for a preimage
// can become a direct desired-output -> matching-input map.
//
// The evaluator is intentionally conservative. It only admits pure integer
// SSA after mem2reg/instcombine, direct calls into the same safe call graph,
// and integer-only control flow. No loads/stores, globals, syscalls, I/O,
// unknown callees, or recursive call cycles are admitted. Every evaluator
// process and every verifier invocation is time-limited. A timeout, malformed
// result, or failed proof leaves the original IR untouched.
#include "common.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace freshet {
namespace {

static std::string trimPC(const std::string& s) {
  const size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return {};
  const size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

static std::string shellQuotePC(const std::string& s) {
  std::string q = "'";
  for (char c : s) {
    if (c == '\'') q += "'\\''";
    else q += c;
  }
  q += '\'';
  return q;
}

static std::string readTextPC(const std::string& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return {};
  std::ostringstream s;
  s << in.rdbuf();
  return s.str();
}

static bool writeTextPC(const std::string& p, const std::string& s) {
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out.write(s.data(), static_cast<std::streamsize>(s.size()));
  return out.good();
}

static bool readBinaryPC(const std::string& p, std::vector<uint8_t>& bytes) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return false;
  in.seekg(0, std::ios::end);
  const std::streamoff n = in.tellg();
  if (n < 0) return false;
  in.seekg(0, std::ios::beg);
  bytes.resize(static_cast<size_t>(n));
  if (n && !in.read(reinterpret_cast<char*>(bytes.data()), n)) return false;
  return true;
}

static std::string resolveCommandPC(const char* envName, const std::string& fallback,
                                    const std::string& llvmRoot = {}) {
  const char* e = std::getenv(envName);
  if (e && *e) {
    std::string p(e);
    if (access(p.c_str(), X_OK) == 0) return p;
    if (p.find('/') == std::string::npos) {
      FILE* fp = popen(("command -v " + shellQuotePC(p) + " 2>/dev/null").c_str(), "r");
      if (fp) {
        char b[2048];
        std::string got;
        if (fgets(b, sizeof b, fp)) got = trimPC(b);
        pclose(fp);
        if (!got.empty() && access(got.c_str(), X_OK) == 0) return got;
      }
    }
    return {};
  }
  if (!llvmRoot.empty()) {
    const std::string p = llvmRoot + "/bin/" + fallback;
    if (access(p.c_str(), X_OK) == 0) return p;
  }
  FILE* fp = popen(("command -v " + shellQuotePC(fallback) + " 2>/dev/null").c_str(), "r");
  if (!fp) return {};
  char b[2048];
  std::string got;
  if (fgets(b, sizeof b, fp)) got = trimPC(b);
  pclose(fp);
  return (!got.empty() && access(got.c_str(), X_OK) == 0) ? got : std::string();
}

static bool runOkPC(const std::string& cmd) {
  fflush(nullptr);
  const int rc = std::system(cmd.c_str());
  return rc != -1 && WIFEXITED(rc) && WEXITSTATUS(rc) == 0;
}

struct PCArg {
  unsigned width = 0;
  std::string name;
  std::string abiAttrs;
};

struct PCFunction {
  std::string name;
  std::string header;
  std::string body;
  std::string retType;
  std::string returnAbi;
  std::string callConv;
  std::vector<PCArg> args;
  size_t begin = 0;
  size_t bodyBegin = 0;
  size_t bodyEnd = 0;
  size_t end = 0;
  unsigned retWidth = 0;
  bool scalarIntSignature = false;
  size_t instructions = 0;
  std::vector<std::string> callees;
};

static bool isIdentPC(char c) {
  const unsigned char u = static_cast<unsigned char>(c);
  return std::isalnum(u) || c == '_' || c == '.' || c == '$' || c == '-';
}

static bool isLabelLinePC(const std::string& line) {
  std::string code = trimPC(line);
  const size_t comment = code.find(';');
  if (comment != std::string::npos) code = trimPC(code.substr(0, comment));
  return !code.empty() && code.back() == ':';
}

static bool parseWidthPC(const std::string& t, unsigned& width) {
  std::smatch m;
  if (!std::regex_search(t, m, std::regex(R"(^\s*i([0-9]+)\b)"))) return false;

  // Do not use std::stoul here: Penstock's LLVM pass plugin inherits LLVM's
  // -fno-exceptions build flags. Parse the decimal digits directly and reject
  // overflow (and widths outside this pass's supported 1..64-bit domain).
  const std::string digits = m[1].str();
  unsigned parsed = 0;
  for (char c : digits) {
    if (c < '0' || c > '9') return false;
    const unsigned digit = static_cast<unsigned>(c - '0');
    if (parsed > (64U - digit) / 10U) return false;
    parsed = parsed * 10U + digit;
  }
  if (parsed < 1 || parsed > 64) return false;
  width = parsed;
  return true;
}

static std::string onlyAbiAttrsPC(const std::string& s) {
  std::string out;
  for (const char* attr : {"signext", "zeroext", "inreg"}) {
    if (std::regex_search(s, std::regex(std::string(R"(\b)") + attr + R"(\b)"))) {
      if (!out.empty()) out += ' ';
      out += attr;
    }
  }
  return out;
}

static std::string stripMetadataPC(std::string line) {
  // Debug and profiling metadata are not semantically part of the copied
  // evaluator module. Remove attachments such as , !dbg !123 and !prof !12.
  static const std::regex attach(R"(\s*,\s*![A-Za-z0-9_.$-]+\s+![0-9]+)");
  line = std::regex_replace(line, attach, "");
  static const std::regex headerAttach(R"(\s+![A-Za-z0-9_.$-]+\s+![0-9]+(?=\s*\{))");
  line = std::regex_replace(line, headerAttach, " ");
  // Attribute group IDs are module-local non-semantic bundles. The minimal
  // evaluator module does not copy them, so remove references to #N.
  static const std::regex attrRef(R"(\s+#\d+\b)");
  line = std::regex_replace(line, attrRef, " ");
  return line;
}

static bool parseFunctionHeaderPC(const std::string& line, PCFunction& f) {
  if (line.rfind("define ", 0) != 0) return false;
  const size_t at = line.find('@');
  if (at == std::string::npos || at + 1 >= line.size()) return false;
  // Quoted LLVM names are legal but are not emitted by Freshet for user
  // functions. Conservatively skip them rather than guessing the escaping.
  if (line[at + 1] == '"') return false;
  size_t nameEnd = at + 1;
  while (nameEnd < line.size() && isIdentPC(line[nameEnd])) ++nameEnd;
  if (nameEnd == at + 1) return false;
  f.name = line.substr(at + 1, nameEnd - at - 1);
  const size_t paren = line.find('(', nameEnd);
  if (paren == std::string::npos) return false;
  size_t close = paren;
  int depth = 0;
  for (; close < line.size(); ++close) {
    if (line[close] == '(') ++depth;
    else if (line[close] == ')' && --depth == 0) break;
  }
  if (close >= line.size()) return false;
  const size_t brace = line.find('{', close + 1);
  if (brace == std::string::npos) return false;

  const std::string beforeName = line.substr(7, at - 7);
  std::smatch retMatch;
  if (!std::regex_search(beforeName, retMatch, std::regex(R"((i[0-9]+)\s*$)")))
    return false;
  f.retType = retMatch[1].str();
  if (!parseWidthPC(f.retType, f.retWidth)) return false;
  const size_t retAt = beforeName.rfind(f.retType);
  f.returnAbi = onlyAbiAttrsPC(beforeName.substr(0, retAt));
  const std::string ccPrefix = trimPC(beforeName.substr(0, retAt));
  // Canonicalisation may assign fastcc to internal functions. Carry the
  // convention into the evaluator call instead of treating it as a semantic
  // restriction. Rare target-specific conventions are still rejected.
  static const std::regex ccMatch(R"(\b(ccc|fastcc|coldcc|ghccc|hipecc|webkit_jscc|anyregcc|preserve_mostcc|preserve_allcc|swiftcc|tailcc|cxx_fast_tlscc|cc\s+[0-9]+)\b)");
  std::smatch cc;
  f.callConv = std::regex_search(ccPrefix, cc, ccMatch) ? trimPC(cc[1].str()) : "";
  if (f.callConv.rfind("cc ", 0) == 0 && f.callConv.size() > 16) return false;

  const std::string params = line.substr(paren + 1, close - paren - 1);
  if (trimPC(params) != "") {
    size_t start = 0;
    int nested = 0;
    for (size_t i = 0; i <= params.size(); ++i) {
      if (i < params.size() && params[i] == '(') ++nested;
      if (i < params.size() && params[i] == ')') --nested;
      if (i != params.size() && (params[i] != ',' || nested != 0)) continue;
      const std::string p = trimPC(params.substr(start, i - start));
      start = i + 1;
      PCArg arg;
      unsigned width = 0;
      if (!parseWidthPC(p, width)) return false;
      const size_t tyEnd = p.find_first_of(" \t");
      if (tyEnd == std::string::npos) return false;
      const std::string tail = p.substr(tyEnd + 1);
      std::smatch nameMatch;
      if (!std::regex_search(tail, nameMatch, std::regex(R"((%[-A-Za-z$._0-9]+)\s*$)")))
        return false;  // no explicit SSA parameter name
      arg.width = width;
      arg.name = nameMatch[1].str();
      const size_t argNameAt = tail.rfind(arg.name);
      arg.abiAttrs = onlyAbiAttrsPC(tail.substr(0, argNameAt));
      f.args.push_back(arg);
    }
  }
  unsigned sum = 0;
  for (const auto& a : f.args) sum += a.width;
  f.scalarIntSignature = sum > 0 && sum <= 64 && f.retWidth <= 64;
  f.header = line;
  return true;
}

static std::map<std::string, PCFunction> parseFunctionsPC(const std::string& text) {
  std::map<std::string, PCFunction> out;
  size_t pos = 0;
  while (pos < text.size()) {
    const size_t lineEnd = text.find('\n', pos);
    const size_t next = lineEnd == std::string::npos ? text.size() : lineEnd + 1;
    const std::string line = text.substr(pos, (lineEnd == std::string::npos ? text.size() : lineEnd) - pos);
    if (line.rfind("define ", 0) == 0) {
      PCFunction f;
      if (parseFunctionHeaderPC(line, f)) {
        f.begin = pos;
        const size_t brace = line.find('{', line.find(')', line.find('@')));
        f.bodyBegin = pos + brace + 1;
        size_t scan = next;
        bool foundClose = false;
        while (scan < text.size()) {
          const size_t e = text.find('\n', scan);
          const size_t n = e == std::string::npos ? text.size() : e;
          const std::string t = trimPC(text.substr(scan, n - scan));
          if (t == "}") {
            f.bodyEnd = scan;
            f.end = (e == std::string::npos) ? text.size() : e + 1;
            foundClose = true;
            break;
          }
          scan = e == std::string::npos ? text.size() : e + 1;
        }
        if (foundClose) {
          f.body = text.substr(f.bodyBegin, f.bodyEnd - f.bodyBegin);
          std::istringstream body(f.body);
          std::string bline;
          while (std::getline(body, bline)) {
            const std::string t = trimPC(bline);
            if (t.empty() || t[0] == ';' || isLabelLinePC(t)) continue;
            ++f.instructions;
          }
          out[f.name] = f;
          pos = f.end;
          continue;
        }
      }
    }
    pos = next;
  }
  return out;
}

static std::string instructionOpcodePC(const std::string& line) {
  std::string t = trimPC(line);
  if (t.empty() || t[0] == ';' || isLabelLinePC(t)) return {};
  const size_t eq = t.find('=');
  if (eq != std::string::npos) t = trimPC(t.substr(eq + 1));
  std::istringstream in(t);
  std::string op;
  in >> op;
  if (op == "tail" || op == "musttail" || op == "notail") in >> op;
  return op;
}

static bool extractCalleePC(const std::string& line, std::string& callee) {
  static const std::regex target(R"(@([-A-Za-z$._][A-Za-z$._0-9-]*)\s*\()");
  std::smatch m;
  if (!std::regex_search(line, m, target)) return false;
  callee = m[1].str();
  return true;
}

static bool basicBodySafetyPC(const PCFunction& f, std::vector<std::string>& callees) {
  if (!f.scalarIntSignature || f.retWidth == 0 || f.retType == "void") return false;
  std::istringstream body(f.body);
  std::string line;
  static const std::set<std::string> allowed = {
    "ret", "br", "phi", "add", "sub", "mul", "and", "or", "xor",
    "icmp", "select", "trunc", "zext", "sext", "shl", "lshr", "ashr",
    "freeze", "call"
  };
  while (std::getline(body, line)) {
    const std::string t = trimPC(line);
    if (t.empty() || t[0] == ';' || isLabelLinePC(t)) continue;
    const std::string op = instructionOpcodePC(t);
    if (allowed.find(op) == allowed.end()) { if (g_opts.keepIr) fprintf(stderr, "precompute: unsafe opcode in %s: %s\n", f.name.c_str(), t.c_str()); return false; }
    // Do not execute operations that can trap the evaluator (e.g. division by
    // zero). Optimisation may add sound nuw/nsw flags; such paths can yield
    // poison during enumeration, but the candidate is only accepted when
    // Alive2 proves it against the original function semantics.
    if (op == "sdiv" || op == "udiv" || op == "srem" || op == "urem") return false;
    if (op == "call") {
      std::string callee;
      if (!extractCalleePC(t, callee)) { if (g_opts.keepIr) fprintf(stderr, "precompute: unsafe indirect call in %s: %s\n", f.name.c_str(), t.c_str()); return false; } // no indirect/external calls
      // Reject constant expressions / globals hidden in call operands. The
      // only accepted @symbol in a call line is the direct callee itself.
      const size_t targetAt = t.find("@" + callee);
      if (targetAt == std::string::npos || t.find('@', targetAt + callee.size() + 1) != std::string::npos) { if (g_opts.keepIr) fprintf(stderr, "precompute: unsafe call operand in %s: %s\n", f.name.c_str(), t.c_str()); return false; }
      if (t.find("ptrtoint") != std::string::npos || t.find("inttoptr") != std::string::npos)
        return false;
      callees.push_back(callee);
    } else if (t.find('@') != std::string::npos) {
      return false; // no global-variable references or hidden side effects
    }
  }
  return true;
}

static bool safeFunctionDFS_PC(const std::string& name,
                               const std::map<std::string, PCFunction>& funcs,
                               std::map<std::string, int>& state,
                               std::set<std::string>& safe) {
  auto found = funcs.find(name);
  if (found == funcs.end()) return false;
  if (state[name] == 2) return safe.find(name) != safe.end();
  if (state[name] == 1 || state[name] == 3) return false; // cycle or known-unsafe function
  state[name] = 1;
  std::vector<std::string> callees;
  if (!basicBodySafetyPC(found->second, callees)) {
    state[name] = 3;
    return false;
  }
  for (const auto& callee : callees) {
    if (!safeFunctionDFS_PC(callee, funcs, state, safe)) {
      state[name] = 3;
      return false;
    }
  }
  state[name] = 2;
  safe.insert(name);
  return true;
}

static std::string sanitizeCopiedFunctionPC(const PCFunction& f) {
  std::string result;
  std::istringstream in(f.header + "\n" + f.body + "}\n");
  std::string line;
  while (std::getline(in, line)) {
    line = stripMetadataPC(line);
    if (trimPC(line).empty()) continue;
    result += line + "\n";
  }
  return result;
}

static std::string makeEvaluatorModulePC(
    const std::map<std::string, PCFunction>& funcs,
    const std::set<std::string>& safe,
    const PCFunction& target,
    uint64_t domain) {
  std::set<std::string> closure;
  std::vector<std::string> work{target.name};
  while (!work.empty()) {
    std::string name = work.back(); work.pop_back();
    if (!closure.insert(name).second) continue;
    const auto it = funcs.find(name);
    if (it == funcs.end() || safe.find(name) == safe.end()) return {};
    std::vector<std::string> callees;
    if (!basicBodySafetyPC(it->second, callees)) return {};
    for (const auto& c : callees) work.push_back(c);
  }

  std::string module = "; Penstock finite-domain evaluator module\n";
  for (const auto& name : closure) {
    auto it = funcs.find(name);
    if (it == funcs.end()) return {};
    module += sanitizeCopiedFunctionPC(it->second) + "\n";
  }
  const std::string results = "@__penstock_precompute_results = internal global [" +
      std::to_string(domain) + " x i64] zeroinitializer, align 8\n";
  module += results;
  module += "declare i64 @write(i32, ptr, i64)\n\n";
  module += "define i32 @main() {\nentry:\n  br label %loop\n\nloop:\n";
  module += "  %ps_idx = phi i64 [ 0, %entry ], [ %ps_next, %loop ]\n";

  std::vector<std::string> callArgs;
  unsigned offset = 0;
  for (size_t i = 0; i < target.args.size(); ++i) {
    const PCArg& arg = target.args[i];
    const std::string bits = "%ps_argbits_" + std::to_string(i);
    const std::string val = "%ps_arg_" + std::to_string(i);
    if (offset == 0) module += "  " + std::string("; low bits are argument ") + std::to_string(i) + "\n";
    else module += "  " + bits + " = lshr i64 %ps_idx, " + std::to_string(offset) + "\n";
    const std::string source = offset == 0 ? "%ps_idx" : bits;
    module += "  " + val + " = trunc i64 " + source + " to i" + std::to_string(arg.width) + "\n";
    std::string callArg = "i" + std::to_string(arg.width);
    if (!arg.abiAttrs.empty()) callArg += " " + arg.abiAttrs;
    callArg += " " + val;
    callArgs.push_back(callArg);
    offset += arg.width;
  }
  std::string args;
  for (size_t i = 0; i < callArgs.size(); ++i) {
    if (i) args += ", ";
    args += callArgs[i];
  }
  module += "  %ps_value = call ";
  if (!target.callConv.empty()) module += target.callConv + " ";
  if (!target.returnAbi.empty()) module += target.returnAbi + " ";
  module += target.retType + " @" + target.name + "(" + args + ")\n";
  if (target.retWidth == 64) module += "  %ps_value64 = add i64 %ps_value, 0\n";
  else module += "  %ps_value64 = zext " + target.retType + " %ps_value to i64\n";
  module += "  %ps_slot = getelementptr inbounds [" + std::to_string(domain) +
      " x i64], ptr @__penstock_precompute_results, i64 0, i64 %ps_idx\n";
  module += "  store i64 %ps_value64, ptr %ps_slot, align 8\n";
  module += "  %ps_next = add i64 %ps_idx, 1\n";
  module += "  %ps_done = icmp eq i64 %ps_next, " + std::to_string(domain) + "\n";
  module += "  br i1 %ps_done, label %exit, label %loop\n\nexit:\n";
  module += "  %ps_written = call i64 @write(i32 1, ptr @__penstock_precompute_results, i64 " +
      std::to_string(domain * sizeof(uint64_t)) + ")\n";
  module += "  ret i32 0\n}\n";
  return module;
}

static std::string signedConstantPC(uint64_t value, unsigned width) {
  if (width == 1) return (value & 1u) ? "1" : "0";
  if (width < 64) {
    const uint64_t mask = (uint64_t(1) << width) - 1;
    value &= mask;
    if (value & (uint64_t(1) << (width - 1))) value |= ~mask;
  }
  int64_t signedValue = 0;
  std::memcpy(&signedValue, &value, sizeof signedValue);
  return std::to_string(signedValue);
}

static std::string makeLookupBodyPC(const PCFunction& f,
                                    const std::string& globalName,
                                    uint64_t domain) {
  const std::string tag = "%__ps_lut_" + globalName.substr(std::string("__penstock_lut_").size());
  std::string body = "\nentry:\n";
  std::vector<std::string> parts;
  unsigned offset = 0;
  for (size_t i = 0; i < f.args.size(); ++i) {
    const std::string base = tag + "_arg" + std::to_string(i);
    body += "  " + base + " = zext i" + std::to_string(f.args[i].width) + " " + f.args[i].name + " to i64\n";
    std::string part = base;
    if (offset) {
      part = tag + "_part" + std::to_string(i);
      body += "  " + part + " = shl i64 " + base + ", " + std::to_string(offset) + "\n";
    }
    parts.push_back(part);
    offset += f.args[i].width;
  }
  std::string key = parts.front();
  for (size_t i = 1; i < parts.size(); ++i) {
    const std::string combined = tag + "_idx" + std::to_string(i);
    body += "  " + combined + " = or i64 " + key + ", " + parts[i] + "\n";
    key = combined;
  }
  body += "  " + tag + "_slot = getelementptr inbounds [" + std::to_string(domain) +
      " x " + f.retType + "], ptr @" + globalName + ", i64 0, i64 " + key + "\n";
  body += "  " + tag + "_value = load " + f.retType + ", ptr " + tag + "_slot, align 1\n";
  body += "  ret " + f.retType + " " + tag + "_value\n";
  return body;
}

static std::string installTablePC(const std::string& current,
                                  const std::string& fnName,
                                  const std::string& globalName,
                                  unsigned retWidth,
                                  const std::vector<uint64_t>& values) {
  std::map<std::string, PCFunction> funcs = parseFunctionsPC(current);
  auto it = funcs.find(fnName);
  if (it == funcs.end()) return {};
  const PCFunction& f = it->second;
  const uint64_t domain = values.size();
  std::ostringstream g;
  g << "@" << globalName << " = private unnamed_addr constant [" << domain << " x i"
    << retWidth << "] [";
  for (size_t i = 0; i < values.size(); ++i) {
    if (i) g << ", ";
    g << "i" << retWidth << " " << signedConstantPC(values[i], retWidth);
  }
  g << "], align 16\n\n";

  std::string out = current;
  // Replace only this definition's body.
  const std::string newBody = makeLookupBodyPC(f, globalName, domain);
  out.replace(f.bodyBegin, f.bodyEnd - f.bodyBegin, newBody + "\n");

  // A pure source function may carry memory(none)/readnone. The compiler-
  // generated immutable LUT load is read-only memory access, so give this
  // one function a cloned, truthful attribute group instead of mutating a
  // group shared with other functions.
  std::map<int, std::string> groups;
  std::istringstream lines(out);
  std::string line;
  int maxGroup = -1;
  while (std::getline(lines, line)) {
    std::smatch m;
    if (std::regex_search(line, m, std::regex(R"(^\s*attributes\s+#([0-9]+)\s*=\s*(\{.*\})\s*$)"))) {
      int id = atoi(m[1].str().c_str());
      groups[id] = m[2].str();
      maxGroup = std::max(maxGroup, id);
    }
  }
  std::string header = f.header;
  std::smatch ref;
  if (std::regex_search(header, ref, std::regex(R"(#([0-9]+)\b)"))) {
    const int oldId = atoi(ref[1].str().c_str());
    auto ag = groups.find(oldId);
    if (ag != groups.end()) {
      std::string attrs = ag->second;
      bool changed = false;
      if (attrs.find("memory(none)") != std::string::npos) {
        const size_t p = attrs.find("memory(none)");
        attrs.replace(p, std::string("memory(none)").size(), "memory(read)");
        changed = true;
      } else if (attrs.find("readnone") != std::string::npos) {
        const size_t p = attrs.find("readnone");
        attrs.replace(p, std::string("readnone").size(), "readonly");
        changed = true;
      }
      if (changed) {
        const int newId = maxGroup + 1;
        header = std::regex_replace(header, std::regex("#" + std::to_string(oldId) + R"(\b)"),
                                    "#" + std::to_string(newId));
        out.replace(f.begin, f.begin + f.header.size() <= out.size() ? f.header.size() : 0, header);
        const std::string newAttrLine = "attributes #" + std::to_string(newId) + " = " + attrs + "\n";
        // Attribute groups live after functions and before the metadata block.
        size_t insertAt = out.find("\n!900 =");
        if (insertAt == std::string::npos) insertAt = out.find("\n!llvm.");
        if (insertAt == std::string::npos) insertAt = out.size();
        out.insert(insertAt, "\n" + newAttrLine);
      }
    }
  }

  // Insert the immutable table before the first function definition (after
  // declarations and type definitions). Forward references would also work,
  // but this layout keeps the resulting module easy to inspect.
  const auto outFns = parseFunctionsPC(out);
  if (outFns.empty()) return {};
  size_t firstDefine = out.size();
  for (const auto& entry : outFns) firstDefine = std::min(firstDefine, entry.second.begin);
  if (firstDefine == out.size()) return {};
  out.insert(firstDefine, g.str());
  return out;
}

static std::string makeProbeCanonicalizationInputPC(
    const std::string& original,
    const std::map<std::string, PCFunction>& funcs) {
  // Keep each integer function addressable and prevent LLVM's interprocedural
  // passes from deleting or inlining it during the canonicalisation fallback.
  // The probe is never executed: volatile values only make its call arguments
  // non-constant to the optimizer, allowing mem2reg/instcombine to expose pure
  // SSA bodies even when the LLVM `opt` binary is not installed.
  std::map<int, std::string> attrGroups;
  {
    std::istringstream in(original);
    std::string line;
    while (std::getline(in, line)) {
      std::smatch m;
      if (std::regex_search(line, m,
            std::regex(R"(^\s*attributes\s+#([0-9]+)\s*=\s*(\{.*\})\s*$)")))
        attrGroups[atoi(m[1].str().c_str())] = m[2].str();
    }
  }

  std::string base = std::regex_replace(original,
      std::regex(R"(@main\s*\()"), "@__penstock_original_main(");
  std::istringstream lines(base);
  std::string decorated;
  std::string line;
  while (std::getline(lines, line)) {
    if (line.rfind("define ", 0) == 0 && line.find('{') != std::string::npos &&
        line.find("noinline") == std::string::npos) {
      bool alwaysInline = false;
      std::smatch id;
      if (std::regex_search(line, id, std::regex(R"(#([0-9]+)\b)"))) {
        auto a = attrGroups.find(atoi(id[1].str().c_str()));
        if (a != attrGroups.end()) {
          alwaysInline = a->second.find("alwaysinline") != std::string::npos;
          if (a->second.find("noinline") != std::string::npos) alwaysInline = true;
        }
      }
      if (!alwaysInline) {
        const size_t brace = line.rfind('{');
        line.insert(brace, " noinline ");
      }
    }
    decorated += line + "\n";
  }

  std::string inputName = "__penstock_pc_probe_input";
  std::string sinkName = "__penstock_pc_probe_sink";
  unsigned suffix = 0;
  while (original.find("@" + inputName) != std::string::npos ||
         original.find("@" + sinkName) != std::string::npos) {
    ++suffix;
    inputName = "__penstock_pc_probe_input_" + std::to_string(suffix);
    sinkName = "__penstock_pc_probe_sink_" + std::to_string(suffix);
  }
  decorated += "\n@" + inputName + " = internal global i64 0, align 8\n";
  decorated += "@" + sinkName + " = internal global i64 0, align 8\n";
  decorated += "define i32 @main() {\nentry:\n";
  size_t fnIndex = 0;
  for (const auto& kv : funcs) {
    const PCFunction& f = kv.second;
    if (f.name == "main" || f.args.empty() || f.retWidth == 0) continue;
    bool supported = true;
    for (const auto& a : f.args) if (a.width == 0 || a.width > 64) supported = false;
    if (!supported) continue;
    std::vector<std::string> args;
    for (size_t i = 0; i < f.args.size(); ++i) {
      const std::string raw = "%pc_probe_raw_" + std::to_string(fnIndex) + "_" + std::to_string(i);
      const std::string val = "%pc_probe_arg_" + std::to_string(fnIndex) + "_" + std::to_string(i);
      decorated += "  " + raw + " = load volatile i64, ptr @" + inputName + "\n";
      std::string argValue = raw;
      if (f.args[i].width != 64) {
        decorated += "  " + val + " = trunc i64 " + raw + " to i" + std::to_string(f.args[i].width) + "\n";
        argValue = val;
      }
      std::string arg = "i" + std::to_string(f.args[i].width);
      if (!f.args[i].abiAttrs.empty()) arg += " " + f.args[i].abiAttrs;
      arg += " " + argValue;
      args.push_back(arg);
    }
    std::string argText;
    for (size_t i = 0; i < args.size(); ++i) {
      if (i) argText += ", ";
      argText += args[i];
    }
    const std::string ret = "%pc_probe_ret_" + std::to_string(fnIndex);
    decorated += "  " + ret + " = call ";
    if (!f.callConv.empty()) decorated += f.callConv + " ";
    if (!f.returnAbi.empty()) decorated += f.returnAbi + " ";
    decorated += f.retType + " @" + f.name + "(" + argText + ")\n";
    std::string wide = ret;
    if (f.retWidth != 64) {
      wide = "%pc_probe_wide_" + std::to_string(fnIndex);
      decorated += "  " + wide + " = zext " + f.retType + " " + ret + " to i64\n";
    }
    decorated += "  store volatile i64 " + wide + ", ptr @" + sinkName + "\n";
    ++fnIndex;
  }
  decorated += "  ret i32 0\n}\n";
  return decorated;
}

static bool verifyAlivePC(const std::string& alive, const std::string& timeout,
                          const std::string& before, const std::string& after,
                          const std::string& log, int seconds) {
  const std::string cmd = shellQuotePC(timeout) + " " + std::to_string(seconds) + "s " +
      shellQuotePC(alive) + " " + shellQuotePC(before) + " " + shellQuotePC(after) +
      " > " + shellQuotePC(log) + " 2>&1";
  const bool rc = runOkPC(cmd);
  const std::string text = readTextPC(log);
  return rc && text.find("Transformation seems to be correct!") != std::string::npos &&
         text.find("Transformation doesn't verify!") == std::string::npos &&
         text.find("ERROR:") == std::string::npos;
}

} // namespace

bool runPrecomputeRewriter(const std::string& clang,
                           const std::string& llvmRoot,
                           const std::string& finalLl,
                           int optLevel,
                           int verifyBudgetSec) {
  const bool wide = optLevel >= 4;
  if (optLevel < 3) return false;
  const std::string original = readTextPC(finalLl);
  if (original.empty()) return false;
  const size_t moduleLimit = wide ? 4u * 1024u * 1024u : 512u * 1024u;
  if (original.size() > moduleLimit) {
    fprintf(stderr, "precompute: skipped — module exceeds %s input budget (%zu > %zu bytes)\n",
            wide ? "-O4" : "-O3", original.size(), moduleLimit);
    return false;
  }

  const std::string opt = resolveCommandPC("PENSTOCK_OPT", "opt", llvmRoot);
  const std::string alive = resolveCommandPC("PENSTOCK_ALIVE_TV", "alive-tv");
  const std::string timeout = resolveCommandPC("PENSTOCK_TIMEOUT", "timeout");
  if (alive.empty() || timeout.empty()) {
    fprintf(stderr, "precompute: skipped — exhaustive mapping requires Alive2's alive-tv and timeout; IR unchanged\n");
    return false;
  }

  if (verifyBudgetSec <= 0) verifyBudgetSec = wide ? 10 : 2;
  const std::string base = finalLl + ".precompute." + std::to_string(getpid());
  const std::string optIn = base + ".input.ll";
  const std::string canonical = base + ".canonical.ll";
  const std::string optLog = base + ".opt.log";
  const std::string compileLog = base + ".compile.log";
  if (!writeTextPC(optIn, original)) {
    fprintf(stderr, "precompute: unable to make canonicalisation input; skipped\n");
    return false;
  }

  const int optSeconds = wide ? 15 : 8;
  bool optOk = false;
  if (!opt.empty()) {
    std::string optCmd = shellQuotePC(timeout) + " " + std::to_string(optSeconds) + "s " +
        shellQuotePC(opt) + " -S -passes=mem2reg,instcombine,simplifycfg " +
        shellQuotePC(optIn) + " -o " + shellQuotePC(canonical) + " > " +
        shellQuotePC(optLog) + " 2>&1";
    optOk = runOkPC(optCmd);
    if (!optOk) {
      unlink(canonical.c_str());
      optCmd = shellQuotePC(timeout) + " " + std::to_string(optSeconds) + "s " +
          shellQuotePC(opt) + " -S -mem2reg -instcombine -simplifycfg " +
          shellQuotePC(optIn) + " -o " + shellQuotePC(canonical) + " > " +
          shellQuotePC(optLog) + " 2>&1";
      optOk = runOkPC(optCmd);
    }
  }
  std::string canonicalText = optOk ? readTextPC(canonical) : std::string();
  std::string probeInput = base + ".probe-input.ll";
  if (canonicalText.empty()) {
    // Clang ships the same LLVM optimiser even in installations that omit the
    // standalone `opt` executable. Use a non-executed volatile probe module to
    // keep integer functions live while promoting stack locals to SSA.
    auto rawFunctions = parseFunctionsPC(original);
    const std::string probe = makeProbeCanonicalizationInputPC(original, rawFunctions);
    const std::string probeSeconds = std::to_string(wide ? 18 : 10);
    if (writeTextPC(probeInput, probe)) {
      const std::string canonCmd = shellQuotePC(timeout) + " " + probeSeconds + "s " +
          shellQuotePC(clang) + " -O1 -fno-inline -fno-unroll-loops -S -emit-llvm "
          "-Wno-override-module " + shellQuotePC(probeInput) + " -o " +
          shellQuotePC(canonical) + " > " + shellQuotePC(optLog) + " 2>&1";
      if (runOkPC(canonCmd)) canonicalText = readTextPC(canonical);
    }
    if (canonicalText.empty()) {
      fprintf(stderr, "precompute: could not canonicalise IR with opt or Clang; no changes made\n");
      if (!g_opts.keepIr) { unlink(optIn.c_str()); unlink(canonical.c_str()); unlink(optLog.c_str()); unlink(probeInput.c_str()); }
      return false;
    }
    fprintf(stderr, "precompute: used Clang's LLVM pipeline because standalone opt was unavailable/failed\n");
  }

  auto funcs = parseFunctionsPC(canonicalText);
  if (funcs.empty()) {
    fprintf(stderr, "precompute: no usable LLVM function definitions found\n");
    if (!g_opts.keepIr) { unlink(optIn.c_str()); unlink(canonical.c_str()); unlink(optLog.c_str()); unlink(probeInput.c_str()); }
    return false;
  }
  std::map<std::string, int> state;
  std::set<std::string> safe;
  for (const auto& kv : funcs) {
    if (state[kv.first] == 0) safeFunctionDFS_PC(kv.first, funcs, state, safe);
  }

  if (g_opts.keepIr) {
    for (const auto& name : safe) {
      auto it = funcs.find(name);
      if (it != funcs.end()) fprintf(stderr, "precompute: safe candidate graph node %s (%zu instructions)\n", name.c_str(), it->second.instructions);
    }
  }

  auto originalFuncs = parseFunctionsPC(original);
  struct Candidate { std::string name; uint64_t domain; unsigned bits; size_t cost; };
  std::vector<Candidate> candidates;
  const unsigned maxBits = wide ? 16u : 8u;
  const uint64_t maxEvaluations = wide ? 65536u : 1024u;
  const size_t maxCandidates = wide ? 8u : 4u;
  const size_t minBodyInstructions = wide ? 8u : 9u;
  for (const auto& name : safe) {
    const auto ci = funcs.find(name);
    const auto oi = originalFuncs.find(name);
    if (ci == funcs.end() || oi == originalFuncs.end()) continue;
    const PCFunction& f = ci->second;
    const PCFunction& origF = oi->second;
    if (name == "main" || !f.scalarIntSignature || f.args.empty() ||
        f.instructions < minBodyInstructions || origF.args.size() != f.args.size() ||
        origF.retType != f.retType) continue;
    unsigned bits = 0;
    bool argsOk = true;
    for (size_t i = 0; i < f.args.size(); ++i) {
      if (f.args[i].width != origF.args[i].width ||
          f.args[i].width > maxBits || origF.args[i].name.empty()) argsOk = false;
      bits += f.args[i].width;
    }
    if (!argsOk || bits == 0 || bits > maxBits) continue;
    const uint64_t domain = uint64_t(1) << bits;
    const uint64_t bytes = domain * ((f.retWidth + 7u) / 8u);
    const uint64_t byteBudget = wide ? 512u * 1024u : 16u * 1024u;
    if (bytes > byteBudget) continue;
    candidates.push_back({name, domain, bits, f.instructions});
  }
  if (g_opts.keepIr) {
    for (const auto& c : candidates) fprintf(stderr, "precompute: bounded map candidate %s (%llu entries)\n", c.name.c_str(), static_cast<unsigned long long>(c.domain));
  }
  std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
    if (a.cost != b.cost) return a.cost > b.cost;
    return a.name < b.name;
  });

  const auto stageStart = std::chrono::steady_clock::now();
  const auto stageLimit = std::chrono::seconds(wide ? 45 : 12);
  uint64_t evaluations = 0;
  unsigned attempted = 0;
  bool changedAny = false;
  std::string accepted = original;
  const std::string runnerLl = base + ".runner.ll";
  const std::string runnerBin = base + ".runner.bin";
  const std::string runnerOut = base + ".runner.out";
  const std::string aliveBefore = base + ".alive.before.ll";
  const std::string aliveAfter = base + ".alive.after.ll";
  const std::string aliveLog = base + ".alive.log";

  for (const Candidate& c : candidates) {
    if (attempted >= maxCandidates || evaluations + c.domain > maxEvaluations) continue;
    if (std::chrono::steady_clock::now() - stageStart > stageLimit) break;
    ++attempted;
    const PCFunction& f = funcs.at(c.name);
    std::string evaluator = makeEvaluatorModulePC(funcs, safe, f, c.domain);
    if (evaluator.empty() || !writeTextPC(runnerLl, evaluator)) continue;
    unlink(runnerBin.c_str()); unlink(runnerOut.c_str());

    const int compileSeconds = wide ? 12 : 5;
    const std::string compileCmd = shellQuotePC(timeout) + " " +
        std::to_string(compileSeconds) + "s " + shellQuotePC(clang) +
        " -O0 -Wno-override-module " + shellQuotePC(runnerLl) + " -o " +
        shellQuotePC(runnerBin) + " > " + shellQuotePC(compileLog) + " 2>&1";
    if (!runOkPC(compileCmd)) {
      fprintf(stderr, "precompute: skipped '%s' — evaluator compilation failed\n", c.name.c_str());
      continue;
    }

    const int evalSeconds = wide ? 10 : 2;
    const std::string evalCmd = shellQuotePC(timeout) + " " +
        std::to_string(evalSeconds) + "s " + shellQuotePC(runnerBin) +
        " > " + shellQuotePC(runnerOut) + " 2>/dev/null";
    if (!runOkPC(evalCmd)) {
      fprintf(stderr, "precompute: skipped '%s' — mapping evaluation timed out or failed\n", c.name.c_str());
      continue;
    }
    std::vector<uint8_t> raw;
    if (!readBinaryPC(runnerOut, raw) || raw.size() != c.domain * sizeof(uint64_t)) {
      fprintf(stderr, "precompute: skipped '%s' — incomplete mapping (wanted %llu outputs)\n",
              c.name.c_str(), static_cast<unsigned long long>(c.domain));
      continue;
    }
    std::vector<uint64_t> values(static_cast<size_t>(c.domain));
    std::memcpy(values.data(), raw.data(), raw.size());
    evaluations += c.domain;

    // Construct a constant-table candidate. Its function body is reduced to
    // a packed-key calculation and an immutable lookup. Alive2 must prove this
    // candidate against the currently accepted module before it is installed.
    std::string suffix;
    for (char ch : c.name) suffix += isIdentPC(ch) ? ch : '_';
    const uint32_t hash = static_cast<uint32_t>(std::hash<std::string>{}(c.name));
    std::string tag = "__penstock_lut_" + suffix + "_" + std::to_string(hash);
    unsigned collision = 0;
    while (accepted.find("@" + tag) != std::string::npos ||
           accepted.find("%" + tag) != std::string::npos)
      tag = "__penstock_lut_" + suffix + "_" + std::to_string(hash) + "_" + std::to_string(++collision);
    const std::string trial = installTablePC(accepted, c.name, tag, f.retWidth, values);
    if (trial.empty()) continue;
    if (!writeTextPC(aliveBefore, accepted) || !writeTextPC(aliveAfter, trial)) continue;
    if (!verifyAlivePC(alive, timeout, aliveBefore, aliveAfter, aliveLog, verifyBudgetSec)) {
      fprintf(stderr, "precompute: rejected '%s' — Alive2 did not prove the mapping rewrite\n",
              c.name.c_str());
      if (g_opts.keepIr) fprintf(stderr, "precompute: verifier log kept at %s\n", aliveLog.c_str());
      continue;
    }
    // §7.4 profitability gate — deliberately SEPARATE from verification:
    // a verified mapping is only installed when the table is expected to
    // pay for itself.  The original body must carry real work (>= 8
    // instructions — replacing 3 ops with a load is noise), and the table
    // must stay comfortably inside L1 (<= 4 KiB; <= 64 KiB in the -O4
    // wide scope, i.e. L2 territory) next to the program's other hot
    // data.  Without profile data the call frequency is unknown, so this
    // is a conservative static gate; the outcome is reported as
    // "verified but NOT profitable", never conflated with "not verified".
    {
      const uint64_t tableBytes = c.domain * ((f.retWidth + 7u) / 8u);
      const uint64_t profitByteCap = wide ? 64u * 1024u : 4096u;
      if (f.instructions < 8 || tableBytes > profitByteCap) {
        fprintf(stderr,
            "precompute: '%s' verified but NOT profitable (table %llu bytes "
            "vs %zu-instruction body; caps: >= 8 inst, <= %llu bytes) — "
            "keeping the original function\n",
            c.name.c_str(), static_cast<unsigned long long>(tableBytes),
            f.instructions, static_cast<unsigned long long>(profitByteCap));
        continue;
      }
    }
    accepted = trial;
    changedAny = true;
    fprintf(stderr,
        "precompute: Alive2-verified complete mapping for '%s' (%llu input tuples, %zu -> lookup body; scope=%s)\n",
        c.name.c_str(), static_cast<unsigned long long>(c.domain), f.instructions,
        wide ? "-O4" : "-O3");
  }

  if (changedAny) {
    const std::string acceptPath = base + ".accepted.ll";
    if (!writeTextPC(acceptPath, accepted) || rename(acceptPath.c_str(), finalLl.c_str()) != 0) {
      unlink(acceptPath.c_str());
      fprintf(stderr, "precompute: could not atomically install verified mapping IR; original retained\n");
      changedAny = false;
    }
  }
  if (!g_opts.keepIr) {
    unlink(optIn.c_str()); unlink(canonical.c_str()); unlink(optLog.c_str()); unlink(probeInput.c_str());
    unlink(compileLog.c_str()); unlink(runnerLl.c_str()); unlink(runnerBin.c_str());
    unlink(runnerOut.c_str()); unlink(aliveBefore.c_str()); unlink(aliveAfter.c_str()); unlink(aliveLog.c_str());
  } else {
    fprintf(stderr, "precompute: intermediate evaluator/verification files kept with prefix %s\n", base.c_str());
  }
  if (!changedAny && candidates.empty()) {
    fprintf(stderr, "precompute: no safe functions fit the finite-domain scope budget\n");
  }
  return changedAny;
}

} // namespace freshet
