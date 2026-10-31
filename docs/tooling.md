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
7. **Releases go to GitHub Releases** as archives with install scripts, as MSI, setup.exe, .pkg,
   .deb, and .rpm installers, and as the VS Code extension's .vsix; from there, when the user
   says, to winget, a Homebrew tap, apt and dnf repositories on dl.futureblitz.dev, and the
   Marketplace and Open VSX (the user, 2026-10-30). Self-update waits for M10.
8. **Windows is signed, macOS isn't.** fb's executables, the MSI, and setup.exe are signed with
   Azure Artifact Signing once its secrets are set (the user, 2026-10-30). macOS stays unsigned:
   the install script and Homebrew download with curl, which Gatekeeper doesn't stop.
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
  GitHub Release of the assets, which is published by hand.
- **The version is the tag's**: v1.0.0 builds 1.0.0, which CMake's `FB_VERSION` gives fb
  (`fb --version`, and fb-lsp's initialize), the MSI and setup.exe, the .pkg, the .deb and
  .rpm, the .vsix, and every file's name. A tag is v and three numbers; one that isn't
  CMakeLists.txt's version only warns, as builds from that commit say CMakeLists.txt's. Without
  `FB_VERSION`, and in a dispatch on a branch, it's CMakeLists.txt's; a dispatch on a
  v<version> tag is that tag's, and drafts its release too. Each install's test, by script,
  MSI, setup.exe, .pkg, Homebrew, .deb, and .rpm, checks `fb --version` says it.
- **One sentence** describes FutureBlitz in every package manager, the .deb and .rpm's summary,
  winget's, Homebrew's, and the .vsix's: "A compiled, general-purpose language with BASIC's
  readability and native speed." (tools/release/packages.py's `SUMMARY`, which a test keeps
  the same everywhere).
- **What a release has**: for each target its archive; on Windows the MSI and
  `futureblitz-<version>-windows-x64-setup.exe`; on macOS the .pkg; on Linux
  `futureblitz_<version>-1_<amd64|arm64>.deb` and `futureblitz-<version>-1.<x86_64|aarch64>.rpm`;
  `futureblitz-<version>.vsix`; install.sh and install.ps1; and SHA256SUMS, every other file's
  hash, as sha256sum writes it, which publish.yml makes winget's manifests and the formula from.
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
  updates. install.ps1 runs an archive's fb only when the file is a program, since Windows
  shows a window that waits to be closed for one that isn't.
- A release they download is checked against its SHA256SUMS before it's unpacked, and one that
  doesn't match isn't installed. The latest is found by that file too, from GitHub's
  `releases/latest/download/SHA256SUMS` redirect, which needs no API, so the API's 60 requests
  an hour for an address without a token, as CI runners share, never stops an install. An
  `--archive` is installed as it is. install.sh's body is a function its last line calls, so a
  download cut short runs none of it.
- The site serves both (M9), which is how they're run: `curl -fsSL
  https://futureblitz.dev/install.sh | sh` (`sh -s -- --version 1.0.0` with arguments), and
  `irm https://futureblitz.dev/install.ps1 | iex` (`iex "& { $(irm ...) } -Version 1.0.0"` with
  arguments); each release's copies stay a fallback. iex runs install.ps1 in the caller's
  session, so all of it is a script block, `& {
  param(...) ... } @args`, whose parameters, preferences, TLS, and function are its own: a
  script's own param() would set the session's $Version, $Archive, and $NoModifyPath. It's
  ASCII, as Windows PowerShell's irm reads a script with no charset as Latin-1, so the site
  serves it as `text/plain; charset=utf-8`, which also stops a browser running it. Its test
  runs it dot-sourced and by iex, over a session's own variables of those names.
- The MSI (WiX 5, tools/install/futureblitz.wxs) installs the same folder for the user alone,
  in %USERPROFILE%\.futureblitz with its bin on the user's PATH, and uninstalls both; a newer one
  replaces an older one. Each version's ProductCode is its own, a UUID made from the version
  (`tools/release/packages.py product-code`), which the build checks the MSI has, so winget's
  manifest knows it without reading the MSI. setup.exe (tools/install/bundle.wxs) is a WiX Burn
  bundle around it, with WiX's standard window, which links the license, and installs the MSI
  the same way; `/quiet` installs it with no window, and `/uninstall` takes it away. The .pkg
  (pkgbuild) installs it for every user in /usr/local/futureblitz, with
  /etc/paths.d/futureblitz putting its bin on PATH, as Go's does.
