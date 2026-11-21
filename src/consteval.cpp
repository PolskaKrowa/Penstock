// fsht-02:03 (registry 06.03.01) — compile-time evaluation.
//
// Two evaluation entry points share one u64 (usize) domain:
//   * evalConstEnc — const-expression ENCODINGS produced by the parser in
//     type positions: array lengths (`[T; N * 2]`) and const type
//     arguments (`Vector<f64, square(3)>`).
//   * evalConstExpr / evalConstFnCall — the source-level interpreter for
//     `const fn` bodies and const_assert arguments (statements: let/return/
//     if/while/for/blocks; expressions: literals, params, arithmetic,
//     comparisons, casts, calls to other const fns).
//
// Every operator evaluates in exact u64 arithmetic; division by zero,
// unknown names, non-const constructs and budget exhaustion fail the
// evaluation (callers surface F0219 / F0220).  Budgets are generous but
// finite: 4096 interpreter steps and 32 recursion levels per call.
#include "common.h"
#include <map>
#include <string>
#include <vector>

namespace freshet {

static const int kConstStepBudget = 1 << 14;
static const int kConstDepthBudget = 32;

// ---------------------------------------------------------------- encoding
// Encoding grammar (parser.cpp tryParseConstExprEnc):
//   "5"  literal |  "N"  name |  "(A + B)"  binary |  "sq(3,4)"  call
struct ConstEvalFail {};

static uint64_t evalEncRec(const std::string& e, size_t& i,
                            const std::map<std::string, uint64_t>* cenv,
                            int depth);

static bool isEncIdentChar(char c) {
  return isalnum((unsigned char)c) || c == '_';
}

// parse one operand starting at i; advance i past it
static uint64_t evalOperand(const std::string& e, size_t& i,
                             const std::map<std::string, uint64_t>* cenv,
                             int depth) {
  if (i >= e.size()) throw ConstEvalFail{};
  char c = e[i];
  // literal
  if (isdigit((unsigned char)c)) {
    size_t j = i;
    uint64_t v = 0;
    while (j < e.size() && isdigit((unsigned char)e[j])) {
      if (v > (UINT64_MAX - (e[j]-'0')) / 10) throw ConstEvalFail{};
      v = v * 10 + (e[j++] - '0');
    }
    i = j;
    return v;
  }
  // identifier or call
  if (isEncIdentChar(c)) {
    size_t j = i;
    while (j < e.size() && isEncIdentChar(e[j])) ++j;
    std::string name = e.substr(i, j - i);
    i = j;
    if (i < e.size() && e[i] == '(') {
      // const-fn call: name(arg,arg)
      ++i;
      std::vector<uint64_t> args;
      if (i < e.size() && e[i] != ')') {
        while (true) {
          size_t start = i;
          args.push_back(evalEncRec(e, i, cenv, depth));
          (void)start;
          if (i < e.size() && e[i] == ',') { ++i; continue; }
          break;
        }
      }
      if (i >= e.size() || e[i] != ')') throw ConstEvalFail{};
      ++i;
      FnDecl* f = lookupFn(name);
      if (!f || !f->isConst) throw ConstEvalFail{};
      uint64_t out = 0;
      if (!evalConstFnCall(f, args, &out, depth + 1)) throw ConstEvalFail{};
      return out;
    }
    // name: const parameter from the environment
    if (!cenv) throw ConstEvalFail{};
    auto it = cenv->find(name);
    if (it == cenv->end()) throw ConstEvalFail{};
    return it->second;
  }
  // parenthesised binary: "(A op B)"
  if (c == '(') {
    ++i;
    return evalEncRec(e, i, cenv, depth);
  }
  throw ConstEvalFail{};
}

static uint64_t evalEncRec(const std::string& e, size_t& i,
                            const std::map<std::string, uint64_t>* cenv,
                            int depth) {
  if (depth > kConstDepthBudget) throw ConstEvalFail{};
  uint64_t left = evalOperand(e, i, cenv, depth);
  // binary tail inside a parenthesised group: " <op> "
  if (i + 1 < e.size() && e[i] == ' ') {
    size_t j = i + 1;
    char op = 0;
    if (j < e.size() && (e[j] == '+' || e[j] == '-' || e[j] == '*' ||
                         e[j] == '/' || e[j] == '%'))
      op = e[j++];
    if (op && j < e.size() && e[j] == ' ') {
      i = j + 1;
      uint64_t right = evalOperand(e, i, cenv, depth);
      if (i < e.size() && e[i] == ')') ++i;   // close the group
      switch (op) {
        case '+': return left + right;
        case '-': return left - right;
        case '*': {
          if (right != 0 && left > UINT64_MAX / right) throw ConstEvalFail{};
          return left * right;
        }
        case '/': if (right == 0) throw ConstEvalFail{}; return left / right;
        case '%': if (right == 0) throw ConstEvalFail{}; return left % right;
      }
    }
  }
  return left;
}

bool evalConstEnc(const std::string& enc, const std::map<std::string, uint64_t>* cenv,
                  SrcLoc loc, uint64_t* out) {
  if (enc.empty()) return false;
  size_t i = 0;
  try {
    uint64_t v = evalEncRec(enc, i, cenv, 0);
    if (i != enc.size()) return false;   // trailing garbage
    if (v == 0) {
      // zero-length arrays are invalid; surface as const-eval failure with
      // a pointed message instead of a generic binder error
      diag(Diag::Error, Code::ConstEval,
           "const-evaluated length is 0 ('" + enc + "') — array lengths "
           "must be at least 1", loc);
      return false;
    }
    *out = v;
    return true;
  } catch (ConstEvalFail&) {
    diag(Diag::Error, Code::ConstEval,
         "const expression '" + enc + "' cannot be evaluated at compile "
         "time — it must be built from integer literals, const parameters "
         "and const fn calls over usize", loc);
    return false;
  }
}

// ---------------------------------------------------------------- interpreter
namespace {

struct Interp {
  std::map<std::string, uint64_t> vars;
  int steps = 0;
  int depth = 0;

