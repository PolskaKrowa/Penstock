#!/usr/bin/env python3
"""Fuzz nested-parfor collapse programs against the penstock front-end.

All ingredients verified sema-valid: inner parfor carries NO clauses
(any clause turns the nest into the work-stealing path), writes inside
bodies go only to the outer reduction accumulator, lets are deduped.
Rejection-samples until the program reaches IRgen.
"""
import os, random, subprocess, sys

OUT = "/tmp/fuzz"
os.makedirs(OUT, exist_ok=True)

CHECKBIN = "/tmp/asan/fecheck_clang"

def gen(rng):
    n = rng.choice([8, 16, 64])
    lines = ["fn main() {"]
    lines.append("    let n: i64 = %d;" % n)
    lines.append("    let mut acc: i64 = 0;")
    reds = ["reduction(+:acc)"]
    if rng.random() < 0.3:
        lines.append("    let mut s: i64 = 1;")
        reds.append("reduction(*:s)")
    use_arr = rng.random() < 0.5
    if use_arr:
        lines.append("    let un: usize = %d;" % n)
        lines.append("    let buf: *i64 = malloc(i64, un);")
        if rng.random() < 0.5:
            lines.append("    parfor (q in 0..un) [reduction(+:acc)] { buf[q] = q as i64; }")
    if rng.random() < 0.3:  # adjacent parfor before the nest (fusion/pairing bait)
        lines.append("    parfor (q in 0..n) [reduction(+:acc)] { acc = acc + 2; }")
    # outer body: lets before (deduped names), inner parfor, statements after
    pre, post, inner = [], [], []
    names = iter("tuvwxyz")
    used = set()
    for _ in range(rng.randrange(0, 3)):
        nm = next(names)
        used.add(nm)
        pre.append(rng.choice([
            "        let %s: usize = i;" % nm,
            "        let %s: i64 = i as i64;" % nm,
        ]))
    for _ in range(rng.randrange(0, 3)):
        if rng.random() < 0.5:
            post.append("        acc = acc - 1;")
        else:
            nm = next(names)
            post.append("        let %s: usize = i;" % nm)
    for _ in range(rng.randrange(0, 3)):
        nm = next(names)
        inner.append("            let %s: usize = j;" % nm)
    arrw = " buf[j] = j as i64;" if use_arr else ""
    lines.append("    parfor (i in 0..n) [%s, collapse:2] {" % ", ".join(reds))
    for s in pre:
        lines.append(s)
    lines.append("        parfor (j in 0..n) {")
    for s in inner:
        lines.append(s)
    lines.append("            acc = acc + 1;" + (" s = s * 2;" if len(reds) > 1 else "") + arrw)
    for _ in range(rng.randrange(0, 2)):
        lines.append("            acc = acc + j as i64;")
    lines.append("        }")
    for s in post:
        lines.append(s)
    lines.append("    }")
    if rng.random() < 0.4:  # adjacent parfor after the nest (pairing bait)
        lines.append("    parfor (q in 0..n) [reduction(+:acc)] { acc = acc + q as i64; }")
    tail = " + s" if len(reds) > 1 else ""
    lines.append("    print_i64(acc%s); println();" % tail)
    if use_arr:
        lines.append("    print_i64(buf[0]); println();")
        lines.append("    free(buf);")
    lines.append("}")
    return "\n".join(lines) + "\n"

def reaches_irgen(path):
    try:
        r = subprocess.run([CHECKBIN, path, "-o", "/dev/null"],
                           capture_output=True, text=True, timeout=30)
    except subprocess.TimeoutExpired:
        return False
    return r.returncode == 0 and "IR written" in r.stdout + r.stderr

def main():
    seed0 = int(sys.argv[1]) if len(sys.argv) > 1 else 1
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 200
    bins = sys.argv[3].split(",") if len(sys.argv) > 3 else [
        "/tmp/asan/fecheck_asan", "/tmp/asan/fecheck_clang",
        "/tmp/asan/fecheck_O3"]
    flagsets = ["", "--smt-interleave", "--batch-ops", "--mem-reuse --mov-minimize",
                "--mov-minimize --mem-reuse --batch-ops --smt-interleave --vector-plan",
                "--vector-plan"]
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0:abort_on_error=0")
    made = 0
    tries = 0
    crashes = 0
    while made < count and tries < count * 6:
        tries += 1
        rng = random.Random(seed0 * 1000000 + tries)
        src = gen(rng)
        path = "%s/f%05d.fsh" % (OUT, made)
        with open(path, "w") as f:
            f.write(src)
        if not reaches_irgen(path):
            os.unlink(path)
            continue
        # confirm collapse actually fires on at least the plain run
        r = subprocess.run([CHECKBIN, path, "-o", "/dev/null"],
                           capture_output=True, text=True, timeout=30)
        if "F0907" not in r.stderr:
            os.unlink(path)
            continue
        made += 1
        for b in bins:
            for fl in flagsets:
                try:
                    r = subprocess.run([b] + (fl.split() if fl else []) + [path, "-o", "/dev/null"],
                                       capture_output=True, text=True, timeout=30, env=env)
                except subprocess.TimeoutExpired:
                    continue
                out = r.stdout + r.stderr
                if r.returncode >= 128 or "AddressSanitizer" in out or "runtime error" in out:
                    crashes += 1
                    print("CRASH: %s bin=%s flags=[%s] exit=%d" % (path, b, fl, r.returncode))
                    print(out[:4000])
                    return 1
        if made % 25 == 0:
            print("... %d/%d valid (crashes=%d)" % (made, count, crashes), flush=True)
    print("done: %d valid programs (%d tries), %d crashes" % (made, tries, crashes))
    return 0

if __name__ == "__main__":
    sys.exit(main())
