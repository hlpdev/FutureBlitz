# Tooling (M8)

M8's design, planned with the user on 2026-10-29: everything that makes FutureBlitz something
other people can install and use every day in an editor. Step 0 settles what's still open here.

## Decided

1. **Installable first.** fb has to install and build programs on a machine with no C++
   compiler before anything else, since every other tool, and M9's Install button, depends on
   it. M8 starts with the self-contained toolchain and fb's releases.
2. **A debugger in M8.** Debug info through LLVM (DWARF on Linux and macOS, CodeView on
   Windows), so VS Code sets breakpoints and steps through FutureBlitz code, and panics show
   stack traces. Daily-driving in an editor needs it.
3. **The package manager isn't in M8.** Its client and the registry are one system, built and
   tested together in M10 (packages online), after M9's site, whose pages accounts and the
   package site use.
4. **One grammar.** The TextMate grammar the VS Code extension uses is the one M9's site
   highlights code with.

## The toolchain (steps 0 and 1)

- fb ships lld and what each target links besides the program: the runtime, std's bundled
  libraries, and the C runtime's startup and libraries, so building needs no C++ compiler.
- Windows: the MinGW-w64 runtime fb is built with (static, as today), bundled.
- Linux: the system's libc and its startup files; fb bundles the rest (libstdc++'s or libc++'s
  static parts, libgcc's or compiler-rt's).
- macOS: Apple's Command Line Tools for the SDK, which can't be redistributed; `fb` says how to
  install them when they're missing.
- To settle in step 0: libc++ or libstdc++ on Linux, the oldest glibc a build supports, and
  whether fb links through lld directly rather than a C++ driver.

## Releases (step 2)

- Versioned builds of fb for windows-x64, linux-x64, linux-arm64, macos-arm64, and macos-x64,
  from CI, as archives with an install script per OS, and `fb --version`.
- `fb new <name>` makes a package: project.toml, main.fb, and a .gitignore.
- To settle in step 0: where releases go before M10's dl.futureblitz.dev exists (GitHub
  Releases), installers or archives only, and self-update.

## Debug info (step 3)

- LLVM's DIBuilder: functions, lines, locals, parameters, and types (Structs, Classes, arrays,
  Strings, nullables, Enums), in debug builds, and line tables in release.
- Panics and uncaught errors print a stack trace of FutureBlitz names, files, and lines.
- VS Code debugs through lldb-dap on Linux and macOS and cppvsdbg or lldb-dap on Windows; Strings,
  arrays, and Classes show as their values, not their bytes.

## The LSP (step 4)

- Diagnostics as you type, hover with types and docs, go to definition, find references,
  completion (members, names in scope, Imports, keywords), document and workspace symbols, rename,
  signature help, semantic tokens, and code actions for the fix-its fb already has.
- A front end that re-checks only what an edit touches, a file and what depends on it, so a
  large package stays responsive.

## The VS Code extension (step 5)

- The TextMate grammar, the LSP client, snippets, tasks for build, run, and test, and the
  debugger's configuration.
- Published to the VS Code Marketplace and Open VSX, under accounts the user makes.

## The formatter (step 6)

- `fb fmt` formats files in place, `--check` reports without changing them, and the LSP formats
  on save. One style, with no options but the line width.

## Testing (step 7)

- std.test and `fb test`: test functions found by an attribute, assertions that say what they
  compared, tests run in parallel, filters by name, and a summary.
- Doc tests: the examples in `///` comments compile and run.

## Docs from the code (step 8)

- `///` doc comments on declarations, and `fb doc`, which writes a package's public API as JSON
  (modules, types, members, signatures, docs, and source links) for M9's reference pages.

## Not in M8

The package manager and the registry (M10); the website (M9); the IDE (the engine phase).
