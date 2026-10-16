# Overview

## What FutureBlitz is

FutureBlitz is a from-scratch **sibling** of Blitz3D:

- a BASIC-like, statically typed language (`.fb` files)
- compiled ahead-of-time to fast native binaries through LLVM
- multi-platform (Windows, Linux, macOS first)
- with a modern game engine and renderer built in
- plus first-party tooling: CLI, language server, and eventually its own IDE

It keeps the spirit of Blitz3D (approachable syntax, script-style programs, a simple
entity-based 3D API, a user-owned main loop) without inheriting its baggage.

License: **MIT**. Planned as Hunter's honors project (starting fall 2027).

## Goals

- Familiar BASIC-like syntax with modern language features as first-class parts of the
  language, not add-ons
- Real modules and a clean FFI instead of `Include` and `.decls` files
- Native performance and multi-platform output
- A modern Vulkan renderer that can still reproduce the classic Blitz3D look exactly
- A standardized, modern asset system
- Strong tooling: errors with exact locations, LSP, formatter, test runner, package manager

## Non-goals

- **No source compatibility with Blitz3D.** Existing `.bb` code is translated, not compiled.
  See [migration.md](migration.md).
- No "classic vs modern" split. There is one language and one renderer; every feature is
  first-class.
- No inheriting Blitz design mistakes for nostalgia's sake.

## What we're fixing from Blitz3D

| Blitz3D problem | FutureBlitz |
|---|---|
| Type suffixes `%`, `#`, `$` | Removed. Real type annotations plus inference for locals |
| Implicit variable declaration (typos silently create variables) | Every variable must be declared |
| `And`/`Or` are bitwise and don't short-circuit | C-style `&&` `\|\|` `!` (short-circuit), separate bitwise ops |
| Only Int, Float, String | Full set of sized integers, floats, `Bool`, math types |
| `Dim` arrays are global; arrays in Types are fixed size | Dynamic arrays, fixed arrays, `Map`, `Set`, and more |
| `Type` = global linked list with no methods | `Class`/`Struct` with methods, constructors, interfaces |
| No function pointers | First-class functions and closures |
| No string escapes | Full escape sequences, interpolation, raw and multi-line strings |
| `Include` text-paste plus `.decls` | `Import` modules plus in-source `Extern` FFI |
| No `Continue`, enums, or real constants | All included |
| No error handling | Checked `Throws`, `Try`/`Catch`/`Finally`, `Panic` |
| `Goto`/`Gosub` | Removed |
| Command-style calls (`Graphics3D 800, 600`) | Parentheses always required |
| Flat global engine API with int handles | Object API with properties and typed handles |
| Magic-number flags for assets, no caching, blocking loads | Typed import settings, cached refcounted assets, async loading |
| DX7 fixed-function renderer, Windows-only | Own Vulkan renderer, multi-platform |
