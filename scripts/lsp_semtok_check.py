#!/usr/bin/env python3
"""LSP semantic-tokens smoke test: drives the language server over stdio
JSON-RPC and checks textDocument/semanticTokens/full — the
reference-accurate colouring layer for types, function arguments,
locals/fields/enum members and module names.

Checks:
  * the capability advertises the legend (10 standard token types,
    4 modifiers) and `full` support;
  * decoded tokens land exactly on the expected identifiers with the
    expected type/modifiers (declarations, parameters, locals, calls,
    struct fields, enum variants, module name, qualified enum paths,
    builtin calls with the defaultLibrary modifier);
  * every token text is a plain identifier, positions are sorted and
    non-overlapping.
Self-contained: the analysed document is embedded here."""
import subprocess, json, sys

proc = subprocess.Popen([sys.argv[1]], stdin=subprocess.PIPE,
                        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)

def send(msg):
    body = json.dumps(msg)
    proc.stdin.write(f"Content-Length: {len(body)}\r\n\r\n{body}".encode())
    proc.stdin.flush()

def recv():
    hdr = b""
    while not hdr.endswith(b"\r\n\r\n"):
        c = proc.stdout.read(1)
        if not c:
            return None
        hdr += c
    return json.loads(proc.stdout.read(int(hdr.decode().split(":")[1].strip())))

TEXT = (
    "module demo;\n"
    "\n"
    "struct Vec2 { x: f64, y: f64 }\n"
    "\n"
    "enum Color { Red, Green, Blue }\n"
    "\n"
    "fn norm(v: Vec2) -> f64 {\n"
    "    let d: f64 = v.x + v.y;\n"
    "    return d;\n"
    "}\n"
    "\n"
    "fn describe(c: Color) -> i64 {\n"
    "    match (c) {\n"
    "        Red => { return 10; }\n"
    "        _ => { return 0; }\n"
    "    }\n"
    "}\n"
    "\n"
    "fn main() {\n"
    "    let v: Vec2 = Vec2 { x: 3.0, y: 4.0 };\n"
    "    let n: f64 = norm(v);\n"
    "    let c: Color = Color.Green;\n"
    "    print_f64(n, 2); println();\n"
    "    print_i64(describe(c)); println();\n"
    "}\n"
)
LINES = TEXT.split("\n")

uri = "file:///tmp/lsp_semtok_check.fsht"

send({"jsonrpc": "2.0", "id": 1, "method": "initialize",
      "params": {"capabilities": {}}})
init = recv()
assert init and init.get("id") == 1, "missing initialize response"
caps = (init.get("result") or {}).get("capabilities") or {}
st = caps.get("semanticTokensProvider")
assert isinstance(st, dict), "server must advertise semanticTokensProvider"
legend = st.get("legend") or {}
assert legend.get("tokenTypes") == [
    "namespace", "type", "struct", "enum", "interface", "function",
    "variable", "parameter", "property", "enumMember",
], f"unexpected tokenTypes legend: {legend.get('tokenTypes')!r}"
assert legend.get("tokenModifiers") == [
    "declaration", "readonly", "defaultLibrary", "static",
], f"unexpected tokenModifiers legend: {legend.get('tokenModifiers')!r}"
assert st.get("full") is True, "full-document semantic tokens must be advertised"

send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": uri, "languageId": "freshet",
                     "version": 1, "text": TEXT}}})
recv()

send({"jsonrpc": "2.0", "id": 2, "method": "textDocument/semanticTokens/full",
      "params": {"textDocument": {"uri": uri}}})
r = recv()
data = (r.get("result") or {}).get("data")
assert isinstance(data, list) and len(data) % 5 == 0, \
    "semanticTokens data must be a flat list of 5-tuples"

# decode the delta encoding into absolute (line, char, len, type, mods)
tokens = []
line, char = 0, 0
for i in range(0, len(data), 5):
    dl, dc, ln, ty, mods = data[i:i + 5]
    line += dl
    char = char + dc if dl == 0 else dc
    tokens.append((line, char, ln, ty, mods))

# every token must sit on a plain identifier, sorted, non-overlapping
KEY = lambda l, c: l * 4096 + c
prev_end = -1
for (l, c, ln, ty, mods) in tokens:
    assert 0 <= l < len(LINES), f"token line {l} outside the document"
    line_text = LINES[l]
    assert 0 <= c and c + ln <= len(line_text), \
        f"token {l}:{c}+{ln} outside its line"
    word = line_text[c:c + ln]
    assert word and all(ch.isalnum() or ch == "_" for ch in word), \
        f"token text {word!r} is not an identifier"
    assert KEY(l, c) >= prev_end, \
        f"tokens must be sorted and non-overlapping at {l}:{c}"
    prev_end = KEY(l, c + ln)
    assert 0 <= ty < 10 and 0 <= mods < 16, "legend indices out of range"

