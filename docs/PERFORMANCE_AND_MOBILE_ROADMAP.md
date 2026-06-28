# Abyss — Performance & Cross-Platform Mobile Roadmap

Two goals, treated as engineering targets, not slogans:

1. **Be faster than Dart** — on measured axes, proven by a benchmark suite.
2. **Be ready for cross-platform mobile** — one codebase → Android + iOS.

---

## Part 0 — What "faster than Dart" actually means

Dart is **already AOT-compiled to native** and close to the metal on raw
throughput. You will *not* win by being vaguely "faster." You win on specific
axes, and you must **measure each one against Dart AOT**:

| Axis | What it measures | Abyss strategy | Realistic verdict |
|------|------------------|----------------|-------------------|
| **Frame-time jank** (p99) | Stutter during animation | **ARC, no tracing GC** → no stop-the-world pauses | **Abyss should win** — this is the headline |
| **Cold start** | Time to first frame | Small runtime, AOT, no GC heap warmup | Abyss can win |
| **Memory (RSS)** | Footprint at rest | ARC frees eagerly; value types avoid heap | Abyss can win |
| **Throughput** | Compute-heavy loops | LLVM backend, monomorphized generics, value types | Match / slightly beat |
| **Binary size** | App download size | Minimal runtime, dead-code elimination | Comparable |

> The thesis in one line: **Dart trades frame smoothness for GC convenience.
> Abyss keeps the convenience (ARC is automatic too) without the GC tax.**

**Rule: no "faster than Dart" claim ships without a reproducible benchmark
backing that exact axis.** See Part 3.

---

## Part 1 — Performance Track

Layered so each phase produces a *measurable* speedup. Order matters: correct
first, then fast.

### P-1. Baseline: get it running and measurable
- Finish interpreter (Roadmap Phase 2) so programs run at all.
- Stand up the **benchmark harness vs Dart** (Part 3) early — you cannot
  optimize what you cannot measure.
- 📏 *Gate: every benchmark runs on both Abyss and Dart, numbers recorded.*

### P-2. Native backend via LLVM (the throughput foundation)
- Transpile to **C first** (fast to build, inherits clang/LLVM optimization),
  then graduate to **emitting LLVM IR directly** for full control.
- Enable LLVM opt passes: inlining, constant folding, loop optimization.
- 📏 *Gate: compute benchmarks (mandelbrot, binary-trees, fib) within 10% of
  Dart AOT.*

### P-3. ARC — the headline win over Dart
- Compiler inserts **retain/release** automatically (no manual memory, no GC).
- **Cycle handling**: `weak` references; later, optional cycle detection.
- **This is where you beat Dart on jank**: deterministic frees, zero
  stop-the-world pauses → flat frame times.
- 📏 *Gate: a 120fps animation loop shows **lower p99 frame time** than the
  equivalent Dart app. This is THE proof point.*

### P-4. ARC optimization (so ARC isn't the new bottleneck)
- Naive ARC is slow (retain/release on every assignment). Fix it like Swift:
  - **Escape analysis** — elide retain/release when a value provably doesn't
    escape.
  - **Ownership/borrow hints** — pass by borrow, no refcount churn.
  - **Retain/release elision & coalescing** across a function.
- 📏 *Gate: ARC overhead < 5% vs a manual-memory version of the same hot loop.*

### P-5. Value types & memory layout
- `struct` is a **value type** (stack-allocated, copied) by default — avoids
  heap allocation Dart often pays.
- Control over layout (packed fields), arrays of structs (cache-friendly),
  no per-element boxing.
- 📏 *Gate: struct-heavy benchmark beats Dart on both time and allocations.*

### P-6. Monomorphized generics & static dispatch
- Generics **specialized at compile time** (like Rust/C++), not type-erased →
  no boxing, inlinable.
- **Static dispatch by default**; devirtualize where the type is known →
  no vtable lookup.
- 📏 *Gate: generic container benchmark matches hand-written specialized code.*

### P-7. Startup & runtime diet
- Minimal runtime, lazy initialization, AOT means no JIT warmup.
- Dead-code elimination / tree-shaking at link time.
- 📏 *Gate: minimal-app cold start beats Dart's; binary size comparable.*

