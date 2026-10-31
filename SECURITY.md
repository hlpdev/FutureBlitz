# Security

## Reporting a vulnerability

Please don't report a vulnerability in a public issue. Report it privately, on GitHub, at
[Report a vulnerability](https://github.com/hlpdev/FutureBlitz/security/advisories/new) (the
repository's Security tab, then "Report a vulnerability"). Only you and the maintainers see it.

Say what's affected and in which version (`fb --version`), how to reproduce it, ideally with a
small program, and what someone could do with it. You'll hear back within a week, and as the fix
goes on.

## What's covered

- fb: the compiler, the runtime, and the CLI, `fb build` and `fb run` among them.
- std, as a program uses it: TLS, the HTTP client and server, WebSockets, crypto, compression,
  and the parsers of JSON, XML, YAML, TOML, and the others, on input a program doesn't trust.
- The package manager: `fb add`, `fb update`, `fb publish`, `fb login`, archives, and fb.lock's
  checksums.
- The package registry, pkg.futureblitz.dev, and its API, whose code is private; report them here
  too.
- futureblitz.dev, the install scripts, the release files and their signatures, and the apt and
  dnf repositories.
- fb-lsp and the VS Code extension.

A package that's malicious, or breaks the registry's
[package policy](https://pkg.futureblitz.dev/policies/packages), is
[reported on the package site](https://pkg.futureblitz.dev/policies/report) instead. A
vulnerability in fbengine goes to [fbengine's own](https://github.com/hlpdev/fbengine/security).

## Supported versions

The latest release gets security fixes, as a new patch release. Older releases don't, so update
to the latest first if you can.

## What happens

The maintainers confirm the report, fix it in private, and release the fix, then publish a GitHub
security advisory with a CVE, crediting you unless you'd rather not be. Please keep it private
until the advisory is out.