TOK = {(l, c): (ln, ty, mods) for (l, c, ln, ty, mods) in tokens}

# token type / modifier indices from the legend above
NS, TYPE, STRUCT, ENUM, IFACE, FUNC, VAR, PARAM, PROP, EMEM = range(10)
DECL, RO, DEFLIB, STATIC = 1, 2, 4, 8

def expect(word, line, col, ty, mods):
    key = (line, col)
    got = TOK.get(key)
    assert got is not None, f"no token at {line}:{col} for {word!r}"
    assert got[1] == ty and got[2] == mods, \
        f"{word!r}: expected (type={ty}, mods={mods}), got (type={got[1]}, mods={got[2]})"
    assert LINES[line][col:col + got[0]] == word, \
        f"token at {line}:{col} is {LINES[line][col:col + got[0]]!r}, not {word!r}"

def col_of(line, word, start=0):
    return LINES[line].index(word, start)

def after_paren(line, word):
    """column of `word` inside the first (...) on the line"""
    return LINES[line].index(word, LINES[line].index("(") + 1)

# ---- module name -> namespace + declaration -------------------------------
expect("demo", 0, col_of(0, "demo"), NS, DECL)

# ---- struct declaration + fields -------------------------------------------
expect("Vec2", 2, col_of(2, "Vec2"), STRUCT, DECL)
expect("x", 2, col_of(2, "x"), PROP, DECL)
expect("y", 2, col_of(2, "y"), PROP, DECL)

# ---- enum declaration + variants -------------------------------------------
expect("Color", 4, col_of(4, "Color"), ENUM, DECL)
expect("Red", 4, col_of(4, "Red"), EMEM, DECL)
expect("Green", 4, col_of(4, "Green"), EMEM, DECL)
expect("Blue", 4, col_of(4, "Blue"), EMEM, DECL)

# ---- fn declarations + parameters ------------------------------------------
expect("norm", 6, col_of(6, "norm"), FUNC, DECL)
expect("v", 6, col_of(6, "v"), PARAM, DECL | RO)
expect("describe", 11, col_of(11, "describe"), FUNC, DECL)
expect("c", 11, after_paren(11, "c"), PARAM, DECL | RO)
expect("main", 18, col_of(18, "main"), FUNC, DECL)

# ---- type references in annotation positions (textual pass) ---------------
expect("Vec2", 6, col_of(6, "Vec2"), STRUCT, 0)
expect("Color", 11, col_of(11, "Color"), ENUM, 0)

# ---- locals: declarations (let) and uses -----------------------------------
expect("d", 7, col_of(7, "d"), VAR, DECL | RO)
expect("v", 7, col_of(7, "v"), PARAM, RO)   # v is norm's parameter at its use
expect("d", 8, col_of(8, "d"), VAR, RO)
expect("v", 19, col_of(19, "let") + 4, VAR, DECL | RO)
expect("n", 20, col_of(20, "n"), VAR, DECL | RO)
expect("c", 21, col_of(21, "c"), VAR, DECL | RO)

# ---- struct fields at access sites -----------------------------------------
expect("x", 7, col_of(7, "x"), PROP, 0)
expect("y", 7, col_of(7, "y"), PROP, 0)

# ---- struct-literal type name ----------------------------------------------
expect("Vec2", 19, col_of(19, "Vec2", col_of(19, "=") + 1), STRUCT, 0)

# ---- match: variant + scrutinee parameter -----------------------------------
expect("c", 12, after_paren(12, "c"), PARAM, RO)
expect("Red", 13, col_of(13, "Red"), EMEM, 0)

# ---- calls: user fns and compiler-known builtins ---------------------------
expect("norm", 20, col_of(20, "norm"), FUNC, 0)
expect("v", 20, col_of(20, "v"), VAR, RO)
expect("describe", 23, col_of(23, "describe"), FUNC, 0)
expect("c", 23, col_of(23, "describe") + len("describe") + 1, VAR, RO)
expect("print_f64", 22, col_of(22, "print_f64"), FUNC, DEFLIB)
expect("println", 22, col_of(22, "println"), FUNC, DEFLIB)
expect("print_i64", 23, col_of(23, "print_i64"), FUNC, DEFLIB)

# ---- qualified enum path: enum name + variant member ------------------------
expect("Color", 21, col_of(21, "Color"), ENUM, 0)
expect("Green", 21, col_of(21, "Green"), EMEM, 0)

# ---- type references in annotations inside main() --------------------------
expect("Vec2", 19, col_of(19, "Vec2"), STRUCT, 0)
expect("Color", 21, col_of(21, "Color"), ENUM, 0)

send({"jsonrpc": "2.0", "id": 3, "method": "shutdown"})
send({"jsonrpc": "2.0", "method": "exit"})
proc.wait(timeout=10)
sys.exit(0)
