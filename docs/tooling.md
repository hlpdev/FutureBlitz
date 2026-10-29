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
  `notices/`, and `std/`. fb takes them from beside itself when its bin/ has lld and the folder
  above lib/<target>/ and std/, and else from its build folder, where CMake lays lib/<target>/
  out the same way, copying the C runtime's pieces from the C++ compiler that builds the
  runtime (fb/toolchain.hpp).
- lld's arguments go in a response file, so a link of many objects fits Windows' command line.
- Windows: the pieces of the MinGW-w64 toolchain fb is built with that a program links: crt2.o,
  crtbegin.o and crtend.o, libmingw32, libmingwex, UCRT's and the system's import libraries,
  libgcc, libgcc_eh, libstdc++, and libwinpthread, all static, as today.
- Linux: glibc 2.28's startup files (crt1.o, crti.o, crtn.o), its libraries' stubs and
  libc_nonshared.a, and GCC's crtbegin and crtend, libgcc, libgcc_eh, and libstdc++. The runtime
  and std's bundled libraries are built against glibc 2.28 too, so they ask for no newer symbol:
  the Linux release is built on glibc 2.28 (step 2), and a build folder has its own system's.
  A package's system libraries are found by their development links, like libGL.so, or else by
  the files the system loads, like libGL.so.1, so they need no -dev packages.
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
  compiler writes a table into every binary of where each call is, with its function's FB name,
  its file, and its line, and those of the calls it's inlined into; the runtime walks the stack
  with the platform's unwinder, which gives each frame's function, and finds each frame's call in
  the table, in a beside library's frames too.
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

The extension is editors/vscode/, in TypeScript, with npm and a pinned package-lock.json; its
README says how to build, test, and install it.

