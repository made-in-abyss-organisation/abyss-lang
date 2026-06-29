# Abyss Roadmap

Milestone-driven, not date-driven. Each phase ends with something that
**runs**, so progress is always provable. Guiding order: *make it run before
you make it fast; make it fast before you make it pretty.*

| Phase | Name | Deliverable |
|------:|------|-------------|
| 0 | **Specification** | Grammar + semantics written down (`docs/SPEC.md`). |
| 1 | **Front-end (Lexer + Parser)** | `abyssc` reads `.aby` → tokens → AST. ← *lexer + core parser done; UI-tree/struct/match next* |
| 2 | **Tree-walking interpreter** | Programs run: vars, arithmetic, recursion, `if`, `while`, `for`/ranges, `struct`, `match`, `print`, string interpolation. ← *done* |
| 3 | **Type checker** | Static analysis: type inference, arg/field/return checks, Bool conditions — bad programs rejected with clear errors. ← *done (null-safety/immutability still TODO)* |
| 4 | **Backend (transpile to C)** | Emit C, compile with cc/clang → native executable. ← *done: `abyssc --emit-c` + `make native`; ~370× faster than the interpreter on fib(32)* |
| 5 | **Native types + benchmarks** | Type-directed codegen: emit native `long long`/`double`/`int` (not boxed `AV`) where types are known; benchmark suite vs Dart AOT. ← *done: matches hand-C, beats Dart-AOT ~2.3× on fib / ~1.1× on the integer loop (`make bench`)* |
| 5b | **Runtime + stdlib** | ARC memory management, `String`/`List`/`Map`, async runtime. |
| 6 | **Mobile layer** | Bind Skia; widget framework; `state` → re-render. `Counter` runs on a real Android device. |
| 7 | **Tooling** | Hot reload (<1s), error reporting, package manager, LSP. ← *CI on Linux/macOS/Windows + differential test harness done (`.github/workflows/ci.yml`, `tests/run_tests.py`); hot reload / LSP / package manager still TODO* |
| 8 | **Platform & packaging** | Android (APK) first, then iOS (IPA, requires a Mac). ← *desktop delivery pipeline done: tagged releases build prebuilt `abyssc` binaries for macOS (universal), Windows (x64) & Linux (x64) (`.github/workflows/release.yml`); mobile APK/IPA still TODO* |
| ∞ | **Self-hosting** | Rewrite `abyssc` in Abyss itself. |

### Delivery (desktop)

The compiler now builds and ships cross-platform. The backend was made portable
(the POSIX-only `open_memstream` was removed in favour of a type-prediction
codegen path), so `abyssc` compiles cleanly on macOS, Linux, and **Windows**
(via clang). CI proves the interpreter and native backends agree on all
supported programs across all three OSes on every push; pushing a `v*` tag cuts
a GitHub Release with prebuilt binaries for each. Mobile packaging (Phase 8's
APK/IPA) is a separate, later mountain — see
[`PERFORMANCE_AND_MOBILE_ROADMAP.md`](PERFORMANCE_AND_MOBILE_ROADMAP.md).

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