- **Signing (Windows)**: with the AZURE_TENANT_ID, AZURE_CLIENT_ID, and AZURE_CLIENT_SECRET
  secrets and the AZURE_SIGNING_ENDPOINT, AZURE_SIGNING_ACCOUNT, and AZURE_SIGNING_PROFILE
  variables (docs/release-setup.md), azure/artifact-signing-action (once trusted-signing-action)
  signs fb.exe, fb-lsp.exe, and fb-bindgen.exe before they're archived, then the MSI before it
  goes in setup.exe, then setup.exe's engine, taken out with `wix burn detach` and put back with
  `reattach`, then setup.exe; each is timestamped, and the build checks every signature, the
  archive's fb.exe's too. With none of them set, as in a fork, it's unsigned, with a warning;
  with some, the build fails. LLVM's DLLs and lld are LLVM's, and stay as they come.
- **Linux packages**: nfpm (tools/release/nfpm.yaml), checked against its release's checksums,
  makes a .deb and an .rpm of the release's folder, which go in **/usr/lib/futureblitz** as the
  archive has it, with /usr/bin/fb, fb-lsp, and fb-bindgen linking to its bin/; fb finds lib/
  and std/ beside its bin/ through the link, as it takes its own path from /proc/self/exe. They
  depend on what fb's executables load that isn't beside them (cmake/bundle.cmake): the .deb on
  `libc6 (>= 2.35)`, libgcc-s1, `libstdc++6 (>= 12)`, zlib1g, libzstd1, liblzma5, and libtinfo6,
  and the .rpm on `glibc >= 2.35`, libgcc, libstdc++, and zlib's, zstd's, xz's, and libtinfo's
  sonames, so Ubuntu 22.04, Debian 12, and Fedora 36 and newer have them; nothing that compiles.
  With REPO_SIGNING_KEY the .rpm is signed, which the dnf repository needs. The workflow installs
  the .deb with apt in ubuntu:22.04 and the .rpm with dnf in fedora:43, on x64 and arm64, runs
  `fb new`'s package with /usr/bin/fb, and removes them.
- **The VS Code extension**: release.yml's vscode job sets its package.json to the version, runs
  its unit tests, and makes the .vsix with vsce. package.json's publisher, `futureblitz`, is the
  Marketplace's publisher and Open VSX's namespace; its icon is assets/brand/mark-tile-128.png,
  copied in as images/icon.png when it compiles, with the LICENSE (Apache-2.0).
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
  and a plain ubuntu:22.04 container with no compiler, each installing with its script, on
  Windows with the MSI and setup.exe too, on macOS with Homebrew and the .pkg, and on Linux
  with the .deb, then running `fb new hello` and `fb run` in it; and a program fb builds there
  runs in almalinux:8.
- macOS is unsigned, so an archive or a .pkg downloaded in a browser warns; Windows' files do
  until signing's secrets are set.

### Package managers

.github/workflows/publish.yml sends a **published** release to each package manager ticked in
its dispatch, never on its own: `gh workflow run publish.yml -f version=1.0.0 -f winget=true -f
homebrew=true -f linux=true -f vscode=true`. Each job takes the release's files from its GitHub
Release, checked against SHA256SUMS. docs/release-setup.md has the accounts, keys, and secrets,
made once, and its launch checklist, whose tools/release/launch_check.py checks a published
release has every file, signed installers, winget's manifest, and the tap's formula at its
version.

- **winget**: tools/release/packages.py makes FutureBlitz.FutureBlitz's manifests (schema
  1.10.0: the version's, the installer's, and en-US's) from SHA256SUMS: the MSI, `wix`, for the
  user, x64 (which arm64 Windows runs), with its ProductCode and UpgradeCode, and its Commands.
  The job brings the fork (WINGET_FORK, hlpdev/winget-pkgs) up to microsoft/winget-pkgs, puts
  them in `manifests/f/FutureBlitz/FutureBlitz/<version>/` on a branch of it through GitHub's
  API, and opens the pull request, with WINGET_TOKEN. Then `winget install FutureBlitz.FutureBlitz`.
  release.yml makes them for each release's MSI too, which winget validates where the runner
  has it.
- **Homebrew**: packages.py makes futureblitz.rb from SHA256SUMS, for macOS' two archives (macOS
  15 and newer) and Linux's two: it installs the release's folder into libexec, links fb,
  fb-lsp, and fb-bindgen into bin, and its test block runs `fb --version` and builds and runs
  `fb new`'s package. The job pushes it to HOMEBREW_TAP (hlpdev/homebrew-tap) as
  Formula/futureblitz.rb, with HOMEBREW_TAP_TOKEN. Then `brew install hlpdev/tap/futureblitz`.
  release.yml installs each Mac's archive with it, from a tap of its own, and runs `brew test`.
