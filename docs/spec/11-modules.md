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
  std/collections/map.fb is `std.collections.map`. A package's start with its name, so the game
  package's ui/hud.fb is `game.ui.hud` (see [Packages](#packages)), and its main file is named
  that way too, as `game.main`. Without a project.toml, a program's main file is named by its
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
Import physics                      // package from the manifest
Import "./ui/hud"                   // local file, relative path
Import std.collections As C         // alias
From std.math Import Vec3, Lerp     // selected names
```

- `Import std.a.b` names std/a/b.fb, or else the folder std/a/b/, whose files (not its
  subfolders') are each a module, imported together: `Import std.collections` brings in Map,
  Set, Stack, and the rest.
- A quoted path is a file from the importing file's folder, with `.fb` added:
  `Import "./ui/hud"` in game.fb is ui/hud.fb beside it, the module `ui.hud`. It names a file, so
  one that ends in a folder, like `".."` or `"./"`, is an error. A path can't leave
  the folder its module names start from: its package's, the main file's for a program without
  a project.toml, or std/ for std's own files.
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
name = "scpcb"
version = "0.1.0"

[dependencies]
physics = "1.2"

[native]
joltc = { static = "libs/joltc" }
```

- Dependencies are versioned.
- `[native]` declares C libraries for FFI and how to link them. Static linking is the
  default so games ship as a single binary (M5 step 6; dynamic libraries in M8).
- The package manager is part of the `cli` tool (Milestone M7).

Until then (M5), a dependency is a local package, by its folder from the project.toml's:

```toml
[project]
name = "game"
version = "0.1.0"
main = "main.fb"                    # the default

[dependencies]
physics = { path = "../physics" }
```

- `[project]` needs `name`, an identifier other than `std`, and `version`, three numbers like
  `"0.1.0"`. `main` is a `.fb` file in the package's folder, `main.fb` unless it says otherwise,
  and has to be there if it's written; a package without `main` or a main.fb is a library. A
  key project.toml doesn't use is a warning, and its other mistakes are errors at their lines. A
  version string for a dependency, like `physics = "1.2"`, waits for the package manager. A
  project.toml that can't be read stops the command, as a `.fb` file that can't be read does.
- A dependency's own project.toml names it as `[dependencies]` does, and so do the packages it
  depends on, each read once, by whatever path reaches its folder, however many packages depend
  on it. Two packages of one name, and packages that depend on each other in a cycle, are
  errors.
- A package's files are modules named by their paths from its folder, starting with its name: the
  game package's ui/hud.fb is `game.ui.hud`, and physics' shapes/circle.fb is
  `physics.shapes.circle`. `Import physics` brings in the modules in the physics package's folder,
  but not its main file, however project.toml spells it, or the program's own main module, and
  `Import physics.shapes` its shapes.fb or the modules in its shapes/ folder. Only a package that
  depends on physics, or physics itself, can import it by name; a package's own files are imported
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
  `.fb` files as a module of its own, but not those in its build/ folder, in hidden folders, or
  in folders with a project.toml of their own; it has nothing to build or run.
- `[native]` names the package's C libraries, each by the name `Lib` gives it (see
  [12](12-ffi.md)): `joltc = "libs/joltc"`, or `joltc = { static = "libs/joltc" }`, is a static
  archive from the package's folder: the file itself, or without its `lib` prefix and its
  extension, libs/libjoltc.a, libs/joltc.a, or libs/joltc.lib, which has to be there and be an
  archive; `m = { system = "m" }` is one the system's linker finds by its name, as `-lm`.
  `dynamic` waits for M8. A name is letters, digits, and `_`, `-`, `.`, or `+`, not starting
  with `-`.
- A program links every `[native]` library of its packages, statically: its own code, then each
  package's archives in its project.toml's order, a package's before those of the packages it
  depends on, then the runtime, and then the system libraries, each once. On MinGW, programs
  link the UCRT C runtime, so an archive is built for it too, as with gcc's `-mcrtdll=ucrt`.
  A system library the linker can't find is an error at its project.toml line.
- An Import that names a file or folder in other letter cases than the disk has, like
  `"./UI/Hud"` for ui/hud.fb or `physics.Shapes` for shapes/, gets a warning, since only a disk
  that ignores letter case finds it.
