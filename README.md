# FutureBlitz

## Installing

FutureBlitz is a compiled, general-purpose language with BASIC's readability and native speed.
Each release is on [GitHub Releases](https://github.com/hlpdev/FutureBlitz/releases), for Windows
10 or newer (x64, which arm64 Windows runs too), Linux (x64 and arm64, with glibc 2.35, as Ubuntu
22.04, Debian 12, and Fedora 36 have; the programs fb builds run on glibc 2.28, as RHEL 8 and
Ubuntu 20.04 have), and macOS 15 or newer (arm64 and x64). On Windows, fb's programs, the MSI, and
setup.exe are signed. macOS's aren't, so a browser warns about an archive or a .pkg it downloads;
the install script and Homebrew don't.

Each way below installs a published release; before the first one, build FutureBlitz from source,
as [Building from source](#building-from-source) says.

On Linux and macOS, the install script puts FutureBlitz in ~/.futureblitz and its bin on PATH:

```sh
curl -fsSL https://futureblitz.dev/install.sh | sh
```

On Windows, in PowerShell, it puts it in %USERPROFILE%\.futureblitz:

```powershell
irm https://futureblitz.dev/install.ps1 | iex
```

Run again, either updates it; `sh -s -- --version 1.0.0` (or `$env:FUTUREBLITZ_VERSION`) installs a
given release. Or with a package manager:

| | |
|---|---|
| winget | `winget install FutureBlitz.FutureBlitz` |
| Homebrew (macOS and Linux) | `brew install hlpdev/tap/futureblitz` |
| apt (Ubuntu and Debian) | below |
| dnf (Fedora) | below |

```sh
# apt
curl -fsSL https://dl.futureblitz.dev/apt/futureblitz.gpg | sudo tee /usr/share/keyrings/futureblitz.gpg > /dev/null
echo "deb [signed-by=/usr/share/keyrings/futureblitz.gpg] https://dl.futureblitz.dev/apt stable main" | sudo tee /etc/apt/sources.list.d/futureblitz.list
sudo apt update && sudo apt install futureblitz

# dnf
sudo curl -fsSL -o /etc/yum.repos.d/futureblitz.repo https://dl.futureblitz.dev/rpm/futureblitz.repo
sudo dnf install futureblitz
```

A release's installers do it too: on Windows the MSI or setup.exe, for your user, in
%USERPROFILE%\.futureblitz; on macOS the .pkg, in /usr/local/futureblitz; and on Linux the .deb
or .rpm, in /usr/lib/futureblitz, with fb in /usr/bin. Building a program needs nothing else,
but on macOS Apple's Command Line Tools (`xcode-select --install`).

```sh
fb new hello
cd hello
fb run
```

The VS Code extension, FutureBlitz, is on the Visual Studio Marketplace and Open VSX:
`code --install-extension futureblitz.futureblitz`.

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
