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
| **M7** | Libraries and native packages: static and dynamic libraries for C and FutureBlitz callers, one shared runtime, interface files, prebuilt native code in packages | a C program and an FB program each call one FB static and one FB dynamic library; a package wraps a prebuilt C++ DLL; a program that uses only std is still one executable |
| **M8** | Tooling: LSP, VS Code extension, formatter, test runner, package manager | daily-driving FutureBlitz in an editor feels good |

## Phase 2: Engine

| | Goal |
|---|---|
| **M9** | Engine bootstrap: window, input, frame loop, main scheduler |
| **M10** | 2D: images, drawing, fonts |
| **M11** | 3D scene: entities, cameras, lights, hierarchy |
| **M12** | Vulkan renderer and materials |
| **M13** | Asset pipeline: VFS, cooker, pak files, typed references, hot reload |
| **M14** | Audio and physics (miniaudio, Jolt) |
| **M15** | The IDE |
| **M16** | Migrator, plus porting a real Blitz3D game as the final proof |

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
like `File.ReadAsync`, come with `std.io` in M6, and `Flip()` with the engine in M9.

## M5 build order

Every step ends with programs that run with no leaks in debug and release builds, in
tests/programs or, from step 5, as packages in tests/packages, and every earlier program still
prints the same output.

| Step | What | Why this order |
|---|---|---|
| 1 | Module scopes: each file is a module with its own top-level names, named by its path, with no new syntax yet; a name is found in the code's own scopes, then its module, then what the module imports, then the built-ins, and for now every module imports all of std; only the main module's top-level code runs; MIR and LLVM qualify the names outside the main module; a program can declare its own `Stack` or `Map` | everything after lives in modules, and swapping sema's one namespace for a scope per module is M5's riskiest refactor, so it goes first and alone, with every existing test as the check, like M2's type table and M3's step 1 |
| 2 | `Import` and `From`: `Import std.collections`, `Import "./ui/hud"`, `As` aliases, `From x Import A, B`, and qualified names in values and types, like `C.Map<K, V>`; the driver loads std/core/ and what the imports reach, each file once, cycles allowed; missing and ambiguous names reported where they're used; Globals initialize imports first; std/ and the programs that use collections import what they use | with scopes in place, this finishes how programs and std/ are put together; packages, std.ffi, and bindgen's output are all modules that something imports |
| 3 | Visibility: `Private` (the default), `Package`, `Protected`, and `Public` on top-level declarations and on the members of Structs, Classes, and Extends, checked at every use from another file; the rules for Overrides, Interface implementations, Operators, constructors, and Public signatures; std's API marked `Public` | a restriction on modules that already work; std/, std.ffi, and bindgen's output are written with their final visibility from here on |
| 4 | `Extern "C"` and `"stdcall"` blocks of Functions, with `= "symbol"`, called from safe code; C's integers and floats, `Bool`, Enums, and `CString`, which a `String` argument becomes for the call; each C symbol declared once, as clang would; `Print` moves into std.core over `Extern "runtime"`, and the built-in one goes | the smallest FFI that runs, since every program links the C runtime already; Print is the first std code over it, which decisions.md said waits for std.core |
| 5 | `project.toml` and packages: `[project]` and local `[dependencies]`, with errors at their lines; `Import physics` and `Import physics.shapes`; `Package` means the real package; `fb check`, `build`, and `run` on a package folder | importing packages and `Package` visibility need package boundaries, and `[native]` lives in the manifest |
| 6 | Native libraries: `[native]` static archives and system libraries, linked statically, in order, for a package and its dependencies; `Lib` names an entry; a C test library in tests/native, built by CMake with the runtime's toolchain and called through hand-written Externs | the first separately compiled C library, linked before bindgen exists, so linking and C runtime mismatches show up on their own |
| 7 | Pointers and `Unsafe`: `Ptr<T>` and `Ptr<Void>`, stored and passed in safe code; `Unsafe` blocks, Functions, and Methods, which alone dereference (`p.Value`, `p[i]`), do pointer arithmetic, take `AddressOf`, use `Mem`, and read a `CString`; `SizeOf<T>()`; std.ffi's `Mem`, written over the C runtime | C APIs pass memory, structs go through pointers, and callbacks take user data, so pointers come before the last two C types |
| 8 | `@CLayout` Structs: Fields of C types only, laid out as C does, passed by value and through `Ptr` by the target's C ABI (Win64 and SysV x86-64) | LLVM leaves the C ABI to the front end, and a mistake corrupts memory silently, so it's done alone and checked against C; Callback and bindgen build on it |
| 9 | `Callback(A, B): R`: a C function pointer, from a Function's name or a lambda that captures nothing, through a C-ABI thunk; closures rejected, saying why; its code checked as work on another thread | spec 12's last construct; it needs `Ptr<Void>` user data and step 8's ABI |
| 10 | bindgen: tools/bindgen's `fb-bindgen`, over libclang's C API, turns a C header into a `Public Extern` module, mapping to every FFI construct and skipping what can't map with a comment; `fb bindgen` runs it | everything it writes already compiles and runs, and as a tool of its own, libclang's problems can't break `fb` |
| 11 | Loose ends: the rows of the Deferred work below planned for it, and a package that calls the C test library through bindings fb-bindgen generates: numbers, strings, pointers, structs, enums, constants, callbacks, and an opaque handle | M5's "done when" |