```
THROUGHPUT path:   LLVM backend → value types → monomorphization
SMOOTHNESS path:   ARC → ARC optimization        ← the real Dart-killer
STARTUP path:      runtime diet → tree-shaking
```

---

## Part 2 — Cross-Platform Mobile Track

A language that runs on a laptop is not a mobile framework. These layers turn
Abyss into "Flutter, but Abyss." Reuse proven engines; build only the glue.

### M-1. Rendering engine — bind Skia / Impeller (don't build one)
- Bind to **Skia** (or Impeller) via its C/C++ API. This is the single
  rendering engine that draws every pixel → **identical look on iOS & Android**
  (Flutter's exact strategy).
- 📏 *Gate: draw a rectangle + text on a GPU surface from Abyss.*

### M-2. Widget framework + layout engine
- Core widgets: `Text`, `Column`, `Row`, `Button`, `Stack`, `Image`.
- **Layout engine** (flexbox-style constraints) — measure & position.
- Wire the `component` / `state` / `render` grammar to a **reactive
  reconciler**: when `state` changes, diff the widget tree, repaint only what
  changed.
- 📏 *Gate: the `Counter` example renders and updates on screen (desktop window
  first — cheapest target).*

### M-3. Platform integration layer
- **Event loop** integrated with the OS main thread (touch, lifecycle).
- **FFI / platform channels** to native APIs: camera, GPS, sensors,
  notifications, filesystem, biometrics.
- 📏 *Gate: a tap event reaches Abyss; a native API (e.g. vibration) fires.*

### M-4. Android target first (cheap toolchain)
- Compile Abyss → native ARM via the **Android NDK**; package as **APK/AAB**.
- Embed Skia + the Abyss runtime in the app.
- 📏 *Gate: `Counter` runs on a real Android device. **Abyss is now a mobile
  language.***

### M-5. iOS target (the Mac tax)
- Compile for ARM64 iOS; package **IPA**; code signing. **Requires macOS +
  Xcode toolchain** — plan for this hardware cost (same wall every mobile
  framework hits).
- 📏 *Gate: the same `Counter` source runs on an iPhone, unchanged.*

### M-6. Developer loop — hot reload
- File watch → recompile changed code → patch the running app, preserving
  `state`. The single feature that makes daily dev bearable.
- 📏 *Gate: edit `.aby`, see the device update in < 1s.*

### M-7. Distribution
- `abyss build android` / `abyss build ios`, package manager for libraries,
  store-submission guidance.

```
Skia bind → widgets+layout → reactive runtime → platform glue
   → Android (APK) → iOS (IPA) → hot reload → ship
```

---

## Part 3 — The Benchmark Suite (how you PROVE "faster than Dart")

Without this, "faster than Dart" is marketing. Build it early, run it in CI,
publish the numbers.

**Setup:** identical algorithms in Abyss and Dart, same device, release/AOT
builds, many iterations, report median + p99.

**Compute (throughput):**
- `binary_trees`, `mandelbrot`, `fannkuch`, `nbody`, `fib(35)`
- JSON parse/serialize of a large payload

**The headline (jank / frame consistency):**
- A render loop pushing 10k animated nodes at 120fps target.
- Record **per-frame time histogram**; the win is **lower p99 and zero
  multi-ms pauses** vs Dart's GC spikes. *This single chart is your strongest
  evidence.*

**Startup & footprint:**
- Cold start of a minimal app (time to first frame).
- RSS at idle; APK/IPA size.

**Discipline:**
- Numbers committed to the repo per release.
- A regression in any axis blocks the release.
- Never claim an axis you haven't measured on real hardware.

---

## Honest risk register

- **Beating Dart on throughput is not guaranteed** — Dart AOT is good. Your
  *reliable* win is **frame smoothness via ARC**, not raw compute. Lead with that.
- **ARC has its own cost** — without P-4 optimization, ARC can be *slower*
  than GC on allocation-heavy code. The optimization work is mandatory, not optional.
- **iOS needs Apple hardware** — a fixed cost, not an engineering choice.
- **The mobile track is bigger than the language track.** Reusing Skia is what
  makes it survivable; building a renderer would not be.
```
