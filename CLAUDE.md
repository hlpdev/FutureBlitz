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
Phase 1 is done on staging (2026-10-30): M8, tooling; M9, futureblitz.dev's site and docs; and
M10, packages online, the package manager and the registry, with their milestone reviews' fixes.
The site, the package site, and the registry's API are in the private repo hlpdev/futureblitz-web
(planned page by page in its docs/site-plan.md) and serve staging.futureblitz.dev and
staging-pkg.futureblitz.dev. v1.0.0 waits on the user's one-time steps in docs/release-setup.md
and futureblitz-web's READMEs (signing, the tap, winget, the extension's publishers, the apt and
dnf key, R2, production's Cloudflare settings and alerts), then tools/release/launch_check.py, a
v1.0.0 tag, and production's deploy. The engine is packages built on FutureBlitz, never part of the
language, compiler, runtime, or std (the user, 2026-10-30): the main scheduler runs at a top-level
Await, Wait(), and the end of top-level code, and a loop of a program's own or a library's calls
`Scheduler.Main.Step()`. docs/milestones.md's Deferred work lists everything left for later.

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
- M8 (2026-10-29 to 30): fb's own lld and the C runtime's pieces in a self-contained release,
  installers and draft GitHub Releases, DWARF and CodeView with fb's own stack traces, fb-lsp, the
  VS Code extension over CodeLLDB, `fb fmt`, `@Test` and `fb test`, `///` docs and `fb doc`, and
  the miscased-keyword fix-it.
- M9 (2026-10-29 to 30): futureblitz.dev in futureblitz-web: the landing and download pages, Learn's
  tutorials, Docs, the language reference, std's reference, facts.js and the copy check, on
  staging; std fully documented with `fb doc --check`; the scheduler without Flip and the spec as
  the 1.0 reference.
- M10 (2026-10-29 to 30): semver and the resolver, fb.lock, archives, and `fb add`, `update`,
  `search`, `publish`, `yank`, `owner`, and `login`; the registry's API, the package site,
  accounts, trusted publishing, backups and restore, on staging; every install method, Windows
  signed.
