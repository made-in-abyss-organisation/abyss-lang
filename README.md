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

🚧 **Phase 1 — Front-end.** The **lexer** and a **parser** are working:
`abyssc` parses declarations, functions, control flow, and full
expression-precedence into an AST. UI-tree parsing (`component`/`render`) and
`struct`/`match`/`for` are the next parser increment. See
[`docs/ROADMAP.md`](docs/ROADMAP.md) for the plan and
[`docs/SPEC.md`](docs/SPEC.md) for the grammar.

## Build & run

```sh
make                          # build the abyssc compiler
./abyssc examples/demo.aby    # parse and print the AST
./abyssc --tokens examples/demo.aby   # print the raw token stream
make run                      # build + parse the demo example
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
