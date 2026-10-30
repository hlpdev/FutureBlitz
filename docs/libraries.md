# Libraries and native packages

M7's design, planned with the user on 2026-10-22 and replanned on 2026-10-24: FutureBlitz builds
executables and libraries for FutureBlitz programs only. A library is built into the executable
that uses it, or ships beside it in FutureBlitz's own library format, as its project.toml says,
and a package can bring prebuilt native code, like the engine's. The package manager that ships
them is designed here and in [website.md](website.md), and built in M10.

## Decided

1. **Project types.** `[project] type` is `"executable"` (the default with a main) or `"library"`
   (the default without one). Nothing FutureBlitz builds is called from C: no `@Export`, no C
   headers, no C-callable DLLs.
2. **Embedded or beside.** Each dependency is built into the executable (`embed`, the default) or
   shipped beside it as a library file (`beside`), as the program's project.toml says. A library's
   author doesn't choose; the program that uses it does.
3. **The OS's own dynamic libraries.** A library beside the executable is `<name>.dll` on
   Windows, `lib<name>.so` on Linux, and `lib<name>.dylib` on macOS, with the OS's own names and
   extensions, built only for FutureBlitz hosts, with its build recorded as an exported
   `fb.(build).<name>`: its name, version, and ABI hash, which covers its target and mode, and on
   Windows the executable it was built for. The OS loads it, so debuggers, profilers, and crash
   dumps work on it as on any library.
4. **One runtime, the executable's.** The executable always has the runtime built in, as now, and
   every library beside it uses the executable's: one heap, one set of refcounts, one scheduler,
   one registry of types. A library carries no runtime of its own.