  void tick() { if (++steps > kConstStepBudget) throw ConstEvalFail{}; }

  // expression evaluation — same domain as the encoding evaluator
  uint64_t ex(Expr* e) {
    if (!e) throw ConstEvalFail{};
    tick();
    switch (e->k) {
      case Expr::INT:
        return e->ival;
      case Expr::BOOL:
        return e->bval ? 1 : 0;
      case Expr::FLT:
        // integers only in the const-generic domain; a float that happens
        // to be integral is accepted (e.g. `2.0`)
        if (e->fval == (double)(uint64_t)e->fval && e->fval >= 0)
          return (uint64_t)e->fval;
        throw ConstEvalFail{};
      case Expr::IDENT: {
        auto it = vars.find(e->name);
        if (it == vars.end()) throw ConstEvalFail{};
        return it->second;
      }
      case Expr::BIN: {
        const std::string& op = e->op;
        if (op == "&&") return ex(e->lhs) != 0 && ex(e->rhs) != 0 ? 1 : 0;
        if (op == "||") return ex(e->lhs) != 0 || ex(e->rhs) != 0 ? 1 : 0;
        uint64_t l = ex(e->lhs);
        uint64_t r = ex(e->rhs);
        if (op == "+") return l + r;
        if (op == "-") return l - r;
        if (op == "*") {
          if (r != 0 && l > UINT64_MAX / r) throw ConstEvalFail{};
          return l * r;
        }
        if (op == "/") { if (r == 0) throw ConstEvalFail{}; return l / r; }
        if (op == "%") { if (r == 0) throw ConstEvalFail{}; return l % r; }
        if (op == "<<") { if (r >= 64) throw ConstEvalFail{}; return l << r; }
        if (op == ">>") { if (r >= 64) throw ConstEvalFail{}; return l >> r; }
        if (op == "&") return l & r;
        if (op == "|") return l | r;
        if (op == "^") return l ^ r;
        if (op == "==") return l == r ? 1 : 0;
        if (op == "!=") return l != r ? 1 : 0;
        if (op == "<")  return l <  r ? 1 : 0;
        if (op == "<=") return l <= r ? 1 : 0;
        if (op == ">")  return l >  r ? 1 : 0;
        if (op == ">=") return l >= r ? 1 : 0;
        throw ConstEvalFail{};
      }
      case Expr::UN: {
        uint64_t v = ex(e->rhs);
        if (e->op == "!") return v == 0 ? 1 : 0;
        if (e->op == "-") return (uint64_t)(-(int64_t)v);
        throw ConstEvalFail{};
      }
      case Expr::CAST: {
        // numeric casts evaluate at the target width (truncation model)
        uint64_t v = ex(e->base);
        if (e->castTy) {
          switch (e->castTy->k) {
            case Type::U8:  return v & 0xFFu;
            case Type::U16: return v & 0xFFFFu;
            case Type::U32: return v & 0xFFFFFFFFu;
            case Type::U64: case Type::USIZE: return v;
            case Type::I8:  return (uint64_t)(int8_t)v;
            case Type::I16: return (uint64_t)(int16_t)v;
            case Type::I32: return (uint64_t)(int32_t)v;
            case Type::I64: return v;
            case Type::BOOL: return v != 0 ? 1 : 0;
            default: throw ConstEvalFail{};
          }
        }
        return v;
      }
      case Expr::CALL: {
        // sizeof/alignof are resolved by sema into `ival` (typeExpr sets
        // it while typing the argument) — the interpreter just reads it
        if ((e->name == "sizeof" || e->name == "alignof") && e->constant)
          return e->ival;
        // calls: other const fns
        FnDecl* f = lookupFn(e->name);
        if (!f || !f->isConst) throw ConstEvalFail{};
        std::vector<uint64_t> args;
        for (auto* a : e->args) args.push_back(ex(a));
        uint64_t out = 0;
        if (!evalConstFnCall(f, args, &out, depth + 1)) throw ConstEvalFail{};
        return out;
      }
      default:
        // STR / NILQ / INDEX / FIELD / SLICE / TRYQ / SLIT / ARRLIT:
        // not const-evaluable in the first cut
        throw ConstEvalFail{};
    }
  }