- **The grammar** is syntaxes/futureblitz.tmLanguage.json, scope `source.futureblitz`, standalone
  JSON with no build step, so M9's Shiki loads the same file. It has the keywords as the lexer
  has them, in their case only, the contextual ones where they're keywords (Get at a line's
  start, Set(value) inside a Property, which goes to its End Property or the next member, Step
  after a For header's or a range's To or Until, Lib after Extern's convention, From before
  Import, Weak in a capture list, Sendable before Func, and Has and Or in a For Method's Where),
  attributes and Attribute declarations, strings with escapes, interpolation, and format specs (a
  `:` with no space after it, outside the interpolation's brackets), raw and triple-quoted
  strings, Chars, numbers, `//`, nested `/* */`, and `///` comments with their Markdown's code,
  links, and headings, #If lines and their symbols, declarations' names, parameters, and types
  (after a declaration's `:`, a lambda's and an Operator's too, As, Is, Implements, Extends,
  Throws, and Where, and type arguments, `Name<...>` with no space before the `<` and no `&&` in
  them), calls, members, UPPER_CASE constants, and operators. A colon is the ternary's after its
  spaced `?`, or at the start of a line that goes on from one, and a named argument's, a Map
  entry's, or a typed loop's elsewhere. Brackets balance in a parameter list, so a default's call
  doesn't end it. A single-line string, an interpolation in it, and type arguments end with their
  line, as the lexer has them, so one left open doesn't color the rest of the file.
- **The language configuration**: `//` and `/* */` for comment toggling, brackets and quotes that
  close themselves, `///` going on to the next line, numbers as one word as the lexer reads them
  (`0xFF_00`, `1.5e-3`), folding by indentation and by #If ... #End If, and indentation rules for
  the blocks: the line after one's first line indents, and Else, ElseIf, Case, Default, Catch,
  Finally, Until, and End lines go back. A `Then` or `=>` in a comment or a string isn't the
  line's. The rules see one line at a time, so a Select's first Case, a block deeper than Select,
  an Interface's or Extern's Functions without bodies, and a call to a Method named Set with one
  name for its argument, which looks like an accessor, come out wrong; the snippets lay the
  blocks out, and formatting on save fixes the rest.
- **The language server** is `fb-lsp`, from `futureblitz.lspPath`, else beside fb (a release's
  bin/, or a CMake build folder's lsp/ beside cli/), else on PATH, started over stdio through
  vscode-languageclient. Missing, it says so once, with Don't Show Again, and the language status
  item says so too; one that fails to start, doesn't answer initialize in 20 seconds, or stops 4
  times in 3 minutes, is said, with Restart and Show Output, and it's started again after fewer
  stops. A start or stop that comes while another is under way supersedes it, so only one server
  runs. The grammar, tasks, and debugging work without it. `[futureblitz]`'s defaults format on
  save, with 4 spaces, through the editor's format on save and the server's
  `textDocument/formatting`; the client passes `formatOnSave: false` in its initializationOptions,
  so the server's own `willSaveWaitUntil` doesn't format the save a second time.
- **fb** is `futureblitz.path`, else fb on PATH (an .exe or .com on Windows, not a shim that needs
  a shell), else ~/.futureblitz/bin, where the install scripts put it. A setting's
  `${workspaceFolder}` and `~` are expanded, and one that names nothing is said by name.
- **Tasks**, of type `futureblitz`: build, run, test, fmt, and check on the workspace folder's
  package, and build, run, fmt, and check on the open file (`${file}`), with `target`, `release`,
  `filter`, `check`, `args`, and `programArgs` in tasks.json. They run in the workspace folder, so
  fb's paths are relative to it, or with no folder open, in the target's. Problem matchers, which
  take a relative path from the task's folder: `$fb` for diagnostic_render.cpp's
  `path:line:col: error[code]: message` and warnings, `$fb-note` for its notes, and `$fb-runtime`
  for what a program or its tests print with a place: `panic: ... at path:line:col`, `uncaught
  Type[: message] at`, `expect failed: ... at`, `failed[: message] at` (Fail), and `threw
  Type[: message] at`. fb counts columns in code points and VS Code in UTF-16 units, so a task's
  problem after a character outside the BMP on its line is a column left per such character,
  until fb can print UTF-16 columns; the server's diagnostics are right. Without fb, a task says
  why. FutureBlitz: Run, Debug, and Run Tests do the open file's package, or the file.
- **Debugging**: a `futureblitz` launch configuration, with `program` (a .fb file or a package's
  folder), `args`, `cwd`, `env`, `release`, `stopOnEntry`, `terminal`, and CodeLLDB's own commands
  and settings, builds the program in debug with `fb build -o` where fb puts it anyway (a
  package's, or its main file's, build/<name>, another of its files' build/<file>, and a file
  outside any package's beside it), as a task, so a failed build's errors are in Problems and
  nothing launches. It runs in the program's own workspace folder, which in a workspace of
  several isn't always the one F5 passes, or the program's folder with none open. A library has
  no program, which it says before building. Then CodeLLDB (`vadimcn.vscode-lldb`, an extension
  dependency, on every OS) gets an `lldb` configuration whose initCommands load fb's lldb script:
  `futureblitz.debug.lldbScript`, else beside fb (lldb/futureblitz.py, where releases ship it, or
  the source tree's tools/lldb/ for a build folder in it), else the extension's own copy of
  tools/lldb/futureblitz.py. F5 with no launch.json debugs the open file's package, or the file.
  The extension adds breakpoints to .fb files, which CodeLLDB doesn't.
- **Snippets** for If, For, For In, While, Repeat, Loop, Select, Function, Method, lambdas,
  Class, Struct, Interface, Enum, Property, Operator, Try, Lock, Unsafe, Extern, Extend, @Test, #If,
  and From ... Import.
- **Tests**, `npm test` in editors/vscode, which needs Node 22 and isn't in CMake or verify.py:
  the unit tests under Node (the grammar's vscode-tmgrammar-test assertion files, whose excluded
  scopes exclude the scopes under them too, and snapshots of real std and tests/programs code,
  every .fb in the repo leaving nothing open and nothing invalid, every keyword in
  token_kinds.def, the indentation rules and the word pattern, each snippet read by `fb fmt` and
  the Operator's checked, the problem matchers over fb's output, the launch schema and the
  README, finding fb and the server, and lldb loading the script where its lldb has Python), then
  the extension tests in VS Code through @vscode/test-electron, with a stand-in for CodeLLDB that
  records each launch and a stand-in server, on a workspace of three folders and then with no
  folder open: tasks, problems, format on save once, missing, failing, crashing, hung, and
  overlapping servers, and debug launches. `FUTUREBLITZ_FB` names fb for the tests that build,
  and `FUTUREBLITZ_LSP` fb-lsp for those of its diagnostics and formatting on save.
- `npm run package` makes futureblitz.vsix with @vscode/vsce; publishing waits for the accounts.

## The formatter (step 6)

- `fb fmt [files and folders]` formats .fb files in place, a folder's and its folders' but for
  hidden ones and a package's build/, or the current package's without any; `--check` lists the
  files it would change and exits with 1. The LSP formats on save through the compiler's
  `format_source` (compiler/include/fb/format.hpp). One style, with no options but the line
  width, `[format] width` in project.toml, 100 without it, counted in the columns a terminal
  shows: 2 for an East Asian wide character, and none for a combining mark.
- The formatter changes only the space between tokens and where long lines break, so the code
  means what it did: `format_source` parses the result and compares its tokens and tree with the
  original's, and leaves a file it would change otherwise as it is. A file that doesn't parse is
  left as it is, with its errors reported. Formatting is idempotent.

The style, std's own (settled with the user 2026-10-29), which `fb fmt` enforces:

- **Indentation**: 4 spaces a block. A Select's Cases are a block, with their bodies a block
  deeper; so are a Property's Get and Set, an Enum's members, and an Extern block's Functions.
  A declaration's Where lines are a step deeper than it. Else, ElseIf, Case, Catch, Finally,
  Until, and End lines go back to their block's level, and `End Function)` after a lambda's block
  goes back to the statement the lambda is in.
- **Spaces**: one around binary operators, `=`, `=>`, and a ternary's `?` and `:`, and after a
  comma and any other `:`; none inside parentheses, brackets, or a map's braces, `{"a": 1}`, or
  before a call's or an index's bracket. None after a prefix `-`, `!`, or `~`, or around `.`,
  `?.`, and `..`, or between a name and its type arguments, `Map<K, V>()`. A nullable's `?`, and
  As? and Try?, touch what's before them. An Operator's name touches its parameters,
  `Operator []=(`. Inside a string, nothing changes; an interpolation's code is spaced as code,
  and a map's brace next to an interpolation's keeps a space, as `{ {1: 2}.Count}`.
- **Blank lines**: at most one in a row, none at the start of the file or of a block or before a
  block's end, and the file ends with one line break, `\r\n` when its first line ends so.
- **Comments** stay where they are. One on its own line gets the indentation of the line after
  it, or a block's when it's written inside the block before its end; one after code is one
  space after it. Their text is kept, but for spaces at their end.
- **Lines that go on**: a statement goes on to another line only after a comma, an operator,
  `=`, `=>`, or an opening bracket, and inside brackets also before a ternary's `:`. Inside
  brackets, the line lines up one past the innermost open bracket, its items packed; a `:` that
  starts it lines up with its ternary's `?`. Outside brackets, it lines up with the start of the
  statement's expression: after `=` or `=>`, or after Return, If, ElseIf, While, Until, Throw,
  Case, Select, Lock, Implements, Throws, or In, or with the statement's start. A bracket that
  ends its line has what's inside it 4 spaces deeper than that line, 8 for a declaration's
  parameters, so they stand apart from its body, and a closing bracket that starts a line goes at
  the indentation of the line it opened on.
- **Long lines**: the author's line breaks inside a statement stay; lines are never joined. A
  line wider than the width breaks where it may, at the best place on it: a comma or a ternary's
  `:` before an operator, the loosest operator first, at the outermost bracket level, the latest
  there; then after `=` or `=>` outside brackets when all that follows fits on the next line;
  after an opening bracket only when nothing else fits; and after `=` or `=>` last. When what
  comes after the best break still can't fit, the next best is tried. An operator inside a
  ternary's level doesn't break it, and a single-line If breaks only at a comma in its
  statement's brackets. A statement that can't fit at any break, as with a long string, is left
  as written, with only its spacing and indentation fixed.
- **#If** lines, from an outermost #If to its #End If, are left as written, so the formatting is
  the same whatever's defined. The rest is laid out as code without them, so an #If has to hold
  whole statements: one that holds part of one, as a block's first line, makes fb fmt leave the
  file as it is and say so.
- **Constraints**: one constraint, on one of a declaration's own type parameters, goes inline,
  `<T: Hashable>`; more, a `&`, or one that relates two type parameters, `S: Iterator<T>`, go on
  Where lines. The formatter doesn't move them, as that changes the tree: sema warns about the
  other form, with a fix-it the LSP offers as a code action.

## Testing (step 7)

- `@Test` Functions, which take nothing and return nothing or throw, anywhere in a package,
  seeing their file's Private names, are left out of `fb build`; a package's tests/ folder holds
  tests that use it from outside, as another package does.
- std.test: `Expect(a == b)` is built in, and on failure prints the expression and each side's
  value, and the test goes on; `ExpectThrows`, `Fail`, and `Skip`. A panic or an Assert in a test
  fails it.
- `fb test` runs the tests in parallel in one process, and starts it again after a test that
  panics, running the rest, and in a debug build the ones that finished in that run too, whose
  leaks it didn't check; filters by name, a summary, and leaks reported per test.
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