- **apt and dnf**: tools/release/repo.py makes both repositories from every release's packages,
  those already in R2 and this release's, signed with REPO_SIGNING_KEY's key (RSA 4096, no
  passphrase, which only signs), and the job uploads them to the R2 bucket R2_BUCKET that
  dl.futureblitz.dev serves, packages first and indexes last, so no index names a package that
  isn't there yet. What never changes, as its name is its version or its hash, is cached for a
  year, immutable: the packages, apt's indexes by-hash, and createrepo_c's checksum-named
  files; nothing is deleted, so an older InRelease or repomd.xml finds what it names. What
  changes isn't cached (`no-cache`), and goes up last, each signature right after what it
  signs: apt's plain indexes, Release, Release.gpg, and InRelease, then repomd.xml and its
  .asc. apt reads InRelease, signature and all, and with `Acquire-By-Hash: yes` fetches each
  index by its hash, so the InRelease a client has, old or new, matches the indexes it gets;
  dnf has no such thing, and between repomd.xml's upload and its .asc's, a client can get one
  without the other.
  - `dl.futureblitz.dev/apt`: `stable main`, for amd64 and arm64, a pool in
    `pool/main/f/futureblitz/`, `dists/stable/main/binary-<arch>/Packages` and `Packages.gz`,
    each in that folder's `by-hash/SHA256/` by its hash too, and `dists/stable/Release`,
    `Release.gpg`, and `InRelease`, which repo.py writes itself, so it needs only gpg; and the
    key as futureblitz.gpg, for signed-by, and futureblitz.asc. The packages' Maintainer is
    `FutureBlitz <noreply@futureblitz.dev>`, as Debian's has to be an address, which takes no
    mail.

    ```sh
    curl -fsSL https://dl.futureblitz.dev/apt/futureblitz.gpg | sudo tee /usr/share/keyrings/futureblitz.gpg > /dev/null
    echo "deb [signed-by=/usr/share/keyrings/futureblitz.gpg] https://dl.futureblitz.dev/apt stable main" | sudo tee /etc/apt/sources.list.d/futureblitz.list
    sudo apt update && sudo apt install futureblitz
    ```

  - `dl.futureblitz.dev/rpm`: the .rpms in `packages/`, createrepo_c's `repodata/`, and
    `repomd.xml.asc`, its signature; futureblitz.repo, with `gpgcheck` and `repo_gpgcheck`, and
    the key as futureblitz.asc. repo.py refuses an .rpm that isn't signed with the key.

    ```sh
    sudo curl -fsSL -o /etc/yum.repos.d/futureblitz.repo https://dl.futureblitz.dev/rpm/futureblitz.repo
    sudo dnf install futureblitz
    ```

  - dl.futureblitz.dev's packages are `/<name>/<version>/` (registry-api.md), and `apt` and
    `rpm` are reserved names, with `fb`, `futureblitz`, and `releases`, so no package's is
    either.
