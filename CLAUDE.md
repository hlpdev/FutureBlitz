# FutureBlitz

From-scratch Blitz3D successor: language (.fb), compiler, tooling, then engine.
Source of truth: docs/spec/

## How we work
- Since M6, you write the code, std's .fb files included, directly in the repo, one step at a
  time, in my code style. Verify each step before calling it done: ctest, release outputs,
  clang-format, and clang-tidy. Every bug a check or review finds, of any severity, is fixed,
  with a test, before the step closes.
- Commit each verified step as me (a short lowercase message, no Claude attribution) and push.
- Do the work yourself, unless a /goal I give says to work as docs/m6-plan.md describes;
  otherwise ask before starting review agents or separate sessions.
- You write the tests (tests/) directly, in my code style.
- When I ask for a review, read my files and point out problems. Fix minor issues yourself
  (formatting, naming, comments, small portability fixes); leave real changes to me.
- Keep explanations short.

## C++ conventions
- C++23, CMake, deps via FetchContent
- snake_case; types end in _t; private/protected members use m_; enum constants UPPER_CASE; template params CamelCase
- .clang-format and .clang-tidy are authoritative
- Style: never align declarations or assignments; spaces inside braced lists `{ .line = 0 }`
- Headers in <project>/include/fb/, namespace fb
- Tests: Catch2 via FetchContent; compiler unit tests in tests/compiler/ (fb_compiler_tests), run with ctest

## Projects
compiler (lib), runtime (lib), cli (exe), lsp (exe), tools/bindgen (fb_bindgen lib, fb-bindgen
exe)

## Current step
M6 (the full standard library), planned 2026-10-20 and 2026-10-21 in 32 steps (docs/milestones.md;
design in docs/stdlib.md). Its lanes were wound down on 2026-10-24 for the weekly usage limit, each
unit pushed with a handover note; they resume on 2026-10-26 from the coordinator's notes.
docs/milestones.md's Deferred work lists everything left for later, and where it's planned.
The whole-codebase review's fixes and the rest of M6 run as docs/m6-plan.md describes.
M7, libraries and native packages, is planned in docs/libraries.md; tooling is M8.

## History
Each milestone's steps, in detail (what each added, by name, and each review's fixes), are in
docs/history.md; a landed step's notes go there, newest milestone first.
- M1 (2026-10-16): source and diagnostics, the lexer, the parser, sema, codegen over LLVM's C
  API with lld, and the fb CLI.
- M2 (2026-10-16 to 17): Structs, Classes and refcounting, nullability, inheritance, arrays,
  Enums, Defer, errors, Weak and leak reports, Statics and Properties.
- M3 (2026-10-17): several files, tuples, function values, generics, Operators, Interfaces,
  Extend, sequences, compile-time code, reflection, and std.collections.
- M4 (2026-10-17 to 18): threads, Sendable, Atomic and Mutex, Shared Class, Spawn and Task,
  @MainThread, Parallel For, Channel, Async and Await, coroutines and schedulers.
- M5 (2026-10-18 to 20): modules, Import and From, visibility, Extern and CString,
  project.toml and packages, native libraries, pointers and Unsafe, @CLayout and the C ABI,
  Callback, and bindgen.
- M6 so far: step 0 (CI on Windows, Linux, and macOS), 1 (std's foundation), 2 (indexing,
  iteration, and ranges), 3 (collections I), 4 (collections II), 5 (formatting, parsing, and the
  debug form), 6 (text I, Char), 7 (text II, regex), 9 (math I), 10 (math II, ThreadLocal
  Global), 11 (big math and units), 12 (ffi: CData and Buffer), 13 (streams), 16 (time), 21
  (crypto), 22 (compression), 23 (uuid and SQLite), 27 (algo); parts of 17 (Value), 18
  (concurrency, events, Sendable Func, Where T: Class, Task<T> Throws E), 28 (addresses, URLs,
  DNS), and 32 (std.debug); and every one of the whole-codebase review's fixes.