**Not in M5:** the package manager (versioned dependencies, resolving and fetching them, lock
files, a registry, `fb new` and `fb add`) comes in M8, where spec 11 puts it, with `fb test`, the
formatter, the LSP's use of modules, and running bindgen from the manifest at build time. Dynamic
`[native]` libraries wait for M7, with FB's own libraries. The rest of std.ffi, the
std modules that will use `Extern "runtime"` (std.text, std.io, std.os), and std.concurrent as a
module of its own are M6; M4's built-ins stay built in. Separate compilation or caching of
modules stays an open question, and so do C varargs functions like printf, unions, bit fields, C
globals, `long double`, macros other than number constants, typed opaque handles, C++ headers,
`@MainThread` callbacks, and building FutureBlitz code as a C library. Callbacks that capture
stay out, as spec 12 says. CI on Linux and macOS (M0) waited for M6 step 0, which ran the SysV
struct ABI's C calls on Linux and added AArch64's for macOS. The self-contained toolchain stays
open for M8: M5 still links through the C++ compiler that built the runtime.

## M6 build order

Every step ends with programs in tests/programs, or packages in tests/packages, that run with no
leaks in debug and release builds, and every earlier program still prints the same output. A
bundled C library arrives with the first step that uses it, built for the target by the C
compiler that builds the runtime. A program that doesn't use one links none of its code. What
a step adds to std is Public and documented in docs/stdlib.md, and a step that changes the
language changes its spec. A test never depends on the OS, the locale, or the time of day,
except where it's about those.

