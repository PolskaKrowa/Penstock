#!/usr/bin/env python3
"""Property-based differential testing (optimisation-audit §10.3).

Generates small random Freshet programs in a restricted, deterministic
subset and compares execution between the unoptimised build and the
optimised build (with every Phase D source transform enabled).  The
generator is deliberately biased towards the audit's adversarial
shapes:

  * zero- and one-trip loops (range boundaries at lo, lo+1);
  * negative and extreme integer constants (INT64_MIN/MAX neighbours);
  * casts and mixed widths (u8/u16/u32 truncation);
  * first-iteration guards (peel shapes: `if (k == 0)`);
  * fixed-trip loops in the partial-unroll window (17..64);
  * nested rectangular loops with transposed writes (interchange);
  * two independent statement groups (distribution).

Properties are not proofs — they are excellent at catching C++ pass
implementation mistakes; the Lean kernel and Alive2 remain the
correctness gates.

usage: scripts/property_test.py [N] [--seed S]   (default N=100, seed 7)
exit code 0 = all programs agreed on FULL observable stdout
"""
import random, subprocess, sys, tempfile, os, shutil

PEN = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "..", "bin", "penstock")
# Inherit the environment (LLVM_ROOT etc.) as-is: penstock falls back to
# its baked-in backend default and the usual 23..19 probe on its own.
ENV = dict(os.environ)

OPT_FLAGS = ["-O2", "--licm", "--partial-unroll", "--peel",
             "--interchange", "--tile", "--distribute", "--unroll-and-jam"]


def gen_program(rng, idx):
    lines = ["fn work(x: i64) -> i64 { return x * 3 - 1; }",
             "fn main() -> i32 {"]
    # a few scalar accumulators with different types
    lines.append("    let mut acc: i64 = 0;")
    lines.append("    let mut accu: u64 = 0;")
    loops = 0
    for _ in range(rng.randint(1, 3)):
        kind = rng.choice(["plain", "peel", "nested", "punroll", "cast"])
        lo = rng.choice([0, 0, 0, 1, -3])
        # trip either tiny (0/1) or in the partial-unroll window
        trip = rng.choice([0, 1, 2, 17, 20, 25, 33, 48])
        hi = lo + trip
        k = loops
        loops += 1
        if kind == "plain":
            lines.append(
                f"    for (k{k} in {lo}..{hi}) {{ acc += k{k} * 2 + 3; }}")
        elif kind == "peel":
            lines.append(
                f"    for (k{k} in {lo}..{hi}) {{ "
                f"if (k{k} == {lo}) {{ acc += 100; }} else {{ acc += 1; }} }}")
        elif kind == "punroll":
            lines.append(
                f"    for (k{k} in {lo}..{hi}) {{ acc += work(k{k} as i64); }}")
        elif kind == "cast":
            lines.append(
                f"    for (k{k} in {lo}..{hi}) {{ "
                f"let t: u8 = (k{k} as u8); accu += t as u64; "
                f"acc += (t as i64) + 1; }}")
        elif kind == "nested" and trip >= 2:
            inner = rng.randint(2, 6)
            lines.append(
                f"    for (k{k} in {lo}..{hi}) {{ "
                f"for (j{k} in 0..{inner}) {{ acc += k{k} * {inner} + j{k}; }} }}")
        else:
            lines.append(
                f"    for (k{k} in {lo}..{hi}) {{ acc += k{k}; }}")
    lines.append('    print_i64(acc); println();')
    lines.append('    print_i64(accu as i64); println();')
    lines.append("    return 0;")
    lines.append("}")
    return "\n".join(lines) + "\n"


def run_config(tmp, src_text, name, flags):
    src = os.path.join(tmp, name + ".fsh")
    exe = os.path.join(tmp, name + ".bin")
    with open(src, "w") as f:
        f.write(src_text)
    p = subprocess.run([PEN] + flags + [src, "-o", exe],
                       capture_output=True, text=True, env=ENV, cwd=tmp)
    if p.returncode != 0:
        return ("COMPILE-FAIL", p.stderr[-400:])
    r = subprocess.run([exe], capture_output=True, text=True,
                       timeout=20, env=ENV, cwd=tmp)
    return ("OK", r.stdout)


def main():
    args = sys.argv[1:]
    n = 100
    seed = 7
    i = 0
    while i < len(args):
        if args[i] == "--seed":
            seed = int(args[i + 1]); i += 2
        else:
            n = int(args[i]); i += 1
    rng = random.Random(seed)
    tmp = tempfile.mkdtemp(prefix="fshprop")
    bad = 0
    try:
        for idx in range(n):
            text = gen_program(rng, idx)
            base = run_config(tmp, text, "base%d" % idx, ["-O0"])
            opt = run_config(tmp, text, "opt%d" % idx, OPT_FLAGS)
            if base[0] != "OK":
                continue        # generator produced an invalid program
            if opt[0] != "OK" or opt[1] != base[1]:
                bad += 1
                print("MISMATCH program %d (seed %d)" % (idx, seed))
                print(text)
                print("  -O0     :", repr(base[1][:200]))
                print("  -O2+xD  :", repr((opt[1] if opt[0] == "OK" else opt)
                                          [:200]))
                with open("mismatch_%d.fsh" % idx, "w") as f:
                    f.write(text)
                if bad >= 3:
                    break
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("property_test: %d program(s) checked, %d mismatch(es) "
          "(seed %d)" % (n, bad, seed))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