- **The VS Code extension**: the job publishes the release's .vsix to the Marketplace with vsce
  (editors/vscode's own, pinned) and VSCE_PAT, and to Open VSX with ovsx and OVSX_PAT.
- **Tests**: tests/tools/release_tests.py (in ctest) checks the manifests, the formula, and the
  ProductCode, the one sentence and the Maintainer, and builds the apt repository from fake
  .debs with a key made for the test, which gpgv checks as apt does, that a changed Release
  fails, that each index's by-hash copy is there, and that a .deb cut off anywhere, or whose
  control.tar is broken, is refused; with nfpm on PATH, it makes nfpm.yaml's .deb, and checks
  its links and that the repository takes it.
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
  of the disk's. Sema checks the declarations of every module a check loads, and only the roots'
  function bodies (M11's step 1), so an edit of a file that few others import loads few modules,
  and their bodies aren't checked again.
- **An edit** is parsed at once, and its syntax errors published (M11's step 2), and it checks
  the edited file once there's been no other edit for 150 ms, on a thread for edited documents,
  and publishes its diagnostics. Then, 400 ms after, a thread for the rest
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
  (`std_names_cache_t`), by each file's text, and each file's tokens, and what the disk said
  (`disk_cache_t`), until the client says files changed (M11's step 1).
- **Semantic tokens** are the lexer's keywords, literals, and comments, with each name as what it
  names over them. A string's or a character's tokens leave out its escapes, `{{` and `}}`
  among them, and its interpolations' braces, which VS Code would otherwise color as the string,
  over the grammar's own colors for them.
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
  runs; a client that's still starting gets up to 2 seconds to finish before it's stopped, and is
  stopped once it does, as vscode-languageclient sets up a client's features after it says it's
  running, and one stopped in between would keep them. The grammar, tasks, and debugging work without it. `[futureblitz]`'s defaults format on
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
- `npm run package` makes futureblitz.vsix with @vscode/vsce. Each release has one at its
  version, which publish.yml publishes to the Marketplace and Open VSX (Releases).

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
  after an opening bracket only when nothing else fits, one after which all that follows fits
  on the next line first, so brackets in brackets break once, `JsonReader((Try client.Get(` and
  the URL on the next line, not after each in turn; and after `=` or `=>` last. When what
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
  tests that import it as another package does, and see its Package names, as its own files do.
- std.test: `Expect(a == b)` is built in, and on failure prints the expression and each side's
  value, and the test goes on; `ExpectThrows`, `Fail`, and `Skip`. A panic or an Assert in a test
  fails it.
- `fb test` runs the tests in parallel in one process, and starts it again after a test that
  panics, running the rest, and in a debug build the ones that finished in that run too, whose
  leaks it didn't check; filters by name, a summary, and leaks reported per test.
- Doc tests: the examples in `///` comments' `# Examples` compile and run as tests, each a test
  named after its declaration, like `game.shapes.Circle.Grow (example)`, in a package of their
  own, `<name>.doctests`, that imports the package and sees its Package names (spec 09). `fb test
  std` runs std's, and `fb test` on a `.fb` file outside a package runs that file's.

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
  ones. Every module of std's has a `//!` summary, one plain sentence, and every Public
  declaration, every Extend of what its users see, and every member they see, a `///` doc of its
  own, even one that implements an Interface's member or overrides a Class's; `fb doc std --check`
  checks it, in a test.
- `fb doc [package | std]` writes a package's public API as JSON, to `build/<name>.json` in its
  folder, or for std, std.json in the current one, unless `-o` names a file, or `-` for stdout,
  after a warning for each link that names nothing and each doc comment on nothing. `--release`
  documents a release build's `#If` branches. `--check` writes nothing, and fails on each of those
  warnings, each module without a `//!` doc, and each item without a `///` doc of its own, an
  Extend's too, one that only has what it implements' or overrides', or one whose doc has no
  summary.
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
  - `doc`: its doc comment, or null; an Extend's says what it adds; a member without one that implements an Interface's member
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

## M11: the IDE (planned 2026-10-31)

Writing FutureBlitz in VS Code should feel as CLion, Rider, and IntelliJ do: errors the moment
they're typed, completion that knows what goes there and imports it, quick fixes for most errors,
inspections, refactorings, project.toml as a file the editor understands, and AI assistants that
know the language (the user, 2026-10-31). Everything here is the compiler's and fb-lsp's, so
`fb check` reports the same errors, warnings, and fixes the editor shows.

**Latency budgets**, on a release build, for a file in a large package (std's own folder, and a
program of a few hundred files), each met when its 50th percentile is in it and its 95th in one and
a half times it:

| What | Budget |
|---|---|
| A syntax error shown, from the keystroke | 50 ms |
| A type error shown, from the last keystroke | 300 ms |
| Completion, hover, signature help, highlights | 100 ms |
| The quick fixes at a position | 100 ms |
| A rename or find references across the package | 1 s |

**Steps**:

| | Step | Done when |
|---|---|---|
| 0 | **Measure.** A benchmark of fb-lsp over its messages (tests/lsp), on std and a generated large package: each request's time from the edit, at the 50th and 95th percentile, written as a report CI keeps; and a check that fails when a budget is missed | the report shows where today's time goes, and the budgets are checked |
| 1 | **Checks that keep what didn't change** (done 2026-10-31). Measuring showed a check's time went to asking the disk, not to compiling, so the checks keep what the disk said until it changes, and what didn't change of the rest: each file's tokens; the package's files aren't listed for every check; a check of a few files checks only their bodies, unless theirs need the others'; a file's key is worked out once; and completion lists the modules an Import can name only at an Import (below) | the benchmark's type error, completion, signature help, references, and rename meet their budgets on both workloads and are enforced; tests edit a body, a declaration, and an Import, and change files on disk in the client's folders and out of them, and check that after each, every file's diagnostics are the same as a check from nothing gives |
| 2 | **Errors as typed** (done 2026-10-31). Each change parses the file alone at once and publishes its syntax errors with no wait, with its last check's other errors where the edit moved them; the full check follows at its pause, and its errors replace them. The parser recovers at each statement and block, and a declaration that only goes at the top level, starting its line, ends the blocks a missing `End` left open, so one missing `End` or `)` doesn't hide the rest of the file's errors; tests of recovery for each block and each kind of broken line | a syntax error shows within 50 ms, and a file with three unrelated syntax errors shows all three |
| 3 | **Completion that knows** (done 2026-10-31). Names from every module the file can import (its package's, std's, its dependencies'), not only those it imports, with the Import added when one is chosen; ranked by the type expected where the cursor is, then by scope and use; an Enum's members where an Enum is expected; named arguments; the members a Class can override and an Interface's to implement, written out with their signatures; keywords only where they can go; postfix completions (`.if`, `.ifnull`, `.for`, `.not`, `.try`, `.local`); completion on a word as it's typed, not only after `.` | each kind has a test of what's offered and in what order, and choosing an unimported name adds its Import |
| 4 | **The editor's basics** (done 2026-10-31). Inlay hints (a Local's inferred type, parameter names at calls, each a setting); highlighting a symbol's other uses; folding by blocks, comments, and Import groups; expanding a selection by syntax; go to type definition and to implementations; call and type hierarchies; code lenses for references; run and debug for each `@Test` and main file in VS Code's Testing panel and the gutter | each request has tests, and the extension's Testing panel runs and debugs a package's tests |
| 5 | **Quick fixes.** Every error that has a likely fix offers it, from the compiler: import the module that declares an unknown name (and `fb add` the registry package that has it, when one does); "did you mean" for a misspelled name; add `Try`, `Try?`, or a Throws; create the Function, Method, Field, Property, Local, or Enum member a use names, with its types from the use; implement an Interface's members or an Abstract Class's; add a missing `End`; a conversion with `As`; a Null check or `?` for nullability; make a declaration Public or Package for a use that can't see it; add `@MainThread`; wrap in `Unsafe`; add a missing Return; fix a keyword's case | each fix has a test that applies it and checks the file compiles |
| 6 | **Inspections.** Warnings, with fixes, from the compiler: unused Locals, parameters, Imports, and Private members; unreachable code; a name that shadows another; a redundant `As` or `?`; a condition that's always True or False. `fb check` and `fb build` show them; project.toml's `[lints]` turns each off or makes it an error | each inspection has tests of when it warns and when it doesn't, and std builds with none |
| 7 | **Refactorings.** Extract a Local, a Function, or a Method from a selection (with its parameters and its result worked out); inline a Local or a Function; change a signature (add, remove, rename, and reorder parameters, updating every call); organize Imports (sorted, unused ones removed), on save if asked | each refactoring has tests over std-sized code that check the result compiles and behaves the same |
| 8 | **project.toml.** fb-lsp serves project.toml too: completion and hover for its keys and values from what fb reads, the errors fb gives for it as you type, package names completed from the registry's search, versions from its index, hover with a package's description and newest version, quick fixes to update a dependency or add what `fb publish` needs; offline, from the cache | completing a dependency's name and version against a test registry works, and every project.toml error fb has shows in the editor |
| 9 | **AI that knows FutureBlitz.** `fb mcp`, an MCP server over stdio, gives any assistant tools to search the spec and std's reference (fb doc's JSON), look up the project's symbols, check a file or a snippet and read its errors, and format code; the VS Code extension offers it to Copilot through VS Code's MCP and language-model tool APIs, so agent mode uses it without setup; `fb new` writes an AGENTS.md with a short primer on the language and `fb mcp`, which Copilot, Claude Code, and Cursor read | an assistant given only `fb mcp` writes a program that compiles from a one-line request, in a test with a scripted client |
| 10 | **Loose ends and the review**, with the budgets met, and FutureBlitz 1.2 released | every step's done-when holds, the budgets pass, and the release is out |

Each step lands with its docs: this file, the extension's README, and the website's tooling pages.

**The benchmark** (step 0) is tests/lsp/bench_tests.cpp, which ctest leaves out: on a release
build, `fb_lsp_tests "[bench]"` runs it on a generated program of 300 modules in 10 layers, each
importing three of the layer below's, editing one in the middle layer, and on std's own folder,
editing std.text.fuzzy, with the editor's own delays (150 ms, and 400 ms for what imports the
edited file). An edit is timed from its didChange: to the publication of its diagnostics, for a
syntax error and a type error; to the response, for completion after `.` and on a word, and
signature help after `(`. Hover, the quick fixes at an error, references, and a rename are timed
with every check done. Each is timed 20 times (`FB_LSP_BENCH_SAMPLES`), and the report,
lsp-bench.md and lsp-bench.json in the folder `FB_LSP_BENCH_REPORT` names, has each one's 50th
and 95th percentiles against its budget, and how long the edited file's own checks took, with how
many modules they had. Each metric has an `enforced` flag, which the step that meets its budget
turns on; then a miss fails the benchmark, with `FB_LSP_BENCH_ENFORCE` set, and a run without it
only reports, as a PC busy with builds misses some. CI's LSP benchmark job sets it, runs it on
Linux, puts the report in the run's summary, and keeps it as the lsp-bench artifact. ctest runs a small one, of 2
layers of 2 modules, which checks that each measurement works.

**Where the time goes** (2026-10-31: the first reports, from a release build on the user's PC,
with other builds running, so the 95th percentiles are noisy; the 50th held across three runs):

| At the 50th percentile | The 300-module program | std |
|---|---|---|
| A check of the edited file alone, with what it imports | 1,067 ms, of 108 modules | 150 ms, of 17 modules |
| A syntax error, and a type error, shown | 2,139 ms, 2,020 ms | 338 ms, 345 ms |
| Completion after `.`, and signature help | 1,830 ms, 1,751 ms | 185 ms, 162 ms |
| Hover, and the quick fixes at an error | 2.7 ms, 1.1 ms | 0.7 ms, 0.2 ms |
| References, and a rename | 8.0 s, 8.5 s | 1.4 s, 1.5 s |

- **Every check starts over.** It checks each module the file imports, std's among them, about
  8 ms a module, and keeps nothing from the check before, so an edit costs as much as everything
  its file imports. Step 2's parse meets the syntax error's budget; the type error's and
  completion's need checks that keep the modules an edit didn't change and check only what it
  touches, which step 1 is for (the user, 2026-10-31).
- **An edit waits 150 ms** for the next keystroke before its check, half the type error's
  budget.
- **The workspace's own work around a check** grows with the check's modules: the rest of an
  edit's time, 600 to 800 ms on the program and about 30 ms on std. Among it, after each check,
  `file_key` canonicalizes each module's path and each of its imports', at about 0.45 ms a path
  on Windows (`std::filesystem::weakly_canonical`).
- **Completion and signature help** wait for the check of the text with the keystroke, so each
  costs a check.
- **References and renames** check the declaring file with every file that imports it, and their
  doc comments' examples, from nothing each time.
- **Hover and the quick fixes** answer from the last check in a few milliseconds; their budgets
  are enforced from the start.

**Step 1, checks that keep what didn't change** (2026-10-31). Step 0's report said every check
started over, so the plan was to keep each module's parse and declarations between checks. A
profile of one check of the program's middle module found otherwise: of about 1.1 s, lexing and
parsing its 108 modules took 25 ms and sema 70 ms, and the rest went to asking the disk, about
4,500 real paths, 600 canonical ones, and 1,800 statuses and comparisons, at a tenth to half a
millisecond each on Windows, for the 200 or so paths a check has. So:

- **The disk cache** (`fb/disk.hpp`): `disk_cache_t` keeps what the front end asks the disk, where a
  path really leads, what's at it, a folder's entries, and a source file's text, and
  `disk::weakly_canonical`, `real_path`, `exists`, `is_regular_file`, `is_directory`, `equivalent`,
  `entries`, `files_under`, and `read` answer through the one a `disk_scope_t` puts in use on the
  thread, as std::filesystem does, or from the disk without one. Each run of the front end has a
  scope of its own, so `fb check` and `fb build` ask the disk once a path too;
  `check_request_t::disk` is the LSP's, which its checks share while the client watches the files.
  The workspace empties it, and the one `file_key` asks through, when the client says files changed,
  when a folder is added or removed, and when a document closes; an answer asked for while it was
  emptied isn't kept (`disk_cache_t::remember`), so a check that began before a change can't leave
  what it saw. fb-lsp asks the client to watch every file, `**/*`, as an Embed's, a [native]
  library's, or a .fbi's change is the checks' too, and the checks share the cache only once the
  client takes the registration, and then keep answers only about the client's folders and std's,
  which changes only with the toolchain (`disk_cache_t::keep_only_under`). The packages a check
  reads outside them, a path dependency's, are watched too when the client's watchers can have
  folders of their own (relativePatternSupport), each a RelativePattern of its own; without that,
  what's outside is asked each time. A folder's deletion takes the files the workspace knows in it,
  and its creation is read when a check asks. A changed file that an Embed, EmbedText, or
  EmbedFolder named, found or not, or one in a folder EmbedFolder named, checks again the files
  whose checks named it (`compilation_t::embeds_named`). A changed .proto checks again what imports
  it, through the .proto's module (`compilation_t::made_from`), and a package's fb.lock, library, or
  .fbi reads its package again, as its project.toml does. A client that doesn't watch gets a cache
  for each check, which the disk can't change under.
- **Only the roots' bodies.** fb-lsp publishes only a check's roots' diagnostics, so
  `analyze()` takes the files whose functions' bodies it checks (`file_set_t::root_bodies`);
  the others' declarations are checked, and Const Functions' bodies as they run. Two things in
  the roots need the other bodies: a worker, whose work's calls lead through them, and generic
  code, whose instances could lead back through them. Sema says so
  (`analysis_t::needs_every_body`), and the check runs again with every body.
- **Shared tokens.** The checks share each file's tokens and doc comments by its text and the
  symbols defined, when lexing it reported nothing (`std_names_cache_t::lexed`).
- **No list of the package's files.** Loading a package listed every file in it, to see whether
  one without an interface file for this build was prebuilt for another; it looks at lib/ first
  now, which rarely has one.
- **Completion** asks for the modules an Import can name, which copies the unit and names all its
  files, only at an Import.

Dependents are still checked again after every edit, not only when a declaration changed: a
body's change can change what imports it through a Const Function, a use of what only the main
thread can, and a generic instance, and checking them is cheap now.

| At the 50th percentile, ms | The program, before | After | std, before | After |
|---|---|---|---|---|
| A check of the edited file | 1,067 | 43 | 150 | 11 |
| A type error shown | 2,020 | 209 | 345 | 166 |
| Completion after `.`, signature help | 1,830, 1,751 | 49, 48 | 185, 162 | 13, 11 |
| References, a rename | 8,043, 8,454 | 208, 294 | 1,402, 1,525 | 285, 289 |
| The first check of the whole folder | 7,086 | 673 to 2,597 | 2,884 | 695 to 1,434 |

Each of these met its budget at the 95th percentile too, as the report shows, and is enforced.
The review's fixes (2026-10-31): the watching above, where the step had watched only some files
and trusted the client's capability alone, which left a kept answer about a file no watcher
matched, or one outside the client's folders, until something else emptied the cache; a changed
.proto or fb.lock checks again what it changes; and the budgets' rule, CI's alone. A
syntax error still waits for the check, step 2's.

**Step 2, errors as typed** (2026-10-31):

- **A parse as it's typed.** Each change parses the document alone (`parse_alone`), before the
  check that waits for the next keystroke, and publishes it at once when it has syntax errors or
  what's published has: its errors, with the last check's other diagnostics, moved where the edit
  put them (`moved_t`, by what the two texts share before and after the edit), but for those the
  edit was in, without their fixes and their labels in other files. The workspace keeps, for each
  open document, what its last check published but for the diagnostics a parse of its text
  gives (`shown_t`, `note_shown`), so a parse's errors replace the check's. A newer check's
  diagnostics replace them all, and an older one's don't. A change that has no syntax error,
  when none is shown, publishes nothing until its check, so what's shown doesn't flicker.
- **Recovery.** The parser already went on at each statement and after a wrong End. A block whose
  End is missing took in the rest of the file, though, so every declaration after it was reported
  as one that only goes at the top level. Now a declaration that only goes there, a Function, a
  Struct, Class, or Interface, an Enum, an Alias, an Extend, an Extern block, a Global, an
  Attribute, or a Const Function, with any attributes and modifiers, at the start of its line,
  ends the blocks open around it (`at_top_level_declaration`), each reporting its End there, one
  error a place: fb fmt indents a block's code, and those declarations are an error in a block
  anyway, so valid code is never read differently. An Extern block's Functions are its own.
- The benchmark times each thing 20 times again, so its 95th percentile isn't one slow sample.

A syntax error shows in under a millisecond on both workloads, and its budget is enforced.

**Step 3, completion that knows** (2026-10-31), in lsp/src/completion.cpp:

- **Ranks.** Each item has a `sortText` of its rank, then its label: what the code wants, the
  Locals and parameters, what's near (the file's, Self's), what it imports, the built-ins, the
  names of modules it doesn't import, and the keywords. What the code wants is the type of the
  parameter of the call's argument the word is in (`call_at()`, which signature help shares, by
  name for a named argument), of the variable a typed initializer or a plain assignment gives a
  value, of what the function returns after Return, or of the other side of an `==` or `<>`;
  a value of it, or what calling a function of it returns, ranks first, an Enum's members, as
  `Color.RED`, are offered where it's an Enum, and True and False rank first where it's Bool and
  Null where it's nullable. A value the parser found missing at the next line counts as where
  the word is.
- **Named arguments.** At the start of a call's argument, the names of the parameters it doesn't
  give yet, by position or by name, as `times:`, which write `times: `.
- **Keywords where they go.** At the start of a line: a top-level declaration's and a main
  file's statements' outside every declaration, a member's in a Struct's, Class's, Interface's,
  or Extend's body, and a statement's in a function's; after it, an expression's.
- **Postfix completions.** After a value's `.`, on one line: `if` and `not` for a Bool, `ifnull`
  for a nullable, `for` for an array or an Iterable, `try` for a call, and `local` for any, each
  a snippet that replaces the value and the `.`, indented as its line.
- **Overriding and implementing.** At the start of a line in a Struct's or Class's body, the
  Methods of the Classes it extends that it can override, and those of the Interfaces its
  declaration lists, and theirs, that it doesn't have, but for Static, Final, and Destroy ones,
  each written out from its signature with `End Method`; one an Interface gives a default for
  is an Override.
- **Names to import.** The names of the modules the file could import but doesn't: the Public
  top-level names each check's modules declare (`exports_of`, kept by unit,
  `workspace_t::importable`) and std's (`std_name_t::kind` now says what each is), as the
  word typed starts, ignoring case, so the list says it's incomplete and the editor asks again;
  each adds `Import <module>` after the file's last Import, or at its start. A Package name of
  its own package isn't offered yet.
