# Libraries and native packages

M7's design, planned with the user on 2026-10-22: FutureBlitz builds executables, static
libraries, and dynamic libraries, which C and FutureBlitz programs both call, and a package can
bring prebuilt native code, like the engine's. The package manager that ships them is designed
here too, and built in M8.

## Decided

1. **Project types.** `[project] type` is `"executable"` (the default with a main), `"library"`
   (FB source compiled into its users, the default without one, as now), `"static-library"`, or
   `"dynamic-library"`. The library's author decides, as in Swift and Rust.
2. **Callers.** A static or dynamic library serves both C callers (a C ABI) and FB callers (the
   FB ABI: Classes, Interfaces, generics, errors, Tasks).
3. **One shared runtime.** A program that uses no FB dynamic library keeps the runtime inside its
   executable, one file as now. A program that uses one, or a C program hosting one, links a
   versioned runtime DLL (`fb_runtime_7.dll`, `libfb_runtime.so.7`, `libfb_runtime.7.dylib`) that
   the executable and every FB DLL share, like MSVC's /MT and /MD.
4. **Interface files.** A prebuilt FB library comes with an interface file: its Public
   declarations, and the bodies of its generic, @Inline, and Const code, which callers compile
   themselves (like C++ headers with templates, or Swift's .swiftinterface). The rest is only in
   the binary, so an FB library can be closed source.
5. **Exact-version ABI.** A caller is built against one exact build of a library. Each binary
   carries an ABI hash, and linking or loading a mismatched one is an error that names both
   builds. Layouts and Method tables can change freely between versions.
6. **C++ through a C API.** The engine, and any C++ library, exposes `extern "C"` functions;
   fb-bindgen writes the FB bindings, and an FB package wraps them in an idiomatic API. No direct
   C++ interop.
7. **Native code is prebuilt, per target.** A package ships native binaries in
   `native/<target>/`, built by its own CI. A C++ library comes as a DLL over its C API, built
   with any compiler; a static archive is C, or C++ built with fb's toolchain. A target the
   package lacks is an error naming the package.
8. **One version per program.** Every package in a program agrees on one version of each
   dependency: semver ranges, resolved to one version and pinned in a lock file.
9. **The package manager is designed here and built in M8** (tooling), which has std.net to
   download with.
10. **Also in M7:** `[native]` dynamic libraries, loading a library at run time, and Classes as
    handles for C.
11. **A program that uses only std is one executable**, depending on nothing but the OS's own
    libraries. Every third-party library std bundles links statically. The toolchain itself (fb
    with LLVM, fb-bindgen with libclang) may use DLLs.
12. **The engine is a separate C++ project**, not part of the runtime or std: a C API in a DLL,
    shipped as a package with prebuilt binaries, wrapped by FB code.

## The C ABI

- `@Export` marks a top-level Function for C; `@Export("name")` gives it another symbol. Its
  parameters and result follow Extern's rules (C scalars, Enums, Ptr, CString, @CLayout Structs,
  Callbacks). A String parameter takes a `const char*`, copied in, as Extern does in reverse. A
  String result isn't allowed: it gives a CString, which C frees with `fb_cstring_free`.
- An exported Function can't Throw or be Async. A panic inside one ends the process with its
  report, as in a program.
- fb writes `<name>.h` beside the library: the prototypes, the @CLayout Structs, the Enums, the
  Callback types, and the handle types, in C99, which C++ includes too.
- **Classes as handles:** `@Export Class Sprite` gives C `typedef struct Sprite Sprite;`,
  `Sprite* Sprite_New(...)` for each Public New that takes C types, `Sprite_Retain` and
  `Sprite_Release`, and `Sprite_<Name>(Sprite*, ...)` for each Public Method and Property accessor
  that takes and gives C types. A member C can't call is left out of the header, with a note
  there saying why.
- **Starting up:** a library's Globals are set up on the first call into it, once, whichever
  thread makes it, never in DllMain or a load-time constructor (where Windows' loader lock
  forbids starting threads). The first thread to call in is the library's main thread for
  @MainThread. The runtime's finish (the leak report in debug builds) runs at process exit.

## The FB ABI

- Every Public declaration of a static or dynamic library is there for FB callers; Package
  visibility stays inside it.
- The interface file is `<name>.fbi`: FutureBlitz source, readable and diffable, with bodies left
  out of what isn't generic, @Inline, or Const, and the ABI hash on its first line. The driver's
  loader reads a prebuilt package's .fbi in place of its source.
- A Global crosses a binary's edge as accessor functions, not as data.
- A generic type's instance made in two binaries is one type: the runtime registers each Class by
  its full name, so `Is`, casts, and Interface tables agree across binaries.
- A Class can extend a library's Class across a DLL's edge, its base New and Method table coming
  from the library.
- Debug and release builds of a library differ (panics, checks), so a binary package ships both,
  and fb links the one that matches the build.
- The ABI hash covers the interface file, the compiler's version, the target, and the mode.

## Natives in packages

- `[native]` takes `{ dynamic = "native/{target}/engine" }`, which finds `engine.dll` and its
  import library on Windows, `libengine.so` on Linux, and `libengine.dylib` on macOS. `{target}`
  in any native path is the build's target: `windows-x64`, `linux-x64`, `linux-arm64`,
  `macos-arm64`, or `macos-x64` (64-bit only, as always).
- A build copies every DLL the program needs (the package's, FB libraries', and the runtime's
  when it's shared) beside the executable, which finds them there (`$ORIGIN` on Linux,
  `@loader_path` on macOS), and fb prints which, as it prints bundled libraries now.

## Loading at run time

- std.ffi's `DynamicLibrary.Open(path)` throws an FfiError when it can't load; Unsafe
  `Function<F>(name)` gives a Callback of type F, or Null; `Close()` lets it go.
- A **plugin** is an FB dynamic library that depends on an interface package of its host; the
  host loads it with `Plugin.Load(path)` (checking the ABI hash) and gets values of the host's
  Interfaces from the plugin's exported entry Function.

## The package manager (built in M8)

- The registry is at futureblitz.dev (the subdomain is still to choose), with an index of each
  package's versions and each archive's SHA-256.
- A package archive holds project.toml, a license, a readme, its .fb source or its prebuilt FB
  binaries per target and mode (with the .fbi), and its natives per target.
- `physics = "1.2"` means 1.2 or any later 1.x; `fb.lock` pins each package's exact version and
  hash; `fb add`, `fb update`, and `fb publish`; downloads go to a cache folder per user. Signing
  comes later.

## Not in M7

A stable ABI across versions (not planned); direct C++ interop (not planned); building a
package's native code from source (maybe later); the registry and downloads (M8); signing
packages (later).
