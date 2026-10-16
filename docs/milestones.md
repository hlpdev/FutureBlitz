# Milestones

The language, compiler, and tooling come first and must be complete before engine work
begins. The runtime is built thread-aware from M2 (separate atomic and non-atomic
refcounts), so concurrency doesn't require a rewrite later.

Timeline: honors project begins **fall 2027**, so there is plenty of runway.

## Phase 1: Language and toolchain

| | Goal | Done when |
|---|---|---|
| **M0** | Setup: repo, CMake, configs, CI | builds on Windows, Linux, macOS |
| **M1** | Core: primitives, functions, control flow, strings | `Print("hello")` compiles to a native exe |
| **M2** | Data: structs, classes, refcounting, arrays, errors, `Defer` | test programs run with no leaks |
| **M3** | Generics, interfaces, `Extend`, compile-time functions, reflection | `std.collections` written in FutureBlitz |
| **M4** | Concurrency: Sendable checks, tasks, `Parallel For`, async/coroutines, channels, Mutex/Atomic | a parallel job and an async chain run correctly |
| **M5** | Modules, packages, `project.toml`, FFI, bindgen | calls a C library through generated bindings |
| **M6** | Full standard library | `std.*` complete, with tests |
| **M7** | Tooling: LSP, VS Code extension, formatter, test runner, package manager | daily-driving FutureBlitz in an editor feels good |

## Phase 2: Engine

| | Goal |
|---|---|
| **M8** | Engine bootstrap: window, input, frame loop, main scheduler |
| **M9** | 2D: images, drawing, fonts |
| **M10** | 3D scene: entities, cameras, lights, hierarchy |
| **M11** | Vulkan renderer and materials |
| **M12** | Asset pipeline: VFS, cooker, pak files, typed references, hot reload |
| **M13** | Audio and physics (miniaudio, Jolt) |
| **M14** | The IDE |
| **M15** | Migrator, plus porting a real Blitz3D game as the final proof |

## M1 build order

| Step | What | Why this order |
|---|---|---|
| 1 | Source and diagnostics: `source_file_t`, `span_t`, `diagnostic_t` | every later stage reports errors through this; the LSP depends on exact spans |
| 2 | Tokens and lexer, with tests | first real stage |
| 3 | AST and parser, with error recovery | recovery must be designed in from the start for the LSP |
| 4 | Semantic analysis for the M1 subset | name resolution and type checking |
| 5 | Codegen (LLVM gets linked here) | ends with a native "hello world" |
| 6 | `cli` wired up as `fb`, including `--show` to print each stage (tokens, AST, types, MIR, LLVM IR) in color | `fb build hello.fb` works end to end |

**The M1 subset** that step 4 checks: `Bool`, the sized integers and their aliases,
`Float32`/`Float64`, and `String`, with `As` between numbers. Until `std.core` exists, the
compiler provides a temporary built-in `Print(String)`. `ByRef` parameters are in. Strings
support `+`, `==`, `!=`, and interpolation; ordering them (`<`) waits for `Comparable` in M3.
`T?`, arrays, math types, `Is`/`As?`, `For ... In`, and classes report "not supported yet"
until M2.

## M2 build order

Every step ends with programs in tests/programs that run with no leaks.

| Step | What | Why this order |
|---|---|---|
| 1 | Type table: types become entries in a table instead of a fixed enum; the built-ins keep their ids | user types, `T?`, and `T[]` need somewhere to live; the M1 tests check the refactor |
| 2 | Structs: fields with defaults, the generated constructor, field access and assignment, copies, field-wise `==`, methods, `ByRef Method`, Strings inside structs | value types first: no heap, no refcount changes |
| 3 | Classes and the object runtime: object header (refcount, type info), `New`, fields, methods, `Destroy()`, `RefEquals`, retain/release for every reference type, leaks reported by class | refcounting is the heart of M2 |
| 4 | Nullability: `T?`, `Null`, `?.`, `??`, `T` → `T?`, narrowing after a null check; non-nullable class fields assigned by every constructor | classes need it to point at nothing |
| 5 | Inheritance: `Extends`, `Override` and virtual dispatch, `Super`, `Abstract`, `Final`, `Is`, `As`, `As?`, `Case e As T` | builds on classes and `T?` |
| 6 | Arrays: `T[]` (literals, bounds-checked indexing, `Count`, `Add`, `Insert`, `RemoveAt`, `Clear`, `Contains`, `Clone`), `For ... In`, fixed `T[N]` and `T[W, H]` | the first container; needs every kind of element |
| 7 | Enums: underlying type, automatic values, `@Flags`, `As` to and from integers, exhaustive `Select` | small, and `Select` is already there |
| 8 | `Defer`: runs at scope exit in reverse order, through `Return`, `Break`, and `Continue` | `Finally` reuses it |
| 9 | Errors: built-in `Error` class, `Throws`, `Throw`, `Try`/`Catch`/`Finally`, `Try` and `Try?` prefixes, lowered as return values; `Panic()` and `Assert()` | needs classes, `T?`, and `Defer` |
| 10 | Weak references and leak reports: `Weak<T>`; debug builds report objects left at exit by class and point out cycles among them | M2's "no leaks" needs a way to break and find cycles |
| 11 | Class extras: `Static` fields and methods, properties, `ReadOnly` | conveniences on top of finished classes |

**Not in M2:** interfaces, `Extend`, operator overloading, tuples, lambdas and closures, slices,
variadic parameters, and `.Chars`/`.Bytes` come in M3. Visibility modifiers come with modules in
M5. `Shared Class` and atomics come in M4.

## Process

- **Proposed:** track milestones M0–M15 as GitHub milestones, with issues per task.
- Every feature lands with tests: Catch2 unit tests, plus golden tests once they're decided
  (see [open-questions.md](open-questions.md)).
