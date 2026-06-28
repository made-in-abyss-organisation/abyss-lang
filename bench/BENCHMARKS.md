# Abyss Benchmarks

Reproducible comparison of **Abyss-native** (Abyss → C via `abyssc --emit-c`,
compiled with `cc -O2`) against a **hand-written C** baseline (the
speed-of-light ceiling) and **Dart AOT** (`dart compile exe`), the language
Abyss is measured against.

Run it yourself: `make bench` (or `python3 bench/run.py`).

## Method (honest by construction)

- **Same optimisation** for all C (`-O2`); Dart via **AOT**, never `dart run`
  (JIT would be an unfair comparison against a static native binary).
- **Correctness gate**: the harness refuses to trust any timing unless every
  implementation prints the **same** result. (It does.)
- **Best-of-N** wall time (warmup runs discarded), the run least perturbed by
  OS scheduling — the standard robust metric for compute microbenchmarks.
- Identical algorithms, hand-translated into each language (`bench/*.aby`,
  `*.dart`, `*.c`). The benchmarks resist constant-folding/DCE (recursive
  `fib`; a data-dependent LCG loop whose result is printed), so the work
  genuinely happens — verified: the binaries take real, non-trivial time.

## Results

Environment: Intel i7-9750H, macOS, Apple clang 21, Dart SDK 3.11.1 (AOT),
best of 15 runs.

| Benchmark        | abyss-native | hand-C | Dart-AOT | abyss vs Dart |
|------------------|-------------:|-------:|---------:|--------------:|
| `fib(35)`        |      ~36 ms  | ~26 ms |  ~84 ms  | **~2.3× faster** |
| `loop` (100M LCG)|     ~366 ms  | ~368 ms| ~411 ms  | **~1.1× faster** |

(Numbers vary run-to-run on a loaded laptop; the relationships are stable.)

## What this shows — and what it does NOT

**Shows:** because Abyss transpiles to C and the backend emits **native scalar
types** (not boxed values) for code whose types are known, hot numeric Abyss
lowers to essentially the same machine code a C programmer would write. So it
**matches hand-C and beats Dart-AOT** on these compute kernels — `fib` by a
wide margin (Dart's function calls carry more overhead), the integer loop by a
hair (both are near the C ceiling).

**Does NOT show "Abyss beats Dart at everything."** This is throughput parity
with native C on monomorphic numeric code, which is the honest ceiling for a
transpile-to-C backend. Dart's AOT is genuinely good; on float/SIMD-friendly
kernels it can match or edge ahead. Per the project's performance thesis
(`docs/PERFORMANCE_AND_MOBILE_ROADMAP.md`), the *durable* advantage over Dart is
intended to be **frame-time smoothness from ARC instead of a tracing GC** — a
later phase — not raw throughput.

## A note on boxing and the optimiser

The backend used to box every value in a tagged union (`AV`). Measured at `-O0`
that boxing costs ~13× on `fib` (e.g. ~1.0s vs ~0.08s for hand-C), but at `-O2`
clang's scalar-replacement already eliminated it for these monomorphic cases —
so boxed and native ran the same at `-O2`. Phase 5's native-type lowering makes
that speed **robust** (not dependent on the optimiser scalarising `AV`) and is
the prerequisite for native struct fields and monomorphization later. The
codegen "grep gate" (no `av_*` calls in a hot native function) confirms the
boxing is actually gone, e.g. `af_fib` emits `if ((a_n < 2))` and
`return (af_fib((a_n - 1)) + af_fib((a_n - 2)));`.
