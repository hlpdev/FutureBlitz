# FutureBlitz

From-scratch Blitz3D successor: language (.fb), compiler, tooling, then engine.
Source of truth: docs/spec/

## How we work
- I type all C++ and other real code myself. Write it in chat; I'll rewrite it.
  Keep snippets focused on the current step.
- You write the tests (tests/) directly, in my code style.
- You may do project setup yourself: CMake, tool configs, dependencies, stub files, and docs
  when I ask.
- When I ask for a review, read my files and point out problems. Fix minor issues yourself
  (formatting, naming, comments, small portability fixes); leave real changes to me.
- Keep explanations short.

## C++ conventions
- C++23, CMake, deps via FetchContent
- snake_case; types end in _t; private/protected members use m_; enum constants UPPER_CASE; template params CamelCase
- .clang-format and .clang-tidy are authoritative
- Style: never align declarations or assignments; spaces inside braced lists `{ .line = 0 }`
- Headers in <project>/include/fb/, namespace fb
- Tests: Catch2 via FetchContent; compiler unit tests in tests/compiler/ (fb_compiler_tests), run with ctest

## Projects
compiler (lib), runtime (lib), cli (exe), lsp (exe)

## Current step
M2 (data: structs, classes, refcounting, arrays, errors, `Defer`): planning the design and the
build order. Nothing decided yet.

Done: M1, finished 2026-10-16, in six steps: source and diagnostics; tokens and lexer; AST and
parser with error recovery (tests compare `fb::dump_ast`); semantic analysis (types, const_value,
sema); codegen (runtime, MIR, lowering, LLVM C API codegen, linking with lld; end-to-end tests in
tests/programs); and the `fb` CLI (`check`, `build`, `run`, `--show` for every stage, color).
`#If` directives are deferred past M1; the lexer reports them as not supported yet.