| Step | What | Done when | Why this order |
|---|---|---|---|
| 0 | CI: GitHub Actions builds fb and runs every test on Windows, Linux, and macOS for each push, installing LLVM and libclang on each runner | the existing 725 tests pass on all three, or each one that can't is a Deferred work row saying why | M6 adds most of FutureBlitz's platform code, which only CI can check on Linux and macOS |
| 1 | The foundation: third_party/ fetches and builds bundled C libraries, starting with libuv; the runtime's I/O thread runs libuv's loop, started on first use and stopped before the leak report, and finishes Tasks that Await and Wait resume where they started; top-level code can Await, and the runtime runs the main scheduler until no work is left; `fb` prints the bundled libraries a build linked and their licenses; every std name needs its Import, std.core's too (spec 11), and every program imports what it uses; an attribute through an alias, like `@C.Rename`, parses; `Print(values...)`, `Write`, `PrintError`, `WriteError`, `Flush`, and `Input`; the area Errors with `Kind`, `Cause`, and `Place`, which Throw fills, and an uncaught error's report; `AssertEqual`, `DebugAssert`, `Unreachable`, `Todo`; `@PanicsAtCaller` | a program writes to stdout and stderr in order and reads Input; an Await on a runtime I/O request resumes on the main thread, many times over with no leaks, and so does a top-level Await; popping an empty Stack panics at the program's line; an uncaught error prints its place and causes; a name from std without its Import is an error that says which Import; a program without I/O links none of libuv | everything later stands on the loop, the Imports, the Errors, and caller-pointing panics, so they come first, alone, like M5's module scopes |
| 2 | Indexing, iteration, and ranges: `Operator []` and `Operator []=`, with one index or more; generic default Methods on Interfaces, called through the Interface's values; `Iterable<T>`; Iterator<T>'s lazy adapters and consumers, `OrderBy`, `Repeated`, and `Empty`; ranges as values, `0 Until 10`, `1 To 10`, and `Step` | `grid[x, y]` works on a program's own types; a chain of adapters runs through an `Iterator<T>` value, an `Iterable<T>` parameter, and a Range, giving the same as a For loop | the collections and every later module are written with these |
| 3 | Collections I: Map and Set in insertion order through removals, `==` and Hashable by contents; T[]'s search, ends, bulk, order (`SortUnstable`), shape, and memory Methods; `SortBy`, `ThenBy`, and the Descending forms; the Try twins; Map's `[]`, defaults (`Get(key, fallback)`, `GetOrAdd`, `Map(default: v)`), `TryAdd`, `Update`; Set's algebra; Pool's `IterateHandles`; `ArrayView`, the `AsReadOnly` views; `Cloneable`, `Defaultable`; `Min`, `Max`, `Clamp`, `Swap`, `HashCombine`, `TypeName`; a `T?`'s `Expect` and `OrElse` | each Method is tested, empty cases and panics included; removing and re-adding keeps a Map's order; `counts[w] += 1` counts; `SortUnstable` sorts without making an array; a view sees its array's changes and keeps it alive | the most-used part of std, on step 2's language |
| 4 | Collections II: `SortedMap`, `SortedSet`, `RingBuffer`, `Grid`, `Grid3`, `BitSet`, `LruCache`, `Counter`, `MultiMap`, `Trie`, `LinkedList`, `IntervalTree`, `SpatialHash`, `QuadTree`, `Octree`, and `ImmutableArray`, `ImmutableMap`, and `ImmutableSet` | each structure matches a plain array or Map doing the same work over random operations; the immutable ones share structure and cross threads | the rest of the collections, which later modules store things in |
| 5 | Formatting, parsing, and the debug form: format specifiers in interpolation (width, alignment, fill, precision, sign, bases, exponent, thousands), checked at compile time; `Formattable`; `Format(template, values...)`, which throws a FormatError; `Int.Parse`, `Float.Parse`, `Bool.Parse`, giving a `T?`; `"{x:?}"` and `Dump(x)` for every value, from reflection; invariant always | `"{price:.2}"` prints 3.14; a spec that doesn't fit its value is a compile error at the spec; what Format writes Parse reads back, every Float64 included; Dump shows a Struct holding arrays, Maps, and T?s | text, logging, and data all write with these |
| 6 | Text I: the built-in `Char` (`'a'` becomes a Char) and `s.Chars`; String's Methods by byte position, with `Length`, `CharCount`, `GraphemeCount`, `Slice` that panics mid-character, search, split, join, trim, case, pad, compare; `StringBuilder` (a Writer from step 13 on); `Scanner`; std.text.unicode over utf8proc; std.text.encoding's UTF-16, UTF-32, ASCII, Latin-1, and Windows-1252 with BOMs, Base64, Hex, Base32, percent-encoding, HTML escaping, `Escape`/`Unescape` | each Method's tests include text beyond ASCII, combining marks, and emoji; `ToUpper("straße")` is `STRASSE`; encodings round-trip every byte | text is what io, data, and net read and write |
| 7 | Text II: std.text.regex over PCRE2, with match limits, the String shortcuts, and the compiler's check of literal patterns; `Glob`, `WordWrap`, `Dedent`, `Indent`, `Levenshtein`; fuzzy matching; `Humanize`; std.text.diff | a bad literal pattern is a compile error at it; a pattern known to backtrack forever throws within its limit; captures by number and name; diffs and merges match known results | the rest of text's everyday tools, apart so PCRE2 arrives alone |
| 8 | Text III: std.text.template (Mustache-style) and std.text.markdown over md4c | templates fill from a Map, a Struct, and a JSON Value, escaping HTML; Markdown passes CommonMark's spec tests | the text tools that build on everything before |
| 9 | Math I: constants, scalars, angles (`Deg`, `Rad`), std.math.bits; `Vec2`-`Vec4`, `Vec2i`, `Vec3i`, `Quat`, `Mat3`, `Mat4` with SIMD; `Color` and `Color32`; geometry | the math types leave the Deferred work; a matrix times its inverse is the identity within a tolerance; LookAt and Perspective match known values for 0..1 depth; Color32 and Color convert with sRGB gamma both ways | the engine's types, which need no I/O |
| 10 | Math II: `Random` (PCG) and `ThreadLocal Global` (the language change) behind `Rand` and `SeedRandom`; an array's `Shuffle` and `Sample`; noise, easing, stats; curves and splines; polygon geometry; `Fixed32` and `Fixed64` | a seed gives the same numbers in debug, release, and on every thread's own generator; ThreadLocal values are released as their threads end; Fixed math gives the same bits as reference values | random needs per-thread state, so ThreadLocal comes with its first user |
| 11 | Math III: std.math.big (BigInt, Decimal, Rational, Complex) and typed units, which need computed Const type arguments with `-` (the open question in the Deferred work, settled here) | BigInt and Decimal match known results; `Meters / Seconds` is `MetersPerSecond`, and adding Meters to Seconds is a compile error | the last of math, and a compiler change it alone needs |
| 12 | std.ffi's additions: `CData`, a constraint the compiler decides, as it does Sendable (numbers, Bool, Enums, Ptrs, CString, Callbacks, @CLayout Structs), so `Ptr<T>` can take a type parameter that has it; `Buffer<T: CData>` and its views; `CLong`, `CULong`, and `CWChar`, which bindgen writes; `Mem.AllocAligned`, `FreeAligned`, `AllocUninitialized`; `Extern "stdcall"` goes | a Buffer's view keeps it alive after the Buffer's last variable goes; C fills a Buffer through its Pointer, read back in safe code; a binding using CLong is right on every target | std.io reads into Buffers, so they come before it |
| 13 | std.io's streams: `Reader`, `Writer`, and `Seeker`, with Async twins (Async Methods in Interfaces, deferred from M4 step 11); buffered, memory, and string streams; `BinaryReader` and `BinaryWriter`; `Copy`, `ReadAll`, `ReadExact` | each stream reads back what was written, at buffer edges too; an Async read through a Reader value awaits on the main thread | every file, console, socket, and compressor is one of these |
| 14 | std.io's files: `Path` (Strings convert to it), `File` with its one-call helpers, `Open` and options, atomic writes, `FileLock`, `Metadata`, line endings, and Async twins over libuv; `MappedFile`; `Dir` with `Entries`, `Walk`, and `Glob`; temporary files and folders; `FileWatcher`; the console | a test writes, reads, renames, maps, watches, locks, and removes files in a temporary folder, with non-ASCII names; errors come as IOError Kinds; nothing's left behind | the first real OS work, on step 13's streams |
| 15 | std.io's extras: `Embed`, `EmbedText`, and `EmbedFolder` (compiler built-ins); the virtual file system; `Preferences` | an embedded file's bytes match the file's; a VFS reads the same file from a folder and an embedded folder (zips mount once step 22 adds them); Preferences survive a run | file access beyond the disk, on step 14's files |
| 16 | std.time: `Duration` (`2.Seconds`, and the timing built-ins taking one), `Timestamp`, `Date`, `TimeOfDay`, `LocalDateTime`, `ZonedDateTime`, `TimeZone` (embedded IANA data and the OS's zone), `Stopwatch`, `Measure`, `Sleep`, formats, timers, `Deadline`, `FixedStep`, `RateLimiter`, `Debounce` | date arithmetic and conversions match known values across DST changes in several zones; `Await Wait(500.Milliseconds)` resumes on the main thread; tests take time as a value, never the clock | io's timeouts and net's deadlines need it |
| 17 | std.os: `Args.Parse<T>()` with attributes and the builder, filling a T Field by Field without running New (each Field gets the data's value, else its default, else its zero, else an error names it), and `OnLoaded()` after, which std.data and std.db use too; `Env` and `.env` files; `Process`, `Command`, and `Child` with piped streams; `OnInterrupt` and `OnTerminate`; system info; `Folders`; `OpenDefault` | a program runs another with arguments and environment, reads its output, and gets its exit code, with no shell; generated help and errors read well | processes need streams and time |
| 18 | std.concurrent, std.log, and std.events: `Task<T> Throws E` written as a type, with E a type parameter where it's generic; the locks, `Lazy`, `ConcurrentMap`, `ConcurrentQueue`, `WhenAll`, `WhenAny`, `TaskSource`, `Cancellation`, `Select`, and the parallel algorithms; `Logger` and `FB_LOG`; `Event<T>` and its Subscriptions | each concurrent type runs under many threads, many times over, with the same result; a Cancellation stops a blocking read; logs filter by module; an owner's handlers go when it does, with no leak | blocking calls take a Cancellation, and logging writes through io and time |
| 19 | std.data I: `Value`; JSON with exact integers and a streaming reader and writer; automatic serialization by reflection, every Field, with `@Rename`, `@Skip`, `@Default`, `@Flatten`, and custom Encode/Decode; MessagePack, with `@Since` and `@Renamed` for versioned saves | JSON's conformance suite passes; a Struct with nested Structs, arrays, Maps, Enums, and T?s round-trips through JSON and MessagePack; a version-1 save loads into the version-2 type; serialization leaves the Deferred work | the formats saves and net use, on text and io |
| 20 | std.data II: TOML, CSV, INI, XML (tree, streaming, XPath-lite), YAML (1.2 core schema); Protocol Buffers, with fb reading `.proto` files at compile time | each format round-trips the step 19 Struct and passes its spec's test suite where one exists; generated protobuf types read what protoc's encoder wrote | the rest of the formats, on the same serialization |
| 21 | std.crypto: the hashes, `Hmac`, `Hkdf`, `Pbkdf2`, `SecureRandom`, `ConstantTimeEquals`, `Crc32`, `XxHash`, `Blake3`; `Argon2id` and `Bcrypt`; AES-256-GCM and ChaCha20-Poly1305 through `Seal` and `Open`; Ed25519, ECDSA P-256, RSA verify, X25519, and PEM | every algorithm matches its published test vectors; a password hash verifies, and a wrong one doesn't | TLS, HTTP, and the database's tools need them |
| 22 | std.compress: Deflate, Zlib, Gzip, Zstd, Lz4, and XZ streams; Zip and Tar to read and write; 7z to read | each format round-trips, and reads what common tools wrote | archives for the VFS, packages, and HTTP |
| 23 | std.uuid and std.db: `Uuid.V4`, `V7`, `Parse`; SQLite with `Execute`, `Query<T>`, transactions, prepared statements, and `Migrate` | Query<T> reads rows into Structs; a failed transaction leaves nothing; migrations run once each, in order | the first of the extra modules, on data and io |
| 24 | std.image I: the codecs (PNG, JPEG, BMP, TGA, GIF, HDR, QOI, WebP, SVG, animated GIF and APNG) into an `Image`, and the transforms | each format round-trips where it can be written, and decodes reference files to known pixels; resizing matches reference output within a tolerance | images, which the engine's 2D will share |
| 25 | std.image II: drawing lines, shapes, fills, and TrueType text | drawn images match reference images pixel for pixel | the drawing M10 builds on |
| 26 | std.term: line widgets and full-screen terminal apps | widgets render to a fake terminal as expected, with and without color; a full-screen app redraws only what changed | console tools, on io and text |
| 27 | std.algo: BFS, DFS, Dijkstra, A*, flow fields, union-find, topological sort, combinatorics, Delaunay, Voronoi | paths match known shortest paths on grids and graphs; triangulations match reference results | the algorithms games and tools reach for |
| 28 | std.net I: `IpAddress`, `SocketAddress`, `Dns`, `TcpListener`, `TcpStream`, `UdpSocket`, `IpcListener`, `IpcStream`, `Url`, `QueryParams`, LAN discovery | a program talks to itself over TCP, UDP, and IPC, blocking and Async; a refused connection is a NetError Kind; URLs parse per the WHATWG tests that apply | the bottom of the network stack |
| 29 | std.net II: `TlsStream` with the OS's roots and a TLS listener; `HttpClient`, `Http.Get`, `Download`, over libcurl | a TLS client and server in one program talk through a test certificate; the client gets, posts, follows redirects, and decompresses from a local test server, with no internet needed | the client the tooling milestone's package manager needs |
| 30 | std.net III: `HttpServer` (its loop, `Workers`, routes and attributed Classes, middleware, static files), WebSockets, and automatic HTTPS (ACME) | the program's own HttpClient and WebSocket client run against its own server, over HTTP and HTTPS, with no leaks after many connections; ACME runs against a local test CA | the top of the stack, on everything before it |
| 31 | std.net IV: reliable UDP for games (std.net.game) | two programs keep a reliable-ordered channel in order and an unreliable one fresh over a link that drops and reorders packets on purpose | games' networking, on step 28's UDP |
| 32 | Loose ends: the rows of the Deferred work planned for M6, std.debug, docs/stdlib.md as std's reference, and a program that uses std alone to fetch JSON over HTTPS, keep it in a SQLite database, render a chart image of it, and serve both to its own WebSocket client | every module in docs/stdlib.md exists, documented and tested | M6's "done when": `std.*` complete, with tests |

**Decided for step 1:**

- `@PanicsAtCaller` on a Function or Method passes the call's place as a hidden parameter, so its
  Panics and Asserts report where it was called, as Rust's `#[track_caller]` does. std uses it
  on everything that panics on misuse, and a program's own helpers can too.
- After a build that links a bundled C library, `fb` prints which ones it linked and their
  licenses. Shipping the notices is the programmer's job, and fb writes no notices file.
- Each bundled library's runtime functions are an archive of their own, like `fb_runtime_io`
  for the `fb_io_` ones over libuv, which a program links, with the library and its system
  libraries, only when it calls one of them. lld takes archives in any order.

**Decided for step 3:**

- `Expect` and `OrElse` are built-in Methods of every `T?`, since no Extend can name a `T?`.

**Proposed, to settle in its step:**

- Step 16: the lexer reads `2.Seconds` as `2` and then `.Seconds`, not as a float.
- Step 18: `Select` is a builder (`Select().On(ch, f).Timeout(d).Wait()`) unless a statement
  reads better.
- Step 29: curl uses mbedTLS, given the OS's roots: Windows' and macOS' certificate stores, and
  the distribution's bundle file on Linux.

**Not in M6:** `std.test` comes with M8's `fb test`. Stack traces for panics wait for debug info
(open question). An HTTP/2 server, QUIC and HTTP/3, FTP, and SMTP stay out until something
needs them. Loading a library at run time comes in M7.
A `Locale` for formatting comes later, and only ever explicit. Platform-only APIs stay out of
std. Navigation meshes, game AI, and procedural generation wait for the engine phase. CI on
Linux and macOS can't be checked from this machine, so steps there are checked by CI alone.

## M7 build order

Planned 2026-10-22, with the design in [libraries.md](libraries.md). Every step ends with tests on
all three CI runners, and a program that uses only std stays one executable.

| Step | What | Done when |
|---|---|---|
| 0 | The spec and docs: spec 11's project types, natives, and package archives, spec 12's `@Export` | the spec says what M7 builds |
| 1 | `type` in project.toml; a static library for C callers: `@Export` Functions, the C header, and the runtime's archive beside it | a C program built by CMake links an FB static library and calls it |
| 2 | A dynamic library for C callers: only its exports visible, the shared runtime DLL, set up on the first call | a C host loads an FB DLL and calls it from two threads, with no leaks |
| 3 | Classes as handles for C | C makes, calls, and releases an FB object through its handle |
| 4 | `[native]` dynamic libraries and `{target}` paths; DLLs copied beside the executable, rpaths | a program calls a C DLL from its package's native/<target>/ and runs from its build folder |
| 5 | The interface file: fb writes a library's .fbi, and the loader reads it in place of source | a package checks against a library's .fbi as it does against its source |
| 6 | FB callers of a static library: its archive and .fbi, generic instances, and the ABI hash | an FB program uses a prebuilt FB static library's Classes, generics, and errors; a mismatched build is an error naming both |
| 7 | FB callers of a dynamic library: export tables and import libraries, the shared runtime, one type per instance across binaries, Globals' accessors, extending a library's Class | an FB program and an FB DLL share objects, Tasks, and `Is` checks, with one runtime and no leaks |
| 8 | Loading at run time: `DynamicLibrary`, and plugins | a host loads an FB plugin and a C library at run time |
| 9 | A package with prebuilt natives: a stand-in C++ engine DLL with a C API (tests/native), wrapped by an FB package through fb-bindgen | the package builds and runs on Windows, Linux, and macOS |
| 10 | Loose ends, and the milestone's review | M7's "done when" |

## Deferred work

Everything the spec describes that the compiler doesn't do yet, and where it's planned. Each step
adds what it leaves out. Checked against all of docs/spec/ on 2026-10-17.

| What | Left out in | Planned for |
|---|---|---|
| Computed Const type arguments with `-`, `/`, or `%`, which can leave a size below 1 | M3 step 13 | M6 step 11 |
| Setting a reflected Property or Static Field, which needs telling apart the ones without a Set or that are ReadOnly | M3 step 13 | open question |
| Serialization by reflection, sketched as `@Serialize` and now automatic | M3 step 11 | M6 step 19 |
| A Task that another thread waits for, which needs a Sendable result and one waiter for its error | M4 step 5 | open question |
| A Step in a Parallel For | M4 step 7 | open question |
| A task that waits on a Channel holds its worker, so tasks that all wait can leave none for the work they wait for | M4 step 8 | open question |
| A throwing Task's type written out, like `Task<Level> Throws FileError`, which only inference gives now; after a function's return type, Throws is the function's | M4 step 11 | M6 step 18 |
| Async Methods in Interfaces, required or default | M4 step 11 | M6 step 13 |
| A generic type's own Operator used on a Class extending one of its instances, like Box<T>'s `<` on `Class Nums Extends Box<Int>`, whose type arguments aren't inferred through the base, so "no Operator < takes (Nums, Nums)"; what an Extend of the instance adds works | M5 step 3 | open question |
| By-value Structs to and from C on 64-bit targets that aren't x86-64 or AArch64, like RISC-V, where fb-bindgen skips functions that pass one | M5 step 8 | open question |
| A Callback that takes or gives a Struct by value, whose thunk would need the C ABI's lowering in reverse | M5 step 9 | open question |
| A fix-it for a miscased keyword, like `if` for `If`, and `///` doc comments | M1 | M8 |
| Stack traces for panics, which need debug info | M1 | open question |
| A Float range, like `0.0 To 1.0 Step 0.1`, which a For loop counts but a range doesn't | M6 step 2 | open question |
| A `For i In 0 Until n` that counts without making a Range, as a For loop does | M6 step 2 | M6 step 32 |

## Process

- **Proposed:** track milestones M0–M16 as GitHub milestones, with issues per task.
- Every feature lands with tests: Catch2 unit tests, plus golden tests once they're decided
  (see [open-questions.md](open-questions.md)).
