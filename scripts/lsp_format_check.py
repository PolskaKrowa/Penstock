#!/usr/bin/env python3
"""LSP formatting smoke test: drives the language server over stdio
JSON-RPC and checks textDocument/formatting + textDocument/rangeFormatting
("Format Document" / "Format Selection") — the same token-preserving
formatter that powers `penstock fmt`, applied to the open buffer.

Checks:
  * capabilities advertise documentFormattingProvider + range provider;
  * formatting a messy document returns one whole-document edit whose
    applied result follows the house style (spacing, single blank lines
    between declarations, one trailing newline);
  * formatting is idempotent (second request returns no edits);
  * range formatting snaps outward to whole toplevel declarations only —
    bytes outside the selected declaration are untouched, and a range
    covering only inter-declaration whitespace yields no edits;
  * FormattingOptions are honoured (tabSize 2 / insertSpaces).
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

def apply_edit(text, edit):
    """Apply one LSP TextEdit (single-line-compatible splicing)."""
    def offset(pos):
        n = 0
        ls = text.split("\n")
        for i in range(pos["line"]):
            n += len(ls[i]) + 1
        return n + pos["character"]
    s = offset(edit["range"]["start"])
    e = offset(edit["range"]["end"])
    return text[:s] + edit["newText"] + text[e:]

uri = "file:///tmp/lsp_format_check.fsht"
uri2 = "file:///tmp/lsp_format_check2.fsht"

# deliberately messy: multiple spaces, crammed signatures, 4 blank lines
TEXT = ("fn   add (a:i64, b:i64)->i64{ return   a+b; }\n"
        "\n\n\n\n"
        "fn main(){ let  total: i64 = add( 2 , 3 ); print_i64(total); println(); }\n")

send({"jsonrpc": "2.0", "id": 1, "method": "initialize",
      "params": {"capabilities": {}}})
init = recv()
assert init and init.get("id") == 1, "missing initialize response"
caps = (init.get("result") or {}).get("capabilities") or {}
assert caps.get("documentFormattingProvider") is True, \
    "server must advertise documentFormattingProvider"
assert caps.get("documentRangeFormattingProvider") is True, \
    "server must advertise documentRangeFormattingProvider"

send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": uri, "languageId": "freshet",
                     "version": 1, "text": TEXT}}})
recv()

# ---- Format Document ------------------------------------------------------
send({"jsonrpc": "2.0", "id": 2, "method": "textDocument/formatting",
      "params": {"textDocument": {"uri": uri},
                 "options": {"tabSize": 4, "insertSpaces": True}}})
r = recv()
edits = r.get("result")
assert isinstance(edits, list) and len(edits) == 1, \
    f"formatting must return exactly one whole-document edit, got {edits!r}"
e = edits[0]
assert e["range"]["start"] == {"line": 0, "character": 0}, "edit starts at 0:0"
doc_end_line = TEXT.count("\n")
assert e["range"]["end"] == {"line": doc_end_line, "character": 0}, \
    f"whole-document edit must end at the document end, got {e['range']['end']!r}"
FMT = apply_edit(TEXT, e)

# house style assertions
assert "fn   add" not in FMT and "fn add(" in FMT, "fn header normalised"
assert "a+b" not in FMT and "a + b" in FMT, "binary operators spaced"
assert "add( 2 , 3 )" not in FMT and "add(2, 3)" in FMT, "call spacing normalised"
assert "\n\n\n" not in FMT, "declaration gaps collapse to one blank line"
assert FMT.endswith("\n") and not FMT.endswith("\n\n"), \
    "exactly one trailing newline"
assert "let  total" not in FMT and "let total: i64" in FMT, "let spacing"
assert "\n    " in FMT, "statements indent one level"

# ---- idempotence: reformatting the formatted text returns no edits --------
send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
    "textDocument": {"uri": uri, "version": 2},
    "contentChanges": [{"text": FMT}]}})
recv()
send({"jsonrpc": "2.0", "id": 3, "method": "textDocument/formatting",
      "params": {"textDocument": {"uri": uri},
                 "options": {"tabSize": 4, "insertSpaces": True}}})
r = recv()
assert r.get("result") == [], \
    f"reformatting formatted text must be a no-op, got {r.get('result')!r}"

# ---- Format Selection ------------------------------------------------------
# select INSIDE the `fn add` declaration of the messy document: the edit
# must stay confined to that declaration; `fn main` stays byte-identical.
send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
    "textDocument": {"uri": uri, "version": 3},
    "contentChanges": [{"text": TEXT}]}})
recv()
send({"jsonrpc": "2.0", "id": 4, "method": "textDocument/rangeFormatting",
      "params": {"textDocument": {"uri": uri},
                 "range": {"start": {"line": 0, "character": 10},
                           "end": {"line": 0, "character": 30}},
                 "options": {"tabSize": 4, "insertSpaces": True}}})
r = recv()
redits = r.get("result")
assert isinstance(redits, list) and len(redits) <= 1, \
    f"range formatting returns at most one edit, got {redits!r}"
assert redits, "range formatting inside fn add must produce an edit"
re_ = redits[0]
assert re_["range"]["start"]["line"] == 0 and re_["range"]["end"]["line"] == 0, \
    f"range edit must stay inside fn add's single-line declaration, got {re_['range']!r}"
OUT = apply_edit(TEXT, re_)
assert "fn   add" not in OUT and "fn add(" in OUT, "selected fn was formatted"
assert "fn main(){ let  total: i64 = add( 2 , 3 )" in OUT, \
    "bytes outside the selected declaration must stay untouched"

# selection in whitespace only (between the two fns): nothing to format
send({"jsonrpc": "2.0", "id": 5, "method": "textDocument/rangeFormatting",
      "params": {"textDocument": {"uri": uri},
                 "range": {"start": {"line": 1, "character": 0},
                           "end": {"line": 3, "character": 0}},
                 "options": {"tabSize": 4, "insertSpaces": True}}})
r = recv()
assert r.get("result") == [], \
    f"range covering only inter-declaration whitespace must yield no edits, got {r.get('result')!r}"

# ---- FormattingOptions are honoured: two-space indentation ---------------
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": uri2, "languageId": "freshet",
                     "version": 1, "text": TEXT}}})
recv()
send({"jsonrpc": "2.0", "id": 6, "method": "textDocument/formatting",
      "params": {"textDocument": {"uri": uri2},
                 "options": {"tabSize": 2, "insertSpaces": True}}})
r = recv()
edits2 = r.get("result")
assert isinstance(edits2, list) and len(edits2) == 1, "tabSize 2 formatting edit"
e2 = edits2[0]
assert "\n    " not in e2["newText"], \
    "tabSize 2 must produce two-space indentation"
assert "\n  " in e2["newText"], "tabSize 2 must produce two-space indentation"

send({"jsonrpc": "2.0", "id": 7, "method": "shutdown"})
send({"jsonrpc": "2.0", "method": "exit"})
proc.wait(timeout=10)
sys.exit(0)
