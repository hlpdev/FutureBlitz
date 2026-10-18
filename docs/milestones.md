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
| 13 | Loose ends: explicit arithmetic; Throws, generic Methods, constraints, and Const type parameters on Interfaces; a throwing Iterate; Extends of one instance or array type, Properties in `Extend T[]`, and conditional Interfaces as values; generic bases and Statics; generic Const Functions, Const Functions in declarations' types, and computed Const type arguments; reflection over Properties, Statics, Methods of a Func type, the owner's type parameters and Self, a type's attributes, and lambdas in the loops; consistent errors for later names and parser recovery | so M3 doesn't leave half-done the language it started |

**Not in M3:** the math types (`Vec3`, `Mat4`, `Color`, and the rest) come with `std.math` in M6,
written with operators. `Sendable`, `Shared Class`, `@MainThread`, and atomics come in M4, and
modules, visibility, `@CLayout`, and `Callback` in M5. Running ordinary functions at compile time,
not only `Const Function`s, stays an open question.

## M4 build order

Every step ends with programs in tests/programs that run with no leaks, the same output every
time, in debug and release builds.

| Step | What | Why this order |
|---|---|---|
| 1 | Threads in the runtime: a work-stealing pool with a worker per core but one, started when first used; jobs, completions that a thread waits on while it helps run jobs, ranges split into chunks, and dedicated threads; Print writes whole lines, a panic on any thread ends the program once, and fb_finish stops the pool before the leak report | everything after runs on it; runtime-only and tested in tests/runtime, as M2 built the object runtime first |
| 2 | `Sendable`: a built-in Interface that the compiler decides, which a type can list to have it checked: primitives, Strings, Enums, and the Structs, tuples, and fixed arrays whose fields and elements all are; `<T: Sendable>` and `Where T: Sendable`; errors that say which field isn't | the rule every later step checks |
| 3 | `Atomic<T>` and `Mutex<T>`: `Atomic` of an integer or Bool, with `Load`, `Store`, `Add`, `Subtract`, `Exchange`, and `CompareExchange`, lowered to LLVM's atomics; `Mutex<T>` of a Sendable T, read and changed only in `Lock m As s ... End Lock`, which lets go on every way out, as a Defer runs; both Sendable | the shared data that Shared Classes and Globals hold |
| 4 | `Shared Class`: atomic refcounts and Weak counts; Fields that are Atomic, Mutex, or ReadOnly and Sendable; a Shared Class extends only a Shared Class; its objects are Sendable | objects that cross threads, which need step 3's Fields |
| 5 | `Spawn` and `Task<T>`: a lambda runs on the pool, `t.Wait()` blocks for its result, and `t.IsDone()` checks; captures can't change and are Sendable; the result moves to the thread that waits, and a Task stays on the thread that spawned it; `Thread.Start(fn)` and `Join()` for long blocking work | the first code that runs on another thread |
| 6 | Main-thread rules: `@MainThread` Functions and mutable Globals and Statics are the main thread's; code that reaches one, through its calls, overrides, and Interface implementations, can't run in a Spawn or on a Thread; a Global that's Atomic or Mutex, or a Const that's Sendable, can be used anywhere | data races become compile errors, M4's promise |
| 7 | `Parallel For i = a Until b` (and `To`): chunks on the pool, all done before the next statement; the body follows step 5's and step 6's rules, and can change an outside array's element at the loop variable; no Break, Return, or error leaves it | the second way to do heavy work, on top of the pool and the rules |
| 8 | `Channel<T>(capacity)` of a Sendable T: `Send` waits while it's full and `Receive` while it's empty, `TrySend`, `TryReceive`, and `Close`, and `For x In ch` until it's closed and empty | threads that talk, which needs Sendable and the pool |
| 9 | `Async` Functions and `Await`: stackless coroutines; an Async call without Await starts it and gives its Task; Await on a Task, a Spawn, or another Async call, in Async code; Throws on Async Functions, re-thrown through Await and Wait | spec 10's second job, waiting without freezing |
| 10 | Coroutines and schedulers: the main scheduler, `Scheduler()` with `Run` and `Tick(dt)`, `NextFrame()`, `Seconds(n)` on the scheduler's time (paused and scaled with it), `RealSeconds(n)`, and `MainThread()` | spec 10's third job, doing things over time, on top of Async |
| 11 | Loose ends: the rows of the Deferred work below planned for it, and programs that run a parallel job and an async chain together, many times over, with no leaks | M4's "done when" |

**Not in M4:** lock-free collections, actors, and GPU compute stay out of v1 (spec 10). I/O awaits,
like `File.ReadAsync`, come with `std.io` in M6, and `Flip()` with the engine in M8.

## Deferred work

Everything the spec describes that the compiler doesn't do yet, and where it's planned. Each step
adds what it leaves out. Checked against all of docs/spec/ on 2026-10-17.

| What | Left out in | Planned for |
|---|---|---|
| Computed Const type arguments with `-`, `/`, or `%`, which can leave a size below 1 | M3 step 13 | open question |
| Setting a reflected Property or Static Field, which needs telling apart the ones without a Set or that are ReadOnly | M3 step 13 | open question |
| Indexing a Map, `hp["boss"]`, which needs an index Operator the spec doesn't have yet | M3 step 12 | open question |
| `@Serialize`, which generates serialization code with reflection | M3 step 11 | M6 |
| A Task that another thread waits for, which needs a Sendable result and one waiter for its error | M4 step 5 | open question |
| A Step in a Parallel For | M4 step 7 | open question |
| A task that waits on a Channel holds its worker, so tasks that all wait can leave none for the work they wait for | M4 step 8 | open question |
| A throwing Task's type written out, like `Task<Level> Throws FileError`, which only inference gives now; after a function's return type, Throws is the function's | M4 step 11 | open question |
| Async Methods in Interfaces, required or default | M4 step 11 | open question |
| Modules: one per file, `Import`, `From`, visibility, and `project.toml` | | M5 |
| FFI: `Extern`, `Ptr<T>`, `Void`, `CString`, `Callback`, `@CLayout`, `Unsafe`, `Mem`, `AddressOf`, and bindgen | | M5 |
| The math types, `Vec2` through `Color` | | M6 |
| A fix-it for a miscased keyword, like `if` for `If`, and `///` doc comments | M1 | M7 |
| Stack traces for panics, which need debug info; a panic in std/, like popping an empty Stack, points at the library's line, not the caller's | M1 | open question |

## Process

- **Proposed:** track milestones M0–M15 as GitHub milestones, with issues per task.
- Every feature lands with tests: Catch2 unit tests, plus golden tests once they're decided
  (see [open-questions.md](open-questions.md)).
