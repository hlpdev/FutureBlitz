# Tooling (M8)

M8's design, planned with the user on 2026-10-29: everything that makes FutureBlitz something
other people can install and use every day in an editor. Step 0 settled what was still open
(2026-10-29), in decisions 5 to 12.

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
5. **fb runs its own lld directly**, with each target's startup files and libraries, and no C++
   driver between them. lld reads its arguments as UTF-8, so the ANSI code page workarounds of
   Windows' C++ driver go.
6. **Linux programs link against glibc 2.28's stubs**, which fb ships, so a machine needs no
   libc6-dev, and a program runs on any distribution since about 2018 (RHEL 8, Ubuntu 20.04,
   Debian 10). libstdc++ and libgcc stay static, as today.
7. **Releases go to GitHub Releases** as archives with install scripts, and as MSI and .pkg
   installers; self-update waits for M10's dl.futureblitz.dev.
8. **Unsigned for now.** The install scripts download with curl and PowerShell, which
   Gatekeeper and SmartScreen don't stop; signing waits for the accounts it needs.
9. **Stack traces from fb's own table**, in every binary, debug and release, so a player's
   crash names its lines with no debug files shipped.
10. **CodeLLDB on every OS**, which bundles its own lldb, with one script of fb's that shows
    FutureBlitz values.
11. **Tests are `@Test` Functions anywhere**, seeing their file's Private names, and a package's
    tests/ folder; `Expect` is built in, and one process runs them in parallel.
12. **Doc comments are Markdown**, Rust's way: a summary sentence, sections like `# Errors` and
    `# Examples`, and `[Name]` links.

## The toolchain (steps 0 and 1)

- fb ships lld and what each target links besides the program: the runtime, std's bundled
  libraries, and the C runtime's startup files and libraries, so building needs no C++ compiler.
  fb runs lld itself (ld.lld, its MinGW driver, and ld64.lld), as one binary beside fb.
- A release's folder: `bin/` (fb, fb-bindgen, lld, and on Windows LLVM-C.dll and libclang.dll),
  `lib/<target>/` (the runtime's archives, std's bundled libraries, and the C runtime's pieces),
  and `std/`. fb finds them from its own path, and a build folder from CMake's, as today.
- Windows: the pieces of the MinGW-w64 toolchain fb is built with that a program links: crt2.o,
  crtbegin.o and crtend.o, libmingw32, libmingwex, UCRT's and the system's import libraries,
  libgcc, libgcc_eh, libstdc++, and libwinpthread, all static, as today.
- Linux: glibc 2.28's startup files (crt1.o, crti.o, crtn.o), its libraries' stubs and
  libc_nonshared.a, and GCC's crtbegin and crtend, libgcc, libgcc_eh, and libstdc++. The runtime
  and std's bundled libraries are built against glibc 2.28 too, so they ask for no newer symbol.
- macOS: Apple's Command Line Tools for the SDK (libSystem and libc++, which are the OS's own),
  which can't be redistributed; `fb` finds it with `xcrun` and says how to install it when it's
  missing.
- fb-bindgen ships clang's own headers and the target's C headers (MinGW's, glibc 2.28's, or
  the SDK's), so it reads a C header with no compiler installed.
- A target's pieces come from the toolchain that builds fb's release for it; cross-compiling to
  another OS stays for later.

## Releases (step 2)

- Versioned builds of fb for windows-x64, linux-x64, linux-arm64, macos-arm64, and macos-x64,
  from CI, on GitHub Releases, when a `v<version>` tag is pushed.
- Each target's archive (.zip on Windows, .tar.gz elsewhere), and install.sh and install.ps1,
  which fetch the latest, unpack it under ~/.futureblitz, and put fb on PATH; running one again
  updates. An MSI for Windows (WiX) and a .pkg for macOS (pkgbuild) install the same folder.
- Unsigned: an archive or installer downloaded in a browser warns until signing comes.
- `fb --version` prints the version, the commit, and the target.
- `fb new <name>` makes a package: project.toml, main.fb, and a .gitignore.

## Debug info (step 3)

- LLVM's DIBuilder: functions, lines, locals, parameters, and types (Structs, Classes, arrays,
  Strings, nullables, Enums), in debug builds, and line tables in release. DWARF everywhere, and
  on Windows CodeView too, which lld writes as a PDB beside the executable for Visual Studio and
  WinDbg.
- Panics and uncaught errors print a stack trace of FutureBlitz names, files, and lines. The
  compiler writes a table into every binary of its functions' address ranges, FB names, and each
  call's file and line, inlined calls included; the runtime walks the stack with the platform's
  unwinder and reads it, in a beside library's frames too.
- VS Code debugs through CodeLLDB on every OS; a Python script fb ships, which the extension
  loads, shows Strings, arrays, Maps, nullables, and Classes as their values, not their bytes.

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
  on save. One style, with no options but the line width, `[format] width` in project.toml.
- The style std is written in: 4 spaces, a width of 100, a space around binary operators and
  after commas and colons, spaces inside braced lists `{ a, b }` and none inside parentheses or
  brackets, at most one blank line in a row and none at a block's start or end. A long line
  breaks after a comma or before an operator, continuing 4 spaces deeper; a list that doesn't fit
  goes one item per line.
- Comments stay where they are, and the formatter changes no code's meaning, which its tests
  check by parsing before and after.

## Testing (step 7)

- `@Test` Functions, which take nothing and return nothing or throw, anywhere in a package,
  seeing their file's Private names, are left out of `fb build`; a package's tests/ folder holds
  tests that use it from outside, as another package does.
- std.test: `Expect(a == b)` is built in, and on failure prints the expression and each side's
  value, and the test goes on; `ExpectThrows`, `Fail`, and `Skip`. A panic or an Assert in a test
  fails it.
- `fb test` runs the tests in parallel in one process, and starts it again after a test that
  panics, running the rest; filters by name, a summary, and leaks reported per test.
- Doc tests: the examples in `///` comments' `# Examples` compile and run as tests.

## Docs from the code (step 8)

- `///` doc comments on declarations, in Markdown, Rust's way: the first sentence is the summary,
  sections like `# Errors`, `# Panics`, and `# Examples`, parameters in backticks, and `[Name]`
  or `[Type.Member]` links, which fb doc resolves and checks.
- std's comments on its declarations become `///` ones.
- `fb doc`, which writes a package's public API as JSON (modules, types, members, signatures,
  docs, and source links) for M9's reference pages.

## Not in M8

The package manager and the registry (M10); the website (M9); the IDE (the engine phase).
