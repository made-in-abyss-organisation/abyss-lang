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

🚧 **Phase 2 — Abyss runs.** A tree-walking interpreter executes programs:
variables and scopes, arithmetic with correct precedence, comparisons,
`&&`/`||`/`??`, `if`/`else`, **recursive functions**, string concatenation and
`${...}` interpolation, and a built-in `print`. The front-end (lexer + parser)
also handles the **`component`/`state`/`render` UI tree** — though UI rendering
itself waits for the mobile phase. Next: a type checker, then a native backend.
See [`docs/ROADMAP.md`](docs/ROADMAP.md) and [`docs/SPEC.md`](docs/SPEC.md).

## Build & run

```sh
make                              # build abyssc
./abyssc examples/run_demo.aby    # RUN a program (calls main)
./abyssc --ast examples/demo.aby  # print the AST instead
./abyssc --tokens examples/demo.aby   # print the raw token stream
make run                          # build + run the demo program
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