- **The editor.** The extension turns VS Code's word-based suggestions off for .fb files, so the
  language server's are what's shown, and VS Code asks for completion as a word is typed.

Each has a test of what's offered and in what order (tests/lsp/completion_tests.cpp), and the
budgets still hold.

**Step 4, the editor's basics** (2026-10-31), in lsp/src/navigate.cpp, but for the code lenses'
outline in features.cpp:

- **Folding** (`folding_ranges`, the file parsed alone): each block from its first line to the
  one before its End; an If's branches and Else, a Select's Cases and Default, and a Try's
  Catches and Finally, each on its own; lambdas with bodies, and arrays and Maps over more than a
  line; runs of `//` lines and comments over lines, as comments; each run of Import lines, as
  imports; and each #If block.
- **Expand Selection** (`selection_ranges`, the file parsed alone): from the word out, through
  each expression, statement, declaration with its attributes, and block of statements around it,
  to the file.
- **Highlights** (`document_highlights`): the file's names of what's named at the cursor, a Write
  where it's declared or assigned, a Read elsewhere.
- **Inlay hints** (`inlay_hints`): the types of Locals without one written (but where a New's call
  says it), a For In's variables, a Local that takes a tuple apart, and a lambda's parameters
  without types; and each parameter's name before a call's argument, but for a named argument, a
  variadic or one-letter parameter, an argument whose own name is the parameter's, and the one
  argument of a function whose name ends with the parameter's. initializationOptions and
  workspace/didChangeConfiguration's `futureblitz.inlayHints` turn `types` and `parameterNames`
  off, and the server asks a client that can for them again.
