#!/usr/bin/env python3
"""LSP navigation smoke test (report §10.1): drives the language server
over stdio JSON-RPC and checks hover / definition / references / rename
resolve through the sema-annotated AST.  Self-contained: the analysed
document is embedded here so every position is exact."""
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

#      0         1         2         3
#      0123456789012345678901234567890123456789
TEXT = """fn add(a: i64, b: i64) -> i64 {
    return a + b;
}
fn main() {
    let total: i64 = add(2, 3);
    let other: i64 = add(total, 4);
    print_i64(other); println();
}
fn accepts_string(value: String) { }
fn nested_scope() {
    {
        let hidden: i64 = 9;
        print_i64(hidden);
    }
}
"""
# line 4: `    let total: i64 = add(2, 3);`   -> add at [21,24)
# line 6: `    print_i64(other); println();`  -> other at [15,20)
uri = "file:///tmp/lsp_nav_check.fsh"

send({"jsonrpc": "2.0", "id": 1, "method": "initialize",
      "params": {"capabilities": {}}})
init = recv()
assert init and init.get("id") == 1, "missing initialize response"
init_result = init.get("result") or {}
init_capabilities = init_result.get("capabilities") or {}
assert init_capabilities.get("textDocumentSync") == 1, \
    "InitializeResult.capabilities missing or malformed"
assert init_capabilities.get("positionEncoding") == "utf-16", \
    "server must explicitly advertise the UTF-16 positions it uses"
assert init_result.get("serverInfo", {}).get("name") == \
    "freshet-language-server", "serverInfo must be at InitializeResult root"
send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": uri, "languageId": "freshet",
                     "version": 1, "text": TEXT}}})
recv()

# hover over the `add` call (line 4, char 22) -> the full signature
send({"jsonrpc": "2.0", "id": 2, "method": "textDocument/hover", "params": {
    "textDocument": {"uri": uri},
    "position": {"line": 4, "character": 22}}})
r = recv()
assert r and r.get("result"), "hover returned nothing"
assert "fn add" in r["result"]["contents"]["value"], "hover signature"

# goto-definition of `add` (line 4 char 22) -> its declaration (line 0)
send({"jsonrpc": "2.0", "id": 3, "method": "textDocument/definition",
      "params": {"textDocument": {"uri": uri},
                 "position": {"line": 4, "character": 22}}})
r = recv()
assert r and r["result"]["range"]["start"]["line"] == 0, "definition line"

# goto-definition of the local `other` (line 6 char 17) -> its `let`
send({"jsonrpc": "2.0", "id": 4, "method": "textDocument/definition",
      "params": {"textDocument": {"uri": uri},
                 "position": {"line": 6, "character": 17}}})
r = recv()
assert r and r["result"]["range"]["start"]["line"] == 5, "local definition"

# references of `add` -> declaration + 2 call sites = 3 locations
send({"jsonrpc": "2.0", "id": 5, "method": "textDocument/references",
      "params": {"textDocument": {"uri": uri},
                 "position": {"line": 4, "character": 22},
                 "includeDeclaration": True}})
r = recv()
assert r and len(r["result"]) == 3, "reference count"

# rename `other` -> `result`: declaration + single use = 2 edits
send({"jsonrpc": "2.0", "id": 6, "method": "textDocument/rename",
      "params": {"textDocument": {"uri": uri},
                 "position": {"line": 6, "character": 17},
                 "newName": "result"}})
r = recv()
edits = r["result"]["changes"][uri]
assert len(edits) == 2, "rename edit count"
assert all(e["newText"] == "result" for e in edits), "rename text"

# documentSymbol must not include synthetic prelude declarations with
# SrcLoc{} (for example String is added lazily when a String type is used).
send({"jsonrpc": "2.0", "id": 7, "method": "textDocument/documentSymbol",
      "params": {"textDocument": {"uri": uri}}})
r = recv()
assert r and isinstance(r.get("result"), list), "documentSymbol response"
assert {s["name"] for s in r["result"]} >= {"add", "main", "accepts_string"}, \
    "documentSymbol should include declarations in this document"
for symbol in r["result"]:
    for range_key in ("range", "selectionRange"):
        for endpoint in ("start", "end"):
            pos = symbol[range_key][endpoint]
            assert pos["line"] >= 0, "documentSymbol line must be non-negative"
            assert pos["character"] >= 0, "documentSymbol character must be non-negative"
assert "String" not in {s["name"] for s in r["result"]}, \
    "synthetic prelude String must not appear as a document symbol"

# Local identifier annotations must outlive the lexical scopes in which
# they were declared.  The semantic analyser pops both the nested block and
# function-body scopes before LSP navigation walks these AST references.
send({"jsonrpc": "2.0", "id": 9, "method": "textDocument/hover", "params": {
    "textDocument": {"uri": uri},
    "position": {"line": 12, "character": 20}}})
r = recv()
assert r and r.get("result"), "hover on a nested-scope local returned nothing"
assert "hidden" in r["result"]["contents"]["value"], "nested local hover name"
assert "i64" in r["result"]["contents"]["value"], "nested local hover type"

send({"jsonrpc": "2.0", "id": 10, "method": "textDocument/references", "params": {
    "textDocument": {"uri": uri},
    "position": {"line": 12, "character": 20},
    "includeDeclaration": True}})
r = recv()
assert r and len(r["result"]) == 2, "nested local references include declaration + use"

send({"jsonrpc": "2.0", "id": 11, "method": "shutdown"})
send({"jsonrpc": "2.0", "method": "exit"})
proc.wait(timeout=10)
sys.exit(0)
