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
M7, libraries and packages, is done: steps 0 to 7 of docs/milestones.md's M7 build order landed by
2026-10-29, on Windows, Linux, and macOS, with plugins and loading at run time left for later
(Deferred work). M8, tooling, is under way (docs/tooling.md; steps 0, the design, and 3, debug info
and stack traces, done 2026-10-29): the self-contained toolchain and releases, debug info and a
debugger, the LSP, VS Code, the formatter, `fb test`, and `fb doc`. M9 is futureblitz.dev's site,
docs, and playground, and M10 packages online, the package manager and the registry, both in
docs/website.md; phase 1 ends with M10, and the engine is a phase of its own. docs/milestones.md's
Deferred work lists everything left for later, and where it's planned.

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
- M6 (2026-10-20 to 27): CI on Windows, Linux, and macOS; std's foundation, indexing and ranges,
  collections, formatting and parsing, text, Char, and regex, Markdown and templates, math, big math
  and units, ffi's CData and Buffer, streams, files, Embed, the VFS, and Preferences, time, Value,
  Load and Save, std.os, processes, signals, and Args, concurrency, events, and std.log, JSON and
  MessagePack, TOML, CSV, INI, XML, YAML, and Protocol Buffers, crypto, compression, uuid, SQLite,
  and Query<T>, images, drawing and fonts, the terminal, algo, addresses, URLs, DNS, sockets, and
  LAN discovery, TLS, the HTTP client, the HTTP server, WebSockets, HTTPS with ACME, game
  networking, std.debug, and a program that uses them together; in the language, partial
  type-argument inference, overloads by type-parameter count, Defaults<T>, and For Method's own type
  parameters, picks, and throwing Methods; a parallel verify; and every one of the whole-codebase
  review's fixes.
- M7 (2026-10-28 to 29): project types and interface files, prebuilt libraries embedded and
  beside the executable as the OS's own DLLs, one runtime across the edge, dynamic natives and
  {target}, a package with a prebuilt C++ engine DLL, each library's recorded builds, and a
  program that embeds one prebuilt library and has another beside it.
