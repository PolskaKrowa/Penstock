#!/usr/bin/env python3
"""LSP regression checks for diagnostics, comments and entry-point policy."""
import json
import subprocess
import sys
import pathlib
import tempfile

SERVER = sys.argv[1]


def run_document(path, text):
    proc = subprocess.Popen([SERVER], stdin=subprocess.PIPE,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def send(message):
        body = json.dumps(message, separators=(",", ":")).encode()
        proc.stdin.write(f"Content-Length: {len(body)}\r\n\r\n".encode() + body)
        proc.stdin.flush()

    def recv():
        header = b""
        while not header.endswith(b"\r\n\r\n"):
            byte = proc.stdout.read(1)
            if not byte:
                raise AssertionError("language server exited before replying")
            header += byte
        length = int(next(line.split(b":", 1)[1].strip()
                           for line in header.split(b"\r\n")
                           if line.lower().startswith(b"content-length:")))
        payload = proc.stdout.read(length)
        return json.loads(payload)

    try:
        send({"jsonrpc": "2.0", "id": 1, "method": "initialize",
              "params": {"capabilities": {}}})
        init = recv()
        assert init.get("result", {}).get("capabilities", {}).get("textDocumentSync") == 1
        send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
        uri = "file://" + path
        send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
            "textDocument": {"uri": uri, "languageId": "freshet",
                             "version": 1, "text": text}}})
        notification = recv()
        assert notification.get("method") == "textDocument/publishDiagnostics", notification
        diagnostics = notification.get("params", {}).get("diagnostics", [])
        send({"jsonrpc": "2.0", "id": 2, "method": "shutdown"})
        recv()
        send({"jsonrpc": "2.0", "method": "exit", "params": {}})
        proc.wait(timeout=10)
        return diagnostics
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait(timeout=5)


# A real narrowing cast should cover the full expression and explain what
# can be lost. Cast-like text in both supported comment forms must be ignored.
initial_diags = run_document("/tmp/penstock-lsp-check.fsh", """\
fn narrow(x: usize) -> u8 {
    return x as u8;
}
fn main() {}
// This is only prose: x as u8 and x / 10 should not produce warnings.
/* Nor should this commented code: let fake: u8 = x as u8; let q = x / 10;
   /* Nor this nested comment: let nested: u8 = x as u8; let z = x / 10; */
*/
""")
comment_arith = [d for d in initial_diags if d.get("code") in ("F0401", "F0402")]
assert not comment_arith, f"comment contents produced arithmetic warnings: {comment_arith}"
cast_diags = [d for d in initial_diags if d.get("code") == "F0211"]
assert len(cast_diags) == 1, f"expected one cast warning, got {cast_diags}"
cast = cast_diags[0]
start = cast["range"]["start"]
end = cast["range"]["end"]
assert end["line"] == start["line"] and end["character"] - start["character"] >= 6, \
    f"cast diagnostic must underline the complete expression, got {cast['range']}"
message = cast["message"]
for detail in ("usize", "u8", "0..255", "256", "truncated"):
    assert detail in message, f"cast warning missing useful detail {detail!r}: {message}"

# No fn main() requirement for ordinary source files or for files in nested
# src subdirectories. It applies only to direct src/*.fsh files with `main`
# as a standalone token in the basename.
def has_main_error(path):
    return any(d.get("code") == "F0206" for d in
               run_document(path, "fn helper() {}\n"))

cases = {
    "/tmp/penstock-project/src/helper.fsh": False,
    "/tmp/penstock-project/src/domain.fsh": False,
    "/tmp/penstock-project/src/foo_main.fsh": True,
    "/tmp/penstock-project/src/main.fsh": True,
    "/tmp/penstock-project/src/foo/bar_main.fsh": False,
    "/tmp/penstock-project/main.fsh": False,
}
for path, expected in cases.items():
    actual = has_main_error(path)
    assert actual == expected, \
        f"main requirement for {path}: expected {expected}, got {actual}"

# Diagnostics from an imported module must not be published using the
# currently-open entry document's URI/ranges.  That previously painted an
# imported file's F0401 warning over unrelated source lines (including
# comments and function signatures) in the entry buffer.
with tempfile.TemporaryDirectory(prefix="freshet-lsp-import-diags-") as td:
    root = pathlib.Path(td)
    src = root / "src"
    src.mkdir()
    entry_path = src / "main.fsh"
    module_path = src / "maths.fsh"
    module_text = """pub fn divide_by_ten(value: usize) -> usize {
    return value / 10;
}
"""
    module_path.write_text(module_text)
    entry_text = """import maths;
fn main() {
    let mut n: usize = 1;
    n += 1;
    // An imported-module warning used to land on this comment line.
    /* And sometimes it appeared here, despite no division in this file. */
}
"""
    entry_path.write_text(entry_text)
    entry_diags = run_document(str(entry_path), entry_text)
    leaked = [d for d in entry_diags if d.get("code") in ("F0401", "F0402")]
    assert not leaked, \
        f"imported module arithmetic diagnostics leaked into entry document: {leaked}"

    # The same warning is still available when the imported file itself is
    # opened as the document being analysed.
    own_module_diags = run_document(str(module_path), module_text)
    own_division = [d for d in own_module_diags if d.get("code") == "F0401"]
    assert len(own_division) == 1, \
        f"expected imported module's F0401 when opened directly, got {own_module_diags}"

print("LSP diagnostic/comment/main/import-attribution checks passed")
