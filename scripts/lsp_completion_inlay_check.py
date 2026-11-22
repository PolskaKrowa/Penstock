#!/usr/bin/env python3
"""LSP completion + inlayHint smoke test for freshet-language-server.

Drives the server over stdio JSON-RPC: initialize, didOpen, then
textDocument/completion (scope-aware + prefix-filtered) and
textDocument/inlayHint over the whole file.  Asserts the responses
contain the expected labels; prints a PASS/FAIL summary.
"""
import json
import subprocess
import sys
import time

SERVER = sys.argv[1] if len(sys.argv) > 1 else "/tmp/lsp-test"

# NOTE: parameter/identifier names must avoid parfor clause keywords
# (`by`, `step`, ...) — a parse error would skip sema and legitimately
# produce empty completion/hint results.
DOC = """\
module lspdemo;

fn dist2(ax: f64, ay: f64, bx: f64, vy: f64) -> f64 {
    let dx = bx - ax;
    let dy: f64 = vy - ay;
    return dx * dx + dy * dy;
}

fn area(w: f64, h: f64) -> f64 {
    return w * h;
}

fn main() {
    let d = dist2(1.0, 2.0, 4.0, 6.0);
    let a = area(3.0, 5.0);
    print_i64(42);
}
"""


def frame(msg: str) -> bytes:
    return f"Content-Length: {len(msg)}\r\n\r\n{msg}".encode()


class LspClient:
    def __init__(self, binary):
        self.p = subprocess.Popen([binary], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE)
        self.buf = b""
        self.next_id = 1

    def request(self, method, params):
        rid = self.next_id
        self.next_id += 1
        msg = json.dumps({"jsonrpc": "2.0", "id": rid, "method": method,
                          "params": params})
        self.p.stdin.write(frame(msg))
        self.p.stdin.flush()
        return rid

    def notify(self, method, params):
        msg = json.dumps({"jsonrpc": "2.0", "method": method,
                          "params": params})
        self.p.stdin.write(frame(msg))
        self.p.stdin.flush()

    def read_msg(self, timeout=20.0):
        import selectors
        sel = selectors.DefaultSelector()
        sel.register(self.p.stdout, selectors.EVENT_READ)
        deadline = time.monotonic() + timeout
        while b"\r\n\r\n" not in self.buf:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not sel.select(remaining):
                return None
            chunk = self.p.stdout.read1(65536)
            if not chunk:
                return None
            self.buf += chunk
        head, rest = self.buf.split(b"\r\n\r\n", 1)
        length = int(
            next(l for l in head.split(b"\r\n")
                 if l.lower().startswith(b"content-length")).split(b":")[1])
        while len(rest) < length:
            rest += self.p.stdout.read1(length - len(rest))
        body, self.buf = rest[:length], rest[length:]
        sel.close()
        return json.loads(body)

    def wait_for(self, rid, timeout=20.0):
        while True:
            msg = self.read_msg(timeout)
            if msg is None:
                return None
            if msg.get("id") == rid and "result" in msg:
                return msg["result"]

    def close(self):
        try:
            self.request("shutdown", {})
            self.read_msg(5)
            self.notify("exit", {})
            self.p.wait(5)
        except Exception:
            self.p.kill()


