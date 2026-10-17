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

## M3 build order

Every step ends with programs in tests/programs that run with no leaks, as in M2.

| Step | What | Why this order |
|---|---|---|
| 1 | Programs of several files: the main file plus library files the compiler adds from std/, sharing one namespace; spans, diagnostics, analysis, lowering, and panic locations work per file; only the main file's top-level code runs | `std.collections` has to live in FutureBlitz files before modules exist; a refactor first, like M2's type table |
| 2 | Tuples: `(A, B)` types and values, multiple returns, and destructuring like `Local lo, hi = MinMax(xs)` | a value type like a Struct without a name; small, and Maps hand out pairs |
| 3 | Function types and lambdas: `Func(A, B): R`, `Function(x) => expr` and block lambdas, closures that capture by reference, `[Weak x]` capture lists, and the warning for a closure stored in a field that captures `Self` strongly | collections' `Filter`, `Map`, `Find`, and `Sort` take functions; a closure is a refcounted object, which M2 built |
| 4 | Generics: type parameters on functions, Methods, Structs, and Classes; explicit type arguments and inference; `Name<...>` disambiguation and `>>`; monomorphization, one copy per set of type arguments | the core of M3, which everything after it uses |
| 5 | Operator overloading: `Operator +(a: Money, b: Money): Money` and the other operators on Structs and Classes, `==` and `!=` among them | interfaces like `Numeric` require operators, so operators come first |
| 6 | Interfaces: declarations with default bodies, `Implements` on Classes and Structs, `Self`, required statics and operators; interface types for Class values, with dynamic dispatch; constraints `<T: I>`, `Where`, and `&`, and conditional Methods; the Abstract and Static Properties that step 11 of M2 left out | constraints are interfaces, so generic code becomes checkable here |
| 7 | Built-in interfaces: `Equatable`, `Comparable`, `Hashable`, `Printable`, and `Numeric`, which primitives, Strings, and arrays implement; Class `==` through `Equatable`, `<` on Strings, Structs and Classes in strings through `Printable`, and `==` on arrays of `Equatable` elements | M2 left these waiting for interfaces |
| 8 | `Extend`: Methods and conformances added to existing types, built-ins included, and conditional ones with `Where` | needs interfaces and constraints |
| 9 | Sequences: an iteration protocol, so `For ... In` loops over any type that has one; `.Chars` and `.Bytes`; slices `a..b`; variadic parameters `Int...` | the protocol is an interface, and every collection needs it |
| 10 | Compile time: `Const Function`s, run by an interpreter over MIR; const generics like `Grid<T, Const W: Int, Const H: Int>`; the attributes `@Inline` and `@Deprecated`; `#If` conditional compilation with `DEBUG`, `RELEASE`, and `PLATFORM_*` | const generics need generics and compile-time values, and reflection builds on attributes |
| 11 | Compile-time reflection: generic code reads a type's fields, Methods, and attributes, in loops the compiler unrolls, to generate code like a serializer | the last language feature; it needs generics, interfaces, and attributes |
| 12 | `std.collections` in FutureBlitz: `Map` and its `{key: value}` literal, `Set`, `Queue`, `Stack`, `Deque`, `PriorityQueue`, and `Pool`, sharing `Add`, `Remove`, `Contains`, `Count`, `Clear`, `Sort`, `Filter`, `Map`, `Find`, and `Clone` | M3's goal; it exercises every step before it |

**Proposed, to settle in its step:**

- Step 6: constraints say what a generic body can do with its type parameters, which step 4
  checks once and lowers per set of type arguments.
- Step 6: an interface type holds a Class's object and calls through a table for that Class and
  interface. A Struct implements interfaces for generic code, which never boxes it.
- Step 11: the spec doesn't say how reflection is written yet, so the step starts with that
  design.

**Not in M3:** the math types (`Vec3`, `Mat4`, `Color`, and the rest) come with `std.math` in M6,
written with operators. `Sendable`, `Shared Class`, `@MainThread`, and atomics come in M4, and
modules, visibility, `@CLayout`, and `Callback` in M5. Running ordinary functions at compile time,
not only `Const Function`s, stays an open question.

## Process

- **Proposed:** track milestones M0–M15 as GitHub milestones, with issues per task.
- Every feature lands with tests: Catch2 unit tests, plus golden tests once they're decided
  (see [open-questions.md](open-questions.md)).
