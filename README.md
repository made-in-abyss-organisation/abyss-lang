# Abyss 🕳️

**A native-fast programming language built for mobile development.**

Abyss compiles ahead-of-time to native machine code, manages memory with
Automatic Reference Counting (no stop-the-world GC pauses), and treats the
**UI as a first-class language construct** — not a library bolted on top.

> Design thesis: *a garbage collector is a UI's smoothness ceiling. Abyss
> removes the GC and keeps the ergonomics.*

```rust
component Counter {
    state count: Int = 0

    fn increment() { count += 1 }

    render {
        Column(spacing: 12) {
            Text("Count: ${count}").size(24).bold()
            Button("Increment", on_tap: increment)
        }
    }
}
```

## Pillars

1. **Native-fast** — AOT compiled, no interpreter shipped on device.
2. **UI is first-class** — reactive `component` / `render` built into the grammar.
3. **Predictable performance** — ARC memory management, no GC stutter.
4. **Safe by default** — null safety and immutability by default.
5. **Ergonomic** — type inference, clean newline-terminated syntax.

## Status

🚧 **Phase 5 — Native types, benchmarked vs Dart.** The backend now emits
**native C scalar types** (`long long`/`double`/`int`) instead of boxed values
wherever the type checker proved a concrete type, with a single box/unbox
chokepoint at the boundaries. Result on identical algorithms (`make bench`,
clang 21 vs Dart 3.11.1 AOT, i7-9750H): Abyss-native **matches hand-written C**
and **beats Dart-AOT** — ~2.3× on `fib(35)`, ~1.1× on a 100M-iteration integer
loop. (Honest scope: this is throughput *parity with native C* on numeric code,
not a universal win; the durable edge over Dart is meant to be ARC/no-GC
frame-time, a later phase. See [`bench/BENCHMARKS.md`](bench/BENCHMARKS.md).)

Pipeline: **lex → parse → type-check (annotates types) → (interpret | emit C → cc → native)**.
Static checks catch `Int + String`, wrong arg/field counts, non-`Bool`
conditions, unknown fields, return mismatches, and undefined names (with line
numbers; `--no-check` skips). Language: variables/scopes, arithmetic,
`&&`/`||`/`??`, `if`/`else`, `while` and `for`/range loops, recursion,
`struct`s, `match`, string concat and `${...}` interpolation, `print`. The
front-end also parses the `component`/`state`/`render` UI tree. (The C backend
now covers everything the interpreter runs — functions, control flow,
arithmetic, strings, `print`, `struct`s and `match` — kept honest by a
differential test harness that builds on macOS, Linux **and Windows** and
asserts the interpreter and native binary print identical output.) See
[`docs/ROADMAP.md`](docs/ROADMAP.md) and [`docs/SPEC.md`](docs/SPEC.md).

## Build & run

**macOS / Linux** (with `make`):

```sh
make                              # build abyssc
./abyssc examples/run_demo.aby    # RUN a program via the interpreter
make native                       # transpile run_demo to C, compile, run natively
./abyssc --emit-c examples/run_demo.aby   # print the generated C
./abyssc --ast examples/demo.aby  # print the AST
./abyssc --tokens examples/demo.aby       # print the token stream
```

No `make`? Use the helper scripts:

```sh
./scripts/build.sh                # macOS / Linux  -> ./abyssc
```

**Windows** (PowerShell, needs LLVM/clang — `winget install LLVM.LLVM`):

```powershell
pwsh scripts/build.ps1            # -> .\abyssc.exe
.\abyssc.exe examples\run_demo.aby
```

Running `run_demo.aby` prints:

```
factorial(5) = 120
fib(10)      = 55
2 + 3 * 4    = 14
greeting     = hello, Abyss
7 is odd
```

Requires a C11 compiler. On macOS/Linux any of `cc`/`clang`/`gcc` works; on
Windows use **clang** (the C the backend emits uses GNU statement-expressions
and `_Generic`, which MSVC does not accept).

## CI & releases

- **CI** ([`.github/workflows/ci.yml`](.github/workflows/ci.yml)) builds `abyssc`
  on **Linux, macOS, and Windows** on every push/PR and runs the differential
  test harness ([`tests/run_tests.py`](tests/run_tests.py)) — every
  backend-supported program must produce identical output through the
  interpreter and the native binary.
- **Releases** ([`.github/workflows/release.yml`](.github/workflows/release.yml))
  are cut by pushing a tag. Each tag builds and packages prebuilt `abyssc`
  binaries — **macOS (universal x86_64 + arm64), Windows (x86_64), Linux
  (x86_64)** — and attaches them to a GitHub Release:

  ```sh
  git tag v0.5.0 && git push origin v0.5.0
  ```

## Layout

```
src/        compiler source (C)
  token.h   token definitions
  lexer.*   the lexer (Phase 1)
  main.c    abyssc entry point
examples/   sample .aby programs
bench/      benchmark suite (vs hand-C and Dart AOT)
tests/      cross-platform differential test harness
scripts/    no-make build helpers (build.sh / build.ps1)
.github/    CI + release pipelines (Linux/macOS/Windows)
docs/       language spec & roadmap
```

## License

Apache License 2.0 — see [LICENSE](LICENSE).