  // statement execution; `retVal` receives the value of the first RETURN
  // (returns true when a return fired)
  bool st(Stmt* s, uint64_t* retVal) {
    if (!s) return false;
    tick();
    switch (s->k) {
      case Stmt::LET: {
        uint64_t v = ex(s->init);
        vars[s->var] = v;
        return false;
      }
      case Stmt::ASSIGN: {
        // `x = expr` / `x op= expr` — the lhs is an IDENT binding
        // (const fn bodies have no pointer/field mutation in the first cut)
        if (!s->lhs || s->lhs->k != Expr::IDENT) throw ConstEvalFail{};
        uint64_t v = ex(s->rhs);
        if (s->op2 != "=" && s->op2 != "+=" && s->op2 != "-=" &&
            s->op2 != "*=" && s->op2 != "/=" && s->op2 != "%=")
          throw ConstEvalFail{};
        auto it = vars.find(s->lhs->name);
        if (s->op2 != "=") {
          if (it == vars.end()) throw ConstEvalFail{};
          uint64_t l = it->second;
          if (s->op2 == "+=") v = l + v;
          else if (s->op2 == "-=") v = l - v;
          else if (s->op2 == "*=") v = l * v;
          else if (s->op2 == "/=") { if (v == 0) throw ConstEvalFail{}; v = l / v; }
          else if (s->op2 == "%=") { if (v == 0) throw ConstEvalFail{}; v = l % v; }
        }
        vars[s->lhs->name] = v;
        return false;
      }
      case Stmt::RETURN:
        *retVal = ex(s->retVal);
        return true;
      case Stmt::EXPR:
        (void)ex(s->expr);   // side-effect-free: value discarded
        return false;
      case Stmt::IF:
        if (ex(s->cond) != 0) return runBlock(s->thenB, retVal);
        if (s->elseB) return runBlock(s->elseB, retVal);
        return false;
      case Stmt::BLOCK:
        return runBlock(s, retVal);
      case Stmt::WHILE: {
        // note: loop bodies live in `body` (thenB/elseB belong to IF)
        while (ex(s->cond) != 0) {
          if (runBlock(s->body, retVal)) return true;
          tick();
        }
        return false;
      }
      case Stmt::FOR: {
        if (!s->forRange) throw ConstEvalFail{};
        uint64_t lo = ex(s->rangeLo), hi = ex(s->rangeHi);
        if (lo > hi) throw ConstEvalFail{};
        for (uint64_t k = lo; k < hi; ++k) {
          vars[s->ivar] = k;
          if (runBlock(s->body, retVal)) return true;
          tick();
        }
        return false;
      }
      case Stmt::BREAK:
      case Stmt::CONTINUE:
      case Stmt::MATCH:
      case Stmt::PARFOR:
      default:
        throw ConstEvalFail{};
    }
  }

  bool runBlock(Stmt* b, uint64_t* retVal) {
    if (!b) return false;
    for (auto* inner : b->stmts)
      if (st(inner, retVal)) return true;
    return false;
  }
};

} // namespace

bool evalConstExpr(Expr* e, const std::map<std::string, uint64_t>& env,
                   uint64_t* out, SrcLoc loc) {
  (void)loc;
  Interp in;
  in.vars = env;
  try {
    *out = in.ex(e);
    return true;
  } catch (ConstEvalFail&) {
    return false;
  }
}

bool evalConstFnCall(FnDecl* f, const std::vector<uint64_t>& args, uint64_t* out,
                     int depth) {
  if (!f || !f->isConst) return false;
  if (depth > kConstDepthBudget) return false;
  Interp in;
  in.depth = depth;
  if (f->params.size() != args.size()) return false;
  for (size_t i = 0; i < args.size(); ++i)
    in.vars[f->params[i].name] = args[i];
  uint64_t ret = 0;
  try {
    if (!in.runBlock(f->body, &ret)) return false;   // no return fired
  } catch (ConstEvalFail&) {
    return false;
  }
  *out = ret;
  return true;
}

} // namespace freshet
