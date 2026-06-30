# Known Limitations

Tracked honestly. The two execution paths — the **interpreter** (`abyssc file`)
and the **native** backend (`abyssc --emit-c file | cc`) — are differentially
fuzzed (see the adversarial test pass); they agree on all tested programs
*except* the cases below.

## Backend coverage

- The C backend compiles functions, control flow, arithmetic, comparisons,
  `&&`/`||`/`??`, strings + `${}` interpolation, `print`, **`struct`s**
  (construction, field read/write, and printing), **`match`** (literal,
  binding, and wildcard patterns) and **`List`s** (literals, indexing,
  element-assignment, `len`/`push`, `for`-iteration, and nesting). It covers
  everything the interpreter runs; the differential harness
  (`tests/run_tests.py`) checks that interpreter and native output match,
  including `examples/features.aby` and `examples/lists.aby`.
- **List element types are dynamic** in this release: list elements are stored
  boxed (the tagged `AV` value) in both backends, so `xs[i]` is typed `Any` and
  not yet a native scalar. Homogeneous unboxed lists (and `Map`) are a later
  Phase-5b step. This is a performance note, not a correctness divergence — the
  two backends still produce identical output.
- UI constructs (`component` / `state` / `render`) now **execute in both
  backends**: a component is mounted by calling it (`Counter()`), `render(app)`
  walks the `render` tree against the instance's live `state`, and invoking a
  method (`app.increment()`) mutates state so the next `render` reflects it
  (`examples/counter_app.aby`). The native backend lowers components to a C
  struct-object instance, per-method functions, a mount function, and a render
  function, with runtime dispatch by type name; the interpreter and native
  binary print byte-identical frames (checked in the differential suite). The
  one remaining caveat: the render target is a **headless text tree**, not a
  graphics surface — binding it to Skia/Canvas is Phase 6 proper.
- **Component-backend edge cases (native).** A component method or `render`
  resolves a bare name to a `state` field whenever one exists, so a local
  variable or parameter that *shadows* a state field is not yet handled in the
  native backend (the interpreter resolves it correctly via scope). None of the
  current examples shadow state; this is a known gap, not a silent divergence.

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

## Android (native execution)

- An Abyss program runs on the **Android emulator** today as a native *console*
  binary: `scripts/android_run.ps1` / `scripts/android_run.sh` emit C, cross-
  compile it with the NDK clang for the device's ABI, push it with `adb`, and
  run it. `examples/counter_app.aby` produces output byte-identical to the
  interpreter on an x86_64 / API 36 emulator.
- Two things the runner must get right (and does):
  - **ABI match** — it reads `ro.product.cpu.abi`; a desktop emulator is usually
    `x86_64`, a phone `arm64-v8a`. Running the wrong-ABI binary fails in the
    translation loader (`missing DT_SYMTAB`).
  - **16 KB pages** — recent Android images use a 16 KB page size, so the binary
    is linked with `-Wl,-z,max-page-size=16384`; without it the loader SIGSEGVs.
- Not yet: a **graphical** surface (the render target is still a text widget
  tree — Skia binding is Phase 6 proper), an **APK** wrapper / `NativeActivity`,
  and **iOS**. So this is "Abyss native code executing on Android," not yet a
  store-installable GUI app.

## Notes

- The native `??` lowering uses a GCC/Clang statement-expression; the generated
  C therefore assumes one of those compilers (the project already uses `cc`).
- No ARC / garbage collection yet: heap strings from concatenation are not freed
  (acceptable for short-lived programs; ARC is a later roadmap phase).