def main():
    c = LspClient(SERVER)
    uri = "file:///tmp/lsp_demo.fsht"
    init = c.request("initialize", {"processId": None, "rootUri": None,
                                    "capabilities": {}})
    caps = c.wait_for(init)
    assert caps, "no initialize response"
    capd = caps.get("capabilities", {})
    ok_caps = {
        "completion": "completionProvider" in capd,
        "inlayHint": "inlayHintProvider" in capd,
    }
    c.notify("initialized", {})
    c.notify("textDocument/didOpen", {
        "textDocument": {"uri": uri, "languageId": "freshet",
                         "version": 1, "text": DOC}})

    lines = DOC.splitlines()

    def pos_after(needle):
        for i, l in enumerate(lines):
            if needle in l:
                j = l.index(needle)
                return {"line": i, "character": j + len(needle)}
        raise AssertionError(f"not found: {needle}")

    # 1. completion with prefix "d" right after `... + d` inside dist2
    r1 = c.request("textDocument/completion",
                   {"textDocument": {"uri": uri},
                    "position": pos_after("return dx * dx + d")})
    items1 = c.wait_for(r1).get("items", [])
    labels1 = [i["label"] for i in items1]

    # 2. completion with empty prefix after `let a = ` in main
    r2 = c.request("textDocument/completion",
                   {"textDocument": {"uri": uri},
                    "position": pos_after("let a = ")})
    items2 = c.wait_for(r2).get("items", [])
    labels2 = [i["label"] for i in items2]

    # 3. inlay hints over the whole document
    r3 = c.request("textDocument/inlayHint", {
        "textDocument": {"uri": uri},
        "range": {"start": {"line": 0, "character": 0},
                  "end": {"line": len(lines), "character": 0}}})
    hints = c.wait_for(r3)

    checks = []
    checks.append(("caps.advertise.completion", ok_caps["completion"]))
    checks.append(("caps.advertise.inlayHint", ok_caps["inlayHint"]))
    # scope + prefix filtering in one: inside dist2, prefix "d"
    checks.append(("complete.sees.local(dx)", "dx" in labels1))
    checks.append(("complete.sees.local(dy)", "dy" in labels1))
    checks.append(("complete.sees.fn(dist2)", "dist2" in labels1))
    checks.append(("complete.prefix.filters.area", "area" not in labels1))
    # empty prefix in main: whole visible surface
    checks.append(("complete.sees.local(d)", "d" in labels2))
    checks.append(("complete.sees.local(a)", "a" in labels2))
    checks.append(("complete.sees.fn(area)", "area" in labels2))
    checks.append(("complete.sees.struct(String)", "String" in labels2))
    checks.append(("complete.sees.enum(Option)", "Option" in labels2))
    checks.append(("complete.keyword(parfor)", "parfor" in labels2))
    checks.append(("complete.builtin(vec_new)", "vec_new" in labels2))
    checks.append(("complete.builtin(parse_i64)", "parse_i64" in labels2))
    checks.append(("complete.builtin.checked_add)", "checked_add" in labels2))

    ok_hints = isinstance(hints, list)
    checks.append(("hint.array", ok_hints))
    if ok_hints:
        thint = [h for h in hints if h["kind"] == 1]
        phint = [h for h in hints if h["kind"] == 2]
        tlabels = [h["label"] for h in thint]
        plabels = [h["label"] for h in phint]
        checks.append(("hint.type(dx: f64)", ": f64" in tlabels))
        # `let dy: f64` is annotated — no type hint for it
        checks.append(("hint.type.count(dx,d,a == 3)", len(thint) == 3))
        checks.append(("hint.param(ax:)", "ax:" in plabels))
        checks.append(("hint.param(ay:)", "ay:" in plabels))
        checks.append(("hint.param(bx:)", "bx:" in plabels))
        checks.append(("hint.param(vy:)", "vy:" in plabels))
        checks.append(("hint.param(w:)", "w:" in plabels))
        checks.append(("hint.param(h:)", "h:" in plabels))
        # positions well-formed (inside the doc, monotone-ish per fn)
        good_pos = all("position" in h and "label" in h for h in hints)
        checks.append(("hint.shape", good_pos))

    c.close()

    failures = [name for name, ok in checks if not ok]
    for name, ok in checks:
        print(f"{'PASS' if ok else 'FAIL'}  {name}")
    print(f"\nitems1={len(items1)} items2={len(items2)} "
          f"hints={len(hints) if isinstance(hints, list) else hints}")
    if ok_hints:
        for h in hints:
            p = h["position"]
            print(f"  hint kind={h['kind']} label={h['label']!r} "
                  f"at line {p['line']} char {p['character']}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
