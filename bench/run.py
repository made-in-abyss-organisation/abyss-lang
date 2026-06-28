#!/usr/bin/env python3
"""Abyss benchmark harness.

Compares, on identical algorithms:
  - abyss-native : Abyss -> C (abyssc --emit-c) -> cc -O2
  - hand-C       : a human-written C baseline -> cc -O2  (speed-of-light)
  - dart-AOT     : dart compile exe              (the language we target)

Methodology (see docs/PERFORMANCE_AND_MOBILE_ROADMAP.md, Part 3):
  * same -O2 for all C; Dart via AOT (never `dart run`, which is JIT).
  * a correctness gate: every implementation must print the SAME output before
    any timing is trusted.
  * WARMUP runs discarded, then REPS timed runs; report median + min wall time.
  * benchmarks chosen to resist constant-folding/DCE (recursive fib; a
    data-dependent LCG loop whose result is printed), since Abyss has no argv
    to parameterize N. We verify wall time is non-trivial (work really happened).
"""
import os, subprocess, statistics, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
ABYSSC = os.path.join(ROOT, "abyssc")
WARMUP, REPS = 3, 15

BENCHMARKS = ["fib", "loop"]  # bench/<name>.aby / .dart / .c


def sh(cmd, **kw):
    return subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, **kw)


def build(name):
    os.makedirs(BUILD, exist_ok=True)
    impls = {}
    # abyss-native: emit C then compile
    cgen = sh([ABYSSC, "--emit-c", f"bench/{name}.aby"])
    if cgen.returncode != 0:
        print(f"  ! abyss emit-c failed for {name}: {cgen.stderr.strip()}")
    else:
        cpath = os.path.join(BUILD, f"{name}_abyss.c")
        open(cpath, "w").write(cgen.stdout)
        if cgen.stderr.strip():
            print(f"  ! {name}: {cgen.stderr.strip()}")
        r = sh(["cc", "-O2", cpath, "-o", os.path.join(BUILD, f"{name}_abyss")])
        if r.returncode == 0:
            impls["abyss-native"] = [os.path.join(BUILD, f"{name}_abyss")]
        else:
            print(f"  ! abyss cc failed for {name}: {r.stderr.strip()[:200]}")
    # hand-C baseline
    r = sh(["cc", "-O2", f"bench/{name}.c", "-o", os.path.join(BUILD, f"{name}_c")])
    if r.returncode == 0:
        impls["hand-C"] = [os.path.join(BUILD, f"{name}_c")]
    # dart AOT
    r = sh(["dart", "compile", "exe", f"bench/{name}.dart", "-o", os.path.join(BUILD, f"{name}_dart")])
    if r.returncode == 0:
        impls["dart-AOT"] = [os.path.join(BUILD, f"{name}_dart")]
    else:
        print(f"  ! dart compile failed for {name}: {r.stderr.strip()[:200]}")
    return impls


def time_cmd(cmd):
    t = time.perf_counter()
    subprocess.run(cmd, cwd=ROOT, capture_output=True)
    return time.perf_counter() - t


def main():
    print(f"Abyss benchmarks  (warmup={WARMUP}, reps={REPS}, best-of-N wall time)\n")
    results = {}
    for name in BENCHMARKS:
        print(f"## {name}")
        impls = build(name)
        # correctness gate: all impls must agree
        outputs = {k: sh(v).stdout.strip() for k, v in impls.items()}
        agree = len(set(outputs.values())) == 1
        gate = "OK (all equal: %s)" % next(iter(outputs.values())) if agree else "MISMATCH! " + str(outputs)
        print(f"  correctness gate: {gate}")
        row = {}
        for label, cmd in impls.items():
            for _ in range(WARMUP):
                time_cmd(cmd)
            samples = sorted(time_cmd(cmd) for _ in range(REPS))
            row[label] = (statistics.median(samples), min(samples))
        results[name] = row
        # report best-of-N (min): the run least perturbed by OS scheduling,
        # the standard robust metric for compute microbenchmarks.
        for label in ("abyss-native", "dart-AOT", "hand-C"):
            if label in row:
                med, mn = row[label]
                print(f"    {label:14s} best {mn*1000:8.1f} ms   (median {med*1000:.1f} ms)")
        if "abyss-native" in row and "dart-AOT" in row:
            ratio = row["dart-AOT"][1] / row["abyss-native"][1]   # by best time
            tag = f"{ratio:.2f}x faster than" if ratio >= 1 else f"{1/ratio:.2f}x slower than"
            print(f"    => abyss-native is {tag} Dart-AOT")
        print()
    return results


if __name__ == "__main__":
    main()
