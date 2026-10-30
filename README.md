# FutureBlitz

## Installing

Releases are on [GitHub Releases](https://github.com/hlpdev/FutureBlitz/releases), for Windows
(x64), Linux (x64 and arm64; Ubuntu 22.04 or newer, and the programs fb builds run on glibc 2.28,
as RHEL 8 and Ubuntu 20.04 have), and macOS 15 or newer (arm64 and x64). They're unsigned for
now, so a browser warns about the archives and installers it downloads; the install scripts
don't.

The install scripts install a published release, and there's none yet: the first one is coming.
Until it's out, build FutureBlitz from source, as [Building from source](#building-from-source)
says.

On Linux and macOS:

```sh
curl -fsSL https://github.com/hlpdev/FutureBlitz/releases/latest/download/install.sh | sh
```

On Windows, in PowerShell:

```powershell
irm https://github.com/hlpdev/FutureBlitz/releases/latest/download/install.ps1 | iex
```

Each puts FutureBlitz in ~/.futureblitz (%USERPROFILE%\.futureblitz) and its bin on PATH, and
updates it when it's run again; `sh -s -- --version 0.1.0` (or `$env:FUTUREBLITZ_VERSION`)
installs a given release. A
release's MSI does the same on Windows, and its .pkg installs it in /usr/local/futureblitz on
macOS. Building a program needs nothing else, but on macOS Apple's Command Line Tools
(`xcode-select --install`).

```sh
fb new hello
cd hello
fb run
```

## Building from source

FutureBlitz builds with CMake and Ninja, a C++23 compiler (GCC 14 or newer, MinGW-w64's on
Windows, or Apple's Clang on macOS), and LLVM 22 with its lld and libclang, whose CMake package
`-DLLVM_DIR` names, or on Windows `-DFB_LLVM_ROOT` the folder LLVM's installer put it in. The
other libraries it uses CMake fetches itself.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DLLVM_DIR=/usr/lib/llvm-22/lib/cmake/llvm
cmake --build build
build/cli/fb new hello
```

A build folder's fb finds its toolchain and std where CMake put them, and
`cmake --install build --prefix <folder>` lays a release's folder out there.