5. **Interface files.** A prebuilt library comes with an interface file: its Public declarations,
   and the bodies of its generic, @Inline, and Const code, which callers compile themselves (like
   C++ headers with templates, or Swift's .swiftinterface). The rest is only in the binary, so a
   library can be closed source.
6. **Exact-version ABI.** A program is built against one exact build of each library. Each binary
   carries an ABI hash, and linking or loading a mismatched one is an error that names both builds.
   Layouts and Method tables can change freely between versions.
7. **Plugins, later.** Loading a library a program doesn't know at build time, a mod, isn't in
   M7 (the user's call, 2026-10-28); a program's libraries are its project.toml's dependencies.
8. **C++ through a C API.** The engine, and any C++ library, exposes `extern "C"` functions;
   fb-bindgen writes the FB bindings, and an FB package wraps them in an idiomatic API. No direct
   C++ interop.
9. **Native code is prebuilt, per target.** A package ships native binaries in
   `native/<target>/`, built by its own CI. A C++ library comes as a DLL over its C API, built with
   any compiler; a static archive is C, or C++ built with fb's toolchain. A target the package
   lacks is an error naming the package.
10. **One version per program.** Every package in a program agrees on one version of each
    dependency: semver ranges, resolved to one version and pinned in a lock file. A package is in
    exactly one place: the executable, or its own library file beside it.
11. **A program that uses only std, or embeds every package, is one executable**, depending on
    nothing but the OS's own libraries, unless a package brings a dynamic native library. Every
    third-party library std bundles links statically. The toolchain itself (fb with LLVM,
    fb-bindgen with libclang) may use DLLs.
12. **The engine is a separate C++ project**, not part of the runtime or std: a C API in a DLL,
    shipped as a package with prebuilt binaries, wrapped by FB code.

## project.toml

```toml
[project]
name = "space-game"
type = "executable"

[dependencies]
physics = "1.2"                                       # built into the executable
engine = { version = "0.4", link = "beside" }        # engine.dll beside space-game.exe
```

- `link = "beside"` builds the dependency as its library file, copies it beside the executable
  (with the native DLLs it needs), and fb prints each file the program needs, as it prints
  bundled libraries now.
- The executable loads its beside libraries as it starts, before any Global, checks each one's ABI
  hash, and stops with an error naming the file when one is missing or doesn't match its build.
- A beside library's file name can't be another file's the build puts beside the executable, such
  as a package's native DLL of the same name: the build stops with an error naming both.
- A beside library's own dependencies are in the executable, with std, unless the program lists
  them beside too; either way each package is in one place, and the library uses them from it.
  Its static natives are in the executable too.

## The ABI

- Every Public declaration of a library is there for its users; Package visibility stays inside.
- The interface file is `<name>.fbi`: FutureBlitz source, readable and diffable, with bodies left
  out of what isn't generic, @Inline, or Const, and the ABI hash on its first line. The driver's
  loader reads a prebuilt package's .fbi in place of its source.
- A prebuilt library is `<name>.fblib` per target and mode: its compiled code as LLVM bitcode,
  under a line naming its build and one naming the bundled C libraries it calls, with its .fbi,
  whose ABI hash covers both. Embedding links its bitcode into the program's module, which is then
  optimized as one; `beside` builds the same bitcode into its .dll, .so, or .dylib, so a package
  ships one binary form and its user picks.
- A library beside the executable takes the runtime, std, and what else it uses from the
  executable itself, which loads it as it starts (spec 11): on Windows the executable exports them
  and the library links against its import library, so the executable keeps its built name; on
  Linux the executable's symbols are dynamic (`-rdynamic`); on macOS the library is linked with
  `-undefined dynamic_lookup` against the executable's exports, since `-bundle_loader` can't build
  a dylib. The library exports all of its own code, since a name with ", " in it can't go as an
  /export directive, and one beside library takes another's from that one: on Windows through
  its import library, and elsewhere as the executable loads that one first, with RTLD_GLOBAL.
- A beside library's Global is reached from the other binaries through accessor functions, not
  as data, and so is a ThreadLocal of the executable's that a library uses, since Windows can't
  import one; the executable's other Globals, an embedded package's or std's, a library imports
  as data.
- A generic type's instance made in two binaries is one type: the executable links each beside
  library's bitcode for the code it shares, so every instance and type record is the
  executable's, which each binary uses, and `Is`, casts, and Interface tables agree across them.
- A Class can extend a library's Class across the edge, its base New and Method table coming from
  the library.
- Debug and release builds of a library differ (panics, checks), so a binary package ships both,
  and fb links the one that matches the build.
- The ABI hash covers the interface file, the library's compiled code, the compiler's version,
  the target, and the mode, and a program checks it as it reads a prebuilt library. The interface
  file keeps the library's `///` and `//!` docs, for fb doc and the LSP, but the hash leaves out
  their lines, and the modules' line counts they change, so rewording a doc is the same build
  (decided 2026-10-29). The interface
  file records the build of each package the library was built with, a prebuilt one's hash or a
  hash of a source one's files, and a program that has another build of one is an error naming
  both.

## Natives in packages

- `[native]` takes `{ dynamic = "native/{target}/engine" }`, which finds `engine.dll` and its
  import library on Windows, `libengine.so` on Linux, and `libengine.dylib` on macOS. `{target}`
  in any native path is the build's target: `windows-x64`, `linux-x64`, `linux-arm64`,
  `macos-arm64`, or `macos-x64` (64-bit only, as always).
- A build copies every native DLL the program needs beside the executable, which finds them there
  (`$ORIGIN` on Linux, `@loader_path` on macOS).

## The package manager (built and hosted in M10)

- The registry is at futureblitz.dev, designed in [website.md](website.md), with a sparse index of
  each package's versions and each archive's SHA-256.
- A package's archives hold project.toml, a license, a readme, and its .fb source, or its prebuilt
  `.fblib` per target and mode (with the .fbi), and its natives per target.
- `physics = "1.2"` means 1.2 or any later 1.x; `fb.lock` pins each package's exact version and
  hash; `fb add`, `fb update`, and `fb publish`; downloads go to a cache folder per user. Signing
  comes later.

### The client's design (M10 step 0, settled 2026-10-29)

fb's side of the registry: what fb reads and writes. The server's side, and the wire format it
serves, the index's lines, config.json, and the API's JSON, are futureblitz-web's
`workers/api` docs, mirrored in [website.md](website.md); what's here of them is what fb expects.

**Versions.** A package's version is semver 2.0: three numbers, without leading zeros, and maybe a
pre-release after a `-`, like `1.0.0-beta.2`, ordered as semver orders them. Build metadata (`+...`)
isn't taken, since two versions that differ only there would be one version to the registry.

**Ranges**, as Cargo's, which `[dependencies]` gives as text, `physics = "1.2"`, or as
`version` in a table, `physics = { version = "1.2", link = "beside" }`:

| Range | Means |
|---|---|
| `1.2.3`, `^1.2.3` | `>=1.2.3, <2.0.0`: what's compatible, by semver |
| `1.2`, `^1.2` | `>=1.2.0, <2.0.0` |
| `1` | `>=1.0.0, <2.0.0` |
| `0.4.1`, `0.4` | `>=0.4.1, <0.5.0` and `>=0.4.0, <0.5.0`: below 1.0, the minor version breaks |
| `0.0.3` | `=0.0.3` |
| `~1.2.3`, `~1.2` | `>=1.2.3, <1.3.0` and `>=1.2.0, <1.3.0` |
| `=1.2.3` | that version only |
| `>=1.2`, `>1.2`, `<2`, `<=1.4.0` | as written, a missing number 0 |
| `>=1.2, <1.5` | every comparator, separated by commas, holds |
| `*`, `1.*`, `1.2.*` | any version, any 1.x, any 1.2.x |

A range matches a pre-release only when one of its comparators names a pre-release of the same
three numbers: `1.0.0-beta.1` matches `>=1.0.0-beta.0`, but not `>=0.9` or `1`.

**The resolver.** Every package in a program has one version (decided above), so the resolver
picks one version of each package that every range on it allows: the program's own project.toml's,
its path dependencies', and each picked version's, from its index line. It tries the newest
version first, but a version fb.lock has goes first while every range still allows it, so adding
a dependency changes nothing else. It skips yanked versions, unless fb.lock has them, and versions
whose `fb` range this fb isn't in; `fb update` resolves anew, so a yanked version it unlocks goes.
When a choice leads to a conflict, it jumps back to the choice that made it, the version whose
range conflicts or the program's own, past any unrelated choices in between, and tries that one's
next newest; when nothing works, it says which ranges conflict and who asks for each:
"shapes: game asks for 0.4, and physics 1.2.3 asks for 0.3". Targets don't take part: a prebuilt
package without the build's target is the build's error, which names the package (spec 11).

**fb.lock**, beside project.toml, written by fb, which programs commit:

```toml
# fb.lock: the exact versions this package builds with, which fb writes. Don't edit it.
version = 1

[[package]]
name = "physics"
version = "1.2.3"
source = "https://index.futureblitz.dev/"
checksum = "sha256:9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08"
size = 48213
dependencies = ["shapes"]
targets = { "linux-x64" = { checksum = "sha256:...", size = 912 }, "windows-x64" = { checksum = "sha256:...", size = 1024 } }

[[package]]
name = "shapes"
version = "0.3.4"
source = "path+../registry/index/"
checksum = "sha256:..."
dependencies = []
```

- One `[[package]]` for each registry package the program has, by name: its version, the index
  it's from (`source`), the SHA-256 and size of its archive, its dependencies' names, and the hash
  and size of each target's archive, for every target its version has, so one lock builds on
  every OS. Path dependencies aren't in it; their own registry dependencies are.
- A registry on disk's `source` is `path+` and its folder from fb.lock's, so a lock committed
  with a registry beside the package pins the same versions in any checkout. A change of only a
  package's source is said as one: "Updating physics v1.2.3, now from ...".
- A build reads fb.lock and changes it only when project.toml asks for what it doesn't have: a
  new dependency, a range the locked version is outside, or another registry. Then fb resolves
  again, keeping every locked version it can, and says what it added or changed. `fb update`
  resolves anew, from the newest versions, and `fb update physics` changes only physics and what
  it has to.
- A download whose SHA-256 isn't fb.lock's is an error naming the package and both hashes, and
  nothing of it is used.

**The cache folder** is `~/.futureblitz/` (`%USERPROFILE%\.futureblitz\` on Windows), or the
folder `FB_HOME` names:

| Path | What |
|---|---|
| `cache/index/<registry>/<name>` | a package's index file, as last fetched, and `<name>.etag`, its ETag |
| `cache/index/<registry>/config.json` | the registry's config.json |
| `cache/archives/<registry>/<name>-<version>[-<target>].fbpkg` | the archives fb downloaded |
| `cache/packages/<registry>/<name>-<version>/` | each version unpacked, a package folder the driver loads |

- `<registry>` is a folder name made of the index's host and the first 16 hex digits of its URL's
  SHA-256, like `index.futureblitz.dev-1a2b3c4d5e6f7a8b`, so two registries never share a folder.
- A download goes to a temporary file, is checked against its hash, and is then renamed into
  place; an archive unpacks into a temporary folder that's renamed when it's whole, with a file,
  `.fbpkg-<archive>`, holding the hash it came from. So two fb processes at once, or one that's
  stopped halfway, never leave a package that looks whole and isn't.
- A download fb can't write is that, naming the file, not the network's failure; on Windows, a
  path in the cache past MAX_PATH's 259 characters says so, and that a shorter `FB_HOME` holds
  it, as an archive's file that would be too long unpacked does. A target's archive is said
  with its target: "Downloading jsonkit v1.0.0 (windows-x64)".
- The driver finds a registry dependency through fb.lock alone, in `cache/packages/`, and never
  uses the network: `fb check`, `build`, `run`, `test`, `doc`, and `publish` fetch and unpack what
  fb.lock needs first, and the LSP uses what's there, or says to build.
- **Offline:** with `--offline`, or when the registry can't be reached, fb uses the index files
  and archives in the cache, and says so; a package that isn't there is an error naming it.
  `fb publish` takes `--offline` only with `--dry-run`, as publishing is the network's.

**Archives.** A `.fbpkg` is a tar (POSIX ustar, with a pax header for a path ustar can't hold)
compressed with gzip, which a Worker can read with its own DecompressionStream to check it. fb
writes the files in their paths' byte order, each with mode 0644, no owner, and the time 0, so the
same files make the same tar, inside the subset of tar the registry takes (registry-api.md's
Archives): ustar headers with strict octal numbers, files only, and at most a pax `path` before an
entry. `fb publish` refuses a project.toml over 1 MB, as the registry does.

- `<name>-<version>.fbpkg` holds the package's folder: project.toml, the license and readme, and
  its source, its other files (an Embed's, a .proto), but not build/, tests/, fb.lock, hidden
  files and folders, or folders with a project.toml of their own.
- `<name>-<version>-<target>.fbpkg` holds the package's `lib/<target>/` (a prebuilt library's
  .fblib and .fbi for each mode) and `native/<target>/`. The source archive leaves them out, so a
  build downloads only its target's.
- fb unpacks only regular files and folders, refusing what the registry refuses at publish
  (docs/registry-api.md): an absolute path, an empty, `.`, or `..` part, a control character or
  one of `\ : < > " | ? *`, a part Windows can't name, a path over 255 bytes or not UTF-8, a link,
  two paths that are one ignoring letter case, more than 20,000 files, and an archive that unpacks
  to more than 8 times the registry's limit for its kind: 80 MB a package's own, and 1.6 GB a
  target's. A package's own archive with a target's files in it, or a target's with anything else,
  is refused too. Downloads go to a file as they come, and unpacking reads it a piece at a time,
  so no archive is ever all in memory.

**Registries.** The default is futureblitz.dev, named `futureblitz`. project.toml's
`[registries]` names others, and can name `futureblitz` again to use a mirror:

```toml
[registries]
company = { index = "https://packages.example.com/index/" }
local = { index = "../registry/index" }               # a folder: a registry on disk

[dependencies]
tools = { version = "2", registry = "company" }
```

- An index is an `https://` URL, an `http://` one only on this machine (localhost and the names
  in it, like `index.localhost`, 127.0.0.1, and [::1]), or a folder, which is a registry on disk,
  as tests use: the same files the server serves, `<index>/config.json` and `<index>/<name>`, with
  config.json's `dl` a template of a folder from the index's, or of a `file://` URL. `fb publish`
  and `fb yank` write to one directly, and `fb search` reads its index files' names, finding
  those with every one of its words, in any letter case, as the API's search finds packages
  whose names, descriptions, or keywords have every word. A folder without config.json is no
  registry, which every command says, before it does anything, and one named like a host, which
  isn't a folder, is most likely an index's URL without its `https://`, which fb suggests. A
  dependency's registry on an index line is compared as its canonical index, and
  futureblitz.dev's is the mirror [registries] names for it, if there is one.
- A dependency is from futureblitz.dev unless it names another registry; a package on another
  registry depends on that one's packages by naming it, and its index lines record that
  registry's index URL. futureblitz.dev's packages depend only on its own.
- **Credentials:** `fb login` stores a registry's token in the OS's credential store, under
  `futureblitz:<canonical index>`: Windows' Credential Manager, the macOS keychain, or libsecret's
  Secret Service on Linux, which fb loads as it needs it, or in the JSON file
  `FB_CREDENTIALS_FILE` names, readable only by its owner, for a machine without a store.
  `FB_REGISTRY_TOKEN` comes first when it's set, as in CI, then in a GitHub workflow trusted
  publishing's token (below); with no store (a Linux machine without a Secret Service), fb says to
  use `FB_CREDENTIALS_FILE` or `FB_REGISTRY_TOKEN`. An empty credentials file has no tokens yet;
  on POSIX it's made owner-only before a token goes in. A token is printable ASCII without spaces.

**The commands:**

- `fb login [--registry <name>]` checks the store can be read first, starts a sign-in with the
  registry's API, shows its code, which the page shows too, and opens its page in the browser,
  only an `https://` one or one on this machine, with an opener it doesn't wait for (`FB_BROWSER`
  names another, or `none`); once the person approves it there, fb's polling gets a token, which
  it stores. Polls that are rate limited (after Retry-After), or that the network or the registry
  fails, are tried again until the sign-in would have expired. A token the store then couldn't
  keep is shown, not lost. `fb login --token` reads one from stdin instead, made on
  the dashboard, and asks the registry about it (`GET /me`) before it keeps it, so one that's
  mistyped, revoked, or expired is refused, leaving the token there was. Each says the account
  it signed in as. `fb logout` removes the stored one.
- `fb publish [folder] [--dry-run] [--registry <name>]` builds the archives, writes `fb doc`'s
  JSON, and checks what the registry checks: a library (an executable can't be a dependency) whose
  name the registry takes (not `std`, `fb`, `futureblitz`, `apt`, `rpm`, or `releases`, even with
  `_`s), with a description of one line, at most 300 characters as the registry counts them, in
  UTF-16, a license that's an SPDX expression, a license file, and a readme in UTF-8 under 1 MB;
  only versioned dependencies; paths the archive can hold; the size limits, a target's 200 MB only
  for a registry on disk, since one on the network raises it per package, and each archive's
  20,000 files and 8 times its limit unpacked; and for a prebuilt package each target's debug and release builds and
  their .fbi agreeing. `--dry-run` stops there and lists each archive's files; otherwise it checks
  the version is new, uploads each archive and the docs (in parts when they're over 64 MB), and
  publishes them. Its sizes are in the registry's units, as its limits are, a KB a thousand bytes
  and an MB a million, and Uploading says the size Packaged did. In a GitHub workflow without
  `FB_REGISTRY_TOKEN`, fb asks GitHub for an OIDC token for the registry's audience, exchanges it
  for a short-lived token, publishes with it, and revokes it. A package the exchange doesn't name,
  as it names only packages already on the registry that trust the workflow, is refused before
  anything's uploaded: a package's first version comes from `fb login` or `FB_REGISTRY_TOKEN`.
- `fb yank <name>@<version> [--undo]`, `fb owner <name> [--add <who> | --remove <who>]`, and
  `fb search <words>`, each with `--registry`. The package's name is checked first, and `<who>`
  is a username or `team:<name>`. `fb owner` lists the owners, and the invites to own the package
  that aren't accepted yet, with no token, as the registry shows them to anyone; `--add` invites
  a user ("Invited alice to own physics") or adds a team, and `--add` and `--remove` need a token
  with the `owners` scope, which the sign-in page doesn't tick unless it's asked. A token without
  a scope a request needs is refused with how to get one that has it.
- `fb add <name>[@<range>] [--registry <name>] [--beside]` adds the dependency to project.toml,
  as the newest version's range (`physics = "1.4.2"`, which is `^1.4.2`), keeping the file's
  other lines as they are, and then locks and fetches it, and says it's added once it's locked;
  a dependency that can't be locked is taken out again. Its `--registry` is a name
  `[registries]` gives, which the dependency is written with, not an index's URL or folder.
  `fb update [names]` resolves again.
- When no version fits a range, fb says the package's newest five by version, and why those that
  fit don't: that they're yanked, or that one needs another fb than this one.

**The wire format** (config.json, the index's lines, the archives' checks, and the API) is
[registry-api.md](registry-api.md), the server's, which futureblitz-web's `workers/api` keeps and
this repo mirrors. fb reads an index line of a later format (`"v"`) as a later fb's, and skips it.

**HTTP** is libcurl's, the static one std.net.http is built on, with mbedTLS and the OS's roots
from the runtime's tls_roots.cpp: it's built, tested, and linked statically on every OS already,
with proxies, redirects, HTTP/2, and gzip, and using it from the compiler's library needs no
runtime. std.net.http's own functions would bring the runtime's objects and its I/O thread into
fb. A request the API answers with a `429` or a `503` `unavailable` is sent again after its
Retry-After (1 to 60 seconds), twice at most, and fb login's polls wait as it says too.

## Not in M7

Plugins and loading libraries at run time, `Plugin.Load` and `DynamicLibrary` (later);
FutureBlitz called from C or other languages (not planned); a stable ABI across versions (not
planned); direct C++ interop (not planned); building a package's native code from source (maybe
later); the registry and downloads (M10); signing packages (later).
