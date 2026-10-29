# FutureBlitz

## Installing

Releases are on [GitHub Releases](https://github.com/hlpdev/FutureBlitz/releases), for Windows
(x64), Linux (x64 and arm64; Ubuntu 22.04 or newer, and the programs fb builds run on glibc 2.28,
as RHEL 8 and Ubuntu 20.04 have), and macOS 15 or newer (arm64 and x64). They're unsigned for
now, so a browser warns about the archives and installers it downloads; the install scripts
don't.

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
