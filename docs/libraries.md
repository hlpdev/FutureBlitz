# Libraries and native packages

M7's design, planned with the user on 2026-10-22 and replanned on 2026-10-24: FutureBlitz builds
executables and libraries for FutureBlitz programs only. A library is built into the executable
that uses it, or ships beside it in FutureBlitz's own library format, as its project.toml says,
and a package can bring prebuilt native code, like the engine's. The package manager that ships
them is designed here and in [website.md](website.md), and built in M8 and M9.

## Decided

1. **Project types.** `[project] type` is `"executable"` (the default with a main) or `"library"`
   (the default without one). Nothing FutureBlitz builds is called from C: no `@Export`, no C
   headers, no C-callable DLLs.
2. **Embedded or beside.** Each dependency is built into the executable (`embed`, the default) or
   shipped beside it as a library file (`beside`), as the program's project.toml says. A library's
   author doesn't choose; the program that uses it does.
3. **The OS's own dynamic libraries.** A library beside the executable is `<name>.dll` on
   Windows, `lib<name>.so` on Linux, and `lib<name>.dylib` on macOS, with the OS's own names and
   extensions, built only for FutureBlitz hosts, with a metadata section (its interface's hash,
   its version, target, and mode). The OS loads it, so debuggers, profilers, and crash dumps work
   on it as on any library.
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
  a dylib.
- A Global crosses a binary's edge as accessor functions, not as data.
- A generic type's instance made in two binaries is one type: the runtime registers each Class by
  its full name, so `Is`, casts, and Interface tables agree across binaries.
- A Class can extend a library's Class across the edge, its base New and Method table coming from
  the library.
- Debug and release builds of a library differ (panics, checks), so a binary package ships both,
  and fb links the one that matches the build.
- The ABI hash covers the interface file, the compiler's version, the target, and the mode.

## Natives in packages

- `[native]` takes `{ dynamic = "native/{target}/engine" }`, which finds `engine.dll` and its
  import library on Windows, `libengine.so` on Linux, and `libengine.dylib` on macOS. `{target}`
  in any native path is the build's target: `windows-x64`, `linux-x64`, `linux-arm64`,
  `macos-arm64`, or `macos-x64` (64-bit only, as always).
- A build copies every native DLL the program needs beside the executable, which finds them there
  (`$ORIGIN` on Linux, `@loader_path` on macOS).

## The package manager (built in M8, hosted in M9)

- The registry is at futureblitz.dev, designed in [website.md](website.md), with a sparse index of
  each package's versions and each archive's SHA-256.
- A package's archives hold project.toml, a license, a readme, and its .fb source, or its prebuilt
  `.fblib` per target and mode (with the .fbi), and its natives per target.
- `physics = "1.2"` means 1.2 or any later 1.x; `fb.lock` pins each package's exact version and
  hash; `fb add`, `fb update`, and `fb publish`; downloads go to a cache folder per user. Signing
  comes later.

## Not in M7

Plugins and loading libraries at run time, `Plugin.Load` and `DynamicLibrary` (later);
FutureBlitz called from C or other languages (not planned); a stable ABI across versions (not
planned); direct C++ interop (not planned); building a package's native code from source (maybe
later); the registry and downloads (M8 and M9); signing packages (later).
