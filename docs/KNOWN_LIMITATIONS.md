# Known Limitations

Tracked honestly. The two execution paths — the **interpreter** (`abyssc file`)
and the **native** backend (`abyssc --emit-c file | cc`) — are differentially
fuzzed (see the adversarial test pass); they agree on all tested programs
*except* the cases below.

## Backend coverage

- The C backend compiles functions, control flow, arithmetic, comparisons,
  `&&`/`||`/`??`, strings + `${}` interpolation, `print`, **`struct`s**
  (construction, field read/write, and printing) and **`match`** (literal,
  binding, and wildcard patterns). It now covers everything the interpreter
  runs; the differential harness (`tests/run_tests.py`) checks that interpreter
  and native output match, including `examples/features.aby`.
- UI constructs (`component` / `state` / `render`) parse but have no runtime in
  either backend yet (a later roadmap phase), so they do not execute to output.

## Known divergences (interpreter vs native)

- **Out-of-range integer literals (≥ 2⁶³).** The interpreter parses with
  `strtoll`, which saturates to `LLONG_MAX`; the native backend emits the
  literal text verbatim, so C wraps it. Both behaviours are arguably wrong;
  literal range-checking at parse time is the proper fix (not yet done). In-range
  arithmetic overflow wraps identically in both.
- **Very deep recursion (thousands of frames).** The tree-walking interpreter
  recurses on the C stack with no depth guard and can `SIGSEGV`; the native
  binary (real C stack frames) handles far deeper recursion correctly. For
  deeply recursive programs, prefer the native backend.

## Fixed (previously divergent)

- `??` now short-circuits in the native backend (evaluates the right operand
  only when the left is nil), matching the interpreter.
- Integer division/modulo by zero now raises the same clean runtime error
  (exit 70) in both backends instead of C undefined behaviour.

## Notes

- The native `??` lowering uses a GCC/Clang statement-expression; the generated
  C therefore assumes one of those compilers (the project already uses `cc`).
- No ARC / garbage collection yet: heap strings from concatenation are not freed
  (acceptable for short-lived programs; ARC is a later roadmap phase).
