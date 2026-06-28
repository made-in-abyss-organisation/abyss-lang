# Abyss Roadmap

Milestone-driven, not date-driven. Each phase ends with something that
**runs**, so progress is always provable. Guiding order: *make it run before
you make it fast; make it fast before you make it pretty.*

| Phase | Name | Deliverable |
|------:|------|-------------|
| 0 | **Specification** | Grammar + semantics written down (`docs/SPEC.md`). |
| 1 | **Front-end (Lexer + Parser)** | `abyssc` reads `.aby` → tokens → AST. ← *lexer + core parser done; UI-tree/struct/match next* |
| 2 | **Tree-walking interpreter** | Programs actually run: vars, arithmetic, recursion, `if`, `print`, string interpolation. ← *done (match/structs pending parser)* |
| 3 | **Type checker** | Static analysis: type inference, null-safety, immutability — bad programs rejected with clear errors. |
| 4 | **Backend (transpile to C)** | Emit C, compile with clang → native ARM executable. |
| 5 | **Runtime + stdlib** | ARC memory management, `String`/`List`/`Map`, async runtime. |
| 6 | **Mobile layer** | Bind Skia; widget framework; `state` → re-render. `Counter` runs on a real Android device. |
| 7 | **Tooling** | Hot reload (<1s), error reporting, package manager, LSP. |
| 8 | **Platform & packaging** | Android (APK) first, then iOS (IPA, requires a Mac). |
| ∞ | **Self-hosting** | Rewrite `abyssc` in Abyss itself. |

### Sequencing insight

Phases 1–5 produce a **complete, fast, general-purpose language that runs on
a laptop** — valuable on its own. The mobile-specific mountain (6–8) only
begins *after* the language works. If the framework ever proves too big, the
language still stands. That de-risks the whole journey.

```
RUN IT        → TYPE IT    → SPEED IT    → DRAW IT     → SHIP IT
P1-2 frontend   P3 checker   P4-5 native   P6-7 mobile   P8 stores
+ interpreter                + ARC         + tooling
```