- **Type definition** (`type_definition`): the type of what's at the cursor, through T?, T[], and
  Weak<T>.
- **Implementations** (`implementations`, from the checks of the declaring file and what imports
  it): an Interface's Structs and Classes, and the Classes extending those; a Class's subclasses; an
  Interface member's implementations; and the Methods that override a Method or those.
- **Call hierarchy** (`prepare_call_hierarchy`, `incoming_calls`, `outgoing_calls`): a Function,
  Method, or declared New, whose item's data is its `symbol_key_t`; who calls it, functions and
  files' top-level code, from the checks of its file and its dependents; and what it, or a file's
  top-level code, calls, from its file's check.
- **Type hierarchy** (`prepare_type_hierarchy`, `supertypes`, `subtypes`): a type's base Class and
  the Interfaces its declaration lists, and the types that extend it or list it.
- **Code lenses** (`code_lenses`, resolved by `codeLens/resolve`): a count of references at each
  type, Function, Method, and Property, which `futureblitz.showReferences` opens; and Run and Debug
  above a program's main file.
- **The Testing panel**: fb-lsp sends each file's @Test Functions as the checks find them, by fb
  test's names, `fb/tests` (when initializationOptions.tests asks), and answers an `fb/tests`
  request with all of them. The extension's controller (editors/vscode/src/tests.ts) lists them by
  file, runs a file's or a test's with `fb test --filter` and reads its output for each one's
  result (`testOutcomes`), and debugs them by building the executable with `fb test -o`, new
  here, which builds the tests without running them, and running it under CodeLLDB with fb's
  script.

Each request has tests (tests/lsp/navigate_tests.cpp); fb test -o has its CLI test, the output's
parsing its unit tests, and the Testing panel its extension test, which CI runs.

## Not in M8

The package manager and the registry (M10); the website (M9); the IDE (the engine phase).
