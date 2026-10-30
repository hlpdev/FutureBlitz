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
- A release's folder: `bin/` (fb, fb-bindgen, fb's language server, lld, and the shared
  libraries they load: LLVM-C.dll and libclang.dll on Windows), `lib/<target>/` (the runtime's
  archives, std's bundled libraries, the C runtime's pieces, and in `include/` the target's C
  headers), `lib/clang/` (clang's own headers), `notices/`, `std/`, and `lldb/futureblitz.py`,
  which the VS Code extension finds beside the folder bin/ is in. fb takes them from beside
  itself when its bin/ has lld and the folder above lib/<target>/ and std/, and else from its
  build folder, where CMake lays lib/<target>/ out the same way, copying the C runtime's pieces
  from the C++ compiler that builds the runtime (fb/toolchain.hpp).
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
  from .github/workflows/release.yml, which a pushed `v<version>` tag runs, or a dispatch on a
  branch to check it (`gh workflow run release.yml --ref <branch>`). A tag makes a **draft**
  GitHub Release of the assets, which is published by hand; the tag has to be CMakeLists.txt's
  version.
- What fb itself needs: Windows 10 or newer (x64, which arm64 Windows runs too), Ubuntu 22.04 or
  newer, or another Linux with glibc 2.35, and macOS 15 or newer (the user, 2026-10-29). The
  programs it builds run on glibc 2.28, and on the macOS fb runs on.
- Each target's archive, `futureblitz-<version>-<target>.zip` on Windows and `.tar.gz`
  elsewhere, holds one folder of that name, laid out as above. `cmake --install <build> --prefix
  <folder>` writes it (cmake/install.cmake), under `DESTDIR` too, and the install test in ctest
  installs the build into a folder of its own and builds and runs `fb new`'s package there with
  PATH empty.
- tools/install/install.sh (Linux and macOS) and install.ps1 (Windows), assets of each release,
  install the latest release, one given with `--version` (`-Version`, or `FUTUREBLITZ_VERSION`
  for both, with or without its v), or an archive with `--archive` (`-Archive`), under
  ~/.futureblitz (%USERPROFILE%\.futureblitz, or `FUTUREBLITZ_HOME`). An archive has to be this
  machine's target, and its fb has to run, before the release's folders replace those there;
  anything else there stays. Then its bin goes on PATH: in the shell's startup files, .zshrc,
  .profile and .bashrc for bash on Linux, the first there is of .bash_profile, .bash_login, and
  .profile for bash on macOS (else a new .bash_profile), fish's config.fish, else .profile; or
  in Windows' user PATH, as it's written there, %VAR%s and its REG_EXPAND_SZ kept.
  `--no-modify-path` (`-NoModifyPath`) leaves it, and says what to add. Running one again
  updates. install.ps1 runs in a script block of its own, parameters and all, so `irm | iex`
  leaves the session's preferences, TLS, and variables as they were, those named like its
  parameters too; and it runs an archive's fb only when the file is a program, since Windows
  shows a window that waits to be closed for one that isn't.
- The MSI (WiX 5, tools/install/futureblitz.wxs) installs the same folder for the user alone,
  in %USERPROFILE%\.futureblitz with its bin on the user's PATH, and uninstalls both; a newer one
  replaces an older one. The .pkg (pkgbuild) installs it for every user in
  /usr/local/futureblitz, with /etc/paths.d/futureblitz putting its bin on PATH, as Go's does.
- fb's own executables run with nothing else installed. On Windows they link MinGW's runtime
  statically (where winpthread links statically: CLion's MinGW, whose winpthread is built for
  UCRT, gets libwinpthread-1.dll beside them instead), and LLVM-C.dll and libclang.dll, which
  hold all of LLVM, go beside them. On Linux they link libstdc++ and libgcc statically, and in a
  release LLVM's static libraries (`FB_LLVM_STATIC`); on macOS Homebrew's LLVM has only its
  shared libraries. Elsewhere than Windows, cmake/bundle.cmake copies into bin/ the shared
  libraries that fb's executables and lld load, but the system's, and has them found there:
  a RUNPATH of $ORIGIN on Linux (patchelf), and on macOS @rpath names, an @loader_path rpath
  (install_name_tool), and each binary signed again; each one's license goes in
  notices/shared/.
- Linux: fb is built on Ubuntu 22.04 with GCC 14 and apt.llvm.org's LLVM, while lib/<target>/,
  with glibc 2.28's and the kernel's headers in its include/, comes from a runtime-only build
  (`FB_RUNTIME_ONLY`, no LLVM) in a manylinux_2_28 container (AlmaLinux 8, gcc-toolset-14),
  which the release takes as `FB_TOOLCHAIN_PREBUILT`. apt.llvm.org's lld and libclang load its
  libLLVM, which goes beside them with what it loads.
- macOS: fb needs Apple's Command Line Tools for the SDK, and is built with Homebrew's LLVM on
  macOS 15, for macOS 15 and newer.
- fb-bindgen finds clang's headers in lib/clang/ and the target's C headers in
  lib/<target>/include/ (MinGW-w64's and winpthreads', or glibc 2.28's and the kernel's), or on
  macOS the SDK's, from `xcrun`; a build folder has libclang's own and the C compiler's.
- notices/: std's bundled libraries', some of which fb's executables have in them too; LLVM's
  (for LLVM, clang, and lld) and toml++'s, in fb's executables, and nlohmann/json's, in
  fb-lsp; MinGW-w64's, winpthreads', and
  GCC's on Windows, with libxml2's, which the LLVM installer links into lld.exe; and GCC's,
  glibc's, and the Linux headers' on Linux; from cmake/notices/, and the bundled shared
  libraries' own, in notices/shared/, from their Debian packages or Homebrew kegs.
- The workflow checks each release on a fresh machine: Windows' runner with PATH cut to the
  system's, macOS' with PATH cut to the system's and a home of its own, with no startup files,
  and a plain ubuntu:22.04 container with no compiler, each installing with its script, and on
  Windows and macOS with the MSI and the .pkg too, then running `fb new hello` and `fb run` in
  it; and a program fb builds there runs in almalinux:8.
- Unsigned: an archive or installer downloaded in a browser warns until signing comes.
- `fb --version` prints the version, the commit, from git when CMake configured the build, of the
  checkout whose top folder the source is, else "unknown", and the target:
  `fb 0.1.0 (abc1234, windows-x64)`.
- `fb new <folder>` makes a package there, named by the folder's own name, however it's
  written (`fb new .` names it by the current folder's), which is checked as project.toml's names
  are, and on Windows isn't a device's, like CON: project.toml, a main.fb that imports std.core
  and prints "Hello, world!", and a .gitignore of build/. It refuses a folder that's there and
  isn't empty.

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
- Formatting through `format_source`, with the package's `[format] width`: the whole document,
  and on save, through the editor's own format on save, or willSaveWaitUntil when the client's
  initializationOptions say `formatOnSave`.

How it's built (2026-10-29):

- **fb-lsp** is an executable of its own beside fb, which an editor starts and talks to in
  JSON-RPC over stdin and stdout, read and written with nlohmann/json. Positions count UTF-16 code
  units, or UTF-8 bytes when the client offers them. lsp/ is the `fb_lsp_server` library, which
  tests/lsp drives through its messages in process, and main.cpp.
- **Units.** One compilation covers a unit: a package with its tests/ folder, as fb test has it,
  the standard library, or a .fb file outside a package. Each package in the client's folders,
  and std when it's among them, is checked whole once, in the background, which gives every
  file's diagnostics and the import graph.
- **A check** is the driver's `check_files()`: some files of one unit, the roots, each a module of
  its own, with what they import, and no other file. The open documents' texts are read in place
  of the disk's. Sema checks a compilation's modules whole, as it does a build's, so what makes it
  incremental is what a check loads: an edit of a file that few others import loads few modules.
- **An edit** checks the edited file once there's been no other edit for 150 ms, on a thread for
  edited documents, and publishes its diagnostics. Then, 400 ms after, a thread for the rest
  checks the files that import it, directly or not, as the graph of the checks before says, one
  check for each unit they're in. Every file imports std/core/ and std.collections.map without
  saying so. A file with top-level code is the main file of its own check; one that's another
  main file in a check of several has its top-level code left out, as fb test's build leaves it,
  so it's checked again on its own. A check that started before a newer one doesn't replace that
  one's diagnostics.
- **Requests** answer from the check of the document's current text, which they wait for, kept
  for each open document; references and renames check the file that declares the name with every
  file that imports it, and their doc comments' examples, as fb test compiles them, so a rename
  changes an example's code in its doc comment too. A prebuilt package's declarations are in its
  interface file, which definitions go to. Sema records each use of a symbol (`analysis_t::uses`,
  symbol_use_t lists the kinds), which hover, definitions, references, renames, and semantic tokens look names up in,
  and a test checks a program with each kind of use; and a member being typed, "x.", is a member
  with an empty name, whose object sema works out quietly, which completion takes the members
  from. Document and workspace symbols parse the file alone.
- **Renames** change a Method or Property with the ones that override it or that it overrides,
  and with the Interface members it implements and their other implementations, from the checks
  of each of their files and what imports them. A rename is refused when another declaration of
  the new name is where the old one is declared (a variable's function, or a member's type), when
  code where the name is used sees the new name already, for a New, a module's qualifier, an
  Import's As name, and a declaration outside the workspace, and for a name that isn't ASCII
  letters, digits, and '_'.
- **Shared work.** A check's driver parses each std file it doesn't load, for the error that says
  which module declares a name, which took most of a check's time; the checks share what it finds
  (`std_names_cache_t`), by each file's text.
- **Docs on hover** are a declaration's `///` doc comment, the AST's (step 8), as Markdown without
  its examples' hidden lines: a Function's, a type's, a member's, and an Enum member's.
- **Files changed outside the editor**, as by a git checkout, come in workspace/didChangeWatchedFiles,
  which the server asks the client for when it can: each is checked again with what imports it,
  and a changed project.toml's package whole.
- In a debug build, a std file's check, std/text/fuzzy.fb's, has 17 of std's 206 modules, and an
  edit's takes about 300 ms; the requests after it take 1 to 25 ms. std as a folder is checked
  whole in about 20 s, once.

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
  strings, Chars, numbers, `//`, nested `/* */`, and `///` and `//!` comments with their
  Markdown's code, links, and headings, #If lines and their symbols, declarations' names,
  parameters, and types (after a declaration's `:`, a lambda's and an Operator's too, As, Is,
  Implements, Extends, Throws, and Where, and type arguments, `Name<...>` with no space before
  the `<` and no `&&` in them), calls, members, UPPER_CASE constants, and operators. A colon is the ternary's after its
  spaced `?`, or at the start of a line that goes on from one, and a named argument's, a Map
  entry's, or a typed loop's elsewhere. Brackets balance in a parameter list, so a default's call
  doesn't end it. A single-line string, an interpolation in it, and type arguments end with their
  line, as the lexer has them, so one left open doesn't color the rest of the file.
- **The language configuration**: `//` and `/* */` for comment toggling, brackets and quotes that
  close themselves, `///` and `//!` going on to the next line, numbers as one word as the lexer
  reads them (`0xFF_00`, `1.5e-3`), folding by indentation and by #If ... #End If, and indentation rules for
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
  fb's paths are relative to it, or with no folder open, in the target's, with `FB_PATHS=absolute`,
  which has fb name files by their whole paths: VS Code takes a problem matcher's `${cwd}` with no
  folder open to be its own process's folder, not the task's, so those tasks use `$fb-whole`,
  `$fb-note-whole`, and `$fb-runtime-whole`, the same matchers with absolute paths. Their owner is
  `futureblitz-tasks`, not the language client's diagnostics' `futureblitz`, whose changes would
  replace or clear a task's problems, and which VS Code keeps from the extension host. Problem
  matchers: `$fb` for diagnostic_render.cpp's `path:line:col: error[code]: message` and warnings,
  `$fb-note` for its notes, and `$fb-runtime` for what a program or its tests print with a place: `panic: ... at path:line:col`, `uncaught
  Type[: message] at`, `expect failed: ... at`, `failed[: message] at` (Fail), and `threw
  Type[: message] at`. fb's diagnostics count columns in code points, and with `FB_COLUMNS=utf-16`
  in UTF-16 code units, as VS Code does, which the tasks set; a running program's places, which
  the compiler writes in, still count code points, so one after a character outside the BMP on
  its line is a column left per such character. Without fb, a task says why. FutureBlitz: Run, Debug, and Run Tests do the open file's package, or the file.
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
  space after it. Their text is kept, but for spaces at their end, which a doc comment keeps,
  as its Markdown's line breaks and its examples' strings have them; the meaning check compares
  doc comments' text too.
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
- Doc tests: the examples in `///` comments' `# Examples` compile and run as tests, each a test
  named after its declaration, like `game.shapes.Circle.Grow (example)`, in a package of their
  own, `<name>.doctests`, that uses the package from outside (spec 09). `fb test std` runs std's,
  and `fb test` on a `.fb` file outside a package runs that file's.

## Docs from the code (step 8)

- `///` doc comments on declarations, in Markdown, Rust's way: the first sentence is the summary,
  sections like `# Errors`, `# Panics`, and `# Examples`, parameters in backticks, and `[Name]`
  or `[Type.Member]` links, which fb doc resolves and checks. `//!` lines at the top of a file are
  its module's (spec 01).
- The parser attaches each to its declaration in the AST, from the doc comments the lexer finds:
  `ast_t::doc()` gives a declaration's by where its name is, as a symbol's span is, which is what
  the LSP's hover shows, and `module_doc()` a file's. `read_doc()` (compiler/include/fb/doc.hpp)
  reads one's Markdown into its summary, sections, links, and examples.
- A prebuilt library's interface file keeps its `///` and `//!` docs, so `fb doc` and the hover
  have them for a prebuilt package, and a `// line` comment before each declaration and its
  module's doc, the line of the source it comes from, so `fb doc` gives a prebuilt package the
  lines its source has. Its ABI hash leaves their lines out, and the module line
  counts they change, so a doc's new words are the same build; a doc that adds or takes out lines
  moves the code's lines, which the binary's stack traces and debug info name, and so is another
  (spec 11).
- std's comments on its declarations became `///` ones, and its files' first comments `//!`
  ones. Every Public declaration of std's, and every member its users see, has a summary, of its
  own or from the Interface member it implements or the Class member it overrides; a test checks
  each is in `fb doc std`'s JSON with one.
- `fb doc [package | std]` writes a package's public API as JSON, to `build/<name>.json` in its
  folder, or for std, std.json in the current one, unless `-o` names a file, or `-` for stdout,
  after a warning for each link that names nothing and each doc comment on nothing. `--release`
  documents a release build's `#If` branches.
- An example has `Import std.core`, `Import std.test`, and its declaration's module without
  writing them, and writes any other Import at its top, as a file does; a line that starts with
  `# ` is compiled but not shown, as Rust's are, for setup the reader doesn't need. Its
  declarations go outside its test, and the rest is its body. It's compiled as a module of its own,
  whose lines are mapped to their place in the doc comment, so its errors, failures, panics, and
  stack traces show the comment's lines. A fence with `fb no_run` only compiles, for code that
  would wait on a network or a person; one with another language isn't a test.

fb doc's JSON, format 1, which M9's reference pages read. Every object has every key shown, but
for the ones marked as only sometimes there:

```json
{
  "format": 1,
  "package": "std",
  "version": "0.1.0",
  "modules": [
    {
      "name": "std.collections.stack",
      "file": "collections/stack.fb",
      "doc": "<doc> or null",
      "items": ["<item>"]
    }
  ]
}
```

- A module's `file` is its path from its package's folder, std's from std/. A module with no
  Public declarations and no doc comment is left out, and the modules are sorted by name.
- An item is a declaration its users see: a module's Public ones, and its Extends of a Public
  type, a built-in, or every `T[]`, with their Public members. Its keys:
  - `kind`: function, struct, class, interface, attribute, enum, alias, global, const, field,
    static field, method, static method, constructor, operator, property, enum member, or extend.
  - `name`: an Operator's is its operator, like `+`, a New's is `New`, and an Extend's is its type
    without its type arguments, like `String` or `T[]`.
  - `path`: its module's name, its type's name for a member, and its name, as links name it:
    `std.collections.stack.Stack.Push`, or for an Operator, `...Vec3.Operator+`. An Extend's is
    `<module>.Extend.<name>`, and its members' are `<module>.<name>.<member>`, its type's in its
    module. Overloads share their path.
  - `signature`: its first line as written, with its visibility and modifiers but not its
    attributes or comments, and a line break as a space: a function's through its result and
    Throws, a type's through its Implements, a variable's through its type, or its value for a
    Const or one without a type, an Enum member's through its value.
  - `attributes`: each attribute before it as written, like `@PanicsAtCaller`.
  - `visibility`: Public, or Protected for a Class's member that only the Classes extending it
    see.
  - `file` and `line`: where it starts, at its first attribute, its line from 1.
  - `doc`: its doc comment, or null; a member without one that implements an Interface's member
    or overrides a base Class's has that one's, and `doc_from`, only then, is its path.
  - `extends`, only on an Extend: the path of the type it extends, or a built-in's name.
  - `members`, on a type, an Enum, and an Extend: their items, in source order, a Struct's and a
    Class's Public members and a Class's Protected ones, an Interface's all, and an Enum's.
- A doc: `summary`, its first sentence, empty when it starts with a heading or code; `markdown`,
  its text without its examples' hidden lines; `sections`, each `heading` (empty for what comes
  before the first) and its `markdown`; `links`, each `[Name]` in it, a heading's too, but for a
  type parameter's, like `[T]`, its `text` as written without backticks, a `()`, or type
  arguments, as `[Map<K, V>.Get]` is `Map.Get`, and the `path` it names, or a built-in's name
  with `builtin` true, or an empty path for one that names nothing; and `examples`, each one's
  `code` as shown, the `line` of its opening fence, and whether it `runs` or only compiles. A
  module's doc's examples run as a declaration's do, named after the module.
- Members are what code outside the package sees: an Operator has its type's visibility, and
  an Override that says none has what it overrides'. A link names what code where it is sees:
  another file's Private member, or another package's Package one, names nothing.
- An Extend of one array type, like `Extend UInt8[]`, is named for it, `UInt8[]`, and an
  Extend of every array, `T[]`. Two Extends of one type in a module share their path, as
  overloads do.

## Not in M8

The package manager and the registry (M10); the website (M9); the IDE (the engine phase).
