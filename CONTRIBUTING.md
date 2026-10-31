# Contributing

Thanks for helping with FutureBlitz. Bug reports, ideas, docs fixes, and pull requests are all
welcome. Everyone in the project's spaces keeps to the [code of conduct](CODE_OF_CONDUCT.md).

## Issues

[Open an issue](https://github.com/hlpdev/FutureBlitz/issues/new/choose) for a bug in fb, std,
fb-lsp, or the VS Code extension, a mistake in the docs or on futureblitz.dev, a problem with the
package registry, or an idea for the language or std. Look for an existing one first, and for a
bug, give the smallest program that shows it and what `fb --version` prints.

A vulnerability isn't an issue: report it privately, as [SECURITY.md](SECURITY.md) says. The
engine is [fbengine](https://github.com/hlpdev/fbengine), with its own issues.

A new issue gets `needs-triage`. A maintainer gives it its area (`area/compiler`, `area/std`,
`area/lsp`, and the others), then marks it `confirmed`, asks for a program that shows it with
`needs-repro`, or closes it as a `duplicate` or `wontfix`. `good first issue` marks a small one
that's a good start, and `help wanted` one that's open to anyone who'd like to take it.

## Pull requests

Any pull request is welcome. For a change to the language, or a new std module, opening an issue
first lets it be talked through before you write it; the language's design is in
[docs/spec/](docs/spec/README.md), and why it is as it is in [docs/decisions.md](docs/decisions.md).

A pull request:

- Builds and passes its tests on Windows, Linux, and macOS, as CI checks.
- Comes with a test for what it fixes or adds.
- Follows the style: `.clang-format` and `.clang-tidy` for C++, as
  [docs/conventions.md](docs/conventions.md) sums up, and `fb fmt` for .fb.
- Updates every doc the change touches: the spec for the language, [docs/stdlib.md](docs/stdlib.md)
  and its `///` docs for std (`fb doc --check` finds a public one without), and the README.
- Keeps to one change, so it's easy to review.

Commit messages are short and lowercase, saying what changed, with the area first when it helps:
`lsp: hover shows a property's doc`.

## Building and testing

[Building from source](README.md#building-from-source) has what fb needs. With a build folder:

```sh
ctest --test-dir build --output-on-failure
```

runs every test: the compiler's, the runtime's, the CLI's, fb-lsp's, and the test programs in
tests/programs/, each of which prints what its .out file says. `tools/verify.py` runs what CI does,
and clang-format and clang-tidy on what changed:

```sh
python tools/verify.py --build build --quick
```

`--quick` runs the unit tests and only the test programs that changed; without it, it runs them
all, in release mode too, and the concurrent ones many times over.

The VS Code extension is in editors/vscode, with its own tests:

```sh
npm ci
npm run compile
npm run test:unit
```

## License

FutureBlitz is under the [Apache License 2.0](LICENSE), and what you contribute is under it too,
as its section 5 says.
