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

🚧 **Phase 4 — Compiles to native code.** Abyss now **transpiles to C and
compiles to a native binary** (`abyssc --emit-c file.aby | cc`), on top of a
working interpreter and static type checker. The compiled binary runs **~370×
faster than the interpreter** on `fib(32)` and produces identical output — the
first concrete step toward the "faster than Dart" goal.

Pipeline: **lex → parse → type-check → (interpret | emit C → cc → native)**.
Static checks catch `Int + String`, wrong arg/field counts, non-`Bool`
conditions, unknown fields, return mismatches, and undefined names (with line
numbers; `--no-check` skips). Language: variables/scopes, arithmetic,
`&&`/`||`/`??`, `if`/`else`, `while` and `for`/range loops, recursion,
`struct`s, `match`, string concat and `${...}` interpolation, `print`. The
front-end also parses the `component`/`state`/`render` UI tree. (The C backend
covers functions, control flow, arithmetic, strings and `print`; `struct`/`match`
run in the interpreter and are next for the backend.) See
[`docs/ROADMAP.md`](docs/ROADMAP.md) and [`docs/SPEC.md`](docs/SPEC.md).

## Build & run

```sh
make                              # build abyssc
./abyssc examples/run_demo.aby    # RUN a program via the interpreter
make native                       # transpile run_demo to C, compile, run natively
./abyssc --emit-c examples/run_demo.aby   # print the generated C
./abyssc --ast examples/demo.aby  # print the AST
./abyssc --tokens examples/demo.aby       # print the token stream
```

Running `run_demo.aby` prints:

```
factorial(5) = 120
fib(10)      = 55
2 + 3 * 4    = 14
greeting     = hello, Abyss
7 is odd
```

Requires a C11 compiler (`cc`/`clang`/`gcc`) and `make`.

## Layout

```
src/        compiler source (C)
  token.h   token definitions
  lexer.*   the lexer (Phase 1)
  main.c    abyssc entry point
examples/   sample .aby programs
docs/       language spec & roadmap
```

## License

Apache License 2.0 — see [LICENSE](LICENSE).
