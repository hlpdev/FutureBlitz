# 11 · Modules and Packages

Replaces Blitz's `Include` (textual paste) and `.decls` files.

## Modules

- **One file is one module.**
- Everything is **private** unless marked `Public` (see visibility in
  [06](06-user-types.md#visibility)). An Import brings in only what the importing file can use:
  another module's Public declarations, and its Package ones in the same package.
- There is **no textual include**.
- Circular imports between modules are allowed; the compiler resolves them.
- A module is named by its path from its root, without `.fb` and with dots for folders:
  std/collections/map.fb is `std.collections.map`. A package's start with its name, so the app
  package's ui/hud.fb is `app.ui.hud` (see [Packages](#packages)), and its main file is named
  that way too, as `app.main`. Without a project.toml, a program's main file is named by its
  stem.
- A name is found in the code's own scopes, then among its module's top-level declarations, then
  in the modules it imports, then among the built-ins. A module's own types and variables hide
  imported ones, and its Functions hide imported ones that take the same types (with the same
  constraints, for generic ones); otherwise Functions of one name are one overload set, and a
  call that two imported modules' Functions fit equally well is ambiguous. A built-in Function,
  like Panic, Assert, or RefEquals, is hidden by a type or variable of its name, and by a
  module's own or imported Function that takes its types; one that takes any type, like
  RefEquals' Classes, fits a call worse than a Function that names its types. Spawn, Thread,
  Atomic, Mutex, Channel, and Scheduler, which the compiler checks itself, are hidden by any
  declaration of their name.
- Only the main module's top-level code runs; the other modules only declare things. Globals
  initialize before it runs: std/core/'s first, then std.collections.map's, which a Map
  literal needs whether or not anything imports it, then each module after the modules it
  imports, depth-first in the order of its Import lines, starting from the main module, which is
  last. A
  Global's initializer can't read a Global or Const that initializes after it, though a Const
  known at compile time is known before any of them.

## Imports

```vb
Import std.math                     // standard library module
Import geometry                     // package from the manifest
Import "./ui/hud"                   // local file, relative path
Import "proto/orders.proto"         // a Protocol Buffers file's types
Import std.collections As C         // alias
From std.math Import Vec3, Lerp     // selected names
```

- `Import std.a.b` names std/a/b.fb, or else the folder std/a/b/, whose files (not its
  subfolders') are each a module, imported together: `Import std.collections` brings in Map,
  Set, Stack, and the rest.
- A quoted path is a file from the importing file's folder, with `.fb` added:
  `Import "./ui/hud"` in app.fb is ui/hud.fb beside it, the module `ui.hud`. It names a file, so
  one that ends in a folder, like `".."` or `"./"`, is an error. A path can't leave
  the folder its module names start from: its package's, the main file's for a program without
  a project.toml, or std/ for std's own files.
- A quoted path that ends in `.proto` names a Protocol Buffers file, proto2 or proto3, which fb
  reads as it builds the program into a module of its types, named by the path as a `.fb`
  file's is, `proto.orders.proto`, under its file's stem, `orders`: a Struct for each message,
  which std.data's Protocol Buffers encoding reads and writes, an Enum for each enum, and an
  Interface for each service. Its package doesn't name the module, since one package often spans
  several files.
  A `.proto`'s own imports are found from its folder, then from each folder above it up to the
  one its package's paths start from, the nearest first, as protoc finds them in its `-I`
  folders, and a `google/protobuf/` path none of them has is one of protobuf's well-known types,
  which std has, as is one an Import names that isn't beside its file. `.proto` files can't
  import each other in a cycle, and a `.proto`'s mistakes are errors at its lines and columns,
  which leave its module empty.
- `Import X` brings X's names in without a qualifier, and under X's last name, as in
  `collections.Stack` or `hud.Draw`. `Import X As C` brings them in only as `C.Name`.
  `From X Import A, B` brings in only A and B, which X has to declare.
- A qualifier goes before a name in values and in types: `collections.Stack<Int>()`,
  `C.Map<String, Int>`, `shapes.Kind.Square`, `Extends shapes.Shape`. It isn't a value itself,
  and a declaration of its module can't take its name.
- Import and From lines come first in a file, before its other statements. `#If` can go around
  them, since the lexer skips lines.
- Every name of std's needs its Import, std.core's too: `Import std.core` brings in
  Print, Error, the Interfaces, and Iterator. The language's own features go through std without
  one: Throw and Catch, `==`, strings, `For ... In`, and a `{key: value}` literal, which makes
  std.collections.map's Map, always part of the program. Naming Map, Set, and the rest, or using
  array.fb's `Sort`, `Filter`, `Map`, `Find`, and `Remove`, needs `Import std.collections`. A
  message that suggests one of std's names, like `<T: Printable>`, says which Import it needs.
- A constraint that names no Interface is an error once, and what code does with the type
  parameter that the constraint could have allowed isn't another.
- A type or variable that two imports give is an error only where code uses it without a
  qualifier, naming the modules. A name the code doesn't see, that another module declares, gets
  an error saying which module to import: "'Stack' is in std.collections, which this file
  doesn't import".
- What an Extend adds is seen where its module is, or is imported (by an Import or From line
  that names it or its folder), and where it isn't seen it hides nothing and overrides nothing.
  The Interfaces it adds hold in the whole program, so a type has one conformance to each, and
  so does its Operator ==, which is what == means for the type everywhere. Two modules' Extends
  can each add a member of one name: the code's own module's hides an imported one, two
  imported Methods are overloads that a call both fit finds ambiguous, and a Property that two
  imports add is ambiguous wherever it's used.
- What implements an Interface's member is what the code that lists the Interface (the type's
  declaration, or the Extend) sees by those rules, so two imported modules' Methods that both
  fit are an error there; and a `For ... In` goes through the `Iterate` that a call written in
  its place would find.

## Packages

A package is a directory with a `project.toml` manifest.

```toml
[project]
name = "editor"
version = "0.1.0"

[dependencies]
geometry = "1.2"

[native]
zstd = { static = "libs/zstd" }
```

- Dependencies are versioned, and resolved to one version of each, which fb.lock pins.
- `[native]` declares C libraries for FFI and how to link them. Static linking is the
  default, so a program ships as a single binary; a package can bring dynamic ones too (see
  [Native libraries](#native-libraries)).

A dependency can also be a local package, by its folder from the project.toml's, with a range
too when it's published:

```toml
[project]
name = "app"
version = "0.1.0"
main = "main.fb"                    # the default

[dependencies]
geometry = { path = "../geometry" }
layout = { path = "../layout", version = "0.4" }    # its folder here, 0.4 once published
```

- `[project]` needs `name`, an identifier other than `std`, and `version`, three numbers like
  `"0.1.0"`. `main` is a `.fb` file in the package's folder, `main.fb` unless it says otherwise,
  and has to be there if it's written. `type` is `"executable"` or `"library"`: an executable,
  the default for a package with a main file, builds a program, and a library, the default for
  one without, is built only into the programs that use it, or beside them (see
  [Libraries](#libraries)). An executable without a main file, or a library with `main`
  written, is an error. A key project.toml doesn't use is a warning, and its other mistakes are
  errors at their lines. A project.toml that can't be read stops the command, as a `.fb` file that
  can't be read does.
- `version` is semver: three numbers without leading zeros, and maybe a pre-release, like
  `"1.0.0-beta.2"`. What the registry shows of a package is in `[project]` too, and `fb publish`
  needs `license` and `description`: `license`, an SPDX expression like `"MIT OR Apache-2.0"`;
  `description`, a line of at most 300 characters; `license_file` and `readme`, files in the
  package's folder, when they aren't LICENSE (LICENSE.md, LICENSE.txt, or COPYING) and
  README.md (README or README.txt); `repository`, `homepage`, and `documentation`, `https://`
  URLs; `keywords`, at most five, each lowercase letters, digits, and `-`, not starting with `-`,
  at most 32 long; and `fb`, the range of fb's versions the package builds with, like `">=1.2"`.
- A dependency is a package from a registry, by a range of its versions, `geometry = "1.2"` or
  `geometry = { version = "1.2" }`, a local one by its folder, `{ path = "../geometry" }`, or a
  local one that's published too, by both, `{ path = "../geometry", version = "1.2" }`, as
  packages developed together in one repository are: a build uses its folder, whose version has to
  be in the range, and `fb publish` writes only the range into the published package's
  project.toml, so builds of the published package take the registry's. `registry = "company"`
  takes it from a registry `[registries]` names rather than futureblitz.dev; a dependency with
  only a path takes none. `[registries]` names each by its index, `company = { index =
  "https://packages.example.com/index/" }`, or a folder, a registry on disk; its names are
  identifiers, and `futureblitz` is futureblitz.dev's, which it can name again for a mirror. A
  range's forms, the resolver, fb.lock, and the cache folder are the package manager's, which
  [its own documentation](../libraries.md) describes. A program's registry dependencies, and
  theirs, each have one version, which fb.lock beside its project.toml pins, and which builds
  from the cache.
- `[format]` has `width`, the line width `fb fmt` formats the package's code to: a whole number
  of columns from 40 to 1000, 100 without it. A width outside that range is an error.
- A dependency's own project.toml names it as `[dependencies]` does, and so do the packages it
  depends on, each read once, by whatever path reaches its folder, however many packages depend
  on it. Two packages of one name, and packages that depend on each other in a cycle, are
  errors.
- A package's files are modules named by their paths from its folder, starting with its name: the
  app package's ui/hud.fb is `app.ui.hud`, and geometry's shapes/circle.fb is
  `geometry.shapes.circle`. `Import geometry` brings in the modules in the geometry package's
  folder,
  but not its main file, however project.toml spells it, or the program's own main module, and
  `Import geometry.shapes` its shapes.fb or the modules in its shapes/ folder. Only a package that
  depends on geometry, or geometry itself, can import it by name; a package's own files are imported
  by its name or by quoted paths. Its folders can have any names, like std or a dependency's.
- An Import stays in its package's files: it can't leave the package's folder, or go into a
  folder in it that has a project.toml of its own, which is another package's, whose modules
  only its name imports, as a dependency.
- `Package` declarations are seen by the modules of their package, a dependency's by its own
  modules only.
- `fb check`, `build`, and `run` take a `.fb` file, a package's folder, or nothing, for the
  package the current folder is in, from it up. A `.fb` file with a project.toml in its folder or
  above builds in that package, as its main file. A package's build writes
  `<folder>/build/<name>`, named after the package, or after another main file it was given, with
  the platform's extension, unless `-o` says where. `fb check` on a library checks each of its
  `.fb` files as a module of its own, but not those in its build/ folder, its tests/ folder, in
  hidden folders, or in folders with a project.toml of their own; it has nothing to run, and
  `fb build` writes its interface file and binary (see [Prebuilt libraries](#prebuilt-libraries)).
  `fb test` builds a package's tests, from each of its `.fb` files, its tests/ folder's, and its
  doc comments' examples, and runs them (see [09](09-errors.md#tests)). `fb doc` writes a
  package's public API as JSON, to `<folder>/build/<name>.json` unless `-o` says where.
- `[native]` names the package's C libraries, each by the name `Lib` gives it (see
  [12](12-ffi.md)): `zstd = "libs/zstd"`, or `zstd = { static = "libs/zstd" }`, is a static
  archive from the package's folder: the file itself, or without its `lib` prefix and its
  extension, libs/libzstd.a, libs/zstd.a, or libs/zstd.lib, which has to be there and be an
  archive; `m = { system = "m" }` is one the system's linker finds by its name, as `-lm`; and
  `sqlite3 = { dynamic = "native/{target}/sqlite3" }` is a dynamic library (see
  [Native libraries](#native-libraries)). A name is letters, digits, and `_`, `-`, `.`, or `+`,
  not starting with `-`.
- A path in project.toml, `main`, a dependency's `path`, `[build]`'s program, or a static or
  dynamic library's, takes `\` as a `/`, as an Import's path does, so a project.toml written on
  Windows means the same everywhere.
- A program links every static `[native]` library of its packages: its own code, then each
  package's archives in its project.toml's order, a package's before those of the packages it
  depends on, then the runtime, and then the system libraries, each once. On MinGW, programs
  link the UCRT C runtime, so an archive is built for it too, as with gcc's `-mcrtdll=ucrt`.
  A system library the linker can't find is an error at its project.toml line.
- An Import that names a file or folder in other letter cases than the disk has, like
  `"./UI/Hud"` for ui/hud.fb or `geometry.Shapes` for shapes/, gets a warning, since only a disk
  that ignores letter case finds it.

### Build steps

A package's build step is a program fb builds and runs before it compiles the package, as
Cargo's build.rs is, to write code or data the package's build has: typed references to a
game's assets, tables worked out ahead, or a format's code from its schema.

```toml
[build]
program = "tools/generate.fb"        # a program in the package's folder
```

```toml
[build]
package = "assets"                   # one of [dependencies], whose program it is
program = "cook.fb"
```

- `program` is a `.fb` file with top-level code, in its package's folder: this one's, or with
  `package`, that dependency's. It's built in debug as a program of its package, which it can
  import, with that package's dependencies, but without build steps, so it can't use what a step
  makes; its registry packages are the ones the build's fb.lock has. The package's own program
  isn't one of the package's modules.
- It's given its package's name, folder, where to write, and what's being built in its
  environment: `FB_BUILD_PACKAGE`, `FB_BUILD_FOLDER` (whole), `FB_BUILD_OUT`, a folder,
  `FB_BUILD_TARGET`, like `windows-x64`, and `FB_BUILD_MODE`, `debug` or `release`. It runs in
  the folder fb runs in, with no input, so it reads its package's files from `FB_BUILD_FOLDER`.
- What it writes in `FB_BUILD_OUT`, which is `build/steps/<target>/<mode>/<package>/out/` in the
  folder of the package being built, is its package's, at the same paths as the package's own
  files: a `.fb` file there is a module of the package, `out/res.fb` the package's `res` module,
  and the other files are what Embed, EmbedText, and EmbedFolder take, by their paths in the
  package, from the package's own code or the made modules'. A path the package's folder has
  too is an error. The folder stays from one run to the next, so a step can do only what's new.
- A line it prints `fb:rerun-if-changed=<path>` names a file or folder it read, from its
  package's folder or whole; `fb:warning=<text>` is a warning at its `[build]` line. Its other
  output is shown only when it fails.
- It runs again when what it read changes, or its program's files do, or fb's version does: a
  file or folder `fb:rerun-if-changed` named, or without one, any of its package's files, as its
  archive has them. Otherwise its last run's files are the build's.
- Each package's step runs before its package compiles, after those of the packages it depends
  on, and a dependency's runs for the build that uses it, into that build's folder. A prebuilt
  package's step ran where it was built, whose modules are in its interface file.
- `fb build`, `run`, `test`, `check`, and `doc` run the steps that are out of date, and the editor
  runs one that never ran, and otherwise checks with its last run's files, which those commands
  bring up to date. A step that doesn't build, or that ends with an exit code other than 0, is an
  error at its `[build]` line, with its errors or its output, and the package isn't compiled.

## Libraries

A library package is built into each program that uses it, or beside it as a library file of its
own, as the program's project.toml says; the library's author doesn't choose.

```toml
[dependencies]
geometry = { path = "../geometry" }                   # built into the executable
audio = { path = "../audio", link = "beside" }        # audio.dll beside the executable
```

- `link` is `"embed"`, the default, or `"beside"`. An embedded library's code is linked into the
  executable. A beside one is its own file, the OS's own kind of dynamic library: `<name>.dll` on
  Windows, `lib<name>.so` on Linux, and `lib<name>.dylib` on macOS, built only for FutureBlitz
  programs, which the build puts beside the executable, printing each file the program needs, as
  it prints the bundled C libraries. `link` in a library's own `[dependencies]` is a warning that
  it's ignored, since only the program decides; a dependency whose `type` is `"executable"` is an
  error naming it.
- A program that uses only std, or embeds every package, is one executable, which needs nothing
  but the OS's own libraries, unless a package brings a dynamic native library (see
  [Native libraries](#native-libraries)), which goes beside it.
- Each package is in one place in a program: the executable, or its own file beside it. A beside
  library's own dependencies that the program doesn't list beside are in the executable, with
  std, and the library uses them from it, so a package that the executable and beside libraries
  all use is still one copy.
- The runtime and std are always in the executable, and every library beside it uses them: one
  heap, one set of refcounts, one scheduler, one registry of types, and one copy of each of std's
  modules and their Globals. A library beside the executable has no runtime or std of its own,
  and the executable links every std module and package its beside libraries use, and every
  generic instance and type record they make, so each is the executable's.
- The executable loads its beside libraries itself as it starts, before any Global initializes,
  each after those it depends on, and checks each one's build (see
  [Prebuilt libraries](#prebuilt-libraries)). One that's missing or isn't the build the program
  was built against stops it with an error naming the file, before any of the program's code
  runs.
- A beside library reaches the runtime, std, and the packages in the executable through the
  executable's exported symbols: its dynamic ones on Linux, and on macOS its exports, which a
  beside library is linked to look up as it loads; on Windows, its exports through its import
  library, by the executable's file name. A Windows executable with beside libraries therefore
  keeps the name it was built with, which has to be ASCII, and one that's renamed stops as it
  starts, with an error naming both names; a beside library built for another executable, as
  another main file of the package builds into the same folder, stops it with an error naming
  both. A beside library reaches another that it uses directly: on Windows through its import
  library, and elsewhere as the executable loads that one first.
- Across a binary's edge, code works as it does inside one: a Class can extend a library's Class,
  objects and Tasks pass either way, and a generic type's instance made in two binaries is one
  type, so `Is`, casts, and Interfaces agree. A Global of a library is reached through its
  library, never copied, and a ThreadLocal through the binary that has it, so each thread has
  one of each.
- Only a library's Public declarations are seen by its users' code; its Package ones stay in it.
- A beside library's file can't have the name, compared ignoring letter case, of another file the
  build puts beside the executable, such as a package's native library or another beside
  library, of a library the executable or any library takes from the OS, or of one of Windows'
  known DLLs: the build stops with an error naming both.
- In a library, a main.fb is an ordinary module unless `main` is written, which is an error.

### Prebuilt libraries

- A library can be prebuilt, so its users needn't build it, and so the rest of its code can stay
  closed. `fb build` on a library writes `<folder>/build/<target>/<mode>/<name>.fblib`, its
  compiled code, which links into the executable when it's embedded and into its library file when
  it's beside, and `<name>.fbi`, its interface file, beside it. The .fblib's first line names its
  build, as the interface file's does, its second the bundled C libraries its code calls, which a
  program that links it links too, and the rest is its code as LLVM bitcode, which a program's
  build links with its own and optimizes with it; std's code and the code the interface file keeps,
  which both have, are one. A package folder with a
  project.toml and `lib/<target>/<mode>/<name>.fblib` and `.fbi` for the build's target and mode,
  and no `.fb` source, is prebuilt; one with both is an error naming the package.
- The interface file is FutureBlitz source, written for one target and mode with every `#If`
  resolved, holding what a program compiles against: every Public declaration, with the bodies of
  its generic, `@Inline`, and Const code, which the program compiles itself; the library's Private
  and Package declarations that those bodies reach, which only those bodies can use, whose symbols
  the library's binary exports, with bodies only when they're generic, `@Inline`, or Const
  themselves; every Extend; and every member of each type it shows, Private ones too, with their
  visibility, so layouts, `For Field`, Interfaces, and the names a Class that extends one can use
  agree. A Const is written as its value, and an `Embed` in code the interface file keeps, which a
  program compiles, is an error when the library is built; a Field's default, which a program
  sets as it makes the Field's Struct or Class, is written as its value.
- In it, a declaration whose code is in the library's binary is its first line and `=> ...`, and
  a Global or Static Field whose value the binary gives is `= ...` after its type,
  which is written for one that had none; `@MainThreadOnly("why")` before one says that its code
  can only run on the main thread, and why, as [10](10-concurrency.md)'s message would. Only an
  interface file can write them. Each module, a `.proto` file's as the module fb makes from it, is
  its part of the file, after a line `// module <path> <lines>`, with its `//!` doc comment and its
  Imports, and each declaration it keeps has its `///` doc comment, its members' and an Enum's
  members' too, so `fb doc` and the LSP show a prebuilt package's docs (see
  [01](01-lexical.md#doc-comments)); its other comments are left out.
- Its first line is the library's build: its name, its version, and its ABI hash, which covers
  the rest of the interface file but for its doc comments' lines and its modules' line counts,
  the library's compiled code, the compiler's version, the target, and the mode. A change to a
  doc's words alone is the same build; one that adds or takes out lines moves the code after it,
  whose lines the binary's stack traces and debug info name, so that's another. A program is
  built against one exact build of each library, and linking or loading another is an error that
  names both builds' names, versions, and hashes. Between versions a library's layouts and
  Method tables can change freely.
- After it, a line `// uses <name> <version> <build>` names each package the library was built
  with: a prebuilt one's hash, or `source:` and a hash of a source one's files. A program that
  has another build of one stops with an error naming the library and both builds, and a
  prebuilt library whose files aren't the build their first line names, for this fb, the target,
  and the mode, is an error too.
- A debug and a release build of a library differ, as their checks and panics do, so a prebuilt
  package has both, and a build links the one of its own mode.

### Native libraries

- `[native]`'s `dynamic` names a C library the program loads as it starts: `sqlite3 = { dynamic
  = "native/{target}/sqlite3" }` finds sqlite3.dll and its import library (sqlite3.lib,
  libsqlite3.dll.a, or libsqlite3.a beside it) on Windows, libsqlite3.so on Linux, and
  libsqlite3.dylib on macOS, which have to be there. `Lib "sqlite3"` names it as it names a
  static one (see [12](12-ffi.md)).
- `{target}` in any of `[native]`'s paths is the target the program is built for:
  `windows-x64`, `linux-x64`, `linux-arm64`, `macos-arm64`, or `macos-x64`, so a package ships a
  binary of its natives per target, `native/<target>/`. A target a package has no binary for is an
  error naming the package.
- A build copies every dynamic library the program needs beside the executable, which finds them
  there: on Windows beside it, on Linux through `$ORIGIN`, and on macOS through `@loader_path`.
  Each is copied under the name the executable records for it: its SONAME on Linux, and on macOS
  its install name, which has to be `@rpath/lib<name>.dylib` or a name in `@loader_path`, or the
  build stops with an error naming the package and the file. A beside library's dynamic natives
  are its own, and are copied beside the executable too; its static ones are in the executable,
  as its dependencies are, which it takes them from, and its system libraries both link.
- A C++ library is used through a C API (`extern "C"` functions), which fb-bindgen writes bindings
  for; there's no direct C++ interop.

### Package archives

- A package is shipped as an archive: its project.toml, a license, a readme, and either its `.fb`
  source, or its prebuilt `lib/<target>/<mode>/`, each `.fblib` with its `.fbi`, and its natives
  per target. The package manager fetches these archives, `.fbpkg` files, from the registry, and
  `fb publish` sends a package's archive to it.
