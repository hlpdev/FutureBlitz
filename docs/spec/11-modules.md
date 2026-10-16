# 11 · Modules and Packages

Replaces Blitz's `Include` (textual paste) and `.decls` files.

## Modules

- **One file is one module.**
- Everything is **private** unless marked `Public` (see visibility in
  [06](06-user-types.md#visibility)).
- There is **no textual include**.
- Circular imports between modules are allowed; the compiler resolves them.

## Imports

```vb
Import std.math                     // standard library module
Import physics                      // package from the manifest
Import "./ui/hud"                   // local file, relative path
Import std.collections As C         // alias
From std.math Import Vec3, Lerp     // selected names
```

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
  default so games ship as a single binary.
- The package manager is part of the `cli` tool (Milestone M7).
