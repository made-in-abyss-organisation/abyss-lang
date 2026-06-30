#!/usr/bin/env python3
"""Cross-platform test harness for Abyss.

The project's stated correctness gate (docs/KNOWN_LIMITATIONS.md) is that the
two execution paths agree: the tree-walking **interpreter** (`abyssc file`) and
the **native** backend (`abyssc --emit-c file | cc`). This harness enforces it.

For every backend-supported program it asserts the interpreter and the native
binary produce byte-identical stdout. Programs that are too slow to interpret
are built+run natively only (a build smoke test). Interpreter-only programs
(struct / match / component) are run through the interpreter and asserted to
exit cleanly. Exit code is non-zero on any failure, so CI fails loudly.

Usage:
    python tests/run_tests.py [path/to/abyssc]
"""
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = ".exe" if os.name == "nt" else ""

# Differential: interpreter output MUST equal native output. The C backend now
# covers struct + match (examples/features.aby), so it is checked here too.
DIFFERENTIAL = [
    "examples/hello.aby",
    "examples/run_demo.aby",
    "examples/features.aby",
    "examples/lists.aby",
    "examples/counter_app.aby",
    "bench/fib.aby",
]
# Native build+run only (too slow to interpret 100M iterations).
NATIVE_SMOKE = [
    "bench/loop.aby",
]
# Interpreter-only programs (none currently — the component/state/render
# runtime is now lowered to the C backend too, so the first app is checked
# differentially above).
INTERP_ONLY = []


def find_abyssc():
    if len(sys.argv) > 1:
        return os.path.abspath(sys.argv[1])
    for cand in (f"abyssc{EXE}", os.path.join("build", f"abyssc{EXE}")):
        p = os.path.join(ROOT, cand)
        if os.path.exists(p):
            return p
    sys.exit("error: abyssc not found — build it first or pass its path")


def find_cc():
    # Allow overriding the C compiler (may be multi-token, e.g. "zig cc").
    env = os.environ.get("ABYSS_CC")
    if env:
        return env.split()
    for c in ("clang", "cc", "gcc"):
        if shutil.which(c):
            return [c]
    sys.exit("error: no C compiler (clang/cc/gcc) on PATH (or set ABYSS_CC)")


ABYSSC = find_abyssc()
CC = find_cc()
BUILD = os.path.join(ROOT, "build")
os.makedirs(BUILD, exist_ok=True)


def run(cmd):
    return subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)


def interpret(path):
    r = run([ABYSSC, path])
    if r.returncode != 0:
        sys.exit(f"FAIL: interpreter errored on {path} (exit {r.returncode})\n{r.stderr}")
    return r.stdout


def native(path):
    name = os.path.splitext(os.path.basename(path))[0]
    cpath = os.path.join(BUILD, name + ".c")
    binp = os.path.join(BUILD, name + EXE)
    cg = run([ABYSSC, "--emit-c", path])
    if cg.returncode != 0:
        sys.exit(f"FAIL: --emit-c errored on {path}\n{cg.stderr}")
    with open(cpath, "w") as f:
        f.write(cg.stdout)
    cc = run(CC + ["-std=c11", "-O2", cpath, "-o", binp])
    if cc.returncode != 0:
        sys.exit(f"FAIL: C compile errored on {path}\n{cc.stderr}")
    r = run([binp])
    if r.returncode != 0:
        sys.exit(f"FAIL: native binary errored on {path} (exit {r.returncode})\n{r.stderr}")
    return r.stdout


def main():
    print(f"abyssc = {ABYSSC}")
    print(f"cc     = {' '.join(CC)}\n")
    failures = 0

    for path in DIFFERENTIAL:
        got_i = interpret(path)
        got_n = native(path)
        ok = got_i == got_n
        print(f"[{'PASS' if ok else 'FAIL'}] differential  {path}")
        if not ok:
            failures += 1
            print(f"       interpreter: {got_i!r}")
            print(f"       native:      {got_n!r}")

    for path in NATIVE_SMOKE:
        out = native(path).strip()
        print(f"[PASS] native smoke  {path}  -> {out[:48]!r}")

    for path in INTERP_ONLY:
        out = interpret(path)
        print(f"[PASS] interp-only   {path}  -> {len(out.splitlines())} line(s)")

    print()
    if failures:
        sys.exit(f"{failures} differential mismatch(es) — backends disagree")
    print("all tests passed")


if __name__ == "__main__":
    main()
